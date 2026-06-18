// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// camera2_probe — Milestone 2/3a command-line harness.
//
// Enumerates the device's cameras through the Camera2 native development kit
// (via the libhybris shim), prints each one's capabilities, and opens the back
// camera to prove the device can be opened.  Options:
//   --all            also dump every metadata tag the chosen camera exposes
//   --stream [secs]  start a real preview capture session for N seconds (default
//                    5) and report the achieved frame rate — the M3 proof that
//                    the camera actually delivers frames (Camera1 never did).
//
// Deliberately Qt-free so it builds and runs on the phone without the full GUI
// app.  On a non-Halium host (the desktop) it is built against the libhybris
// stub and simply reports that Camera2 is inert.

#include "CameraSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    using namespace furicam2;

    bool        dumpAll    = false;
    bool        usePrivate = false;   // PRIVATE format + GPU usage (the HAL-preferred path)
    bool        doPoll     = false;   // also drain the reader from the main thread
    bool        photoMode  = false;
    std::string photoPath;
    int         recordSecs = 0;
    std::string recordPath;
    int         recordW    = 1920;
    int         recordH    = 1080;
    int         recordBitrate = 20000000;
    double      manualExpMs = 0;   // >0 => manual exposure during --stream
    int         manualIso   = 400;
    int         streamSecs = 0;
    bool        prewarm    = false;   // warm the mic before --record
    bool        videoModeTest = false; // simultaneous preview+record test
    int         flashArg   = -1;      // 0=off 1=on 2=auto (with --photo)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--flash") == 0 && i + 1 < argc) {
            flashArg = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--all") == 0) {
            dumpAll = true;
        } else if (std::strcmp(argv[i], "--prewarm") == 0) {
            prewarm = true;
        } else if (std::strcmp(argv[i], "--videomode") == 0) {
            videoModeTest = true;
        } else if (std::strcmp(argv[i], "--private") == 0) {
            usePrivate = true;
        } else if (std::strcmp(argv[i], "--poll") == 0) {
            doPoll = true;
        } else if (std::strcmp(argv[i], "--photo") == 0) {
            photoMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                photoPath = argv[++i];
        } else if (std::strcmp(argv[i], "--record") == 0) {
            recordSecs = 5;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                recordSecs = std::atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                recordPath = argv[++i];
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &recordW, &recordH);
        } else if (std::strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            recordBitrate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            manualExpMs = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--iso") == 0 && i + 1 < argc) {
            manualIso = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--stream") == 0) {
            streamSecs = 5;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                streamSecs = std::atoi(argv[++i]);
        }
    }

    CameraSession session;   // defaults to logging on stdout

    if (CameraSession::isHostStub()) {
        std::puts("camera2_probe: built with the libhybris host stub (non-Halium host) — "
                  "Camera2 is inert here.\nBuild and run this on the phone for a real "
                  "capability dump.");
        return 0;   // success: this build exists only for compile/link checking
    }

    if (!session.enumerate()) {
        std::printf("camera2_probe: enumerate failed: %s\n", session.lastError().c_str());
        return 1;
    }

    const auto& cams = session.cameras();
    std::printf("Device has %zu exposed camera(s)\n\n", cams.size());
    for (const auto& c : cams) {
        session.dumpSummary(c);
        std::puts("");
    }

    if (cams.empty()) {
        std::puts("no cameras to open");
        return 0;
    }

    // Prefer the first back-facing camera; otherwise the first camera reported.
    std::string openId = cams.front().id;
    for (const auto& c : cams) {
        if (c.facing == ACAMERA_LENS_FACING_BACK) {
            openId = c.id;
            break;
        }
    }

    std::printf("opening camera %s ...\n", openId.c_str());
    if (!session.open(openId)) {
        std::printf("open failed: %s\n", session.lastError().c_str());
        return 1;
    }
    std::printf("opened camera %s OK\n", openId.c_str());

    // Full list of PRIVATE (preview/recording-capable) sizes — what video could
    // record at, any aspect (incl. squares).
    {
        auto privs = session.privateSizes();
        std::sort(privs.begin(), privs.end(),
                  [](const CameraSession::StreamConfig& a, const CameraSession::StreamConfig& b) {
                      return (long)a.width * a.height > (long)b.width * b.height;
                  });
        auto gcd = [](int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a ? a : 1; };
        std::printf("\nPRIVATE (video-recordable) sizes: %zu\n", privs.size());
        for (const auto& s : privs) {
            const int g = gcd(s.width, s.height);
            const double mp = s.width * (double)s.height / 1e6;
            std::printf("  %4dx%-4d  %2d:%-2d  %.1f MP\n", s.width, s.height, s.width / g, s.height / g, mp);
        }
        std::printf("\n");
    }

    if (photoMode) {
        if (photoPath.empty())
            photoPath = "/tmp/camera2_shot.jpg";
        std::atomic<int> photoDone{0};   // 0 pending, 1 ok, 2 fail
        session.setPhotoCallback([&](const std::string& p, bool ok) {
            std::printf("photo callback: %s ok=%d\n", p.c_str(), (int)ok);
            photoDone.store(ok ? 1 : 2);
        });
        // YUV preview just to drive a running session; still output is enabled.
        if (!session.startPreview(1280, 720, AIMAGE_FORMAT_YUV_420_888, 0, 30, /*withStill*/ true)) {
            std::printf("startPreview failed: %s\n", session.lastError().c_str());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // let AE/AF settle
            if (flashArg == 2) {
                // Auto-flash emulation (same as the bridge): AE in ON_AUTO_FLASH,
                // read AE_STATE, force ON_ALWAYS_FLASH when FLASH_REQUIRED.
                session.setFlashMode(2);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                session.triggerPrecapture();
                int s = 0;
                for (int i = 0; i < 30; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    s = session.aeState();
                    if (i >= 4 && (s == 2 || s == 3 || s == 4)) break;
                }
                std::printf("AUTO flash: final AE state=%d -> %s\n",
                            s, (s == 4 ? "FIRE (always-flash)" : "no flash"));
                session.setFlashMode(s == 4 ? 1 : 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else if (flashArg >= 0) {
                session.setFlashMode(flashArg);
                std::printf("flash mode set to %d (1=on 2=auto)\n", flashArg);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::printf("capturing photo to %s ...\n", photoPath.c_str());
            if (!session.capturePhoto(photoPath))
                std::printf("capturePhoto failed: %s\n", session.lastError().c_str());
            for (int i = 0; i < 60 && photoDone.load() == 0; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const int r = photoDone.load();
            std::printf("photo result: %s\n", r == 1 ? "OK" : (r == 2 ? "FAILED" : "TIMEOUT"));
        }
        session.setPhotoCallback({});
        session.stopPreview();
    }

    if (videoModeTest) {
        // Verify simultaneous preview+record: the encoder surface joins the live
        // preview session; record just switches the repeating request.  Preview
        // must keep flowing during record, and the persistent codec must record
        // multiple clips.  Mirrors the app's PRIVATE preview + JPEG + encoder combo.
        std::printf("=== simultaneous preview+record test ===\n");
        if (!session.startPreview(1280, 720, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 30, /*withStill*/ true)) {
            std::printf("startPreview failed: %s\n", session.lastError().c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("preview-only: %d frames in ~1s\n", session.frameCount());

        if (prewarm)
            session.prepareRecording();

        if (!session.enterVideoMode(recordW, recordH, 30, recordBitrate)) {
            std::printf("enterVideoMode failed: %s\n", session.lastError().c_str());
            return 1;
        }
        std::printf("entered video mode (preview+encoder session)\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        for (int clip = 1; clip <= 2; ++clip) {
            char p[64];
            std::snprintf(p, sizeof p, "/tmp/fc2_simul%d.mp4", clip);
            const int before = session.frameCount();
            if (!session.startRecording(p, recordW, recordH, 30, recordBitrate)) {
                std::printf("clip %d startRecording failed: %s\n", clip, session.lastError().c_str());
                continue;
            }
            std::printf("clip %d recording -> %s\n", clip, p);
            std::this_thread::sleep_for(std::chrono::seconds(4));
            const int during = session.frameCount() - before;
            session.stopRecording();
            std::printf("clip %d stopped; PREVIEW frames during record: %d (~%d fps) %s\n",
                        clip, during, during / 4,
                        during > 30 ? "-> preview LIVE during record" : "-> PREVIEW STALLED");
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }

        const int beforeExit = session.frameCount();
        session.exitVideoMode();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("after exitVideoMode: preview still live (+%d frames in ~1s)\n",
                    session.frameCount() - beforeExit);
        session.stopPreview();
        std::fflush(stdout);   // _Exit() does not flush stdio buffers
        std::_Exit(0);   // skip the (intentionally blocking) binder pool thread teardown
    }

    if (recordSecs > 0) {
        const std::string path = recordPath.empty() ? "/tmp/camera2_rec.mp4" : recordPath;
        if (prewarm) {
            // Warm the mic ~1.5 s ahead so startRecording just attaches it.
            std::printf("pre-warming mic ...\n");
            bool ok = session.prepareRecording();
            for (int i = 0; i < 15 && !session.isAudioReady(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::printf("mic warm: started=%s ready=%s\n",
                        ok ? "yes" : "no", session.isAudioReady() ? "yes" : "no");
        }
        std::printf("recording %d s of %dx%d@30 H.264 (%d bps) -> %s ...\n",
                    recordSecs, recordW, recordH, recordBitrate, path.c_str());
        if (!session.startRecording(path, recordW, recordH, 30, recordBitrate)) {
            std::printf("startRecording failed: %s\n", session.lastError().c_str());
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(recordSecs));
            std::printf("stopping recording ...\n");
            session.stopRecording();
            std::printf("recording stopped\n");
        }
    }

    if (streamSecs > 0) {
        const int kW = 1280, kH = 720;
        const int      fmt   = usePrivate ? AIMAGE_FORMAT_PRIVATE : AIMAGE_FORMAT_YUV_420_888;
        const uint64_t usage = usePrivate ? AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE : 0;
        std::printf("starting %dx%d %s preview for %d s (%s) ...\n",
                    kW, kH, usePrivate ? "PRIVATE" : "YUV_420_888", streamSecs,
                    doPoll ? "callback + poll" : "callback");
        // Count QR-analysis (YUV luma) frames to prove the analysis stream works.
        std::atomic<int> qrFrames{0};
        std::atomic<int> qrW{0}, qrH{0}, qrStride{0};
        session.setAnalysisCallback([&](const uint8_t* y, int w, int h, int stride) {
            qrFrames.fetch_add(1, std::memory_order_relaxed);
            qrW.store(w); qrH.store(h); qrStride.store(stride);
        });
        if (!session.startPreview(kW, kH, fmt, usage)) {
            std::printf("startPreview failed: %s\n", session.lastError().c_str());
            session.close();
            return 1;
        }
        if (manualExpMs > 0) {
            std::printf("setting manual exposure: %.1f ms, ISO %d (fps should hold even in dim light)\n",
                        manualExpMs, manualIso);
            session.setManualExposure(manualIso, (int64_t)(manualExpMs * 1.0e6));
        }
        int prev = 0;
        for (int s = 1; s <= streamSecs; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int polled = doPoll ? session.pollFrames() : 0;
            int now = session.frameCount();
            std::printf("  t=%2ds  frames=%d  (+%d)  callbacks=%d  polled=%d  acquireStatus=%d  last=%dx%d\n",
                        s, now, now - prev, session.callbackCount(), polled, session.lastAcquireStatus(),
                        session.lastFrameWidth(), session.lastFrameHeight());
            prev = now;
        }
        int total = session.frameCount();
        std::printf("captured %d frames in %d s = %.1f fps\n",
                    total, streamSecs, streamSecs ? (double)total / streamSecs : 0.0);
        std::printf("QR analysis (YUV luma) frames: %d  (%dx%d stride %d)\n",
                    qrFrames.load(), qrW.load(), qrH.load(), qrStride.load());
        // Teardown happens via session.close() below, which closes the session
        // and device before freeing the reader (correct order).
    }

    if (dumpAll) {
        std::puts("");
        session.dumpAllTags(openId);
    }

    std::printf("closing camera %s ...\n", openId.c_str());
    session.close();
    std::printf("closed camera %s\n", openId.c_str());

    // A binder thread-pool thread (started for streaming) blocks forever in
    // joinThreadPool; let the OS reap it rather than risk a static-destructor
    // hang on the way out.
    std::fflush(stdout);
    std::_Exit(0);
}
