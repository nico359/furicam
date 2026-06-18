#pragma once

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>

// Full Camera2 NDK vtable — every symbol used by the motioncam camera layer.
struct Camera2NDK {
    // ACameraManager
    ACameraManager*  (*ACameraManager_create)();
    void             (*ACameraManager_delete)(ACameraManager*);
    camera_status_t  (*ACameraManager_getCameraIdList)(ACameraManager*, ACameraIdList**);
    void             (*ACameraManager_deleteCameraIdList)(ACameraIdList*);
    camera_status_t  (*ACameraManager_getCameraCharacteristics)(ACameraManager*,
                         const char*, ACameraMetadata**);
    camera_status_t  (*ACameraManager_openCamera)(ACameraManager*, const char*,
                         ACameraDevice_StateCallbacks*, ACameraDevice**);

    // ACameraMetadata
    camera_status_t  (*ACameraMetadata_getConstEntry)(const ACameraMetadata*, uint32_t,
                         ACameraMetadata_const_entry*);
    void             (*ACameraMetadata_free)(ACameraMetadata*);
    ACameraMetadata* (*ACameraMetadata_copy)(const ACameraMetadata*);

    // ACameraDevice
    camera_status_t  (*ACameraDevice_close)(ACameraDevice*);
    camera_status_t  (*ACameraDevice_createCaptureSession)(ACameraDevice*,
                         const ACaptureSessionOutputContainer*,
                         const ACameraCaptureSession_stateCallbacks*,
                         ACameraCaptureSession**);
    camera_status_t  (*ACameraDevice_createCaptureRequest)(const ACameraDevice*,
                         ACameraDevice_request_template, ACaptureRequest**);

    // ACaptureSessionOutputContainer
    camera_status_t  (*ACaptureSessionOutputContainer_create)(ACaptureSessionOutputContainer**);
    void             (*ACaptureSessionOutputContainer_free)(ACaptureSessionOutputContainer*);
    camera_status_t  (*ACaptureSessionOutputContainer_add)(ACaptureSessionOutputContainer*,
                         const ACaptureSessionOutput*);
    camera_status_t  (*ACaptureSessionOutputContainer_remove)(ACaptureSessionOutputContainer*,
                         const ACaptureSessionOutput*);

    // ACaptureSessionOutput
    camera_status_t  (*ACaptureSessionOutput_create)(ANativeWindow*, ACaptureSessionOutput**);
    void             (*ACaptureSessionOutput_free)(ACaptureSessionOutput*);

    // ACameraOutputTarget
    camera_status_t  (*ACameraOutputTarget_create)(ANativeWindow*, ACameraOutputTarget**);
    void             (*ACameraOutputTarget_free)(ACameraOutputTarget*);

    // ACaptureRequest — setters
    camera_status_t  (*ACaptureRequest_addTarget)(ACaptureRequest*, const ACameraOutputTarget*);
    camera_status_t  (*ACaptureRequest_removeTarget)(ACaptureRequest*, const ACameraOutputTarget*);
    camera_status_t  (*ACaptureRequest_setEntry_u8)(ACaptureRequest*, uint32_t, uint32_t, const uint8_t*);
    camera_status_t  (*ACaptureRequest_setEntry_i32)(ACaptureRequest*, uint32_t, uint32_t, const int32_t*);
    camera_status_t  (*ACaptureRequest_setEntry_i64)(ACaptureRequest*, uint32_t, uint32_t, const int64_t*);
    camera_status_t  (*ACaptureRequest_setEntry_float)(ACaptureRequest*, uint32_t, uint32_t, const float*);
    camera_status_t  (*ACaptureRequest_setEntry_double)(ACaptureRequest*, uint32_t, uint32_t, const double*);
    camera_status_t  (*ACaptureRequest_setEntry_rational)(ACaptureRequest*, uint32_t, uint32_t, const ACameraMetadata_rational*);
    // ACaptureRequest — getters
    camera_status_t  (*ACaptureRequest_getConstEntry)(const ACaptureRequest*, uint32_t, ACameraMetadata_const_entry*);
    void             (*ACaptureRequest_free)(ACaptureRequest*);

    // ACameraCaptureSession
    camera_status_t  (*ACameraCaptureSession_setRepeatingRequest)(ACameraCaptureSession*,
                         ACameraCaptureSession_captureCallbacks*, int, ACaptureRequest**, int*);
    camera_status_t  (*ACameraCaptureSession_capture)(ACameraCaptureSession*,
                         ACameraCaptureSession_captureCallbacks*, int, ACaptureRequest**, int*);
    camera_status_t  (*ACameraCaptureSession_stopRepeating)(ACameraCaptureSession*);
    void             (*ACameraCaptureSession_close)(ACameraCaptureSession*);
    camera_status_t  (*ACameraCaptureSession_abortCaptures)(ACameraCaptureSession*);
};

struct MediaNDK {
    media_status_t  (*AImageReader_new)(int32_t, int32_t, int32_t, int32_t, AImageReader**);
    media_status_t  (*AImageReader_newWithUsage)(int32_t, int32_t, int32_t,
                        uint64_t, int32_t, AImageReader**);
    void            (*AImageReader_delete)(AImageReader*);
    media_status_t  (*AImageReader_getWindow)(AImageReader*, ANativeWindow**);
    media_status_t  (*AImageReader_setImageListener)(AImageReader*,
                        AImageReader_ImageListener*);
    media_status_t  (*AImageReader_acquireLatestImage)(AImageReader*, AImage**);
    media_status_t  (*AImageReader_acquireNextImage)(AImageReader*, AImage**);
    media_status_t  (*AImageReader_getMaxImages)(const AImageReader*, int32_t*);

    void            (*AImage_delete)(AImage*);
    media_status_t  (*AImage_getHardwareBuffer)(const AImage*, AHardwareBuffer**);
    media_status_t  (*AImage_getWidth)(const AImage*, int32_t*);
    media_status_t  (*AImage_getHeight)(const AImage*, int32_t*);
    media_status_t  (*AImage_getNumberOfPlanes)(const AImage*, int32_t*);
    media_status_t  (*AImage_getFormat)(const AImage*, int32_t*);
    media_status_t  (*AImage_getPlaneData)(const AImage*, int32_t, uint8_t**, int*);
    media_status_t  (*AImage_getPlaneRowStride)(const AImage*, int32_t, int32_t*);
    media_status_t  (*AImage_getPlanePixelStride)(const AImage*, int32_t, int32_t*);
    media_status_t  (*AImage_getTimestamp)(const AImage*, int64_t*);
};

bool load_camera2ndk(Camera2NDK& out);
bool load_mediandk(MediaNDK& out);
bool start_binder_thread_pool();

// Globally cached instances, populated by the load_* functions.
// Used by Camera2Preview renderer (render thread) to acquire preview images.
extern MediaNDK   g_mediandk;
extern Camera2NDK g_camera2ndk;
