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
#include "../hdrprocessor.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exiv2/exiv2.hpp>
#include <thread>

#include <ReadBarcode.h>
#include <BarcodeFormat.h>
#include <ImageView.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariant>
#include <QOpenGLFramebufferObject>
#include <QStandardPaths>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusVariant>

#include <cmath>
#include <cstdio>

#include "PreviewRenderer.h"

namespace furicam {

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
        rotation_ = bridge->previewRotation();
        cropX_    = bridge->cropScaleX();
        cropY_    = bridge->cropScaleY();
        mirror_   = bridge->previewMirrored();
    }

    void render() override
    {
        renderer_.render(reader_, viewSize_.width(), viewSize_.height(), rotation_,
                         cropX_, cropY_, mirror_);
    }

private:
    PreviewRenderer renderer_;
    QSize           viewSize_{1, 1};
    AImageReader*   reader_   = nullptr;
    int             rotation_ = 90;
    float           cropX_    = 1.0f;
    float           cropY_    = 1.0f;
    bool            mirror_   = false;
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
    for (const auto& c : cams)
        if (c.facing == ACAMERA_LENS_FACING_FRONT)
            haveFront = true;

    std::string chosen;
    int chosenOrientation;
    const int idx = selectedCameraIndex_.load();
    if (idx >= 0 && idx < (int)cams.size()) {
        // Explicit camera pick (e.g. the secondary back/macro camera).
        chosen = cams[idx].id;
        chosenOrientation = cams[idx].sensorOrientation;
        lensFacingPref_.store(cams[idx].facing);   // keep facing in sync for mirroring
    } else {
        // First camera with the wanted facing (camera 0 = main back, not the
        // secondary macro camera that also reports back-facing).
        chosen = cams.front().id;
        chosenOrientation = cams.front().sensorOrientation;
        const int want = lensFacingPref_.load();
        bool chosenSet = false;
        for (const auto& c : cams) {
            if (c.facing == want && !chosenSet) {
                chosen = c.id;
                chosenOrientation = c.sensorOrientation;
                chosenSet = true;
            }
        }
    }
    sensorOrientation_.store(chosenOrientation);

    // pony: pull sensor range for manual exposure sliders from the chosen camera
    for (const auto& c : cams) {
        if (c.id == chosen) {
            isoMin_.store(c.isoMin);
            isoMax_.store(c.isoMax);
            exposureMinMs_.store((int)(c.exposureMinNs / 1000000));
            exposureMaxMs_.store((int)(c.exposureMaxNs / 1000000));
            manualSensor_.store(c.manualSensor);
            minFocusDistance_.store(c.minFocusDistance);
            focusDistanceCalibration_.store(c.focusDistanceCalibration);
            break;
        }
    }
    emit manualRangeChanged();

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
            onPhotoCaptured(p, ok);
        }, Qt::QueuedConnection);
    });

    pickPreviewStreamSize();   // match the preview aspect to the chosen still aspect
    if (!session_->startPreview(previewStreamW_, previewStreamH_, AIMAGE_FORMAT_PRIVATE,
                                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 30)) {
        // Fall back to a universally-supported 16:9 preview if the picked size fails.
        previewStreamW_ = 1280; previewStreamH_ = 720;
        if (!session_->startPreview(previewStreamW_, previewStreamH_, AIMAGE_FORMAT_PRIVATE,
                                    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 30)) {
            emit cameraError(QString::fromStdString(session_->lastError()));
            return;
        }
    }

    previewReader_ = session_->previewReader();
    // Decode QR/barcodes from the analysis (YUV luma) stream — photo mode only.
    session_->setAnalysisCallback([this](const uint8_t* y, int w, int h, int stride) {
        qrDecode(y, w, h, stride);
    });
    if (hasFrontCamera_.exchange(haveFront) != haveFront)
        emit hasFrontCameraChanged();
    updateDisplayRotation();   // also recomputes previewAspectRatio_ from the stream size
    // Claim the accelerometer so orientation stays live; we read it on-demand
    // at capture time to tag photos/videos with the device tilt.  The preview
    // stays portrait-locked — updateDisplayRotation ignores device rotation.
    claimAccelerometer();
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
    // Gesture flip reverts to facing-based pick (the main camera of each side).
    selectedCameraIndex_.store(-1);
    lensFacingPref_.store(lensFacingPref_.load() == ACAMERA_LENS_FACING_BACK
                              ? ACAMERA_LENS_FACING_FRONT
                              : ACAMERA_LENS_FACING_BACK);
    stopCameraSession();
    startCamera();
}

