/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * furicam2_camera_c.h — a backend-neutral C API over the camera engine.
 *
 * This is the seam that lets non-C++/non-Qt frontends (GTK, Rust, Flutter, …)
 * drive the same engine the Qt app uses.  The vocabulary is deliberately
 * generic ("open camera / start preview / set control / capture / record")
 * rather than Camera2-specific, so a future libcamera- or V4L2-backed engine
 * could implement the same API without changing any frontend.
 */

#ifndef FURICAM2_CAMERA_C_H
#define FURICAM2_CAMERA_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fc2_camera fc2_camera;

#define FC2_FACING_BACK  1
#define FC2_FACING_FRONT 0

/* 1 if a real camera backend is reachable (0 on a non-Halium host stub build). */
int fc2_available(void);

/* Open the engine and the first camera matching `facing`.  NULL on failure. */
fc2_camera* fc2_open(int facing);
void        fc2_close(fc2_camera* c);

/* Sensor mount orientation in degrees — the frontend uses it to rotate preview. */
int fc2_sensor_orientation(fc2_camera* c);

/* Preview (PRIVATE/GPU buffers for the zero-copy GL path).  1 on success. */
int  fc2_start_preview(fc2_camera* c, int width, int height);
void fc2_stop_preview(fc2_camera* c);

/* Draw the latest preview frame into the CURRENT GL context, filling
 * [0,0,view_w,view_h] rotated `rotation_deg`.  Returns 1 if a frame was drawn. */
int  fc2_render_preview(fc2_camera* c, int view_w, int view_h, int rotation_deg);

/* Called on a camera thread when a new frame is ready (schedule a redraw). */
void fc2_set_frame_callback(fc2_camera* c, void (*cb)(void* user), void* user);

/* Controls. */
void  fc2_set_white_balance(fc2_camera* c, int app_mode); /* 0=Auto 1=Daylight 2=Cloudy 3=Fluor 4=Incand */
void  fc2_set_zoom(fc2_camera* c, float ratio);           /* 1 .. fc2_max_zoom */
float fc2_max_zoom(fc2_camera* c);
void  fc2_set_torch(fc2_camera* c, int on);
void  fc2_set_ae_lock(fc2_camera* c, int lock);
void  fc2_set_awb_lock(fc2_camera* c, int lock);
void  fc2_set_auto_exposure(fc2_camera* c);
void  fc2_set_manual_exposure(fc2_camera* c, int iso, int64_t exposure_ns);
void  fc2_set_focus_point(fc2_camera* c, float x, float y); /* normalized [0,1] */

/* Capture / record.  Capture and recording finish asynchronously. */
int  fc2_capture_photo(fc2_camera* c, const char* path);
int  fc2_start_recording(fc2_camera* c, const char* path, int width, int height, int fps, int bitrate);
void fc2_stop_recording(fc2_camera* c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FURICAM2_CAMERA_C_H */
