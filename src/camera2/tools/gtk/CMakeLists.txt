# Standalone build for the GTK4 live-preview prototype.
#
#   cd src/camera2/tools/gtk && mkdir -p b && cd b && cmake .. && make
#
# Needs only GTK4 + the shared furicam_camera engine library (no Qt) — all
# present on the phone.
cmake_minimum_required(VERSION 3.16)
project(camera2_gtk LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# Build the shared, toolkit-agnostic engine library (src/camera2/CMakeLists.txt).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. furicam_camera_build)

add_executable(camera2_gtk ${CMAKE_CURRENT_SOURCE_DIR}/camera2_gtk.c)
# The engine is C++ (static lib) so link with the C++ driver to pull libstdc++.
set_target_properties(camera2_gtk PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(camera2_gtk PRIVATE furicam_camera PkgConfig::GTK4)
