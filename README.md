# FuriCam
<img src="furicam.svg" width="100px">

An enhanced camera app for FuriPhone, built with Qt5/QML/C++.

Based on [furios-camera](https://github.com/nicothegamer/furios-camera) by FuriLabs.

Licensed under GPL-2.0.

## Building

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
