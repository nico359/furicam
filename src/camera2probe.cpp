#include "camera2probe.h"
#include "ndk_loader.h"

#include <camera/NdkCameraMetadataTags.h>
#include <QDebug>

QList<Camera2Info> camera2_probe()
{
    Camera2NDK cam;
    if (!load_camera2ndk(cam)) {
        qWarning() << "camera2probe: failed to load libcamera2ndk.so";
        return {};
    }

    ACameraManager* mgr = cam.ACameraManager_create();
    if (!mgr) {
        qWarning() << "camera2probe: ACameraManager_create returned null";
        return {};
    }

    ACameraIdList* ids = nullptr;
    if (cam.ACameraManager_getCameraIdList(mgr, &ids) != ACAMERA_OK || !ids) {
        qWarning() << "camera2probe: getCameraIdList failed";
        cam.ACameraManager_delete(mgr);
        return {};
    }

    QList<Camera2Info> result;

    for (int i = 0; i < ids->numCameras; i++) {
        const char* id = ids->cameraIds[i];
        ACameraMetadata* chars = nullptr;
        if (cam.ACameraManager_getCameraCharacteristics(mgr, id, &chars) != ACAMERA_OK)
            continue;

        Camera2Info info;
        info.id = QString::fromUtf8(id);
        info.supportsRaw = false;
        info.isBack = false;
        info.hardwareLevel = -1;

        ACameraMetadata_const_entry entry;

        if (cam.ACameraMetadata_getConstEntry(chars, ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL, &entry) == ACAMERA_OK)
            info.hardwareLevel = entry.data.u8[0];

        if (cam.ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK)
            info.isBack = (entry.data.u8[0] == ACAMERA_LENS_FACING_BACK);

        if (cam.ACameraMetadata_getConstEntry(chars, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &entry) == ACAMERA_OK) {
            for (uint32_t j = 0; j < entry.count; j++) {
                if (entry.data.u8[j] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW)
                    info.supportsRaw = true;
            }
        }

        qInfo() << "camera2probe: id=" << info.id
                << "back=" << info.isBack
                << "hwLevel=" << info.hardwareLevel
                << "raw=" << info.supportsRaw;

        // Dump RAW stream configs — this is the definitive check.
        // ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS is a flat int32 array:
        // [format, width, height, isInput] repeated. RAW formats: RAW16=32, RAW10=37, RAW12=38.
        if (cam.ACameraMetadata_getConstEntry(chars,
                ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry) == ACAMERA_OK) {
            for (uint32_t j = 0; j + 3 < entry.count; j += 4) {
                int32_t fmt     = entry.data.i32[j];
                int32_t w       = entry.data.i32[j+1];
                int32_t h       = entry.data.i32[j+2];
                int32_t isInput = entry.data.i32[j+3];
                if (isInput) continue;
                if (fmt == 0x20 || fmt == 0x25 || fmt == 0x26) { // RAW16, RAW10, RAW12
                    const char* fmtName = fmt == 0x20 ? "RAW16" : fmt == 0x25 ? "RAW10" : "RAW12";
                    qInfo() << "  camera2probe:  RAW output:" << fmtName << w << "x" << h;
                }
            }
        } else {
            qInfo() << "  camera2probe:  no stream config metadata";
        }

        result.append(info);
        cam.ACameraMetadata_free(chars);
    }

    cam.ACameraManager_deleteCameraIdList(ids);
    cam.ACameraManager_delete(mgr);
    return result;
}
