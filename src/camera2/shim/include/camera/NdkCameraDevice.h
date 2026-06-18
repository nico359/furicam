/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCameraDevice.h — the opened camera device, capture-request
 * templates, device state callbacks, and session-output containers/targets.
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERADEVICE_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERADEVICE_H

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraCaptureSession.h"
#include "camera/NdkCaptureRequest.h"
#include "android/native_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraDevice ACameraDevice;

/* Standard capture-request templates. */
typedef enum {
    TEMPLATE_PREVIEW          = 1,
    TEMPLATE_STILL_CAPTURE    = 2,
    TEMPLATE_RECORD           = 3,
    TEMPLATE_VIDEO_SNAPSHOT   = 4,
    TEMPLATE_ZERO_SHUTTER_LAG = 5,
    TEMPLATE_MANUAL           = 6,
} ACameraDevice_request_template;

typedef void (*ACameraDevice_StateCallback)(void* context, ACameraDevice* device);
typedef void (*ACameraDevice_ErrorStateCallback)(void* context, ACameraDevice* device, int error);

typedef struct ACameraDevice_StateCallbacks {
    void*                            context;
    ACameraDevice_StateCallback      onDisconnected;
    ACameraDevice_ErrorStateCallback onError;
} ACameraDevice_StateCallbacks;

/* Session output container and a single session output. */
typedef struct ACaptureSessionOutput          ACaptureSessionOutput;
typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;

camera_status_t ACaptureSessionOutputContainer_create(ACaptureSessionOutputContainer** container);
void            ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer* container);
camera_status_t ACaptureSessionOutput_create(ANativeWindow* window, ACaptureSessionOutput** output);
void            ACaptureSessionOutput_free(ACaptureSessionOutput* output);
camera_status_t ACaptureSessionOutputContainer_add(ACaptureSessionOutputContainer* container,
                                                   const ACaptureSessionOutput* output);
camera_status_t ACaptureSessionOutputContainer_remove(ACaptureSessionOutputContainer* container,
                                                      const ACaptureSessionOutput* output);

camera_status_t ACameraDevice_close(ACameraDevice* device);
const char*     ACameraDevice_getId(const ACameraDevice* device);
camera_status_t ACameraDevice_createCaptureRequest(const ACameraDevice* device,
                                                   ACameraDevice_request_template templateId,
                                                   ACaptureRequest** request);
camera_status_t ACameraDevice_createCaptureSession(
        ACameraDevice* device, const ACaptureSessionOutputContainer* outputs,
        const ACameraCaptureSession_stateCallbacks* callbacks, ACameraCaptureSession** session);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERADEVICE_H */
