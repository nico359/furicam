// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// CameraSession — Qt-free C++ wrapper around the Android Camera2 native
// development kit (NDK) lifecycle, reached via the libhybris shim
// (Camera2NDKShim.c).  Milestone 2 scope: create the camera manager, enumerate
// cameras, read each one's characteristics, open a camera, and dump
// capabilities.  Later milestones grow this class with capture sessions,
// outputs and per-request controls.
//
// Deliberately depends only on the C++ standard library and Camera2NDK.h (no
// Qt), so it can be exercised by the tiny src/camera2/tools/camera2_probe.cpp
// command-line harness on the phone without building the whole GUI app.

#ifndef FURICAM_CAMERA_SESSION_H
#define FURICAM_CAMERA_SESSION_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Camera2NDK.h"

namespace furicam {

class VideoEncoder;
class AudioEncoder;

class CameraSession {
public:
    // One supported output stream configuration (one (format, size) the camera
    // can deliver).  Input configs are recorded too but flagged.
    struct StreamConfig {
        int  format = 0;   // AIMAGE_FORMAT_* / HAL pixel format
        int  width  = 0;
        int  height = 0;
        bool isInput = false;
    };

    // Everything we read from a camera's characteristics for M2 logging and for
    // later milestones to size streams / gate manual controls.
    struct CameraInfo {
        std::string id;
        int  facing            = -1;  // ACAMERA_LENS_FACING_*
        int  sensorOrientation = 0;   // degrees, clockwise
        int  hardwareLevel     = -1;  // ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_*
        int  pixelArrayW       = 0;   // full sensor resolution
        int  pixelArrayH       = 0;
        int  activeArray[4]    = {0, 0, 0, 0};  // raw int32[4] from the HAL
        int  isoMin            = 0;
        int  isoMax            = 0;
        int64_t exposureMinNs  = 0;
        int64_t exposureMaxNs  = 0;
        bool manualSensor      = false;  // derived: MANUAL_SENSOR capability present
        float minFocusDistance = 0.0f;   // 0 = fixed focus; diopters (>0 = closest focus distance)
        int   focusDistanceCalibration = 0;  // LENS_INFO_FOCUS_DISTANCE_CALIBRATION: 0=UNCALIBRATED, 1=APPROXIMATE, 2=CALIBRATED
        // Capability ranges read from the open camera (not hardcoded).
        float zoomRatioMin     = 1.0f;   // CONTROL_ZOOM_RATIO_RANGE
        float zoomRatioMax     = 0.0f;   // 0 = range absent (fall back to a default)
        int   evCompMin        = 0;      // CONTROL_AE_COMPENSATION_RANGE
        int   evCompMax        = 0;      // max==min = range absent
        float evCompStep       = 0.0f;   // CONTROL_AE_COMPENSATION_STEP (EV per index)
        bool  videoStabSupported = false;// CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES has ON
        // ── RAW/DNG characteristics ───────────────────────────────────────
        bool  rawSupported  = false;     // RAW capability + a RAW16 output size present
        int   raw16W        = 0;         // largest RAW16 output size
        int   raw16H        = 0;
        int   whiteLevel    = 1023;
        int   blackLevel[4] = {64, 64, 64, 64};
        int   cfaArrangement = 0;        // 0=RGGB 1=GRBG 2=GBRG 3=BGGR
        int   refIlluminant1 = 21, refIlluminant2 = 17;
        bool  haveColor2  = false;       // dual-illuminant calibration present
        bool  haveForward = false;       // forward matrices present
        double colorMatrix1[9]   = {0};  // SENSOR_COLOR_TRANSFORM1 (XYZ->camera)
        double colorMatrix2[9]   = {0};
        double forwardMatrix1[9] = {0};  // SENSOR_FORWARD_MATRIX1  (camera->XYZ D50)
        double forwardMatrix2[9] = {0};
        double neutralColorPoint[3] = {1, 1, 1};  // AsShotNeutral fallback
        std::vector<int>          capabilities;  // ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_*
        std::vector<StreamConfig> outputs;       // output stream configs only
    };

    // Sink for human-readable log lines (defaults to stdout).  CameraSession
    // never assumes Qt; the Bridge can route these to qDebug/signals in M8.
    using LogFn = std::function<void(const std::string&)>;

