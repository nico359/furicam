// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// Camera2NDK.h — minimal declarations for the Android Camera2 / MediaNDK C
// application programming interface (API).
//
// Why this file exists: the Camera2/MediaNDK *libraries* are present on the
// FuriPhone FLX1s (Halium) at /android/system/lib64/{libcamera2ndk,
// libmediandk,libnativewindow}.so, but libhybris-dev ships only the legacy
// Camera1 compatibility headers (<hybris/camera/...>, <hybris/media/...>) —
// NOT the modern <camera/Ndk*.h> / <media/Ndk*.h> native development kit (NDK)
// headers.  So we declare here exactly the types, enums and function
// prototypes that Camera2NDKShim.cpp forwards and that CameraSession.cpp /
// VideoEncoder.cpp call.
//
// These declarations match the public, stable Android NDK API (API level 24+).
// The signatures are dictated by the application binary interface (ABI) of the
// shared objects above — there is only one correct spelling for each — so this
// header is a description of that public interface, not an original work of
// expression (Apache-2.0 AOSP headers are the canonical source).
//
// Only the subset FuriCam uses is declared.  Metadata tag value constants
// (ACAMERA_* in NdkCameraMetadataTags.h) are intentionally deferred to M2,
// where CameraSession needs them and they can be transcribed exactly from the
// Apache-2.0 AOSP header — getting those values wrong is a latent runtime bug,
// not a compile error, so they do not belong in the shim milestone.

#ifndef FURICAM_CAMERA2_NDK_H
#define FURICAM_CAMERA2_NDK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>   // ssize_t

#ifdef __cplusplus
extern "C" {
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Opaque handle types.  We only ever hold pointers to these, so incomplete
// declarations are sufficient and keep us decoupled from the real layouts.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct ACameraManager                 ACameraManager;
typedef struct ACameraDevice                  ACameraDevice;
typedef struct ACameraCaptureSession          ACameraCaptureSession;
typedef struct ACaptureRequest                ACaptureRequest;
typedef struct ACaptureSessionOutput          ACaptureSessionOutput;
typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;
typedef struct ACameraOutputTarget            ACameraOutputTarget;
typedef struct ACameraMetadata                ACameraMetadata;

typedef struct AImageReader   AImageReader;
typedef struct AImage         AImage;
typedef struct AMediaCodec    AMediaCodec;
typedef struct AMediaFormat   AMediaFormat;
typedef struct AMediaMuxer    AMediaMuxer;
typedef struct AMediaCrypto   AMediaCrypto;

typedef struct ANativeWindow  ANativeWindow;
typedef struct AHardwareBuffer AHardwareBuffer;

// ─────────────────────────────────────────────────────────────────────────────
// Status / result enums (returned by value, so must be complete types).
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    ACAMERA_OK                          = 0,
    ACAMERA_ERROR_BASE                  = -10000,
    ACAMERA_ERROR_UNKNOWN               = ACAMERA_ERROR_BASE,
    ACAMERA_ERROR_INVALID_PARAMETER     = ACAMERA_ERROR_BASE - 1,
    ACAMERA_ERROR_CAMERA_DISCONNECTED   = ACAMERA_ERROR_BASE - 2,
    ACAMERA_ERROR_NOT_ENOUGH_MEMORY     = ACAMERA_ERROR_BASE - 3,
    ACAMERA_ERROR_METADATA_NOT_FOUND    = ACAMERA_ERROR_BASE - 4,
    ACAMERA_ERROR_CAMERA_DEVICE         = ACAMERA_ERROR_BASE - 5,
    ACAMERA_ERROR_CAMERA_SERVICE        = ACAMERA_ERROR_BASE - 6,
    ACAMERA_ERROR_SESSION_CLOSED        = ACAMERA_ERROR_BASE - 7,
    ACAMERA_ERROR_INVALID_OPERATION     = ACAMERA_ERROR_BASE - 8,
    ACAMERA_ERROR_STREAM_CONFIGURE_FAIL = ACAMERA_ERROR_BASE - 9,
    ACAMERA_ERROR_CAMERA_IN_USE         = ACAMERA_ERROR_BASE - 10,
    ACAMERA_ERROR_MAX_CAMERA_IN_USE     = ACAMERA_ERROR_BASE - 11,
    ACAMERA_ERROR_CAMERA_DISABLED       = ACAMERA_ERROR_BASE - 12,
    ACAMERA_ERROR_PERMISSION_DENIED     = ACAMERA_ERROR_BASE - 13,
    ACAMERA_ERROR_UNSUPPORTED_OPERATION = ACAMERA_ERROR_BASE - 14,
} camera_status_t;

