/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCameraCaptureSession.h — the capture session, its lifecycle/capture
 * callbacks, and the repeating/one-shot capture control calls.
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  The callback
 * structs are populated by the caller, so they are defined faithfully to the
 * NDK layout.  Spelling matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERACAPTURESESSION_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERACAPTURESESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraMetadata.h"
#include "camera/NdkCaptureRequest.h"
#include "android/native_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraCaptureSession ACameraCaptureSession;

typedef void (*ACameraCaptureSession_stateCallback)(void* context,
                                                    ACameraCaptureSession* session);

typedef struct ACameraCaptureSession_stateCallbacks {
    void*                               context;
    ACameraCaptureSession_stateCallback onClosed;
    ACameraCaptureSession_stateCallback onReady;
    ACameraCaptureSession_stateCallback onActive;
} ACameraCaptureSession_stateCallbacks;

typedef struct ACameraCaptureFailure {
    int64_t frameNumber;
    int     reason;
    int     sequenceId;
    bool    wasImageCaptured;
} ACameraCaptureFailure;

typedef void (*ACameraCaptureSession_captureCallback_start)(
        void* context, ACameraCaptureSession* session,
        const ACaptureRequest* request, int64_t timestamp);
typedef void (*ACameraCaptureSession_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result);
typedef void (*ACameraCaptureSession_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraCaptureFailure* failure);
typedef void (*ACameraCaptureSession_captureCallback_sequenceEnd)(
        void* context, ACameraCaptureSession* session,
        int sequenceId, int64_t frameNumber);
typedef void (*ACameraCaptureSession_captureCallback_sequenceAbort)(
        void* context, ACameraCaptureSession* session, int sequenceId);
typedef void (*ACameraCaptureSession_captureCallback_bufferLost)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ANativeWindow* window, int64_t frameNumber);

typedef struct ACameraCaptureSession_captureCallbacks {
    void*                                               context;
    ACameraCaptureSession_captureCallback_start         onCaptureStarted;
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;
    ACameraCaptureSession_captureCallback_result        onCaptureCompleted;
    ACameraCaptureSession_captureCallback_failed        onCaptureFailed;
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;
} ACameraCaptureSession_captureCallbacks;

void            ACameraCaptureSession_close(ACameraCaptureSession* session);
camera_status_t ACameraCaptureSession_setRepeatingRequest(
        ACameraCaptureSession* session, ACameraCaptureSession_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests, int* captureSequenceId);
camera_status_t ACameraCaptureSession_capture(
        ACameraCaptureSession* session, ACameraCaptureSession_captureCallbacks* callbacks,
        int numRequests, ACaptureRequest** requests, int* captureSequenceId);
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession* session);
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession* session);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERACAPTURESESSION_H */
