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
    AMediaFormat_setInt32(fmt, KEY_I_FRAME_INTERVAL, 1);
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
        lastOutputMs_    = nowMs();
        framesWritten_.store(0);
        audioFramesWritten_.store(0);
        clipActive_.store(true);
        // The codec's output format is already known after the first clip —
        // add the video track immediately instead of waiting for it again.
        if (cachedFormat_) {
            trackIdx_        = AMediaMuxer_addTrack(muxer_, cachedFormat_);
            videoTrackReady_ = (trackIdx_ >= 0);
            maybeStartMuxerLocked();
        }
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
                std::lock_guard<std::mutex> lk(muxerMutex_);
                lastOutputMs_ = nowMs();
                if (buf && muxer_ && muxerStarted_ && trackIdx_ >= 0) {
                    // Drop output until this clip's first IDR so the file is
                    // independently decodable, then normalize the PTS to zero.
                    if (!sawKeyFrame_ && (info.flags & BUFFER_FLAG_KEY_FRAME))
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

    // The caller has already stopped the camera feeding the input surface, so the
    // codec's output goes quiet once its pipeline tail is drained.  Wait for that.
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
}

void VideoEncoder::maybeStartMuxerLocked()
{
    if (muxerStarted_ || !muxer_)
        return;
    if (!videoTrackReady_)
        return;
    if (audioExpected_ && audioTrackIdx_ < 0)
        return;   // still waiting for the audio track to be added
    if (AMediaMuxer_start(muxer_) == AMEDIA_OK)
        muxerStarted_ = true;
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
    if (firstAudioPtsUs_ < 0)
        firstAudioPtsUs_ = info.presentationTimeUs;
    info.presentationTimeUs -= firstAudioPtsUs_;
    if (info.presentationTimeUs < 0)
        info.presentationTimeUs = 0;
    AMediaMuxer_writeSampleData(muxer_, (size_t)audioTrackIdx_, data, &info);
    audioFramesWritten_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace furicam2