typedef enum {
    AMEDIA_OK                              = 0,
    AMEDIA_ERROR_BASE                      = -10000,
    AMEDIA_ERROR_UNKNOWN                   = AMEDIA_ERROR_BASE,
    AMEDIA_ERROR_MALFORMED                 = AMEDIA_ERROR_BASE - 1,
    AMEDIA_ERROR_UNSUPPORTED               = AMEDIA_ERROR_BASE - 2,
    AMEDIA_ERROR_INVALID_OBJECT            = AMEDIA_ERROR_BASE - 3,
    AMEDIA_ERROR_INVALID_PARAMETER         = AMEDIA_ERROR_BASE - 4,
    AMEDIA_ERROR_INVALID_OPERATION         = AMEDIA_ERROR_BASE - 5,
    AMEDIA_ERROR_END_OF_STREAM             = AMEDIA_ERROR_BASE - 6,
    AMEDIA_ERROR_IO                        = AMEDIA_ERROR_BASE - 7,
    AMEDIA_ERROR_WOULD_BLOCK               = AMEDIA_ERROR_BASE - 8,
} media_status_t;

// Capture-request templates (NdkCameraDevice.h).
typedef enum {
    TEMPLATE_PREVIEW          = 1,
    TEMPLATE_STILL_CAPTURE    = 2,
    TEMPLATE_RECORD           = 3,
    TEMPLATE_VIDEO_SNAPSHOT   = 4,
    TEMPLATE_ZERO_SHUTTER_LAG = 5,
    TEMPLATE_MANUAL           = 6,
} ACameraDevice_request_template;

// AMediaMuxer output container format (NdkMediaMuxer.h).
typedef enum {
    AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4    = 0,
    AMEDIAMUXER_OUTPUT_FORMAT_WEBM      = 1,
    AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP = 2,
} OutputFormat;

// AMediaCodec buffer / configure flags and dequeue sentinels (NdkMediaCodec.h).
// AMediaCodec_dequeueOutputBuffer returns these negative values as an ssize_t.
enum {
    AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG    = 2,
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM   = 4,
    AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME   = 8,
    AMEDIACODEC_CONFIGURE_FLAG_ENCODE       = 1,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED  = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER        = -1,
};

// Image formats (NdkImage.h).  Values mirror the HAL pixel formats.
enum {
    AIMAGE_FORMAT_RGBA_8888         = 0x1,
    AIMAGE_FORMAT_RGBX_8888         = 0x2,
    AIMAGE_FORMAT_RGB_888           = 0x3,
    AIMAGE_FORMAT_RGB_565           = 0x4,
    AIMAGE_FORMAT_RGBA_FP16         = 0x16,
    AIMAGE_FORMAT_YUV_420_888       = 0x23,
    AIMAGE_FORMAT_JPEG              = 0x100,
    AIMAGE_FORMAT_RAW16             = 0x20,
    AIMAGE_FORMAT_RAW_PRIVATE       = 0x24,
    AIMAGE_FORMAT_RAW10            = 0x25,
    AIMAGE_FORMAT_RAW12            = 0x26,
    AIMAGE_FORMAT_PRIVATE           = 0x22,
};

// AHardwareBuffer usage flags (hardware_buffer.h) — the subset we set when
// requesting GPU-sampleable preview buffers / encoder input.
enum {
    AHARDWAREBUFFER_USAGE_CPU_READ_NEVER     = 0UL,
    AHARDWAREBUFFER_USAGE_CPU_READ_RARELY    = 2UL,
    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN     = 3UL,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER    = 0UL << 4,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY   = 2UL << 4,
    AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN    = 3UL << 4,
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE  = 1UL << 8,
    AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT   = 1UL << 9,
    AHARDWAREBUFFER_USAGE_VIDEO_ENCODE       = 1UL << 16,
};

