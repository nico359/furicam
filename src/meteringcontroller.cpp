// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Joaquin Philco <joaquinphilco@gmail.com>

#include "meteringcontroller.h"
#include <dlfcn.h>
#include <QDebug>

// Android camera coordinate space: -1000 to +1000 with (0,0) at centre.
static constexpr int kAndroidHalf   = 1000;
static constexpr int kRegionSize    = 150;  // half-width/height of the metering window

MeteringController::MeteringController(QObject *parent)
    : QObject(parent)
{
}

bool MeteringController::resolveSymbols()
{
    if (m_resolved)
        return m_servicePtr && m_androidControl && m_setMetering;

    m_resolved = true;

    m_aalLib = dlopen("libaalcamera.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!m_aalLib) {
        m_aalLib = dlopen("/usr/lib/aarch64-linux-gnu/qt5/plugins/mediaservice/libaalcamera.so",
                          RTLD_LAZY | RTLD_NOLOAD);
    }
    if (!m_aalLib) {
        qWarning() << "MeteringController: AAL camera plugin not loaded in process";
        return false;
    }

    m_servicePtr = (void**)dlsym(m_aalLib, "_ZN16AalCameraService9m_serviceE");
    if (!m_servicePtr) {
        qWarning() << "MeteringController: Cannot find AalCameraService::m_service";
        return false;
    }

    m_androidControl = (AndroidControlFn)dlsym(m_aalLib, "_ZN16AalCameraService14androidControlEv");
    if (!m_androidControl) {
        qWarning() << "MeteringController: Cannot find AalCameraService::androidControl()";
        return false;
    }

    m_hybrisLib = dlopen("libcamera-hybris.so.1", RTLD_LAZY);
    if (!m_hybrisLib) {
        qWarning() << "MeteringController: Cannot load libcamera-hybris:" << dlerror();
        return false;
    }

    m_setMetering   = (SetMeteringFn)dlsym(m_hybrisLib,   "android_camera_set_metering_region");
    m_resetMetering = (ResetMeteringFn)dlsym(m_hybrisLib, "android_camera_reset_metering_region");
    m_startAutofocus = (StartAutofocusFn)dlsym(m_hybrisLib, "android_camera_start_autofocus");

    if (!m_setMetering) {
        qWarning() << "MeteringController: Cannot find android_camera_set_metering_region";
        return false;
    }
    if (!m_startAutofocus) {
        qWarning() << "MeteringController: Cannot find android_camera_start_autofocus (AE may not re-evaluate)";
    }

    qDebug() << "MeteringController: Symbols resolved successfully";
    return true;
}

void MeteringController::setMeteringPoint(qreal x, qreal y)
{
    if (!resolveSymbols())
        return;

    if (!m_servicePtr || !*m_servicePtr) {
        qWarning() << "MeteringController: No active camera service";
        return;
    }

    void *cameraControl = m_androidControl(*m_servicePtr);
    if (!cameraControl) {
        qWarning() << "MeteringController: No Android camera control";
        return;
    }

    // Convert normalised [0,1] → Android [-1000, +1000]
    int cx = static_cast<int>(x * 2 * kAndroidHalf) - kAndroidHalf;
    int cy = static_cast<int>(y * 2 * kAndroidHalf) - kAndroidHalf;

    // Clamp so the region fits within the sensor bounds
    const int maxCenter = kAndroidHalf - kRegionSize;
    cx = qBound(-maxCenter, cx, maxCenter);
    cy = qBound(-maxCenter, cy, maxCenter);

    MeteringRegion region;
    region.left   = cx - kRegionSize;
    region.right  = cx + kRegionSize;
    region.top    = cy - kRegionSize;
    region.bottom = cy + kRegionSize;
    region.weight = 5;

    m_setMetering(cameraControl, &region);

    // The Android camera firmware only re-evaluates AE when autofocus is
    // triggered — setting the metering region alone is not enough.
    if (m_startAutofocus) {
        m_startAutofocus(cameraControl);
        qDebug() << "MeteringController: metering" << x << y
                 << "region [" << region.left << region.top << region.right << region.bottom << "] + AF triggered";
    }
}

void MeteringController::resetMetering()
{
    if (!resolveSymbols())
        return;

    if (!m_servicePtr || !*m_servicePtr)
        return;

    void *cameraControl = m_androidControl(*m_servicePtr);
    if (!cameraControl)
        return;

    if (m_resetMetering) {
        m_resetMetering(cameraControl);
    } else {
        // Fallback: set a large centre region
        MeteringRegion region;
        region.left   = -kAndroidHalf;
        region.right  =  kAndroidHalf;
        region.top    = -kAndroidHalf;
        region.bottom =  kAndroidHalf;
        region.weight = 1;
        m_setMetering(cameraControl, &region);
    }
}
