/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * Camera2NDKShim.c — libhybris forwarding shim for the Android Camera2 /
 * Media NDK C application programming interface (API).  This is the whole of
 * libcamera2ndk-hybris: each line below turns one Bionic-linked Android NDK
 * symbol into one a glibc-linked Linux process can call directly.
 *
 * Each HYBRIS_IMPLEMENT_FUNCTION_* line expands (via <hybris/common/binding.h>)
 * into a real function with the exact NDK symbol name.  On first call it
 * android_dlopen()s the backing Android shared object and android_dlsym()s the
 * symbol, caching the resolved pointer in a function-local static, then
 * forwards.  Callers link -lcamera2ndk-hybris -lhybris-common and call
 * ACameraManager_create() (etc.) exactly as the Android NDK documents — with no
 * awareness of libhybris, except for the one-time threadpool init in
 * <camera2ndk_hybris.h>.
 *
 * Why this is C, not C++:
 *   HYBRIS_DLSYSM() resolves symbols with `*(fptr) = (void *) android_dlsym(..)`
 *   — an implicit void*-to-function-pointer assignment that is valid C but
 *   ill-formed C++.  libhybris's own wrappers (egl.c, glesv2.c, ...) are C for
 *   the same reason.  The NDK headers guard their prototypes with extern "C", so
 *   C++ callers link against these C-linkage definitions cleanly.
 *
 * The backing libraries live under /android/system/lib64 (e.g. on the FuriPhone
 * FLX1s) and are reached through libhybris's android linker namespace; we never
 * link them directly (see CMakeLists.txt: only libhybris-common is linked).
 *
 * Keep this file in lock-step with the prototype list in the NDK headers.
 */

#include <dlfcn.h>      /* RTLD_LAZY, referenced by HYBRIS_LIBRARY_INITIALIZE */
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>  /* ssize_t */

#include "Camera2NDK.h"
#include <hybris/common/binding.h>

/* ───────────────────────────────────────────────────────────────────────────
 * libcamera2ndk.so
 * ─────────────────────────────────────────────────────────────────────────── */
HYBRIS_LIBRARY_INITIALIZE(camera2ndk, "libcamera2ndk.so")

/* Manager. */
HYBRIS_IMPLEMENT_FUNCTION0(camera2ndk, ACameraManager*, ACameraManager_create)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACameraManager_delete, ACameraManager*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACameraManager_getCameraIdList,
                           ACameraManager*, ACameraIdList**)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACameraManager_deleteCameraIdList, ACameraIdList*)
HYBRIS_IMPLEMENT_FUNCTION3(camera2ndk, camera_status_t, ACameraManager_getCameraCharacteristics,
                           ACameraManager*, const char*, ACameraMetadata**)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACameraManager_openCamera,
                           ACameraManager*, const char*, ACameraDevice_StateCallbacks*, ACameraDevice**)

/* Metadata (read-only characteristics / results). */
HYBRIS_IMPLEMENT_FUNCTION3(camera2ndk, camera_status_t, ACameraMetadata_getConstEntry,
                           const ACameraMetadata*, uint32_t, ACameraMetadata_const_entry*)
HYBRIS_IMPLEMENT_FUNCTION3(camera2ndk, camera_status_t, ACameraMetadata_getAllTags,
                           const ACameraMetadata*, int32_t*, const uint32_t**)
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, ACameraMetadata*, ACameraMetadata_copy,
                           const ACameraMetadata*)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACameraMetadata_free, ACameraMetadata*)

/* Device. */
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, camera_status_t, ACameraDevice_close, ACameraDevice*)
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, const char*, ACameraDevice_getId, const ACameraDevice*)
HYBRIS_IMPLEMENT_FUNCTION3(camera2ndk, camera_status_t, ACameraDevice_createCaptureRequest,
                           const ACameraDevice*, ACameraDevice_request_template, ACaptureRequest**)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACameraDevice_createCaptureSession,
                           ACameraDevice*, const ACaptureSessionOutputContainer*,
                           const ACameraCaptureSession_stateCallbacks*, ACameraCaptureSession**)

