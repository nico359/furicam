#!/usr/bin/env bash
# Build + install furicam2 (Camera2 backend) ON THE PHONE.
#
# The FuriPhone is the only aarch64 + libhybris environment available, so the
# app is built directly on-device (no chroot, no .deb — just `make install`).
# Two Debian-packaging quirks are worked around without disturbing the phone's
# GLES Qt or apt state:
#
#   1. qtmultimedia5-dev pulls the non-GLES qtbase5-dev (conflicts).  But the
#      runtime libQt5Multimedia.so.5 is GL-backend-agnostic, so we *extract* the
#      dev headers into a local sysroot and point cmake at them — never install.
#   2. The full OpenCV dev meta (libopencv-dev) can't install (its other module
#      -dev packages aren't available), so find_package(OpenCV) fails the
#      all-modules validation.  We only use core/imgproc/photo (which ARE
#      installed), so we generate a minimal OpenCVConfig.cmake for just those.
#
# Run on the phone:  ssh flx-usb 'bash ~/projects/furicam2-src/build-on-phone.sh'
set -e

SRC="$HOME/projects/furicam2-src"
cd "$SRC"

echo "=== installing non-conflicting build-deps ==="
sudo apt-get install -y \
  qtbase5-gles-dev qtdeclarative5-dev qttools5-dev-tools zlib1g-dev \
  libopencv-core-dev libopencv-imgproc-dev libopencv-photo-dev libtbb-dev \
  libexiv2-dev libzxing-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libhybris-common-dev libegl-dev

# ── 1. QtMultimedia dev headers → local sysroot (extract, don't install) ──────
echo
echo "=== staging qtmultimedia5-dev headers (sysroot) ==="
QTMM="$SRC/qtmm-sysroot"
rm -rf "$QTMM"; mkdir -p "$QTMM"
( cd /tmp && rm -f qtmultimedia5-dev_*.deb && apt-get download qtmultimedia5-dev \
  && dpkg-deb -x qtmultimedia5-dev_*.deb "$QTMM" )
# The -dev .deb ships only the .so symlink, not the versioned runtime libs the
# cmake imported targets check for.  Symlink the real installed runtime libs
# (at /usr/lib) into the sysroot so the Qt5Multimedia*Config checks resolve.
QTMM_LIB="$QTMM/usr/lib/aarch64-linux-gnu"
mkdir -p "$QTMM_LIB"
for so in /usr/lib/aarch64-linux-gnu/libQt5Multimedia*.so.*; do
  [ -e "$so" ] && ln -sf "$so" "$QTMM_LIB/$(basename "$so")"
done
QTMM_CMAKE="$(find "$QTMM" -type d -name 'Qt5Multimedia' | head -1)"
QTMM_CMAKE_DIR="$(dirname "$QTMM_CMAKE")"
echo "Qt5Multimedia cmake: $QTMM_CMAKE"

# ── 2. Minimal OpenCVConfig.cmake for the 3 installed modules ─────────────────
echo
echo "=== generating minimal OpenCVConfig.cmake (core/imgproc/photo only) ==="
OCV="$SRC/opencv-mini"
rm -rf "$OCV"; mkdir -p "$OCV"
cat > "$OCV/OpenCVConfig.cmake" <<'CMAKE'
# Minimal OpenCV config: only the modules furicam2 uses and that are installed.
set(OpenCV_FOUND TRUE)
set(OpenCV_VERSION 4.10.0)
set(OpenCV_INCLUDE_DIRS "/usr/include/opencv4")
set(_ocv_libdir "/usr/lib/aarch64-linux-gnu")
foreach(_m core imgproc photo)
  if(NOT TARGET opencv_${_m})
    add_library(opencv_${_m} SHARED IMPORTED)
    set_target_properties(opencv_${_m} PROPERTIES
      IMPORTED_LOCATION "${_ocv_libdir}/libopencv_${_m}.so"
      INTERFACE_INCLUDE_DIRECTORIES "${OpenCV_INCLUDE_DIRS}")
  endif()
  set(OpenCV_${_m}_FOUND TRUE)
endforeach()
set(OpenCV_LIBS opencv_core opencv_imgproc opencv_photo)
CMAKE

# ── configure + build + install ───────────────────────────────────────────────
echo
echo "=== configuring (Camera2 ON) ==="
rm -rf build; mkdir build; cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DFURICAM2_CAMERA2=ON \
  -DQt5Multimedia_DIR="$QTMM_CMAKE_DIR/Qt5Multimedia" \
  -DQt5MultimediaWidgets_DIR="$QTMM_CMAKE_DIR/Qt5MultimediaWidgets" \
  -DOpenCV_DIR="$OCV" \
  -DCMAKE_INCLUDE_PATH="$QTMM/usr/include/aarch64-linux-gnu/qt5;$QTMM/usr/include;/usr/include/opencv4"

echo
echo "=== building (slow step) ==="
make -j"$(nproc)"

echo
echo "=== installing to system ==="
sudo make install
# furicam2 now links the separate libcamera2ndk-hybris.so; refresh the linker
# cache so the app finds it at /usr/lib/<triplet> at runtime.
sudo ldconfig
sudo update-desktop-database /usr/share/applications/ 2>/dev/null || true

echo
echo "=== done ==="
echo "binary:   $(command -v furicam2 || echo /usr/bin/furicam2)"
echo "launcher: 'FuriCam2' should appear next to 'FuriCam'"
