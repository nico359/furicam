/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkMediaFormat.h — codec/track format key-value bags (AMediaFormat).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 21+).
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAFORMAT_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAFORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media/NdkMediaError.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AMediaFormat AMediaFormat;

AMediaFormat*  AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat* format);
void           AMediaFormat_setString(AMediaFormat* format, const char* name, const char* value);
void           AMediaFormat_setInt32(AMediaFormat* format, const char* name, int32_t value);
void           AMediaFormat_setInt64(AMediaFormat* format, const char* name, int64_t value);
void           AMediaFormat_setFloat(AMediaFormat* format, const char* name, float value);
void           AMediaFormat_setBuffer(AMediaFormat* format, const char* name,
                                      const void* data, size_t size);
bool           AMediaFormat_getInt32(AMediaFormat* format, const char* name, int32_t* out);
bool           AMediaFormat_getInt64(AMediaFormat* format, const char* name, int64_t* out);
const char*    AMediaFormat_toString(AMediaFormat* format);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAFORMAT_H */