// ─────────────────────────────────────────────────────────────────────────────
// Camera metadata tags + value constants (NdkCameraMetadata*.h).  Values match
// AOSP exactly: every tag is (section << 16) + index, so the section-start enum
// below makes each tag self-evidently correct.  Only the subset FuriCam
// reads/writes is declared; extend as later milestones need more.
// ─────────────────────────────────────────────────────────────────────────────

// Entry value type (the `type` field of ACameraMetadata_const_entry).
enum {
    ACAMERA_TYPE_BYTE     = 0,
    ACAMERA_TYPE_INT32    = 1,
    ACAMERA_TYPE_FLOAT    = 2,
    ACAMERA_TYPE_INT64    = 3,
    ACAMERA_TYPE_DOUBLE   = 4,
    ACAMERA_TYPE_RATIONAL = 5,
    ACAMERA_NUM_TYPES     = 6,
};

typedef enum acamera_metadata_section_start {
    ACAMERA_COLOR_CORRECTION_START = 0u << 16,
    ACAMERA_CONTROL_START     = 1u  << 16,
    ACAMERA_FLASH_START       = 4u  << 16,
    ACAMERA_JPEG_START        = 7u  << 16,
    ACAMERA_LENS_START        = 8u  << 16,
    ACAMERA_LENS_INFO_START   = 9u  << 16,
    ACAMERA_REQUEST_START     = 12u << 16,
    ACAMERA_SCALER_START      = 13u << 16,
    ACAMERA_SENSOR_START      = 14u << 16,
    ACAMERA_SENSOR_INFO_START = 15u << 16,
    ACAMERA_INFO_START        = 21u << 16,
} acamera_metadata_section_start;

