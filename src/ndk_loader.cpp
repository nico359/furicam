#include "ndk_loader.h"
#include <dlfcn.h>
#include <cstdio>

MediaNDK   g_mediandk   = {};
Camera2NDK g_camera2ndk = {};

// Bootstrap: load libhybris-common.so via the system linker to get
// android_dlopen / android_dlsym — no hybris headers needed at build time.
static void* (*g_android_dlopen)(const char*, int)   = nullptr;
static void* (*g_android_dlsym)(void*, const char*)  = nullptr;
static const char* (*g_android_dlerror)()            = nullptr;

static bool init_hybris() {
    static bool done = false;
    static bool ok   = false;
    if (done) return ok;
    done = true;
    void* hyb = dlopen("libhybris-common.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!hyb) {
        fprintf(stderr, "ndk_loader: dlopen libhybris-common.so.1: %s\n", dlerror());
        return false;
    }
    g_android_dlopen  = (void*(*)(const char*,int))  dlsym(hyb, "android_dlopen");
    g_android_dlsym   = (void*(*)(void*,const char*))dlsym(hyb, "android_dlsym");
    g_android_dlerror = (const char*(*)())           dlsym(hyb, "android_dlerror");
    ok = g_android_dlopen && g_android_dlsym;
    if (!ok) fprintf(stderr, "ndk_loader: missing android_dlopen/android_dlsym in hybris\n");
    return ok;
}

#define LOAD_SYM(handle, st, name) \
    do { \
        (st).name = (decltype((st).name)) g_android_dlsym(handle, #name); \
        if (!(st).name) { \
            fprintf(stderr, "ndk_loader: missing symbol %s\n", #name); \
            ok = false; \
        } \
    } while (0)

#define LOAD_SYM_OPT(handle, st, name) \
    do { \
        (st).name = (decltype((st).name)) g_android_dlsym(handle, #name); \
    } while (0)

bool load_camera2ndk(Camera2NDK& out) {
    if (!init_hybris()) return false;
    void* lib = g_android_dlopen("libcamera2ndk.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "ndk_loader: failed to open libcamera2ndk.so\n");
        return false;
    }
    bool ok = true;
    LOAD_SYM(lib, out, ACameraManager_create);
    LOAD_SYM(lib, out, ACameraManager_delete);
    LOAD_SYM(lib, out, ACameraManager_getCameraIdList);
    LOAD_SYM(lib, out, ACameraManager_deleteCameraIdList);
    LOAD_SYM(lib, out, ACameraManager_getCameraCharacteristics);
    LOAD_SYM(lib, out, ACameraManager_openCamera);
    LOAD_SYM(lib, out, ACameraMetadata_getConstEntry);
    LOAD_SYM(lib, out, ACameraMetadata_free);
    LOAD_SYM_OPT(lib, out, ACameraMetadata_copy);
    LOAD_SYM(lib, out, ACameraDevice_close);
    LOAD_SYM(lib, out, ACameraDevice_createCaptureSession);
    LOAD_SYM(lib, out, ACameraDevice_createCaptureRequest);
    LOAD_SYM(lib, out, ACaptureSessionOutputContainer_create);
    LOAD_SYM(lib, out, ACaptureSessionOutputContainer_free);
    LOAD_SYM(lib, out, ACaptureSessionOutputContainer_add);
    LOAD_SYM_OPT(lib, out, ACaptureSessionOutputContainer_remove);
    LOAD_SYM(lib, out, ACaptureSessionOutput_create);
    LOAD_SYM(lib, out, ACaptureSessionOutput_free);
    LOAD_SYM(lib, out, ACameraOutputTarget_create);
    LOAD_SYM(lib, out, ACameraOutputTarget_free);
    LOAD_SYM(lib, out, ACaptureRequest_addTarget);
    LOAD_SYM_OPT(lib, out, ACaptureRequest_removeTarget);
    LOAD_SYM(lib, out, ACaptureRequest_setEntry_u8);
    LOAD_SYM(lib, out, ACaptureRequest_setEntry_i32);
    LOAD_SYM(lib, out, ACaptureRequest_setEntry_i64);
    LOAD_SYM_OPT(lib, out, ACaptureRequest_setEntry_float);
    LOAD_SYM_OPT(lib, out, ACaptureRequest_setEntry_double);
    LOAD_SYM_OPT(lib, out, ACaptureRequest_setEntry_rational);
    LOAD_SYM_OPT(lib, out, ACaptureRequest_getConstEntry);
    LOAD_SYM(lib, out, ACaptureRequest_free);
    LOAD_SYM(lib, out, ACameraCaptureSession_setRepeatingRequest);
    LOAD_SYM(lib, out, ACameraCaptureSession_capture);
    LOAD_SYM(lib, out, ACameraCaptureSession_stopRepeating);
    LOAD_SYM(lib, out, ACameraCaptureSession_close);
    LOAD_SYM_OPT(lib, out, ACameraCaptureSession_abortCaptures);
    if (ok) g_camera2ndk = out;
    return ok;
}

bool start_binder_thread_pool() {
    if (!init_hybris()) return false;
    void* lib = g_android_dlopen("libbinder_ndk.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "ndk_loader: failed to open libbinder_ndk.so\n");
        return false;
    }
    auto start  = (void(*)()) g_android_dlsym(lib, "ABinderProcess_startThreadPool");
    if (!start) {
        fprintf(stderr, "ndk_loader: missing ABinderProcess_startThreadPool\n");
        return false;
    }
    start();
    return true;
}

bool load_mediandk(MediaNDK& out) {
    if (!init_hybris()) return false;
    void* lib = g_android_dlopen("libmediandk.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "ndk_loader: failed to open libmediandk.so\n");
        return false;
    }
    bool ok = true;
    LOAD_SYM(lib, out, AImageReader_new);
    LOAD_SYM_OPT(lib, out, AImageReader_newWithUsage);
    LOAD_SYM(lib, out, AImageReader_delete);
    LOAD_SYM(lib, out, AImageReader_getWindow);
    LOAD_SYM(lib, out, AImageReader_setImageListener);
    LOAD_SYM(lib, out, AImageReader_acquireLatestImage);
    LOAD_SYM(lib, out, AImageReader_acquireNextImage);
    LOAD_SYM_OPT(lib, out, AImageReader_getMaxImages);
    LOAD_SYM(lib, out, AImage_delete);
    LOAD_SYM_OPT(lib, out, AImage_getHardwareBuffer);
    LOAD_SYM(lib, out, AImage_getWidth);
    LOAD_SYM(lib, out, AImage_getHeight);
    LOAD_SYM(lib, out, AImage_getNumberOfPlanes);
    LOAD_SYM(lib, out, AImage_getFormat);
    LOAD_SYM(lib, out, AImage_getPlaneData);
    LOAD_SYM(lib, out, AImage_getPlaneRowStride);
    LOAD_SYM(lib, out, AImage_getPlanePixelStride);
    LOAD_SYM(lib, out, AImage_getTimestamp);
    if (ok) g_mediandk = out;
    return ok;
}