    explicit CameraSession(LogFn log = {});
    ~CameraSession();

    CameraSession(const CameraSession&)            = delete;
    CameraSession& operator=(const CameraSession&) = delete;

    // True when this binary was built against the libhybris host stub (desktop):
    // every NDK call would be a no-op, so callers should skip them.
    static bool isHostStub();

    // Create the manager (if needed), enumerate cameras and read each one's
    // characteristics into cameras().  Returns false + sets lastError() on
    // failure (including host-stub builds).
    bool enumerate();
    const std::vector<CameraInfo>& cameras() const { return cameras_; }

    // Read full characteristics for a single camera id.
    bool readInfo(const std::string& id, CameraInfo* out);

    // Open / close a camera device — proves the device can actually be opened.
    bool open(const std::string& id);
    void close();
    bool isOpen() const { return device_ != nullptr; }
    const std::string& openId() const { return openId_; }

    // ── Preview streaming (Milestone 3a) ─────────────────────────────────────
    // Create an AImageReader of the given size/format, wire it as the sole
    // output of a capture session on the open device, and start a repeating
    // TEMPLATE_PREVIEW request so frames flow.  The image listener drains and
    // counts every delivered frame.  Requires open() first.  Returns false +
    // sets lastError() on any step.
    // usage: 0 -> AImageReader_new (format-default usage); non-zero ->
    // AImageReader_newWithUsage with the given AHARDWAREBUFFER_USAGE_* flags
    // (e.g. GPU_SAMPLED_IMAGE for the implementation-defined PRIVATE format the
    // camera HAL prefers).
    bool startPreview(int width, int height, int format = AIMAGE_FORMAT_YUV_420_888,
                      uint64_t usage = 0, int targetFps = 30, bool withStill = true);
    void stopPreview();
    bool isStreaming() const { return streaming_; }

    // ── Still capture (Milestone 4) ──────────────────────────────────────────
    // One-shot JPEG to `path`.  Requires startPreview(withStill=true), which
    // adds a full-resolution JPEG output to the capture session.  The capture is
    // asynchronous: when the JPEG arrives it is written to disk and the photo
    // callback (if set) fires.  Returns false if the request could not be
    // submitted.
    bool capturePhoto(const std::string& path, int deviceRotation = 0);

    // Burst capture: submit N still-capture requests in a single HAL call,
    // each writing to the corresponding path.  Much more efficient than N
    // sequential captures — the HAL pipelines them as one batch.  The photo
    // callback fires once per frame (in order).  HDR burst uses this.
    // When evBrackets is provided (same size as paths), each request gets a
    // different AE exposure compensation step (e.g. {-4, 0, +4} for ±2 EV).
    // ponytail: ACameraCaptureSession_capture(numRequests=N) IS the NDK burst.
    bool captureBurst(const std::vector<std::string>& paths, int deviceRotation = 0,
                      const std::vector<int>& evBrackets = {});
    void setPhotoCallback(std::function<void(const std::string& path, bool ok)> cb)
    {
        photoCallback_ = std::move(cb);
    }

    // Current device rotation in degrees clockwise from natural portrait
    // (0/90/180/270), for capture tagging only — does NOT rotate the preview.
    void setDeviceRotation(int degrees) { deviceRotation_.store(((degrees % 360) + 360) % 360); }

    // Receive luma frames from the analysis YUV stream (for QR scanning).  Called
    // on a camera thread; decode synchronously and marshal results yourself.  The
    // analysis stream exists only while previewing (photo mode), not recording.
    void setAnalysisCallback(std::function<void(const uint8_t* y, int width, int height, int rowStride)> cb)
    {
        analysisCallback_ = std::move(cb);
    }

    // Available JPEG capture sizes for the open camera (output configs).
    std::vector<StreamConfig> jpegSizes() const;
    // Request a specific JPEG capture size; (0,0) = the sensor's max.  Applied on
    // the next startPreview (the caller restarts the camera to take effect).
    void setJpegSize(int width, int height) { reqJpegW_ = width; reqJpegH_ = height; }
    void jpegSize(int* width, int* height) const { *width = jpegW_; *height = jpegH_; }
    // Still JPEG quality [1,100] (default 95).  Set from the app's quality setting
    // so the HAL encodes at the chosen quality directly (no on-disk re-encode).
    void setJpegQuality(int q) { jpegQuality_ = q < 1 ? 1 : (q > 100 ? 100 : q); }
    int  jpegQuality() const  { return jpegQuality_; }

