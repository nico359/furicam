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
    property alias videoResolutionModel: videoResModel
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
    ListModel { id: videoResModel }

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
                "mp": mp, "label": mp + " MP (" + w + "×" + h + ")  " + (w / g) + ":" + (h / g)
            })
        }
        // Default to the largest (first) size until the user picks one.
        if (res.length > 0 && currentResWidth === 0) {
            currentResWidth = res[0].width
            currentResHeight = res[0].height
        }
    }

    // Short consumer name for the common 16:9 video sizes; 4:3 / 1:1 sizes have no
    // standard short name, so they get none.
    function videoResShortName(w, h) {
        if (Math.abs(w / h - 16 / 9) > 0.05) return ""
        if (h >= 2160) return "4K"
        if (h >= 1440) return "1440p"
        if (h >= 1080) return "1080p"
        if (h >= 720)  return "HD"
        return "SD"
    }

    // Populate the video-resolution picker from the engine's encoder-capable sizes
    // (HAL PRIVATE sizes the H.264 encoder accepts), largest first.
    function fnVideoResolutions() {
        videoResModel.clear()
        var res = cam2.availableVideoResolutions()
        for (var i = 0; i < res.length; i++) {
            var w = res[i].width
            var h = res[i].height
            var g = gcd(w, h)
            var mp = Math.round((w * h) / 100000) / 10
            var sn = videoResShortName(w, h)
            videoResModel.append({
                "resWidth": w, "resHeight": h,
                "label": mp + " MP (" + w + "×" + h + ")  " + (w / g) + ":" + (h / g) + (sn ? "  " + sn : "")
            })
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
        // No-op: the flash mode is applied via the reactive `flashMode` binding on
        // cam2 (settings.flashMode → engine), which also covers startup.  Doing an
        // imperative cam2.setFlashMode here would break that binding.
    }

    function handleCameraTakeShot() {
        pinchArea.enabled = true
        freezeCurrentPreview()         // hold ~what you shot while the still capture stalls preview
        window.triggerCaptureFlash()   // immediate shutter cue
        if (settings.soundOn === 1)
            sound.play()
        if (mediaView.index < 0)
            mediaView.folder = StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/furicam2"
        // Capture directly at the chosen quality (no on-disk re-encode afterward).
        cam2.setJpegQuality(settings.jpegQuality)
        // Single full-resolution capture; the engine writes the JPEG and emits
        // photoSaved(path), handled in onPhotoSaved below.
        cam2.capturePhoto("")
    }

    // Grab the current preview frame and hold it over the viewfinder during the
    // still-capture stall, so the user instantly sees ~what they captured.  It
    // fades out the moment fresh live frames resume (frozenFrame's Connections).
    function freezeCurrentPreview() {
        cam2.grabToImage(function(result) {
            if (!result)
                return
            frozenFrame.source = result.url
            frozenFrame.baseCount = cam2.frameCount
            frozenFrame.opacity = 1
            frozenHideTimer.restart()
        })
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
        var cams = cam2.availableCameras()
        var facing = (cams[deviceIdToSet] !== undefined) ? cams[deviceIdToSet].facing : 1  // 0=front,1=back
        // Keep the gesture/mirror state in sync first so the cameraPosition change
        // below doesn't trigger applyCameraPosition() into a redundant switch.
        frontActive = (facing === 0)
        settings.cameraPosition = (facing === 0) ? Camera.FrontFace : Camera.BackFace
        cam2.selectCamera(deviceIdToSet)
    }

    function handleSetZoom(zoomLevel) {
        var z = Math.max(0, Math.min(zoomLevel, maxZoom))
        currentZoom = z
        var ratio = 1.0 + (maxZoom > 0 ? (z / maxZoom) : 0) * (cam2.maxZoom() - 1.0)
        cam2.setZoom(ratio)
    }

    // Brightness via exposure compensation; ev in [0,1] (0.5 = neutral).  The
    // engine maps it onto the camera's real AE-compensation range and applies it
    // to the active request — so it brightens the live video while recording.
    function handleSetBrightness(ev) {
        cam2.setExposureCompensation(ev)
    }

    // Video frame-rate mode: 0 = steady 30 fps; 1 = auto (5–30), which lets
    // auto-exposure drop the rate in low light for a brighter, lower-noise frame.
    function handleSetVideoFps(mode) {
        if (mode === 1) cam2.setVideoFps(5, 30)
        else            cam2.setVideoFps(30, 30)
    }

    // Apply a chosen video resolution now (atomic w+h, avoiding split-binding
    // churn): re-letterboxes the preview to the new aspect immediately and rebuilds
    // the video session at the new size when in video mode (not while recording).
    function handleSetVideoResolution(w, h) {
        cam2.setVideoResolution(w, h)
    }

    // H.264 video bitrate (kbps from the slider).
    function handleSetVideoBitrate(kbps) {
        cam2.setVideoBitrate(kbps)
    }

    // Scene mode: 0 = normal/auto, 2 = ACTION (freeze motion).
    function handleSetSceneMode(mode) {
        cam2.setSceneMode(mode)
    }
    // Tone map: 0 = Standard (HAL default), 1 = HDR (in-ISP DRO — lifts shadows /
    // rolls off highlights), 2 = Contrast (punchier).  Applied to preview + capture.
    function handleSetToneMap(type) {
        cam2.setToneMap(type)
    }
    // HDR/Contrast tone-curve strength [0..0.85], applied live.
    function handleSetDroStrength(k) {
        cam2.setDroStrength(k)
    }

    // RAW (DNG): also save a color-accurate raw alongside the JPEG (the RAW16
    // stream replaces live QR while on).  No-op if the camera lacks RAW capability.
    function handleSetRaw(on) {
        cam2.setRawEnabled(on)
    }

    // Zebra: stripe blown highlights + crushed shadows on the preview (exposure aid).
    function handleSetZebra(on) {
        cam2.setZebra(on)
    }

    // Post-processing levels: 0=off, 1=fast, 2=high quality (HQ on stills).
    function handleSetNoiseReduction(level) {
        cam2.setNoiseReduction(level)
    }
    function handleSetEdgeEnhancement(level) {
        cam2.setEdgeEnhancement(level)
    }

    // Populate the camera selector from the engine's full list (incl. the
    // secondary back/macro camera), keyed by camera index.
    function initializeCameraList() {
        allCamerasModel.clear()
        window.backCameras = 0
        window.frontCameras = 0
        var cams = cam2.availableCameras()
        for (var i = 0; i < cams.length; i++) {
            var c = cams[i]   // {index, facing(0=front,1=back), megapixels}
            allCamerasModel.append({
                "cameraId": c.index, "index": c.index,
                "position": (c.facing === 1) ? Camera.BackFace : Camera.FrontFace
            })
            if (settings.cameras[c.index])
                settings.cameras[c.index].resolution = c.megapixels
            if (c.facing === 1) window.backCameras += 1
            else window.frontCameras += 1
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
            // Color correction re-encodes; save directly at the chosen quality so
            // there's no separate re-encode pass (the shot was already captured at
            // this quality when correction is off).
            fileManager.applyColorCorrection(path,
                settings.colorCorrectionRed, settings.colorCorrectionGreen,
                settings.colorCorrectionBlue, settings.colorCorrectionSaturation,
                settings.jpegQuality)
        }
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

    // ── Live QR scanning ─────────────────────────────────────────────────────
    // The engine decodes QR codes from the analysis stream (photo mode) and the
    // bridge emits qrDetected(text, points).  Show a tappable result banner;
    // tapping it parses the code and offers the matching action (open/connect/copy).
    property string qrText: ""
    property var    qrPoints: []
    property bool   qrVisible: false

    Connections {
        target: cam2
        function onQrDetected(text, points) {
            cameraItem.qrText = text
            cameraItem.qrPoints = points
            cameraItem.qrVisible = true
            qrClearTimer.restart()
        }
    }
    Timer {
        id: qrClearTimer
        interval: 1500
        onTriggered: cameraItem.qrVisible = false
    }

    function handleQrTap() {
        if (!qrText.length)
            return
        var qrType = QRCodeHandler.parseQrString(qrText)
        if (qrType === "URL") {
            window.openPopup("Open URL?", qrText,
                [{text: "Cancel"}, {text: "Copy"}, {text: "Open", isPrimary: true}], qrText)
        } else if (qrType === "WIFI") {
            var wifiID = QRCodeHandler.getWifiId()
            window.openPopup("Connect to Network?", wifiID,
                [{text: "Cancel"}, {text: "Connect", isPrimary: true}], wifiID)
        } else {
            window.openPopup("QR Code Detected", "Content: " + qrText,
                [{text: "OK", isPrimary: true}, {text: "Copy"}], qrText)
        }
    }


    // ── The live preview: the Camera2 engine item ───────────────────────────
    // Dark backdrop behind the letterboxed preview (fills the aspect-ratio bars).
    Rectangle {
        anchors.fill: parent
        color: "black"
        z: -1
    }

    Camera2Bridge {
        id: cam2
        // Letterbox the preview to its capture-matched aspect (WYSIWYG).  The aspect
        // itself is decided in the bridge (mid layer); previewAspectRatio is the
        // on-screen width/height.  Children (grid/focus/zoom) ride along for free.
        property real dispAspect: previewAspectRatio > 0 ? previewAspectRatio : (9.0 / 16.0)
        width: Math.min(parent.width, parent.height * dispAspect)
        height: width / dispAspect
        anchors.centerIn: parent
        hdrEnabled: settings.hdrEnabled   // HDR burst+fuse handled in the bridge
        // Flash mode tracks the GUI setting reactively (applied on startup + every
        // change), mapping the QtMultimedia enum to the engine's 0=off/1=on/2=auto.
        flashMode: (settings.flashMode === Camera.FlashOn) ? 1
                 : (settings.flashMode === Camera.FlashAuto) ? 2 : 0

        // Video mode + recording size are applied atomically at discrete moments
        // (entering video mode, starting a recording) via applyVideoMode() /
        // handleVideoRecording() — reactive split bindings churned and could
        // produce a mismatched (e.g. 1920x2160) size.
        Component.onCompleted: cam2.startCamera()

        onReadyChanged: {
            if (ready) {
                focusState.state = "Default"
                cameraItem.fnAspectRatio()
                cameraItem.fnVideoResolutions()
                cam2.setExposureCompensation(settings.brightnessEv)   // restore brightness after (re)start
                cameraItem.handleSetVideoFps(settings.videoFpsMode)   // restore fps mode before video mode
                if (settings.videoBitrate > 40000)                    // enforce the 40 Mbps ceiling on older saved values
                    settings.videoBitrate = 40000
                cam2.setVideoBitrate(settings.videoBitrate)           // restore chosen video bitrate
                cam2.setDroStrength(settings.droStrength)             // tone-curve strength
                cam2.setToneMap(settings.toneMap)                     // restore tone map (0/1/2)
                cam2.setSceneMode(settings.sceneMode)                 // restore scene mode (0=normal, 2=action)
                cam2.setRawEnabled(settings.rawEnabled)               // restore RAW (DNG) capture
                cam2.setZebra(settings.zebraEnabled)                   // restore clipping overlay
                cam2.setNoiseReduction(settings.noiseReductionLevel)   // restore denoise level (0/1/2)
                cam2.setEdgeEnhancement(settings.edgeLevel)            // restore sharpening level (0/1/2)
                cameraItem.applyVideoMode()   // enter video mode if starting on the video tab
                // Sync GUI position state to the camera that actually opened (bridge
                // ground truth) — the flash button and other UI gate on
                // settings.cameraPosition, and reading frontActive here would race
                // with the switch that triggered this signal.
                frontActive = (cam2.currentFacing() === 0)   // 0=front
                settings.cameraPosition = frontActive ? Camera.FrontFace : Camera.BackFace
            }
        }
        onCameraError: {
            cameraItem.errorBannerText = message
            cameraItem.errorBannerVisible = true
            errorBannerTimer.restart()
        }
        onPhotoSaved: cameraItem.onCam2PhotoSaved(path)
        // A finished recording also needs the gallery to rescan; reuse the same
        // photoSaved() signal that main.qml wires to the gallery refresh timer.
        onRecordingSaved: cameraItem.photoSaved()

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

    // Boxes around faces the HAL detects (Face capture mode); normalized rects
    // from the bridge map onto the letterboxed preview, same frame as the QR box.
    Item {
        id: faceOverlay
        z: 8999
        anchors.fill: cam2
        visible: settings.sceneMode === 1 && !mediaView.visible && !window.videoCaptured
        property var faces: []

        Connections {
            target: cam2
            function onFacesDetected(f) { faceOverlay.faces = f }
        }

        Repeater {
            model: faceOverlay.faces
            delegate: Rectangle {
                color: "transparent"
                border.color: "#3dff7a"
                border.width: 2 * window.scalingRatio
                radius: 6 * window.scalingRatio
                x: modelData.x * faceOverlay.width
                y: modelData.y * faceOverlay.height
                width:  modelData.w * faceOverlay.width
                height: modelData.h * faceOverlay.height
            }
        }
    }

    // Box drawn around the detected QR code (tappable → action popup).
    Item {
        id: qrOverlay
        z: 9000
        anchors.fill: cam2   // map normalized QR points onto the letterboxed preview
        visible: cameraItem.qrVisible && !mediaView.visible
                 && (typeof cslate === "undefined" || cslate.state === "PhotoCapture")
        property bool valid: cameraItem.qrPoints && cameraItem.qrPoints.length === 4
        property real minX: valid ? Math.min(qrPoints[0].x, qrPoints[1].x, qrPoints[2].x, qrPoints[3].x) : 0
        property real maxX: valid ? Math.max(qrPoints[0].x, qrPoints[1].x, qrPoints[2].x, qrPoints[3].x) : 0
        property real minY: valid ? Math.min(qrPoints[0].y, qrPoints[1].y, qrPoints[2].y, qrPoints[3].y) : 0
        property real maxY: valid ? Math.max(qrPoints[0].y, qrPoints[1].y, qrPoints[2].y, qrPoints[3].y) : 0

        Rectangle {
            id: qrBox
            visible: qrOverlay.valid
            x: qrOverlay.minX * qrOverlay.width
            y: qrOverlay.minY * qrOverlay.height
            width:  Math.max(40 * window.scalingRatio, (qrOverlay.maxX - qrOverlay.minX) * qrOverlay.width)
            height: Math.max(40 * window.scalingRatio, (qrOverlay.maxY - qrOverlay.minY) * qrOverlay.height)
            radius: 8 * window.scalingRatio
            color: "#330099ff"
            border.color: "#3399ff"
            border.width: 3 * window.scalingRatio

            // Decoded text label just above the box.
            Rectangle {
                anchors.bottom: parent.top
                anchors.bottomMargin: 6 * window.scalingRatio
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(cameraItem.width - 24 * window.scalingRatio,
                                qrLabel.implicitWidth + 24 * window.scalingRatio)
                height: qrLabel.implicitHeight + 12 * window.scalingRatio
                radius: 8 * window.scalingRatio
                color: "#dd1d6fcf"
                Text {
                    id: qrLabel
                    anchors.centerIn: parent
                    width: parent.width - 16 * window.scalingRatio
                    text: cameraItem.qrText
                    color: "white"; font.pixelSize: 13 * window.scalingRatio
                    elide: Text.ElideRight; maximumLineCount: 1
                    horizontalAlignment: Text.AlignHCenter
                }
            }
            MouseArea { anchors.fill: parent; onClicked: cameraItem.handleQrTap() }
        }
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

    // ── Capture freeze-frame ─────────────────────────────────────────────────
    // Grabbed copy of the preview at shutter time, held over the viewfinder while
    // the still capture stalls the live preview; fades the instant live resumes.
    Image {
        id: frozenFrame
        anchors.fill: cam2
        z: 8000
        fillMode: Image.PreserveAspectCrop
        cache: false
        opacity: 0
        visible: opacity > 0
        property int baseCount: 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
    }

    Timer {
        id: frozenHideTimer
        interval: 4000   // safety: never hold the frozen frame indefinitely
        onTriggered: frozenFrame.opacity = 0
    }

    Connections {
        target: cam2
        // A few fresh live frames after the capture stall ⇒ preview is back; reveal it.
        function onFrameCountChanged() {
            if (frozenFrame.opacity > 0 && cam2.frameCount - frozenFrame.baseCount >= 3) {
                frozenFrame.opacity = 0
                frozenHideTimer.stop()
            }
        }
    }
}
