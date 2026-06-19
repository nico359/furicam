# furicam2_camera — Qt-free Camera2 engine library, reusable by any frontend
# (the Qt app, a GTK app, the CLI probe, Rust/Flutter via the C wrapper).
#
# It is the C++ engine (CameraSession + VideoEncoder + AudioEncoder +
# PreviewRenderer) plus a backend-neutral C API (furicam2_camera_c).  The
# Bionic->glibc Camera2 / Media NDK forwarding lives in its own library,
# camera2ndk-hybris (src/camera2/shim/), which this engine links and re-exports;
# on a Halium host that reaches Android through libhybris-common, on any other
# host it links a no-op stub and the camera is inert.
#
# The including project must enable both C and CXX languages.

find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL egl)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# The Bionic->glibc Camera2 / Media NDK shim: its own shared library + the
# standard <camera/Ndk*.h> / <media/Ndk*.h> headers.  Linked PUBLIC below so the
# camera2 include dir, the host-stub define and libhybris-common (or the host
# stub) reach everything that links furicam2_camera.
add_subdirectory(shim)

add_library(furicam2_camera STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DngWriter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/VideoEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AudioEncoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PreviewRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/furicam2_camera_c.cpp)

set_property(TARGET furicam2_camera PROPERTY POSITION_INDEPENDENT_CODE ON)

# Audio track (Milestone 6): GStreamer captures the mic and encodes AAC, which
# AudioEncoder muxes into the recording's MP4.  Needs gstreamer-app (GstAppSink).
# When unavailable (e.g. a dev box without the -base dev package), AudioEncoder
# compiles as a stub and recording is video-only.
pkg_check_modules(GST gstreamer-1.0)
pkg_check_modules(GSTAPP gstreamer-app-1.0)
if(GST_FOUND AND GSTAPP_FOUND)
    target_compile_definitions(furicam2_camera PRIVATE FURICAM2_HAVE_AUDIO=1)
    target_include_directories(furicam2_camera PRIVATE ${GST_INCLUDE_DIRS} ${GSTAPP_INCLUDE_DIRS})
    target_link_libraries(furicam2_camera PUBLIC ${GST_LIBRARIES} ${GSTAPP_LIBRARIES})
    target_link_directories(furicam2_camera PUBLIC ${GST_LIBRARY_DIRS} ${GSTAPP_LIBRARY_DIRS})
    message(STATUS "furicam2_camera: audio track ENABLED (GStreamer ${GST_VERSION} + app, AAC)")
else()
    message(STATUS "furicam2_camera: audio track DISABLED (gstreamer-app-1.0 not found) — video-only")
endif()

# Public: consumers get the engine headers here; the camera2 NDK headers, the
# host-stub define and libhybris come transitively from camera2ndk-hybris.
target_include_directories(furicam2_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(EGL_INCLUDE_DIRS)
    target_include_directories(furicam2_camera PUBLIC ${EGL_INCLUDE_DIRS})
endif()

target_link_libraries(furicam2_camera PUBLIC camera2ndk-hybris Threads::Threads)
if(EGL_LIBRARIES)
    target_link_libraries(furicam2_camera PUBLIC ${EGL_LIBRARIES})
endif()