    // ── RAW/DNG capture ─────────────────────────────────────────────────
    // When enabled, each shot also saves a color-accurate .dng (the RAW16
    // stream replaces the live-QR stream — 3-stream HAL limit).  Toggling
    // rebuilds the preview session.  No-op if the camera lacks RAW capability.
    void setRawEnabled(bool on);
    bool isRawEnabled() const { return rawEnabled_; }
    bool rawSupported() const;

    // ── Video recording (Milestone 5) ────────────────────────────────────────
    // Record hardware H.264 to an MP4 at `path`.  Reconfigures the camera into a
    // dedicated recording session whose output is the H.264 encoder's input
    // surface — camera frames flow straight into the encoder (no CPU, no GL).
    // Stops any running preview first.  Requires an open camera.
    // With withAudio (default), also captures the microphone and muxes an AAC
    // track into the MP4 (Milestone 6); if audio fails to start, recording
    // continues video-only.
    bool startRecording(const std::string& path, int width = 1920, int height = 1080,
                        int fps = 30, int bitrate = 20000000, bool withAudio = true,
                        int deviceRotation = 0);
    void stopRecording();
    bool isRecording() const { return recording_; }

    // ── Pre-warm the mic (Milestone 6) ───────────────────────────────────────
    // Warm up the audio pipeline ahead of recording (e.g. on entering video
    // mode) so startRecording just attaches the already-running stream instead
    // of spinning up the mic + encoder.  Frames are captured and discarded until
    // a recording attaches.  prepareRecording() returns false if the mic can't
    // start (recording then proceeds video-only).  releaseRecording() tears the
    // warm pipeline down (e.g. on leaving video mode); it is ignored mid-record.
    bool prepareRecording();
    void releaseRecording();
    bool isAudioReady() const;   // mic warm AND first sample's format captured

    // ── Simultaneous preview + record (pre-configured) ───────────────────────
    // Reconfigure the live preview session to ALSO carry the H.264 encoder's
    // input surface as an output, with the encoder opened and idle.  The
    // repeating request stays preview-only (no frames reach the encoder → no
    // encoding, no power), so all the slow capture-session setup is paid here,
    // up front (e.g. on entering video mode).  startRecording() then just adds
    // the encoder to the repeating request — preview never stops and there is no
    // spin-up.  Requires a running preview.  exitVideoMode() drops the encoder
    // and restores the preview-only session.
    bool enterVideoMode(int width = 1920, int height = 1080,
                        int fps = 30, int bitrate = 20000000);
    void exitVideoMode();
    bool isVideoMode() const { return videoMode_; }

    // ── Manual controls (Milestone 7) ────────────────────────────────────────
    // Update control state and re-submit the active repeating request (preview
    // or recording).  Safe to call before streaming — the state is applied when
    // the next session begins.  Enum args use ACAMERA_CONTROL_* values.
    void  setAutoExposure();                               // AE back to auto
    void  setManualExposure(int iso, int64_t exposureNs);  // AE off + fixed values
    void  setFocusDistance(float diopters);                // 0=infinity, >0=closer
    void  setExposureCompensation(int steps);              // AE comp index (× step EV)
    void  setAeLock(bool lock);
    void  setAwbLock(bool lock);
    void  setAwbMode(int awbMode);                         // ACAMERA_CONTROL_AWB_MODE_*
    void  setAfMode(int afMode);                           // ACAMERA_CONTROL_AF_MODE_*
    void  setZoomRatio(float ratio);                       // 1.0 .. maxZoomRatio()
    void  setTorch(bool on);
    void  setFlashMode(int mode);                          // 0=off, 1=on, 2=auto (per-shot)
    void  triggerPrecapture();                             // kick AE precapture (auto-flash metering)
    int   aeState() const { return lastAeState_.load(); }  // ACAMERA_CONTROL_AE_STATE_* (result)
    void  setFocusPoint(float x, float y);                 // normalized [0,1]; triggers AF
    float minZoomRatio() const { return openZoomMin_; }    // from open camera characteristics
    float maxZoomRatio() const { return openZoomMax_; }    // from open camera characteristics
    // AE-compensation index range of the open camera.
    int   evCompMin()    const { return openEvMin_; }
    int   evCompMax()    const { return openEvMax_; }
    float evCompStep()   const { return openEvStep_; }
    // Electronic video stabilization (EIS). Default on; applied to recording
    // request + idle video preview when the camera supports it.
    bool  videoStabSupported() const { return openVideoStab_; }
    void  setVideoStabilization(bool on) { videoStabEnabled_ = on; }

