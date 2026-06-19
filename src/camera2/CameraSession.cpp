// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// CameraSession implementation — Milestone 2: enumerate cameras, read
// characteristics, open a camera, dump capabilities.  See CameraSession.h.

#include "CameraSession.h"
#include "VideoEncoder.h"
#include "AudioEncoder.h"   // gst-free header; a stub backs it when audio is off

#include <cstdarg>
#include <cstdio>
#include <thread>
#include <utility>

// android_dlopen/dlsym come from libhybris-common on the phone, or from the
// HostDlStub on a non-Halium host (where they return null and the helper below
// is a no-op).
extern "C" void* android_dlopen(const char* filename, int flag);
extern "C" void* android_dlsym(void* handle, const char* symbol);

namespace furicam2 {

namespace {

// Start an Android binder thread pool for this process.
//
// A bare libhybris process can make *outgoing* binder calls (that is how we
// open the camera), but it has no threads servicing *incoming* transactions.
// Camera2 needs the latter: the Android cameraserver calls back into our
// process to dequeue buffers from the AImageReader's BufferQueue, and to
// disconnect streams on close.  Without a pool those callbacks fail ("Broken
// pipe", zero frames) and ACameraDevice_close deadlocks waiting on nested
// callbacks.  A normal Android app gets this pool from its runtime; we start it
// ourselves with ProcessState::self()->startThreadPool(), which also sets
// mThreadPoolStarted so the kernel can spawn extra threads (BR_SPAWN_LOOPER) for
// re-entrant transactions — a single hand-rolled joinThreadPool thread is not
// enough and deadlocks on close.
//
// ProcessState::self() returns sp<ProcessState> by value.  Being non-trivially
// copyable, the AArch64 ABI returns it indirectly (caller-allocated buffer,
// address in x8).  We model that with an oversized POD struct return so the C
// call matches, and read the first pointer (the ProcessState*).  The returned
// sp's destructor is skipped, leaking one ref on a process-singleton — harmless.
void startBinderThreadPoolOnce()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true))
        return;

    void* libbinder = android_dlopen("libbinder.so", /*RTLD_LAZY*/ 1);
    if (!libbinder)
        return;   // non-Halium host (stub) or libbinder unavailable

    struct SpHolder { void* p[4]; };   // >16 bytes -> indirect (x8) return, matches sp<>
    using PsSelfFn  = SpHolder (*)(void);
    using PsStartFn = void     (*)(void* thiz);
    auto psSelf = reinterpret_cast<PsSelfFn>(
        android_dlsym(libbinder, "_ZN7android12ProcessState4selfEv"));
    auto psStart = reinterpret_cast<PsStartFn>(
        android_dlsym(libbinder, "_ZN7android12ProcessState15startThreadPoolEv"));
    if (!psSelf || !psStart)
        return;

    SpHolder h = psSelf();
    if (h.p[0])
        psStart(h.p[0]);
}

// printf-style formatting into a std::string (keeps the call sites readable).
std::string vfmt(const char* f, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, f, ap2);
    va_end(ap2);
    if (n < 0)
        return {};
    std::string s(static_cast<size_t>(n), '\0');
    std::vsnprintf(&s[0], static_cast<size_t>(n) + 1, f, ap);
    return s;
}

std::string fmt(const char* f, ...)
{
    va_list ap;
    va_start(ap, f);
    std::string s = vfmt(f, ap);
    va_end(ap);
    return s;
}

} // namespace

CameraSession::CameraSession(LogFn log)
    : logFn_(std::move(log))
{
    if (!logFn_) {
        logFn_ = [](const std::string& s) {
            std::fputs(s.c_str(), stdout);
            std::fputc('\n', stdout);
        };
    }
}

CameraSession::~CameraSession()
{
    stopRecording();
    close();
    if (manager_) {
        ACameraManager_delete(manager_);
        manager_ = nullptr;
    }
}

bool CameraSession::isHostStub()
{
#ifdef FURICAM2_CAMERA2_HOST_STUB
    return true;
#else
    return false;
#endif
}

void CameraSession::log(const std::string& line) const
{
    logFn_(line);
}

bool CameraSession::ensureManager()
{
    if (isHostStub()) {
        lastError_ = "Camera2 unavailable: built with the libhybris host stub "
                     "(not a Halium device)";
        return false;
    }
    if (!manager_) {
        manager_ = ACameraManager_create();
        if (!manager_) {
            lastError_ = "ACameraManager_create() returned null";
            return false;
        }
    }
    return true;
}

bool CameraSession::enumerate()
{
    cameras_.clear();
    if (!ensureManager())
        return false;

    ACameraIdList* ids = nullptr;
    camera_status_t st = ACameraManager_getCameraIdList(manager_, &ids);
    if (st != ACAMERA_OK || !ids) {
        lastError_ = fmt("ACameraManager_getCameraIdList failed (status %d)", (int)st);
        return false;
    }

    for (int i = 0; i < ids->numCameras; ++i) {
        CameraInfo info;
        if (readInfo(ids->cameraIds[i], &info))
            cameras_.push_back(std::move(info));
    }
    ACameraManager_deleteCameraIdList(ids);
    return true;
}

bool CameraSession::readInfo(const std::string& id, CameraInfo* out)
{
    if (!ensureManager())
        return false;

    ACameraMetadata* meta = nullptr;
    camera_status_t st = ACameraManager_getCameraCharacteristics(manager_, id.c_str(), &meta);
    if (st != ACAMERA_OK || !meta) {
        lastError_ = fmt("getCameraCharacteristics(%s) failed (status %d)", id.c_str(), (int)st);
        return false;
    }

    CameraInfo info;
    info.id = id;
    ACameraMetadata_const_entry e{};

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count >= 1)
        info.facing = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count >= 1)
        info.sensorOrientation = e.data.i32[0];
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &e) == ACAMERA_OK && e.count >= 1)
        info.hardwareLevel = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 2) {
        info.pixelArrayW = e.data.i32[0];
        info.pixelArrayH = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4)
        for (int k = 0; k < 4; ++k)
            info.activeArray[k] = e.data.i32[k];
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        info.isoMin = e.data.i32[0];
        info.isoMax = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        info.exposureMinNs = e.data.i64[0];
        info.exposureMaxNs = e.data.i64[1];
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
        for (uint32_t k = 0; k < e.count; ++k) {
            int cap = e.data.u8[k];
            info.capabilities.push_back(cap);
            if (cap == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)
                info.manualSensor = true;
        }
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
        // int32[n*4]: (format, width, height, direction).
        for (uint32_t k = 0; k + 3 < e.count; k += 4) {
            StreamConfig sc;
            sc.format  = e.data.i32[k];
            sc.width   = e.data.i32[k + 1];
            sc.height  = e.data.i32[k + 2];
            sc.isInput = (e.data.i32[k + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT);
            if (!sc.isInput)
                info.outputs.push_back(sc);
        }
    }

    // Capability ranges used to drive zoom / exposure-comp / EIS without
    // hardcoding device-specific constants (read per camera).
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_ZOOM_RATIO_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        info.zoomRatioMin = e.data.f[0];
        info.zoomRatioMax = e.data.f[1];
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &e) == ACAMERA_OK && e.count >= 2) {
        info.evCompMin = e.data.i32[0];
        info.evCompMax = e.data.i32[1];
    }
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &e) == ACAMERA_OK
        && e.count >= 1 && e.data.r[0].denominator != 0)
        info.evCompStep = (float)e.data.r[0].numerator / (float)e.data.r[0].denominator;
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, &e) == ACAMERA_OK)
        for (uint32_t k = 0; k < e.count; ++k)
            if (e.data.u8[k] == ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON)
                info.videoStabSupported = true;

    ACameraMetadata_free(meta);
    *out = std::move(info);
    return true;
}

