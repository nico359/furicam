// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2024 Furi Labs
// Copyright (C) 2026 Sean Pollard <spollard08@gmail.com>
//
// Camera.qml — the camera item.  Rewritten to drive the Camera2 engine
// (Camera2Bridge) instead of the QtMultimedia Camera + gst-droid pipeline,
// while keeping the EXACT contract main.qml depends on (the handle*/set*
// functions, the resolutionModel/currentRes*/maxZoom/currentZoom properties,
// and the photoSaved() signal).  main.qml is unchanged.
//
// QtMultimedia is still imported, but ONLY for its enum *values* (Camera.FlashOff,
// Camera.FrontFace, Camera.FocusContinuous, …) that main.qml passes in — no
// QtMultimedia Camera/VideoOutput is instantiated here anymore.

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.12
import QtGraphicalEffects 1.0
import QtMultimedia 5.15
import QtQuick.Layouts 1.15
import Qt.labs.settings 1.0
import Qt.labs.platform 1.1
import FuriCam 1.0

Item {
    id: cameraItem
    width: 400
    height: 800

    property int lockedVideoRotation: 0

    property alias resolutionModel: resModel
    property int currentResWidth: 0
    property int currentResHeight: 0

    // Zoom is exposed in the legacy "slider value" model main.qml expects:
    //   maxZoom is the slider range, currentZoom is the slider value, and the
    //   magnification label main.qml shows is 1 + (currentZoom/maxZoom)*3.
    property real currentZoom: 0
    property real maxZoom: cam2.ready ? cam2.maxZoom() : 0

    property int  colorTemperature: 0
    property bool frontActive: false

    // Emitted whenever a final photo has been saved and is ready for the gallery.
    signal photoSaved()

    // Kept instantiated for compatibility; HDR/metering are simplified for the
    // Camera2 path (single capture).  TODO: HDR via Camera2 exposure bracketing.
    HdrProcessor      { id: hdrProcessor }
    MeteringController { id: meteringController }

    ListModel { id: resModel }

    function setColorTemperature(temp) { colorTemperature = temp }

    function setWhiteBalanceMode(mode) { cam2.setWhiteBalanceMode(mode) }

    function gcd(a, b) { return b == 0 ? a : gcd(b, a % b) }

    // Populate the photo-resolution picker from the engine's JPEG output sizes.
    function fnAspectRatio() {
        resModel.clear()
        var res = cam2.availableResolutions()
        for (var i = 0; i < res.length; i++) {
            var w = res[i].width
            var h = res[i].height
            var g = gcd(w, h)
            var mp = Math.round((w * h) / 100000) / 10
            resModel.append({
                "resWidth": w, "resHeight": h,
                "aspectRatio": (w / g) + ":" + (h / g),
                "mp": mp, "label": mp + " MP (" + w + "×" + h + ")"
            })
        }
        // Default to the largest (first) size until the user picks one.
        if (res.length > 0 && currentResWidth === 0) {
            currentResWidth = res[0].width
            currentResHeight = res[0].height
        }
    }

    function setResolution(width, height) {
        currentResWidth = width
        currentResHeight = height
        if (settings.cameras && settings.cameras[settings.cameraId]) {
            settings.cameras[settings.cameraId].resWidth = width
            settings.cameras[settings.cameraId].resHeight = height
        }
        cam2.setResolution(width, height)   // restarts the camera at the new still size
    }

    function handleSetFlashState(flashState) {
        // Map the legacy flash cycle to the torch (continuous light).  A proper
        // per-shot flash would set the capture-request flash mode — TODO.
        cam2.setTorch(flashState !== Camera.FlashOff)
    }

    function handleCameraTakeShot() {
        pinchArea.enabled = true
        if (settings.soundOn === 1)
            sound.play()
        if (mediaView.index < 0)
            mediaView.folder = StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/furicam2"
        // Single full-resolution capture; the engine writes the JPEG and emits
        // photoSaved(path), handled in onPhotoSaved below.
        cam2.capturePhoto("")
    }

    function handleCameraTakeVideo() { handleVideoRecording() }

    function handleCameraChangeResolution(resolution) {
        // 4:3 / 16:9 toggle — cosmetic until engine capture-size selection lands.
    }

    function handleStopCamera() {
        cam2.stopCamera()
        cameraLoader.active = false
    }

    function handleStartCamera() { cam2.startCamera() }

    function handleSetFocusMode(focusMode) {
        // FocusContinuous -> continuous AF + AE unlocked; FocusAuto (the app's
        // "locked" state) -> hold focus + lock AE.
        if (focusMode === Camera.FocusContinuous) {
            cam2.setAutoFocus()
            cam2.setAELock(false)
        } else {
            cam2.setFocusLock(true)
            cam2.setAELock(true)
        }
    }

    function handleSetFocusPointMode(focusPointMode) {
        // The focus region is driven by the tap handler (cam2.setFocusPoint).
    }

    function handleSetCameraAspWide(aspWide) {
        // Aspect-ratio preference is cosmetic for the Camera2 capture path.
    }

    function handleSetDeviceID(deviceIdToSet) {
        settings.deviceId = deviceIdToSet
        // cameraId 0 = back, 1 = front in our minimal list (initializeCameraList).
        settings.cameraPosition = (deviceIdToSet === 1) ? Camera.FrontFace : Camera.BackFace
        applyCameraPosition()
    }

    function handleSetZoom(zoomLevel) {
        var z = Math.max(0, Math.min(zoomLevel, maxZoom))
        currentZoom = z
        var ratio = 1.0 + (maxZoom > 0 ? (z / maxZoom) : 0) * (cam2.maxZoom() - 1.0)
        cam2.setZoom(ratio)
    }

    // Minimal camera list (back + front) so main.qml's selector is populated.
    // TODO: expose the engine's full camera list (incl. the secondary back/macro).
    function initializeCameraList() {
        allCamerasModel.clear()
        window.backCameras = 0
        window.frontCameras = 0
        allCamerasModel.append({ "cameraId": 0, "index": 0, "position": Camera.BackFace })
        window.backCameras += 1
        if (cam2.hasFrontCamera) {
            allCamerasModel.insert(0, { "cameraId": 1, "index": 1, "position": Camera.FrontFace })
            window.frontCameras += 1
        }
    }

    function applyCameraPosition() {
        var wantFront = (settings.cameraPosition === Camera.FrontFace)
        if (wantFront !== frontActive) {
            cam2.switchCamera()
            frontActive = wantFront
        }
    }

    // Enter/leave video mode, setting the recording size atomically BEFORE video
    // mode so the encoder is built at the right resolution from the start.
    function applyVideoMode() {
        if (typeof cslate === "undefined" || !cam2.ready)
            return
        if (cslate.state === "VideoCapture") {
            cam2.setVideoResolution(settings.videoResWidth, settings.videoResHeight)
            cam2.videoMode = true
        } else {
            cam2.videoMode = false
        }
    }

    function handleVideoRecording() {
        if (!window.videoCaptured) {
            // Lock in the current size right before recording (also covers a
            // resolution change made while already in video mode).
            cam2.setVideoResolution(settings.videoResWidth, settings.videoResHeight)
            cam2.startRecording("")   // attaches the pre-warmed mic, finalizes an MP4
            window.videoCaptured = true
        } else {
            cam2.stopRecording()
            window.videoCaptured = false
        }
    }

    // Post-process + announce a saved photo (fires on the GUI thread).
    function onCam2PhotoSaved(path) {
        if (settings.colorCorrectionEnabled) {
            fileManager.applyColorCorrection(path,
                settings.colorCorrectionRed, settings.colorCorrectionGreen,
                settings.colorCorrectionBlue, settings.colorCorrectionSaturation)
        }
        if (settings.jpegQuality < 100)
            fileManager.reencodeJpeg(path, settings.jpegQuality)
        if (window.locationAvailable === 1)
            fileManager.appendGPSMetadata(path)
        photoSaved()
    }

    // React to camera-position changes (gestures set settings.cameraPosition).
    Connections {
        target: settings
        function onCameraPositionChanged() { cameraItem.applyCameraPosition() }
    }

    // Enter/leave video mode as the photo/video tab changes.
    Connections {
        target: cslate
        function onStateChanged() { cameraItem.applyVideoMode() }
    }


    // ── The live preview: the Camera2 engine item ───────────────────────────
    Camera2Bridge {
        id: cam2
        anchors.fill: parent

        // Video mode + recording size are applied atomically at discrete moments
        // (entering video mode, starting a recording) via applyVideoMode() /
        // handleVideoRecording() — reactive split bindings churned and could
        // produce a mismatched (e.g. 1920x2160) size.
        Component.onCompleted: cam2.startCamera()

        onReadyChanged: {
            if (ready) {
                focusState.state = "Default"
                cameraItem.fnAspectRatio()
                cameraItem.applyVideoMode()   // enter video mode if starting on the video tab
            }
        }
        onCameraError: {
            cameraItem.errorBannerText = message
            cameraItem.errorBannerVisible = true
            errorBannerTimer.restart()
        }
        onPhotoSaved: cameraItem.onCam2PhotoSaved(path)

        // Live color correction, same shader the legacy path used.
        layer.enabled: settings.colorCorrectionEnabled
        layer.effect: ShaderEffect {
            property real redScale:   settings.colorCorrectionRed
            property real greenScale: settings.colorCorrectionGreen
            property real blueScale:  settings.colorCorrectionBlue
            property real saturation: settings.colorCorrectionSaturation
            fragmentShader: "qrc:/colorCorrection.frag"
        }

        PinchArea {
            id: pinchArea
            anchors.fill: parent
            pinch.target: camZoom
            pinch.maximumScale: (cameraItem.maxZoom > 0 ? cameraItem.maxZoom : 1) / camZoom.zoomFactor
            pinch.minimumScale: 0
            enabled: !mediaView.visible && !window.videoCaptured

            MouseArea {
                id: dragArea
                hoverEnabled: true
                anchors.fill: parent
                enabled: !mediaView.visible && !window.videoCaptured
                property real startX: 0
                property real startY: 0
                property int swipeThreshold: 80
                property var lastTapTime: 0
                property int doubleTapInterval: 300

                onPressed: {
                    startX = mouse.x
                    startY = mouse.y
                }

                onReleased: {
                    var deltaX = mouse.x - startX
                    var deltaY = mouse.y - startY

                    var currentTime = new Date().getTime();
                    if (currentTime - lastTapTime < doubleTapInterval) {
                        window.blurView = 1;
                        settings.cameraPosition = settings.cameraPosition === Camera.BackFace ? Camera.FrontFace : Camera.BackFace;
                        settings.flashMode = settings.cameraPosition === Camera.FrontFace ? Camera.FlashOff : settings.flashMode;
                        cameraSwitchDelay.start();
                        lastTapTime = 0;
                    } else {
                        lastTapTime = currentTime;
                        if (Math.abs(deltaY) > Math.abs(deltaX) && Math.abs(deltaY) > swipeThreshold) {
                            if (deltaY > 0) { // Swipe down
                                configBarDrawer.open()
                            } else { // Swipe up — flip camera
                                window.blurView = 1;
                                settings.flashMode = Camera.FlashOff
                                settings.cameraPosition = settings.cameraPosition === Camera.BackFace ? Camera.FrontFace : Camera.BackFace;
                                settings.flashMode = settings.cameraPosition === Camera.FrontFace ? Camera.FlashOff : settings.flashMode;
                                cameraSwitchDelay.start();
                            }
                        } else if (Math.abs(deltaX) > swipeThreshold) {
                            if (deltaX > 0) { // Swipe right
                                window.blurView = 1
                                window.swipeDirection = 0
                                swappingDelay.start()
                            } else { // Swipe left
                                window.blurView = 1
                                window.swipeDirection = 1
                                swappingDelay.start()
                            }
                        } else { // Tap — focus here
                            var relativePoint = Qt.point(mouse.x / width, mouse.y / height)

                            if (aefLockTimer.running) {
                                focusState.state = "TargetLocked"
                                aefLockTimer.stop()
                            } else {
                                focusState.state = "AutomaticFocus"
                                window.aeflock = "AEFLockOff"
                            }

                            if (window.aeflock !== "AEFLockOn" || focusState.state === "TargetLocked") {
                                cam2.setFocusPoint(relativePoint.x, relativePoint.y)
                                focusPointRect.width = 60 * window.scalingRatio
                                focusPointRect.height = 60 * window.scalingRatio
                                window.focusPointVisible = true
                                focusPointRect.x = mouse.x - (focusPointRect.width / 2)
                                focusPointRect.y = mouse.y - (focusPointRect.height / 2)
                            }

                            window.blurView = 0
                            configBarDrawer.close()
                            optionContainer.state = "closed"
                            visTm.start()
                        }
                    }
                }
            }

            onPinchUpdated: {
                camZoom.zoom = pinch.scale * camZoom.zoomFactor
            }

            Rectangle {
                id: focusPointRect
                border { width: 2; color: "#FDD017" }
                color: "transparent"
                radius: 5 * window.scalingRatio
                width: 80 * window.scalingRatio
                height: 80 * window.scalingRatio
                visible: window.focusPointVisible

                Timer {
                    id: visTm
                    interval: 500; running: false; repeat: false
                    onTriggered: window.aeflock === "AEFLockOff" ? window.focusPointVisible = false : null
                }
            }

            // 3x3 grid overlay
            Item {
                id: gridOverlay
                anchors.fill: parent
                visible: settings.gridEnabled === 1
                enabled: false
                z: 1
                Rectangle { x: parent.width / 3;       width: 1; height: parent.height; color: "#50ffffff" }
                Rectangle { x: parent.width * 2 / 3;   width: 1; height: parent.height; color: "#50ffffff" }
                Rectangle { y: parent.height / 3;      width: parent.width; height: 1;  color: "#50ffffff" }
                Rectangle { y: parent.height * 2 / 3;  width: parent.width; height: 1;  color: "#50ffffff" }
            }

            // Level indicator
            Item {
                id: levelIndicator
                visible: settings.levelEnabled === 1
                anchors.centerIn: parent
                width: parent.width * 0.35
                height: 30 * window.scalingRatio
                rotation: window.levelAngle
                enabled: false
                z: 2
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: 2 * window.scalingRatio
                    color: window.isLevel ? "#4CAF50" : "#80ffffff"
                    radius: 1
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 8 * window.scalingRatio
                    height: 8 * window.scalingRatio
                    radius: 4 * window.scalingRatio
                    color: window.isLevel ? "#4CAF50" : "#80ffffff"
                    border.width: 1
                    border.color: window.isLevel ? "#388E3C" : "#40ffffff"
                    Behavior on color { ColorAnimation { duration: 200 } }
                }
            }
        }

        // Dim overlay during transitions.
        Rectangle {
            anchors.fill: parent
            opacity: blurView ? 1 : 0
            color: "#40000000"
            visible: opacity != 0
            Behavior on opacity { NumberAnimation { duration: 300 } }
        }
    }

    // ── Pipeline-error banner (kept for surfacing engine errors) ────────────
    property bool errorBannerVisible: false
    property string errorBannerText: ""
    Timer {
        id: errorBannerTimer
        interval: 5000
        repeat: false
        onTriggered: cameraItem.errorBannerVisible = false
    }
    Rectangle {
        id: errorBanner
        z: 10000
        anchors.top: parent.top
        anchors.topMargin: 16
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 24, errorBannerLabel.implicitWidth + 32)
        height: errorBannerLabel.implicitHeight + 20
        radius: 8
        color: "#dd8a1c1c"
        border.color: "#ffffff"
        border.width: 1
        visible: cameraItem.errorBannerVisible || opacity > 0
        opacity: cameraItem.errorBannerVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Text {
            id: errorBannerLabel
            anchors.fill: parent
            anchors.margins: 10
            text: cameraItem.errorBannerText
            color: "white"
            font.pixelSize: 14
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        MouseArea {
            anchors.fill: parent
            onClicked: cameraItem.errorBannerVisible = false
        }
    }

    // Pinch-zoom helper (drives handleSetZoom as the pinch scale changes).
    Item {
        id: camZoom
        property real zoomFactor: 2.0
        property real zoom: 0
        NumberAnimation on zoom { duration: 200; easing.type: Easing.InOutQuad }
        onScaleChanged: cameraItem.handleSetZoom(scale * zoomFactor)
    }

    FastBlur {
        id: vBlur
        anchors.fill: parent
        opacity: blurView ? 1 : 0
        source: cam2
        radius: 128
        visible: opacity != 0
        transparentBorder: false
        Behavior on opacity { NumberAnimation { duration: 300 } }
    }

    Glow {
        anchors.fill: vBlur
        opacity: blurView ? 1 : 0
        radius: 4
        samples: 1
        color: "black"
        source: vBlur
        visible: opacity != 0
        Behavior on opacity { NumberAnimation { duration: 300 } }
    }
}
