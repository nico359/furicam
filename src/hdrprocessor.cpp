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
//   3. Fuse with cv::MergeMertens (Mertens–Kautz–Van Reeth 2007) which weights
//      pixels by contrast, saturation and well-exposedness and blends them via
//      a Laplacian pyramid — no HDR intermediate needed.
//   4. Convert float result → 8-bit, wrap in QImage, save as JPEG.
//   5. Copy EXIF from the base (middle, 0 EV) frame via exiv2.
//   6. Delete the raw burst frames.

#include "hdrprocessor.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QDateTime>

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
    if (framePaths.size() < 2) {
        qDebug() << "HdrProcessor: need at least 2 frames, got" << framePaths.size();
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

    // --- 2. Align with AlignMTB ---
    // AlignMTB is fast (no feature extraction) and works well for small
    // hand-held motion between frames captured in quick succession.
    std::vector<cv::Mat> aligned;
    try {
        cv::Ptr<cv::AlignMTB> aligner = cv::createAlignMTB();
        aligner->process(frames, aligned);
    } catch (const cv::Exception &e) {
        qDebug() << "HdrProcessor: alignment failed:" << e.what();
        return QString();
    }

    // --- 3. Fuse with MergeMertens ---
    // Default weights (contrast=1, saturation=1, exposedness=0.85) give
    // good results for a 3-frame bracket.  No explicit exposure times are
    // needed because Mertens fusion is exposure-time agnostic.
    cv::Mat fused32f;
    try {
        cv::Ptr<cv::MergeMertens> merger = cv::createMergeMertens();
        merger->process(aligned, fused32f);
    } catch (const cv::Exception &e) {
        qDebug() << "HdrProcessor: merge failed:" << e.what();
        return QString();
    }

    // --- 4. Convert float [0,1] → uint8 and wrap in QImage ---
    cv::Mat fused8u;
    fused32f.convertTo(fused8u, CV_8U, 255.0);

    QImage result = matToQImage(fused8u);
    if (result.isNull()) {
        qDebug() << "HdrProcessor: result image is null";
        return QString();
    }

    // --- 5. Determine output path in the caller-specified output directory ---
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

    // --- 6. Copy EXIF from the base (middle) frame ---
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

    // --- 7. Delete raw burst frames ---
    for (const QString &rawPath : framePaths) {
        QString path = rawPath;
        if (path.startsWith("file://"))
            path = path.mid(7);
        if (!QFile::remove(path))
            qDebug() << "HdrProcessor: could not delete burst frame" << path;
    }

    return outPath;
}