/* Session output container / targets. */
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, camera_status_t, ACaptureSessionOutputContainer_create,
                           ACaptureSessionOutputContainer**)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACaptureSessionOutputContainer_free,
                                ACaptureSessionOutputContainer*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACaptureSessionOutput_create,
                           ANativeWindow*, ACaptureSessionOutput**)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACaptureSessionOutput_free, ACaptureSessionOutput*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACaptureSessionOutputContainer_add,
                           ACaptureSessionOutputContainer*, const ACaptureSessionOutput*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACaptureSessionOutputContainer_remove,
                           ACaptureSessionOutputContainer*, const ACaptureSessionOutput*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACameraOutputTarget_create,
                           ANativeWindow*, ACameraOutputTarget**)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACameraOutputTarget_free, ACameraOutputTarget*)

/* Capture request building. */
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACaptureRequest_addTarget,
                           ACaptureRequest*, const ACameraOutputTarget*)
HYBRIS_IMPLEMENT_FUNCTION2(camera2ndk, camera_status_t, ACaptureRequest_removeTarget,
                           ACaptureRequest*, const ACameraOutputTarget*)
HYBRIS_IMPLEMENT_FUNCTION3(camera2ndk, camera_status_t, ACaptureRequest_getConstEntry,
                           const ACaptureRequest*, uint32_t, ACameraMetadata_const_entry*)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACaptureRequest_setEntry_i32,
                           ACaptureRequest*, uint32_t, uint32_t, const int32_t*)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACaptureRequest_setEntry_i64,
                           ACaptureRequest*, uint32_t, uint32_t, const int64_t*)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACaptureRequest_setEntry_float,
                           ACaptureRequest*, uint32_t, uint32_t, const float*)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACaptureRequest_setEntry_u8,
                           ACaptureRequest*, uint32_t, uint32_t, const uint8_t*)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACaptureRequest_free, ACaptureRequest*)

/* Capture session control. */
HYBRIS_IMPLEMENT_VOID_FUNCTION1(camera2ndk, ACameraCaptureSession_close, ACameraCaptureSession*)
HYBRIS_IMPLEMENT_FUNCTION5(camera2ndk, camera_status_t, ACameraCaptureSession_setRepeatingRequest,
                           ACameraCaptureSession*, ACameraCaptureSession_captureCallbacks*,
                           int, ACaptureRequest**, int*)
HYBRIS_IMPLEMENT_FUNCTION5(camera2ndk, camera_status_t, ACameraCaptureSession_capture,
                           ACameraCaptureSession*, ACameraCaptureSession_captureCallbacks*,
                           int, ACaptureRequest**, int*)
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, camera_status_t, ACameraCaptureSession_stopRepeating,
                           ACameraCaptureSession*)
HYBRIS_IMPLEMENT_FUNCTION1(camera2ndk, camera_status_t, ACameraCaptureSession_abortCaptures,
                           ACameraCaptureSession*)

/* ───────────────────────────────────────────────────────────────────────────
 * libmediandk.so
 * ─────────────────────────────────────────────────────────────────────────── */
HYBRIS_LIBRARY_INITIALIZE(mediandk, "libmediandk.so")

/* AImageReader. */
HYBRIS_IMPLEMENT_FUNCTION5(mediandk, media_status_t, AImageReader_new,
                           int32_t, int32_t, int32_t, int32_t, AImageReader**)
HYBRIS_IMPLEMENT_FUNCTION6(mediandk, media_status_t, AImageReader_newWithUsage,
                           int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(mediandk, AImageReader_delete, AImageReader*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImageReader_getWindow,
                           AImageReader*, ANativeWindow**)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImageReader_acquireNextImage,
                           AImageReader*, AImage**)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImageReader_acquireLatestImage,
                           AImageReader*, AImage**)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImageReader_setImageListener,
                           AImageReader*, AImageReader_ImageListener*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImageReader_getFormat,
                           AImageReader*, int32_t*)

/* AImage. */
HYBRIS_IMPLEMENT_VOID_FUNCTION1(mediandk, AImage_delete, AImage*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getWidth, const AImage*, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getHeight, const AImage*, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getFormat, const AImage*, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getTimestamp, const AImage*, int64_t*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getNumberOfPlanes, const AImage*, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION4(mediandk, media_status_t, AImage_getPlaneData,
                           const AImage*, int, uint8_t**, int*)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, media_status_t, AImage_getPlaneRowStride,
                           const AImage*, int, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, media_status_t, AImage_getPlanePixelStride,
                           const AImage*, int, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AImage_getHardwareBuffer,
                           const AImage*, AHardwareBuffer**)

