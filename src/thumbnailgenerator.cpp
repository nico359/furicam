// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Droidian Project
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>
// Erik Inkinen <erik.inkinen@gmail.com>
// Alexander Rutz <alex@familyrutz.com>

#include "thumbnailgenerator.h"

#include <QUrl>
#include <QDir>
#include <QFile>
#include <QDebug>

ThumbnailGenerator::ThumbnailGenerator(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QImage>("QImage");
} 
void ThumbnailGenerator::setVideoSource(const QString &videoSource) {
    // The QML side passes a file:// URL; ffmpeg needs a plain filesystem path.
    QString path = videoSource;
    const QUrl u(videoSource);
    if (u.isLocalFile())
        path = u.toLocalFile();

    if (path.isEmpty() || !QFile::exists(path)) {
        qDebug() << "ThumbnailGenerator: video not found:" << path;
        return;
    }

    // Only one extraction at a time; drop any in-flight job (e.g. when the user
    // swipes quickly between videos).
    if (m_proc) {
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }

    m_outPath = QDir::tempPath() + QStringLiteral("/furicam_vidthumb.jpg");
    QFile::remove(m_outPath);

    m_proc = new QProcess(this);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        qDebug() << "ThumbnailGenerator: ffmpeg finished, exit:" << exitCode << "path:" << m_outPath;
        QImage img(m_outPath);
        if (!img.isNull()) {
            qDebug() << "ThumbnailGenerator: image loaded" << img.width() << "x" << img.height();
            emit thumbnailGenerated(img);
        } else {
            qDebug() << "ThumbnailGenerator: failed to load thumbnail from" << m_outPath;
        }
        if (m_proc) {
            m_proc->deleteLater();
            m_proc = nullptr;
        }
    });

    // Decode just the first frame, scaled down to a thumbnail.  Software decode
    // (the default) works regardless of the device's hardware codec path.
    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-loglevel") << QStringLiteral("error")
         << QStringLiteral("-i") << path
         << QStringLiteral("-frames:v") << QStringLiteral("1")
         << QStringLiteral("-vf") << QStringLiteral("scale=320:-2")
         << m_outPath;
    qDebug() << "ThumbnailGenerator: starting" << "/usr/bin/ffmpeg" << args;
    m_proc->start(QStringLiteral("/usr/bin/ffmpeg"), args);
}

QString ThumbnailGenerator::toQmlImage(const QImage &image) {
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    image.save(&buffer, "PNG");
    return QString("data:image/png;base64,") + QString(byteArray.toBase64());
}
