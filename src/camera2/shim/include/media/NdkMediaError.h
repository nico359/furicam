/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkMediaError.h — Media NDK status codes (media_status_t).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Returned by
 * value, so the enum must be a complete type.  Values match the public Android
 * NDK ABI (API level 21+) exactly.
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAERROR_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AMEDIA_OK                      = 0,
    AMEDIA_ERROR_BASE              = -10000,
    AMEDIA_ERROR_UNKNOWN           = AMEDIA_ERROR_BASE,
    AMEDIA_ERROR_MALFORMED         = AMEDIA_ERROR_BASE - 1,
    AMEDIA_ERROR_UNSUPPORTED       = AMEDIA_ERROR_BASE - 2,
    AMEDIA_ERROR_INVALID_OBJECT    = AMEDIA_ERROR_BASE - 3,
    AMEDIA_ERROR_INVALID_PARAMETER = AMEDIA_ERROR_BASE - 4,
    AMEDIA_ERROR_INVALID_OPERATION = AMEDIA_ERROR_BASE - 5,
    AMEDIA_ERROR_END_OF_STREAM     = AMEDIA_ERROR_BASE - 6,
    AMEDIA_ERROR_IO                = AMEDIA_ERROR_BASE - 7,
    AMEDIA_ERROR_WOULD_BLOCK       = AMEDIA_ERROR_BASE - 8,
} media_status_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAERROR_H */