QVariantList Camera2Bridge::availableCameras()
{
    QVariantList list;
    if (!session_)
        return list;
    if (session_->cameras().empty())
        session_->enumerate();
    int i = 0;
    for (const auto& c : session_->cameras()) {
        long best = 0;
        for (const auto& s : c.outputs)
            if (s.format == AIMAGE_FORMAT_JPEG)
                best = std::max(best, (long)s.width * s.height);
        QVariantMap m;
        m["index"]      = i++;
        m["facing"]     = (c.facing == ACAMERA_LENS_FACING_FRONT) ? 0 : 1;   // 0=front, 1=back
        m["megapixels"] = (int)((best + 500000) / 1000000);
        list.append(m);
    }
    return list;
}

void Camera2Bridge::selectCamera(int index)
{
    selectedCameraIndex_.store(index);
    if (ready_.load()) {
        stopCameraSession();
        startCamera();
    }
}

void Camera2Bridge::setDeviceRotation(int degrees)
{
    deviceRotation_.store(((degrees % 360) + 360) % 360);
    updateDisplayRotation();
    update();
}

void Camera2Bridge::claimAccelerometer()
{
    // Claim iio-sensor-proxy (the auto-rotate service) once so it keeps the
    // AccelerometerOrientation property live.  We do NOT poll/feed the preview —
    // the preview is portrait-locked — we just read it on demand at capture time
    // (queryDeviceRotation) to tag photos/videos with the device tilt.
    if (orientationMonitorStarted_)
        return;
    orientationMonitorStarted_ = true;

    QDBusInterface sensor(QStringLiteral("net.hadess.SensorProxy"),
                          QStringLiteral("/net/hadess/SensorProxy"),
                          QStringLiteral("net.hadess.SensorProxy"),
                          QDBusConnection::systemBus());
    if (sensor.isValid())
        sensor.call(QStringLiteral("ClaimAccelerometer"));
}

// Read the current device orientation from iio-sensor-proxy and map it to degrees
// clockwise from natural portrait (0/90/180/270).  Defaults to 0 (portrait) when
// the sensor is unavailable.
int Camera2Bridge::queryDeviceRotation()
{
    QDBusInterface props(QStringLiteral("net.hadess.SensorProxy"),
                         QStringLiteral("/net/hadess/SensorProxy"),
                         QStringLiteral("org.freedesktop.DBus.Properties"),
                         QDBusConnection::systemBus());
    QDBusReply<QDBusVariant> r = props.call(QStringLiteral("Get"),
                                            QStringLiteral("net.hadess.SensorProxy"),
                                            QStringLiteral("AccelerometerOrientation"));
    if (!r.isValid())
        return 0;
    const QString o = r.value().variant().toString();
    return (o == QLatin1String("left-up"))  ? 90
         : (o == QLatin1String("bottom-up")) ? 180
         : (o == QLatin1String("right-up"))  ? 270
         :                                     0;   // "normal" / unknown
}

void Camera2Bridge::updateDisplayRotation()
{
    // Preview is portrait-locked: rotation depends only on the sensor mount angle,
    // never device rotation (turning the phone must NOT counter-rotate the preview
    // because the UI is portrait-only for now).  The +180 corrects the texcoord-
    // rotation convention in the renderer (verified on-device: back camera, portrait).
    // deviceRotation_ is still tracked via setDeviceRotation() as the hook for a
    // future rotating UI; wire it back in here when the UI elements rotate.
    const int sensor = sensorOrientation_.load();
    displayRotation_.store(((sensor + 180) % 360 + 360) % 360);
    recomputePreviewAspect();
}

// The effective still size: the user-chosen one, else the sensor's largest size.
void Camera2Bridge::effectiveCaptureSize(int& cw, int& ch)
{
    cw = captureW_; ch = captureH_;
    if ((cw <= 0 || ch <= 0) && session_) {
        long best = 0;
        for (const auto& s : session_->jpegSizes()) {
            const long area = (long)s.width * s.height;
            if (area > best) { best = area; cw = s.width; ch = s.height; }
        }
    }
    if (cw <= 0 || ch <= 0) { cw = 4; ch = 3; }
}

