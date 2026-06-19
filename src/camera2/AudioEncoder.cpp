// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// AudioEncoder implementation — see AudioEncoder.h.  GStreamer captures the mic
// and encodes AAC continuously; a pull thread either muxes samples (when
// attached) or discards them (warm but idle).

#include "AudioEncoder.h"

#include <utility>

#ifdef FURICAM_HAVE_AUDIO

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace furicam {

namespace {
std::once_flag g_gstInitOnce;

// Pick the best available AAC encoder element (libav first — better quality;
// then the voaac fallback).  Returns nullptr if none is installed.
const char* pickAacEncoder()
{
    static const char* candidates[] = { "avenc_aac", "voaacenc", "faac", nullptr };
    for (int i = 0; candidates[i]; ++i) {
        GstElementFactory* f = gst_element_factory_find(candidates[i]);
        if (f) {
            gst_object_unref(f);
            return candidates[i];
        }
    }
    return nullptr;
}

// Build a 2-byte AAC LC AudioSpecificConfig for the given rate/channels, used
// only if the encoder's caps carry no codec_data.
void buildAscFallback(int sampleRate, int channels, std::vector<uint8_t>& out)
{
    static const int kFreq[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350 };
    int freqIdx = 4;  // default 44100
    for (int i = 0; i < (int)(sizeof(kFreq) / sizeof(kFreq[0])); ++i)
        if (kFreq[i] == sampleRate) { freqIdx = i; break; }
    const int objectType = 2;  // AAC LC
    uint8_t b0 = (uint8_t)((objectType << 3) | ((freqIdx >> 1) & 0x07));
    uint8_t b1 = (uint8_t)(((freqIdx & 0x01) << 7) | ((channels & 0x0F) << 3));
    out = { b0, b1 };
}
} // namespace

struct AudioEncoder::Impl {
    GstElement*        pipeline = nullptr;
    GstAppSink*        sink     = nullptr;
    std::thread        pullThread;
    std::atomic<bool>  stopping {false};
    std::atomic<int>   frames   {0};

    std::mutex           mtx;            // guards everything below
    bool                 formatReady = false;
    int                  rate     = 48000;
    int                  channels = 1;
    std::vector<uint8_t> asc;            // cached AudioSpecificConfig (csd-0)
    bool                 active     = false;
    bool                 trackAdded = false;
    AddTrackFn           addTrack;
    WriteFn              write;
};

AudioEncoder::~AudioEncoder()
{
    stop();
}

int AudioEncoder::framesProduced() const
{
    return d_ ? d_->frames.load() : 0;
}

bool AudioEncoder::isRunning() const
{
    return d_ != nullptr;
}

bool AudioEncoder::isReady() const
{
    if (!d_)
        return false;
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->formatReady;
}

bool AudioEncoder::start(int bitrate, int sampleRate, int channels)
{
    if (d_) {
        lastError_ = "already started";
        return false;
    }
    std::call_once(g_gstInitOnce, [] { gst_init(nullptr, nullptr); });

    const char* enc = pickAacEncoder();
    if (!enc) {
        lastError_ = "no AAC encoder element (voaacenc/avenc_aac/faac) available";
        return false;
    }

    // autoaudiosrc resolves to pulsesrc on FuriOS; force a fixed rate/channel
    // raw format, then AAC-encode and parse to raw (codec_data in caps).
    gchar* desc = g_strdup_printf(
        "autoaudiosrc ! queue ! audioconvert ! audioresample ! "
        "audio/x-raw,rate=%d,channels=%d ! %s bitrate=%d ! "
        "aacparse ! appsink name=fc2sink sync=false max-buffers=16 drop=false",
        sampleRate, channels, enc, bitrate);

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!pipeline || err) {
        lastError_ = std::string("gst_parse_launch failed: ")
                   + (err ? err->message : "unknown");
        if (err) g_error_free(err);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }

    GstElement* sinkEl = gst_bin_get_by_name(GST_BIN(pipeline), "fc2sink");
    if (!sinkEl) {
        lastError_ = "appsink 'fc2sink' not found in pipeline";
        gst_object_unref(pipeline);
        return false;
    }
    // Constrain the appsink to raw AAC so the AudioSpecificConfig is in caps.
    GstCaps* sinkCaps = gst_caps_new_simple("audio/mpeg",
                                            "mpegversion", G_TYPE_INT, 4,
                                            "stream-format", G_TYPE_STRING, "raw",
                                            nullptr);
    gst_app_sink_set_caps(GST_APP_SINK(sinkEl), sinkCaps);
    gst_caps_unref(sinkCaps);

    d_ = new Impl();
    d_->pipeline = pipeline;
    d_->sink     = GST_APP_SINK(sinkEl);   // owned by the pipeline

    GstStateChangeReturn r = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (r == GST_STATE_CHANGE_FAILURE) {
        lastError_ = "failed to set audio pipeline to PLAYING";
        stop();
        return false;
    }

    d_->pullThread = std::thread(&AudioEncoder::pullLoop, this);
    return true;
}