typedef enum acamera_metadata_tag {
    // CONTROL — request-side controls (M5/M7) + the FPS-range list we read.
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE            = ACAMERA_CONTROL_START + 0,
    ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION       = ACAMERA_CONTROL_START + 1,
    ACAMERA_CONTROL_AE_LOCK                        = ACAMERA_CONTROL_START + 2,
    ACAMERA_CONTROL_AE_MODE                        = ACAMERA_CONTROL_START + 3,
    ACAMERA_CONTROL_AE_REGIONS                     = ACAMERA_CONTROL_START + 4,
    ACAMERA_CONTROL_AE_TARGET_FPS_RANGE            = ACAMERA_CONTROL_START + 5,
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER          = ACAMERA_CONTROL_START + 6,
    ACAMERA_CONTROL_AF_MODE                        = ACAMERA_CONTROL_START + 7,
    ACAMERA_CONTROL_AF_REGIONS                     = ACAMERA_CONTROL_START + 8,
    ACAMERA_CONTROL_AF_TRIGGER                     = ACAMERA_CONTROL_START + 9,
    ACAMERA_CONTROL_AWB_LOCK                       = ACAMERA_CONTROL_START + 10,
    ACAMERA_CONTROL_AWB_MODE                       = ACAMERA_CONTROL_START + 11,
    ACAMERA_CONTROL_MODE                           = ACAMERA_CONTROL_START + 15,
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE       = ACAMERA_CONTROL_START + 17,
    ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES = ACAMERA_CONTROL_START + 20,
    ACAMERA_CONTROL_AE_COMPENSATION_RANGE          = ACAMERA_CONTROL_START + 21,
    ACAMERA_CONTROL_AE_COMPENSATION_STEP           = ACAMERA_CONTROL_START + 22,
    ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES = ACAMERA_CONTROL_START + 26,
    ACAMERA_CONTROL_AE_STATE                       = ACAMERA_CONTROL_START + 31,  // result: AE_STATE_*
    ACAMERA_CONTROL_ZOOM_RATIO_RANGE               = ACAMERA_CONTROL_START + 46,
    ACAMERA_CONTROL_ZOOM_RATIO                     = ACAMERA_CONTROL_START + 47,

    ACAMERA_FLASH_MODE                             = ACAMERA_FLASH_START + 2,

    ACAMERA_JPEG_ORIENTATION                       = ACAMERA_JPEG_START + 3,
    ACAMERA_JPEG_QUALITY                           = ACAMERA_JPEG_START + 4,

    ACAMERA_LENS_FACING                            = ACAMERA_LENS_START + 5,
    ACAMERA_LENS_FOCUS_DISTANCE                    = ACAMERA_LENS_START + 0,
    ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE       = ACAMERA_LENS_INFO_START + 0,
    ACAMERA_LENS_INFO_HYPERFOCAL_DISTANCE          = ACAMERA_LENS_INFO_START + 1,
    ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS      = ACAMERA_LENS_INFO_START + 2,
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION   = ACAMERA_LENS_INFO_START + 6,   // byte[1]: 0=UNCALIBRATED, 1=APPROXIMATE, 2=CALIBRATED

    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES         = ACAMERA_REQUEST_START + 12,

    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS = ACAMERA_SCALER_START + 10,

    // SENSOR — request-side manual controls (M7) and characteristics we log.
    ACAMERA_SENSOR_EXPOSURE_TIME                   = ACAMERA_SENSOR_START + 0,
    ACAMERA_SENSOR_FRAME_DURATION                  = ACAMERA_SENSOR_START + 1,
    ACAMERA_SENSOR_SENSITIVITY                     = ACAMERA_SENSOR_START + 2,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1           = ACAMERA_SENSOR_START + 3,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT2           = ACAMERA_SENSOR_START + 4,
    ACAMERA_SENSOR_COLOR_TRANSFORM1                = ACAMERA_SENSOR_START + 7,
    ACAMERA_SENSOR_COLOR_TRANSFORM2                = ACAMERA_SENSOR_START + 8,
    ACAMERA_SENSOR_FORWARD_MATRIX1                 = ACAMERA_SENSOR_START + 9,
    ACAMERA_SENSOR_FORWARD_MATRIX2                 = ACAMERA_SENSOR_START + 10,
    ACAMERA_SENSOR_BLACK_LEVEL_PATTERN             = ACAMERA_SENSOR_START + 12,
    ACAMERA_SENSOR_NEUTRAL_COLOR_POINT             = ACAMERA_SENSOR_START + 18,
    ACAMERA_SENSOR_ORIENTATION                     = ACAMERA_SENSOR_START + 14,

    ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE          = ACAMERA_SENSOR_INFO_START + 0,
    ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE          = ACAMERA_SENSOR_INFO_START + 1,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT   = ACAMERA_SENSOR_INFO_START + 2,
    ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE        = ACAMERA_SENSOR_INFO_START + 3,
    ACAMERA_SENSOR_INFO_PHYSICAL_SIZE              = ACAMERA_SENSOR_INFO_START + 5,
    ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE           = ACAMERA_SENSOR_INFO_START + 6,
    ACAMERA_SENSOR_INFO_WHITE_LEVEL                = ACAMERA_SENSOR_INFO_START + 7,

    // COLOR_CORRECTION — per-frame WB gains (for DNG AsShotNeutral)
    ACAMERA_COLOR_CORRECTION_GAINS                 = ACAMERA_COLOR_CORRECTION_START + 2,

    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL          = ACAMERA_INFO_START + 0,
} acamera_metadata_tag;

// Selected enum *values* used when decoding the tags above for logging.
enum {
    ACAMERA_LENS_FACING_FRONT    = 0,
    ACAMERA_LENS_FACING_BACK     = 1,
    ACAMERA_LENS_FACING_EXTERNAL = 2,
};
enum {
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED  = 0,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_FULL     = 1,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY   = 2,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_3        = 3,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL = 4,
};
enum {
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT = 0,
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT  = 1,
};
enum {
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE    = 0,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR          = 1,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING = 2,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW                    = 3,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS   = 5,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE          = 6,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT           = 8,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MOTION_TRACKING        = 10,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA   = 11,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MONOCHROME             = 12,
};

