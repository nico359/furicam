// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// Camera2Bridge — Qt5 widget that exposes Android Camera2 native development
// kit (NDK) to QML.  Reaches the Android Camera2 NDK via libhybris on Halium-
// based FuriOS.  See src/camera2/CAMERA2_PLAN.md for architecture and
// project_furicam2_camera_investigation memory for why this exists.
//
// Architecturally this mirrors the standard Camera2 NDK + Qt5 integration
// pattern (preview AImageReader -> AHardwareBuffer -> EGL external OES
// texture -> QQuickFramebufferObject).  Independent observers writing against
// the Camera2 NDK headers will produce structurally similar code regardless of
// reference; that's a property of the application programming interface (API)
// not of any single implementation.

#ifndef FURICAM2_CAMERA2_BRIDGE_H
#define FURICAM2_CAMERA2_BRIDGE_H

#include <QQuickFramebufferObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QMutex>
#include <atomic>
#include <memory>

// Forward declarations to keep NDK types out of QML/Qt headers.  The actual
// types come from <camera/NdkCameraManager.h> et al. in the .cpp.
struct ACameraDevice;
struct ACameraCaptureSession;
struct ACameraManager;
struct ACaptureRequest;
struct ACaptureSessionOutputContainer;
struct AImageReader;

namespace furicam2 {

class CameraSession;
class VideoEncoder;

class Camera2Bridge
    : public QQuickFramebufferObject
{
    Q_OBJECT

    // Lifecycle / status — read from QML for UI state binding.
    Q_PROPERTY(bool    ready               READ isReady               NOTIFY readyChanged)
    Q_PROPERTY(bool    recording           READ isRecording           NOTIFY recordingChanged)
    Q_PROPERTY(bool    hasFrontCamera      READ hasFrontCamera        NOTIFY hasFrontCameraChanged)
    Q_PROPERTY(int     frameCount          READ frameCount            NOTIFY frameCountChanged)

    // Video mode: bind this to the GUI's photo/video toggle.  When true, the
    // bridge pre-configures the camera for simultaneous preview+record (the
    // encoder joins the live preview session) so the record button just taps the
    // running stream — preview never freezes and there is no spin-up.  This keeps
    // the GUI a one-line binding; the Camera2 lifecycle policy lives here.
    Q_PROPERTY(bool    videoMode           READ videoMode WRITE setVideoMode NOTIFY videoModeChanged)

    // Recording size — bind to the app's video-resolution setting so the encoder
    // size is always current before video mode is entered (a reactive binding,
    // not a one-shot signal handler).
    Q_PROPERTY(int     videoWidth          READ videoWidth  WRITE setVideoWidth  NOTIFY videoSizeChanged)
    Q_PROPERTY(int     videoHeight         READ videoHeight WRITE setVideoHeight NOTIFY videoSizeChanged)

    // Current exposure for the on-screen badge ("ISO 200, 1/60").  ISO here is
    // international standards organization sensor sensitivity; shutterNs is
    // exposure time in nanoseconds.
    Q_PROPERTY(int     isoValue            READ isoValue              NOTIFY exposureChanged)
    Q_PROPERTY(qint64  shutterNs           READ shutterNs             NOTIFY exposureChanged)

    // Preview surface aspect ratio (width / height).  QML uses this to size the
    // viewfinder Item so the framebuffer maps 1:1 to pixels.
    Q_PROPERTY(qreal   previewAspectRatio  READ previewAspectRatio    NOTIFY previewAspectRatioChanged)

    // Path of the most recent saved photo (for the gallery thumbnail).
    Q_PROPERTY(QString lastPhotoPath       READ lastPhotoPath         NOTIFY lastPhotoPathChanged)

    // HDR mode — bind to the GUI's HDR toggle.  When on, capturePhoto() takes a
    // short burst and fuses it (HdrProcessor); the GUI stays a one-line binding.
    Q_PROPERTY(bool    hdrEnabled          READ hdrEnabled WRITE setHdrEnabled NOTIFY hdrEnabledChanged)

    // Per-shot flash mode (0=off 1=on 2=auto).  Bind to the GUI's flash setting so
    // it's applied on startup and on every change (not only via a signal on tap).
    Q_PROPERTY(int     flashMode           READ flashMode  WRITE setFlashMode  NOTIFY flashModeChanged)

public:
    explicit Camera2Bridge(QQuickItem* parent = nullptr);
    ~Camera2Bridge() override;

    // QQuickFramebufferObject interface — Qt calls this on the render thread
    // to construct the renderer that pulls AImage frames and uploads them to
    // an OpenGL external texture.
    Renderer* createRenderer() const override;

    // Property accessors.
    bool    isReady()             const { return ready_.load(); }
    bool    isRecording()         const { return recording_.load(); }
    bool    hasFrontCamera()      const { return hasFrontCamera_.load(); }
    int     frameCount()          const { return frameCount_.load(); }
    int     isoValue()            const { return lastIso_.load(); }
    qint64  shutterNs()           const { return lastExposureNs_.load(); }
    qreal   previewAspectRatio()  const { return previewAspectRatio_.load(); }
    bool    hdrEnabled()          const { return hdrEnabled_.load(); }
    void    setHdrEnabled(bool on);
    int     flashMode()           const { return flashMode_; }
    QString lastPhotoPath()       const { QMutexLocker lk(&lastPhotoMutex_); return lastPhotoPath_; }

    // Accessed by the renderer on the render thread.  The renderer reads from
    // the preview AImageReader to populate its OpenGL Embedded Systems (GLES)
    // texture each frame.
    AImageReader* previewReader() const { return previewReader_; }
    int           displayRotation() const { return displayRotation_.load(); }
    // Centred sub-rect of the (4:3 full-FOV) preview stream to show, so the preview
    // is cropped to the chosen still aspect (WYSIWYG).  Read on the render thread.
    float         cropScaleX() const { return cropScaleX_.load(); }
    float         cropScaleY() const { return cropScaleY_.load(); }
    // Mirror the preview left-right for the front (selfie) camera.
    bool          previewMirrored() const { return lensFacingPref_.load() == 0; }

    // ── QML API ─────────────────────────────────────────────────────────────
    // All Q_INVOKABLE methods may be called from the QML/JavaScript thread.

    // Start the camera session.  Opens the back camera by default and begins
    // streaming preview frames.  Emits readyChanged(true) once preview is
    // delivering frames.
    Q_INVOKABLE void startCamera();

    // Tear down the current camera session.  Safe to call repeatedly.
    Q_INVOKABLE void stopCamera();

    // Switch between back and front camera.  Tears down and reopens.
    Q_INVOKABLE void switchCamera();

    // All cameras: [{index, facing(0=front,1=back), megapixels}].  selectCamera()
    // opens a specific one by index (e.g. the secondary back/macro camera).
    Q_INVOKABLE QVariantList availableCameras();
    Q_INVOKABLE void selectCamera(int index);
    // Facing of the currently-OPENED camera (0=front, 1=back) — ground truth for
    // syncing the GUI's camera-position state.
    Q_INVOKABLE int  currentFacing() const { return lensFacingPref_.load(); }

    // Begin recording to outputPath (a writable filesystem path with .mp4
    // extension).  If outputPath is empty, uses defaultOutputPath().  Emits
    // recordingChanged(true) once the AMediaCodec encoder is producing
    // hardware (HW) H.264 buffers and AMediaMuxer is writing to disk.
    Q_INVOKABLE void startRecording(const QString& outputPath = QString());

    // Stop recording.  Drains the encoder, finalises the MPEG-4 Part 14 (MP4)
    // container, emits recordingSaved(path) on success.
    Q_INVOKABLE void stopRecording();

    // Pre-warm the microphone ahead of recording (e.g. on entering video mode)
    // so startRecording attaches the already-running audio stream instead of
    // spinning up the mic + encoder.  releaseRecording() tears it down (e.g. on
    // leaving video mode).  audioReady() is true once the hot mic has captured
    // its first sample (mic confirmed working).
    Q_INVOKABLE void prepareRecording();
    Q_INVOKABLE void releaseRecording();
    Q_INVOKABLE bool audioReady() const;

    // Pre-configure simultaneous preview+record (e.g. on entering video mode):
    // the encoder surface joins the live preview session so startRecording just
    // taps it — preview never freezes and there is no capture-session spin-up.
    // exitVideoMode() restores the preview-only session.  Prefer the videoMode
    // property (above) from QML; these are the lower-level primitives.
    Q_INVOKABLE void enterVideoMode();
    Q_INVOKABLE void exitVideoMode();

    bool videoMode() const { return videoModeDesired_; }
    void setVideoMode(bool on);

    // Take a single still photo.  Triggers a one-shot capture request to the
    // Joint Photographic Experts Group (JPEG) AImageReader.  settingsJson may
    // include shooting overrides ({"flash": "auto", "exposureComp": 0.5}).
    // Emits photoSaved(path) on success.
    Q_INVOKABLE void capturePhoto(const QString& outputPath = QString(),
                                  const QString& settingsJson = QString());

    // Manual exposure controls.
    //   setAutoExposure()                  — restore AE_MODE_ON
    //   setManualExposure(iso, exposureMs) — switch to AE_MODE_OFF and apply values
    //   setExposureCompensation(ev)        — 0.0 most under, 0.5 neutral, 1.0 most over
    Q_INVOKABLE void setAutoExposure();
    Q_INVOKABLE void setManualExposure(int iso, int exposureMs);
    Q_INVOKABLE void setExposureCompensation(float ev);

    // Lock auto-exposure (AE) and auto-white-balance (AWB) at current values.
    Q_INVOKABLE void setAELock(bool lock);
    Q_INVOKABLE void setAWBLock(bool lock);

    // Continuous-video auto-focus is the default.  Lock holds the current focal
    // distance; unlock returns to continuous.
    Q_INVOKABLE void setFocusLock(bool lock);
    Q_INVOKABLE void setAutoFocus();

    // Tap-to-focus.  x,y in normalised [0,1] across the visible preview surface.
    Q_INVOKABLE void setFocusPoint(float x, float y);

    // Torch (continuous flash for video lighting).
    Q_INVOKABLE void setTorch(bool on);
    Q_INVOKABLE void setSceneMode(int mode);   // 0=off; 2=ACTION (freeze motion)
    Q_INVOKABLE void setRawEnabled(bool on);   // also save a .dng per shot
    Q_INVOKABLE bool rawSupported() const;     // open camera advertises RAW capability
    Q_INVOKABLE void setNoiseReduction(bool on);    // HIGH_QUALITY denoise on stills
    Q_INVOKABLE void setEdgeEnhancement(bool on);   // HIGH_QUALITY sharpening on stills
    Q_INVOKABLE void setFlashMode(int mode);   // 0=off, 1=on, 2=auto (per-shot)

    // Called from QML when device rotation changes (degrees, 0/90/180/270
    // clockwise from natural portrait).  Combined with sensor mount angle to
    // compute displayRotation_ for the renderer.
    Q_INVOKABLE void setDeviceRotation(int degrees);

    // App-UI-matching controls so the existing QML can drive Camera2 unchanged.
    //   white balance mode: 0=Auto,1=Daylight,2=Cloudy,3=Fluorescent,4=Incandescent
    //   zoom: ratio in [1, maxZoom()]
    Q_INVOKABLE void  setWhiteBalanceMode(int appMode);
    Q_INVOKABLE void  setZoom(float ratio);
    Q_INVOKABLE float maxZoom() const;

    // Available JPEG (photo) capture sizes for the open camera, largest first,
    // each as {"width": w, "height": h}.  setResolution() picks one and restarts
    // the camera to recreate the still output at that size.
    Q_INVOKABLE QVariantList availableResolutions();
    Q_INVOKABLE void setResolution(int width, int height);
    Q_INVOKABLE void setJpegQuality(int quality);   // 1..100; HAL encodes at this directly

    // Encoder-capable video sizes (PRIVATE sizes the H.264 encoder accepts: this
    // device caps at the 4K box ≤3840×2160), clean aspects, largest first.
    Q_INVOKABLE QVariantList availableVideoResolutions();

    // Video recording size (e.g. 3840x2160, 1920x1080).  Applied to the encoder;
    // if already in video mode (and not recording) the session is rebuilt at the
    // new size.  Bind to the app's video-resolution setting.
    Q_INVOKABLE void setVideoResolution(int width, int height);
    // Video target-fps range: (30,30) steady 30; (5,30) allow low-light drop.
    Q_INVOKABLE void setVideoFps(int minFps, int maxFps);
    // H.264 bitrate in kbps (from the bitrate slider).
    Q_INVOKABLE void setVideoBitrate(int kbps);
    int  videoWidth()  const { return videoW_; }
    int  videoHeight() const { return videoH_; }
    void setVideoWidth(int width);
    void setVideoHeight(int height);

signals:
    void readyChanged();
    void recordingChanged();
    void hasFrontCameraChanged();
    void frameCountChanged();
    void exposureChanged();
    void previewAspectRatioChanged();
    void lastPhotoPathChanged();
    void videoModeChanged();
    void videoSizeChanged();
    void hdrEnabledChanged();
    void flashModeChanged();

    // One-shot signals for outcomes.
    void cameraError(const QString& message);
    void recordingSaved(const QString& path);
    void photoSaved(const QString& path);

    // A QR/barcode was decoded from the live preview.  points is the 4 corners
    // (topLeft, topRight, bottomRight, bottomLeft) as {x,y} normalized to [0,1].
    void qrDetected(const QString& text, const QVariantList& points);

private slots:
    // iio-sensor-proxy PropertiesChanged → refresh the cached device orientation.
    void onSensorPropertiesChanged(const QString& iface, const QVariantMap& changed,
                                   const QStringList& invalidated);

private:
    // Helpers used by the implementation; not part of the QML surface.
    void initCamera();
    void stopCameraSession();
    void updateDisplayRotation();
    void pickPreviewStreamSize();     // set previewStream{W,H}_ (4:3 full FOV)
    void recomputePreviewAspect();    // previewAspectRatio_ + cropScale from still aspect
    void effectiveCaptureSize(int& w, int& h);   // chosen still size, else sensor max
    void onPhotoCaptured(const QString& path, bool ok);   // single + HDR-burst routing
    void doSingleCapture(const QString& outputPath);      // one still (after any precapture)
    void beginAutoFlashCapture(const QString& outputPath, int attempt);  // poll AE then shoot
    void captureNextHdrFrame();
    void finishHdrBurst();            // fuse the burst on a worker thread
    void claimAccelerometer();        // claim iio-sensor-proxy so orientation is live
    int  queryDeviceRotation();       // on-demand device tilt (0/90/180/270) for capture tagging
    void applyVideoMode();            // reconcile videoModeDesired_ with the session
    void rebuildVideoIfActive();      // re-enter video mode at a new size if active
    void qrDecode(const uint8_t* y, int width, int height, int rowStride);  // on a camera thread
    QString defaultVideoPath() const;
    QString defaultPhotoPath() const;

    void setLastPhotoPath(const QString& path);

    // ── State ───────────────────────────────────────────────────────────────
    // Owned NDK / lifecycle objects.
    std::unique_ptr<CameraSession> session_;
    std::unique_ptr<VideoEncoder>  encoder_;
    AImageReader*                  previewReader_  = nullptr;
    AImageReader*                  jpegReader_     = nullptr;

    // Status flags read from QML; std::atomic for safe cross-thread access.
    std::atomic<bool>    ready_              {false};
    std::atomic<bool>    recording_          {false};
    std::atomic<bool>    hasFrontCamera_     {false};
    std::atomic<int>     lensFacingPref_     {1};       // 1=back, 0=front
    std::atomic<int>     selectedCameraIndex_{-1};      // explicit camera pick (-1 = by facing)
    bool                 videoModeDesired_   = false;   // GUI's photo/video toggle
    int                  videoW_             = 1920;     // recording size
    int                  videoH_             = 1080;
    int                  videoFpsMin_        = 30;       // video target-fps range
    int                  videoFpsMax_        = 30;
    int                  videoBitrate_       = 0;        // kbps; 0 = resolution-scaled default
    int                  captureW_           = 0;        // chosen still size (0 = sensor max)
    int                  captureH_           = 0;
    int                  previewStreamW_     = 1280;     // preview stream size; its aspect
    int                  previewStreamH_     = 720;      // follows the still aspect (WYSIWYG)
    std::atomic<int>     frameCount_         {0};
    std::atomic<int32_t> lastIso_            {0};
    std::atomic<int64_t> lastExposureNs_     {0};
    std::atomic<int64_t> lastQrMs_           {0};   // throttle QR decode rate
    std::atomic<float>   previewAspectRatio_ {9.0f / 16.0f};
    std::atomic<float>   cropScaleX_         {1.0f};
    std::atomic<float>   cropScaleY_         {1.0f};
    std::atomic<int>     sensorOrientation_  {90};
    std::atomic<int>     deviceRotation_     {0};
    std::atomic<int>     displayRotation_    {90};
    std::atomic<int>     cachedDeviceRotation_ {0};  // orientation cache (D-Bus signal-fed)

    // HDR burst state (GUI thread).  kHdrFrames matches the original 3-frame burst.
    static constexpr int kHdrFrames = 3;
    std::atomic<bool> hdrEnabled_     {false};
    bool              hdrBurstActive_ = false;
    QStringList       hdrPaths_;
    QString           hdrFinalPath_;
    int               flashMode_      = 0;   // 0=off 1=on 2=auto (mirrors the engine)

    // Photo / video bookkeeping.
    QString        lastPhotoPath_;
    QString        recordingPath_;
    mutable QMutex lastPhotoMutex_;
    bool           orientationMonitorStarted_ = false;
};

} // namespace furicam2

#endif // FURICAM2_CAMERA2_BRIDGE_H
