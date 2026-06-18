#include "camera2session.h"
#include "ndk_loader.h"
#include "rawsave.h"

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

#include <QtConcurrent/QtConcurrent>
#include <QDebug>
#include <QFile>
#include <QMutexLocker>
#include <QMetaObject>

// RAW16 = 0x20, preview YUV_420_888 = 0x23
static const int32_t kFmtRaw16 = 0x20;
static const int32_t kFmtYuv   = 0x23;

// Preview size — 4:3 to match sensor (1440×1080 landscape, rotated 90° = 1080×1440 portrait)
const int32_t kPreviewW = 1440;
const int32_t kPreviewH = 1080;

Camera2Session::Camera2Session(QObject* parent)
    : QObject(parent)
    , m_cam(std::make_unique<Camera2NDK>())
    , m_media(std::make_unique<MediaNDK>())
{}

Camera2Session::~Camera2Session() { close(); }

bool Camera2Session::open(const QString& cameraId)
{
    if (m_device) return true;  // already open

    // Reset per-camera metadata so stream-config scan runs fresh for each camera
    m_rawWidth = m_rawHeight = m_jpegWidth = m_jpegHeight = 0;
    m_blackLevel = 64.0f; m_whiteLevel = 1023.0f;
    m_cfaPattern = 0; m_focalLength = 4.0f; m_illuminant = 21;
    std::fill(std::begin(m_colorMatrix), std::end(m_colorMatrix), 0.0f);
    m_colorMatrix[0] = m_colorMatrix[4] = m_colorMatrix[8] = 1.0f;

    if (!load_camera2ndk(*m_cam)) {
        emit captureError("Failed to load libcamera2ndk.so");
        return false;
    }
    if (!load_mediandk(*m_media)) {
        emit captureError("Failed to load libmediandk.so");
        return false;
    }

    // Binder thread pool is required for Camera2 on Android/Halium —
    // without it the camera service callbacks have no thread to run on.
    start_binder_thread_pool();

    m_cameraId = cameraId;
    m_manager = m_cam->ACameraManager_create();
    if (!m_manager) {
        emit captureError("ACameraManager_create failed");
        return false;
    }

    // Cache sensor metadata from characteristics
    ACameraMetadata* chars = nullptr;
    if (m_cam->ACameraManager_getCameraCharacteristics(
            m_manager, cameraId.toUtf8().constData(), &chars) == ACAMERA_OK && chars) {
        ACameraMetadata_const_entry e;
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK)
            m_blackLevel = e.data.i32[0];
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK)
            m_whiteLevel = e.data.i32[0];
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK)
            m_cfaPattern = e.data.u8[0];
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &e) == ACAMERA_OK)
            m_focalLength = e.data.f[0];

        // Color matrix XYZ→camera (maps directly to DNG ColorMatrix1)
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_COLOR_TRANSFORM1, &e) == ACAMERA_OK
                && e.count >= 9) {
            for (int i = 0; i < 9; i++)
                m_colorMatrix[i] = (float)e.data.r[i].numerator / (float)e.data.r[i].denominator;
        }

        // Illuminant for ColorMatrix1 (17=Standard light A, 21=D65, 23=D50)
        if (m_cam->ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK)
            m_illuminant = e.data.u8[0];

        // Get RAW size (first = largest) and JPEG size from stream configs
        if (m_cam->ACameraMetadata_getConstEntry(chars,
                ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
            for (uint32_t i = 0; i + 3 < e.count; i += 4) {
                int32_t fmt = e.data.i32[i], w = e.data.i32[i+1], h = e.data.i32[i+2];
                bool isOutput = !e.data.i32[i+3];
                if (isOutput && fmt == kFmtRaw16 && m_rawWidth == 0) {
                    m_rawWidth  = w;
                    m_rawHeight = h;
                }
                if (isOutput && fmt == 0x100 /*JPEG*/ && m_jpegWidth == 0) {
                    m_jpegWidth  = w;
                    m_jpegHeight = h;
                }
            }
        }
        // Fallback: use RAW dims for JPEG if nothing found
        if (m_jpegWidth == 0) { m_jpegWidth = m_rawWidth; m_jpegHeight = m_rawHeight; }
        m_cam->ACameraMetadata_free(chars);
    }

    qInfo() << "camera2session: RAW sensor" << m_rawWidth << "x" << m_rawHeight
            << "bl=" << m_blackLevel << "wl=" << m_whiteLevel
            << "JPEG output" << m_jpegWidth << "x" << m_jpegHeight;

    m_deviceCallbacks = {};
    m_deviceCallbacks.context        = this;
    m_deviceCallbacks.onDisconnected = onDeviceDisconnected;
    m_deviceCallbacks.onError        = onDeviceError;

    if (m_cam->ACameraManager_openCamera(
            m_manager, cameraId.toUtf8().constData(), &m_deviceCallbacks, &m_device) != ACAMERA_OK) {
        emit captureError("Failed to open camera " + cameraId);
        return false;
    }

    return setupSession();
}

