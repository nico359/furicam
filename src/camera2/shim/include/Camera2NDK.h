/* SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
 *
 * Camera2NDK.h — convenience umbrella that pulls in every public header of the
 * Camera2 / Media NDK subset that libcamera2ndk-hybris forwards.
 *
 * Prefer the individual standard headers (e.g. <camera/NdkCameraManager.h>) in
 * new code, exactly as you would against the real Android NDK — this umbrella
 * just saves listing them all.  Include <camera2ndk_hybris.h> separately for the
 * libhybris-specific init helper.
 */

#ifndef CAMERA2NDK_HYBRIS_UMBRELLA_H
#define CAMERA2NDK_HYBRIS_UMBRELLA_H

#include "camera/NdkCameraError.h"
#include "camera/NdkCameraMetadata.h"
#include "camera/NdkCameraMetadataTags.h"
#include "camera/NdkCameraManager.h"
#include "camera/NdkCameraDevice.h"
#include "camera/NdkCameraCaptureSession.h"
#include "camera/NdkCaptureRequest.h"

#include "media/NdkMediaError.h"
#include "media/NdkMediaFormat.h"
#include "media/NdkMediaCodec.h"
#include "media/NdkMediaMuxer.h"
#include "media/NdkImage.h"
#include "media/NdkImageReader.h"

#include "android/native_window.h"
#include "android/hardware_buffer.h"

#endif /* CAMERA2NDK_HYBRIS_UMBRELLA_H */
