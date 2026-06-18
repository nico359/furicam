# camera2ndk-hybris

**The Android Camera2 / Media NDK, callable from a glibc Linux process via
libhybris.**

On a Halium-based Linux phone (Droidian, FuriOS, Ubuntu Touch, …) the modern
Android camera lives in vendor blobs behind `libcamera2ndk.so`,
`libmediandk.so` and `libnativewindow.so` — but those are linked against
Bionic, so a normal glibc app can't call them. libhybris already bridges
Bionic↔glibc for EGL, GLES, OpenSL and the legacy Camera **HAL1**, but it ships
**no Camera2 NDK** wrapper. This library is that missing layer.

It is a thin forwarding shim: every public Camera2 / Media NDK symbol is
resolved through libhybris's Android linker and forwarded to the device's real
`.so`. You write ordinary Android NDK code; it runs on Linux.

```c
#include <camera/NdkCameraManager.h>
#include <camera2ndk_hybris.h>

camera2ndk_hybris_start_threadpool();          /* once, before opening a camera */
ACameraManager* mgr = ACameraManager_create(); /* …exactly as the NDK docs say */
```

## Why a separate library

Every Camera2-on-Halium app of this generation needs this exact piece. Without
it, each project reinvents the Bionic→glibc forwarding (and rediscovers the
binder-threadpool gotcha below). Shipping it once means the next person just
links it.

## What's in it

* `Camera2NDKShim.c` — the forwarding wrappers (`libcamera2ndk`, `libmediandk`,
  `libnativewindow`). Written in C: the libhybris binding macros assign a
  `void*` from `android_dlsym()` to a function pointer, which is valid C but
  ill-formed C++.
* `Camera2NDKHybrisInit.c` — `camera2ndk_hybris_start_threadpool()`.
* `include/` — standard Android NDK headers (`camera/Ndk*.h`, `media/Ndk*.h`,
  `android/native_window.h`, `android/hardware_buffer.h`), a convenience umbrella
  `Camera2NDK.h`, and the libhybris-specific `camera2ndk_hybris.h`. Only the
  subset currently forwarded is declared; the spellings/values match the public
  NDK ABI (API 24+) so caller code is the same as on Android. Extending coverage
  is additive — add the prototype to a header and one wrapper line to the shim.

## The binder-threadpool gotcha

A glibc process reaching Android via libhybris can make *outgoing* binder calls
(opening the camera) but, unlike a real Android app, runs **no threads
servicing incoming transactions**. Camera2 needs those — the cameraserver calls
back to deliver buffers and to tear down streams. Without a thread pool you get
**zero frames** and `ACameraDevice_close()` can deadlock.

`camera2ndk_hybris_start_threadpool()` fixes this: it `android_dlopen`s
libbinder and calls `ProcessState::self()->startThreadPool()`. Call it once at
startup, before `ACameraManager_openCamera()`. This is the single most important
thing a consumer must do that the Android docs never mention.

## Build & install

Target platform is a Halium device (aarch64 + libhybris); needs
`libhybris-common-dev`.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --build build --target install
```

Then consumers use pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs camera2ndk-hybris)
```

Headers install under `${includedir}/camera2ndk-hybris/`, and the `.pc` adds
that to the include path — so `#include <camera/NdkCameraManager.h>` resolves
without the library claiming the global `camera/` include namespace.

On any non-Halium host (a dev desktop / CI) the build falls back to a no-op stub
(`HostDlStub.c` + a vendored copy of libhybris's `binding.h`), so it still
compiles and links. There, `camera2ndk_hybris_start_threadpool()` returns
`false` — use that as your gate: if it fails you are not on a working stack, so
don't call the forwarding wrappers (each one would `android_dlopen` an absent
`.so` and dereference an unresolved pointer). The stub exists for compile/link
checking, not for running camera code off-device.

## License

Apache-2.0 (see `LICENSE`), matching libhybris. The vendored
`host/hybris/common/{binding.h,floating_point_abi.h}` are Apache-2.0 from
libhybris and retain their original headers.
