// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// VideoEncoder implementation — see VideoEncoder.h.

#include "VideoEncoder.h"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace furicam2 {

namespace {
// AMediaFormat keys (stable strings) and the MediaCodec surface color format.
constexpr const char* KEY_MIME             = "mime";
constexpr const char* KEY_WIDTH            = "width";
constexpr const char* KEY_HEIGHT           = "height";
constexpr const char* KEY_COLOR_FORMAT     = "color-format";
constexpr const char* KEY_BIT_RATE         = "bitrate";
constexpr const char* KEY_FRAME_RATE       = "frame-rate";
constexpr const char* KEY_I_FRAME_INTERVAL = "i-frame-interval";
constexpr const char* KEY_REQUEST_SYNC     = "request-sync-frame";
constexpr int32_t     COLOR_FormatSurface  = 0x7F000789;  // encode from a Surface
constexpr const char* MIME_AVC             = "video/avc";
// AMediaCodec output flag: this buffer is a sync/key frame (IDR).
constexpr uint32_t    BUFFER_FLAG_KEY_FRAME = 1;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t nowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

VideoEncoder::~VideoEncoder()
{
    close();
}

bool VideoEncoder::open(int width, int height, int fps, int bitrate, int orientationDeg)
{
    if (codec_) {
        lastError_ = "already open";
        return false;
    }
    orientation_ = ((orientationDeg % 360) + 360) % 360;

    codec_ = AMediaCodec_createEncoderByType(MIME_AVC);
    if (!codec_) {
        lastError_ = "AMediaCodec_createEncoderByType(video/avc) failed";
        return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, KEY_MIME, MIME_AVC);
    AMediaFormat_setInt32(fmt, KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, KEY_HEIGHT, height);
    AMediaFormat_setInt32(fmt, KEY_COLOR_FORMAT, COLOR_FormatSurface);
    AMediaFormat_setInt32(fmt, KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(fmt, KEY_FRAME_RATE, fps);
    // Request a short GOP so the pre-record ring's last keyframe is recent → a tighter
    // pre-roll.  NOTE: this HAL ignores sub-second i-frame-intervals (measured: it
    // pins keyframes at ~0.9s regardless, and it also ignores request-sync-frame), so
    // the pre-roll lands ~0.5s on average.  Left at the smallest value in case a
    // future HAL honours it; nothing shorter is achievable on this encoder today.
    AMediaFormat_setFloat(fmt, KEY_I_FRAME_INTERVAL, 0.3f);
    media_status_t ms = AMediaCodec_configure(codec_, fmt, nullptr, nullptr,
                                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);
    if (ms != AMEDIA_OK) {
        lastError_ = "AMediaCodec_configure failed (status " + std::to_string((int)ms) + ")";
        return false;
    }

    ms = AMediaCodec_createInputSurface(codec_, &inputWindow_);
    if (ms != AMEDIA_OK || !inputWindow_) {
        lastError_ = "AMediaCodec_createInputSurface failed (status " + std::to_string((int)ms) + ")";
        return false;
    }

    ms = AMediaCodec_start(codec_);
    if (ms != AMEDIA_OK) {
        lastError_ = "AMediaCodec_start failed (status " + std::to_string((int)ms) + ")";
        return false;
    }

    stopDrain_.store(false);
    drainThread_ = std::thread(&VideoEncoder::drainLoop, this);
    return true;
}

bool VideoEncoder::beginClip(const std::string& path)
{
    if (!codec_) {
        lastError_ = "beginClip: codec not open";
        return false;
    }
    if (clipActive_.load()) {
        lastError_ = "beginClip: a clip is already active";
        return false;
    }

    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        lastError_ = "open(" + path + ") failed";
        return false;
    }
    AMediaMuxer* mx = AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!mx) {
        ::close(fd);
        lastError_ = "AMediaMuxer_new failed";
        return false;
    }
    AMediaMuxer_setOrientationHint(mx, orientation_);