bool Camera2Session::setupSession()
{
    // Preview AImageReader
    m_media->AImageReader_new(kPreviewW, kPreviewH, kFmtYuv, 4, &m_previewReader);
    m_media->AImageReader_getWindow(m_previewReader, &m_previewWindow);

    // JPEG AImageReader — 2 buffers at largest supported JPEG resolution
    m_media->AImageReader_new(m_jpegWidth, m_jpegHeight, 0x100 /*JPEG*/, 2, &m_jpegReader);
    m_media->AImageReader_getWindow(m_jpegReader, &m_jpegWindow);

    // ponytail: RAW reader removed — Halium HAL only supports 2 simultaneous outputs.
    //           Add back when JPEG is confirmed working; use a separate capture session or
    //           drop preview during RAW capture if RAW is needed.

    // JPEG image listener
    m_jpegListener = {};
    m_jpegListener.context          = this;
    m_jpegListener.onImageAvailable = onJpegImageAvailable;
    m_media->AImageReader_setImageListener(m_jpegReader, &m_jpegListener);

    // Session output container
    m_cam->ACaptureSessionOutputContainer_create(&m_outContainer);

    auto addOutput = [&](ANativeWindow* win) {
        ACaptureSessionOutput* out = nullptr;
        m_cam->ACaptureSessionOutput_create(win, &out);
        m_cam->ACaptureSessionOutputContainer_add(m_outContainer, out);
        // ponytail: ACaptureSessionOutput leaked — it lives for the session lifetime
    };
    addOutput(m_previewWindow);
    addOutput(m_jpegWindow);

    m_sessionStateCallbacks = {};
    m_sessionStateCallbacks.context  = this;
    m_sessionStateCallbacks.onReady  = onSessionReady;
    m_sessionStateCallbacks.onClosed = onSessionClosed;
    m_sessionStateCallbacks.onActive = onSessionActive;

    if (m_cam->ACameraDevice_createCaptureSession(
            m_device, m_outContainer, &m_sessionStateCallbacks, &m_session) != ACAMERA_OK) {
        emit captureError("Failed to create capture session");
        return false;
    }

    // Repeating preview request targeting preview surface only
    m_cam->ACameraDevice_createCaptureRequest(m_device, TEMPLATE_PREVIEW, &m_previewReq);

    ACameraOutputTarget* previewTarget = nullptr;
    m_cam->ACameraOutputTarget_create(m_previewWindow, &previewTarget);
    m_cam->ACaptureRequest_addTarget(m_previewReq, previewTarget);
    // ponytail: previewTarget leaked — lives for session lifetime

    m_cam->ACameraCaptureSession_setRepeatingRequest(m_session, nullptr, 1, &m_previewReq, nullptr);

    return true;
}

void Camera2Session::captureRaw(const QString& outputPath)
{
    if (!m_device) { emit captureError("Session not open"); return; }
    { QMutexLocker lk(&m_captureMutex); m_pendingCapturePath = outputPath; }
    if (m_sessionReady)
        QMetaObject::invokeMethod(this, [this]{ doCapture(); }, Qt::QueuedConnection);
}

void Camera2Session::captureJpeg(const QString& outputPath, int quality)
{
    if (!m_device) { emit captureError("Session not open"); return; }
    { QMutexLocker lk(&m_captureMutex); m_pendingJpegPath = outputPath; m_pendingJpegQuality = quality; }
    if (m_sessionReady)
        QMetaObject::invokeMethod(this, [this]{ doCapture(); }, Qt::QueuedConnection);
}

