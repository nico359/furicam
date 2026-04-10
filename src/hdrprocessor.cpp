// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Joaquin Philco <joaquinphilco@gmail.com>
//
// HDR burst processing pipeline:
//   1. Load JPEG frames from Qt (QImage → cv::Mat) — avoids libopencv-imgcodecs
//      and its heavy libgdal/libgdcm transitive dependencies.
//   2. Align frames with cv::AlignMTB (Median Threshold Bitmap, Ward 2003).
//   3. Average aligned frames to suppress sensor noise (temporal averaging).
//   4. Create synthetic exposure brackets from the averaged base:
//        −1 stop (×0.5)  — preserves highlights
//         0 stops (×1.0) — balanced mid-tones
//        +1 stop (×2.0)  — recovers shadows (light Gaussian blur)
//   5. Fuse brackets with cv::MergeMertens (Mertens–Kautz–Van Reeth 2007).
//   6. Convert float result → 8-bit, wrap in QImage, save as JPEG.
//   7. Copy EXIF from the base (middle) frame via exiv2.
//   8. Delete the raw burst frames.

#include "hdrprocessor.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QDateTime>

#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <exiv2/exiv2.hpp>

// ---------------------------------------------------------------------------
// Helpers: QImage <-> cv::Mat (RGB, no alpha, no file I/O via OpenCV)
// ---------------------------------------------------------------------------

static cv::Mat qImageToMat(const QImage &img)
{
    QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    cv::Mat mat(rgb.height(), rgb.width(), CV_8UC3,
                const_cast<uchar *>(rgb.constBits()),
                static_cast<size_t>(rgb.bytesPerLine()));
    cv::Mat out;
    mat.copyTo(out);
    return out;
}

static QImage matToQImage(const cv::Mat &mat)
{
    Q_ASSERT(mat.type() == CV_8UC3);
    QImage img(mat.data, mat.cols, mat.rows,
               static_cast<int>(mat.step),
               QImage::Format_RGB888);
    return img.copy(); // detach from mat memory
}

// ---------------------------------------------------------------------------
// HdrProcessor
// ---------------------------------------------------------------------------

HdrProcessor::HdrProcessor(QObject *parent)
    : QObject(parent)
{
}

