/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCaptureRequest.h — capture-request building and output targets
 * (ACaptureRequest, ACameraOutputTarget).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAPTUREREQUEST_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAPTUREREQUEST_H

#include <stdint.h>

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraMetadata.h"
#include "android/native_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACaptureRequest     ACaptureRequest;
typedef struct ACameraOutputTarget ACameraOutputTarget;

camera_status_t ACameraOutputTarget_create(ANativeWindow* window, ACameraOutputTarget** output);
void            ACameraOutputTarget_free(ACameraOutputTarget* output);

camera_status_t ACaptureRequest_addTarget(ACaptureRequest* request,
                                          const ACameraOutputTarget* output);
camera_status_t ACaptureRequest_removeTarget(ACaptureRequest* request,
                                             const ACameraOutputTarget* output);
camera_status_t ACaptureRequest_getConstEntry(const ACaptureRequest* request, uint32_t tag,
                                              ACameraMetadata_const_entry* entry);
camera_status_t ACaptureRequest_setEntry_i32(ACaptureRequest* request, uint32_t tag,
                                             uint32_t count, const int32_t* data);
camera_status_t ACaptureRequest_setEntry_i64(ACaptureRequest* request, uint32_t tag,
                                             uint32_t count, const int64_t* data);
camera_status_t ACaptureRequest_setEntry_float(ACaptureRequest* request, uint32_t tag,
                                               uint32_t count, const float* data);
camera_status_t ACaptureRequest_setEntry_u8(ACaptureRequest* request, uint32_t tag,
                                            uint32_t count, const uint8_t* data);
void            ACaptureRequest_free(ACaptureRequest* request);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAPTUREREQUEST_H */