// The preview stream is always the sensor's full-FOV (4:3) aspect; the renderer
// crops it to the chosen still aspect (recomputePreviewAspect computes the crop),
// so ANY still ratio (1:1, 3:2, 16:9 …) maps corner-for-corner with the capture.
void Camera2Bridge::pickPreviewStreamSize()
{
    previewStreamW_ = 1280;
    previewStreamH_ = 960;   // 4:3 full FOV (fallback to 720 only if this size fails)
}

// previewAspectRatio_ = the on-screen (post-rotation) w/h of the *still* aspect;
// cropScale = the centred sub-rect of the 4:3 stream that equals that aspect.
void Camera2Bridge::recomputePreviewAspect()
{
    int cw, ch;
    if (videoModeDesired_) { cw = videoW_; ch = videoH_; }   // video mode: match the clip
    else                     effectiveCaptureSize(cw, ch);    // photo mode: match the still
    if (cw <= 0 || ch <= 0) { cw = 4; ch = 3; }
    const float ca = (float)cw / (float)ch;                                   // still aspect
    const float sa = (float)previewStreamW_ / (float)previewStreamH_;         // stream aspect (~4:3)

    // Crop the 4:3 stream down to the still aspect (it's always a centred crop of
    // the full sensor): wider-than-4:3 crops height, narrower crops width.
    float sx = 1.0f, sy = 1.0f;
    if (ca >= sa) sy = sa / ca;
    else          sx = ca / sa;
    cropScaleX_.store(sx);
    cropScaleY_.store(sy);

    // Use sensor-only rotation for preview aspect so the viewport doesn't flip
    // when device rotates — only the JPEG orientation uses displayRotation_.
    const int rot = previewRotation();
    const bool portrait = (rot == 90 || rot == 270);
    const float a = portrait ? (float)ch / (float)cw : (float)cw / (float)ch;
    if (previewAspectRatio_.exchange(a) != a)
        emit previewAspectRatioChanged();
    // The next preview frame (and the cam2 resize from previewAspectRatioChanged)
    // re-render and pick up the new crop via the renderer's synchronize().
}

void Camera2Bridge::setLastPhotoPath(const QString& path)
{
    {
        QMutexLocker lk(&lastPhotoMutex_);
        lastPhotoPath_ = path;
    }
    emit lastPhotoPathChanged();
}

// Captures go under <media-dir>/<binary name> (the app convention the built-in
// gallery scans, e.g. ~/Pictures/furicam and ~/Videos/furicam).
static QString mediaSubdir()
{
    return QFileInfo(QCoreApplication::applicationFilePath()).fileName();
}

QString Camera2Bridge::defaultVideoPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
                        + "/" + mediaSubdir();
    return QDir(dir).filePath(
        QStringLiteral("VID_%1.mp4").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss")));
}

QString Camera2Bridge::defaultPhotoPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                        + "/" + mediaSubdir();
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
    // Tag this clip with how the phone is held as recording starts (preview stays
    // portrait); the session applies it to the MP4 rotation hint per clip.
    session_->setDeviceRotation(queryDeviceRotation());
    recordingPath_ = outputPath.isEmpty() ? defaultVideoPath() : outputPath;
    QDir().mkpath(QFileInfo(recordingPath_).absolutePath());
    // Make sure the combined preview+record session is up at the current size so
    // we don't silently fall back to the legacy 1080p path.
    if (videoModeDesired_ && session_->isStreaming() && !session_->isVideoMode())
        enterVideoMode();
    // In video mode the preview keeps streaming during record (same reader), so
    // leave previewReader_ valid.  The legacy path records to a dedicated session
    // that displaces the preview, so its reader becomes stale.
    if (!session_->isVideoMode())
        previewReader_ = nullptr;
    if (!session_->startRecording(recordingPath_.toStdString(), 1920, 1080,
                                    30, 20000000, true,
                                    deviceRotation_.load())) {
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
    if (!session_)
        return;
    // Use the user's bitrate (kbps from the slider) if set; else a sane default.
    const int bitrate = videoBitrate_ > 0 ? videoBitrate_ * 1000
                       : (videoW_ >= 3000) ? 40000000 : 20000000;
    session_->enterVideoMode(videoW_, videoH_, 30, bitrate);
}

void Camera2Bridge::exitVideoMode()
{
    if (session_)
        session_->exitVideoMode();
}

void Camera2Bridge::rebuildVideoIfActive()
{
    // If already in video mode (and not recording), rebuild the encoder at the
    // current size; otherwise the new size just takes effect on the next enter.
    if (session_ && session_->isVideoMode() && !recording_.load()) {
        session_->exitVideoMode();
        enterVideoMode();
    }
}