bool CameraSession::open(const std::string& id)
{
    if (!ensureManager())
        return false;
    close();

    deviceCb_ = ACameraDevice_StateCallbacks{};
    deviceCb_.context        = this;
    deviceCb_.onDisconnected = &CameraSession::onDeviceDisconnected;
    deviceCb_.onError        = &CameraSession::onDeviceError;

    camera_status_t st = ACameraManager_openCamera(manager_, id.c_str(), &deviceCb_, &device_);
    if (st != ACAMERA_OK || !device_) {
        lastError_ = fmt("ACameraManager_openCamera(%s) failed (status %d)", id.c_str(), (int)st);
        device_ = nullptr;
        return false;
    }
    openId_ = id;
    openSensorOrientation_ = 0;
    openFacing_ = -1;
    for (const auto& c : cameras_)
        if (c.id == id) {
            openSensorOrientation_ = c.sensorOrientation;
            openFacing_ = c.facing;
            for (int k = 0; k < 4; ++k)
                openActiveArray_[k] = c.activeArray[k];
            // Cache the open camera's capability ranges (fall back to the prior
            // hardcodes when a tag is absent, e.g. on a LIMITED secondary camera).
            openZoomMax_   = (c.zoomRatioMax > 1.0f) ? c.zoomRatioMax : 4.0f;
            openEvMin_     = (c.evCompMax > c.evCompMin) ? c.evCompMin : -4;
            openEvMax_     = (c.evCompMax > c.evCompMin) ? c.evCompMax :  4;
            openEvStep_    = (c.evCompStep > 0.0f) ? c.evCompStep : 0.5f;
            openVideoStab_ = c.videoStabSupported;
            break;
        }
    return true;
}

void CameraSession::close()
{
    // Tear down any recording / video-mode / mic state first.
    recording_ = false;
    if (audioEnc_) {
        audioEnc_->detach();
        audioEnc_->stop();
        audioEnc_.reset();
    }
    audioPrewarmed_ = false;

    // Order matters: close the session and device BEFORE freeing the
    // AImageReader/window they still reference, else cameraserver logs
    // "native window died from under us" and the close can stall.
    closeSessionLocked();

    // Free the combined-session record objects (the preview-only ones are freed
    // by freeStreamResources()).  Done before encoder_->close() releases the
    // input surface these wrap.
    if (recordRequest_)       { ACaptureRequest_free(recordRequest_);            recordRequest_ = nullptr; }
    if (recordPreviewTarget_) { ACameraOutputTarget_free(recordPreviewTarget_);  recordPreviewTarget_ = nullptr; }
    if (recordTarget_)        { ACameraOutputTarget_free(recordTarget_);         recordTarget_ = nullptr; }
    if (recordOutput_)        { ACaptureSessionOutput_free(recordOutput_);       recordOutput_ = nullptr; }
    videoMode_ = false;

    if (device_) {
        ACameraDevice_close(device_);
        device_ = nullptr;
    }
    if (encoder_) {
        encoder_->close();   // releases the input surface (now unreferenced)
        encoder_.reset();
    }
    freeStreamResources();
    openId_.clear();
}

bool CameraSession::startPreview(int width, int height, int format, uint64_t usage, int targetFps,
                                 bool withStill)
{
    if (!ensureManager())
        return false;
    if (!device_) {
        lastError_ = "startPreview: no open camera (call open() first)";
        return false;
    }
    stopPreview();

    // Ensure cameraserver can call back into us to dequeue buffers (see comment
    // on startBinderThreadPoolOnce); without this the BufferQueue never delivers.
    startBinderThreadPoolOnce();

    media_status_t ms;
    if (usage != 0)
        ms = AImageReader_newWithUsage(width, height, format, usage, /*maxImages*/ 4, &reader_);
    else
        ms = AImageReader_new(width, height, format, /*maxImages*/ 4, &reader_);
    if (ms != AMEDIA_OK || !reader_) {
        lastError_ = fmt("AImageReader_new(%dx%d fmt 0x%x usage 0x%llx) failed (status %d)",
                         width, height, format, (unsigned long long)usage, (int)ms);
        return false;
    }

    readerListener_.context         = this;
    readerListener_.onImageAvailable = &CameraSession::onImageAvailable;
    AImageReader_setImageListener(reader_, &readerListener_);

    ms = AImageReader_getWindow(reader_, &readerWindow_);
    if (ms != AMEDIA_OK || !readerWindow_) {
        lastError_ = fmt("AImageReader_getWindow failed (status %d)", (int)ms);
        stopPreview();
        return false;
    }
    ANativeWindow_acquire(readerWindow_);

    camera_status_t cs = ACaptureSessionOutputContainer_create(&outputContainer_);
    if (cs == ACAMERA_OK)
        cs = ACaptureSessionOutput_create(readerWindow_, &sessionOutput_);
    if (cs == ACAMERA_OK)
        cs = ACaptureSessionOutputContainer_add(outputContainer_, sessionOutput_);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("capture-session output setup failed (status %d)", (int)cs);
        stopPreview();
        return false;
    }

    // Optional full-resolution JPEG output for still capture (M4).  It must be
    // part of the session from the start; a one-shot request targets it later.
    if (withStill) {
        // Use the requested JPEG capture size if set, else the sensor's max.
        int jw = reqJpegW_, jh = reqJpegH_;
        if (jw <= 0 || jh <= 0)
            maxJpegSize(&jw, &jh);
        if (jw > 0 && jh > 0
            && AImageReader_new(jw, jh, AIMAGE_FORMAT_JPEG, /*maxImages*/ 2, &jpegReader_) == AMEDIA_OK
            && jpegReader_) {
            jpegW_ = jw;
            jpegH_ = jh;
            jpegListener_.context         = this;
            jpegListener_.onImageAvailable = &CameraSession::onJpegImageAvailable;
            AImageReader_setImageListener(jpegReader_, &jpegListener_);
            if (AImageReader_getWindow(jpegReader_, &jpegWindow_) == AMEDIA_OK && jpegWindow_) {
                ANativeWindow_acquire(jpegWindow_);
                if (ACaptureSessionOutput_create(jpegWindow_, &jpegOutput_) == ACAMERA_OK)
                    ACaptureSessionOutputContainer_add(outputContainer_, jpegOutput_);
            }
            log(fmt("still capture enabled: JPEG %dx%d", jw, jh));
        }
    }

    // Analysis YUV output (CPU-readable luma) for live QR/barcode scanning.
    // Present only while previewing (photo mode); excluded in video mode and when
    // the preview is full-res (the 3-stream combo exceeds HAL limits, and QR off a
    // 20MP preview is pointless).
    if (withStill && width <= 1920) {
        // Match the analysis aspect to the preview so QR overlay coords align with
        // the on-screen preview; cap width at 1280 to bound CPU decode cost.
        int aw = width, ah = height;
        if (aw > 1280) { ah = (int)((long)ah * 1280 / aw); aw = 1280; }
        aw &= ~1; ah &= ~1;
        if (aw <= 0 || ah <= 0) { aw = 1280; ah = 720; }
        if (AImageReader_new(aw, ah, AIMAGE_FORMAT_YUV_420_888, /*maxImages*/ 2, &analysisReader_) == AMEDIA_OK
            && analysisReader_) {
            analysisListener_.context          = this;
            analysisListener_.onImageAvailable = &CameraSession::onAnalysisImageAvailable;
            AImageReader_setImageListener(analysisReader_, &analysisListener_);
            if (AImageReader_getWindow(analysisReader_, &analysisWindow_) == AMEDIA_OK && analysisWindow_) {
                ANativeWindow_acquire(analysisWindow_);
                if (ACaptureSessionOutput_create(analysisWindow_, &analysisOutput_) == ACAMERA_OK)
                    ACaptureSessionOutputContainer_add(outputContainer_, analysisOutput_);
            }
            log(fmt("QR analysis stream: YUV %dx%d", aw, ah));
        }
    }

    sessionCb_          = ACameraCaptureSession_stateCallbacks{};
    sessionCb_.context  = this;
    sessionCb_.onActive = &CameraSession::onSessionActive;
    sessionCb_.onReady  = &CameraSession::onSessionReady;
    sessionCb_.onClosed = &CameraSession::onSessionClosed;
    resultCb_           = ACameraCaptureSession_captureCallbacks{};
    resultCb_.context   = this;
    resultCb_.onCaptureCompleted = &CameraSession::onCaptureResult;
    cs = ACameraDevice_createCaptureSession(device_, outputContainer_, &sessionCb_, &captureSession_);
    if (cs != ACAMERA_OK || !captureSession_) {
        lastError_ = fmt("ACameraDevice_createCaptureSession failed (status %d)", (int)cs);
        stopPreview();
        return false;
    }

    cs = ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &previewRequest_);
    if (cs == ACAMERA_OK)
        cs = ACameraOutputTarget_create(readerWindow_, &outputTarget_);
    if (cs == ACAMERA_OK)
        cs = ACaptureRequest_addTarget(previewRequest_, outputTarget_);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("preview request setup failed (status %d)", (int)cs);
        stopPreview();
        return false;
    }
    // Also feed the analysis (QR) stream from the repeating preview request.
    if (analysisWindow_
        && ACameraOutputTarget_create(analysisWindow_, &analysisTarget_) == ACAMERA_OK)
        ACaptureRequest_addTarget(previewRequest_, analysisTarget_);

    // Let the preview AE drop the frame rate in low light (range [previewFpsMin_,
    // targetFps], e.g. 5-30) so the viewfinder brightens toward the photo — at the
    // cost of a choppier preview when it's dark.
    previewFps_ = targetFps > 0 ? targetFps : previewFps_;
    if (targetFps > 0) {
        int32_t fpsRange[2] = { previewFpsMin_ > 0 ? previewFpsMin_ : targetFps, targetFps };
        ACaptureRequest_setEntry_i32(previewRequest_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);
    }

    activeSession_ = captureSession_;
    activeRequest_ = previewRequest_;
    applyControls(previewRequest_);

    cs = ACameraCaptureSession_setRepeatingRequest(captureSession_, &resultCb_, 1, &previewRequest_, nullptr);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("ACameraCaptureSession_setRepeatingRequest failed (status %d)", (int)cs);
        stopPreview();
        return false;
    }

    frameCount_.store(0, std::memory_order_relaxed);
    streaming_ = true;
    log(fmt("preview started: %dx%d fmt 0x%x", width, height, format));
    return true;
}

