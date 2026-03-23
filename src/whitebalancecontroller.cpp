// SPDX-License-Identifier: GPL-2.0-only
#include "whitebalancecontroller.h"
#include <dlfcn.h>
#include <QDebug>

WhiteBalanceController::WhiteBalanceController(QObject *parent)
    : QObject(parent)
{
}

bool WhiteBalanceController::resolveSymbols()
{
    if (m_resolved)
        return m_servicePtr && m_androidControl && m_setWhiteBalance;

    m_resolved = true;

    // The AAL plugin is already loaded in our process (via the shader videonode)
    m_aalLib = dlopen("libaalcamera.so", RTLD_LAZY | RTLD_NOLOAD);
    if (!m_aalLib) {
        // Try the full path
        m_aalLib = dlopen("/usr/lib/aarch64-linux-gnu/qt5/plugins/mediaservice/libaalcamera.so",
                          RTLD_LAZY | RTLD_NOLOAD);
    }
    if (!m_aalLib) {
        qWarning() << "WhiteBalanceController: AAL camera plugin not loaded in process";
        return false;
    }

    // Get the static AalCameraService singleton pointer
    // Symbol: AalCameraService::m_service (static AalCameraService*)
    m_servicePtr = (void**)dlsym(m_aalLib, "_ZN16AalCameraService9m_serviceE");
    if (!m_servicePtr) {
        qWarning() << "WhiteBalanceController: Cannot find AalCameraService::m_service";
        return false;
    }

    // Get AalCameraService::androidControl() method
    m_androidControl = (AndroidControlFn)dlsym(m_aalLib, "_ZN16AalCameraService14androidControlEv");
    if (!m_androidControl) {
        qWarning() << "WhiteBalanceController: Cannot find AalCameraService::androidControl()";
        return false;
    }

    // Load libcamera-hybris for the white balance functions
    m_hybrisLib = dlopen("libcamera-hybris.so.1", RTLD_LAZY);
    if (!m_hybrisLib) {
        qWarning() << "WhiteBalanceController: Cannot load libcamera-hybris:" << dlerror();
        return false;
    }

    m_setWhiteBalance = (SetWBFn)dlsym(m_hybrisLib, "android_camera_set_white_balance_mode");
    m_getWhiteBalance = (GetWBFn)dlsym(m_hybrisLib, "android_camera_get_white_balance_mode");

    if (!m_setWhiteBalance) {
        qWarning() << "WhiteBalanceController: Cannot find android_camera_set_white_balance_mode";
        return false;
    }

    qDebug() << "WhiteBalanceController: Symbols resolved successfully";
    return true;
}

int WhiteBalanceController::mode() const
{
    return m_mode;
}

void WhiteBalanceController::setMode(int mode)
{
    if (!resolveSymbols())
        return;

    if (!m_servicePtr || !*m_servicePtr) {
        qWarning() << "WhiteBalanceController: No active camera service";
        return;
    }

    // Get the CameraControl* from the AAL service
    void *cameraControl = m_androidControl(*m_servicePtr);
    if (!cameraControl) {
        qWarning() << "WhiteBalanceController: No Android camera control";
        return;
    }

    // Clamp mode to valid range
    if (mode < 0 || mode > 4) {
        qWarning() << "WhiteBalanceController: Invalid mode" << mode;
        return;
    }

    m_setWhiteBalance(cameraControl, mode);
    m_mode = mode;
    emit modeChanged();
    qDebug() << "WhiteBalanceController: Set white balance to" << mode;
}