void Camera2Session::doCapture()
{
    QString rawPath, jpegPath; int jpegQuality = 85;
    {
        QMutexLocker lk(&m_captureMutex);
        if (m_pendingCapturePath.isEmpty() && m_pendingJpegPath.isEmpty()) return;
        rawPath  = m_pendingCapturePath;  m_pendingCapturePath.clear();
        jpegPath = m_pendingJpegPath;     m_pendingJpegPath.clear();
        jpegQuality = m_pendingJpegQuality;
        m_activeCapturePath = rawPath;
        m_activeJpegPath    = jpegPath;
    }

    if (!m_session) return;

    ACaptureRequest* req = nullptr;
    if (m_cam->ACameraDevice_createCaptureRequest(m_device, TEMPLATE_STILL_CAPTURE, &req) != ACAMERA_OK || !req) {
        qWarning() << "camera2session: failed to create still capture request";
        return;
    }

    // Set JPEG quality directly from app setting — no re-encode needed
    uint8_t q = (uint8_t)qBound(1, jpegQuality, 100);
    m_cam->ACaptureRequest_setEntry_u8(req, 0x0202 /*ACAMERA_JPEG_QUALITY*/, 1, &q);

    // Always include preview; add JPEG surface when requested
    ACameraOutputTarget* previewTarget = nullptr;
    m_cam->ACameraOutputTarget_create(m_previewWindow, &previewTarget);
    m_cam->ACaptureRequest_addTarget(req, previewTarget);

    if (!rawPath.isEmpty()) {
        qWarning() << "camera2session: RAW capture not supported in current session (no RAW reader)";
        { QMutexLocker lk(&m_captureMutex); m_activeCapturePath.clear(); }
    }
    if (!jpegPath.isEmpty()) {
        ACameraOutputTarget* jpegTarget = nullptr;
        m_cam->ACameraOutputTarget_create(m_jpegWindow, &jpegTarget);
        m_cam->ACaptureRequest_addTarget(req, jpegTarget);
        qInfo() << "camera2session: firing JPEG capture to" << jpegPath;
    }

    m_captureCallbacks = {};
    m_captureCallbacks.context            = this;
    m_captureCallbacks.onCaptureCompleted = onCaptureCompleted;

    camera_status_t st = m_cam->ACameraCaptureSession_capture(m_session, &m_captureCallbacks, 1, &req, nullptr);
    qInfo() << "camera2session: capture submitted, status=" << st;
    m_cam->ACaptureRequest_free(req);
}

void Camera2Session::teardownSession()
{
    m_sessionReady = false;
    if (m_session) {
        m_cam->ACameraCaptureSession_stopRepeating(m_session);
        m_cam->ACameraCaptureSession_close(m_session);
        m_session = nullptr;
    }
    if (m_previewReq)   { m_cam->ACaptureRequest_free(m_previewReq);   m_previewReq = nullptr; }
    if (m_outContainer) { m_cam->ACaptureSessionOutputContainer_free(m_outContainer); m_outContainer = nullptr; }
    if (m_rawReader)     { m_media->AImageReader_delete(m_rawReader);     m_rawReader = nullptr; }
    if (m_jpegReader)    { m_media->AImageReader_delete(m_jpegReader);    m_jpegReader = nullptr; }
    if (m_previewReader) { m_media->AImageReader_delete(m_previewReader); m_previewReader = nullptr; }
    m_previewWindow = nullptr;
    m_rawWindow     = nullptr;
    m_jpegWindow    = nullptr;
}

void Camera2Session::close()
{
    teardownSession();
    if (m_device)  { m_cam->ACameraDevice_close(m_device);    m_device  = nullptr; }
    if (m_manager) { m_cam->ACameraManager_delete(m_manager); m_manager = nullptr; }
}

// ── Static callbacks ──────────────────────────────────────────────────────────

void Camera2Session::onDeviceDisconnected(void* ctx, ACameraDevice*)
{
    auto* self = static_cast<Camera2Session*>(ctx);
    QMetaObject::invokeMethod(self, [self]{
        emit self->captureError("Camera disconnected");
    }, Qt::QueuedConnection);
}

void Camera2Session::onDeviceError(void* ctx, ACameraDevice*, int error)
{
    auto* self = static_cast<Camera2Session*>(ctx);
    QMetaObject::invokeMethod(self, [self, error]{
        emit self->captureError(QString("Camera error %1").arg(error));
    }, Qt::QueuedConnection);
}

void Camera2Session::onSessionReady(void*, ACameraCaptureSession*) {}
void Camera2Session::onSessionClosed(void*, ACameraCaptureSession*) {}

void Camera2Session::onSessionActive(void* ctx, ACameraCaptureSession*)
{
    auto* self = static_cast<Camera2Session*>(ctx);
    QMetaObject::invokeMethod(self, [self]{
        qInfo() << "camera2session: onSessionActive (ready=" << self->m_sessionReady << ")";
        if (!self->m_sessionReady) {
            self->m_sessionReady = true;
            qInfo() << "camera2session: session active, preview running";
            emit self->sessionActive();
        }

        // Fire pending capture only once — doCapture clears pendingCapturePath immediately
        bool hasPending;
        {
            QMutexLocker lk(&self->m_captureMutex);
            hasPending = !self->m_pendingCapturePath.isEmpty();
        }
        if (hasPending) self->doCapture();
    }, Qt::QueuedConnection);
}