    {
        std::lock_guard<std::mutex> lk(muxerMutex_);
        fd_              = fd;
        muxer_           = mx;
        trackIdx_        = -1;
        muxerStarted_    = false;
        videoTrackReady_ = false;
        sawKeyFrame_     = false;
        audioTrackIdx_   = -1;
        firstVideoPtsUs_ = -1;
        firstAudioPtsUs_ = -1;
        keyframeWallUs_        = -1;   // ring: set when we flush the start keyframe
        lastFlushedVideoPtsUs_ = -1;   // ring: high-water mark for live frames
        lastFlushedAudioPtsUs_ = -1;
        lastOutputMs_    = nowMs();
        framesWritten_.store(0);
        audioFramesWritten_.store(0);
        clipActive_.store(true);
        // The codec's output format is already known after the first clip —
        // add the video track immediately instead of waiting for it again.
        if (cachedFormat_) {
            trackIdx_        = AMediaMuxer_addTrack(muxer_, cachedFormat_);
            videoTrackReady_ = (trackIdx_ >= 0);
        }
        // Add the AAC track up front too (its format was captured while pre-warming),
        // so the muxer can start with both tracks and flush the pre-roll immediately.
        if (audioExpected_ && cachedAudioFormat_ && audioTrackIdx_ < 0)
            audioTrackIdx_ = AMediaMuxer_addTrack(muxer_, cachedAudioFormat_);
        maybeStartMuxerLocked();
    }

    // Force an IDR so this clip's file starts on a keyframe (decodable alone).
    AMediaFormat* p = AMediaFormat_new();
    AMediaFormat_setInt32(p, KEY_REQUEST_SYNC, 0);
    AMediaCodec_setParameters(codec_, p);
    AMediaFormat_delete(p);
    return true;
}

void VideoEncoder::drainLoop()
{
    while (!stopDrain_.load()) {
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, /*timeoutUs*/ 10000);
        if (idx >= 0) {
            // Codec config (SPS/PPS) is carried by the output format, not a sample.
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, false);
                continue;
            }
            if (info.size > 0) {
                size_t outSize = 0;
                uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, (size_t)idx, &outSize);
                const bool    isKey = (info.flags & BUFFER_FLAG_KEY_FRAME) != 0;
                const int64_t wallU = nowUs();
                std::lock_guard<std::mutex> lk(muxerMutex_);
                lastOutputMs_ = nowMs();

                // Pre-record buffer: keep recent encoded frames so a clip can start
                // from the last keyframe already on hand (set up in flushVideoRingLocked).
                if (buf && ringEnabled_) {
                    videoRing_.push_back(RingFrame{
                        std::vector<uint8_t>(buf + info.offset, buf + info.offset + info.size),
                        info.presentationTimeUs, wallU, isKey});
                    pruneVideoRingLocked(wallU);
                }

                // Live write — only frames produced AFTER this clip's flush point
                // (pts beyond the high-water mark), once the muxer is running.  For
                // the no-ring path lastFlushedVideoPtsUs_ stays -1 (all frames pass)
                // and we still require a keyframe start.
                if (buf && muxer_ && muxerStarted_ && trackIdx_ >= 0
                    && info.presentationTimeUs > lastFlushedVideoPtsUs_) {
                    if (!sawKeyFrame_ && isKey)
                        sawKeyFrame_ = true;
                    if (sawKeyFrame_) {
                        if (firstVideoPtsUs_ < 0)
                            firstVideoPtsUs_ = info.presentationTimeUs;
                        AMediaCodecBufferInfo wi = info;
                        wi.presentationTimeUs = info.presentationTimeUs - firstVideoPtsUs_;
                        AMediaMuxer_writeSampleData(muxer_, (size_t)trackIdx_, buf, &wi);
                        framesWritten_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, (size_t)idx, false);
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            // The real output format (with CSD) — cache it for every clip's track.
            AMediaFormat* of = AMediaCodec_getOutputFormat(codec_);
            if (of) {
                std::lock_guard<std::mutex> lk(muxerMutex_);
                if (cachedFormat_)
                    AMediaFormat_delete(cachedFormat_);
                cachedFormat_ = of;   // owned; re-used to add the track per clip
                if (clipActive_.load() && muxer_ && !videoTrackReady_) {
                    trackIdx_        = AMediaMuxer_addTrack(muxer_, cachedFormat_);
                    videoTrackReady_ = (trackIdx_ >= 0);
                    maybeStartMuxerLocked();
                }
            }
        }
        // AMEDIACODEC_INFO_TRY_AGAIN_LATER (-1): no output (idle/between clips).
    }
}