    // Look up the CameraInfo for the currently open camera (DNG metadata).
    const CameraInfo* activeInfo() const;            // this device reports 4x
    // Drain the reader from the *calling* thread (cross-check for whether frames
    // are arriving independently of the image-available callback).  Returns the
    // number of frames drained this call; also updates frameCount()/last*.
    int pollFrames();

    // The preview AImageReader, for a renderer that pulls frames itself
    // (acquireLatestImage on the render thread).  Valid only while streaming.
    AImageReader* previewReader() const { return reader_; }

    // Install a per-frame callback (called on the image-listener thread).  When
    // set, the listener does NOT drain/count frames — the callee is expected to
    // pull them (render-pull mode, e.g. Camera2Bridge schedules a GL repaint).
    // When unset (the probe), the listener drains and counts (frame stats).
    void setFrameCallback(std::function<void()> cb) { frameCallback_ = std::move(cb); }

    // Frame statistics, updated from the image-listener (background) thread.
    int     frameCount()      const { return frameCount_.load(std::memory_order_relaxed); }
    int     lastFrameWidth()  const { return lastFrameW_.load(std::memory_order_relaxed); }
    int     lastFrameHeight() const { return lastFrameH_.load(std::memory_order_relaxed); }
    int64_t lastTimestampNs() const { return lastTimestampNs_.load(std::memory_order_relaxed); }

    // Diagnostics: how many times the image-available callback fired, and the
    // last media_status_t returned by acquireNextImage (helps tell "listener
    // never called" apart from "acquire failed").
    int callbackCount()     const { return callbackCount_.load(std::memory_order_relaxed); }
    int lastAcquireStatus() const { return lastAcquireStatus_.load(std::memory_order_relaxed); }

    // Logging helpers.
    void dumpSummary(const CameraInfo& info) const;   // curated, decoded report
    void dumpAllTags(const std::string& id) const;    // generic getAllTags walk

    const std::string& lastError() const { return lastError_; }

    // Pretty-printers for the decoded enums (also handy for the Bridge UI).
    static const char* facingName(int facing);
    static const char* hardwareLevelName(int level);
    static const char* capabilityName(int cap);
    static const char* formatName(int format);

private:
    bool ensureManager();
    void log(const std::string& line) const;
    void closeSessionLocked();    // stop + close the capture session (keeps readers)
    void freeStreamResources();   // free readers/windows/outputs/request
    void freeSessionKeepReaders();          // free session/outputs/requests, keep readers
    bool buildSessionFromReaders(bool withEncoder, int targetFps);  // (re)build the session
    bool maxJpegSize(int* w, int* h) const;  // largest JPEG output of the open camera
    bool ensureRawReader();                  // create the RAW16 reader/window (lazily)
    void applyControls(ACaptureRequest* req) const;  // write control state into a request
    bool applyControlsToActive();                    // re-submit the active repeating request
    void applyVideoStabilization(ACaptureRequest* req) const;  // EIS on/off for a record request

    // Background writer thread for serializing JPEG/DNG to disk (fast shot-to-shot).
    void enqueueWrite(std::function<void()> job);
    void stopWriter();

    // ACameraDevice_StateCallbacks targets (context is the CameraSession*).
    static void onDeviceDisconnected(void* ctx, ACameraDevice* device);
    static void onDeviceError(void* ctx, ACameraDevice* device, int error);

    // Streaming callbacks (context is the CameraSession*).
    static void onImageAvailable(void* ctx, AImageReader* reader);
    static void onJpegImageAvailable(void* ctx, AImageReader* reader);
    static void onRawImageAvailable(void* ctx, AImageReader* reader);
    static void onAnalysisImageAvailable(void* ctx, AImageReader* reader);
    static void onSessionActive(void* ctx, ACameraCaptureSession* session);
    static void onSessionReady(void* ctx, ACameraCaptureSession* session);
    static void onSessionClosed(void* ctx, ACameraCaptureSession* session);
    static void onCaptureResult(void* ctx, ACameraCaptureSession* session,
                                ACaptureRequest* request, const ACameraMetadata* result);