// Request-side control value constants (M7 manual controls).
enum {
    ACAMERA_CONTROL_AE_MODE_OFF             = 0,
    ACAMERA_CONTROL_AE_MODE_ON              = 1,
    ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH   = 2,
    ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH = 3,
};
enum { ACAMERA_CONTROL_AE_LOCK_OFF  = 0, ACAMERA_CONTROL_AE_LOCK_ON  = 1 };

enum {
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF  = 0,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ = 1,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ = 2,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO = 3,
};
enum { ACAMERA_CONTROL_AWB_LOCK_OFF = 0, ACAMERA_CONTROL_AWB_LOCK_ON = 1 };
enum {
    ACAMERA_CONTROL_AWB_MODE_OFF              = 0,
    ACAMERA_CONTROL_AWB_MODE_AUTO             = 1,
    ACAMERA_CONTROL_AWB_MODE_INCANDESCENT     = 2,
    ACAMERA_CONTROL_AWB_MODE_FLUORESCENT      = 3,
    ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT = 4,
    ACAMERA_CONTROL_AWB_MODE_DAYLIGHT         = 5,
    ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT  = 6,
    ACAMERA_CONTROL_AWB_MODE_TWILIGHT         = 7,
    ACAMERA_CONTROL_AWB_MODE_SHADE            = 8,
};
enum {
    ACAMERA_CONTROL_AF_MODE_OFF                = 0,
    ACAMERA_CONTROL_AF_MODE_AUTO               = 1,
    ACAMERA_CONTROL_AF_MODE_MACRO              = 2,
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO   = 3,
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE = 4,
    ACAMERA_CONTROL_AF_MODE_EDOF               = 5,
};
enum {
    ACAMERA_CONTROL_AF_TRIGGER_IDLE   = 0,
    ACAMERA_CONTROL_AF_TRIGGER_START  = 1,
    ACAMERA_CONTROL_AF_TRIGGER_CANCEL = 2,
};
enum { ACAMERA_FLASH_MODE_OFF = 0, ACAMERA_FLASH_MODE_SINGLE = 1, ACAMERA_FLASH_MODE_TORCH = 2 };
enum {
    ACAMERA_CONTROL_MODE_OFF            = 0,
    ACAMERA_CONTROL_MODE_AUTO           = 1,
    ACAMERA_CONTROL_MODE_USE_SCENE_MODE = 2,
    ACAMERA_CONTROL_MODE_OFF_KEEP_STATE = 3,
};
enum {
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF = 0,
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON  = 1,
};

// ─────────────────────────────────────────────────────────────────────────────
// Plain-old-data structs we read or populate by value.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct ACameraIdList {
    int          numCameras;
    const char** cameraIds;
} ACameraIdList;

typedef struct ACameraMetadata_rational {
    int32_t numerator;
    int32_t denominator;
} ACameraMetadata_rational;

typedef struct ACameraMetadata_const_entry {
    uint32_t tag;
    uint8_t  type;
    uint32_t count;
    union {
        const uint8_t*                   u8;
        const int32_t*                   i32;
        const float*                     f;
        const int64_t*                   i64;
        const double*                    d;
        const ACameraMetadata_rational*  r;
    } data;
} ACameraMetadata_const_entry;

typedef struct AMediaCodecBufferInfo {
    int32_t  offset;
    int32_t  size;
    int64_t  presentationTimeUs;
    uint32_t flags;
} AMediaCodecBufferInfo;

typedef struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

// ─────────────────────────────────────────────────────────────────────────────
// Callback structs (passed by pointer to the *register/open* calls).  The shim
// only needs the names, but CameraSession / VideoEncoder populate these, so we
// define them faithfully to the NDK layout.
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*ACameraDevice_StateCallback)(void* context, ACameraDevice* device);
typedef void (*ACameraDevice_ErrorStateCallback)(void* context, ACameraDevice* device, int error);

typedef struct ACameraDevice_StateCallbacks {
    void*                            context;
    ACameraDevice_StateCallback      onDisconnected;
    ACameraDevice_ErrorStateCallback onError;
} ACameraDevice_StateCallbacks;

