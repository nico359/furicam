/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * Camera2NDKHybrisInit.c — the one piece of libcamera2ndk-hybris that is NOT a
 * pure forwarding wrapper: starting the Android binder thread pool.
 *
 * This is the hard-won bit every Camera2-over-libhybris consumer needs and that
 * is easy to miss.  A glibc process reaching Android via libhybris can make
 * *outgoing* binder calls (opening the camera) but, unlike a real Android app,
 * runs no threads servicing *incoming* transactions.  Camera2 needs those: the
 * cameraserver calls back into this process to deliver buffers from the
 * AImageReader BufferQueue and to disconnect streams on close.  With no pool,
 * those callbacks fail (zero frames) and ACameraDevice_close can deadlock on
 * nested callbacks.  ProcessState::startThreadPool() also sets mThreadPoolStarted
 * so the kernel may spawn extra looper threads (BR_SPAWN_LOOPER) for re-entrant
 * transactions — a single hand-rolled joinThreadPool thread is not enough.
 *
 * ABI note: ProcessState::self() returns sp<ProcessState> by value.  Being
 * non-trivially copyable, the AArch64 ABI returns it indirectly (caller buffer,
 * address in x8).  We model that with an oversized POD struct return so the C
 * call matches, and read the first pointer (the ProcessState*).  The returned
 * sp's destructor is skipped, leaking one ref on a process-singleton — harmless.
 */

#include "camera2ndk_hybris.h"

#include <hybris/common/binding.h>

/* sp<ProcessState> is one pointer, but size it >16 bytes so the AArch64 ABI
 * returns it indirectly (matching the real sp<> by-value return). */
struct Camera2NDKHybrisSpHolder { void* p[4]; };
typedef struct Camera2NDKHybrisSpHolder (*Camera2NDKHybrisPsSelfFn)(void);
typedef void                            (*Camera2NDKHybrisPsStartFn)(void* thiz);

bool camera2ndk_hybris_start_threadpool(void)
{
    static int started = 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return true;   /* already started (idempotent) */

    void* libbinder = android_dlopen("libbinder.so", 1 /* RTLD_LAZY */);
    if (!libbinder) {
        __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);   /* allow a retry */
        return false;   /* non-Halium host (stub) or libbinder unavailable */
    }

    Camera2NDKHybrisPsSelfFn psSelf = (Camera2NDKHybrisPsSelfFn)
        android_dlsym(libbinder, "_ZN7android12ProcessState4selfEv");
    Camera2NDKHybrisPsStartFn psStart = (Camera2NDKHybrisPsStartFn)
        android_dlsym(libbinder, "_ZN7android12ProcessState15startThreadPoolEv");
    if (!psSelf || !psStart) {
        __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
        return false;
    }

    struct Camera2NDKHybrisSpHolder h = psSelf();
    if (h.p[0])
        psStart(h.p[0]);
    return true;
}