// Set the H.264 video bitrate (kbps).  Rebuilds the video session if it's up
// (and not recording) so the new bitrate takes effect immediately.
void Camera2Bridge::setVideoBitrate(int kbps)
{
    if (kbps <= 0 || kbps == videoBitrate_)
        return;
    videoBitrate_ = kbps;
    rebuildVideoIfActive();
}

void Camera2Bridge::setVideoWidth(int width)
{
    if (width <= 0 || width == videoW_)
        return;
    videoW_ = width;
    emit videoSizeChanged();
    rebuildVideoIfActive();
}

void Camera2Bridge::setVideoHeight(int height)
{
    if (height <= 0 || height == videoH_)
        return;
    videoH_ = height;
    emit videoSizeChanged();
    rebuildVideoIfActive();
}

void Camera2Bridge::setVideoResolution(int width, int height)
{
    const bool changed = (width > 0 && width != videoW_) || (height > 0 && height != videoH_);
    if (width  > 0) videoW_ = width;
    if (height > 0) videoH_ = height;
    if (changed) {
        emit videoSizeChanged();
        rebuildVideoIfActive();
        if (videoModeDesired_)
            recomputePreviewAspect();   // a different video aspect re-letterboxes the preview
    }
}

void Camera2Bridge::setVideoMode(bool on)
{
    if (on == videoModeDesired_)
        return;
    videoModeDesired_ = on;
    applyVideoMode();
    recomputePreviewAspect();   // letterbox/crop follows the photo vs video aspect
    emit videoModeChanged();
}

void Camera2Bridge::applyVideoMode()
{
    // Reconcile the GUI's desired mode with the session.  No-op until preview is
    // streaming (re-applied from startCamera) and never reconfigures mid-record.
    if (!session_ || !session_->isStreaming() || recording_.load())
        return;
    if (videoModeDesired_ && !session_->isVideoMode())
        enterVideoMode();   // bridge's — applies videoW_/videoH_ (NOT session_->enterVideoMode(), which defaults to 1080p)
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
    // HDR: submit a burst of N frames as a single HAL call — the ISP pipelines
    // them without tearing down/recreating the still pipeline each time.  Each
    // frame gets a different AE exposure compensation step so Mertens fusion has
    // real exposure differences to blend (underexposed / neutral / overexposed).
    // ponytail: ACameraCaptureSession_capture(numRequests=3) = NDK burst.
    if (hdrEnabled_.load() && !hdrBurstActive_) {
        qDebug() << "[camera] HDR: starting sequential capture with EV brackets";
        hdrBurstActive_ = true;
        emit hdrBusyChanged();
        emit hdrCapturingChanged();
        hdrFinalPath_   = outputPath;
        hdrPaths_.clear();
        const int mn = session_->evCompMin();
        const int mx = session_->evCompMax();
        // ponytail: middle frame first in auto-exposure, then read its EXIF
        // for real exposure time, bracket ±2 stops from that.
        hdrEvBrackets_  = {0, std::max(-2, mn), std::min(2, mx)};
        hdrNextFrameIdx_ = 0;
        hdrBaseExpNs_ = 0;  // will be filled from middle frame EXIF
        qDebug() << "[camera] HDR EV order:" << hdrEvBrackets_[0] << hdrEvBrackets_[1] << hdrEvBrackets_[2];
        captureSequentialHdrFrame();
        return;
    }
    // Auto flash: kick an AE precapture, then shoot the moment the HAL settles the
    // metering (FLASH_REQUIRED → fire, CONVERGED → don't).  On/Off need no precapture.
    if (flashMode_ == 2) {
        session_->triggerPrecapture();
        beginAutoFlashCapture(outputPath, 0);
        return;
    }
    doSingleCapture(outputPath);
}

