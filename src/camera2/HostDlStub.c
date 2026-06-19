/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * HostDlStub.c — non-Halium stand-in for libhybris's Android dynamic linker.
 *
 * On a Halium device (the FuriPhone FLX1s) the symbols android_dlopen /
 * android_dlsym that Camera2NDKShim.c calls are provided by libhybris-common.
 * On a normal host (the Debian desktop used for fast build iteration) libhybris
 * does not exist, so these stubs let FuriCam still compile, link and run with
 * the Camera2 path simply inert: every Android-library lookup fails cleanly and
 * the higher-level CameraSession reports "Camera2 unavailable" rather than the
 * app failing to link.
 *
 * CMake compiles this file ONLY when find_library(hybris-common) fails (see
 * CMakeLists.txt); on the phone the real libhybris-common is linked instead, so
 * there is never a duplicate definition.
 *
 * Signatures mirror the declarations in <hybris/common/binding.h>.
 */

#include <stddef.h>

void *android_dlopen(const char *filename, int flag)
{
    (void)filename;
    (void)flag;
    return NULL;
}

void *android_dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    return NULL;
}

int android_dlclose(void *handle)
{
    (void)handle;
    return 0;
}

char *android_dlerror(void)
{
    return (char *)"libhybris not available on this host (Camera2 inert)";
}

int android_dladdr(const void *addr, void *info)
{
    (void)addr;
    (void)info;
    return 0;
}