void CameraSession::closeSessionLocked()
{
    // Clear the listeners first so no further frame callbacks fire during teardown.
    if (reader_)
        AImageReader_setImageListener(reader_, nullptr);
    if (jpegReader_)
        AImageReader_setImageListener(jpegReader_, nullptr);
    if (captureSession_) {
        ACameraCaptureSession_stopRepeating(captureSession_);
        ACameraCaptureSession_close(captureSession_);
        captureSession_ = nullptr;
    }
}

void CameraSession::freeStreamResources()
{
    if (previewRequest_) {
        ACaptureRequest_free(previewRequest_);
        previewRequest_ = nullptr;
    }
    if (outputTarget_) {
        ACameraOutputTarget_free(outputTarget_);
        outputTarget_ = nullptr;
    }
    if (outputContainer_) {
        ACaptureSessionOutputContainer_free(outputContainer_);
        outputContainer_ = nullptr;
    }
    if (sessionOutput_) {
        ACaptureSessionOutput_free(sessionOutput_);
        sessionOutput_ = nullptr;
    }
    if (readerWindow_) {
        ANativeWindow_release(readerWindow_);
        readerWindow_ = nullptr;
    }
    if (reader_) {
        AImageReader_delete(reader_);
        reader_ = nullptr;
    }
    // Cached still request + its JPEG target (they reference jpegWindow_; rebuilt
    // lazily on the next capture).  Free before releasing the window.
    if (stillTarget_) {
        ACameraOutputTarget_free(stillTarget_);
        stillTarget_ = nullptr;
    }
    if (stillRequest_) {
        ACaptureRequest_free(stillRequest_);
        stillRequest_ = nullptr;
    }
    if (jpegOutput_) {
        ACaptureSessionOutput_free(jpegOutput_);
        jpegOutput_ = nullptr;
    }
    if (jpegWindow_) {
        ANativeWindow_release(jpegWindow_);
        jpegWindow_ = nullptr;
    }
    if (jpegReader_) {
        AImageReader_delete(jpegReader_);
        jpegReader_ = nullptr;
    }
    if (analysisTarget_) {
        ACameraOutputTarget_free(analysisTarget_);
        analysisTarget_ = nullptr;
    }
    if (analysisOutput_) {
        ACaptureSessionOutput_free(analysisOutput_);
        analysisOutput_ = nullptr;
    }
    if (analysisWindow_) {
        ANativeWindow_release(analysisWindow_);
        analysisWindow_ = nullptr;
    }
    if (analysisReader_) {
        AImageReader_setImageListener(analysisReader_, nullptr);
        AImageReader_delete(analysisReader_);
        analysisReader_ = nullptr;
    }
    // Recording sets its own active request *after* startRecording()'s
    // stopPreview() call, so clearing here can never drop a live record request.
    activeSession_ = nullptr;
    activeRequest_ = nullptr;
    streaming_ = false;
}

void CameraSession::stopPreview()
{
    closeSessionLocked();
    freeStreamResources();
}

void CameraSession::freeSessionKeepReaders()
{
    // Tear down the session + its outputs/requests/targets but KEEP the readers
    // (reader_/readerWindow_, jpegReader_/jpegWindow_) so streaming can resume.
    if (captureSession_) {
        ACameraCaptureSession_stopRepeating(captureSession_);
        ACameraCaptureSession_close(captureSession_);
        captureSession_ = nullptr;
    }
    if (previewRequest_)      { ACaptureRequest_free(previewRequest_);            previewRequest_ = nullptr; }
    if (recordRequest_)       { ACaptureRequest_free(recordRequest_);             recordRequest_ = nullptr; }
    if (outputTarget_)        { ACameraOutputTarget_free(outputTarget_);          outputTarget_ = nullptr; }
    if (recordPreviewTarget_) { ACameraOutputTarget_free(recordPreviewTarget_);   recordPreviewTarget_ = nullptr; }
    if (recordTarget_)        { ACameraOutputTarget_free(recordTarget_);          recordTarget_ = nullptr; }
    if (analysisTarget_)      { ACameraOutputTarget_free(analysisTarget_);        analysisTarget_ = nullptr; }
    if (sessionOutput_)       { ACaptureSessionOutput_free(sessionOutput_);       sessionOutput_ = nullptr; }
    if (jpegOutput_)          { ACaptureSessionOutput_free(jpegOutput_);          jpegOutput_ = nullptr; }
    if (analysisOutput_)      { ACaptureSessionOutput_free(analysisOutput_);      analysisOutput_ = nullptr; }
    if (recordOutput_)        { ACaptureSessionOutput_free(recordOutput_);        recordOutput_ = nullptr; }
    if (outputContainer_)     { ACaptureSessionOutputContainer_free(outputContainer_); outputContainer_ = nullptr; }
    activeSession_ = nullptr;
    activeRequest_ = nullptr;
}