    LogFn                        logFn_;
    std::function<void()>        frameCallback_;
    ACameraManager*              manager_ = nullptr;
    ACameraDevice*               device_  = nullptr;
    std::string                  openId_;
    std::vector<CameraInfo>      cameras_;
    mutable std::string          lastError_;
    ACameraDevice_StateCallbacks deviceCb_{};

    // Preview streaming objects (owned; torn down in stopPreview()).
    AImageReader*                        reader_          = nullptr;
    ANativeWindow*                       readerWindow_    = nullptr;
    ACaptureSessionOutput*               sessionOutput_   = nullptr;
    ACaptureSessionOutputContainer*      outputContainer_ = nullptr;
    ACameraCaptureSession*               captureSession_  = nullptr;
    ACaptureRequest*                     previewRequest_  = nullptr;
    ACameraOutputTarget*                 outputTarget_    = nullptr;
    AImageReader_ImageListener           readerListener_{};
    ACameraCaptureSession_stateCallbacks sessionCb_{};
    ACameraCaptureSession_captureCallbacks resultCb_{};   // reads AE_STATE off results
    std::atomic<int>                     lastAeState_{0};
    std::mutex                           resultMutex_;
    float                                resultGains_[4] = {1, 1, 1, 1};
    bool                                 haveResultGains_ = false;
    int32_t                              resultIso_        = 0;
    int64_t                              resultExposureNs_ = 0;
    bool                                 streaming_ = false;
    int                                  previewFps_ = 30;

    // Simultaneous preview+record: the same captureSession_ also carries the
    // encoder surface output, with a second (record) request that targets both
    // the preview reader and the encoder.  recordRequest_/recordTarget_/
    // recordOutput_ (declared below) are reused for the combined session.
    bool                                 videoMode_ = false;
    ACameraOutputTarget*                 recordPreviewTarget_ = nullptr;

    // Still-capture (JPEG) output, added to the same capture session.
    AImageReader*              jpegReader_   = nullptr;
    ANativeWindow*             jpegWindow_   = nullptr;
    ACaptureSessionOutput*     jpegOutput_   = nullptr;
    AImageReader_ImageListener jpegListener_{};
    int                        jpegW_        = 0;   // actual JPEG reader size
    int                        jpegH_        = 0;
    int                        jpegQuality_  = 95;       // still JPEG quality [1,100]
    int                        reqJpegW_     = 0;   // requested capture size (0 = max)
    int                        reqJpegH_     = 0;
    int                        openSensorOrientation_ = 0;
    int                        openFacing_   = -1;   // ACAMERA_LENS_FACING_* of the open camera
    std::atomic<int>           deviceRotation_ {0};  // device tilt for capture tagging only

    // JPEG_ORIENTATION / MP4 rotation hint for the open camera at the current
    // device rotation (Android convention: + for back, - for front cameras).
    int captureOrientation() const
    {
        const int sensor = ((openSensorOrientation_ % 360) + 360) % 360;
        const int device = ((deviceRotation_.load() % 360) + 360) % 360;
        const int o = (openFacing_ == ACAMERA_LENS_FACING_FRONT)
                          ? (sensor - device + 360)
                          : (sensor + device);
        return ((o % 360) + 360) % 360;
    }

    std::mutex                 photoMutex_;
    std::deque<std::string>    pendingPhotoPaths_;       // queue for burst — pop front on each callback
    std::function<void(const std::string&, bool)> photoCallback_;

    // RAW16 output for DNG capture — swaps in for the analysis stream when raw is
    // enabled.  rawStillTarget_ is added to the still capture request so both a
    // JPEG and a RAW16 arrive per shot; onRawImageAvailable writes a .dng.
    bool                       rawEnabled_     = false;
    AImageReader*              rawReader_      = nullptr;
    ANativeWindow*             rawWindow_      = nullptr;
    ACaptureSessionOutput*     rawOutput_      = nullptr;
    ACameraOutputTarget*       rawStillTarget_ = nullptr;
    AImageReader_ImageListener rawListener_{};
    int                        rawW_           = 0;
    int                        rawH_           = 0;
    std::deque<std::string>    pendingRawPaths_;           // queue — pop front on each raw callback

