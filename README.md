# FuriCam
<img src="furicam.svg" width="100px">

An enhanced camera app for FuriPhone, built with Qt5/QML/C++.

Based on [furios-camera](https://github.com/FuriLabs/furios-camera) by FuriLabs.

I decided not to do an obvious fork and rename it since these changes will most likely never be upstreamed anyway. Not that I dont want that but since Im not a developer all I do is AI generated and I assume they wouldnt want that which is understandable.

Licensed under GPL-2.0.

# Enhancements so far

- Added option to adjust resolution (megapixel)
- Added option to increase jpeg compression
- Added 3x3 grid
- Added level indicator
- Added zoom slider
- Video output is now of reasonable file size (MJPEG --> H.264)
- Video output has adjustable bitrate to further adjust quality or storage savings

# To be improved

- Glitching when refocusing the window or switching cameras
- Claude and I had some communication issues about how the resuming of the camera is handled so maybe there are some unnecessary changes
- Color temperature still seems a little bit off depending on the scenario
- Maybe denoising or post processing in general
- HDR?
- The way the video is currently encoded is suboptimal because it uses software encoding (has once again something to do with how the app receives data from Android Abstraction Layer)
- Aspect ratio behaves weirdly sometimes when switching between photo and video

## Building

Note about building: Some of the listed dependencies here do not seem to be available anymore on Debian Forky. To build furios-camera/furicam directly on the Furiphone I had to to so in a Debian Trixie Distrobox container. If you want to take the same approach, you might have to install distrobox, podman and optionally a GUI to manage containers like Distroshelf from Flathub for example.

* Install build dependencies
```
sudo apt install cmake \
                 qtbase5-dev \
                 qtdeclarative5-dev \
                 libqt5multimedia5-plugins \
                 qttools5-dev-tools \
                 libz-dev \
                 qtmultimedia5-dev \
                 libgstreamer1.0-dev \
                 pkgconf \
                 libzxing-dev \
                 libexiv2-dev
```
* Build
```
mkdir build
cd build
cmake ..
make
```
* Install runtime dependencies
```
sudo apt install qml-module-qtmultimedia \
                 libqt5multimedia5-plugins \
                 qml-module-qtquick2 \
                 qml-module-qtquick-controls2 \
                 qml-module-qtquick-window2 \
                 qt5-cameraplugin-aal \
                 qml-module-qt-labs-platform \
                 qml-module-qt-labs-folderlistmodel \
                 qml-module-qt-labs-settings \
                 qml-module-qtquick-layouts \
                 qml-module-qtgraphicaleffects \
                 qml-module-qtquick-shapes \
                 mkvtoolnix \
                 libqt5svg5 \
                 libgstreamer1.0-0 \
                 gstreamer1.0-droid \
                 gstreamer1.0-plugins-good \
                 gstreamer1.0-plugins-base \
                 libzxing3
```