bool CameraSession::buildSessionFromReaders(bool withEncoder, int targetFps)
{
    if (!device_ || !reader_ || !readerWindow_) {
        lastError_ = "buildSessionFromReaders: no preview reader";
        return false;
    }
    ANativeWindow* ew = nullptr;
    if (withEncoder) {
        if (!encoder_ || !encoder_->inputWindow()) {
            lastError_ = "buildSessionFromReaders: encoder not open";
            return false;
        }
        ew = encoder_->inputWindow();
    }

    freeSessionKeepReaders();

    // Output container: preview reader + optional JPEG + optional encoder surface.
    camera_status_t cs = ACaptureSessionOutputContainer_create(&outputContainer_);
    if (cs == ACAMERA_OK) cs = ACaptureSessionOutput_create(readerWindow_, &sessionOutput_);
    if (cs == ACAMERA_OK) cs = ACaptureSessionOutputContainer_add(outputContainer_, sessionOutput_);
    if (cs == ACAMERA_OK && jpegWindow_) {
        cs = ACaptureSessionOutput_create(jpegWindow_, &jpegOutput_);
        if (cs == ACAMERA_OK) cs = ACaptureSessionOutputContainer_add(outputContainer_, jpegOutput_);
    }
    // QR analysis stream only in preview mode (not while recording video).
    if (cs == ACAMERA_OK && !withEncoder && analysisWindow_) {
        cs = ACaptureSessionOutput_create(analysisWindow_, &analysisOutput_);
        if (cs == ACAMERA_OK) cs = ACaptureSessionOutputContainer_add(outputContainer_, analysisOutput_);
    }
    if (cs == ACAMERA_OK && withEncoder) {
        cs = ACaptureSessionOutput_create(ew, &recordOutput_);
        if (cs == ACAMERA_OK) cs = ACaptureSessionOutputContainer_add(outputContainer_, recordOutput_);
    }
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("session output setup failed (status %d)", (int)cs);
        return false;
    }

    sessionCb_          = ACameraCaptureSession_stateCallbacks{};
    sessionCb_.context  = this;
    sessionCb_.onActive = &CameraSession::onSessionActive;
    sessionCb_.onReady  = &CameraSession::onSessionReady;
    sessionCb_.onClosed = &CameraSession::onSessionClosed;
    resultCb_           = ACameraCaptureSession_captureCallbacks{};
    resultCb_.context   = this;
    resultCb_.onCaptureCompleted = &CameraSession::onCaptureResult;
    cs = ACameraDevice_createCaptureSession(device_, outputContainer_, &sessionCb_, &captureSession_);
    if (cs != ACAMERA_OK || !captureSession_) {
        lastError_ = fmt("createCaptureSession failed (status %d)", (int)cs);
        return false;
    }

    // Preview-only request (targets the preview reader + the QR analysis stream).
    cs = ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &previewRequest_);
    if (cs == ACAMERA_OK) cs = ACameraOutputTarget_create(readerWindow_, &outputTarget_);
    if (cs == ACAMERA_OK) cs = ACaptureRequest_addTarget(previewRequest_, outputTarget_);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("preview request setup failed (status %d)", (int)cs);
        return false;
    }
    if (!withEncoder && analysisWindow_
        && ACameraOutputTarget_create(analysisWindow_, &analysisTarget_) == ACAMERA_OK)
        ACaptureRequest_addTarget(previewRequest_, analysisTarget_);
    {
        // Video mode honours the chosen target-fps range (e.g. 5–30 for low-light
        // auto); photo-mode preview stays pinned at targetFps for smooth motion.
        const int loFps = withEncoder ? videoFpsMin_ : (previewFpsMin_ > 0 ? previewFpsMin_ : targetFps);
        const int hiFps = withEncoder ? videoFpsMax_ : targetFps;
        if (loFps > 0 && hiFps > 0) {
            int32_t r[2] = { loFps, hiFps };
            ACaptureRequest_setEntry_i32(previewRequest_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, r);
        }
    }
    applyControls(previewRequest_);
    // In video mode, stabilize the idle (not-yet-recording) preview too, so its
    // framing matches the EIS-cropped recording (WYSIWYG).  Photo mode keeps the
    // full sensor FOV (no EIS).  During recording the preview rides recordRequest_,
    // which already carries EIS.
    if (withEncoder)
        applyVideoStabilization(previewRequest_);

    // Record request (targets the preview reader AND the encoder surface) so the
    // preview keeps updating while the encoder records.
    if (withEncoder) {
        cs = ACameraDevice_createCaptureRequest(device_, TEMPLATE_RECORD, &recordRequest_);
        if (cs == ACAMERA_OK) cs = ACameraOutputTarget_create(readerWindow_, &recordPreviewTarget_);
        if (cs == ACAMERA_OK) cs = ACaptureRequest_addTarget(recordRequest_, recordPreviewTarget_);
        if (cs == ACAMERA_OK) cs = ACameraOutputTarget_create(ew, &recordTarget_);
        if (cs == ACAMERA_OK) cs = ACaptureRequest_addTarget(recordRequest_, recordTarget_);
        if (cs != ACAMERA_OK) {
            lastError_ = fmt("record request setup failed (status %d)", (int)cs);
            return false;
        }
        if (videoFpsMin_ > 0 && videoFpsMax_ > 0) {
            int32_t r[2] = { videoFpsMin_, videoFpsMax_ };
            ACaptureRequest_setEntry_i32(recordRequest_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, r);
        }
        applyControls(recordRequest_);
        applyVideoStabilization(recordRequest_);   // EIS while recording (if supported)
    }

    cs = ACameraCaptureSession_setRepeatingRequest(captureSession_, &resultCb_, 1, &previewRequest_, nullptr);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("setRepeatingRequest(preview) failed (status %d)", (int)cs);
        return false;
    }
    activeSession_ = captureSession_;
    activeRequest_ = previewRequest_;
    streaming_ = true;
    return true;
}

bool CameraSession::enterVideoMode(int width, int height, int fps, int bitrate)
{
    if (videoMode_)
        return true;
    if (!device_) {
        lastError_ = "enterVideoMode: no open camera";
        return false;
    }
    if (!streaming_ || !reader_) {
        lastError_ = "enterVideoMode: preview not running";
        return false;
    }

    startBinderThreadPoolOnce();

    encoder_ = std::make_unique<VideoEncoder>();
    if (!encoder_->open(width, height, fps, bitrate, captureOrientation())) {
        lastError_ = "enterVideoMode: encoder open failed: " + encoder_->lastError();
        encoder_.reset();
        return false;
    }

    const int sessFps = fps > 0 ? fps : previewFps_;
    if (!buildSessionFromReaders(/*withEncoder*/ true, sessFps)) {
        // Roll back to a preview-only session so the camera keeps previewing.
        encoder_->close();
        encoder_.reset();
        buildSessionFromReaders(/*withEncoder*/ false, previewFps_);
        return false;
    }
    videoMode_ = true;
    log(fmt("video mode ready: preview+encoder session (%dx%d@%d)", width, height, sessFps));
    return true;
}

void CameraSession::exitVideoMode()
{
    if (!videoMode_)
        return;
    if (recording_)
        stopRecording();
    videoMode_ = false;
    // Rebuild a preview-only session FIRST (drops the encoder surface output),
    // THEN close the encoder (releases the surface it owned).
    buildSessionFromReaders(/*withEncoder*/ false, previewFps_);
    if (encoder_) {
        encoder_->close();
        encoder_.reset();
    }
    log("video mode exited: preview-only session, encoder closed");
}

int CameraSession::pollFrames()
{
    if (!reader_)
        return 0;
    int n = 0;
    AImage* img = nullptr;
    media_status_t st;
    while ((st = AImageReader_acquireNextImage(reader_, &img)) == AMEDIA_OK && img) {
        int64_t ts = 0;
        int32_t w = 0, h = 0;
        AImage_getTimestamp(img, &ts);
        AImage_getWidth(img, &w);
        AImage_getHeight(img, &h);
        frameCount_.fetch_add(1, std::memory_order_relaxed);
        lastFrameW_.store(w, std::memory_order_relaxed);
        lastFrameH_.store(h, std::memory_order_relaxed);
        lastTimestampNs_.store(ts, std::memory_order_relaxed);
        AImage_delete(img);
        img = nullptr;
        ++n;
    }
    lastAcquireStatus_.store((int)st, std::memory_order_relaxed);
    return n;
}

