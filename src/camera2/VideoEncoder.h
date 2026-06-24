// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// VideoEncoder — hardware H.264 to MPEG-4 Part 14 (MP4).
//
// Wraps an AMediaCodec "video/avc" encoder in surface-input mode plus an
// AMediaMuxer.  The codec + its input surface (an ANativeWindow the camera
// capture session targets directly — zero copy, no GL, no CPU) are PERSISTENT
// (open()/close()); the muxer + output file are PER-CLIP (beginClip()/endClip()).
//
// This split is what enables simultaneous-preview+record with an instant
// record button (the encoder surface is a session target the whole time the app
// is in video mode; the camera only feeds it while recording).  Because the
// codec persists across clips, endClip() does a "soft stop" — it does NOT signal
// end-of-stream (that would end the codec) but drains the clip's tail once the
// camera has stopped feeding the surface, then finalizes the MP4.  The codec's
// output format (SPS/PPS) is cached from the first INFO_OUTPUT_FORMAT_CHANGED and
// re-used to add the video track on every subsequent clip; each clip is forced to
// start on an IDR keyframe so its file is independently decodable.
//
// Milestone 6: VideoEncoder also owns the per-clip muxer for an optional second
// (audio) track.  The AudioEncoder (GStreamer AAC) feeds samples through
// addAudioTrack() + writeAudioSample().  AMediaMuxer requires every track before
// start(), so muxer start is gated until the video track exists AND (audio is not
// expected OR the audio track has been added).  All track adds and sample writes
// are serialized under muxerMutex_, and each track's presentation timestamps are
// normalized to start at zero so the two streams stay in sync.

#ifndef FURICAM2_VIDEO_ENCODER_H
#define FURICAM2_VIDEO_ENCODER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Camera2NDK.h"

namespace furicam2 {

class VideoEncoder {
public:
    VideoEncoder()  = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // ── Persistent codec + input surface ─────────────────────────────────────
    // Configure + start the codec, create its input surface, start the drain
    // thread.  orientationDeg is written as each clip's MP4 rotation hint so
    // players show the video upright.  Returns false on failure (lastError()).
    bool open(int width, int height, int fps, int bitrate, int orientationDeg);
    bool isOpen() const { return codec_ != nullptr; }

    // Update the MP4 rotation hint used by the NEXT beginClip(), so each clip can
    // be tagged with the device orientation at the moment recording starts (the
    // codec/surface persist across clips, but the hint is written per clip).
    void setOrientation(int deg) { orientation_ = ((deg % 360) + 360) % 360; }

    // The codec input surface — add it as a capture request's video target.
    // Valid between open() and close().
    ANativeWindow* inputWindow() const { return inputWindow_; }

    // Stop + destroy the codec, release the surface, join the drain thread.
    void close();

    // ── Per-clip muxer ───────────────────────────────────────────────────────
    // Begin a new recording clip: create the MP4 muxer/file and (if the codec
    // format is already known) add the video track, then force the codec to emit
    // an IDR so this clip starts on a keyframe.  Returns false on failure.
    bool beginClip(const std::string& path);

    // Finalize the current clip.  The caller must FIRST stop the camera feeding
    // the input surface (switch the repeating request to preview-only / close the
    // record session); endClip() then drains the codec's tail and finalizes the
    // MP4.  The codec stays open for the next clip.
    void endClip();

    bool isClipActive() const { return clipActive_.load(); }

    // ── Back-compat single-shot API (record-only path) ───────────────────────
    bool start(const std::string& path, int width, int height, int fps,
               int bitrate, int orientationDeg)
    {
        return open(width, height, fps, bitrate, orientationDeg) && beginClip(path);
    }
    void stop() { endClip(); close(); }

    bool isRecording() const { return clipActive_.load(); }
    const std::string& lastError() const { return lastError_; }
    int  framesWritten() const { return framesWritten_.load(); }

