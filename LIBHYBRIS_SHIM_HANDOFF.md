# Handoff — `camera2ndk-hybris`: the standalone Camera2/Media NDK→libhybris shim

> **Direction change from the original plan.** The first draft of this file aimed
> to contribute the shim *into* the libhybris source tree (a `hybris/camera2/`
> autotools module, PR upstream). We reframed: **libhybris is already shipped, so
> we *consume* it.** The deliverable is a standalone shared library,
> **`libcamera2ndk-hybris.so`**, that links `-lhybris-common` and lets any
> glibc-linked Halium app call the Android Camera2 / Media NDK exactly as
> documented. Lower friction, and it's the missing layer every Camera2-on-Halium
> app needs (MotionCam-UT reinvented it; so did we).

Durable background is in auto-loaded memory: `furicam2-upstream-roadmap`,
`furicam2-build-packaging`, `project-furicam2-camera2-plan`.

## DONE this session — the package exists under `src/camera2/shim/`

Built and **compile/link/run-checked on the desktop** (host-stub path):

```
src/camera2/shim/
  Camera2NDKShim.c            forwarding wrappers (camera2ndk + mediandk + nativewindow), Apache-2.0
  Camera2NDKHybrisInit.c      camera2ndk_hybris_start_threadpool() — the binder-pool fix, ported from the engine
  HostDlStub.c                non-Halium no-op linker stub (compile/link only), Apache-2.0
  host/hybris/common/*        vendored binding.h + floating_point_abi.h (Apache-2.0 from libhybris) for host builds
  include/
    camera/Ndk*.h             7 standard NDK headers (Manager/Device/CaptureSession/CaptureRequest/Metadata/MetadataTags/Error)
    media/Ndk*.h              6 standard NDK headers (MediaCodec/MediaFormat/MediaMuxer/Image/ImageReader/MediaError)
    android/{native_window,hardware_buffer}.h
    Camera2NDK.h             convenience umbrella (includes them all)
    camera2ndk_hybris.h      the one libhybris-specific call: start_threadpool()
  CMakeLists.txt              builds SHARED libcamera2ndk-hybris.so; Halium→link hybris-common, else→HostDlStub
  camera2ndk-hybris.pc.in     pkg-config; Cflags add -I${includedir}/camera2ndk-hybris
  README.md, LICENSE (Apache-2.0)
```

**Header strategy = the "hybrid" (chosen deliberately):** standard NDK file names
(`#include <camera/NdkCameraManager.h>`, verbatim Android) but installed under a
**private prefix** `${includedir}/camera2ndk-hybris/`, exposed via the `.pc`'s
`-I`. Drop-in source compatibility with zero global-namespace land-grab. Only the
subset we forward is declared; extending coverage is additive (one prototype +
one wrapper line). Verified: a consumer compiles `#include <camera/NdkCameraManager.h>`
through `pkg-config --cflags camera2ndk-hybris` and links the `.so`.

**The threadpool fix is now part of the package.** `camera2ndk_hybris_start_threadpool()`
android_dlopens libbinder and calls `ProcessState::self()->startThreadPool()` — the
single non-obvious thing every consumer must do (no pool ⇒ zero frames, close
deadlocks). Call once before `ACameraManager_openCamera()`.

**furicam2 now consumes it.** `src/camera2/CMakeLists.txt` does `add_subdirectory(shim)`
and links `camera2ndk-hybris` PUBLIC; the engine's old in-tree `Camera2NDKShim.c` /
`HostDlStub.c` / `host/` / host-stub define / hybris-common link all moved into the
shim and now reach the engine transitively. Engine `.cpp` files were **not** changed
(the shim re-defines `FURICAM2_CAMERA2_HOST_STUB` for back-compat, plus the native
`CAMERA2NDK_HYBRIS_HOST_STUB`). Desktop build of `camera2ndk-hybris` + `furicam2_camera`
+ `camera2_probe` all pass; probe links `libcamera2ndk-hybris.so.0` and reports the
host stub correctly.

## DONE on the FuriPhone FLX1s (`ssh flx`, 2026-06-18)

- **Standalone shim builds on-device against the REAL libhybris** — configures as
  "Halium host (/usr/lib/aarch64-linux-gnu/libhybris-common.so)", not the stub.
- **It works as a self-standing library:** a tiny consumer (zero furicam2 code)
  `-lcamera2ndk-hybris -lhybris-common`, called `camera2ndk_hybris_start_threadpool()`
  (→ true) + `ACameraManager_create()` + `ACameraManager_getCameraIdList()` and
  **enumerated the phone's 3 real cameras** (ids 0/1/2).
- **furicam2 rebuilt + installed** via `build-on-phone.sh` with the shim as a
  separate `.so`; `ldd /usr/bin/furicam2` resolves `libcamera2ndk-hybris.so.0`
  (and `libhybris-common.so.1`), zero unresolved.
- **Packaging fixed:** `build-on-phone.sh` now passes `-DCMAKE_INSTALL_PREFIX=/usr`
  (shim `.so` → `/usr/lib/<triplet>`) + runs `ldconfig`; `package-deb.sh` bundles
  the `.so` + soname symlink and ldconfig's in postinst. The `.deb` now contains
  `usr/lib/aarch64-linux-gnu/libcamera2ndk-hybris.so.0{,.1.0}`.

## TODO next — in priority order

1. **Tap-test the furicam2 GUI** on the phone (preview/photo/video) — only the
   on-screen app launch is unverified; the shim + linkage are proven.
2. **(Optional) consolidate** the engine's private `startBinderThreadPoolOnce()`
   (CameraSession.cpp ~L45) onto the shim's `camera2ndk_hybris_start_threadpool()`
   to kill the duplicate — left as-is (proven on-device; verify the swap first).
3. **Extract to its own repo:** `git filter-repo --subdirectory-filter
   src/camera2/shim` → `libcamera2ndk-hybris`. The dir is already self-contained
   (own CMakeLists/LICENSE/README/.pc; only external dep is libhybris-common-dev).
   Then split furicam2's bundled `.so` out into its own `libcamera2ndk-hybris`
   `.deb` + a `Depends:` (replacing the in-package bundle).
4. **Add a tiny Qt-free example/test** to the shim repo (the on-phone smoke test
   — enumerate/open/stream — is the seed; today `camera2_probe` links the whole
   furicam2 engine).
5. **Publish**, increasing reach: own GitHub repo → FuriLabs (they ship FuriOS) →
   libhybris upstream (sibling to its EGL/GLES/OpenSL shims) → maybe obsolete
   sailfishos/droidmedia's stubbed Camera2 (#112).

## Notes / gotchas
- Shim is **C** on purpose (binding.h assigns `void*`→fnptr, ill-formed C++).
- `port camera2_probe.cpp` into a tiny Qt-free example/test for the standalone
  repo (today it links the whole furicam2 engine; the shim repo wants a minimal
  enumerate/open/stream demo).
- Nothing here is committed yet (`git status` in furicam2-src).
