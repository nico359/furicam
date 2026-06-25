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
//   3. Feed aligned frames directly to cv::MergeMertens — the burst frames
//      have real EV differences (underexposed / neutral / overexposed), so
//      no synthetic brackets are needed.  Mertens well-exposedness weighting
//      automatically picks the best pixels from each exposure.
//   4. Convert float result → 8-bit, wrap in QImage, save as JPEG.
//   5. Copy EXIF from the base (middle) frame via exiv2.
//   6. Delete the raw burst frames.

#include "hdrprocessor.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QDateTime>
#include <QTextStream>

#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

#include <exiv2/exiv2.hpp>

// Direct-to-file debug log — qDebug doesn't reliably reach stderr on this
// platform (Halium/Qt output routing), so write a simple trace for HDR.
static void hdrLog(const QString &msg) {
    QFile f("/tmp/hdr_debug.log");
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream s(&f);
        s << QDateTime::currentDateTime().toString("hh:mm:ss.zzz") << " " << msg << "\n";
    }
}

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
    hdrLog(QString("processHdrBurst: %1 frames, outputDir=%2").arg(framePaths.size()).arg(outputDir));
    for (int i = 0; i < framePaths.size(); ++i)
        hdrLog(QString("  frame[%1] = %2").arg(i).arg(framePaths[i]));

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

        QFileInfo fi(path);
        hdrLog(QString("  loading %1, exists=%2, size=%3").arg(path).arg(fi.exists()).arg(fi.size()));

        QImage img(path);
        if (img.isNull()) {
            hdrLog(QString("  ERROR: QImage null for %1").arg(path));
            qDebug() << "HdrProcessor: could not load" << path;
            return QString();
        }
        cv::Mat mat = qImageToMat(img);
        cv::Scalar mean, stddev;
        cv::meanStdDev(mat, mean, stddev);
        hdrLog(QString("  loaded %1x%2 mean=(%3,%4,%5)").arg(img.width()).arg(img.height())
               .arg(mean[0]).arg(mean[1]).arg(mean[2]));
        frames.push_back(mat);
    }

    // --- 2. Align burst frames (camera shake between shots) ---
    std::vector<cv::Mat> aligned;
    if (frames.size() > 1) {
        try {
            hdrLog("  AlignMTB: aligning...");
            cv::Ptr<cv::AlignMTB> aligner = cv::createAlignMTB();
            aligner->process(frames, aligned);
            for (size_t i = 0; i < aligned.size(); ++i) {
                cv::Scalar aMean, aStd;
                cv::meanStdDev(aligned[i], aMean, aStd);
                hdrLog(QString("  aligned[%1] mean=(%2,%3,%4)").arg(i).arg(aMean[0]).arg(aMean[1]).arg(aMean[2]));
            }
        } catch (const cv::Exception &e) {
            hdrLog(QString("  AlignMTB exception: %1, using unaligned").arg(e.what()));
            qDebug() << "HdrProcessor: alignment failed:" << e.what();
            aligned = frames;
        }
    } else {
        aligned = frames;
    }

    // --- 3. Use aligned frames directly as exposure brackets ---
    // When real EV bracketing works, the frames have genuine exposure
    // differences and Mertens fusion produces an HDR result.  When the
    // HAL ignores per-frame EV comp (all frames near-identical), fall
    // back to averaging + synthetic ±1 stop brackets.
    // ponytail: synthetic brackets are a known-good code path; real EV
    // bracketing can be debugged separately.
    std::vector<cv::Mat> brackets;

    // Check if frames have any real exposure difference.
    double mean0 = cv::mean(aligned[0])[0];
    double mean2 = cv::mean(aligned[2])[0];
    double ratio = mean0 > 1.0 ? mean2 / mean0 : 1.0;
    hdrLog(QString("  frame mean ratio (dark/bright): %1").arg(ratio));

    if (ratio > 0.8 && ratio < 1.25) {
        // Frames too similar — EV bracketing not working.  Average + synthetic.
        hdrLog("  frames near-identical, using average + synthetic ±1 stop brackets");
        // ponytail: keep float in [0,255] so convertTo→CV_8UC3 works without scale.
        cv::Mat avgFloat = cv::Mat::zeros(aligned[0].rows, aligned[0].cols, CV_32FC3);
        for (auto &f : aligned) {
            cv::Mat f32;
            f.convertTo(f32, CV_32FC3);  // [0,255] — NO division by 255
            avgFloat += f32;
        }
        avgFloat /= (float)aligned.size();

        cv::Mat avg8u;
        avgFloat.convertTo(avg8u, CV_8UC3);

        // −1 stop for highlights
        cv::Mat darkFloat = avgFloat * 0.5f;
        cv::Mat dark;
        darkFloat.convertTo(dark, CV_8UC3);

        // +1 stop for shadows
        cv::Mat brightFloat = avgFloat * 2.0f;
        cv::threshold(brightFloat, brightFloat, 255.0, 255.0, cv::THRESH_TRUNC);
        cv::Mat bright;
        brightFloat.convertTo(bright, CV_8UC3);

        brackets.push_back(dark);
        brackets.push_back(avg8u);
        brackets.push_back(bright);
    } else {
        hdrLog("  using real EV brackets directly");
        // ponytail: pass 8-bit to MergeMertens (float input is buggy on this
        // OpenCV build — produces near-zero output).
        for (auto &f : aligned)
            brackets.push_back(f);
    }
    frames.clear();
    aligned.clear();

    // --- 4. Fuse brackets with MergeMertens ---
    cv::Mat fused32f;
    try {
        hdrLog(QString("  MergeMertens: %1 brackets, contrast=1.2").arg(brackets.size()));
        cv::Ptr<cv::MergeMertens> merger = cv::createMergeMertens(1.2f, 1.0f, 0.8f);
        merger->process(brackets, fused32f);
        cv::Scalar fmean, fstd;
        cv::meanStdDev(fused32f, fmean, fstd);
        hdrLog(QString("  fused32f Mertens: mean=(%1,%2,%3)")
               .arg(fmean[0]).arg(fmean[1]).arg(fmean[2]));
    } catch (const cv::Exception &e) {
        hdrLog(QString("  MergeMertens EXCEPTION: %1").arg(e.what()));
        qDebug() << "HdrProcessor: merge failed:" << e.what();
        return QString();
    }
    brackets.clear();

    // --- 5. Convert float [0,1] → uint8 ---
    cv::Mat fused8u;
    fused32f.convertTo(fused8u, CV_8U, 255.0);
    {
        cv::Scalar m8u, s8u;
        cv::meanStdDev(fused8u, m8u, s8u);
        hdrLog(QString("  fused8u: mean=(%1,%2,%3)").arg(m8u[0]).arg(m8u[1]).arg(m8u[2]));
    }

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
        hdrLog(QString("  ERROR: save failed to %1").arg(outPath));
        qDebug() << "HdrProcessor: could not save result to" << outPath;
        return QString();
    }
    hdrLog(QString("  saved: %1 (%2 bytes)").arg(outPath).arg(QFileInfo(outPath).size()));

    // --- 8. Copy EXIF from the base (middle) frame ---
    // basePath still uses framePaths; the middle frame's JPEG embedded EXIF
    // is the most authoritative metadata for the fused result.
    // NOTE: the real pixel values come from aligned mid-frame (not saved),
    // but EXIF metadata (timestamp, GPS, orientation) is frame-agnostic.
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

    // --- 8. Clean temp frames ---
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
