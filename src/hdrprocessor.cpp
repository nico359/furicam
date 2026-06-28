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
//   3. Fuse aligned frames directly with cv::MergeMertens — Camera2 captures
//      real EV brackets (±2 stops via shutter speed), so frames have genuine
//      exposure differences.  Mertens well-exposedness weighting picks the
//      best pixels from each exposure.
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
            frames.clear();  // free ~180MB early — aligned holds ref-counted copies
            for (size_t i = 0; i < aligned.size(); ++i) {
                cv::Scalar aMean, aStd;
                cv::meanStdDev(aligned[i], aMean, aStd);
                hdrLog(QString("  aligned[%1] mean=(%2,%3,%4)").arg(i).arg(aMean[0]).arg(aMean[1]).arg(aMean[2]));
            }
        } catch (const cv::Exception &e) {
            hdrLog(QString("  AlignMTB exception: %1, using unaligned").arg(e.what()));
            qDebug() << "HdrProcessor: alignment failed:" << e.what();
            aligned = frames;
            frames.clear();  // drop extra ref, aligned holds the data now
        }
    } else {
        aligned = frames;
    }

    // --- 3. Downscale to 50% for Mertens fusion ---
    // MergeMertens converts every input to float [0,1] and holds all
    // three simultaneously — at 20MP that's ~720MB of float temporaries.
    // Mertens weights (contrast / saturation / well-exposedness) are
    // inherently low-frequency, so computing them at half resolution and
    // upscaling the fused result back loses no perceptible quality while
    // cutting peak memory from ~960MB to ~300MB.
    int origW = aligned[0].cols, origH = aligned[0].rows;
    int halfW = (origW + 1) / 2, halfH = (origH + 1) / 2;
    hdrLog(QString("  downscaling %1x%2 → %3x%4 for Mertens fusion")
           .arg(origW).arg(origH).arg(halfW).arg(halfH));

    std::vector<cv::Mat> small;
    small.reserve(aligned.size());
    for (auto &f : aligned) {
        cv::Mat s;
        cv::resize(f, s, cv::Size(halfW, halfH), 0, 0, cv::INTER_AREA);
        small.push_back(s);
    }
    aligned.clear();  // free ~180MB before MergeMertens

    // --- 4. Fuse downscaled frames with MergeMertens ---
    hdrLog(QString("  MergeMertens: %1 frames, contrast=1.2").arg(small.size()));

    cv::Mat fused32f;
    try {
        cv::Ptr<cv::MergeMertens> merger = cv::createMergeMertens(1.2f, 1.0f, 0.8f);
        merger->process(small, fused32f);
        cv::Scalar fmean, fstd;
        cv::meanStdDev(fused32f, fmean, fstd);
        hdrLog(QString("  fused32f Mertens: mean=(%1,%2,%3)")
               .arg(fmean[0]).arg(fmean[1]).arg(fmean[2]));
    } catch (const cv::Exception &e) {
        hdrLog(QString("  MergeMertens EXCEPTION: %1").arg(e.what()));
        qDebug() << "HdrProcessor: merge failed:" << e.what();
        return QString();
    }
    small.clear();  // free half-res inputs

    // --- 5. Upscale fused float result to original resolution ---
    cv::Mat fusedFull;
    cv::resize(fused32f, fusedFull, cv::Size(origW, origH), 0, 0, cv::INTER_LANCZOS4);
    fused32f.release();  // free half-res float before 8-bit conversion

    // --- 6. Convert float [0,1] → uint8 at full resolution ---
    cv::Mat fused8u;
    fusedFull.convertTo(fused8u, CV_8U, 255.0);
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
        QString("IMG_%1_HDR.jpg").arg(
            QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")));

    if (!result.save(outPath, "JPEG", 95)) {
        hdrLog(QString("  ERROR: save failed to %1").arg(outPath));
        qDebug() << "HdrProcessor: could not save result to" << outPath;
        return QString();
    }
    hdrLog(QString("  saved: %1 (%2 bytes)").arg(outPath).arg(QFileInfo(outPath).size()));

    // --- 8. Copy EXIF from the EV 0 (first) frame ---
    // EV 0 is always captured first, so it's framePaths[0].
    // basePath still uses framePaths; the middle frame's JPEG embedded EXIF
    // is the most authoritative metadata for the fused result.
    // NOTE: the real pixel values come from aligned mid-frame (not saved),
    // but EXIF metadata (timestamp, GPS, orientation) is frame-agnostic.
    QString basePath = framePaths.value(0);  // EV 0 is always first
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
