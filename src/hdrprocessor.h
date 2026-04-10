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
};

#endif // HDRPROCESSOR_H
