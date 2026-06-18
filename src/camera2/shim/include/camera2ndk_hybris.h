/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera2ndk_hybris.h — the libhybris-specific glue that the standard Android
 * NDK headers do not have.  Everything else in this package is a faithful
 * forwarding of the public NDK; this header is the one place where running the
 * NDK over libhybris (rather than on Android) requires the caller to do
 * something extra.
 */

#ifndef CAMERA2NDK_HYBRIS_H
#define CAMERA2NDK_HYBRIS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the Android binder thread pool for this process.
 *
 * Why this is needed: a glibc process that reaches Android via libhybris can
 * make *outgoing* binder calls (that is how the camera is opened) but, unlike a
 * real Android app, has no threads servicing *incoming* transactions.  Camera2
 * needs the latter — the Android cameraserver calls back into this process to
 * deliver buffers from the AImageReader's BufferQueue and to disconnect streams
 * on close.  Without a pool those callbacks fail (zero frames delivered, and
 * ACameraDevice_close can deadlock waiting on nested callbacks).
 *
 * Call this once at startup, before ACameraManager_openCamera().  It android_-
 * dlopen()s libbinder and invokes ProcessState::self()->startThreadPool(); the
 * kernel may then spawn extra looper threads for re-entrant transactions.
 * Idempotent.
 *
 * Returns true if the pool was started (or already running), false if libbinder
 * or the ProcessState symbols could not be resolved (e.g. a non-Halium host).
 */
bool camera2ndk_hybris_start_threadpool(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_H */
