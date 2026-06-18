/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkMediaCodec.h — hardware video encode/decode (AMediaCodec).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris; used here to
 * drive the device's hardware H.264 encoder.  Spelling matches the public
 * Android NDK ABI (API level 21+).
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIACODEC_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIACODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

#include "media/NdkMediaError.h"
#include "media/NdkMediaFormat.h"
#include "android/native_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AMediaCodec  AMediaCodec;
typedef struct AMediaCrypto AMediaCrypto;

/* Buffer/configure flags and the negative dequeue sentinels returned (as an
 * ssize_t) by AMediaCodec_dequeueOutputBuffer. */
enum {
    AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG    = 2,
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM   = 4,
    AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME   = 8,
    AMEDIACODEC_CONFIGURE_FLAG_ENCODE       = 1,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED  = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER        = -1,
};

typedef struct AMediaCodecBufferInfo {
    int32_t  offset;
    int32_t  size;
    int64_t  presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

AMediaCodec*   AMediaCodec_createEncoderByType(const char* mimeType);
AMediaCodec*   AMediaCodec_createCodecByName(const char* name);
media_status_t AMediaCodec_delete(AMediaCodec* codec);
media_status_t AMediaCodec_configure(AMediaCodec* codec, const AMediaFormat* format,
                                     ANativeWindow* surface, AMediaCrypto* crypto, uint32_t flags);
media_status_t AMediaCodec_createInputSurface(AMediaCodec* codec, ANativeWindow** surface);
media_status_t AMediaCodec_start(AMediaCodec* codec);
media_status_t AMediaCodec_stop(AMediaCodec* codec);
media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec* codec);
ssize_t        AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, AMediaCodecBufferInfo* info,
                                               int64_t timeoutUs);
uint8_t*       AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx, size_t* outSize);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render);
AMediaFormat*  AMediaCodec_getOutputFormat(AMediaCodec* codec);
media_status_t AMediaCodec_setParameters(AMediaCodec* codec, const AMediaFormat* params);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIACODEC_H */