void Camera2Session::onRawImageAvailable(void* ctx, AImageReader* reader)
{
    auto* self = static_cast<Camera2Session*>(ctx);
    qInfo() << "camera2session: onRawImageAvailable";

    AImage* image = nullptr;
    if (self->m_media->AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) {
        qWarning() << "camera2session: acquireLatestImage failed";
        return;
    }

    QString path;
    {
        QMutexLocker lk(&self->m_captureMutex);
        path = self->m_activeCapturePath;
        self->m_activeCapturePath.clear();
    }

    if (path.isEmpty()) {
        qWarning() << "camera2session: no active capture path, dropping image";
        self->m_media->AImage_delete(image);
        return;
    }

    // Extract pixel data HERE on the camera callback thread — hybris threads only.
    // All AImage_* calls must happen on a hybris-aware thread; the Qt thread pool is not.
    int32_t width = 0, height = 0;
    self->m_media->AImage_getWidth(image, &width);
    self->m_media->AImage_getHeight(image, &height);

    uint8_t* data    = nullptr;
    int      dataLen = 0, stride = 0;
    bool ok = (self->m_media->AImage_getPlaneData(image, 0, &data, &dataLen) == AMEDIA_OK && data);
    if (ok) self->m_media->AImage_getPlaneRowStride(image, 0, &stride);

    std::vector<uint8_t> rawBytes;
    if (ok && width > 0 && height > 0) {
        rawBytes.resize((size_t)width * 2 * height);
        if (stride == width * 2) {
            memcpy(rawBytes.data(), data, rawBytes.size());
        } else {
            for (int row = 0; row < height; row++)
                memcpy(rawBytes.data() + (size_t)row * width * 2,
                       data + (size_t)row * stride, (size_t)width * 2);
        }
        qInfo() << "camera2session: extracted" << width << "x" << height
                << "RAW16, stride=" << stride;
    } else {
        qWarning() << "camera2session: AImage_getPlaneData failed, w=" << width << "h=" << height;
    }

    // Done with the AImage — delete it on this thread before leaving
    self->m_media->AImage_delete(image);

    if (rawBytes.empty()) {
        QMetaObject::invokeMethod(self, [self]{ emit self->captureError("Failed to read RAW image data"); },
                                  Qt::QueuedConnection);
        return;
    }

    // Capture metadata for DNG
    float bl = self->m_blackLevel, wl = self->m_whiteLevel;
    int   cfa = self->m_cfaPattern;
    float cm[9]; memcpy(cm, self->m_colorMatrix, sizeof(cm));
    int   illuminant = self->m_illuminant;

    // DNG write is plain libtiff — no Android NDK calls, safe on any thread
    QtConcurrent::run([=]() mutable {
        qInfo() << "camera2session: writing DNG";
        bool saved = saveRaw16AsDng(rawBytes, width, height, path, bl, wl, cfa, cm, illuminant);
        qInfo() << "camera2session: DNG write done, ok=" << saved;
        if (saved)
            QMetaObject::invokeMethod(self, [self, path]{ emit self->rawCaptured(path); },
                                      Qt::QueuedConnection);
        else
            QMetaObject::invokeMethod(self, [self]{ emit self->captureError("DNG write failed"); },
                                      Qt::QueuedConnection);
    });
}

void Camera2Session::onCaptureCompleted(void*, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata*)
{
    qInfo() << "camera2session: onCaptureCompleted";
}

void Camera2Session::onJpegImageAvailable(void* ctx, AImageReader* reader)
{
    auto* self = static_cast<Camera2Session*>(ctx);

    AImage* image = nullptr;
    if (self->m_media->AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) {
        qWarning() << "camera2session: JPEG acquireLatestImage failed";
        return;
    }

    QString path;
    {
        QMutexLocker lk(&self->m_captureMutex);
        path = self->m_activeJpegPath;
        self->m_activeJpegPath.clear();
    }

    if (path.isEmpty()) {
        self->m_media->AImage_delete(image);
        return;
    }

    // Extract JPEG bytes on camera thread (hybris rule)
    uint8_t* data = nullptr; int dataLen = 0;
    bool ok = (self->m_media->AImage_getPlaneData(image, 0, &data, &dataLen) == AMEDIA_OK && data && dataLen > 0);

    std::vector<uint8_t> jpegBytes;
    if (ok) { jpegBytes.assign(data, data + dataLen); }
    self->m_media->AImage_delete(image);

    if (jpegBytes.empty()) {
        QMetaObject::invokeMethod(self, [self]{ emit self->captureError("JPEG read failed"); }, Qt::QueuedConnection);
        return;
    }

    qInfo() << "camera2session: JPEG" << jpegBytes.size() << "bytes, saving to" << path;

    // Write on background thread — plain file I/O, no NDK calls
    QtConcurrent::run([=]() mutable {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(reinterpret_cast<const char*>(jpegBytes.data()), jpegBytes.size());
            f.close();
            qInfo() << "camera2session: JPEG saved to" << path;
            QMetaObject::invokeMethod(self, [self, path]{ emit self->jpegCaptured(path); }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(self, [self]{ emit self->captureError("JPEG write failed"); }, Qt::QueuedConnection);
        }
    });
}

