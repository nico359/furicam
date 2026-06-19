cmake_minimum_required(VERSION 3.16)
project (furicam LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Core DBus Widgets Quick Qml Multimedia)
find_package(exiv2 REQUIRED)

find_package(OpenCV REQUIRED COMPONENTS core imgproc photo)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0)

# Path C — Android Camera2 native development kit (NDK) helper via libhybris.
# ON by default: the camera path now drives the Camera2 engine (Camera2Bridge).
# On a non-Halium host (desktop) it links a stub and Camera2 is inert, so the
# build still works there.  Turn OFF only for the legacy GStreamer-only build.
option(FURICAM2_CAMERA2 "Build the Camera2 NDK camera engine (Camera2Bridge)" ON)

if(FURICAM2_CAMERA2)
    # The reusable, Qt-free camera engine is its own static library so the Qt app,
    # the CLI probe, and standalone (GTK/Rust/…) frontends all share one engine.
    # It pulls in libhybris (or a host stub), EGL, the vendored hybris headers and
    # the host-stub define, and exposes them transitively to whatever links it.
    add_subdirectory(src/camera2)
endif()

execute_process(COMMAND pkg-config --cflags gstreamer-1.0 OUTPUT_VARIABLE GST_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND pkg-config --libs gstreamer-1.0 OUTPUT_VARIABLE GST_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)

execute_process(COMMAND pkg-config --variable=libdir glib-2.0 OUTPUT_VARIABLE GLIB_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

include_directories(/usr/include/ZXing)

set(APP_SOURCES
		${CMAKE_SOURCE_DIR}/src/main.cpp
		${CMAKE_SOURCE_DIR}/src/thumbnailgenerator.cpp
		${CMAKE_SOURCE_DIR}/src/flashlightcontroller.cpp
		${CMAKE_SOURCE_DIR}/src/filemanager.cpp
		${CMAKE_SOURCE_DIR}/src/exif.cpp
		${CMAKE_SOURCE_DIR}/src/qrcodehandler.cpp
		${CMAKE_SOURCE_DIR}/src/geocluefind.cpp
		${CMAKE_SOURCE_DIR}/src/singleinstance.cpp
		${CMAKE_SOURCE_DIR}/src/windoweventfilter.cpp
		${CMAKE_SOURCE_DIR}/src/appcontroller.cpp
		${CMAKE_SOURCE_DIR}/src/settingsmanager.cpp
		${CMAKE_SOURCE_DIR}/src/accelreader.cpp
		${CMAKE_SOURCE_DIR}/src/hdrprocessor.cpp
		${CMAKE_SOURCE_DIR}/src/whitebalancecontroller.cpp
		${CMAKE_SOURCE_DIR}/src/meteringcontroller.cpp)

if(FURICAM2_CAMERA2 AND EXISTS ${CMAKE_SOURCE_DIR}/src/camera2/Camera2Bridge.cpp)
    # Camera2Bridge is the Qt/QML adapter; the rest of the engine lives in the
    # furicam2_camera library this target links below.
    list(APPEND APP_SOURCES ${CMAKE_SOURCE_DIR}/src/camera2/Camera2Bridge.cpp)
endif()

set(APP_HEADERS
		${CMAKE_SOURCE_DIR}/src/filemanager.h
		${CMAKE_SOURCE_DIR}/src/flashlightcontroller.h
		${CMAKE_SOURCE_DIR}/src/thumbnailgenerator.h
		${CMAKE_SOURCE_DIR}/src/zxingreader.h
		${CMAKE_SOURCE_DIR}/src/exif.h
		${CMAKE_SOURCE_DIR}/src/qrcodehandler.h
		${CMAKE_SOURCE_DIR}/src/geocluefind.h
		${CMAKE_SOURCE_DIR}/src/singleinstance.h
		${CMAKE_SOURCE_DIR}/src/windoweventfilter.h
		${CMAKE_SOURCE_DIR}/src/appcontroller.h
		${CMAKE_SOURCE_DIR}/src/settingsmanager.h
		${CMAKE_SOURCE_DIR}/src/accelreader.h
		${CMAKE_SOURCE_DIR}/src/hdrprocessor.h
		${CMAKE_SOURCE_DIR}/src/whitebalancecontroller.h
		${CMAKE_SOURCE_DIR}/src/meteringcontroller.h)

if(FURICAM2_CAMERA2
   AND EXISTS ${CMAKE_SOURCE_DIR}/src/camera2/Camera2Bridge.h
   AND EXISTS ${CMAKE_SOURCE_DIR}/src/camera2/Camera2Bridge.cpp)
    # Camera2Bridge.h has Q_OBJECT — list it so AUTOMOC processes it (only once
    # its .cpp exists, else the moc unit would reference undefined methods).
    list(APPEND APP_HEADERS ${CMAKE_SOURCE_DIR}/src/camera2/Camera2Bridge.h)
endif()

qt5_add_resources(APP_RESOURCES
	${CMAKE_SOURCE_DIR}/sounds/sounds.qrc
	${CMAKE_SOURCE_DIR}/icons/icons.qrc
	${CMAKE_SOURCE_DIR}/src/qml/qml.qrc)

add_executable(${PROJECT_NAME} ${APP_SOURCES} ${APP_HEADERS} ${APP_RESOURCES})

target_include_directories(${PROJECT_NAME} PUBLIC
    /usr/include/gstreamer-1.0
    /usr/include/glib-2.0
    ${GLIB_LIBDIR}/glib-2.0/include
    ${GLIB_INCLUDE_DIRS}
)

target_compile_options(${PROJECT_NAME} PUBLIC ${GST_CFLAGS})
target_link_libraries(${PROJECT_NAME} PUBLIC Qt5::Core Qt5::Widgets Qt5::Quick Qt5::Qml Qt5::Multimedia Qt5::DBus ZXing exiv2 dl ${GST_LIBS} ${GLIB_LIBRARIES} ${OpenCV_LIBS})

if(FURICAM2_CAMERA2)
    target_compile_definitions(${PROJECT_NAME} PUBLIC FURICAM2_CAMERA2=1)
    # The engine library brings the camera2 include dirs, the vendored hybris
    # header, the host-stub define, libhybris/EGL and pthreads transitively.
    target_link_libraries(${PROJECT_NAME} PUBLIC furicam2_camera)
endif()

# ── Standalone Camera2 capability probe (Milestone 2) ────────────────────────
# A tiny Qt-free CLI that enumerates/opens/dumps cameras through the NDK shim,
# so we can prove Camera2 reachability on the phone without the whole GUI app.
# Built only when the Camera2 helper is enabled and the sources exist.
if(FURICAM2_CAMERA2 AND EXISTS ${CMAKE_SOURCE_DIR}/src/camera2/tools/camera2_probe.cpp)
    add_executable(camera2_probe ${CMAKE_SOURCE_DIR}/src/camera2/tools/camera2_probe.cpp)
    target_link_libraries(camera2_probe PRIVATE furicam2_camera)
endif()

install(TARGETS ${PROJECT_NAME} DESTINATION /usr/bin)
install(FILES ${CMAKE_SOURCE_DIR}/furicam.desktop DESTINATION /usr/share/applications)
# Share icon, system /etc config, polkit pkla, and GSettings schema with the
# original furicam install — they are read-only and version-agnostic.