void AudioEncoder::attach(AddTrackFn addTrack, WriteFn write)
{
    if (!d_)
        return;
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->addTrack   = std::move(addTrack);
    d_->write      = std::move(write);
    d_->trackAdded = false;        // re-added to this recording's fresh muxer
    d_->active     = true;
}

void AudioEncoder::detach()
{
    if (!d_)
        return;
    // Holding the lock guarantees any in-flight pullLoop write has finished, so
    // the caller may then destroy the muxer/VideoEncoder safely.
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->active     = false;
    d_->trackAdded = false;
    d_->addTrack   = nullptr;
    d_->write      = nullptr;
}

void AudioEncoder::pullLoop()
{
    while (!d_->stopping.load()) {
        GstSample* sample = gst_app_sink_pull_sample(d_->sink);
        if (!sample)
            break;   // EOS or appsink stopped

        // Cache the AAC track format (rate/channels/csd-0) from the first sample.
        if (!d_->formatReady) {
            GstCaps* caps = gst_sample_get_caps(sample);
            int rate = 48000, ch = 1;
            std::vector<uint8_t> asc;
            GstBuffer* cdBuf = nullptr;
            GstMapInfo cdMap{};
            if (caps) {
                GstStructure* s = gst_caps_get_structure(caps, 0);
                gst_structure_get_int(s, "rate", &rate);
                gst_structure_get_int(s, "channels", &ch);
                const GValue* cd = gst_structure_get_value(s, "codec_data");
                if (cd && (cdBuf = gst_value_get_buffer(cd)) &&
                    gst_buffer_map(cdBuf, &cdMap, GST_MAP_READ)) {
                    asc.assign(cdMap.data, cdMap.data + cdMap.size);
                    gst_buffer_unmap(cdBuf, &cdMap);
                }
            }
            if (asc.empty())
                buildAscFallback(rate, ch, asc);
            std::lock_guard<std::mutex> lk(d_->mtx);
            d_->rate = rate;
            d_->channels = ch;
            d_->asc = std::move(asc);
            d_->formatReady = true;
        }

        // Mux the encoded AAC bytes (or discard them if not attached).
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            AMediaCodecBufferInfo info{};
            info.offset = 0;
            info.size   = (int32_t)map.size;
            info.flags  = 0;
            info.presentationTimeUs =
                GST_BUFFER_PTS_IS_VALID(buf) ? (int64_t)(GST_BUFFER_PTS(buf) / 1000) : 0;

            std::lock_guard<std::mutex> lk(d_->mtx);
            if (d_->active) {
                if (!d_->trackAdded && d_->addTrack && d_->formatReady) {
                    AMediaFormat* fmt = AMediaFormat_new();
                    AMediaFormat_setString(fmt, "mime", "audio/mp4a-latm");
                    AMediaFormat_setInt32(fmt, "sample-rate", d_->rate);
                    AMediaFormat_setInt32(fmt, "channel-count", d_->channels);
                    AMediaFormat_setInt32(fmt, "aac-profile", 2);   // AOT_LC
                    AMediaFormat_setBuffer(fmt, "csd-0", d_->asc.data(), d_->asc.size());
                    d_->addTrack(fmt);
                    AMediaFormat_delete(fmt);
                    d_->trackAdded = true;
                }
                if (d_->write)
                    d_->write(map.data, info);
            }
            gst_buffer_unmap(buf, &map);
            d_->frames.fetch_add(1, std::memory_order_relaxed);
        }

        gst_sample_unref(sample);
    }
}

void AudioEncoder::stop()
{
    if (!d_)
        return;

    d_->stopping.store(true);
    if (d_->pipeline) {
        // Setting the pipeline to NULL unblocks a pull_sample() waiting in the
        // pull thread (it returns NULL), letting the loop exit cleanly.
        gst_element_set_state(d_->pipeline, GST_STATE_NULL);
    }
    if (d_->pullThread.joinable())
        d_->pullThread.join();
    if (d_->pipeline) {
        gst_object_unref(d_->pipeline);   // also drops the sink ref
        d_->pipeline = nullptr;
    }
    delete d_;
    d_ = nullptr;
}

} // namespace furicam

#else  // !FURICAM_HAVE_AUDIO — build a stub so the symbols exist (video-only)

namespace furicam {

struct AudioEncoder::Impl {};

AudioEncoder::~AudioEncoder() { stop(); }

int  AudioEncoder::framesProduced() const { return 0; }
bool AudioEncoder::isRunning() const { return false; }
bool AudioEncoder::isReady()   const { return false; }

bool AudioEncoder::start(int, int, int)
{
    lastError_ = "audio support not compiled in (gstreamer-app unavailable)";
    return false;
}

void AudioEncoder::attach(AddTrackFn, WriteFn) {}
void AudioEncoder::detach()  {}
void AudioEncoder::stop()    {}
void AudioEncoder::pullLoop() {}

} // namespace furicam

#endif // FURICAM_HAVE_AUDIO