// Poll the cached AE state until the metering settles, then shoot.  This HAL's
// ON_AUTO_FLASH still never fires even at FLASH_REQUIRED, but ON_ALWAYS_FLASH does
// — so we read the AE decision ourselves and force always-flash when it's dark.
// ACAMERA_CONTROL_AE_STATE: 2=CONVERGED 3=LOCKED 4=FLASH_REQUIRED.
void Camera2Bridge::beginAutoFlashCapture(const QString& outputPath, int attempt)
{
    if (!session_) return;
    const int s = session_->aeState();
    const int elapsedMs = attempt * 50;
    const bool settled = (s == 2 || s == 3 || s == 4);
    if ((settled && elapsedMs >= 200) || elapsedMs >= 1200) {
        if (s == 4) {                       // dark → force the flash to actually fire
            session_->setFlashMode(1);      // ON_ALWAYS_FLASH for this shot
            doSingleCapture(outputPath);
            session_->setFlashMode(2);      // restore Auto for the preview/next shot
        } else {                            // bright → no flash
            doSingleCapture(outputPath);
        }
        return;
    }
    QTimer::singleShot(50, this, [this, outputPath, attempt] {
        beginAutoFlashCapture(outputPath, attempt + 1);
    });
}

void Camera2Bridge::doSingleCapture(const QString& outputPath)
{
    if (!session_)
        return;
    const QString path = outputPath.isEmpty() ? defaultPhotoPath() : outputPath;
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Tag this shot with how the phone is currently held.
    session_->setDeviceRotation(queryDeviceRotation());
    if (!session_->capturePhoto(path.toStdString(), deviceRotation_.load()))
        emit cameraError(QString::fromStdString(session_->lastError()));
}

// Camera2 HAL writes EXIF DateTime in UTC; per EXIF spec it should be local time
// with no timezone field, so shift it by the system UTC offset.
void Camera2Bridge::fixExifDateTime(const QString& path)
{
    try {
        auto img = Exiv2::ImageFactory::open(path.toStdString());
        if (!img) return;
        img->readMetadata();
        auto& exif = img->exifData();
        if (exif.empty()) return;

        int offsetSec = QDateTime::currentDateTime().offsetFromUtc();
        if (offsetSec == 0) return; // already UTC — nothing to shift

        auto fixTag = [&](const std::string& key) {
            auto it = exif.findKey(Exiv2::ExifKey(key));
            if (it == exif.end() || it->count() < 20) return;
            std::string val = it->toString();
            // EXIF DateTime format: "2026:06:20 10:16:12" (19 chars)
            if (val.size() < 19) return;
            QDateTime dt = QDateTime::fromString(QString::fromStdString(val.substr(0, 19)), "yyyy:MM:dd HH:mm:ss");
            if (!dt.isValid()) return;
            dt.setTimeSpec(Qt::UTC);
            dt = dt.addSecs(offsetSec);
            // Write back local time in EXIF format
            it->setValue(dt.toString("yyyy:MM:dd HH:mm:ss").toStdString());
        };

        fixTag("Exif.Image.DateTime");
        fixTag("Exif.Photo.DateTimeOriginal");
        fixTag("Exif.Photo.DateTimeDigitized");

        img->writeMetadata();
    } catch (const Exiv2::Error&) {
        // non-fatal — the image is still valid
    }
}

// Read the exposure time (nanoseconds) from a JPEG's EXIF.  Used by HDR
// bracketing to scale manual-exposure frames from the auto-exposed middle frame.
int64_t Camera2Bridge::readExposureNs(const QString& path)
{
    try {
        auto img = Exiv2::ImageFactory::open(path.toStdString());
        if (!img) return 0;
        img->readMetadata();
        auto& exif = img->exifData();
        // EXIF exposure time as rational (e.g. "1/30" → 33ms)
        auto it = exif.findKey(Exiv2::ExifKey("Exif.Photo.ExposureTime"));
        if (it != exif.end()) {
            auto rat = it->toRational();
            if (rat.second > 0)
                return (int64_t)(1.0e9 * rat.first / rat.second);
        }
        // Fallback: ShutterSpeedValue (APEX)
        it = exif.findKey(Exiv2::ExifKey("Exif.Photo.ShutterSpeedValue"));
        if (it != exif.end()) {
            double apex = it->toFloat();
            return (int64_t)(1.0e9 * std::pow(2.0, -apex));
        }
    } catch (const Exiv2::Error&) {}
    return 0;
}