void CameraSession::onAnalysisImageAvailable(void* ctx, AImageReader* reader)
{
    auto* self = static_cast<CameraSession*>(ctx);
    if (!self || !reader)
        return;
    // Drain every frame (keep buffers flowing); hand the luma plane to the
    // callback (e.g. the bridge's QR decoder) when one is set.
    AImage* img = nullptr;
    while (AImageReader_acquireNextImage(reader, &img) == AMEDIA_OK && img) {
        if (self->analysisCallback_) {
            int32_t  w = 0, h = 0, stride = 0;
            uint8_t* y = nullptr;
            int      len = 0;
            AImage_getWidth(img, &w);
            AImage_getHeight(img, &h);
            if (AImage_getPlaneData(img, 0, &y, &len) == AMEDIA_OK && y
                && AImage_getPlaneRowStride(img, 0, &stride) == AMEDIA_OK)
                self->analysisCallback_(y, w, h, stride);
        }
        AImage_delete(img);
        img = nullptr;
    }
}

void CameraSession::onImageAvailable(void* ctx, AImageReader* reader)
{
    auto* self = static_cast<CameraSession*>(ctx);
    if (!self || !reader)
        return;
    self->callbackCount_.fetch_add(1, std::memory_order_relaxed);
    // Render-pull mode: a consumer (the renderer) acquires frames itself, so we
    // must NOT drain here — just notify it that a new frame is ready.
    if (self->frameCallback_) {
        self->frameCallback_();
        return;
    }
    // Otherwise (the probe) drain every available frame so buffers never back up.
    AImage* img = nullptr;
    media_status_t st;
    while ((st = AImageReader_acquireNextImage(reader, &img)) == AMEDIA_OK && img) {
        int64_t ts = 0;
        int32_t w = 0, h = 0;
        AImage_getTimestamp(img, &ts);
        AImage_getWidth(img, &w);
        AImage_getHeight(img, &h);
        self->frameCount_.fetch_add(1, std::memory_order_relaxed);
        self->lastFrameW_.store(w, std::memory_order_relaxed);
        self->lastFrameH_.store(h, std::memory_order_relaxed);
        self->lastTimestampNs_.store(ts, std::memory_order_relaxed);
        AImage_delete(img);
        img = nullptr;
    }
    self->lastAcquireStatus_.store((int)st, std::memory_order_relaxed);
}

void CameraSession::onSessionActive(void* ctx, ACameraCaptureSession* /*session*/)
{
    if (auto* self = static_cast<CameraSession*>(ctx))
        self->log("capture session active");
}

void CameraSession::onSessionReady(void* /*ctx*/, ACameraCaptureSession* /*session*/)
{
    // Fires when the session goes idle; silent to avoid log noise.
}

void CameraSession::onSessionClosed(void* ctx, ACameraCaptureSession* /*session*/)
{
    if (auto* self = static_cast<CameraSession*>(ctx))
        self->log("capture session closed");
}

// Each completed result carries the AE state; cache it so the bridge can wait for
// an AE precapture to settle (FLASH_REQUIRED/CONVERGED) before an auto-flash shot.
void CameraSession::onCaptureResult(void* ctx, ACameraCaptureSession* /*session*/,
                                    ACaptureRequest* /*request*/, const ACameraMetadata* result)
{
    auto* self = static_cast<CameraSession*>(ctx);
    if (!self || !result)
        return;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_STATE, &e) == ACAMERA_OK && e.count >= 1)
        self->lastAeState_.store(e.data.u8[0]);
}

bool CameraSession::maxJpegSize(int* w, int* h) const
{
    for (const auto& c : cameras_) {
        if (c.id != openId_)
            continue;
        int bw = 0, bh = 0;
        long best = 0;
        for (const auto& s : c.outputs) {
            if (s.format != AIMAGE_FORMAT_JPEG)
                continue;
            long area = (long)s.width * s.height;
            if (area > best) {
                best = area;
                bw = s.width;
                bh = s.height;
            }
        }
        if (best > 0) {
            *w = bw;
            *h = bh;
            return true;
        }
    }
    return false;
}

std::vector<CameraSession::StreamConfig> CameraSession::jpegSizes() const
{
    std::vector<StreamConfig> out;
    for (const auto& c : cameras_) {
        if (c.id != openId_)
            continue;
        for (const auto& s : c.outputs)
            if (s.format == AIMAGE_FORMAT_JPEG && !s.isInput)
                out.push_back(s);
        break;
    }
    return out;
}

std::vector<CameraSession::StreamConfig> CameraSession::privateSizes() const
{
    std::vector<StreamConfig> out;
    for (const auto& c : cameras_) {
        if (c.id != openId_)
            continue;
        for (const auto& s : c.outputs)
            if (s.format == AIMAGE_FORMAT_PRIVATE && !s.isInput)
                out.push_back(s);
        break;
    }
    return out;
}

// Build the still-capture request + its JPEG output target once, lazily.  The
// request is device-scoped and targets jpegWindow_ (a stable session output that
// survives video-mode rebuilds), so it can be reused for every shot — capturePhoto
// only updates per-shot fields, avoiding a template fetch + target alloc round-trip
// on the shutter path.  Freed with jpegWindow_ in freeStreamResources().
bool CameraSession::ensureStillRequest()
{
    if (stillRequest_)
        return true;
    if (!device_ || !jpegWindow_) {
        lastError_ = "ensureStillRequest: no device / JPEG window";
        return false;
    }
    camera_status_t cs = ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &stillRequest_);
    if (cs != ACAMERA_OK || !stillRequest_) {
        stillRequest_ = nullptr;
        lastError_ = fmt("createCaptureRequest(STILL) failed (status %d)", (int)cs);
        return false;
    }
    cs = ACameraOutputTarget_create(jpegWindow_, &stillTarget_);
    if (cs == ACAMERA_OK)
        cs = ACaptureRequest_addTarget(stillRequest_, stillTarget_);
    if (cs != ACAMERA_OK) {
        if (stillTarget_) { ACameraOutputTarget_free(stillTarget_); stillTarget_ = nullptr; }
        ACaptureRequest_free(stillRequest_); stillRequest_ = nullptr;
        lastError_ = fmt("still request target failed (status %d)", (int)cs);
        return false;
    }
    return true;
}

bool CameraSession::capturePhoto(const std::string& path)
{
    if (!captureSession_ || !jpegWindow_) {
        lastError_ = "capturePhoto: no JPEG output (start preview with still capture)";
        return false;
    }
    if (!ensureStillRequest())
        return false;
    {
        std::lock_guard<std::mutex> lk(photoMutex_);
        pendingPhotoPath_ = path;
    }

    // Update only the per-shot fields on the cached request (local metadata writes,
    // no IPC until the submit below).
    //  - JPEG_ORIENTATION: sensor mount + device tilt (Android's getJpegOrientation)
    //  - JPEG_QUALITY: the user's chosen quality, captured directly (no re-encode)
    int32_t orientation = captureOrientation();
    ACaptureRequest_setEntry_i32(stillRequest_, ACAMERA_JPEG_ORIENTATION, 1, &orientation);
    uint8_t quality = (uint8_t)jpegQuality_;
    ACaptureRequest_setEntry_u8(stillRequest_, ACAMERA_JPEG_QUALITY, 1, &quality);

    // Per-shot flash via the AE mode on this still capture.
    uint8_t aeMode = ACAMERA_CONTROL_AE_MODE_ON;                 // off: normal AE, no flash
    if (flashMode_ == 1)      aeMode = ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
    else if (flashMode_ == 2) aeMode = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
    ACaptureRequest_setEntry_u8(stillRequest_, ACAMERA_CONTROL_AE_MODE, 1, &aeMode);

    int seqId = 0;
    camera_status_t cs = ACameraCaptureSession_capture(captureSession_, nullptr, 1, &stillRequest_, &seqId);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("ACameraCaptureSession_capture failed (status %d)", (int)cs);
        return false;
    }
    return true;
}

