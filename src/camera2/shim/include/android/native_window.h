/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * android/native_window.h — the subset of the Android NDK ANativeWindow API
 * that libcamera2ndk-hybris forwards to the device's real libnativewindow.so.
 *
 * Only the handle type and the reference-count lifetime calls are declared:
 * Camera2 hands ANativeWindow* across the AImageReader / capture-session
 * boundary and the renderer holds references while importing buffers.  The
 * spelling matches the public, stable Android NDK ABI (API level 24+); these
 * are interface descriptions, not original expression.
 */

#ifndef CAMERA2NDK_HYBRIS_ANDROID_NATIVE_WINDOW_H
#define CAMERA2NDK_HYBRIS_ANDROID_NATIVE_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque buffer queue producer/consumer handle.  We only ever hold pointers. */
typedef struct ANativeWindow ANativeWindow;

void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_ANDROID_NATIVE_WINDOW_H */
