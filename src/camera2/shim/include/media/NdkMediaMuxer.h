/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * media/NdkMediaMuxer.h — container (MP4 / WebM / 3GP) writer (AMediaMuxer).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 21+).
 */

#ifndef CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAMUXER_H
#define CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAMUXER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t */

#include "media/NdkMediaError.h"
#include "media/NdkMediaFormat.h"
#include "media/NdkMediaCodec.h"   /* AMediaCodecBufferInfo */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AMediaMuxer AMediaMuxer;

/* Output container format passed to AMediaMuxer_new. */
typedef enum {
    AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4    = 0,
    AMEDIAMUXER_OUTPUT_FORMAT_WEBM      = 1,
    AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP = 2,
} OutputFormat;

AMediaMuxer*   AMediaMuxer_new(int fd, OutputFormat format);
media_status_t AMediaMuxer_delete(AMediaMuxer* muxer);
ssize_t        AMediaMuxer_addTrack(AMediaMuxer* muxer, const AMediaFormat* format);
media_status_t AMediaMuxer_start(AMediaMuxer* muxer);
media_status_t AMediaMuxer_stop(AMediaMuxer* muxer);
media_status_t AMediaMuxer_writeSampleData(AMediaMuxer* muxer, size_t trackIdx,
                                           const uint8_t* data, const AMediaCodecBufferInfo* info);
media_status_t AMediaMuxer_setOrientationHint(AMediaMuxer* muxer, int degrees);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_MEDIA_NDKMEDIAMUXER_H */