typedef void (*ACameraCaptureSession_stateCallback)(void* context, ACameraCaptureSession* session);

typedef struct ACameraCaptureSession_stateCallbacks {
    void*                               context;
    ACameraCaptureSession_stateCallback onClosed;
    ACameraCaptureSession_stateCallback onReady;
    ACameraCaptureSession_stateCallback onActive;
} ACameraCaptureSession_stateCallbacks;

typedef struct ACameraCaptureFailure {
    int64_t frameNumber;
    int     reason;
    int     sequenceId;
    bool    wasImageCaptured;
} ACameraCaptureFailure;

typedef void (*ACameraCaptureSession_captureCallback_start)(
        void* context, ACameraCaptureSession* session,
        const ACaptureRequest* request, int64_t timestamp);
typedef void (*ACameraCaptureSession_captureCallback_result)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, const ACameraMetadata* result);
typedef void (*ACameraCaptureSession_captureCallback_failed)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ACameraCaptureFailure* failure);
typedef void (*ACameraCaptureSession_captureCallback_sequenceEnd)(
        void* context, ACameraCaptureSession* session,
        int sequenceId, int64_t frameNumber);
typedef void (*ACameraCaptureSession_captureCallback_sequenceAbort)(
        void* context, ACameraCaptureSession* session, int sequenceId);
typedef void (*ACameraCaptureSession_captureCallback_bufferLost)(
        void* context, ACameraCaptureSession* session,
        ACaptureRequest* request, ANativeWindow* window, int64_t frameNumber);

typedef struct ACameraCaptureSession_captureCallbacks {
    void*                                               context;
    ACameraCaptureSession_captureCallback_start         onCaptureStarted;
    ACameraCaptureSession_captureCallback_result        onCaptureProgressed;
    ACameraCaptureSession_captureCallback_result        onCaptureCompleted;
    ACameraCaptureSession_captureCallback_failed        onCaptureFailed;
    ACameraCaptureSession_captureCallback_sequenceEnd   onCaptureSequenceCompleted;
    ACameraCaptureSession_captureCallback_sequenceAbort onCaptureSequenceAborted;
    ACameraCaptureSession_captureCallback_bufferLost    onCaptureBufferLost;
} ACameraCaptureSession_captureCallbacks;

typedef void (*AImageReader_ImageCallback)(void* context, AImageReader* reader);

typedef struct AImageReader_ImageListener {
    void*                      context;
    AImageReader_ImageCallback onImageAvailable;
} AImageReader_ImageListener;

// ─────────────────────────────────────────────────────────────────────────────
// Function prototypes.  Each one is forwarded to the real shared object by a
// HYBRIS_IMPLEMENT_FUNCTION_* line in Camera2NDKShim.cpp.  Keep this list and
// the shim in lock-step.
// ─────────────────────────────────────────────────────────────────────────────

// libcamera2ndk.so — manager / device / metadata.
ACameraManager* ACameraManager_create(void);
void            ACameraManager_delete(ACameraManager* manager);
camera_status_t ACameraManager_getCameraIdList(ACameraManager* manager, ACameraIdList** cameraIdList);
void            ACameraManager_deleteCameraIdList(ACameraIdList* cameraIdList);
camera_status_t ACameraManager_getCameraCharacteristics(ACameraManager* manager, const char* cameraId, ACameraMetadata** characteristics);
camera_status_t ACameraManager_openCamera(ACameraManager* manager, const char* cameraId, ACameraDevice_StateCallbacks* callback, ACameraDevice** device);

camera_status_t ACameraMetadata_getConstEntry(const ACameraMetadata* metadata, uint32_t tag, ACameraMetadata_const_entry* entry);
camera_status_t ACameraMetadata_getAllTags(const ACameraMetadata* metadata, int32_t* numEntries, const uint32_t** tags);
ACameraMetadata* ACameraMetadata_copy(const ACameraMetadata* src);
void            ACameraMetadata_free(ACameraMetadata* metadata);