    // Analysis YUV output (CPU-readable luma) for QR/barcode scanning — added to
    // the preview-only session (not video mode).  The listener hands the Y plane
    // to analysisCallback_ (set by the bridge), which decodes off the GUI thread.
    AImageReader*              analysisReader_   = nullptr;
    ANativeWindow*             analysisWindow_   = nullptr;
    ACaptureSessionOutput*     analysisOutput_   = nullptr;
    ACameraOutputTarget*       analysisTarget_   = nullptr;
    AImageReader_ImageListener analysisListener_{};
    std::function<void(const uint8_t* y, int width, int height, int rowStride)> analysisCallback_;

    // Video recording (dedicated capture session targeting the encoder surface).
    std::unique_ptr<VideoEncoder>        encoder_;
    std::unique_ptr<AudioEncoder>        audioEnc_;   // optional AAC audio track (M6)
    bool                                 audioPrewarmed_ = false;  // warm-kept across records
    ACameraCaptureSession*               recordSession_   = nullptr;
    ACaptureSessionOutputContainer*      recordContainer_ = nullptr;
    ACaptureSessionOutput*               recordOutput_    = nullptr;
    ACaptureRequest*                     recordRequest_   = nullptr;
    ACameraOutputTarget*                 recordTarget_    = nullptr;
    ACameraCaptureSession_stateCallbacks recordCb_{};
    bool                                 recording_ = false;

    // Manual-control state, applied to whichever repeating request is active.
    ACameraCaptureSession* activeSession_ = nullptr;
    ACaptureRequest*       activeRequest_ = nullptr;
    int     ctlAeMode_     = ACAMERA_CONTROL_AE_MODE_ON;
    int64_t ctlExposureNs_ = 10000000;   // 10 ms (used only when AE off)
    int     ctlIso_        = 400;
    int     ctlEvComp_     = 0;
    int     ctlAeLock_     = 0;
    int     ctlAwbLock_    = 0;
    int     ctlAwbMode_    = ACAMERA_CONTROL_AWB_MODE_AUTO;
    int     ctlAfMode_     = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    float   ctlZoom_       = 1.0f;
    float   ctlFocusDistance_ = 0.0f;   // 0 = infinity (lens focus at furthest); diopters
    int     ctlTorch_      = 0;
    int     flashMode_     = 0;   // per-shot flash: 0=off, 1=on, 2=auto
    int     openActiveArray_[4] = {0, 0, 0, 0};
    float   openZoomMin_       = 1.0f;          // CONTROL_ZOOM_RATIO_RANGE min (fallback 1x)
    float   openZoomMax_  = 4.0f; // CONTROL_ZOOM_RATIO_RANGE max (fallback 4x)
    int     openEvMin_    = -4;   // AE comp range (fallback ±4 @ 0.5 EV/step)
    int     openEvMax_    = 4;
    float   openEvStep_   = 0.5f;
    bool    openVideoStab_ = false;   // EIS supported by the open camera
    bool    videoStabEnabled_ = true; // user toggle (default on)

    std::atomic<int>     frameCount_        {0};
    std::atomic<int>     lastFrameW_        {0};
    std::atomic<int>     lastFrameH_        {0};
    std::atomic<int64_t> lastTimestampNs_   {0};
    std::atomic<int>     callbackCount_     {0};
    std::atomic<int>     lastAcquireStatus_ {0};

    // Background writer thread — lazily started on first enqueueWrite.
    std::thread                       writerThread_;
    std::mutex                        writerMutex_;
    std::condition_variable           writerCv_;
    std::deque<std::function<void()>> writerJobs_;
    bool                              writerStop_    = false;
    bool                              writerStarted_ = false;

    // Wait for async session close before opening a new session (HAL-specific;
    // some implementations need the onClosed callback before re-open).
    std::mutex              sessionCloseMutex_;
    std::condition_variable sessionCloseCv_;
    bool                    sessionCloseDone_ = false;
};

} // namespace furicam

#endif // FURICAM_CAMERA_SESSION_H
