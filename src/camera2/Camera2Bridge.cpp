// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// Camera2Bridge implementation — Milestone 3b: live preview on screen.
//
// startCamera() opens the camera and starts a PRIVATE-format preview through
// CameraSession (M3a).  The preview AImageReader is allocated with GPU-sampled
// usage so each frame's AHardwareBuffer can be imported straight into GL: the
// Renderer (on Qt's scene-graph thread) does
//   AImage -> AImage_getHardwareBuffer -> eglGetNativeClientBufferANDROID
//          -> eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)
//          -> glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)
//          -> textured quad into the QQuickFramebufferObject.
// Zero copy, GPU does the YUV->RGB.  Extensions confirmed present on the FLX1s
// (Mali-G68, hybris EGL): EGL_ANDROID_image_native_buffer,
// EGL_ANDROID_get_native_client_buffer, GL_OES_EGL_image_external.
//
// M4-M7 entry points (recording, photo, manual controls) are present but stubbed
// — they are filled in by their milestones.

#include "Camera2Bridge.h"
#include "CameraSession.h"
#include "VideoEncoder.h"
#include "Camera2NDK.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QOpenGLFramebufferObject>
#include <QStandardPaths>
#include <QTimer>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusVariant>

#include <cmath>
#include <cstdio>

#include "PreviewRenderer.h"

namespace furicam2 {

namespace {

// Qt scene-graph renderer: a thin QQuickFramebufferObject::Renderer that hands
// the actual GL work to the shared (Qt-free) PreviewRenderer.
class Camera2PreviewRenderer : public QQuickFramebufferObject::Renderer {
public:
    ~Camera2PreviewRenderer() override { renderer_.cleanup(); }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override
    {
        viewSize_ = size;
        QOpenGLFramebufferObjectFormat fmt;
        return new QOpenGLFramebufferObject(size, fmt);
    }

    void synchronize(QQuickFramebufferObject* item) override
    {
        auto* bridge = static_cast<Camera2Bridge*>(item);
        reader_   = bridge->previewReader();
        rotation_ = bridge->displayRotation();
    }

