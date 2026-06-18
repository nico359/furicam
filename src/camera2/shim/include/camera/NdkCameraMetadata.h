/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCameraMetadata.h — read-only access to camera characteristics and
 * capture results (ACameraMetadata).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMETADATA_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMETADATA_H

#include <stdint.h>

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraMetadataTags.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque metadata container (characteristics or result). */
typedef struct ACameraMetadata ACameraMetadata;

/* The `type` field of ACameraMetadata_const_entry. */
enum {
    ACAMERA_TYPE_BYTE     = 0,
    ACAMERA_TYPE_INT32    = 1,
    ACAMERA_TYPE_FLOAT    = 2,
    ACAMERA_TYPE_INT64    = 3,
    ACAMERA_TYPE_DOUBLE   = 4,
    ACAMERA_TYPE_RATIONAL = 5,
    ACAMERA_NUM_TYPES     = 6,
};

typedef struct ACameraMetadata_rational {
    int32_t numerator;
    int32_t denominator;
} ACameraMetadata_rational;

typedef struct ACameraMetadata_const_entry {
    uint32_t tag;
    uint8_t  type;
    uint32_t count;
    union {
        const uint8_t*                  u8;
        const int32_t*                  i32;
        const float*                    f;
        const int64_t*                  i64;
        const double*                   d;
        const ACameraMetadata_rational* r;
    } data;
} ACameraMetadata_const_entry;

camera_status_t ACameraMetadata_getConstEntry(const ACameraMetadata* metadata, uint32_t tag,
                                              ACameraMetadata_const_entry* entry);
camera_status_t ACameraMetadata_getAllTags(const ACameraMetadata* metadata, int32_t* numEntries,
                                           const uint32_t** tags);
ACameraMetadata* ACameraMetadata_copy(const ACameraMetadata* src);
void             ACameraMetadata_free(ACameraMetadata* metadata);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMETADATA_H */
