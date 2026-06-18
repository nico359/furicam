# FuriCam2 Path C — Android Camera2 native development kit (NDK) helper

## Goal

Replace the current FuriCam2 recording pipeline (which captures preview frames via `droidcamsrc.vfsrc` and software-encodes with `x264enc`) with a C++ component that talks to Android Camera2 NDK directly via `libhybris`. Gives us:

- Hardware (HW) H.264 encoding via `AMediaCodec` → MPEG-4 Part 14 (MP4) muxing via `AMediaMuxer` — proper 30 frames per second (fps) at 1080p, real 4K30 within reach
- Manual exposure-time, manual sensor gain, manual focus distance — controls Camera1 cannot expose
- Sustained recording without thermal throttling the CPU
- Higher sensor native resolution (5184 × 3880 vs Camera1's 5152 × 3864)
- Better dynamic range (multi-frame composition is possible if we want it later)

Empirically proven viable on the FuriPhone FLX1s via MotionCam-UT (https://github.com/NotKit/motioncam-ut) — see [[project-furicam2-camera-investigation]].

## Architecture

```
┌─ QML Camera.qml (existing) ────────────────────────────────────────────┐
│                                                                        │
│  Camera2Bridge {                  ← new Qt5 widget (QQuickFramebufferObject)
│      id: camera                                                        │
│      Component.onCompleted: camera.startCamera()                       │
│  }                                                                     │
│  // ...                                                                │
│  camera.startRecording(outputPath)                                     │
│  camera.capturePhoto(path, settingsJson)                               │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
                       │
                       │ Q_INVOKABLE
                       ▼
┌─ Camera2Bridge.cpp (Qt5 ↔ NDK glue) ────────────────────────────────────┐
│                                                                        │
│  CameraSession  →  AImageReader (preview, AIMAGE_FORMAT_PRIVATE)       │
│                 →  AImageReader (JPEG capture)                         │
│                 →  AMediaCodec  (H.264 encoder, AIMAGE_FORMAT_PRIVATE) │
│                 →  AMediaMuxer  (MP4 output)                           │
│                                                                        │
│  Renderer (QQuickFramebufferObject::Renderer):                         │
│      AImage → AHardwareBuffer → EGL_NATIVE_BUFFER_ANDROID              │
│              → EGLImage → GL_TEXTURE_EXTERNAL_OES → render quad        │
│                                                                        │
│  Audio: keep existing GStreamer chain (autoaudiosrc → voaacenc),       │
│         pump Advanced Audio Coding (AAC) sample buffers into           │
│         AMediaMuxer alongside video.                                   │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
                       │
                       │ symbol calls
                       ▼
┌─ Camera2NDKShim.cpp (libhybris bridge) ─────────────────────────────────┐
│                                                                        │
│  At startup, hybris_dlopen("libcamera2ndk.so") + libmediandk.so,       │
│  populate vtable of function pointers, and provide C-linkage           │
│  wrapper functions for every NDK symbol the Bridge calls.              │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

## Source layout

```
src/camera2/
├── CAMERA2_PLAN.md              # this file
├── Camera2Bridge.h              # Q_INVOKABLE API surface (Qt5 widget)
├── Camera2Bridge.cpp            # bridge implementation
├── Camera2BridgeRenderer.cpp    # the preview rendering pipeline (called by Qt render thread)
├── CameraSession.h              # Camera2 NDK lifecycle: open camera, create session, attach outputs
├── CameraSession.cpp
├── VideoEncoder.h               # AMediaCodec + AMediaMuxer wrapper for H.264 → MP4
├── VideoEncoder.cpp
├── HybrisLoader.h               # NDK vtable definitions
├── HybrisLoader.cpp             # hybris_dlopen + dlsym population
└── Camera2NDKShim.cpp           # C-linkage shim — calls into the vtable
```

## Milestones (incremental, each independently testable)

### M0 — Build infrastructure (~2 hr)

- Update `CMakeLists.txt`:
  - `find_library(HYBRIS_COMMON_LIB hybris-common REQUIRED)`
  - `pkg_check_modules(EGL REQUIRED egl)` and `glesv2`
  - Conditional compile of `src/camera2/` behind `-DFURICAM2_CAMERA2=ON` so the legacy build still works
- Verify on phone that `libhybris-dev` is reachable
- Empty `Camera2Bridge` that just registers itself as a Qt Modeling Language (QML) type

### M1 — libhybris shim — ✅ DONE (2026-06-16)

**SOLVED via libhybris-dev's Apache 2.0 macros.** `<hybris/common/binding.h>` provides `HYBRIS_IMPLEMENT_FUNCTION0` through `_FUNCTION19` (20 macros, one for each function arity from 0 to 19 arguments). Each NDK function we want to call collapses to a single declarative line:

```c
HYBRIS_LIBRARY_INITIALIZE(camera2ndk, "libcamera2ndk.so")
HYBRIS_IMPLEMENT_FUNCTION0(camera2ndk, ACameraManager*, ACameraManager_create)
HYBRIS_IMPLEMENT_FUNCTION4(camera2ndk, camera_status_t, ACameraManager_openCamera,
                           ACameraManager*, const char*,
                           ACameraDevice_StateCallbacks*, ACameraDevice**)
// ...etc
```

The macro expands into a function with the exact NDK symbol name that lazily `android_dlopen`s the Android library on first call and forwards. The rest of our code calls these wrappers as if they were the real NDK symbols.

**What actually shipped (delivered + verified on-device):**

- `src/camera2/Camera2NDKShim.c` — **87** `HYBRIS_IMPLEMENT_FUNCTION_*` forwarders: 35 for
  `libcamera2ndk.so`, 47 for `libmediandk.so` (`AImageReader_*`, `AImage_*`, `AMediaCodec_*`,
  `AMediaFormat_*`, `AMediaMuxer_*`), 5 for `libnativewindow.so` (`AHardwareBuffer_*` /
  `ANativeWindow_*`, for the M3 EGL import path).
- `src/camera2/Camera2NDK.h` — **the shim is C, not C++ (see below), and libhybris-dev does NOT
  ship the Android NDK headers** (it ships only the legacy Camera1 compatibility headers under
  `/usr/include/hybris/{camera,media}/`). So this file declares the subset of NDK types, enums,
  format/usage constants, callback structs and `extern "C"` prototypes that the shim forwards and
  that M2+ code calls. The metadata tag value constants (`ACAMERA_*`) are deferred to M2.

**Why the shim is `.c` and not `.cpp`:** `binding.h`'s `HYBRIS_DLSYSM` resolves symbols with
`*(fptr) = (void *) android_dlsym(...)` — a `void*`→function-pointer assignment that is valid C but
ill-formed C++. libhybris's own wrappers (`egl.c`, `glesv2.c`, …) are C for exactly this reason. The
top-level `CMakeLists.txt` now uses `LANGUAGES C CXX`, lists `Camera2NDKShim.c`, and adds the
Camera2 sources/headers via an existence filter so each milestone configures before the later
files exist (and so AUTOMOC does not moc `Camera2Bridge.h` before its `.cpp` exists).

**Verification (on the FLX1s phone, gcc 15.2 aarch64):** compiles clean with `-Wall -Wextra`;
links as a shared object with `-Wl,--no-undefined` against `libhybris-common`, leaving only
`android_dlopen`/`android_dlsym` (+ standard C-runtime weak symbols) undefined; all 87 symbols
export with the exact NDK names. A minimal CMake harness mirroring the real Camera2 wiring
(`find_library(hybris-common)`, `pkg egl`/`glesv2`) builds the shim on-device. **Not yet run:** the
full-app `cmake -DFURICAM2_CAMERA2=ON && make` — the phone is missing app-level dev packages
(`Qt5Multimedia`, OpenCV, ZXing), unrelated to Camera2; the Camera2 deps themselves all resolve.

Reference for usage patterns: `libcamera-hybris` (the Camera1 wrapper shipped by libhybris-dev) uses the same macros — Apache 2.0, free to read.

### M2 — Open camera, log capabilities — ✅ DONE (2026-06-16)

Instead of routing through the GUI app (which needs Qt5Multimedia/OpenCV/ZXing, missing on the
phone), M2 shipped a **Qt-free `CameraSession` + a standalone `camera2_probe` CLI** so we can prove
reachability on-device with just gcc/g++ + libhybris-common:

- `src/camera2/CameraSession.{h,cpp}` — create `ACameraManager`, enumerate, read characteristics,
  open/close a device, decode + dump capabilities (curated summary **and** a generic
  `getAllTags` walk). No Qt; logs via a `LogFn`. Reused by `Camera2Bridge` later.
- `src/camera2/tools/camera2_probe.cpp` — `main()` that enumerates, dumps each camera, opens the
  back camera, and (`--all`) dumps every metadata tag. Built as its own CMake target (no Qt).
- `Camera2NDK.h` — added the `ACAMERA_*` metadata tag + enum-value constants (authoritative
  values transcribed from AOSP `NdkCameraMetadataTags.h`).
- Full device dump saved at `src/camera2/FLX1S_CAMERA2_CAPS.txt` (reference for M3/M5/M7 sizes).

**On-device result (the Path C payoff — Camera1 could expose none of this):**

| cam | facing | hw level | sensor | manual sensor | ISO | exposure | PRIVATE/YUV max | JPEG max |
|----|--------|----------|--------|---------------|-----|----------|-----------------|----------|
| 0  | back   | **LEVEL_3** | 5184×3880 | **YES** | 100–2400 | 0.1–400 ms | 5184×3880 | 5152×3864 |
| 1  | front  | **LEVEL_3** | 4176×3088 | YES | 100–2400 | 0.1–400 ms | 4208×3120 | 4208×3120 |
| 2  | back   | LIMITED  | 1600×1200 | YES | 100–2400 | 0.1–400 ms | 1600×1200 | 1600×1200 |

- Camera 0 reports **hardware level 3** (the highest Camera2 tier) with `manual_sensor`,
  `manual_post_processing`, `raw`, `burst_capture`, `read_sensor_settings`. Manual exposure-time
  and sensor gain — impossible on Camera1 here — are fully available.
- Target FPS ranges include **[30,30] and [5,30]** → 30 fps video is supported.
- Full-resolution `PRIVATE` (0x22) and `YUV_420_888` (0x23) outputs are available → the zero-copy
  camera→encoder path for M5 is viable. **Opening camera 0 succeeded** (then closed cleanly).

**Verification:** desktop `cmake -DFURICAM2_CAMERA2=ON && make` builds `camera2_probe` + `furicam2`
clean (probe reports inert host-stub); phone build (`gcc` shim as C + `g++` C++ + `-lhybris-common`)
runs and produces the table above. M2's GUI wiring (`Camera2Bridge::startCamera()`) is deferred to
M8 — the probe is the milestone proof.

### M3 — Live preview

Split into **M3a (camera streaming, headless)** and **M3b (GL preview renderer + Qt)**, because the
existential risk was whether the capture session delivers frames *at all* — the same thing that
killed the Camera1 recording path. Prove that headless first, then plumb pixels to the screen.

#### M3a — Camera2 preview streaming — ✅ DONE (2026-06-16), and it WORKS

`CameraSession::startPreview()` now creates an `AImageReader`, wires it as the sole capture-session
output, builds a repeating `TEMPLATE_PREVIEW` request, and the image listener drains + counts every
frame. `camera2_probe --stream [secs]` exercises it on the phone.

**Result: a steady ~30 fps of real 1280×720 frames** (e.g. 142 frames in 5 s; +30/s after the first
ramp), with clean session/device close. Camera1 recording delivered **zero** frames here — Camera2
streaming delivers a full 30 fps. Path C's core risk is retired.

**THE KEY FINDING — a bare libhybris process needs an Android binder THREAD POOL.**
Symptoms before the fix: the capture session went `active`, the MTK HAL configured the stream and
ran the sensor (AE/AF), but **zero buffers** reached our `AImageReader` (`acquireNextImage` →
`AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE`). logcat showed the HAL spinning on `dequeueSwBuf` and
`cameraserver` logging `Can't dequeue next output buffer: Broken pipe (-32)`.

Cause: our process makes *outgoing* binder calls fine (that is how we open the camera), but
`cameraserver` must call *back into us* to dequeue buffers from the `AImageReader`'s BufferQueue —
and a bare libhybris process has **no thread servicing incoming binder transactions**. A normal
Android app gets this pool from its runtime; we must start it ourselves.

Fix (`startBinderThreadPoolOnce()` in `CameraSession.cpp`): call
`ProcessState::self()->startThreadPool()` from libbinder.so via the shim's `android_dlopen/dlsym`.
Two subtleties:
1. **Use `startThreadPool()`, not a single hand-rolled `joinThreadPool` thread.** One thread streams
   frames but **deadlocks on close** — `ACameraDevice_close` triggers *re-entrant* callbacks from
   cameraserver, and `startThreadPool` sets `mThreadPoolStarted` so the kernel can spawn extra
   threads (`BR_SPAWN_LOOPER`); a lone thread cannot.
2. **ABI:** `ProcessState::self()` returns `sp<ProcessState>` by value; being non-trivially copyable
   it is returned indirectly (x8 on arm64). We call the mangled symbol with an oversized POD-struct
   return type to match, and read the first pointer.

Other gotchas fixed: pin `ACAMERA_CONTROL_AE_TARGET_FPS_RANGE = [30,30]` or the HAL drops to ~10 fps
in low light; close the session+device **before** freeing the `AImageReader` (else "native window
died from under us"); the probe `std::_Exit()`s past the (intentionally blocking) binder pool thread.

#### M3b — GL preview renderer + Qt — ✅ DONE (2026-06-16): live preview on screen

`Camera2Bridge` (QQuickFramebufferObject) + an inline `Camera2PreviewRenderer` do the zero-copy GPU
path on Qt's scene-graph thread:
`AImageReader` PRIVATE (0x22, `GPU_SAMPLED_IMAGE` usage) → `AImage_getHardwareBuffer` →
`eglGetNativeClientBufferANDROID` → `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` →
`glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)` → textured quad (samplerExternalOES; the GPU
does YUV→RGB). `CameraSession` gained render-pull mode (a frame callback that schedules a repaint
instead of draining; the renderer `acquireLatestImage`s on the render thread).

**Verified on the phone: a live back-camera preview renders fullscreen and ran stably** (closed by
the user, no crash). Confirmed on-device first: all required extensions present
(`EGL_ANDROID_image_native_buffer`, `EGL_ANDROID_get_native_client_buffer`, `GL_OES_EGL_image_external`;
Mali-G68, hybris EGL). Orientation looked correct on the first rotation guess
(`displayRotation = (sensorOrientation − deviceRotation) mod 360`, applied as a texcoord rotation).

**Built as a minimal Qt-Quick harness, NOT the full app** (`src/camera2/tools/camera2_preview.cpp` +
`tools/preview/CMakeLists.txt`): it needs only Qt Quick/Qml/Gui (the GLES Qt already installed) +
EGL/GLES + libhybris — **not** Qt5Multimedia/OpenCV/ZXing. The phone runs the **GLES Qt variant**
(`qtbase5-gles-dev`); `qtmultimedia5-dev` pulls `qtbase5-dev`, which conflicts and would risk the
phosh UI — so building the *whole* app on-device (M8) needs either a GLES multimedia package or
dropping Qt5Multimedia, NOT a forced install. App-grid launcher:
`~/.local/share/applications/camera2-preview.desktop`.

Remaining polish (minor): teardown race if stop/start races the render thread; front-camera mirroring;
aspect-fit (currently fills, may stretch slightly). None block M4/M5.

### M4 — JPEG photo capture — ✅ DONE (2026-06-16)

`startPreview(withStill=true)` adds a full-resolution `AIMAGE_FORMAT_JPEG` AImageReader as a second
session output. `CameraSession::capturePhoto(path)` submits a one-shot `TEMPLATE_STILL_CAPTURE`
request targeting it (with `ACAMERA_JPEG_ORIENTATION` from the sensor and `ACAMERA_JPEG_QUALITY`);
the JPEG listener trims the buffer at the End-Of-Image marker and writes the file, then fires the
photo callback. Wired into `Camera2Bridge::capturePhoto` (→ `photoSaved` signal).

**Verified on the phone (`camera2_probe --photo`): a real 5152×3864 (21 MP) hardware JPEG, valid EXIF**
(FuriLabs FLX1s / MediaTek camera), orientation tag applied (saved 3864×5152 upright).

### M5 — Hardware H.264 video — ✅ DONE (2026-06-16): the headline feature works

`VideoEncoder` wraps `AMediaCodec_createEncoderByType("video/avc")` in **surface-input** mode
(`COLOR_FormatSurface`, `AMediaCodec_createInputSurface`) + `AMediaMuxer` (MP4). `CameraSession::
startRecording(path,w,h,fps,bitrate)` builds a dedicated capture session whose only output target is
the encoder's input surface — **camera frames flow straight into the H.264 encoder: no CPU, no GL, no
copies.** A drain thread pumps encoded buffers `AMediaCodec_dequeueOutputBuffer → AMediaMuxer_
writeSampleData`; the track is added on `INFO_OUTPUT_FORMAT_CHANGED`. `stopRecording()` stops the
camera, `signalEndOfInputStream`, drains the tail, finalizes the MP4. `AMediaMuxer_setOrientationHint`
makes it play upright. Wired into `Camera2Bridge::startRecording/stopRecording`.

**Verified on the phone (`camera2_probe --record 5`): a valid 11 MB MP4, `ffprobe` → `codec_name=h264`,
1920×1080, ~4.6 s.** This is exactly what the Camera1/gst-droid path could NEVER do on this device
(it delivered zero recordable frames — see [[project-furicam2-camera-investigation]]). Path C now
delivers true hardware H.264.

Notes: actual fps tracks AE (≈22 fps in low light, 30 in good light — pin a tighter range or lower
the min if needed); recording uses a record-only session (simultaneous on-screen preview + record is
an M8 task; the Bridge resumes preview after `stopRecording`).

**4K30 also works** (`camera2_probe --record N out.mp4 --size 3840x2160 --bitrate 50000000`): a valid
3840×2160 H.264 MP4 is produced. The HW encoder (`C2MtkVenc`) accepts 4K with no back-pressure and AE
targets 30 fps (`ae_mgr MaxFps:30`); the achieved rate is purely **exposure-limited** — ~10 fps in dim
light (the 4K full-res readout bins fewer pixels than 1080p, so AE needs a longer exposure), expected
to approach 30 fps in good light. `--size`/`--bitrate` are now probe options.

### M6 — Audio in MP4 — ✅ DONE (2026-06-17): AAC track muxed, verified on-device

`AudioEncoder` (new, GStreamer): `autoaudiosrc → audioconvert → audioresample → (avenc_aac|voaacenc)
→ aacparse(raw) → appsink`. A pull thread drains the appsink; the first sample's caps yield the AAC
AudioSpecificConfig (`codec_data`, with a 2-byte ASC fallback) used as `csd-0` to add the muxer's audio
track; every sample is handed to `VideoEncoder` via callbacks.

`VideoEncoder` now owns the muxer for **two** tracks. Because `AMediaMuxer` requires all tracks before
`AMediaMuxer_start()`, muxer start is **gated** until the video track exists AND (audio not expected OR
the audio track is added). All track adds + sample writes are serialized under `muxerMutex_`, and each
track's PTS is **normalized to start at zero** so the streams stay in sync. `CameraSession::startRecording`
starts mic capture **before** the camera record session, so the AAC track is added before the first video
frame and no leading video frames are dropped; if audio fails to start it `cancelAudioExpectation()`s and
records **video-only**. New NDK plumbing: `AMediaFormat_setBuffer` (decl + hybris shim) for `csd-0`.

Build: `AudioEncoder.cpp` is always compiled; the GStreamer path is enabled only when `gstreamer-app-1.0`
is found (`FURICAM2_HAVE_AUDIO`), else it compiles as a stub (video-only) so the engine still builds on a
bare host. Added `libgstreamer-plugins-base1.0-dev` (Build-Depends) and `gstreamer1.0-libav` (Depends).

**Verified on the phone** (`camera2_probe --record 5`, built via a Qt-free standalone superbuild since the
phone lacks Qt5Multimedia dev): the MP4 contains **AAC 48 kHz mono (stream 0) + H.264 1920×1080 (stream 1)**,
both `start_time=0.000000`; audio 4.39 s (206 frames) bracketed inside 4.65 s video — zero-based and in sync.

### M7 — Manual controls — ✅ DONE (2026-06-17): backend, verified

`CameraSession` holds control state and an `applyControls(req)` / `applyControlsToActive()` pair that
writes the state into whichever repeating request is active (preview or recording) and re-submits it.
Setters: `setAutoExposure`, `setManualExposure(iso, exposureNs)` (AE_MODE_OFF + SENSOR_EXPOSURE_TIME +
SENSOR_SENSITIVITY), `setExposureCompensation`, `setAeLock`, `setAwbLock`, `setAwbMode`, `setAfMode`,
`setZoomRatio` (CONTROL_ZOOM_RATIO 1–4×), `setTorch` (FLASH_MODE_TORCH), `setFocusPoint` (AF/AE regions
from the active array + one-shot AF_TRIGGER). All `ACAMERA_CONTROL_*` tag + value constants added to
`Camera2NDK.h` (authoritative AOSP values).

`Camera2Bridge` exposes them with the **existing app's method names** so the QML can drive Camera2
unchanged: `setWhiteBalanceMode(0..4)` (maps Auto/Daylight/Cloudy/Fluorescent/Incandescent →
`ACAMERA_CONTROL_AWB_MODE_*`), `setZoom`, `maxZoom`, plus the declared `setManualExposure`,
`setExposureCompensation`, `setAELock/AWBLock/FocusLock/AutoFocus/FocusPoint/Torch`.

**Verified headless (`camera2_probe --stream --exposure`):** manual exposure drives the sensor exactly
— **8 ms → ~30 fps, 100 ms → exactly 10 fps** (100 ms = 1/10 s), proving `AE_MODE_OFF` +
`SENSOR_EXPOSURE_TIME` are applied. The visual controls (white balance, 1–4× zoom, torch, AE lock,
tap-to-focus) are exposed in the `camera2_preview` harness as on-screen buttons for live confirmation.

### M8 — Wire into existing FuriCam2 (next)

Per the decision (2026-06-17): treat `Camera.qml` as the existing abstraction seam — reimplement its
wrapper functions (`setWhiteBalanceMode`, `handleSetZoom`, `handleCameraTakeShot`, focus/AE-lock, …)
against `Camera2Bridge`, and replace `VideoOutput { source: camera }` with a `Camera2Bridge` item, so
the UI in `main.qml` barely changes. Do NOT mimic the QtMultimedia `QCamera` shape (it would force CPU
frames / lose the zero-copy GL preview and keep the Qt5Multimedia dependency). Dropping QtMultimedia is
also what resolves the GLES-Qt build conflict so the full app builds on the phone. The existing UI
already calls `cameraLoader.item.*` wrappers, so the diff lands mostly in `Camera.qml`, not the UI.

Existing settings UI to route (from the map): white balance (slider+toggle → `setWhiteBalanceMode`),
zoom (slider+pinch → `handleSetZoom`/`setZoom`), flash/torch, focus + AE/AF lock, photo/video
resolution, JPEG quality, video bitrate, HDR, color correction, grid/level/timer/GPS.

### M9 — Test, polish, file bugs (~half day)

- Battery, thermal, durability tests
- Compare quality side-by-side with FuriCam2 GStreamer recording and Camera1 still capture

**Total: ~3-4 days.** libhybris shim work is no longer the wildcard now that we know the binding.h macros do the heavy lifting.

## Legal posture

- FuriCam license: `SPDX-License-Identifier: GPL-2.0-only`
- motioncam-ut license: GPL v3 (incompatible — cannot copy verbatim)
- Google Android Open Source Project (AOSP) NDK samples (`github.com/android/ndk-samples`, `camera/basic` and `camera/texture-view`): Apache 2.0 (compatible — can lift directly)

**Practical rule:** read motioncam-ut for architectural understanding (ideas/patterns aren't copyrightable), write our actual code starting from Google's Apache 2.0 samples and the Android NDK headers. The resulting code will look similar to both because both look like the NDK — that's the "merger doctrine" / "scènes à faire": when an API has only one practical way to use it, that usage isn't copyrightable expression.

## Open questions

1. ~~**libhybris shim source**~~ — RESOLVED: libhybris-dev's Apache 2.0 `binding.h` macros do the work.
2. **Audio path** — keep GStreamer or move to `AAudio` native development kit (NDK)? GStreamer is the lower-risk choice for M6.
3. **FuriCam upstream relicense** — no longer needed for the shim, but still worth considering if we end up wanting to lift any motioncam-ut algorithmic code (denoising, exposure metering, etc.).
4. **Preview aspect-ratio orientation** — sensor orientation is 90° relative to display. Renderer needs to handle the rotation matrix.
5. **libcamera2ndk.so + libhybris on this device** — confirm `hybris_dlopen("libcamera2ndk.so")` actually loads on the FuriPhone FLX1s (it should, since MotionCam-UT successfully reaches Camera2 NDK via essentially this mechanism). Validate during M0.
