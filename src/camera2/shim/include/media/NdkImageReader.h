/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkImageReader.h — a CPU/GPU-readable image queue and its ANativeWindow
 * surface (AImageReader); the consumer end of a camera output stream.
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGEREADER_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGEREADER_H

#include <stdint.h>

#include "media/NdkMediaError.h"
#include "media/NdkImage.h"
#include "android/native_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AImageReader AImageReader;

typedef void (*AImageReader_ImageCallback)(void* context, AImageReader* reader);

typedef struct AImageReader_ImageListener {
    void*                      context;
    AImageReader_ImageCallback onImageAvailable;
} AImageReader_ImageListener;

media_status_t AImageReader_new(int32_t width, int32_t height, int32_t format,
                                int32_t maxImages, AImageReader** reader);
media_status_t AImageReader_newWithUsage(int32_t width, int32_t height, int32_t format,
                                         uint64_t usage, int32_t maxImages, AImageReader** reader);
void           AImageReader_delete(AImageReader* reader);
media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
media_status_t AImageReader_acquireNextImage(AImageReader* reader, AImage** image);
media_status_t AImageReader_acquireLatestImage(AImageReader* reader, AImage** image);
media_status_t AImageReader_setImageListener(AImageReader* reader,
                                             AImageReader_ImageListener* listener);
media_status_t AImageReader_getFormat(AImageReader* reader, int32_t* format);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKIMAGEREADER_H */