    // ── Optional audio track (Milestone 6) ───────────────────────────────────
    // Call expectAudio(true) BEFORE beginClip() so the muxer waits for the audio
    // track before it starts (an MP4 track cannot be added after start).
    void expectAudio(bool yes) { audioExpected_ = yes; }

    // ── Pre-record ring buffer ───────────────────────────────────────────────
    // When enabled (and the encoder is being fed during preview), the drain loop
    // keeps the most recent ~1.5s of encoded frames so a clip can start INSTANTLY
    // from the last buffered keyframe instead of waiting for the next one.  The
    // clip then begins up to one GOP before the record button (a tight pre-roll).
    void enableRing(bool on) { ringEnabled_ = on; }

    // The audio source could not start — stop waiting for it (video-only).
    void cancelAudioExpectation();

    // Add the AAC track from the AudioEncoder's format (mime audio/mp4a-latm +
    // csd-0).  Returns the muxer track index (>=0) or -1.  Thread-safe.
    ssize_t addAudioTrack(AMediaFormat* fmt);

    // Write one encoded AAC sample.  info.presentationTimeUs is normalized to
    // the first written audio sample.  No-op until the muxer has started.
    // Thread-safe; safe to call from the AudioEncoder's pull thread.
    void writeAudioSample(const uint8_t* data, AMediaCodecBufferInfo info);

    int  audioFramesWritten() const { return audioFramesWritten_.load(); }

private:
    void drainLoop();
    // Start the muxer iff the video track is ready and audio (if expected) is
    // ready too.  Caller must hold muxerMutex_.
    void maybeStartMuxerLocked();
    void finalizeClipLocked();   // stop+free the muxer, close the file

    // Ring-buffer helpers (caller holds muxerMutex_).
    void pruneVideoRingLocked(int64_t nowU);   // drop history past the window, keep a leading IDR
    bool flushVideoRingLocked();               // write [last keyframe..now] to the muxer; false if no keyframe

    // One encoded H.264 access unit retained for the pre-record buffer.
    struct RingFrame {
        std::vector<uint8_t> data;
        int64_t              ptsUs;    // encoder (sensor-clock) PTS
        int64_t              wallUs;   // dequeue wall time (steady clock)
        bool                 key;      // IDR keyframe
    };

    // Persistent codec + surface.
    AMediaCodec*   codec_        = nullptr;
    ANativeWindow* inputWindow_  = nullptr;
    AMediaFormat*  cachedFormat_ = nullptr;   // codec output format (SPS/PPS)
    int            orientation_  = 0;

    // Per-clip muxer + state (all guarded by muxerMutex_).
    std::mutex     muxerMutex_;
    AMediaMuxer*   muxer_        = nullptr;
    int            fd_           = -1;
    ssize_t        trackIdx_     = -1;
    bool           muxerStarted_ = false;
    bool           audioExpected_   = false;
    bool           videoTrackReady_ = false;
    bool           sawKeyFrame_     = false;   // clip must start on an IDR
    ssize_t        audioTrackIdx_   = -1;
    int64_t        firstVideoPtsUs_ = -1;
    int64_t        firstAudioPtsUs_ = -1;
    int64_t        lastOutputMs_    = 0;       // for the soft-stop drain wait

    // Pre-record ring buffer (all guarded by muxerMutex_).
    bool                  ringEnabled_           = false;
    std::deque<RingFrame> videoRing_;
    int64_t               ringWindowUs_          = 1500000;   // ~1.5s of history
    int64_t               keyframeWallUs_        = -1;        // start keyframe's wall time (audio pre-roll align)
    int64_t               lastFlushedVideoPtsUs_ = -1;        // high-water mark; live frames must exceed it

    std::thread        drainThread_;
    std::atomic<bool>  stopDrain_     {false};
    std::atomic<bool>  clipActive_    {false};
    std::atomic<int>   framesWritten_ {0};
    std::atomic<int>   audioFramesWritten_ {0};
    std::string        lastError_;
};

} // namespace furicam2

#endif // FURICAM2_VIDEO_ENCODER_H
