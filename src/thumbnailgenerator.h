// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Droidian Project
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>
// Erik Inkinen <erik.inkinen@gmail.com>
// Alexander Rutz <alex@familyrutz.com>

#ifndef THUMBNAILGENERATOR_H
#define THUMBNAILGENERATOR_H

#include <QObject>
#include <QImage>
#include <QBuffer>
#include <QProcess>

class ThumbnailGenerator : public QObject
{
    Q_OBJECT
public:
    ThumbnailGenerator(QObject *parent = nullptr);
    Q_INVOKABLE void setVideoSource(const QString &videoSource);
    Q_INVOKABLE QString toQmlImage(const QImage &image);

signals:
    void thumbnailGenerated(const QImage &image);

private:
    // Frames are extracted with ffmpeg (software decode) rather than
    // QMediaPlayer/QVideoProbe: the latter hands back native YUV frames that
    // QImage can't construct from, and on the libhybris hardware-codec path the
    // frames may not even be CPU-mappable — both produce a black thumbnail.
    QProcess *m_proc = nullptr;
    QString m_outPath;
};

#endif // THUMBNAILGENERATOR_H
