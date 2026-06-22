// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// PreviewRenderer implementation — see PreviewRenderer.h.  All GL/EGL entry
// points are resolved at runtime via eglGetProcAddress so this file needs no
// GLES headers (only EGL, present everywhere).

#include "PreviewRenderer.h"

#include <EGL/egl.h>

#include <cmath>
#include <cstdio>

namespace furicam2 {

namespace {

// Minimal GL typedefs / constants (we never include <GLES2/gl2.h>).
typedef unsigned int  GLenum;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef char          GLchar;

constexpr GLenum GL_FRAGMENT_SHADER      = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER        = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS       = 0x8B81;
constexpr GLenum GL_LINK_STATUS          = 0x8B82;
constexpr GLenum GL_FLOAT                = 0x1406;
constexpr GLenum GL_TRIANGLE_STRIP       = 0x0005;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_TEXTURE0             = 0x84C0;
constexpr GLenum GL_TEXTURE_EXTERNAL_OES = 0x8D65;
constexpr GLenum GL_TEXTURE_MIN_FILTER   = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER   = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S       = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T       = 0x2803;
constexpr GLint  GL_LINEAR               = 0x2601;
constexpr GLint  GL_CLAMP_TO_EDGE        = 0x812F;
constexpr GLint  GL_FALSE_               = 0;

#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif

using EGLImageKHR = void*;

const char* kVert =
    "attribute vec2 aPos;\n"
    "attribute vec2 aTex;\n"
    "uniform mat2 uTexRot;\n"
    "uniform vec2 uCrop;\n"     // centred sub-rect of the stream texture to show
    "uniform float uMirror;\n"  // -1 mirrors the preview left-right (front camera)
    "varying vec2 vTex;\n"
    "void main() {\n"
    "    vTex = uCrop * (uTexRot * (aTex - vec2(0.5))) + vec2(0.5);\n"
    "    gl_Position = vec4(aPos.x * uMirror, aPos.y, 0.0, 1.0);\n"
    "}\n";

const char* kFrag =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 vTex;\n"
    "uniform samplerExternalOES uTex;\n"
    "uniform float uZebra;\n"   // 0=off; 1=mark blown highlights (red) + crushed shadows (blue)
    "void main() {\n"
    "    vec4 c = texture2D(uTex, vTex);\n"
    "    if (uZebra > 0.5) {\n"
    "        float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float s = step(0.5, fract((gl_FragCoord.x + gl_FragCoord.y) * 0.0625));\n"  // diagonal stripes
    "        if (l >= 0.96) c.rgb = mix(c.rgb, vec3(1.0, 0.1, 0.1), s);\n"
    "        else if (l <= 0.045) c.rgb = mix(c.rgb, vec3(0.2, 0.5, 1.0), s);\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

template <typename T>
T load(const char* name)
{
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

} // namespace

struct PreviewRenderer::Impl {
    // GL functions.
    GLuint (*glCreateShader)(GLenum) = nullptr;
    void   (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void   (*glCompileShader)(GLuint) = nullptr;
    void   (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void   (*glDeleteShader)(GLuint) = nullptr;
    GLuint (*glCreateProgram)() = nullptr;
    void   (*glAttachShader)(GLuint, GLuint) = nullptr;
    void   (*glLinkProgram)(GLuint) = nullptr;
    void   (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void   (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void   (*glDeleteProgram)(GLuint) = nullptr;
    void   (*glUseProgram)(GLuint) = nullptr;
    void   (*glGenTextures)(GLsizei, GLuint*) = nullptr;
    void   (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void   (*glBindTexture)(GLenum, GLuint) = nullptr;
    void   (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
    GLint  (*glGetAttribLocation)(GLuint, const GLchar*) = nullptr;
    GLint  (*glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void   (*glUniform1i)(GLint, GLint) = nullptr;
    void   (*glUniform1f)(GLint, GLfloat) = nullptr;
    void   (*glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void   (*glUniformMatrix2fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
    void   (*glActiveTexture)(GLenum) = nullptr;
    void   (*glEnableVertexAttribArray)(GLuint) = nullptr;
    void   (*glDisableVertexAttribArray)(GLuint) = nullptr;
    void   (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void   (*glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void   (*glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void   (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void   (*glClear)(GLbitfield) = nullptr;
    // EGL / OES extension functions.
    EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, GLenum, EGLClientBuffer, const EGLint*) = nullptr;
    GLboolean   (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR) = nullptr;
    EGLClientBuffer (*eglGetNativeClientBufferANDROID)(const AHardwareBuffer*) = nullptr;
    void   (*glEGLImageTargetTexture2DOES)(GLenum, EGLImageKHR) = nullptr;

    EGLDisplay  dpy       = EGL_NO_DISPLAY;
    EGLImageKHR eglImage  = nullptr;
    AImage*     held      = nullptr;
    GLuint      program   = 0;
    GLuint      texture   = 0;
    GLint       aPos = -1, aTex = -1, uTex = -1, uRot = -1, uCrop = -1, uMirror = -1, uZebra = -1;
    bool        inited    = false;
    bool        ok        = false;
    bool        haveFrame = false;

    GLuint compile(GLenum type, const char* src)
    {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint okv = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &okv);
        if (!okv) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::fprintf(stderr, "PreviewRenderer: shader compile failed: %s\n", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool init()
    {
        inited = true;
        glCreateShader      = load<GLuint(*)(GLenum)>("glCreateShader");
        glShaderSource      = load<void(*)(GLuint, GLsizei, const GLchar* const*, const GLint*)>("glShaderSource");
        glCompileShader     = load<void(*)(GLuint)>("glCompileShader");
        glGetShaderiv       = load<void(*)(GLuint, GLenum, GLint*)>("glGetShaderiv");
        glGetShaderInfoLog  = load<void(*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetShaderInfoLog");
        glDeleteShader      = load<void(*)(GLuint)>("glDeleteShader");
        glCreateProgram     = load<GLuint(*)()>("glCreateProgram");
        glAttachShader      = load<void(*)(GLuint, GLuint)>("glAttachShader");
        glLinkProgram       = load<void(*)(GLuint)>("glLinkProgram");
        glGetProgramiv      = load<void(*)(GLuint, GLenum, GLint*)>("glGetProgramiv");
        glGetProgramInfoLog = load<void(*)(GLuint, GLsizei, GLsizei*, GLchar*)>("glGetProgramInfoLog");
        glDeleteProgram     = load<void(*)(GLuint)>("glDeleteProgram");
        glUseProgram        = load<void(*)(GLuint)>("glUseProgram");
        glGenTextures       = load<void(*)(GLsizei, GLuint*)>("glGenTextures");
        glDeleteTextures    = load<void(*)(GLsizei, const GLuint*)>("glDeleteTextures");
        glBindTexture       = load<void(*)(GLenum, GLuint)>("glBindTexture");
        glTexParameteri     = load<void(*)(GLenum, GLenum, GLint)>("glTexParameteri");
        glGetAttribLocation = load<GLint(*)(GLuint, const GLchar*)>("glGetAttribLocation");
        glGetUniformLocation = load<GLint(*)(GLuint, const GLchar*)>("glGetUniformLocation");
        glUniform1i         = load<void(*)(GLint, GLint)>("glUniform1i");
        glUniform1f         = load<void(*)(GLint, GLfloat)>("glUniform1f");
        glUniform2f         = load<void(*)(GLint, GLfloat, GLfloat)>("glUniform2f");
        glUniformMatrix2fv  = load<void(*)(GLint, GLsizei, GLboolean, const GLfloat*)>("glUniformMatrix2fv");
        glActiveTexture     = load<void(*)(GLenum)>("glActiveTexture");
        glEnableVertexAttribArray  = load<void(*)(GLuint)>("glEnableVertexAttribArray");
        glDisableVertexAttribArray = load<void(*)(GLuint)>("glDisableVertexAttribArray");
        glVertexAttribPointer = load<void(*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)>("glVertexAttribPointer");
        glDrawArrays        = load<void(*)(GLenum, GLint, GLsizei)>("glDrawArrays");
        glViewport          = load<void(*)(GLint, GLint, GLsizei, GLsizei)>("glViewport");
        glClearColor        = load<void(*)(GLfloat, GLfloat, GLfloat, GLfloat)>("glClearColor");
        glClear             = load<void(*)(GLbitfield)>("glClear");

        eglCreateImageKHR   = load<EGLImageKHR(*)(EGLDisplay, EGLContext, GLenum, EGLClientBuffer, const EGLint*)>("eglCreateImageKHR");
        eglDestroyImageKHR  = load<GLboolean(*)(EGLDisplay, EGLImageKHR)>("eglDestroyImageKHR");
        eglGetNativeClientBufferANDROID = load<EGLClientBuffer(*)(const AHardwareBuffer*)>("eglGetNativeClientBufferANDROID");
        glEGLImageTargetTexture2DOES = load<void(*)(GLenum, EGLImageKHR)>("glEGLImageTargetTexture2DOES");

        dpy = eglGetCurrentDisplay();

        if (!glCreateShader || !glCreateProgram || !eglCreateImageKHR
            || !eglGetNativeClientBufferANDROID || !glEGLImageTargetTexture2DOES
            || dpy == EGL_NO_DISPLAY) {
            std::fprintf(stderr, "PreviewRenderer: failed to resolve GL/EGL functions\n");
            return false;
        }

        GLuint vs = compile(GL_VERTEX_SHADER, kVert);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
        if (!vs || !fs)
            return false;
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            std::fprintf(stderr, "PreviewRenderer: link failed: %s\n", log);
            return false;
        }
        aPos = glGetAttribLocation(program, "aPos");
        aTex = glGetAttribLocation(program, "aTex");
        uTex = glGetUniformLocation(program, "uTex");
        uRot = glGetUniformLocation(program, "uTexRot");
        uCrop = glGetUniformLocation(program, "uCrop");
        uMirror = glGetUniformLocation(program, "uMirror");
    uZebra = glGetUniformLocation(program, "uZebra");
        glGenTextures(1, &texture);
        ok = true;
        return true;
    }
};

PreviewRenderer::PreviewRenderer() : d_(new Impl) {}

PreviewRenderer::~PreviewRenderer()
{
    cleanup();
    delete d_;
}

void PreviewRenderer::cleanup()
{
    if (!d_)
        return;
    if (d_->eglImage && d_->eglDestroyImageKHR && d_->dpy != EGL_NO_DISPLAY) {
        d_->eglDestroyImageKHR(d_->dpy, d_->eglImage);
        d_->eglImage = nullptr;
    }
    if (d_->held) {
        AImage_delete(d_->held);
        d_->held = nullptr;
    }
    if (d_->ok && d_->program && d_->glDeleteProgram) {
        d_->glDeleteProgram(d_->program);
        d_->program = 0;
        if (d_->texture)
            d_->glDeleteTextures(1, &d_->texture);
        d_->texture = 0;
    }
    d_->ok = false;
    d_->haveFrame = false;
}

bool PreviewRenderer::render(AImageReader* reader, int viewW, int viewH, int rotationDeg,
                             float cropX, float cropY, bool mirror, bool zebra)
{
    Impl& d = *d_;
    if (!d.inited)
        d.init();
    if (!d.ok)
        return false;

    // Pull the freshest frame, if any.
    if (reader) {
        AImage* img = nullptr;
        if (AImageReader_acquireLatestImage(reader, &img) == AMEDIA_OK && img) {
            AHardwareBuffer* ahb = nullptr;
            if (AImage_getHardwareBuffer(img, &ahb) == AMEDIA_OK && ahb) {
                if (d.eglImage) {
                    d.eglDestroyImageKHR(d.dpy, d.eglImage);
                    d.eglImage = nullptr;
                }
                if (d.held)
                    AImage_delete(d.held);
                d.held = img;
                EGLClientBuffer cb = d.eglGetNativeClientBufferANDROID(ahb);
                EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE };
                d.eglImage = d.eglCreateImageKHR(d.dpy, EGL_NO_CONTEXT,
                                                 EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
                if (d.eglImage) {
                    d.glBindTexture(GL_TEXTURE_EXTERNAL_OES, d.texture);
                    d.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, d.eglImage);
                    d.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    d.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    d.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    d.glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    d.haveFrame = true;
                }
            } else {
                AImage_delete(img);
            }
        }
    }

    d.glViewport(0, 0, viewW, viewH);
    d.glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    d.glClear(GL_COLOR_BUFFER_BIT);

    if (d.haveFrame && d.eglImage) {
        const float r = rotationDeg * 3.14159265f / 180.0f;
        const float c = std::cos(r), s = std::sin(r);
        const float rot[4] = { c, s, -s, c };   // column-major mat2

        d.glUseProgram(d.program);
        d.glUniformMatrix2fv(d.uRot, 1, GL_FALSE_, rot);
        if (d.uCrop >= 0)
            d.glUniform2f(d.uCrop, cropX, cropY);
        if (d.uMirror >= 0)
            d.glUniform1f(d.uMirror, mirror ? -1.0f : 1.0f);
        if (d.uZebra >= 0)
            d.glUniform1f(d.uZebra, zebra ? 1.0f : 0.0f);
        d.glActiveTexture(GL_TEXTURE0);
        d.glBindTexture(GL_TEXTURE_EXTERNAL_OES, d.texture);
        d.glUniform1i(d.uTex, 0);

        static const float pos[] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
        static const float tex[] = {  0.f,  1.f, 1.f,  1.f,  0.f, 0.f, 1.f, 0.f };
        d.glEnableVertexAttribArray((GLuint)d.aPos);
        d.glEnableVertexAttribArray((GLuint)d.aTex);
        d.glVertexAttribPointer((GLuint)d.aPos, 2, GL_FLOAT, GL_FALSE_, 0, pos);
        d.glVertexAttribPointer((GLuint)d.aTex, 2, GL_FLOAT, GL_FALSE_, 0, tex);
        d.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        d.glDisableVertexAttribArray((GLuint)d.aPos);
        d.glDisableVertexAttribArray((GLuint)d.aTex);
    }
    d.glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    return d.haveFrame;
}

} // namespace furicam2