void CameraSession::onJpegImageAvailable(void* ctx, AImageReader* reader)
{
    auto* self = static_cast<CameraSession*>(ctx);
    if (!self || !reader)
        return;

    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img)
        return;

    std::string path;
    {
        std::lock_guard<std::mutex> lk(self->photoMutex_);
        path = self->pendingPhotoPath_;
    }

    bool ok = false;
    uint8_t* data = nullptr;
    int len = 0;
    if (!path.empty()
        && AImage_getPlaneData(img, 0, &data, &len) == AMEDIA_OK && data && len > 0) {
        // The buffer is sized to the max JPEG; trim to the End-Of-Image marker.
        int actual = len;
        for (int i = len - 2; i >= 0; --i) {
            if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                actual = i + 2;
                break;
            }
        }
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            ok = std::fwrite(data, 1, (size_t)actual, f) == (size_t)actual;
            std::fclose(f);
        }
        self->log(fmt("photo saved: %s (%d bytes)", path.c_str(), actual));
    }
    AImage_delete(img);

    if (self->photoCallback_)
        self->photoCallback_(path, ok);
}

bool CameraSession::startRecording(const std::string& path, int width, int height,
                                   int fps, int bitrate, bool withAudio)
{
    if (!ensureManager())
        return false;
    if (!device_) {
        lastError_ = "startRecording: no open camera (call open() first)";
        return false;
    }
    if (recording_) {
        lastError_ = "already recording";
        return false;
    }

    // Make sure the mic is available.  If it was pre-warmed (prepareRecording),
    // it is already running; otherwise start it now (cold path).
    if (withAudio && !audioEnc_) {
        audioEnc_ = std::make_unique<AudioEncoder>();
        if (!audioEnc_->start()) {
            log("audio capture unavailable, recording video-only: " + audioEnc_->lastError());
            audioEnc_.reset();
        }
    }
    const bool audioActive = withAudio && audioEnc_;

    // Attach the (already-running) mic stream to the current clip's muxer.  The
    // AAC track is added before the first video frame arrives, so the muxer
    // starts with both tracks present and no leading video frames are dropped.
    auto attachAudio = [this, audioActive]() {
        if (!audioActive)
            return;
        VideoEncoder* enc = encoder_.get();
        audioEnc_->attach(
            [enc](AMediaFormat* f) { return enc->addAudioTrack(f); },
            [enc](const uint8_t* d, AMediaCodecBufferInfo i) { enc->writeAudioSample(d, i); });
    };

    // ── Combined path (enterVideoMode): the encoder is already open and its
    // surface is a session output.  Just begin a clip and switch the repeating
    // request to include the encoder — preview never stops, no spin-up. ───────
    if (videoMode_) {
        if (!encoder_) {
            lastError_ = "video mode active but encoder not open";
            return false;
        }
        encoder_->expectAudio(audioActive);
        encoder_->setOrientation(captureOrientation());   // tag this clip with the current device tilt
        if (!encoder_->beginClip(path)) {
            lastError_ = "beginClip failed: " + encoder_->lastError();
            if (audioEnc_ && !audioPrewarmed_)
                audioEnc_.reset();
            return false;
        }
        attachAudio();
        applyControls(recordRequest_);
        camera_status_t rcs = ACameraCaptureSession_setRepeatingRequest(
            captureSession_, nullptr, 1, &recordRequest_, nullptr);
        if (rcs != ACAMERA_OK) {
            lastError_ = fmt("switch to record request failed (status %d)", (int)rcs);
            if (audioActive)
                audioEnc_->detach();
            encoder_->endClip();
            return false;
        }
        activeRequest_ = recordRequest_;
        recording_ = true;
        log(fmt("recording (preview+record) -> %s", path.c_str()));
        return true;
    }

    // ── Legacy record-only path (no enterVideoMode): preview stops during record.
    startBinderThreadPoolOnce();
    if (streaming_)
        stopPreview();

    encoder_ = std::make_unique<VideoEncoder>();
    encoder_->expectAudio(audioActive);   // gate muxer start on the audio track
    if (!encoder_->start(path, width, height, fps, bitrate, captureOrientation())) {
        lastError_ = "encoder start failed: " + encoder_->lastError();
        encoder_.reset();
        if (audioEnc_ && !audioPrewarmed_)
            audioEnc_.reset();   // tear down a cold-started mic; keep a warm one
        return false;
    }
    attachAudio();
    ANativeWindow* ew = encoder_->inputWindow();

    camera_status_t cs = ACaptureSessionOutputContainer_create(&recordContainer_);
    if (cs == ACAMERA_OK)
        cs = ACaptureSessionOutput_create(ew, &recordOutput_);
    if (cs == ACAMERA_OK)
        cs = ACaptureSessionOutputContainer_add(recordContainer_, recordOutput_);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("record output setup failed (status %d)", (int)cs);
        stopRecording();
        return false;
    }

    recordCb_          = ACameraCaptureSession_stateCallbacks{};
    recordCb_.context  = this;
    recordCb_.onActive = &CameraSession::onSessionActive;
    recordCb_.onReady  = &CameraSession::onSessionReady;
    recordCb_.onClosed = &CameraSession::onSessionClosed;
    cs = ACameraDevice_createCaptureSession(device_, recordContainer_, &recordCb_, &recordSession_);
    if (cs != ACAMERA_OK || !recordSession_) {
        lastError_ = fmt("record createCaptureSession failed (status %d)", (int)cs);
        stopRecording();
        return false;
    }

    cs = ACameraDevice_createCaptureRequest(device_, TEMPLATE_RECORD, &recordRequest_);
    if (cs == ACAMERA_OK)
        cs = ACameraOutputTarget_create(ew, &recordTarget_);
    if (cs == ACAMERA_OK)
        cs = ACaptureRequest_addTarget(recordRequest_, recordTarget_);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("record request setup failed (status %d)", (int)cs);
        stopRecording();
        return false;
    }

    int32_t fpsRange[2] = { videoFpsMin_, videoFpsMax_ };
    ACaptureRequest_setEntry_i32(recordRequest_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fpsRange);

    activeSession_ = recordSession_;
    activeRequest_ = recordRequest_;
    applyControls(recordRequest_);
    applyVideoStabilization(recordRequest_);   // EIS while recording (if supported)

    cs = ACameraCaptureSession_setRepeatingRequest(recordSession_, nullptr, 1, &recordRequest_, nullptr);
    if (cs != ACAMERA_OK) {
        lastError_ = fmt("record setRepeatingRequest failed (status %d)", (int)cs);
        stopRecording();
        return false;
    }

    recording_ = true;
    log(fmt("recording started: %dx%d @%dfps %d bps -> %s", width, height, fps, bitrate, path.c_str()));
    return true;
}

void CameraSession::stopRecording()
{
    // ── Combined path: switch back to preview-only (camera stops feeding the
    // encoder; preview keeps running), then soft-drain + finalize the clip.  The
    // encoder stays OPEN for the next clip — no teardown. ────────────────────
    if (videoMode_) {
        if (!recording_)
            return;
        if (captureSession_ && previewRequest_) {
            applyControls(previewRequest_);
            ACameraCaptureSession_setRepeatingRequest(captureSession_, nullptr, 1,
                                                      &previewRequest_, nullptr);
            activeRequest_ = previewRequest_;
        }
        if (audioEnc_) {
            audioEnc_->detach();
            if (!audioPrewarmed_) {
                audioEnc_->stop();
                audioEnc_.reset();
            }
        }
        if (encoder_)
            encoder_->endClip();   // soft-drain + finalize; encoder stays open
        recording_ = false;
        return;
    }

    // ── Legacy record-only teardown ──────────────────────────────────────────
    // Stop the camera feeding the encoder before finalizing the file.
    if (recordSession_) {
        ACameraCaptureSession_stopRepeating(recordSession_);
        ACameraCaptureSession_close(recordSession_);
        recordSession_ = nullptr;
    }
    // Detach the mic from the muxer first (blocks until any in-flight write
    // finishes) so no audio sample races the muxer finalize.  A pre-warmed mic
    // keeps running for the next recording; a cold-started one is torn down.
    if (audioEnc_) {
        audioEnc_->detach();
        if (!audioPrewarmed_) {
            audioEnc_->stop();
            audioEnc_.reset();
        }
    }
    if (encoder_) {
        encoder_->stop();   // signals EOS, drains, finalizes the MP4
        encoder_.reset();
    }
    if (recordRequest_) {
        ACaptureRequest_free(recordRequest_);
        recordRequest_ = nullptr;
    }
    if (recordTarget_) {
        ACameraOutputTarget_free(recordTarget_);
        recordTarget_ = nullptr;
    }
    if (recordOutput_) {
        ACaptureSessionOutput_free(recordOutput_);
        recordOutput_ = nullptr;
    }
    if (recordContainer_) {
        ACaptureSessionOutputContainer_free(recordContainer_);
        recordContainer_ = nullptr;
    }
    activeSession_ = nullptr;
    activeRequest_ = nullptr;
    recording_ = false;
}

