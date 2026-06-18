/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * camera/NdkCameraManager.h — the Camera2 entry point: enumerate cameras, read
 * their characteristics, and open a device (ACameraManager).
 *
 * Subset of the Android NDK forwarded by libcamera2ndk-hybris.  Spelling
 * matches the public Android NDK ABI (API level 24+).
 *
 * NOTE for libhybris consumers: a bare libhybris process has no binder thread
 * pool, so an opened camera will never deliver buffers until one is started.
 * Call camera2ndk_hybris_start_threadpool() (see <camera2ndk_hybris.h>) once at
 * startup, before ACameraManager_openCamera().
 */

#ifndef CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMANAGER_H
#define CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMANAGER_H

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraMetadata.h"
#include "camera/NdkCameraDevice.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ACameraManager ACameraManager;

typedef struct ACameraIdList {
    int          numCameras;
    const char** cameraIds;
} ACameraIdList;

ACameraManager* ACameraManager_create(void);
void            ACameraManager_delete(ACameraManager* manager);
camera_status_t ACameraManager_getCameraIdList(ACameraManager* manager,
                                               ACameraIdList** cameraIdList);
void            ACameraManager_deleteCameraIdList(ACameraIdList* cameraIdList);
camera_status_t ACameraManager_getCameraCharacteristics(ACameraManager* manager,
                                                        const char* cameraId,
                                                        ACameraMetadata** characteristics);
camera_status_t ACameraManager_openCamera(ACameraManager* manager, const char* cameraId,
                                          ACameraDevice_StateCallbacks* callback,
                                          ACameraDevice** device);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CAMERA2NDK_HYBRIS_CAMERA_NDKCAMERAMANAGER_H */
