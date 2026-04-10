// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Joaquin Philco <joaquinphilco@gmail.com>

#ifndef METERINGCONTROLLER_H
#define METERINGCONTROLLER_H

#include <QObject>

// Directly calls android_camera_set_metering_region / android_camera_reset_metering_region
// via dlsym, bypassing the Qt Multimedia layer which does not expose independent metering
// control.  The same dlsym pattern is used by WhiteBalanceController.
class MeteringController : public QObject
{
    Q_OBJECT

public:
    explicit MeteringController(QObject *parent = nullptr);

    // Point is in normalised [0,1] coordinates (same as camera.focus.customFocusPoint).
    // The metering region is a fixed-size window centred on that point.
    Q_INVOKABLE void setMeteringPoint(qreal x, qreal y);

    // Reset to full-frame / centre metering.
    Q_INVOKABLE void resetMetering();

private:
    bool resolveSymbols();

    bool m_resolved = false;
    void *m_aalLib    = nullptr;
    void *m_hybrisLib = nullptr;

    void **m_servicePtr = nullptr;

    typedef void* (*AndroidControlFn)(void*);
    AndroidControlFn m_androidControl = nullptr;

    // MeteringRegion layout must match hybris/camera/camera_compatibility_layer.h
    struct MeteringRegion { int left; int right; int top; int bottom; int weight; };

    typedef void (*SetMeteringFn)(void*, MeteringRegion*);
    SetMeteringFn m_setMetering   = nullptr;

    typedef void (*ResetMeteringFn)(void*);
    ResetMeteringFn m_resetMetering = nullptr;

    // Triggering autofocus is required to make the firmware re-evaluate AE
    // with the new metering region.  Without it the region change is ignored.
    typedef void (*StartAutofocusFn)(void*);
    StartAutofocusFn m_startAutofocus = nullptr;
};

#endif // METERINGCONTROLLER_H
