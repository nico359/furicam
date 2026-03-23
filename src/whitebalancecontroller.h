// SPDX-License-Identifier: GPL-2.0-only
#ifndef WHITEBALANCECONTROLLER_H
#define WHITEBALANCECONTROLLER_H

#include <QObject>

class WhiteBalanceController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)

public:
    enum WhiteBalanceMode {
        Auto = 0,
        Daylight = 1,
        Cloudy = 2,
        Fluorescent = 3,
        Incandescent = 4
    };
    Q_ENUM(WhiteBalanceMode)

    explicit WhiteBalanceController(QObject *parent = nullptr);

    int mode() const;
    Q_INVOKABLE void setMode(int mode);

Q_SIGNALS:
    void modeChanged();

private:
    bool resolveSymbols();

    int m_mode = 0;
    bool m_resolved = false;

    // Function pointers resolved via dlsym
    void *m_aalLib = nullptr;
    void *m_hybrisLib = nullptr;

    // AalCameraService::m_service static pointer
    void **m_servicePtr = nullptr;

    // AalCameraService::androidControl() - takes AalCameraService*, returns CameraControl*
    typedef void* (*AndroidControlFn)(void*);
    AndroidControlFn m_androidControl = nullptr;

    // android_camera_set_white_balance_mode(CameraControl*, int)
    typedef void (*SetWBFn)(void*, int);
    SetWBFn m_setWhiteBalance = nullptr;

    // android_camera_get_white_balance_mode(CameraControl*, int*)
    typedef void (*GetWBFn)(void*, int*);
    GetWBFn m_getWhiteBalance = nullptr;
};

#endif // WHITEBALANCECONTROLLER_H
