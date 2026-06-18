// Qt headers must come before EGL/GLES — they define GL types that clash with X11 macros.
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QQuickFramebufferObject>

#include "camera2preview.h"
#include "camera2session.h"
#include "ndk_loader.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// Preview source dimensions — defined in camera2session.cpp

// ─── EGL renderer ────────────────────────────────────────────────────────────

class Camera2PreviewRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit Camera2PreviewRenderer(Camera2Preview* item) : m_item(item) {}
    ~Camera2PreviewRenderer() override { cleanup(); }

    QOpenGLFramebufferObject* createFramebufferObject(const QSize& size) override {
        m_viewportW = size.width();
        m_viewportH = size.height();
        QOpenGLFramebufferObjectFormat fmt;
        fmt.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, fmt);
    }

    void synchronize(QQuickFramebufferObject* fbo) override {
        m_displayRotation = static_cast<Camera2Preview*>(fbo)->displayRotation();
        m_session         = static_cast<Camera2Preview*>(fbo)->session();
    }

    void render() override {
        if (!m_initialised) init();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        AImageReader* reader = m_session ? m_session->previewReader() : nullptr;
        if (!reader || !m_pfn_eglGetNativeCB || !m_pfn_eglCreateImage) {
            update();
            return;
        }

        AImage* image = nullptr;
        if (g_mediandk.AImageReader_acquireLatestImage(reader, &image) == AMEDIA_OK && image) {
            AHardwareBuffer* ahb = nullptr;
            if (g_mediandk.AImage_getHardwareBuffer && 
                g_mediandk.AImage_getHardwareBuffer(image, &ahb) == AMEDIA_OK && ahb) {
                if (m_eglImage != EGL_NO_IMAGE_KHR) {
                    m_pfn_eglDestroyImage(eglGetCurrentDisplay(), m_eglImage);
                    m_eglImage = EGL_NO_IMAGE_KHR;
                }
                EGLClientBuffer cb = m_pfn_eglGetNativeCB(ahb);
                if (cb) {
                    static const EGLint attr[] = {EGL_NONE};
                    m_eglImage = m_pfn_eglCreateImage(
                        eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                        EGL_NATIVE_BUFFER_ANDROID, cb, attr);
                    if (m_eglImage != EGL_NO_IMAGE_KHR) {
                        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_previewTex);
                        m_pfn_glEGLImageTarget(GL_TEXTURE_EXTERNAL_OES, m_eglImage);
                    }
                }
            }
            g_mediandk.AImage_delete(image);
        }

        if (m_eglImage != EGL_NO_IMAGE_KHR) drawQuad();
        update();
    }