/* AMediaCodec (hardware H.264 encoder). */
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, AMediaCodec*, AMediaCodec_createEncoderByType, const char*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, AMediaCodec*, AMediaCodec_createCodecByName, const char*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaCodec_delete, AMediaCodec*)
HYBRIS_IMPLEMENT_FUNCTION5(mediandk, media_status_t, AMediaCodec_configure,
                           AMediaCodec*, const AMediaFormat*, ANativeWindow*, AMediaCrypto*, uint32_t)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AMediaCodec_createInputSurface,
                           AMediaCodec*, ANativeWindow**)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaCodec_start, AMediaCodec*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaCodec_stop, AMediaCodec*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaCodec_signalEndOfInputStream, AMediaCodec*)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, ssize_t, AMediaCodec_dequeueOutputBuffer,
                           AMediaCodec*, AMediaCodecBufferInfo*, int64_t)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, uint8_t*, AMediaCodec_getOutputBuffer,
                           AMediaCodec*, size_t, size_t*)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, media_status_t, AMediaCodec_releaseOutputBuffer,
                           AMediaCodec*, size_t, bool)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, AMediaFormat*, AMediaCodec_getOutputFormat, AMediaCodec*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AMediaCodec_setParameters,
                           AMediaCodec*, const AMediaFormat*)

/* AMediaFormat. */
HYBRIS_IMPLEMENT_FUNCTION0(mediandk, AMediaFormat*, AMediaFormat_new)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaFormat_delete, AMediaFormat*)
HYBRIS_IMPLEMENT_VOID_FUNCTION3(mediandk, AMediaFormat_setString,
                                AMediaFormat*, const char*, const char*)
HYBRIS_IMPLEMENT_VOID_FUNCTION3(mediandk, AMediaFormat_setInt32,
                                AMediaFormat*, const char*, int32_t)
HYBRIS_IMPLEMENT_VOID_FUNCTION3(mediandk, AMediaFormat_setInt64,
                                AMediaFormat*, const char*, int64_t)
HYBRIS_IMPLEMENT_VOID_FUNCTION3(mediandk, AMediaFormat_setFloat,
                                AMediaFormat*, const char*, float)
HYBRIS_IMPLEMENT_VOID_FUNCTION4(mediandk, AMediaFormat_setBuffer,
                                AMediaFormat*, const char*, const void*, size_t)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, bool, AMediaFormat_getInt32,
                           AMediaFormat*, const char*, int32_t*)
HYBRIS_IMPLEMENT_FUNCTION3(mediandk, bool, AMediaFormat_getInt64,
                           AMediaFormat*, const char*, int64_t*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, const char*, AMediaFormat_toString, AMediaFormat*)

/* AMediaMuxer (MPEG-4 Part 14 container writer). */
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, AMediaMuxer*, AMediaMuxer_new, int, OutputFormat)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaMuxer_delete, AMediaMuxer*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, ssize_t, AMediaMuxer_addTrack, AMediaMuxer*, const AMediaFormat*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaMuxer_start, AMediaMuxer*)
HYBRIS_IMPLEMENT_FUNCTION1(mediandk, media_status_t, AMediaMuxer_stop, AMediaMuxer*)
HYBRIS_IMPLEMENT_FUNCTION4(mediandk, media_status_t, AMediaMuxer_writeSampleData,
                           AMediaMuxer*, size_t, const uint8_t*, const AMediaCodecBufferInfo*)
HYBRIS_IMPLEMENT_FUNCTION2(mediandk, media_status_t, AMediaMuxer_setOrientationHint,
                           AMediaMuxer*, int)

/* ───────────────────────────────────────────────────────────────────────────
 * libnativewindow.so — buffer/window lifetime for the EGL import path (M3).
 * ─────────────────────────────────────────────────────────────────────────── */
HYBRIS_LIBRARY_INITIALIZE(nativewindow, "libnativewindow.so")

HYBRIS_IMPLEMENT_VOID_FUNCTION1(nativewindow, AHardwareBuffer_acquire, AHardwareBuffer*)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(nativewindow, AHardwareBuffer_release, AHardwareBuffer*)
HYBRIS_IMPLEMENT_VOID_FUNCTION2(nativewindow, AHardwareBuffer_describe,
                                const AHardwareBuffer*, AHardwareBuffer_Desc*)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(nativewindow, ANativeWindow_acquire, ANativeWindow*)
HYBRIS_IMPLEMENT_VOID_FUNCTION1(nativewindow, ANativeWindow_release, ANativeWindow*)
