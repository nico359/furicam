/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * android/hardware_buffer.h — the subset of the Android NDK AHardwareBuffer API
 * that libcamera2ndk-hybris forwards to the device's real libnativewindow.so.
 *
 * AHardwareBuffer is how a camera/encoder buffer is referenced and described
 * when importing it into EGL (the zero-copy preview path).  Only the handle,
 * the descriptor struct, the usage-flag subset we set, and the lifetime /
 * describe calls are declared.  Spelling matches the public Android NDK ABI
 * (API level 26+).
 */

#ifndef CAMERA2NDK_HYBRIS_ANDROID_HARDWARE_BUFFER_H
#define CAMERA2NDK_HYBRIS_ANDROID_HARDWARE_BUFFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque GraphicBuffer handle.  We only ever hold pointers. */
typedef struct AHardwareBuffer AHardwareBuffer;

/* Buffer usage flags (subset).  We set these when requesting GPU-sampleable
 * preview buffers or video-encoder input. */
enum {
    AHARDWAREBUFFER_USAGE_CPU_READ_NEVER     = 0UL,
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY    = 2UL,
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN     = 3UL,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER    = 0UL << 4,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY   = 2UL << 4,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN    = 3UL << 4,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE  = 1UL << 8,
    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT   = 1UL << 9,
    AHARDWAREBUFFER_USAGE_VIDEO_ENCODE       = 1UL << 16,
};

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_ANDROID_HARDWARE_BUFFER_H */
