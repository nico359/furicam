/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCameraError.h — Camera2 NDK status codes (camera_status_t).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Returned by
 * value, so the enum must be a complete type.  Values match the public Android
 * NDK ABI (API level 24+) exactly.
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAERROR_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACAMERA_OK                          = 0,
    ACAMERA_ERROR_BASE                  = -10000,
    ACAMERA_ERROR_UNKNOWN               = ACAMERA_ERROR_BASE,
    ACAMERA_ERROR_INVALID_PARAMETER     = ACAMERA_ERROR_BASE - 1,
    ACAMERA_ERROR_CAMERA_DISCONNECTED   = ACAMERA_ERROR_BASE - 2,
    ACAMERA_ERROR_NOT_ENOUGH_MEMORY     = ACAMERA_ERROR_BASE - 3,
    ACAMERA_ERROR_METADATA_NOT_FOUND    = ACAMERA_ERROR_BASE - 4,
    ACAMERA_ERROR_CAMERA_DEVICE         = ACAMERA_ERROR_BASE - 5,
    ACAMERA_ERROR_CAMERA_SERVICE        = ACAMERA_ERROR_BASE - 6,
    ACAMERA_ERROR_SESSION_CLOSED        = ACAMERA_ERROR_BASE - 7,
    ACAMERA_ERROR_INVALID_OPERATION     = ACAMERA_ERROR_BASE - 8,
    ACAMERA_ERROR_STREAM_CONFIGURE_FAIL = ACAMERA_ERROR_BASE - 9,
    ACAMERA_ERROR_CAMERA_IN_USE         = ACAMERA_ERROR_BASE - 10,
    ACAMERA_ERROR_MAX_CAMERA_IN_USE     = ACAMERA_ERROR_BASE - 11,
    ACAMERA_ERROR_CAMERA_DISABLED       = ACAMERA_ERROR_BASE - 12,
    ACAMERA_ERROR_PERMISSION_DENIED     = ACAMERA_ERROR_BASE - 13,
    ACAMERA_ERROR_UNSUPPORTED_OPERATION = ACAMERA_ERROR_BASE - 14,
} camera_status_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAERROR_H */