    void render() override
    {
        renderer_.render(reader_, viewSize_.width(), viewSize_.height(), rotation_);
    }

private:
    PreviewRenderer renderer_;
    QSize           viewSize_{1, 1};
    AImageReader*   reader_   = nullptr;
    int             rotation_ = 90;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

Camera2Bridge::Camera2Bridge(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    // QQuickFramebufferObject renders bottom-up by default; mirror so our texture
    // coordinates match screen orientation.
    setMirrorVertically(true);
}

Camera2Bridge::~Camera2Bridge()
{
    stopCameraSession();
}

QQuickFramebufferObject::Renderer* Camera2Bridge::createRenderer() const
{
    return new Camera2PreviewRenderer();
}

void Camera2Bridge::startCamera()
{
    if (!session_)
        session_ = std::make_unique<CameraSession>([](const std::string& s) {
            std::fprintf(stderr, "[camera] %s\n", s.c_str());
        });

    if (CameraSession::isHostStub()) {
        emit cameraError(QStringLiteral("Camera2 unavailable: host stub build"));
        return;
    }
    if (!session_->enumerate()) {
        emit cameraError(QString::fromStdString(session_->lastError()));
        return;
    }

    const auto& cams = session_->cameras();
    if (cams.empty()) {
        emit cameraError(QStringLiteral("no cameras"));
        return;
    }

    bool haveFront = false;
    bool chosenSet = false;
    std::string chosen = cams.front().id;
    int chosenOrientation = cams.front().sensorOrientation;
    const int want = lensFacingPref_.load();
    for (const auto& c : cams) {
        if (c.facing == ACAMERA_LENS_FACING_FRONT)
            haveFront = true;
        // First camera with the wanted facing wins (camera 0 = main back, not
        // the secondary macro camera that also reports back-facing).
        if (c.facing == want && !chosenSet) {
            chosen = c.id;
            chosenOrientation = c.sensorOrientation;
            chosenSet = true;
        }
    }
    sensorOrientation_.store(chosenOrientation);

    if (!session_->open(chosen)) {
        emit cameraError(QString::fromStdString(session_->lastError()));
        return;
    }

    // Render-pull mode: schedule a repaint per frame; the renderer acquires.
    session_->setFrameCallback([this] {
        frameCount_.fetch_add(1, std::memory_order_relaxed);
        QMetaObject::invokeMethod(this, [this] {
            update();
            emit frameCountChanged();
        }, Qt::QueuedConnection);
    });

    // Photo completion (fires on a camera thread) -> marshal to the GUI thread.
    session_->setPhotoCallback([this](const std::string& path, bool ok) {
        const QString p = QString::fromStdString(path);
        QMetaObject::invokeMethod(this, [this, p, ok] {
            if (ok) {
                setLastPhotoPath(p);
                emit photoSaved(p);
            } else {
                emit cameraError(QStringLiteral("photo capture failed"));
            }
        }, Qt::QueuedConnection);
    });

    if (!session_->startPreview(1280, 720, AIMAGE_FORMAT_PRIVATE,
                                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 30)) {
        emit cameraError(QString::fromStdString(session_->lastError()));
        return;
    }

    previewReader_ = session_->previewReader();
    if (hasFrontCamera_.exchange(haveFront) != haveFront)
        emit hasFrontCameraChanged();
    previewAspectRatio_.store(720.0f / 1280.0f);
    emit previewAspectRatioChanged();
    updateDisplayRotation();
    setupOrientationMonitor();
    // Re-apply a pending video-mode request now that preview is streaming (also
    // re-enters video mode after a camera switch, which reopens the session).
    applyVideoMode();
    ready_.store(true);
    emit readyChanged();
    update();
}

void Camera2Bridge::stopCamera()
{
    stopCameraSession();
    if (ready_.exchange(false))
        emit readyChanged();
}

void Camera2Bridge::stopCameraSession()
{
    if (session_) {
        session_->setFrameCallback({});
        session_->close();
    }
    previewReader_ = nullptr;
}

void Camera2Bridge::switchCamera()
{
    lensFacingPref_.store(lensFacingPref_.load() == ACAMERA_LENS_FACING_BACK
                              ? ACAMERA_LENS_FACING_FRONT
                              : ACAMERA_LENS_FACING_BACK);
    stopCameraSession();
    startCamera();
}

void Camera2Bridge::setDeviceRotation(int degrees)
{
    deviceRotation_.store(((degrees % 360) + 360) % 360);
    updateDisplayRotation();
    update();
}

void Camera2Bridge::setupOrientationMonitor()
{
    // Subscribe to iio-sensor-proxy (the auto-rotate service) and feed the device
    // rotation into setDeviceRotation(), so the preview counter-rotates and stays
    // world-upright.  A host app (FuriCam) may also drive setDeviceRotation()
    // itself — last writer wins, and both read the same physical orientation.
    if (orientationMonitorStarted_)
        return;
    orientationMonitorStarted_ = true;

    QDBusInterface sensor(QStringLiteral("net.hadess.SensorProxy"),
                          QStringLiteral("/net/hadess/SensorProxy"),
                          QStringLiteral("net.hadess.SensorProxy"),
                          QDBusConnection::systemBus());
    if (!sensor.isValid())
        return;
    sensor.call(QStringLiteral("ClaimAccelerometer"));

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        QDBusInterface props(QStringLiteral("net.hadess.SensorProxy"),
                             QStringLiteral("/net/hadess/SensorProxy"),
                             QStringLiteral("org.freedesktop.DBus.Properties"),
                             QDBusConnection::systemBus());
        QDBusReply<QDBusVariant> r = props.call(QStringLiteral("Get"),
                                                QStringLiteral("net.hadess.SensorProxy"),
                                                QStringLiteral("AccelerometerOrientation"));
        if (!r.isValid())
            return;
        const QString o = r.value().variant().toString();
        const int dev = (o == QLatin1String("left-up"))   ? 90
                      : (o == QLatin1String("bottom-up"))  ? 180
                      : (o == QLatin1String("right-up"))   ? 270
                      :                                       0;
        setDeviceRotation(dev);
    });
    timer->start(400);
}

void Camera2Bridge::updateDisplayRotation()
{
    // Back camera: rotate the sensor image opposite the device rotation.  The
    // +180 corrects the texcoord-rotation convention in the renderer (verified
    // on-device: back camera, portrait).  Front-camera mirroring and the full
    // per-orientation matrix are finished in M8 against the app's rotation plumb.
    const int sensor = sensorOrientation_.load();
    const int device = deviceRotation_.load();
    displayRotation_.store(((sensor - device + 180) % 360 + 360) % 360);
}

void Camera2Bridge::setLastPhotoPath(const QString& path)
{
    {
        QMutexLocker lk(&lastPhotoMutex_);
        lastPhotoPath_ = path;
    }
    emit lastPhotoPathChanged();
}

QString Camera2Bridge::defaultVideoPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(dir).filePath(
        QStringLiteral("VID_%1.mp4").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")));
}

QString Camera2Bridge::defaultPhotoPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(dir).filePath(
        QStringLiteral("IMG_%1.jpg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")));
}

void Camera2Bridge::initCamera()
{
    startCamera();
}

// ── M4–M7 entry points: stubbed until their milestones ───────────────────────

void Camera2Bridge::startRecording(const QString& outputPath)
{
    if (!session_ || !session_->isOpen()) {
        emit cameraError(QStringLiteral("startRecording: camera not open"));
        return;
    }
    if (recording_.load())
        return;
    recordingPath_ = outputPath.isEmpty() ? defaultVideoPath() : outputPath;
    QDir().mkpath(QFileInfo(recordingPath_).absolutePath());
    // In video mode the preview keeps streaming during record (same reader), so
    // leave previewReader_ valid.  The legacy path records to a dedicated session
    // that displaces the preview, so its reader becomes stale.
    if (!session_->isVideoMode())
        previewReader_ = nullptr;
    if (!session_->startRecording(recordingPath_.toStdString())) {
        emit cameraError(QString::fromStdString(session_->lastError()));
        return;
    }
    recording_.store(true);
    emit recordingChanged();
}

