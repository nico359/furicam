#pragma once
#include <QObject>
#include <QString>
#include <QMutex>
#include <QSize>
#include <memory>
#include <atomic>

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>

struct Camera2NDK;
struct MediaNDK;
struct ACameraManager;
struct ACaptureSessionOutputContainer;
struct ACaptureRequest;
struct AImageReader;
struct ANativeWindow;

// Camera2 session for the main back camera (id "0").
// Opens at construction-time, stays open until destroyed.
// Provides: live preview frames via AImageReader + RAW16 still capture to DNG.
// captureRaw() is safe to call any time after open() returns true —
// if the session is not yet active it queues the path and fires when ready.
class Camera2Session : public QObject
{
    Q_OBJECT
public:
    explicit Camera2Session(QObject* parent = nullptr);
    ~Camera2Session();

    // Open camera and start preview. Call once; idempotent if already open.
    Q_INVOKABLE bool open(const QString& cameraId);
    Q_INVOKABLE void close();

    // Returns the AImageReader used for preview frames (AHARDWAREBUFFER_FORMAT_YCbCr).
    // Camera2Preview renderer polls this to display the viewfinder.
    AImageReader* previewReader() const { return m_previewReader; }

    // Capture a JPEG to outputPath (.jpg). Safe to call before session is fully active.
    Q_INVOKABLE void captureJpeg(const QString& outputPath, int quality = 85);

    // Queue a RAW capture to outputPath (.dng).
    // Safe to call before session is fully active.
    Q_INVOKABLE void captureRaw(const QString& outputPath);

    Q_INVOKABLE bool isOpen() const { return m_device != nullptr; }

signals:
    // Emitted when the capture session becomes active (preview running).
    void sessionActive();
    void jpegCaptured(const QString& path);
    void rawCaptured(const QString& path);
    void captureError(const QString& message);

private:
    static void onDeviceDisconnected(void* ctx, ACameraDevice* dev);
    static void onDeviceError(void* ctx, ACameraDevice* dev, int error);
    static void onSessionReady(void* ctx, ACameraCaptureSession* session);
    static void onSessionClosed(void* ctx, ACameraCaptureSession* session);
    static void onSessionActive(void* ctx, ACameraCaptureSession* session);
    static void onRawImageAvailable(void* ctx, AImageReader* reader);
    static void onJpegImageAvailable(void* ctx, AImageReader* reader);
    static void onCaptureCompleted(void* ctx, ACameraCaptureSession*,
                                   ACaptureRequest*, const struct ACameraMetadata*);

    bool setupSession();
    void teardownSession();
    void doCapture();  // fires the pending RAW capture request; call from main thread

    std::unique_ptr<Camera2NDK> m_cam;
    std::unique_ptr<MediaNDK>   m_media;

    ACameraManager*                  m_manager      = nullptr;
    ACameraDevice*                   m_device       = nullptr;
    ACameraCaptureSession*           m_session      = nullptr;
    ACaptureSessionOutputContainer*  m_outContainer = nullptr;
    ACaptureRequest*                 m_previewReq   = nullptr;

    AImageReader*  m_previewReader = nullptr;
    AImageReader*  m_rawReader     = nullptr;
    AImageReader*  m_jpegReader    = nullptr;
    ANativeWindow* m_previewWindow = nullptr;
    ANativeWindow* m_rawWindow     = nullptr;
    ANativeWindow* m_jpegWindow    = nullptr;

    // Protected by m_captureMutex; written on main thread, read on Camera2 thread
    QMutex  m_captureMutex;
    QString m_pendingCapturePath;  // RAW — set by captureRaw(), cleared at start of doCapture()
    QString m_activeCapturePath;   // RAW — set by doCapture(), cleared by onRawImageAvailable
    QString m_pendingJpegPath;     // JPEG — set by captureJpeg(), cleared at start of doCapture()
    QString m_activeJpegPath;      // JPEG — set by doCapture(), cleared by onJpegImageAvailable
    int     m_pendingJpegQuality = 85;

    QString m_cameraId;
    bool    m_sessionReady = false;  // true after onSessionActive

    // Callback structs — must outlive all async operations
    ACameraDevice_StateCallbacks                m_deviceCallbacks{};
    ACameraCaptureSession_stateCallbacks        m_sessionStateCallbacks{};
    ACameraCaptureSession_captureCallbacks      m_captureCallbacks{};
    AImageReader_ImageListener                  m_rawListener{};
    AImageReader_ImageListener                  m_jpegListener{};

    // Sensor metadata cached from camera characteristics for DNG
    float   m_blackLevel  = 64.0f;
    float   m_whiteLevel  = 1023.0f;
    int     m_cfaPattern  = 0;
    float   m_focalLength = 4.0f;
    int     m_rawWidth    = 0;
    int     m_rawHeight   = 0;
    int     m_jpegWidth   = 0;
    int     m_jpegHeight  = 0;
    float   m_colorMatrix[9] = {1,0,0, 0,1,0, 0,0,1}; // XYZ→camera from SENSOR_COLOR_TRANSFORM1
    int     m_illuminant  = 21; // 21=D65
};

