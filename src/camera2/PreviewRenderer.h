// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// PreviewRenderer — Qt-free GL preview renderer shared by every frontend.
//
// Draws the latest camera frame (from a PRIVATE-format AImageReader) as a
// GL_TEXTURE_EXTERNAL_OES quad into the *currently bound* GL context, via the
// zero-copy path AImage -> AHardwareBuffer -> eglCreateImageKHR -> external OES
// texture.  It loads all GL/EGL entry points through eglGetProcAddress, so it
// pulls in no GLES headers — it compiles on the desktop (no glesv2) and runs on
// the phone, and is usable from Qt (QQuickFramebufferObject), GTK (GtkGLArea),
// or the C wrapper alike.

#ifndef FURICAM2_PREVIEW_RENDERER_H
#define FURICAM2_PREVIEW_RENDERER_H

#include "Camera2NDK.h"   // AImageReader / AImage / AHardwareBuffer

namespace furicam2 {

class PreviewRenderer {
public:
    PreviewRenderer();
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&)            = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    // Call with a current GL context.  Acquires the freshest frame from `reader`
    // (keeps showing the previous one if none is ready), draws it filling
    // [0,0,viewW,viewH] rotated `rotationDeg` (texcoord rotation for the sensor
    // mount angle).  Lazily initialises GL state on first call.  Returns true if
    // a frame has ever been drawn.
    bool render(AImageReader* reader, int viewW, int viewH, int rotationDeg);

    // Release GL + EGLImage resources.  Must be called with the GL context current.
    void cleanup();

private:
    struct Impl;
    Impl* d_;
};

} // namespace furicam2

#endif // FURICAM2_PREVIEW_RENDERER_H