// Photo completion on the GUI thread; routes HDR-burst frames vs single shots.
void Camera2Bridge::onPhotoCaptured(const QString& path, bool ok)
{
    if (hdrBurstActive_) {
        if (!ok) {
            hdrBurstActive_ = false;
            session_->setAutoExposure();  // restore AE
            emit hdrBusyChanged();
            emit hdrCapturingChanged();
            hdrEvBrackets_.clear();
            for (const QString& p : hdrPaths_) QFile::remove(p);
            hdrPaths_.clear();
            emit cameraError(QStringLiteral("HDR capture failed"));
            return;
        }
        hdrPaths_ << path;
        // Read real exposure time from middle frame EXIF for bracket scaling.
        if (hdrNextFrameIdx_ == 0) {
            hdrBaseExpNs_ = readExposureNs(path);
            qDebug() << "[camera] HDR base exposure from EXIF:" << hdrBaseExpNs_ << "ns";
        }
        hdrNextFrameIdx_++;
        if (hdrNextFrameIdx_ < hdrEvBrackets_.size()) {
            // 150ms breather with a non-blocking timer — QML stays alive,
            // camera hardware settles, "Hold still" badge is visible.
            QTimer::singleShot(150, this, [this] {
                captureSequentialHdrFrame();
            });
        } else {
            hdrEvBrackets_.clear();
            finishHdrBurst();
        }
        return;
    }
    if (ok) {
        fixExifDateTime(path);
        setLastPhotoPath(path);
        emit photoSaved(path);
    } else {
        emit cameraError(QStringLiteral("photo capture failed"));
    }
}

