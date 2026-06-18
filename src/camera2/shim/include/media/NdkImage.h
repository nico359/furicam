/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkImage.h — a single acquired image and its planes (AImage).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGE_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGE_H

#include <stdint.h>

#include "media/NdkMediaError.h"
#include "android/hardware_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AImage AImage;

/* Image formats (values mirror the HAL pixel formats). */
enum {
    AIMAGE_FORMAT_RGBA_8888   = 0x1,
    AIMAGE_FORMAT_RGBX_8888   = 0x2,
    AIMAGE_FORMAT_RGB_888     = 0x3,
    AIMAGE_FORMAT_RGB_565     = 0x4,
    AIMAGE_FORMAT_RGBA_FP16   = 0x16,
    AIMAGE_FORMAT_YUV_420_888 = 0x23,
    AIMAGE_FORMAT_JPEG        = 0x100,
    AIMAGE_FORMAT_RAW16       = 0x20,
    AIMAGE_FORMAT_RAW_PRIVATE = 0x24,
    AIMAGE_FORMAT_RAW10       = 0x25,
    AIMAGE_FORMAT_RAW12       = 0x26,
    AIMAGE_FORMAT_PRIVATE     = 0x22,
};

void           AImage_delete(AImage* image);
media_status_t AImage_getWidth(const AImage* image, int32_t* width);
media_status_t AImage_getHeight(const AImage* image, int32_t* height);
media_status_t AImage_getFormat(const AImage* image, int32_t* format);
media_status_t AImage_getTimestamp(const AImage* image, int64_t* timestampNs);
media_status_t AImage_getNumberOfPlanes(const AImage* image, int32_t* numPlanes);
media_status_t AImage_getPlaneData(const AImage* image, int planeIdx,
                                   uint8_t** data, int* dataLength);
media_status_t AImage_getPlaneRowStride(const AImage* image, int planeIdx, int32_t* rowStride);
media_status_t AImage_getPlanePixelStride(const AImage* image, int planeIdx, int32_t* pixelStride);
media_status_t AImage_getHardwareBuffer(const AImage* image, AHardwareBuffer** buffer);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGE_H */
