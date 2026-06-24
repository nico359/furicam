# furicam_camera — Qt-free Camera2 engine library, reusable by any frontend
# (the Qt app, a GTK app, the CLI probe, Rust/Flutter via the C wrapper).
#
# It is the C++ engine (CameraSession + VideoEncoder + PreviewRenderer) plus the
# C libhybris shim and a backend-neutral C API (camera_c).  On a Halium
# host it reaches Android Camera2/MediaNDK through libhybris-common; on a normal
# host it links a no-op stub and reports the camera unavailable.
#
# The including project must enable both C and CXX languages.

find_package(PkgConfig REQUIRED)
# On the FuriPhone under distrobox, host libs are available at /run/host.
# We link libhybris-common directly (not a stub) so Camera2 works at runtime.
# --allow-shlib-undefined papers over container/host ABI mismatches (e.g. libpulse).
find_library(HYBRIS_COMMON_LIB
    NAMES libhybris-common.so.1 libhybris-common.so
    HINTS /run/host/usr/lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
pkg_check_modules(EGL egl)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(furicam_camera STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DngWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VideoEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AudioEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PreviewRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/camera_c.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Camera2NDKShim.c)

set_property(TARGET furicam_camera PROPERTY POSITION_INDEPENDENT_CODE ON)

# Audio track (Milestone 6): GStreamer captures the mic and encodes AAC, which
# AudioEncoder muxes into the recording's MP4.  Needs gstreamer-app (GstAppSink).
# When unavailable (e.g. a dev box without the -base dev package), AudioEncoder
# compiles as a stub and recording is video-only.
pkg_check_modules(GST gstreamer-1.0)
pkg_check_modules(GSTAPP gstreamer-app-1.0)
if(GST_FOUND AND GSTAPP_FOUND)
    target_compile_definitions(furicam_camera PRIVATE FURICAM_HAVE_AUDIO=1)
    target_include_directories(furicam_camera PRIVATE ${GST_INCLUDE_DIRS} ${GSTAPP_INCLUDE_DIRS})
    target_link_libraries(furicam_camera PUBLIC ${GST_LIBRARIES} ${GSTAPP_LIBRARIES})
    target_link_directories(furicam_camera PUBLIC ${GST_LIBRARY_DIRS} ${GSTAPP_LIBRARY_DIRS})
    message(STATUS "furicam_camera: audio track ENABLED (GStreamer ${GST_VERSION} + app, AAC)")
else()
    message(STATUS "furicam_camera: audio track DISABLED (gstreamer-app-1.0 not found) — video-only")
endif()

# Public: consumers get the engine headers, the vendored <hybris/common/binding.h>,
# the host-stub define, libhybris/EGL and pthreads, all transitively.
target_include_directories(furicam_camera PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/host)
if(EGL_INCLUDE_DIRS)
    target_include_directories(furicam_camera PUBLIC ${EGL_INCLUDE_DIRS})
endif()

target_link_libraries(furicam_camera PUBLIC Threads::Threads)
if(EGL_LIBRARIES)
    target_link_libraries(furicam_camera PUBLIC ${EGL_LIBRARIES})
endif()

if(HYBRIS_COMMON_LIB)
    target_link_libraries(furicam_camera PUBLIC ${HYBRIS_COMMON_LIB})
    message(STATUS "furicam_camera: Halium host (${HYBRIS_COMMON_LIB})")
else()
    target_sources(furicam_camera PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/HostDlStub.c)
    target_compile_definitions(furicam_camera PUBLIC FURICAM_CAMERA2_HOST_STUB=1)
    message(STATUS "furicam_camera: non-Halium host — engine inert (host stub)")
endif()
