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
- Video output is now of reasonable file size (MJPEG --> H.264 but software encoding...)
- Video output has adjustable bitrate to further adjust quality or storage savings
- Added different video resolutions to choose (although 4k doesnt seem possible due to software encoding)
- Added post processing options (RGB channels and saturation) - with slightly increased green as default because on the FLX1s all pictures seem to have a red tint
- Switching between photo and video mode also switches aspect ratio now
- HDR Photography (uses Mertens fusion with OpenCV) - suboptimal because the camera doesnt expose manual exposure correction and therefore this is just software correction and the gained detail is very minimal

# To be improved

- Glitching when refocusing the window or switching cameras
- Rotation bug when filming vertical video mostly
- Manual exposure (not possible due to the camera not even exposing these settings)
- Manual focus (also doesnt seem possible for the same reason)
- The way the video is currently encoded is suboptimal because it uses software encoding (has once again something to do with how the app receives data from Android Abstraction Layer)

## Building

Important note about building:
Some of the listed dependencies here do not seem to be available anymore on Debian Forky. To build furios-camera/furicam directly on the Furiphone I had to to so in a Debian Trixie Distrobox container. If you want to take the same approach, you might have to install distrobox, podman and optionally a GUI to manage containers like Distroshelf from Flathub for example.


* (Optional) Install Distrobox
```
sudo apt install podman distrobox
```

* (Optional) Install GUI to manage and install a Debian Trixie Distrobox
```
sudo flatpak install com.ranfdev.DistroShelf
```

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