bool CameraSession::prepareRecording()
{
    if (audioEnc_)
        return true;   // already warm
    audioEnc_ = std::make_unique<AudioEncoder>();
    if (!audioEnc_->start()) {
        lastError_ = "prepareRecording: mic unavailable: " + audioEnc_->lastError();
        log(lastError_);
        audioEnc_.reset();
        return false;
    }
    audioPrewarmed_ = true;
    log("audio pre-warmed (mic hot, frames discarded until record)");
    return true;
}

void CameraSession::releaseRecording()
{
    if (recording_)
        return;   // never tear down the mic mid-record
    if (audioEnc_) {
        audioEnc_->stop();
        audioEnc_.reset();
    }
    audioPrewarmed_ = false;
}

bool CameraSession::isAudioReady() const
{
    return audioEnc_ && audioEnc_->isReady();
}

// ── Manual controls (M7) ─────────────────────────────────────────────────────

void CameraSession::applyControls(ACaptureRequest* req) const
{
    if (!req)
        return;
    uint8_t ctlMode = ACAMERA_CONTROL_MODE_AUTO;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_MODE, 1, &ctlMode);

    uint8_t ae = (uint8_t)ctlAeMode_;
    // For AUTO flash, keep the preview AE in auto-flash so the HAL meters and can
    // decide to fire on the still capture.  (Always-flash is applied only on the
    // still request — on the preview it would behave like a torch.)
    if (ctlAeMode_ != ACAMERA_CONTROL_AE_MODE_OFF && flashMode_ == 2)
        ae = ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_MODE, 1, &ae);
    if (ctlAeMode_ == ACAMERA_CONTROL_AE_MODE_OFF) {
        int64_t exp = ctlExposureNs_;
        int32_t iso = ctlIso_;
        ACaptureRequest_setEntry_i64(req, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exp);
        ACaptureRequest_setEntry_i32(req, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);
    } else {
        int32_t ev = ctlEvComp_;
        ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &ev);
    }

    uint8_t aeLock  = (uint8_t)ctlAeLock_;
    uint8_t awbLock = (uint8_t)ctlAwbLock_;
    uint8_t awb     = (uint8_t)ctlAwbMode_;
    uint8_t af      = (uint8_t)ctlAfMode_;
    uint8_t flash   = ctlTorch_ ? (uint8_t)ACAMERA_FLASH_MODE_TORCH : (uint8_t)ACAMERA_FLASH_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AWB_LOCK, 1, &awbLock);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AWB_MODE, 1, &awb);
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af);
    ACaptureRequest_setEntry_u8(req, ACAMERA_FLASH_MODE, 1, &flash);

    float zoom = ctlZoom_;
    ACaptureRequest_setEntry_float(req, ACAMERA_CONTROL_ZOOM_RATIO, 1, &zoom);
}

// Electronic video stabilization for a recording request.  ON only when the open
// camera advertises it and the toggle is on; otherwise explicitly OFF (some HALs
// default TEMPLATE_RECORD to ON, so we set it deterministically either way).
void CameraSession::applyVideoStabilization(ACaptureRequest* req) const
{
    if (!req)
        return;
    uint8_t mode = (openVideoStab_ && videoStabEnabled_)
                       ? (uint8_t)ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON
                       : (uint8_t)ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &mode);
}

bool CameraSession::applyControlsToActive()
{
    if (!activeSession_ || !activeRequest_)
        return false;   // not streaming yet — state applies when a session starts
    applyControls(activeRequest_);
    return ACameraCaptureSession_setRepeatingRequest(
               activeSession_, nullptr, 1, &activeRequest_, nullptr) == ACAMERA_OK;
}

void CameraSession::setAutoExposure()
{
    ctlAeMode_ = ACAMERA_CONTROL_AE_MODE_ON;
    applyControlsToActive();
}

void CameraSession::setManualExposure(int iso, int64_t exposureNs)
{
    ctlAeMode_ = ACAMERA_CONTROL_AE_MODE_OFF;
    if (iso > 0)
        ctlIso_ = iso;
    if (exposureNs > 0)
        ctlExposureNs_ = exposureNs;
    applyControlsToActive();
}

void CameraSession::setExposureCompensation(int steps)
{
    ctlEvComp_ = steps;
    applyControlsToActive();
}

void CameraSession::setAeLock(bool lock)  { ctlAeLock_  = lock ? 1 : 0; applyControlsToActive(); }
void CameraSession::setAwbLock(bool lock) { ctlAwbLock_ = lock ? 1 : 0; applyControlsToActive(); }
void CameraSession::setAwbMode(int awbMode) { ctlAwbMode_ = awbMode; applyControlsToActive(); }
void CameraSession::setAfMode(int afMode)   { ctlAfMode_  = afMode;  applyControlsToActive(); }
void CameraSession::setTorch(bool on)       { ctlTorch_   = on ? 1 : 0; applyControlsToActive(); }
void CameraSession::setFlashMode(int mode)  { flashMode_  = mode;       applyControlsToActive(); }

// Kick an AE precapture metering pass on the active repeating request (a pre-flash
// to decide whether AUTO flash should fire on the upcoming still).  One-shot:
// START on a single capture, then reset to IDLE so the repeating request doesn't
// keep re-triggering.
void CameraSession::triggerPrecapture()
{
    if (!captureSession_ || !activeRequest_)
        return;
    uint8_t start = 1;   // ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START
    ACaptureRequest_setEntry_u8(activeRequest_, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &start);
    int seq = 0;
    ACameraCaptureSession_capture(captureSession_, nullptr, 1, &activeRequest_, &seq);
    uint8_t idle = 0;    // ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE
    ACaptureRequest_setEntry_u8(activeRequest_, ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER, 1, &idle);
}

void CameraSession::setZoomRatio(float ratio)
{
    if (ratio < 1.0f)
        ratio = 1.0f;
    if (ratio > maxZoomRatio())
        ratio = maxZoomRatio();
    ctlZoom_ = ratio;
    applyControlsToActive();
}

