# Standalone build for the Qt Quick on-screen preview harness.
#
# Kept separate from the top-level CMakeLists (which requires Qt5Multimedia, in
# conflict with the phone's GLES Qt).  This harness needs only Qt Quick/Qml/Gui
# + the shared furicam_camera engine library, all buildable on the phone:
#
#   cd src/camera2/tools/preview && mkdir -p b && cd b && cmake .. && make
#
cmake_minimum_required(VERSION 3.16)
project(camera2_preview LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Qml Quick DBus)

# Build the shared, Qt-free engine library (src/camera2/CMakeLists.txt).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. furicam_camera_build)

add_executable(camera2_preview
    ${CMAKE_CURRENT_SOURCE_DIR}/../camera2_preview.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Camera2Bridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Camera2Bridge.h)

# furicam_camera carries the camera2 includes, EGL, libhybris and pthreads.
target_link_libraries(camera2_preview PRIVATE
    Qt5::Core Qt5::Gui Qt5::Qml Qt5::Quick Qt5::DBus furicam_camera)