camera_status_t ACameraDevice_close(ACameraDevice* device);
const char*     ACameraDevice_getId(const ACameraDevice* device);
camera_status_t ACameraDevice_createCaptureRequest(const ACameraDevice* device, ACameraDevice_request_template templateId, ACaptureRequest** request);
camera_status_t ACameraDevice_createCaptureSession(ACameraDevice* device, const ACaptureSessionOutputContainer* outputs, const ACameraCaptureSession_stateCallbacks* callbacks, ACameraCaptureSession** session);

// libcamera2ndk.so — session output containers / targets.
camera_status_t ACaptureSessionOutputContainer_create(ACaptureSessionOutputContainer** container);
void            ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer* container);
camera_status_t ACaptureSessionOutput_create(ANativeWindow* anw, ACaptureSessionOutput** output);
void            ACaptureSessionOutput_free(ACaptureSessionOutput* output);
camera_status_t ACaptureSessionOutputContainer_add(ACaptureSessionOutputContainer* container, const ACaptureSessionOutput* output);
camera_status_t ACaptureSessionOutputContainer_remove(ACaptureSessionOutputContainer* container, const ACaptureSessionOutput* output);

camera_status_t ACameraOutputTarget_create(ANativeWindow* anw, ACameraOutputTarget** output);
void            ACameraOutputTarget_free(ACameraOutputTarget* output);

// libcamera2ndk.so — capture request building.
camera_status_t ACaptureRequest_addTarget(ACaptureRequest* request, const ACameraOutputTarget* output);
camera_status_t ACaptureRequest_removeTarget(ACaptureRequest* request, const ACameraOutputTarget* output);
camera_status_t ACaptureRequest_getConstEntry(const ACaptureRequest* request, uint32_t tag, ACameraMetadata_const_entry* entry);
camera_status_t ACaptureRequest_setEntry_i32(ACaptureRequest* request, uint32_t tag, uint32_t count, const int32_t* data);
camera_status_t ACaptureRequest_setEntry_i64(ACaptureRequest* request, uint32_t tag, uint32_t count, const int64_t* data);
camera_status_t ACaptureRequest_setEntry_float(ACaptureRequest* request, uint32_t tag, uint32_t count, const float* data);
camera_status_t ACaptureRequest_setEntry_u8(ACaptureRequest* request, uint32_t tag, uint32_t count, const uint8_t* data);
void            ACaptureRequest_free(ACaptureRequest* request);

// libcamera2ndk.so — capture session control.
void            ACameraCaptureSession_close(ACameraCaptureSession* session);
camera_status_t ACameraCaptureSession_setRepeatingRequest(ACameraCaptureSession* session, ACameraCaptureSession_captureCallbacks* callbacks, int numRequests, ACaptureRequest** requests, int* captureSequenceId);
camera_status_t ACameraCaptureSession_capture(ACameraCaptureSession* session, ACameraCaptureSession_captureCallbacks* callbacks, int numRequests, ACaptureRequest** requests, int* captureSequenceId);
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession* session);
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession* session);

// libmediandk.so — AImageReader.
media_status_t AImageReader_new(int32_t width, int32_t height, int32_t format, int32_t maxImages, AImageReader** reader);
media_status_t AImageReader_newWithUsage(int32_t width, int32_t height, int32_t format, uint64_t usage, int32_t maxImages, AImageReader** reader);
void           AImageReader_delete(AImageReader* reader);
media_status_t AImageReader_getWindow(AImageReader* reader, ANativeWindow** window);
media_status_t AImageReader_acquireNextImage(AImageReader* reader, AImage** image);
media_status_t AImageReader_acquireLatestImage(AImageReader* reader, AImage** image);
media_status_t AImageReader_setImageListener(AImageReader* reader, AImageReader_ImageListener* listener);
media_status_t AImageReader_getFormat(AImageReader* reader, int32_t* format);