void VideoEncoder::endClip()
{
    if (!clipActive_.load())
        return;

    // With the pre-record ring the encoder is fed CONTINUOUSLY (the prefeed keeps the
    // preview request targeting it), so it never goes quiet — the soft-drain below
    // would hang the full deadline.  The clip is already complete up to the latest
    // dequeued frame (the drain loop wrote it live); any frame still in the codec's
    // output queue is post-stop and belongs to the ring, not this clip.  Finalize now.
    if (!ringEnabled_) {
        // No-ring path: the caller has stopped the camera feeding the input surface,
        // so wait for the codec's tail to drain quiet before finalizing.
        using namespace std::chrono;
        auto deadline = steady_clock::now() + milliseconds(2000);
        while (steady_clock::now() < deadline) {
            bool quiet;
            {
                std::lock_guard<std::mutex> lk(muxerMutex_);
                quiet = (nowMs() - lastOutputMs_) > 250;
            }
            if (quiet)
                break;
            std::this_thread::sleep_for(milliseconds(20));
        }
    }

    std::lock_guard<std::mutex> lk(muxerMutex_);
    finalizeClipLocked();
}

void VideoEncoder::finalizeClipLocked()
{
    if (muxer_) {
        if (muxerStarted_)
            AMediaMuxer_stop(muxer_);
        AMediaMuxer_delete(muxer_);
        muxer_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    clipActive_.store(false);
    muxerStarted_    = false;
    trackIdx_        = -1;
    videoTrackReady_ = false;
    sawKeyFrame_     = false;
    audioExpected_   = false;
    audioTrackIdx_   = -1;
    firstVideoPtsUs_ = -1;
    firstAudioPtsUs_ = -1;
    keyframeWallUs_        = -1;
    lastFlushedVideoPtsUs_ = -1;
    lastFlushedAudioPtsUs_ = -1;
}

void VideoEncoder::close()
{
    if (clipActive_.load()) {
        std::lock_guard<std::mutex> lk(muxerMutex_);
        finalizeClipLocked();   // safety: finalize any clip still open
    }

    stopDrain_.store(true);
    if (drainThread_.joinable())
        drainThread_.join();

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (inputWindow_) {
        ANativeWindow_release(inputWindow_);
        inputWindow_ = nullptr;
    }
    if (cachedFormat_) {
        AMediaFormat_delete(cachedFormat_);
        cachedFormat_ = nullptr;
    }
    if (cachedAudioFormat_) {
        AMediaFormat_delete(cachedAudioFormat_);
        cachedAudioFormat_ = nullptr;
    }
}

void VideoEncoder::maybeStartMuxerLocked()
{
    if (muxerStarted_ || !muxer_)
        return;
    if (!videoTrackReady_)
        return;
    if (audioExpected_ && audioTrackIdx_ < 0)
        return;   // still waiting for the audio track to be added
    if (AMediaMuxer_start(muxer_) == AMEDIA_OK) {
        muxerStarted_ = true;
        // Pre-record buffer: write the most recent keyframe-aligned GOP from the ring
        // so the clip starts INSTANTLY from a keyframe already on hand, then the
        // matching buffered audio so the pre-roll has sound.  If nothing was flushed
        // (ring off/empty), fall back to forcing a fresh IDR and waiting for it (the
        // next natural GOP boundary on a HAL that ignores the request).
        if (flushVideoRingLocked()) {
            flushAudioRingLocked();
        } else {
            AMediaFormat* p = AMediaFormat_new();
            AMediaFormat_setInt32(p, KEY_REQUEST_SYNC, 0);
            AMediaCodec_setParameters(codec_, p);
            AMediaFormat_delete(p);
        }
    }
}

// Drop ring frames older than the window, but never below the most recent keyframe
// (a clip must always be able to start on an IDR).  Caller holds muxerMutex_.
void VideoEncoder::pruneVideoRingLocked(int64_t nowU)
{
    while (videoRing_.size() > 1 && videoRing_.front().wallUs < nowU - ringWindowUs_) {
        if (videoRing_.front().key) {
            bool laterKey = false;
            for (size_t i = 1; i < videoRing_.size(); ++i)
                if (videoRing_[i].key) { laterKey = true; break; }
            if (!laterKey)
                break;   // this is the only keyframe left — keep it
        }
        videoRing_.pop_front();
    }
}

// Write the buffered GOP from the most recent keyframe to the muxer, normalizing
// PTS so that keyframe becomes time zero.  Sets the high-water mark + the keyframe's
// wall time (for audio pre-roll alignment).  Returns false if no keyframe is buffered
// (ring disabled/empty), in which case the caller takes the legacy path.
bool VideoEncoder::flushVideoRingLocked()
{
    if (!ringEnabled_ || videoRing_.empty())
        return false;
    ssize_t startIdx = -1;
    for (ssize_t i = (ssize_t)videoRing_.size() - 1; i >= 0; --i)
        if (videoRing_[i].key) { startIdx = i; break; }
    if (startIdx < 0)
        return false;

    const RingFrame& kf = videoRing_[(size_t)startIdx];
    firstVideoPtsUs_ = kf.ptsUs;
    keyframeWallUs_  = kf.wallUs;
    sawKeyFrame_     = true;
    for (size_t i = (size_t)startIdx; i < videoRing_.size(); ++i) {
        const RingFrame& f = videoRing_[i];
        AMediaCodecBufferInfo wi{};
        wi.offset            = 0;
        wi.size              = (int32_t)f.data.size();
        wi.presentationTimeUs = f.ptsUs - firstVideoPtsUs_;
        wi.flags             = f.key ? BUFFER_FLAG_KEY_FRAME : 0;
        AMediaMuxer_writeSampleData(muxer_, (size_t)trackIdx_, f.data.data(), &wi);
        framesWritten_.fetch_add(1, std::memory_order_relaxed);
        lastFlushedVideoPtsUs_ = f.ptsUs;
    }
    fprintf(stderr, "[fc2] pre-record: clip starts from buffered keyframe "
            "(pre-roll %lldms, %zu frames)\n",
            (long long)((videoRing_.back().wallUs - kf.wallUs) / 1000),
            videoRing_.size() - (size_t)startIdx);
    return true;
}

// ── Pre-record audio ─────────────────────────────────────────────────────────
void VideoEncoder::setAudioFormat(AMediaFormat* fmt)
{
    std::lock_guard<std::mutex> lk(muxerMutex_);
    if (cachedAudioFormat_)
        AMediaFormat_delete(cachedAudioFormat_);
    cachedAudioFormat_ = fmt;   // takes ownership (may be nullptr)
}

void VideoEncoder::pruneAudioRingLocked(int64_t nowU)
{
    while (audioRing_.size() > 1 && audioRing_.front().wallUs < nowU - ringWindowUs_)
        audioRing_.pop_front();
}

void VideoEncoder::bufferAudioSample(const uint8_t* data, int size, int64_t ptsUs)
{
    if (!data || size <= 0)
        return;
    const int64_t wallU = nowUs();
    std::lock_guard<std::mutex> lk(muxerMutex_);

    if (ringEnabled_) {
        audioRing_.push_back(AudioFrame{
            std::vector<uint8_t>(data, data + size), ptsUs, wallU});
        pruneAudioRingLocked(wallU);
    }

    // Live write: hold until the first video frame is muxed (so the soundtrack's zero
    // aligns with the picture), then write samples past the flush high-water mark.
    if (muxer_ && muxerStarted_ && audioTrackIdx_ >= 0 && firstVideoPtsUs_ >= 0
        && ptsUs > lastFlushedAudioPtsUs_) {
        if (firstAudioPtsUs_ < 0) {
            // No pre-roll audio was flushed (e.g. ring off): align this first sample
            // to the video start keyframe via the wall clock (a brief silent lead-in),
            // or to itself when there's no keyframe reference (legacy/no-ring).
            int64_t offset = (keyframeWallUs_ >= 0) ? (wallU - keyframeWallUs_) : 0;
            if (offset < 0) offset = 0;
            firstAudioPtsUs_ = ptsUs - offset;
        }
        AMediaCodecBufferInfo info{};
        info.offset            = 0;
        info.size              = size;
        info.flags             = 0;
        info.presentationTimeUs = ptsUs - firstAudioPtsUs_;
        if (info.presentationTimeUs < 0)
            info.presentationTimeUs = 0;
        AMediaMuxer_writeSampleData(muxer_, (size_t)audioTrackIdx_, data, &info);
        audioFramesWritten_.fetch_add(1, std::memory_order_relaxed);
        lastFlushedAudioPtsUs_ = ptsUs;
    }
}

// Write buffered AAC from the video start keyframe's wall time onward, aligned so the
// first such sample lands at its true offset after the keyframe — filling the video
// pre-roll with sound.  Caller holds muxerMutex_ and has already flushed the video.
void VideoEncoder::flushAudioRingLocked()
{
    if (!ringEnabled_ || keyframeWallUs_ < 0 || audioTrackIdx_ < 0 || audioRing_.empty())
        return;
    size_t startIdx = audioRing_.size();
    for (size_t i = 0; i < audioRing_.size(); ++i)
        if (audioRing_[i].wallUs >= keyframeWallUs_) { startIdx = i; break; }
    if (startIdx >= audioRing_.size())
        return;   // no buffered audio overlaps the pre-roll yet

    const AudioFrame& a0 = audioRing_[startIdx];
    firstAudioPtsUs_ = a0.ptsUs - (a0.wallUs - keyframeWallUs_);
    for (size_t i = startIdx; i < audioRing_.size(); ++i) {
        const AudioFrame& a = audioRing_[i];
        AMediaCodecBufferInfo info{};
        info.offset            = 0;
        info.size              = (int32_t)a.data.size();
        info.flags             = 0;
        info.presentationTimeUs = a.ptsUs - firstAudioPtsUs_;
        if (info.presentationTimeUs < 0)
            info.presentationTimeUs = 0;
        AMediaMuxer_writeSampleData(muxer_, (size_t)audioTrackIdx_, a.data.data(), &info);
        audioFramesWritten_.fetch_add(1, std::memory_order_relaxed);
        lastFlushedAudioPtsUs_ = a.ptsUs;
    }
}

void VideoEncoder::cancelAudioExpectation()
{
    std::lock_guard<std::mutex> lk(muxerMutex_);
    audioExpected_ = false;
    maybeStartMuxerLocked();   // video track may already be waiting on us
}

ssize_t VideoEncoder::addAudioTrack(AMediaFormat* fmt)
{
    if (!fmt)
        return -1;
    std::lock_guard<std::mutex> lk(muxerMutex_);
    if (!muxer_ || muxerStarted_)
        return -1;   // too late — the muxer was already started
    audioTrackIdx_ = AMediaMuxer_addTrack(muxer_, fmt);
    maybeStartMuxerLocked();
    return audioTrackIdx_;
}

void VideoEncoder::writeAudioSample(const uint8_t* data, AMediaCodecBufferInfo info)
{
    if (!data || info.size <= 0)
        return;
    std::lock_guard<std::mutex> lk(muxerMutex_);
    if (!muxer_ || !muxerStarted_ || audioTrackIdx_ < 0)
        return;   // drop pre-roll / between-clip audio until both tracks are live
    if (firstVideoPtsUs_ < 0)
        return;   // hold audio until the first (key)frame is muxed, so the
                  // soundtrack's zero aligns with the picture.  (Legacy record-only
                  // path; the ring path uses bufferAudioSample() instead.)
    if (firstAudioPtsUs_ < 0)
        firstAudioPtsUs_ = info.presentationTimeUs;
    info.presentationTimeUs -= firstAudioPtsUs_;
    if (info.presentationTimeUs < 0)
        info.presentationTimeUs = 0;
    AMediaMuxer_writeSampleData(muxer_, (size_t)audioTrackIdx_, data, &info);
    audioFramesWritten_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace furicam2
