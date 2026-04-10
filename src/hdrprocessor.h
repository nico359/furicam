// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Joaquin Philco <joaquinphilco@gmail.com>

#ifndef HDRPROCESSOR_H
#define HDRPROCESSOR_H

#include <QObject>
#include <QString>
#include <QStringList>

class HdrProcessor : public QObject
{
    Q_OBJECT
public:
    explicit HdrProcessor(QObject *parent = nullptr);

    // Takes a list of burst frame paths, aligns and fuses them via Mertens
    // exposure fusion, writes the result next to the first frame, copies
    // EXIF from the first frame, deletes the raw burst frames and returns
    // the output path.  Returns an empty string on failure.
    Q_INVOKABLE QString processHdrBurst(const QStringList &framePaths, const QString &outputDir);

    // Applies software exposure compensation to an already-saved image.
    // ev is in stops: positive brightens (gain = 2^ev), negative darkens.
    // No-op when |ev| < 0.01.  Overwrites the file in place.
    Q_INVOKABLE void applyExposureCompensation(const QString &imagePath, float ev);

    // Remove any leftover burst files from a previous (possibly failed) HDR capture.
    Q_INVOKABLE void cleanBurstDir(const QString &burstDir);
};

#endif // HDRPROCESSOR_H