private:
    void init() {
        m_initialised = true;
        m_pfn_eglGetNativeCB  = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
            eglGetProcAddress("eglGetNativeClientBufferANDROID");
        m_pfn_eglCreateImage  = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        m_pfn_eglDestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        m_pfn_glEGLImageTarget = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        glGenTextures(1, &m_previewTex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_previewTex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        m_prog   = buildProgram();
        m_locPos = glGetAttribLocation(m_prog,  "a_position");
        m_locTc  = glGetAttribLocation(m_prog,  "a_texCoord");
        m_locTex = glGetUniformLocation(m_prog, "s_texture");
    }

    void cleanup() {
        if (m_eglImage != EGL_NO_IMAGE_KHR && m_pfn_eglDestroyImage)
            m_pfn_eglDestroyImage(eglGetCurrentDisplay(), m_eglImage);
        if (m_previewTex) glDeleteTextures(1, &m_previewTex);
        if (m_prog)       glDeleteProgram(m_prog);
    }

    void drawQuad() {
        // UV mapping per rotation (TL BL BR TR — each as u,v pair)
        static const GLfloat kUVs[4][8] = {
            { 1,1,  1,0,  0,0,  0,1 },  // 0°
            { 0,1,  1,1,  1,0,  0,0 },  // 90°
            { 0,0,  0,1,  1,1,  1,0 },  // 180°
            { 1,0,  0,0,  0,1,  1,1 },  // 270°
        };
        static const GLushort kIdx[] = {0, 1, 2, 0, 2, 3};
        const int rot = (m_displayRotation / 90) & 3;
        const GLfloat* uv = kUVs[rot];

        // Content AR after rotation (swap W/H for 90° and 270°)
        // ponytail: hardcoded to match kPreviewW/kPreviewH in camera2session.cpp
        float cAR = (rot & 1) ? (1080.f / 1440.f) : (1440.f / 1080.f);
        float vAR = (m_viewportH > 0) ? (float)m_viewportW / (float)m_viewportH : cAR;

        // "Contain" scaling: fit content inside viewport, letterbox if needed
        float sx = 1.f, sy = 1.f;
        if (cAR > vAR) sy = vAR / cAR;
        else            sx = cAR / vAR;

        GLfloat v[4][5] = {
            { -sx,  sy, 0.f, uv[0], uv[1] },  // TL
            { -sx, -sy, 0.f, uv[2], uv[3] },  // BL
            {  sx, -sy, 0.f, uv[4], uv[5] },  // BR
            {  sx,  sy, 0.f, uv[6], uv[7] },  // TR
        };

        glUseProgram(m_prog);
        glEnableVertexAttribArray(m_locPos);
        glEnableVertexAttribArray(m_locTc);
        glVertexAttribPointer(m_locPos, 3, GL_FLOAT, GL_FALSE, 5*sizeof(GLfloat), v);
        glVertexAttribPointer(m_locTc,  2, GL_FLOAT, GL_FALSE, 5*sizeof(GLfloat), v[0]+3);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_previewTex);
        glUniform1i(m_locTex, 0);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, kIdx);
        glDisableVertexAttribArray(m_locPos);
        glDisableVertexAttribArray(m_locTc);
    }

    static GLuint compileShader(GLenum type, const char* src) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char buf[512]; glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
            fprintf(stderr, "Camera2PreviewRenderer shader error: %s\n", buf);
        }
        return sh;
    }

    static GLuint buildProgram() {
        static const char* kVert =
            "#extension GL_OES_EGL_image_external : require\n"
            "attribute vec4 a_position;\n"
            "attribute vec2 a_texCoord;\n"
            "varying vec2 v_texCoord;\n"
            "void main() { gl_Position = a_position; v_texCoord = a_texCoord; }\n";
        static const char* kFrag =
            "#extension GL_OES_EGL_image_external : require\n"
            "precision mediump float;\n"
            "varying vec2 v_texCoord;\n"
            "uniform samplerExternalOES s_texture;\n"
            "void main() { gl_FragColor = texture2D(s_texture, v_texCoord); }\n";
        GLuint vert = compileShader(GL_VERTEX_SHADER,   kVert);
        GLuint frag = compileShader(GL_FRAGMENT_SHADER, kFrag);
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vert); glAttachShader(prog, frag);
        glLinkProgram(prog);
        glDeleteShader(vert); glDeleteShader(frag);
        return prog;
    }

    Camera2Preview* m_item        = nullptr;
    Camera2Session* m_session     = nullptr;
    bool   m_initialised          = false;
    int    m_displayRotation      = 90;
    int    m_viewportW            = 0;
    int    m_viewportH            = 0;
    GLuint m_previewTex           = 0;
    GLuint m_prog                 = 0;
    GLint  m_locPos               = -1;
    GLint  m_locTc                = -1;
    GLint  m_locTex               = -1;
    EGLImageKHR m_eglImage        = EGL_NO_IMAGE_KHR;

    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC m_pfn_eglGetNativeCB  = nullptr;
    PFNEGLCREATEIMAGEKHRPROC               m_pfn_eglCreateImage  = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC              m_pfn_eglDestroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC    m_pfn_glEGLImageTarget = nullptr;
};

// ─── Camera2Preview QQuickFramebufferObject ───────────────────────────────────

Camera2Preview::Camera2Preview(QQuickItem* parent)
    : QQuickFramebufferObject(parent)
{
    setMirrorVertically(true);
}

QQuickFramebufferObject::Renderer* Camera2Preview::createRenderer() const
{
    return new Camera2PreviewRenderer(const_cast<Camera2Preview*>(this));
}

void Camera2Preview::setSession(Camera2Session* s)
{
    if (m_session == s) return;

    if (m_session)
        disconnect(m_session, nullptr, this, nullptr);

    m_session = s;

    if (m_session) {
        // Trigger a repaint whenever a new preview frame should be available.
        // Camera2 fires the image listener continuously; we poll in the renderer
        // so we only need to call update() at a reasonable rate.
        // The renderer calls update() from within render() to keep the loop going.
    }

    emit sessionChanged();
    update();
}