// libmediandk.so — AImage.
void           AImage_delete(AImage* image);
media_status_t AImage_getWidth(const AImage* image, int32_t* width);
media_status_t AImage_getHeight(const AImage* image, int32_t* height);
media_status_t AImage_getFormat(const AImage* image, int32_t* format);
media_status_t AImage_getTimestamp(const AImage* image, int64_t* timestampNs);
media_status_t AImage_getNumberOfPlanes(const AImage* image, int32_t* numPlanes);
media_status_t AImage_getPlaneData(const AImage* image, int planeIdx, uint8_t** data, int* dataLength);
media_status_t AImage_getPlaneRowStride(const AImage* image, int planeIdx, int32_t* rowStride);
media_status_t AImage_getPlanePixelStride(const AImage* image, int planeIdx, int32_t* pixelStride);
media_status_t AImage_getHardwareBuffer(const AImage* image, AHardwareBuffer** buffer);

// libmediandk.so — AMediaCodec (hardware H.264 encoder).
AMediaCodec*   AMediaCodec_createEncoderByType(const char* mimeType);
AMediaCodec*   AMediaCodec_createCodecByName(const char* name);
media_status_t AMediaCodec_delete(AMediaCodec* codec);
media_status_t AMediaCodec_configure(AMediaCodec* codec, const AMediaFormat* format, ANativeWindow* surface, AMediaCrypto* crypto, uint32_t flags);
media_status_t AMediaCodec_createInputSurface(AMediaCodec* codec, ANativeWindow** surface);
media_status_t AMediaCodec_start(AMediaCodec* codec);
media_status_t AMediaCodec_stop(AMediaCodec* codec);
media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec* codec);
ssize_t        AMediaCodec_dequeueOutputBuffer(AMediaCodec* codec, AMediaCodecBufferInfo* info, int64_t timeoutUs);
uint8_t*       AMediaCodec_getOutputBuffer(AMediaCodec* codec, size_t idx, size_t* outSize);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* codec, size_t idx, bool render);
AMediaFormat*  AMediaCodec_getOutputFormat(AMediaCodec* codec);
media_status_t AMediaCodec_setParameters(AMediaCodec* codec, const AMediaFormat* params);

// libmediandk.so — AMediaFormat.
AMediaFormat*  AMediaFormat_new(void);
media_status_t AMediaFormat_delete(AMediaFormat* format);
void           AMediaFormat_setString(AMediaFormat* format, const char* name, const char* value);
void           AMediaFormat_setInt32(AMediaFormat* format, const char* name, int32_t value);
void           AMediaFormat_setInt64(AMediaFormat* format, const char* name, int64_t value);
void           AMediaFormat_setFloat(AMediaFormat* format, const char* name, float value);
void           AMediaFormat_setBuffer(AMediaFormat* format, const char* name, const void* data, size_t size);
bool           AMediaFormat_getInt32(AMediaFormat* format, const char* name, int32_t* out);
bool           AMediaFormat_getInt64(AMediaFormat* format, const char* name, int64_t* out);
const char*    AMediaFormat_toString(AMediaFormat* format);

// libmediandk.so — AMediaMuxer (MPEG-4 Part 14 container writer).
AMediaMuxer*   AMediaMuxer_new(int fd, OutputFormat format);
media_status_t AMediaMuxer_delete(AMediaMuxer* muxer);
ssize_t        AMediaMuxer_addTrack(AMediaMuxer* muxer, const AMediaFormat* format);
media_status_t AMediaMuxer_start(AMediaMuxer* muxer);
media_status_t AMediaMuxer_stop(AMediaMuxer* muxer);
media_status_t AMediaMuxer_writeSampleData(AMediaMuxer* muxer, size_t trackIdx, const uint8_t* data, const AMediaCodecBufferInfo* info);
media_status_t AMediaMuxer_setOrientationHint(AMediaMuxer* muxer, int degrees);

// libnativewindow.so — ANativeWindow / AHardwareBuffer lifetime + description
// (used by the renderer to import camera buffers into EGL in M3).
void AHardwareBuffer_acquire(AHardwareBuffer* buffer);
void AHardwareBuffer_release(AHardwareBuffer* buffer);
void AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
void ANativeWindow_acquire(ANativeWindow* window);
void ANativeWindow_release(ANativeWindow* window);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // FURICAM_CAMERA2_NDK_H