// ponytail: sequential single-frame capture with per-frame manual exposure.
// This HAL ignores AE_EXPOSURE_COMPENSATION, so we switch to AE_MODE_OFF
// and set explicit sensor exposure times for real bracketed captures.
void Camera2Bridge::captureSequentialHdrFrame()
{
    if (!session_ || hdrNextFrameIdx_ >= hdrEvBrackets_.size()) return;
    int ev = hdrEvBrackets_[hdrNextFrameIdx_];
    
    if (ev == 0) {
        session_->setAutoExposure();
        qDebug() << "[camera] HDR frame" << hdrNextFrameIdx_ << "EV 0 (auto)";
    } else {
        const int64_t baseNs = hdrBaseExpNs_ > 0 ? hdrBaseExpNs_ : 33'333'333LL;
        int iso = 400;
        int64_t expNs = std::clamp<int64_t>(baseNs * std::pow(2.0, ev),
                                            (int64_t)100'000LL, (int64_t)400'000'000LL);
        session_->setManualExposure(iso, expNs);
        qDebug() << "[camera] HDR frame" << hdrNextFrameIdx_ << "EV:" << ev
                 << "baseNs:" << baseNs << "expNs:" << expNs;
    }
    
    // ponytail: no msleep — submit immediately so QML stays alive.
    const QString path = QDir::tempPath() + QStringLiteral("/furicam_hdr_%1.jpg").arg(hdrNextFrameIdx_);
    doSingleCapture(path);
}

// Fuse the burst on a worker thread (OpenCV is heavy), then emit photoSaved.
void Camera2Bridge::finishHdrBurst()
{
    const QStringList paths = hdrPaths_;
    const QString outDir = QFileInfo(hdrFinalPath_.isEmpty() ? defaultPhotoPath()
                                                             : hdrFinalPath_).absolutePath();
    hdrBurstActive_ = false;
    hdrProcessing_  = true;
    emit hdrCapturingChanged();  // "Hold still" → "Processing…"
    session_->setAutoExposure();  // restore AE after manual-exposure brackets
    qDebug() << "[camera] HDR merge: starting OpenCV fusion on worker thread";
    hdrPaths_.clear();
    QDir().mkpath(outDir);
    std::thread([this, paths, outDir] {
        HdrProcessor proc;
        const QString out = proc.processHdrBurst(paths, outDir);
        for (const QString& p : paths) QFile::remove(p);
        QMetaObject::invokeMethod(this, [this, out] {
            hdrProcessing_ = false;
            emit hdrBusyChanged();
            if (!out.isEmpty()) {
                fixExifDateTime(out);
                setLastPhotoPath(out);
                emit photoSaved(out);
            } else {
                emit cameraError(QStringLiteral("HDR merge failed"));
            }
        }, Qt::QueuedConnection);
    }).detach();
}

void Camera2Bridge::setHdrEnabled(bool on)
{
    if (on == hdrEnabled_.exchange(on))
        return;
    emit hdrEnabledChanged();
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
    if (!session_)
        return;
    // ev in [0,1] (0=most under, 0.5=neutral, 1=most over) → the open camera's AE
    // compensation index range, read from CONTROL_AE_COMPENSATION_RANGE (no
    // device-specific hardcode).  For a symmetric range 0.5 maps to 0 (neutral).
    const int mn = session_->evCompMin();
    const int mx = session_->evCompMax();
    int steps = mn + (int)std::lround(ev * (mx - mn));
    if (steps < mn) steps = mn;
    else if (steps > mx) steps = mx;
    session_->setExposureCompensation(steps);
}

void Camera2Bridge::setFocusDistance(float diopters)
{
    if (session_)
        session_->setFocusDistance(diopters);
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
void Camera2Bridge::setFlashMode(int mode)
{
    if (mode == flashMode_)
        return;
    flashMode_ = mode;
    if (session_)
        session_->setFlashMode(mode);
    emit flashModeChanged();
}

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

QVariantList Camera2Bridge::availableResolutions()
{
    QVariantList list;
    if (!session_)
        return list;
    auto sizes = session_->jpegSizes();
    std::sort(sizes.begin(), sizes.end(),
              [](const CameraSession::StreamConfig& a, const CameraSession::StreamConfig& b) {
                  return (long)a.width * a.height > (long)b.width * b.height;
              });
    for (const auto& s : sizes) {
        QVariantMap m;
        m["width"]  = s.width;
        m["height"] = s.height;
        list.append(m);
    }
    return list;
}

void Camera2Bridge::setResolution(int width, int height)
{
    if (!session_)
        return;
    captureW_ = width;
    captureH_ = height;
    recomputePreviewAspect();   // letterbox + crop follow the new still aspect at once
    session_->setJpegSize(width, height);
    // Recreate the still output (JPEG reader) at the new size by restarting the
    // camera (not while recording — that would interrupt the clip).
    if (ready_.load() && !recording_.load()) {
        stopCamera();
        startCamera();
    }
}

void Camera2Bridge::setJpegQuality(int quality)
{
    if (session_)
        session_->setJpegQuality(quality);
}

void Camera2Bridge::setRawEnabled(bool on)
{
    if (session_)
        session_->setRawEnabled(on);
}

bool Camera2Bridge::rawSupported() const
{
    return session_ && session_->rawSupported();
}

bool Camera2Bridge::isRawEnabled() const
{
    return session_ && session_->isRawEnabled();
}

void Camera2Bridge::qrDecode(const uint8_t* y, int w, int h, int stride)
{
    if (!y || w <= 0 || h <= 0)
        return;
    // Decoding runs on a camera thread; throttle to ~6/sec.
    using namespace std::chrono;
    const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    if (now - lastQrMs_.load() < 160)
        return;
    lastQrMs_.store(now);

    ZXing::ImageView image(y, w, h, ZXing::ImageFormat::Lum, stride);
    ZXing::ReaderOptions opts;
    opts.setFormats(ZXing::BarcodeFormat::QRCode);
    opts.setTryHarder(true);
    const ZXing::Barcode bc = ZXing::ReadBarcode(image, opts);
    if (!bc.isValid())
        return;

    const QString text = QString::fromStdString(bc.text());
    // Map sensor-normalized corners to viewfinder-normalized: inverse of the
    // renderer's texcoord rotation (sensor-only, no device rotation), then the
    // FBO vertical mirror.  Emits {x,y} in [0,1] of the preview item.
    const double rad = -previewRotation() * 3.14159265358979 / 180.0;
    const double cc = std::cos(rad), ss = std::sin(rad);
    // The analysis stream is the full 4:3 FOV but the preview is cropped to the
    // still aspect; expand the sensor-normalized point by the inverse crop so the
    // box lands on the (cropped) preview, matching the renderer's uCrop.
    const double cx = cropScaleX_.load(), cy = cropScaleY_.load();
    QVariantList pts;
    const ZXing::Position& pos = bc.position();
    for (const auto& c : { pos.topLeft(), pos.topRight(), pos.bottomRight(), pos.bottomLeft() }) {
        const double nx = ((double)c.x / w - 0.5) / (cx > 0 ? cx : 1.0);
        const double ny = ((double)c.y / h - 0.5) / (cy > 0 ? cy : 1.0);
        QVariantMap m;
        m["x"] = (cc * nx - ss * ny) + 0.5;
        m["y"] = (ss * nx + cc * ny) + 0.5;
        pts.append(m);
    }
    // Marshal the result to the GUI thread.
    QMetaObject::invokeMethod(this, [this, text, pts] {
        emit qrDetected(text, pts);
    }, Qt::QueuedConnection);
}

} // namespace furicam
