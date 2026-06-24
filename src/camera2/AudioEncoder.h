// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// AudioEncoder — Milestone 6: microphone → AAC, fed into the recording's
// AMediaMuxer as a second track.
//
// On a Halium/FuriOS device the Android audio HAL is bridged to PulseAudio, so
// the simplest robust capture path is GStreamer on the Linux side:
//
//     autoaudiosrc → audioconvert → audioresample → (avenc_aac|voaacenc)
//                  → aacparse(raw) → appsink
//
// A pull thread drains the appsink continuously.  The pipeline can be warmed up
// BEFORE recording (start()): it captures the mic, encodes AAC and caches the
// AudioSpecificConfig (csd-0) from the first sample's caps, but DISCARDS frames
// until attach() wires it to a muxer.  Then startRecording just attach()es the
// already-running stream — no mic/encoder spin-up on the record button.  stop
// detach()es (frames discarded again) while the pipeline stays warm.
//
// GStreamer types are kept out of this header (pimpl).  Compiled only when
// GStreamer (incl. gstreamer-app) is available; otherwise a stub backs it and
// recording is video-only.

#ifndef FURICAM2_AUDIO_ENCODER_H
#define FURICAM2_AUDIO_ENCODER_H

#include <functional>
#include <string>

#include "Camera2NDK.h"   // AMediaFormat, AMediaCodecBufferInfo

namespace furicam2 {

class AudioEncoder {
public:
    // Add the AAC track to the muxer from the given format; returns track index.
    using AddTrackFn = std::function<ssize_t(AMediaFormat*)>;
    // Write one encoded AAC sample (data valid only for the call's duration).
    using WriteFn    = std::function<void(const uint8_t*, AMediaCodecBufferInfo)>;
    // Persistent sink: called for EVERY sample once the format is known (data valid
    // only for the call).  Used by the pre-record ring path — the consumer buffers
    // samples itself and decides when to mux them.  Mutually exclusive with attach().
    using SinkFn     = std::function<void(const uint8_t* data, int size, int64_t ptsUs)>;

    AudioEncoder() = default;
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&)            = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // Build + play the capture pipeline (the "hot mic").  Frames are encoded and
    // discarded until attach().  Returns false on setup failure (lastError()).
    bool start(int bitrate = 128000, int sampleRate = 48000, int channels = 1);

    // Send EOS, stop the pipeline and join the pull thread.
    void stop();

    // True once the pipeline is up; true once the first sample's format (csd-0)
    // has been captured (i.e. the mic is confirmed working and a track can be
    // added immediately on attach()).
    bool isRunning() const;
    bool isReady()   const;

    // Begin muxing into a muxer: adds the AAC track (using the cached format)
    // and writes every subsequent sample via the callbacks.  Safe to call while
    // the pipeline is already warm — the next sample is muxed with no spin-up.
    void attach(AddTrackFn addTrack, WriteFn write);

    // Stop muxing; frames are discarded again and the pipeline stays warm.
    // Blocks until any in-flight write completes, so the muxer can then be
    // destroyed safely.
    void detach();

    // Pre-record ring path: deliver every sample to this sink (set null to stop).
    // Takes precedence over attach()'s callbacks.  Build the AAC track format from
    // the cached caps (nullptr until the first sample); caller owns the result.
    void setSink(SinkFn sink);
    AMediaFormat* makeFormat() const;

    const std::string& lastError() const { return lastError_; }
    int  framesProduced() const;

private:
    void pullLoop();   // drains the appsink on its own thread (defined in .cpp)

    struct Impl;
    Impl*       d_ = nullptr;
    std::string lastError_;
};

} // namespace furicam2

#endif // FURICAM2_AUDIO_ENCODER_H
