// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// furicam2_camera_c.cpp — C API implementation over CameraSession + PreviewRenderer.

#include "furicam2_camera_c.h"

#include "CameraSession.h"
#include "PreviewRenderer.h"

#include <new>
#include <string>

using furicam2::CameraSession;
using furicam2::PreviewRenderer;

struct fc2_camera {
    CameraSession   session;
    PreviewRenderer renderer;
    std::string     openId;
    int             sensorOrientation = 0;
};

extern "C" {

int fc2_available(void)
{
    return CameraSession::isHostStub() ? 0 : 1;
}

fc2_camera* fc2_open(int facing)
{
    if (CameraSession::isHostStub())
        return nullptr;
    auto* c = new (std::nothrow) fc2_camera();
    if (!c)
        return nullptr;
    if (!c->session.enumerate() || c->session.cameras().empty()) {
        delete c;
        return nullptr;
    }
    const auto& cams = c->session.cameras();
    std::string id = cams.front().id;
    int orient = cams.front().sensorOrientation;
    for (const auto& ci : cams) {
        if (ci.facing == facing) {
            id = ci.id;
            orient = ci.sensorOrientation;
            break;
        }
    }
    if (!c->session.open(id)) {
        delete c;
        return nullptr;
    }
    c->openId = id;
    c->sensorOrientation = orient;
    return c;
}

void fc2_close(fc2_camera* c)
{
    if (!c)
        return;
    c->session.close();
    delete c;
}

int fc2_sensor_orientation(fc2_camera* c) { return c ? c->sensorOrientation : 0; }

int fc2_start_preview(fc2_camera* c, int width, int height)
{
    if (!c)
        return 0;
    return c->session.startPreview(width, height, AIMAGE_FORMAT_PRIVATE,
                                   AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 30, true)
               ? 1 : 0;
}

void fc2_stop_preview(fc2_camera* c) { if (c) c->session.stopPreview(); }

int fc2_render_preview(fc2_camera* c, int view_w, int view_h, int rotation_deg)
{
    if (!c)
        return 0;
    return c->renderer.render(c->session.previewReader(), view_w, view_h, rotation_deg) ? 1 : 0;
}

void fc2_set_frame_callback(fc2_camera* c, void (*cb)(void*), void* user)
{
    if (!c)
        return;
    if (cb)
        c->session.setFrameCallback([cb, user] { cb(user); });
    else
        c->session.setFrameCallback({});
}

void fc2_set_white_balance(fc2_camera* c, int app_mode)
{
    if (!c)
        return;
    int m;
    switch (app_mode) {
        case 1:  m = ACAMERA_CONTROL_AWB_MODE_DAYLIGHT;        break;
        case 2:  m = ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT; break;
        case 3:  m = ACAMERA_CONTROL_AWB_MODE_FLUORESCENT;     break;
        case 4:  m = ACAMERA_CONTROL_AWB_MODE_INCANDESCENT;    break;
        default: m = ACAMERA_CONTROL_AWB_MODE_AUTO;            break;
    }
    c->session.setAwbMode(m);
}

void  fc2_set_zoom(fc2_camera* c, float ratio) { if (c) c->session.setZoomRatio(ratio); }
float fc2_max_zoom(fc2_camera* c)              { return c ? c->session.maxZoomRatio() : 4.0f; }
void  fc2_set_torch(fc2_camera* c, int on)     { if (c) c->session.setTorch(on != 0); }
void  fc2_set_ae_lock(fc2_camera* c, int lock) { if (c) c->session.setAeLock(lock != 0); }
void  fc2_set_awb_lock(fc2_camera* c, int lock){ if (c) c->session.setAwbLock(lock != 0); }
void  fc2_set_auto_exposure(fc2_camera* c)     { if (c) c->session.setAutoExposure(); }

void fc2_set_manual_exposure(fc2_camera* c, int iso, int64_t exposure_ns)
{
    if (c)
        c->session.setManualExposure(iso, exposure_ns);
}

void fc2_set_focus_point(fc2_camera* c, float x, float y) { if (c) c->session.setFocusPoint(x, y); }

int fc2_capture_photo(fc2_camera* c, const char* path)
{
    return (c && path && c->session.capturePhoto(path)) ? 1 : 0;
}

int fc2_start_recording(fc2_camera* c, const char* path, int width, int height, int fps, int bitrate)
{
    return (c && path && c->session.startRecording(path, width, height, fps, bitrate)) ? 1 : 0;
}

void fc2_stop_recording(fc2_camera* c) { if (c) c->session.stopRecording(); }

} // extern "C"
