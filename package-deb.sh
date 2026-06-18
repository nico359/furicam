#!/usr/bin/env bash
# Assemble a furicam2 .deb from the on-phone build.
#
# Standard dpkg-buildpackage can't be used: qtmultimedia5-dev pulls the non-GLES
# qtbase5-dev, which conflicts with the phone's qtbase5-gles-dev.  build-on-phone.sh
# works around this with a header sysroot, so we reuse that build here and package
# the resulting binary with dpkg-deb (library deps via dpkg-shlibdeps).
#
# Prereq: build-on-phone.sh has been run at least once (build/ exists).
# Run on the phone:  ssh flx 'bash ~/projects/furicam2-src/package-deb.sh'
set -e
SRC="$HOME/projects/furicam2-src"
cd "$SRC"

echo "=== refresh binary ==="
( cd build && make -j"$(nproc)" furicam2 )

VERSION="$(dpkg-parsechangelog -SVersion)"
ARCH="$(dpkg --print-architecture)"
STAGE="$SRC/deb-stage"
rm -rf "$STAGE"
install -d "$STAGE/usr/bin" "$STAGE/usr/share/applications" "$STAGE/DEBIAN"

install -m755 build/furicam2 "$STAGE/usr/bin/furicam2"
strip --strip-unneeded "$STAGE/usr/bin/furicam2" 2>/dev/null || true
install -m644 furicam2.desktop "$STAGE/usr/share/applications/furicam2.desktop"

echo "=== computing library dependencies (dpkg-shlibdeps) ==="
SHLIBS="$(dpkg-shlibdeps -O "$STAGE/usr/bin/furicam2" 2>/dev/null | sed -n 's/^shlibs:Depends=//p' || true)"
# Not auto-detectable: QML modules (loaded at runtime), the shared furicam data
# (icon/schema/polkit/config), and the GStreamer plugins used for AAC audio.
MANUAL="furicam, qml-module-qtmultimedia, qml-module-qtquick2, qml-module-qtquick-controls2, qml-module-qtquick-window2, qml-module-qt-labs-platform, qml-module-qt-labs-folderlistmodel, qml-module-qt-labs-settings, qml-module-qtquick-layouts, qml-module-qtgraphicaleffects, qml-module-qtquick-shapes, qml-module-qtsensors, libqt5svg5, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-libav"
DEPENDS="${SHLIBS:+$SHLIBS, }$MANUAL"
SIZE="$(du -sk "$STAGE/usr" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: furicam2
Version: $VERSION
Architecture: $ARCH
Maintainer: Sean Pollard <spollard08@gmail.com>
Section: graphics
Priority: optional
Installed-Size: $SIZE
Depends: $DEPENDS
Description: FuriCam2 - Camera2/libhybris camera app for the FuriPhone FLX1s
 A camera app for FuriOS that drives the camera through the Android Camera2
 native development kit (NDK) via libhybris, replacing the QtMultimedia/gst-droid
 backend.  Photo, 4K/HD H.264 video, WYSIWYG aspect ratios, flash (off/on/auto),
 white balance, live QR scanning, macro/secondary cameras, HDR and geotagging.
 .
 Coexists with the original furicam, reusing its icon, GSettings schema, polkit
 rule and config.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    update-desktop-database /usr/share/applications 2>/dev/null || true
fi
exit 0
POST
chmod 755 "$STAGE/DEBIAN/postinst"

OUT="$SRC/furicam2_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT"
echo
echo "=== built: $OUT ==="
dpkg-deb -I "$OUT"
echo "--- contents ---"
dpkg-deb -c "$OUT"
