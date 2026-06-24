// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2023 Droidian Project
// Copyright (C) 2024 Furi Labs
//
// Authors:
// Bardia Moshiri <fakeshell@bardia.tech>
// Erik Inkinen <erik.inkinen@gmail.com>
// Alexander Rutz <alex@familyrutz.com>
// Joaquin Philco <joaquinphilco@gmail.com>

import QtQuick 2.15
import QtMultimedia 5.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15
import Qt.labs.folderlistmodel 2.15
import Qt.labs.platform 1.1

Rectangle {
    id: viewRect
    property int index: -1
    property var lastImg: index == -1 ? "" : imgModel.get(viewRect.index, "fileUrl")
    property string currentFileUrl: viewRect.index === -1 || imgModel.get(viewRect.index, "fileUrl") === undefined ? "" : imgModel.get(viewRect.index, "fileUrl").toString()
    property var folder: cslate.state == "VideoCapture" ?
                         StandardPaths.writableLocation(StandardPaths.MoviesLocation) + "/furicam" :
                         StandardPaths.writableLocation(StandardPaths.PicturesLocation) + "/furicam"
    // Internal toggle used by refresh() to force FolderListModel rescan.
    property bool _refreshClearing: false
    property var deletePopUp: "closed"
    property bool hideMediaInfo: false
    property bool showShapes: true
    property real scalingRatio: scalingRatio
    property var scaleRatio: 1.0
    property var vCenterOffsetValue: 0
    property var textSize: viewRect.height * 0.018
    property var mediaState: MediaPlayer.StoppedState
    // Display rotation (deg) for the current video; the muxer stores it as a hint
    // the QML VideoOutput doesn't auto-apply on this backend.
    property int videoRotation: 0
    property var videoAudio: false
    signal playbackRequest()
    signal scanImageComponent()
    signal closed
    color: "black"
    visible: false

    onCurrentFileUrlChanged: {
        viewRect.scanImageComponent()
        viewRect.videoRotation = isVideoFile(currentFileUrl)
                                 ? fileManager.getVideoRotation(currentFileUrl) : 0
    }

    function openPopup(title, body, buttons, data) {
        popupTitle = title
        popupBody = body
        popupButtons = buttons
        popupData = data
        popupState = "opened"
    }

    // Forces FolderListModel to rescan the folder by briefly setting the folder
    // to "" (one event-loop turn) then restoring it, bypassing Qt's change-batching.
    Timer {
        id: _refreshRestoreTimer
        interval: 50
        repeat: false
        onTriggered: { viewRect._refreshClearing = false }
    }

    function refresh() {
        viewRect._refreshClearing = true
        _refreshRestoreTimer.start()
    }

    // Video files may be .mp4 (current encoder) or .mkv (older recordings);
    // treat both as video throughout the gallery.
    function isVideoFile(u) {
        if (!u) return false
        u = u.toString()
        return u.endsWith(".mp4") || u.endsWith(".mkv")
    }

    onVisibleChanged: {
        if (!visible) qrCodeComponent.lastValidResult =  null
    }

    Connections {
        target: thumbnailGenerator

        function onThumbnailGenerated(image) {
            viewRect.lastImg = thumbnailGenerator.toQmlImage(image);
        }
    }

    FolderListModel {
        id: imgModel
        // _refreshClearing momentarily empties the folder, forcing a full rescan
        // when it returns to false (needed because inotify is unavailable on device).
        folder: viewRect._refreshClearing ? "" : viewRect.folder
        showDirs: false
        nameFilters: cslate.state == "VideoCapture" ? ["*.mp4", "*.mkv"] : ["*.jpg"]
        // Sort by modification time, newest LAST, so onStatusChanged's
        // `count - 1` is the most recent capture.  (Name sort is wrong here:
        // it's case-sensitive, so legacy lowercase "image*" files sort after
        // the engine's "IMG_*" files and the newest-looking entry got stuck on
        // an old legacy photo.)
        sortField: FolderListModel.Time
        sortReversed: true

        onStatusChanged: {
            if (imgModel.status == FolderListModel.Ready) {
                viewRect.index = imgModel.count - 1
                if (cslate.state == "VideoCapture" && viewRect.isVideoFile(viewRect.currentFileUrl)) {
                    thumbnailGenerator.setVideoSource(viewRect.currentFileUrl)
                } else {
                    viewRect.lastImg = viewRect.currentFileUrl
                }
            }
        }
    }

    Loader {
        id: mediaLoader
        anchors.fill: parent
        visible: parent.visible
        property string loadedComponentType: ""

        sourceComponent: {
            if (viewRect.index === -1) {
                loadedComponentType = "empty";
                return emptyDirectoryComponent;
            } else if (imgModel.get(viewRect.index, "fileUrl") === undefined) {
                loadedComponentType = "null";
                return null;
            } else if (viewRect.isVideoFile(imgModel.get(viewRect.index, "fileUrl"))) {
                loadedComponentType = "video";
                return videoOutputComponent;
            } else {
                loadedComponentType = "image";
                return imageComponent;
            }
        }

        onVisibleChanged: {
            if (visible && loadedComponentType === "image") {
                viewRect.scanImageComponent.connect(mediaLoader.item.scanImage)
            }
        }
    }

    function swipeGesture(deltaX, deltaY, swipeThreshold) {
        if (Math.abs(deltaY) > Math.abs(deltaX)) {
            if (deltaY < -swipeThreshold) { // Upward swipe
                viewRect.scaleRatio = 0.7
                viewRect.vCenterOffsetValue = -(viewRect.height * 0.19)
                drawerAnimation.to = parent.height - 70 - metadataDrawer.height
                drawerAnimation.start()
            } else if (deltaY > swipeThreshold) { // Downward swipe
                viewRect.scaleRatio = 1.0
                viewRect.vCenterOffsetValue = 0
                drawerAnimation.to = parent.height
                drawerAnimation.start()
            }
            qrCodeComponent.lastValidResult =  null
            viewRect.hideMediaInfo = false
        } else if (Math.abs(deltaX) > swipeThreshold) {
            if (deltaX > 0) { // Swipe right
                if (viewRect.index > 0) {
                    viewRect.index -= 1
                }
            } else { // Swipe left
                if (viewRect.index < imgModel.count - 1) {
                    viewRect.index += 1
                }
            }
            qrCodeComponent.lastValidResult =  null
            viewRect.hideMediaInfo = false
        } else { // Touch
            qrCodeComponent.lastValidResult =  null
            if (viewRect.hideMediaInfo === false) {
                viewRect.scaleRatio = 1.0
                viewRect.vCenterOffsetValue = 0
            } else {
                if (metadataDrawer.y <= 600) {
                    // Match the swipe-up "peek" size (was 0.5 here, 0.7 there),
                    // so toggling the widgets returns the photo to the same size.
                    viewRect.scaleRatio = 0.7
                    viewRect.vCenterOffsetValue = -(viewRect.height * 0.19)
                }
            }

            viewRect.hideMediaInfo = !viewRect.hideMediaInfo
        }
    }

    function scalePoint(point, readWidth, readHeight, imgWidth, imgHeight) {
        var scaledX = (point.x / readWidth) * imgWidth;
        var scaledY = (point.y / readHeight) * imgHeight;
        return Qt.point(scaledX, scaledY);
    }

    function getScaledCorners(position, readWidth, readHeight, imgWidth, imgHeight) {
        var scaledTopLeft = scalePoint(position.topLeft, readWidth, readHeight, imgWidth, imgHeight);
        var scaledTopRight = scalePoint(position.topRight, readWidth, readHeight, imgWidth, imgHeight);
        var scaledBottomLeft = scalePoint(position.bottomLeft, readWidth, readHeight, imgWidth, imgHeight);
        var scaledBottomRight = scalePoint(position.bottomRight, readWidth, readHeight, imgWidth, imgHeight);

        return {
            topLeft: scaledTopLeft,
            topRight: scaledTopRight,
            bottomLeft: scaledBottomLeft,
            bottomRight: scaledBottomRight
        };
    }

    Component {
        id: emptyDirectoryComponent

        Item {
            id: emptyDirectoryItem
            anchors.fill: parent

            Column {
                anchors.centerIn: parent

                Button {
                    implicitWidth: 200 * viewRect.scalingRatio
                    implicitHeight: 200 * viewRect.scalingRatio

                    icon.source: "icons/emblemPhotosSymbolic.svg"
                    icon.width: Math.round(200 * viewRect.scalingRatio)
                    icon.height: Math.round(200 * viewRect.scalingRatio)
                    icon.color: "#8a8a8f"

                    anchors.horizontalCenter: parent.horizontalCenter

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }
                }

                Text {
                    text: "No media found"
                    color: "#8a8a8f"
                    font.bold: true
                    font.pixelSize: textSize * 2
                    style: Text.Raised
                    elide: Text.ElideRight
                    anchors.horizontalCenter: parent.horizontalCenter
                }
            }
        }
    }

    Component {
        id: imageComponent
        Item {
            id: imageContainer
            anchors.fill: parent
            property var positionData

            Image {
                id: image
                width: viewRect.width
                autoTransform: true
                transformOrigin: Item.Center
                scale: viewRect.scaleRatio
                fillMode: Image.PreserveAspectFit
                smooth: true
                source: (viewRect.currentFileUrl && !viewRect.isVideoFile(viewRect.currentFileUrl)) ? viewRect.currentFileUrl : ""

                // Pan offsets (px), applied when zoomed in.  x is otherwise 0 and y
                // keeps the image vertically centred (plus the metadata-drawer peek).
                property real panX: 0
                property real panY: 0
                x: image.panX
                y: parent.height / 2 - height / 2 + viewRect.vCenterOffsetValue + image.panY

                // Largest pan that still keeps the scaled image covering the view.
                function clampPanX(v) {
                    var m = Math.max(0, (paintedWidth * scale - viewRect.width) / 2)
                    return Math.max(-m, Math.min(m, v))
                }
                function clampPanY(v) {
                    var m = Math.max(0, (paintedHeight * scale - viewRect.height) / 2)
                    return Math.max(-m, Math.min(m, v))
                }

                // New photo: clear pinch-zoom and pan, but keep the metadata "peek"
                // (scaleRatio / vCenterOffset) so the half-height + open-properties
                // view persists when swiping between photos.
                onSourceChanged: {
                    image.scale = Qt.binding(function() { return viewRect.scaleRatio })
                    image.panX = 0
                    image.panY = 0
                }

                Behavior on scale {
                    NumberAnimation {
                        duration: 300
                        easing.type: Easing.InOutQuad
                    }
                }
                Behavior on y {
                    // Don't animate while the user is dragging to pan.
                    enabled: !galleryDragArea.panning
                    NumberAnimation{
                        duration: 300
                        easing.type: Easing.InOutQuad
                    }
                }
            }

            function scanImageURL() {
                var result = QRCodeHandler.scanImageURL(currentFileUrl, image.width, image.height)

                if (result.isValid) {
                    imageContainer.positionData = getScaledCorners(result.position, result.readWidth, result.readHeight, image.width, image.height)

                    qrCodeComponent.smoothedPosition = imageContainer.positionData

                    qrCodeComponent.updateLowPass(imageContainer.positionData);

                    qrCodeComponent.updateOBBFromImage(imageContainer.positionData, image.scale, image.x, image.y);

                    qrCodeComponent.lastValidResult = result
                } else {
                    qrCodeComponent.lastValidResult = null
                }
            }

            function scanImage() {
                image.grabToImage(function(result) {
                    if (result.image) {
                        var qrCodeResult = QRCodeHandler.scanImage(result.image);

                        if (qrCodeResult.isValid) {
                            imageContainer.positionData = getScaledCorners(qrCodeResult.position, qrCodeResult.readWidth, qrCodeResult.readHeight, image.width, image.height)

                            qrCodeComponent.smoothedPosition = imageContainer.positionData

                            qrCodeComponent.updateLowPass(imageContainer.positionData);

                            qrCodeComponent.updateOBBFromImage(imageContainer.positionData, image.scale, image.x, image.y);

                            qrCodeComponent.lastValidResult = qrCodeResult
                        } else {
                            qrCodeComponent.lastValidResult = null
                        }
                    } else {
                        console.error("Failed to grab image for QR scanning.");
                        qrCodeComponent.lastValidResult = null;
                    }
                });
            }

            PinchArea {
                id: pinchArea
                anchors.fill: parent
                pinch.target: image
                pinch.maximumScale: 4
                pinch.minimumScale: 1
                enabled: viewRect.visible

                onPinchUpdated: {
                    if (pinchArea.pinch.center !== undefined)
                        image.scale = pinchArea.pinch.scale
                }
                onPinchFinished: {
                    image.panX = image.clampPanX(image.panX)
                    image.panY = image.clampPanY(image.panY)
                }

                MouseArea {
                    id: galleryDragArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: deletePopUp === "closed"
                    property real startX: 0
                    property real startY: 0
                    property real panStartX: 0
                    property real panStartY: 0
                    property bool panning: false
                    property int swipeThreshold: 30

                    onPressed: {
                        startX = mouse.x
                        startY = mouse.y
                        panStartX = image.panX
                        panStartY = image.panY
                        panning = false
                    }

                    // While zoomed in, dragging pans the photo instead of swiping to
                    // the next/previous one, so the two gestures don't collide.
                    onPositionChanged: {
                        if (!pinchArea.pinch.active && image.scale > 1.01) {
                            panning = true
                            image.panX = image.clampPanX(panStartX + (mouse.x - startX))
                            image.panY = image.clampPanY(panStartY + (mouse.y - startY))
                        }
                    }

                    onPressAndHold: {
                        if (image.scale <= 1.01)
                            scanImageURL()
                    }

                    onReleased: {
                        if (panning) {
                            panning = false
                            return
                        }

                        var deltaX = mouse.x - startX
                        var deltaY = mouse.y - startY

                        if (mediaMenu.visible){
                            scanImageTimer.start()
                        }

                        swipeGesture(deltaX, deltaY, swipeThreshold)
                    }
                }
            }

            Timer {
                id: updateObbFromImageScan
                interval: 1000 / 120
                running: !!qrCodeComponent.lastValidResult && !qrCodeComponent.viewfinder
                onTriggered: {
                    if (!qrCodeComponent.lastValidResult) return

                    qrCodeComponent.updateOBBFromImage(imageContainer.positionData, image.scale, image.x, image.y)
                    updateObbFromImageScan.start()
                }
            }

            Timer {
                id: scanImageTimer
                interval: 1000 / 120
                running: false;
                repeat: false
                onTriggered: {
                    if (mediaDate.visible){
                        scanImageURL()
                    }
                }
            }

            onVisibleChanged: {
                if (visible) {
                    scanImage()
                }
            }
        }
    }

    MetadataView {
        id: metadataDrawer
        width: parent.width
        height: parent.height / 2.6
        y: parent.height
        visible: !viewRect.hideMediaInfo

        PropertyAnimation {
            id: drawerAnimation
            target: metadataDrawer
            property: "y"
            duration: 500
            easing.type: Easing.InOutQuad
        }

        currentFileUrl: viewRect.currentFileUrl
        textSize: viewRect.textSize
        scalingRatio: viewRect.scalingRatio
    }

    Component {
        id: videoOutputComponent

        Item {
            id: videoItem
            anchors.fill: parent
            property bool firstFramePlayed: false

            signal playbackStateChange()

            Connections {
                target: viewRect
                function onPlaybackRequest() {
                    playbackStateChangeHandler()
                }
            }

            MediaPlayer {
                id: mediaPlayer
                autoPlay: true
                muted: viewRect.videoAudio
                source: viewRect.visible ? viewRect.currentFileUrl : ""

                onSourceChanged: {
                    firstFramePlayed = false;
                    play();
                }

                onPositionChanged: {
                    if (position > 0 && !firstFramePlayed) {
                        pause();
                        firstFramePlayed = true;
                    }
                }

                onStopped: {
                    viewRect.mediaState = MediaPlayer.StoppedState
                    playVideoButtonFrame.visible = true
                }

                onPaused: {
                    viewRect.mediaState = MediaPlayer.PausedState
                    playVideoButtonFrame.visible = true
                }
            }

            VideoOutput {
                anchors.fill: parent
                source: mediaPlayer
                // Apply the clip's stored rotation hint (the backend won't).
                orientation: viewRect.videoRotation
                visible: viewRect.currentFileUrl && viewRect.isVideoFile(viewRect.currentFileUrl)
            }

            function playbackStateChangeHandler() {
                if (mediaPlayer.playbackState === MediaPlayer.PlayingState) {
                    mediaPlayer.pause();
                } else {
                    if (viewRect.visible == true) {
                        mediaPlayer.play();
                    }
                }
            }

            MouseArea {
                id: galleryDragArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: deletePopUp === "closed"
                property real startX: 0
                property real startY: 0
                property int swipeThreshold: 30

                onPressed: {
                    startX = mouse.x
                    startY = mouse.y
                }

                onReleased: {
                    var deltaX = mouse.x - startX
                    var deltaY = mouse.y - startY

                    swipeGesture(deltaX, deltaY, swipeThreshold)
                }
            }

            Rectangle {
                id: playVideoButtonFrame
                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenter: videoItem.horizontalCenter
                width: 90 * viewRect.scalingRatio
                height: 90 * viewRect.scalingRatio
                radius: 150 * viewRect.scaleRatio
                color: "#2b292a"

                Button {
                    id: playVideoButton
                    icon.source: "icons/playVideo.svg"
                    icon.color: "#f0f0f0"
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2 * viewRect.scalingRatio
                    icon.width: 50 * viewRect.scalingRatio
                    icon.height: 50 * viewRect.scalingRatio
                    visible: true
                    flat: true
                    highlighted: false
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (viewRect.isVideoFile(viewRect.currentFileUrl)) {
                            viewRect.mediaState = MediaPlayer.PlayingState
                            parent.visible = parent.visible ? false : true
                            playbackRequest()
                        }
                    }
                }
            }
        }
    }

    Button {
        id: btnPrev
        implicitWidth: 60 * viewRect.scalingRatio
        implicitHeight: 60 * viewRect.scalingRatio
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        icon.source: "icons/goPreviousSymbolic.svg"
        icon.width: Math.round(btnPrev.width * 0.5)
        icon.height: Math.round(btnPrev.height * 0.5)
        icon.color: "white"
        Layout.alignment : Qt.AlignHCenter

        visible: viewRect.index > 0 && !viewRect.hideMediaInfo
        enabled: deletePopUp === "closed"

        background: Rectangle {
            anchors.fill: parent
            color: "transparent"
        }

        onClicked: {
            if ((viewRect.index - 1) >= 0 ) {
                viewRect.videoAudio = true
                viewRect.index = viewRect.index - 1
            }
        }
    }

    Button {
        id: btnNext
        implicitWidth: 60 * viewRect.scalingRatio
        implicitHeight: 60 * viewRect.scalingRatio
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        icon.source: "icons/goNextSymbolic.svg"
        icon.width: Math.round(btnNext.width * 0.5)
        icon.height: Math.round(btnNext.height * 0.5)
        icon.color: "white"
        Layout.alignment : Qt.AlignHCenter

        visible: viewRect.index < (imgModel.count - 1) && !viewRect.hideMediaInfo
        enabled: deletePopUp === "closed"

        background: Rectangle {
            anchors.fill: parent
            color: "transparent"
        }

        onClicked: {
            if ((viewRect.index + 1) <= (imgModel.count - 1)) {
                viewRect.videoAudio = true
                viewRect.index = viewRect.index + 1
            }
        }
    }

    Item {
        id: mediaMenu

        anchors.bottom: parent.bottom
        width: parent.width
        height: 70 * viewRect.scalingRatio

        Rectangle {
            anchors.fill: parent
            color: "#2b292a"
        }

        Loader {
            id: mediaMenuLoader
            anchors.fill: parent
            sourceComponent: viewRect.mediaState === MediaPlayer.PlayingState ? videoPlayingMenuComponent : videoStoppedMenuComponent
        }

        Component {
            id: videoStoppedMenuComponent

            Item {
                id: videoStoppedMenuItem

                Button {
                    id: btnClose
                    icon.source: "icons/cameraVideoSymbolic.svg"
                    icon.width: parent.width * 0.13
                    icon.height: parent.height * 0.8
                    icon.color: "white"
                    enabled: deletePopUp === "closed" && viewRect.visible
                    anchors.left: parent.left
                    anchors.leftMargin: 20 * viewRect.scalingRatio
                    anchors.verticalCenter: parent.verticalCenter

                    visible: !viewRect.hideMediaInfo

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }

                    onClicked: {
                        viewRect.visible = false
                        playbackRequest();
                        viewRect.index = imgModel.count - 1
                        viewRect.closed();
                    }
                }

                Button {
                    id: btnDelete
                    anchors.right: parent.right
                    anchors.rightMargin: 20 * viewRect.scalingRatio
                    anchors.verticalCenter: parent.verticalCenter
                    icon.source: "icons/editDeleteSymbolic.svg"
                    icon.width: parent.width * 0.1
                    icon.height: parent.width * 0.1
                    icon.color: "white"
                    visible: viewRect.index >= 0 && !viewRect.hideMediaInfo
                    Layout.alignment: Qt.AlignHCenter

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }

                    onClicked: {
                        deletePopUp = "opened"
                        confirmationPopup.open()
                    }
                }

                Popup {
                    id: confirmationPopup
                    width: 200 * viewRect.scalingRatio
                    height: 80 * viewRect.scalingRatio

                    background: Rectangle {
                        border.color: "#444"
                        color: "#2b292a"
                        radius: 10 * viewRect.scalingRatio
                    }

                    closePolicy: Popup.NoAutoClose
                    x: (parent.width - width) / 2
                    y: (parent.height - height)

                    Column {
                        anchors.centerIn: parent
                        spacing: 10

                        Text {
                            text: viewRect.isVideoFile(viewRect.currentFileUrl) ? "  Delete Video?": "  Delete Photo?"
                            horizontalAlignment: parent.AlignHCenter

                            anchors.margins: 5 * viewRect.scalingRatio
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            color: "white"
                            font.bold: true
                            style: Text.Raised
                            styleColor: "black"
                            font.pixelSize: textSize
                        }

                        Row {
                            spacing: 20 * viewRect.scalingRatio

                            Button {
                                text: "Yes"
                                palette.buttonText: "white"
                                font.pixelSize: viewRect.textSize
                                width: 60 * viewRect.scalingRatio
                                height: confirmationPopup.height * 0.6
                                onClicked: {
                                    var tempCurrUrl = viewRect.currentFileUrl
                                    fileManager.deleteImage(tempCurrUrl)
                                    viewRect.index = imgModel.count
                                    deletePopUp = "closed"
                                    confirmationPopup.close()
                                }

                                background: Rectangle {
                                    anchors.fill: parent
                                    color: "#3d3d3d"
                                    radius: 10 * viewRect.scalingRatio
                                }
                            }

                            Button {
                                text: "No"
                                palette.buttonText: "white"
                                font.pixelSize: viewRect.textSize
                                width: 60 * viewRect.scalingRatio
                                height: confirmationPopup.height * 0.6
                                onClicked: {
                                    deletePopUp = "closed"
                                    confirmationPopup.close()
                                }

                                background: Rectangle {
                                    anchors.fill: parent
                                    color: "#3d3d3d"
                                    radius: 10 * viewRect.scalingRatio
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    id: mediaIndexView
                    anchors.centerIn: parent
                    width: parent.width * 0.2
                    height: parent.height
                    color: "transparent"
                    visible: viewRect.index >= 0 && !viewRect.hideMediaInfo
                    Text {
                        text: (viewRect.index + 1) + " / " + imgModel.count

                        anchors.fill: parent
                        anchors.margins: 5
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        color: "white"
                        font.bold: true
                        style: Text.Raised
                        styleColor: "black"
                        font.pixelSize: textSize
                    }
                }
            }
        }

        Component {
            id: videoPlayingMenuComponent

            Item {
                id: videoPlayingMenuItem
                anchors.fill: parent

                Button {
                    icon.source: "icons/cameraVideoSymbolic.svg"
                    icon.width: parent.width * 0.13
                    icon.height: parent.height * 0.8
                    icon.color: "white"
                    enabled: deletePopUp === "closed" && viewRect.visible
                    anchors.left: parent.left
                    anchors.leftMargin: 20 * viewRect.scalingRatio
                    anchors.verticalCenter: parent.verticalCenter

                    visible: !viewRect.hideMediaInfo

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }

                    onClicked: {
                        viewRect.visible = false
                        playbackRequest();
                        viewRect.index = imgModel.count - 1
                        viewRect.closed();
                    }
                }

                Button {
                    id: stopVideo
                    icon.source: "icons/pauseVideo.svg"
                    icon.width: parent.width * 0.13
                    icon.height: parent.height * 0.7
                    icon.color: "white"
                    enabled: viewRect.visible
                    anchors.centerIn: parent

                    visible: !viewRect.hideMediaInfo

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }

                    onClicked: {
                        playbackRequest();
                    }
                }

                Button {
                    id: muteSoundButton
                    icon.source: !viewRect.videoAudio ? "icons/audioOn.svg" : "icons/audioOff.svg"
                    icon.width: parent.width * 0.12
                    icon.height: parent.height * 0.7
                    icon.color: "white"
                    enabled: viewRect.visible
                    anchors.right: parent.right
                    anchors.rightMargin: 20 * viewRect.scalingRatio
                    anchors.verticalCenter: parent.verticalCenter

                    visible: !viewRect.hideMediaInfo

                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                    }

                    onClicked: {
                        viewRect.videoAudio = !viewRect.videoAudio
                    }
                }

            }
        }
    }

    Rectangle {
        id: mediaDate
        anchors.top: parent.top
        width: parent.width
        height: 60 * viewRect.scalingRatio
        color: "#2b292a"
        visible: viewRect.index >= 0 && !viewRect.hideMediaInfo

        Text {
            id: date
            text: {
                if (!viewRect.visible || viewRect.index === -1) {
                    return "None"
                } else {
                    if (viewRect.isVideoFile(viewRect.currentFileUrl)) {
                        return fileManager.getVideoDate(viewRect.currentFileUrl)
                    } else {
                        return fileManager.getPictureDate(viewRect.currentFileUrl)
                    }
                }
            }

            anchors.fill: parent
            anchors.margins: 5
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: "white"
            font.bold: true
            style: Text.Raised 
            styleColor: "black"
            font.pixelSize: viewRect.textSize
        }
    }

    QrCode {
        id: qrCodeComponent
        viewfinder: null
        openPopupFunction: openPopup
    }
}
