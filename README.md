# FuriCam
<img src="furicam.svg" width="100px">

An enhanced camera app for FuriPhone, built with Qt5/QML/C++.

Based on [furios-camera](https://github.com/FuriLabs/furios-camera) by FuriLabs.

I decided not to do an obvious fork and rename it since these changes will most likely never be upstreamed anyway. Not that I dont want that but since Im not a developer all I do is AI generated and I assume they wouldnt want that which is understandable.

Licensed under GPL-2.0.

# Enhancements so far

- HDR Photography (uses Mertens fusion with OpenCV)
- Added option to adjust resolution (megapixel)
- Added option to increase jpeg compression
- Added 3x3 grid
- Added level indicator
- Added zoom slider
- Added Pro Mode (allows for manual ISO and Shutter speed adjustment)
- Added RAW (DNG) output
- Video output is now of reasonable file size (MJPEG --> H.264)
- Video output has adjustable bitrate to further adjust quality or storage savings
- Added different video resolutions to choose
- Added post processing options (RGB channels and saturation) - red was slightly reduced by default due to the FLX1s but does not seem to be an issue anymore
- Switching between photo and video mode also switches aspect ratio now

# To be improved

- Manual focus
- Tap to focus seems to ignore where the tap was
- Maybe different HDR approach as OpenCV is very memory intensive but results are pretty solid overall
- DRO (maybe, spollards fork already includes it but it made everything very washed out so i left it out)
- Feel free to request something if there is anything missing

## Building

Important note about building:
Some of the listed dependencies here do not seem to be available anymore on Debian Forky. To build furios-camera/furicam directly on the Furiphone I had to to so in a Debian Trixie Distrobox container. If you want to take the same approach, you might have to install distrobox, podman and optionally a GUI to manage containers like Distroshelf from Flathub for example.


* Install Distrobox
```
sudo apt install podman distrobox
```

* (Optional) Install GUI to manage and install a Debian Trixie Distrobox
```
sudo flatpak install com.ranfdev.DistroShelf
```

* Install build dependencies (inside distrobox)
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


> [!IMPORTANT]  
> Camera2 build notes:
> Building in distrobox requires a symlink to host libhybris:
>  ```
>  sudo ln -s /run/host/usr/lib/aarch64-linux-gnu/libhybris-common.so.1 \
>              /usr/lib/aarch64-linux-gnu/libhybris-common.so.1
>  ```
>  
> The deb is built with `dpkg-buildpackage -d -us -uc -b` (skip build-dep checks — hybris is host-only).



* Build (inside distrobox)
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