QString HdrProcessor::processHdrBurst(const QStringList &framePaths, const QString &outputDir)
{
    if (framePaths.isEmpty()) {
        qDebug() << "HdrProcessor: no frames provided";
        return QString();
    }

    // --- 1. Load frames ---
    std::vector<cv::Mat> frames;
    frames.reserve(static_cast<size_t>(framePaths.size()));

    for (const QString &rawPath : framePaths) {
        QString path = rawPath;
        if (path.startsWith("file://"))
            path = path.mid(7);

        QImage img(path);
        if (img.isNull()) {
            qDebug() << "HdrProcessor: could not load" << path;
            return QString();
        }
        frames.push_back(qImageToMat(img));
    }

    // --- 2. Align burst frames (camera shake between shots) ---
    std::vector<cv::Mat> aligned;
    if (frames.size() > 1) {
        try {
            cv::Ptr<cv::AlignMTB> aligner = cv::createAlignMTB();
            aligner->process(frames, aligned);
        } catch (const cv::Exception &e) {
            qDebug() << "HdrProcessor: alignment failed:" << e.what();
            aligned = frames;
        }
    } else {
        aligned = frames;
    }

    // --- 3. Average aligned frames for temporal noise reduction ---
    cv::Mat avgFloat = cv::Mat::zeros(aligned[0].rows, aligned[0].cols, CV_32FC3);
    for (const cv::Mat &f : aligned) {
        cv::Mat f32;
        f.convertTo(f32, CV_32FC3);
        avgFloat += f32;
    }
    avgFloat /= static_cast<float>(aligned.size());

    // Free raw frames early to reduce peak memory on the phone.
    frames.clear();
    aligned.clear();

    cv::Mat avg8u;
    avgFloat.convertTo(avg8u, CV_8UC3);

    // --- 4. Create synthetic exposure brackets from the averaged base ---
    // ±1 stop gives a real dynamic range boost.  Temporal averaging across
    // 3 frames already reduces noise by ~1.7×, so NLM denoising is not
    // needed (and would OOM on a phone at full resolution).
    cv::Mat dark, bright, darkFloat, brightFloat;

    // Dark bracket (−1 stop, ×0.5): preserves highlights
    darkFloat = avgFloat * 0.5f;
    darkFloat.convertTo(dark, CV_8UC3);

    // Bright bracket (+1 stop, ×2.0): lifts shadows
    brightFloat = avgFloat * 2.0f;
    // Free avgFloat — no longer needed, mid bracket is avg8u.
    avgFloat.release();
    cv::threshold(brightFloat, brightFloat, 255.0, 255.0, cv::THRESH_TRUNC);
    cv::Mat brightU8;
    brightFloat.convertTo(brightU8, CV_8UC3);
    brightFloat.release();

    // Light Gaussian blur on the bright bracket to soften amplified noise
    // without the extreme memory cost of NLM denoising.
    cv::GaussianBlur(brightU8, brightU8, cv::Size(3, 3), 0);

    std::vector<cv::Mat> brackets = { dark, avg8u, brightU8 };

    // --- 5. Fuse brackets with MergeMertens ---
    cv::Mat fused32f;
    try {
        cv::Ptr<cv::MergeMertens> merger = cv::createMergeMertens(1.0f, 1.0f, 0.8f);
        merger->process(brackets, fused32f);
    } catch (const cv::Exception &e) {
        qDebug() << "HdrProcessor: merge failed:" << e.what();
        return QString();
    }
    // Free brackets before final conversion.
    brackets.clear();
    dark.release();
    avg8u.release();
    brightU8.release();

    // --- 6. Convert float [0,1] → uint8 and wrap in QImage ---
    cv::Mat fused8u;
    fused32f.convertTo(fused8u, CV_8U, 255.0);

    QImage result = matToQImage(fused8u);
    if (result.isNull()) {
        qDebug() << "HdrProcessor: result image is null";
        return QString();
    }

    // --- 7. Determine output path in the caller-specified output directory ---
    QDir outDir(outputDir);
    if (!outDir.exists())
        outDir.mkpath(".");

    QString outPath = outDir.filePath(
        QString("image%1_hdr.jpg").arg(
            QDateTime::currentDateTime().toString("yyyyMMdd_HHmmsszzz")));

    if (!result.save(outPath, "JPEG", 95)) {
        qDebug() << "HdrProcessor: could not save result to" << outPath;
        return QString();
    }

    // --- 8. Copy EXIF from the base (middle) frame ---
    QString basePath = framePaths.value(framePaths.size() / 2);
    if (basePath.startsWith("file://"))
        basePath = basePath.mid(7);
    try {
        auto src = Exiv2::ImageFactory::open(basePath.toStdString());
        src->readMetadata();
        auto dst = Exiv2::ImageFactory::open(outPath.toStdString());
        dst->readMetadata();
        dst->setExifData(src->exifData());
        dst->writeMetadata();
    } catch (const Exiv2::Error &e) {
        qDebug() << "HdrProcessor: EXIF copy warning:" << e.what();
        // non-fatal — the image is still valid
    }

    // --- 10. Delete raw burst frames ---
    for (const QString &rawPath : framePaths) {
        QString path = rawPath;
        if (path.startsWith("file://"))
            path = path.mid(7);
        if (!QFile::remove(path))
            qDebug() << "HdrProcessor: could not delete burst frame" << path;
    }

    return outPath;
}

void HdrProcessor::applyExposureCompensation(const QString &imagePath, float ev)
{
    if (std::fabs(ev) < 0.01f)
        return;

    QString path = imagePath;
    if (path.startsWith("file://"))
        path = path.mid(7);

    QImage img(path);
    if (img.isNull()) {
        qDebug() << "HdrProcessor: applyEV: could not load" << path;
        return;
    }

    cv::Mat mat = qImageToMat(img);

    // Apply gain = 2^ev: positive EV brightens, negative darkens.
    // Work in float to avoid banding from integer truncation.
    cv::Mat mat32f;
    mat.convertTo(mat32f, CV_32F);
    mat32f *= static_cast<float>(std::pow(2.0, static_cast<double>(ev)));
    cv::threshold(mat32f, mat32f, 255.0, 255.0, cv::THRESH_TRUNC);

    cv::Mat result;
    mat32f.convertTo(result, CV_8U);

    QImage resultImg = matToQImage(result);
    if (!resultImg.save(path, "JPEG", 95))
        qDebug() << "HdrProcessor: applyEV: could not save" << path;
}

void HdrProcessor::cleanBurstDir(const QString &burstDir)
{
    QDir dir(burstDir);
    if (!dir.exists())
        return;

    const QStringList files = dir.entryList(QStringList() << "burst_*.jpg", QDir::Files);
    for (const QString &f : files) {
        if (!dir.remove(f))
            qDebug() << "HdrProcessor: could not remove stale burst file" << f;
    }
}
