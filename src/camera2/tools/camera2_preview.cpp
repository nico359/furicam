// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// camera2_preview — Milestone 3b on-screen test harness.
//
// A minimal Qt Quick app that registers the Camera2Bridge QML type and shows it
// fullscreen, driving a live Camera2 preview through the libhybris NDK path.
// Deliberately tiny: it needs only Qt Quick/Qml/Gui (the GLES Qt already on the
// phone) + EGL/GLES + libhybris — NOT Qt5Multimedia/OpenCV/ZXing — so it builds
// on-device without the full app's dependency stack.

#include "Camera2Bridge.h"

#include <QDir>
#include <QGuiApplication>
#include <QQuickView>
#include <QTemporaryFile>
#include <QUrl>
#include <qqml.h>

#include <cstdio>

static const char* kQml = R"QML(
import QtQuick 2.0
import FuriCam 1.0

Rectangle {
    id: root
    color: "black"
    property int  wb: 0
    property real zoom: 1.0
    property bool torch: false
    property bool aelock: false
    readonly property var wbNames: ["AWB", "Daylight", "Cloudy", "Fluor", "Incand"]

    Camera2Bridge {
        id: cam
        anchors.fill: parent
        Component.onCompleted: cam.startCamera()
        onCameraError: console.log("camera error: " + message)
    }

    // Tap anywhere (outside the buttons) to focus there.
    MouseArea {
        anchors.fill: parent
        z: -1
        onClicked: cam.setFocusPoint(mouse.x / width, mouse.y / height)
    }

    Row {
        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: 26 }
        spacing: 12

        Rectangle {
            width: 150; height: 62; radius: 10; color: "#2a2a2a"; border.color: "#666"; border.width: 1
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 22; text: "WB: " + root.wbNames[root.wb] }
            MouseArea { anchors.fill: parent; onClicked: { root.wb = (root.wb + 1) % 5; cam.setWhiteBalanceMode(root.wb) } }
        }
        Rectangle {
            width: 110; height: 62; radius: 10; color: "#2a2a2a"; border.color: "#666"; border.width: 1
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 22; text: "Zoom " + root.zoom.toFixed(0) + "x" }
            MouseArea { anchors.fill: parent; onClicked: { root.zoom = root.zoom >= 4 ? 1 : root.zoom + 1; cam.setZoom(root.zoom) } }
        }
        Rectangle {
            width: 110; height: 62; radius: 10; color: root.torch ? "#7a6000" : "#2a2a2a"; border.color: "#666"; border.width: 1
            Text { anchors.centerIn: parent; color: root.torch ? "yellow" : "white"; font.pixelSize: 22; text: "Torch" }
            MouseArea { anchors.fill: parent; onClicked: { root.torch = !root.torch; cam.setTorch(root.torch) } }
        }
        Rectangle {
            width: 130; height: 62; radius: 10; color: root.aelock ? "#005000" : "#2a2a2a"; border.color: "#666"; border.width: 1
            Text { anchors.centerIn: parent; color: root.aelock ? "lightgreen" : "white"; font.pixelSize: 22; text: root.aelock ? "AE Locked" : "AE Lock" }
            MouseArea { anchors.fill: parent; onClicked: { root.aelock = !root.aelock; cam.setAELock(root.aelock) } }
        }
    }

    Text {
        anchors { left: parent.left; bottom: parent.bottom; margins: 14 }
        color: cam.ready ? "lime" : "orange"
        font.pixelSize: 30
        style: Text.Outline; styleColor: "black"
        text: (cam.ready ? "LIVE" : "starting") + "   frames " + cam.frameCount + "   (tap to focus)"
    }
}
)QML";

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    qmlRegisterType<furicam::Camera2Bridge>("FuriCam", 1, 0, "Camera2Bridge");

    QTemporaryFile qml(QDir::tempPath() + "/camera2_preview_XXXXXX.qml");
    qml.setAutoRemove(true);
    if (!qml.open()) {
        std::fprintf(stderr, "camera2_preview: cannot create temp QML\n");
        return 1;
    }
    qml.write(kQml);
    qml.flush();

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setSource(QUrl::fromLocalFile(qml.fileName()));
    if (view.status() == QQuickView::Error) {
        std::fprintf(stderr, "camera2_preview: QML load error\n");
        return 1;
    }
    view.showFullScreen();
    return app.exec();
}
