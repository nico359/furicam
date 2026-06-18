#pragma once
#include <QString>
#include <QStringList>

// Probes Camera2 NDK (via hybris) and returns info about each camera.
// Returns empty list if libcamera2ndk.so can't be loaded.
struct Camera2Info {
    QString id;
    bool    supportsRaw;
    bool    isBack;       // ACAMERA_LENS_FACING_BACK
    int     hardwareLevel; // 0=legacy 1=limited 2=full 3=level3
};

QList<Camera2Info> camera2_probe();