void CameraSession::setFocusPoint(float x, float y)
{
    if (!activeSession_ || !activeRequest_)
        return;
    const int aw = openActiveArray_[2], ah = openActiveArray_[3];
    if (aw <= 0 || ah <= 0)
        return;
    if (x < 0) x = 0; else if (x > 1) x = 1;
    if (y < 0) y = 0; else if (y > 1) y = 1;

    // A metering window ~10% of the active array centred on the tap.
    int rw = aw / 10, rh = ah / 10;
    int left = (int)(x * aw) - rw / 2;
    int top  = (int)(y * ah) - rh / 2;
    if (left < 0) left = 0; else if (left + rw > aw) left = aw - rw;
    if (top  < 0) top  = 0; else if (top + rh > ah)  top  = ah - rh;
    int32_t region[5] = { left, top, left + rw, top + rh, 1000 };

    ctlAfMode_ = ACAMERA_CONTROL_AF_MODE_AUTO;
    applyControls(activeRequest_);
    ACaptureRequest_setEntry_i32(activeRequest_, ACAMERA_CONTROL_AF_REGIONS, 5, region);
    ACaptureRequest_setEntry_i32(activeRequest_, ACAMERA_CONTROL_AE_REGIONS, 5, region);

    // Fire a one-shot AF trigger, then resume the repeating request (region stays).
    uint8_t start = ACAMERA_CONTROL_AF_TRIGGER_START;
    ACaptureRequest_setEntry_u8(activeRequest_, ACAMERA_CONTROL_AF_TRIGGER, 1, &start);
    int seq = 0;
    ACameraCaptureSession_capture(activeSession_, nullptr, 1, &activeRequest_, &seq);
    uint8_t idle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
    ACaptureRequest_setEntry_u8(activeRequest_, ACAMERA_CONTROL_AF_TRIGGER, 1, &idle);
    ACameraCaptureSession_setRepeatingRequest(activeSession_, &resultCb_, 1, &activeRequest_, nullptr);
}

void CameraSession::onDeviceDisconnected(void* ctx, ACameraDevice* /*device*/)
{
    if (auto* self = static_cast<CameraSession*>(ctx))
        self->log("camera disconnected");
}

void CameraSession::onDeviceError(void* ctx, ACameraDevice* /*device*/, int error)
{
    if (auto* self = static_cast<CameraSession*>(ctx))
        self->log(fmt("camera device error %d", error));
}

void CameraSession::dumpSummary(const CameraInfo& info) const
{
    log(fmt("camera %s: %s, orientation %d deg, %s",
            info.id.c_str(), facingName(info.facing), info.sensorOrientation,
            hardwareLevelName(info.hardwareLevel)));
    log(fmt("  sensor: %dx%d px   active array [%d %d %d %d]",
            info.pixelArrayW, info.pixelArrayH,
            info.activeArray[0], info.activeArray[1], info.activeArray[2], info.activeArray[3]));
    log(fmt("  ISO %d-%d   exposure %.3f-%.1f ms   manual sensor: %s",
            info.isoMin, info.isoMax,
            info.exposureMinNs / 1.0e6, info.exposureMaxNs / 1.0e6,
            info.manualSensor ? "YES" : "no"));
    log(fmt("  zoom %.1f-%.1fx   EV comp %d..%d @ %.3g EV/step   EIS: %s",
            info.zoomRatioMin, info.zoomRatioMax,
            info.evCompMin, info.evCompMax, info.evCompStep,
            info.videoStabSupported ? "YES" : "no"));

    std::string caps;
    for (int c : info.capabilities) {
        if (!caps.empty())
            caps += ", ";
        caps += capabilityName(c);
    }
    log(fmt("  capabilities: %s", caps.empty() ? "(none)" : caps.c_str()));

    log(fmt("  output stream configs: %zu", info.outputs.size()));
    const int notable[] = { AIMAGE_FORMAT_PRIVATE, AIMAGE_FORMAT_YUV_420_888, AIMAGE_FORMAT_JPEG };
    for (int f : notable) {
        int bw = 0, bh = 0;
        long bestArea = 0;
        for (const auto& s : info.outputs) {
            if (s.format != f)
                continue;
            long area = (long)s.width * s.height;
            if (area > bestArea) {
                bestArea = area;
                bw = s.width;
                bh = s.height;
            }
        }
        if (bestArea > 0)
            log(fmt("    %-12s max %dx%d", formatName(f), bw, bh));
    }
}

void CameraSession::dumpAllTags(const std::string& id) const
{
    if (isHostStub()) {
        log("dumpAllTags: host stub build, nothing to read");
        return;
    }
    if (!manager_) {
        log("dumpAllTags: manager not created (call enumerate() first)");
        return;
    }

    ACameraMetadata* meta = nullptr;
    if (ACameraManager_getCameraCharacteristics(manager_, id.c_str(), &meta) != ACAMERA_OK || !meta) {
        log(fmt("dumpAllTags(%s): getCameraCharacteristics failed", id.c_str()));
        return;
    }

    int32_t numTags = 0;
    const uint32_t* tags = nullptr;
    if (ACameraMetadata_getAllTags(meta, &numTags, &tags) == ACAMERA_OK && tags) {
        log(fmt("camera %s exposes %d metadata tags:", id.c_str(), numTags));
        static const char* typeName[] = { "byte", "i32", "f32", "i64", "f64", "rat" };
        for (int32_t i = 0; i < numTags; ++i) {
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(meta, tags[i], &e) != ACAMERA_OK) {
                log(fmt("  0x%08x  <unreadable>", tags[i]));
                continue;
            }
            std::string vals;
            uint32_t show = e.count < 12 ? e.count : 12;
            for (uint32_t k = 0; k < show; ++k) {
                if (!vals.empty())
                    vals += ' ';
                switch (e.type) {
                    case ACAMERA_TYPE_BYTE:     vals += fmt("%d", (int)e.data.u8[k]); break;
                    case ACAMERA_TYPE_INT32:    vals += fmt("%d", e.data.i32[k]); break;
                    case ACAMERA_TYPE_FLOAT:    vals += fmt("%g", e.data.f[k]); break;
                    case ACAMERA_TYPE_INT64:    vals += fmt("%lld", (long long)e.data.i64[k]); break;
                    case ACAMERA_TYPE_DOUBLE:   vals += fmt("%g", e.data.d[k]); break;
                    case ACAMERA_TYPE_RATIONAL: vals += fmt("%d/%d", e.data.r[k].numerator, e.data.r[k].denominator); break;
                    default:                    vals += "?"; break;
                }
            }
            if (e.count > show)
                vals += " ...";
            const char* tn = (e.type < ACAMERA_NUM_TYPES) ? typeName[e.type] : "?";
            log(fmt("  0x%08x  %-4s [%u]  %s", tags[i], tn, e.count, vals.c_str()));
        }
    }
    ACameraMetadata_free(meta);
}

const char* CameraSession::facingName(int facing)
{
    switch (facing) {
        case ACAMERA_LENS_FACING_FRONT:    return "front camera";
        case ACAMERA_LENS_FACING_BACK:     return "back camera";
        case ACAMERA_LENS_FACING_EXTERNAL: return "external camera";
        default:                           return "unknown-facing";
    }
}

const char* CameraSession::hardwareLevelName(int level)
{
    switch (level) {
        case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY:   return "hw level LEGACY";
        case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED:  return "hw level LIMITED";
        case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_FULL:     return "hw level FULL";
        case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_3:        return "hw level 3";
        case ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL: return "hw level EXTERNAL";
        default:                                             return "hw level unknown";
    }
}

const char* CameraSession::capabilityName(int cap)
{
    switch (cap) {
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE:    return "backward_compatible";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR:          return "manual_sensor";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING: return "manual_post_processing";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW:                    return "raw";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS:   return "read_sensor_settings";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE:          return "burst_capture";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT:           return "depth_output";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MOTION_TRACKING:        return "motion_tracking";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA:   return "logical_multi_camera";
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MONOCHROME:             return "monochrome";
        default:                                                           return "cap?";
    }
}

const char* CameraSession::formatName(int format)
{
    switch (format) {
        case AIMAGE_FORMAT_PRIVATE:     return "PRIVATE";
        case AIMAGE_FORMAT_YUV_420_888: return "YUV_420_888";
        case AIMAGE_FORMAT_JPEG:        return "JPEG";
        case AIMAGE_FORMAT_RAW16:       return "RAW16";
        case AIMAGE_FORMAT_RAW10:       return "RAW10";
        case AIMAGE_FORMAT_RAW12:       return "RAW12";
        case AIMAGE_FORMAT_RAW_PRIVATE: return "RAW_PRIVATE";
        case AIMAGE_FORMAT_RGBA_8888:   return "RGBA_8888";
        default:                        return "other-format";
    }
}

} // namespace furicam2