void Camera2Bridge::stopRecording()
{
    if (!session_ || !recording_.load())
        return;
    const bool wasVideoMode = session_->isVideoMode();
    session_->stopRecording();
    recording_.store(false);
    emit recordingChanged();
    emit recordingSaved(recordingPath_);
    // In video mode the preview never stopped; only the legacy path needs the
    // displaced preview restarted.
    if (!wasVideoMode)
        startCamera();
}

void Camera2Bridge::enterVideoMode()
{
    if (session_)
        session_->enterVideoMode();
}

void Camera2Bridge::exitVideoMode()
{
    if (session_)
        session_->exitVideoMode();
}

void Camera2Bridge::setVideoMode(bool on)
{
    if (on == videoModeDesired_)
        return;
    videoModeDesired_ = on;
    applyVideoMode();
    emit videoModeChanged();
}

void Camera2Bridge::applyVideoMode()
{
    // Reconcile the GUI's desired mode with the session.  No-op until preview is
    // streaming (re-applied from startCamera) and never reconfigures mid-record.
    if (!session_ || !session_->isStreaming() || recording_.load())
        return;
    if (videoModeDesired_ && !session_->isVideoMode())
        session_->enterVideoMode();
    else if (!videoModeDesired_ && session_->isVideoMode())
        session_->exitVideoMode();
}

void Camera2Bridge::prepareRecording()
{
    if (!session_)
        return;
    if (!session_->prepareRecording())
        qWarning("Camera2Bridge: mic pre-warm failed: %s", session_->lastError().c_str());
}

void Camera2Bridge::releaseRecording()
{
    if (session_)
        session_->releaseRecording();
}

bool Camera2Bridge::audioReady() const
{
    return session_ && session_->isAudioReady();
}

void Camera2Bridge::capturePhoto(const QString& outputPath, const QString& /*settingsJson*/)
{
    if (!session_ || !session_->isStreaming()) {
        emit cameraError(QStringLiteral("capturePhoto: camera not streaming"));
        return;
    }
    const QString path = outputPath.isEmpty() ? defaultPhotoPath() : outputPath;
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!session_->capturePhoto(path.toStdString()))
        emit cameraError(QString::fromStdString(session_->lastError()));
}

void Camera2Bridge::setAutoExposure()
{
    if (session_)
        session_->setAutoExposure();
}

void Camera2Bridge::setManualExposure(int iso, int exposureMs)
{
    if (session_)
        session_->setManualExposure(iso, (int64_t)exposureMs * 1000000LL);
}

void Camera2Bridge::setExposureCompensation(float ev)
{
    // ev in [0,1] (0=most under, 0.5=neutral, 1=most over) → AE comp steps.
    // This device's compensation range is -4..+4 at 0.5 EV/step.
    int steps = (int)std::lround((ev - 0.5f) * 8.0f);
    if (steps < -4) steps = -4;
    else if (steps > 4) steps = 4;
    if (session_)
        session_->setExposureCompensation(steps);
}

void Camera2Bridge::setAELock(bool lock)  { if (session_) session_->setAeLock(lock); }
void Camera2Bridge::setAWBLock(bool lock) { if (session_) session_->setAwbLock(lock); }

void Camera2Bridge::setFocusLock(bool lock)
{
    if (session_)
        session_->setAfMode(lock ? ACAMERA_CONTROL_AF_MODE_OFF
                                 : ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
}

void Camera2Bridge::setAutoFocus()
{
    if (session_)
        session_->setAfMode(ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE);
}

void Camera2Bridge::setFocusPoint(float x, float y)
{
    if (session_)
        session_->setFocusPoint(x, y);
}

void Camera2Bridge::setTorch(bool on) { if (session_) session_->setTorch(on); }

void Camera2Bridge::setWhiteBalanceMode(int appMode)
{
    if (!session_)
        return;
    // Existing app modes: 0=Auto, 1=Daylight, 2=Cloudy, 3=Fluorescent, 4=Incandescent.
    int c2;
    switch (appMode) {
        case 1:  c2 = ACAMERA_CONTROL_AWB_MODE_DAYLIGHT;        break;
        case 2:  c2 = ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT; break;
        case 3:  c2 = ACAMERA_CONTROL_AWB_MODE_FLUORESCENT;     break;
        case 4:  c2 = ACAMERA_CONTROL_AWB_MODE_INCANDESCENT;    break;
        default: c2 = ACAMERA_CONTROL_AWB_MODE_AUTO;            break;
    }
    session_->setAwbMode(c2);
}

void Camera2Bridge::setZoom(float ratio)
{
    if (session_)
        session_->setZoomRatio(ratio);
}

float Camera2Bridge::maxZoom() const
{
    return session_ ? session_->maxZoomRatio() : 4.0f;
}

} // namespace furicam2
