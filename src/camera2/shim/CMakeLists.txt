# camera2ndk-hybris — Android Camera2 / Media NDK over libhybris for glibc Linux.
#
# Builds libcamera2ndk-hybris.so: a thin forwarding shim that turns the device's
# Bionic-linked libcamera2ndk.so / libmediandk.so / libnativewindow.so into
# symbols a glibc-linked Linux process can call directly.  Consumers link
# -lcamera2ndk-hybris -lhybris-common, #include <camera/NdkCameraManager.h>, and
# use the Camera2 NDK exactly as Android documents it (plus the one-time init in
# <camera2ndk_hybris.h>).
#
# Builds two ways:
#   * standalone   — its own project; `cmake . && make && make install`
#   * add_subdirectory() — consumed in-tree (e.g. by furicam2); exposes the
#     public include dir, the host-stub define and libhybris-common transitively.
#
# Target platform is a Halium device (aarch64 + libhybris).  On any other host
# it still builds against a no-op stub (HostDlStub.c) so it compiles in CI and on
# a dev desktop, with the camera path simply inert.

cmake_minimum_required(VERSION 3.16)
project(camera2ndk-hybris VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

# Always a shared object: libcamera2ndk-hybris.so is the shippable artifact that
# other apps link with -lcamera2ndk-hybris, regardless of BUILD_SHARED_LIBS.
add_library(camera2ndk-hybris SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/Camera2NDKShim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Camera2NDKHybrisInit.c)

set_target_properties(camera2ndk-hybris PROPERTIES
    OUTPUT_NAME camera2ndk-hybris
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    C_VISIBILITY_PRESET default
    POSITION_INDEPENDENT_CODE ON)

# Public: consumers get <camera/Ndk*.h>, <media/Ndk*.h>, <android/*.h>, the
# umbrella <Camera2NDK.h> and <camera2ndk_hybris.h>.
target_include_directories(camera2ndk-hybris PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/camera2ndk-hybris>)

# On a Halium device libhybris-common provides the android_dlopen/dlsym linker
# the shim forwards through.  Off-device we compile a stub and flag the build as
# inert (FURICAM2_CAMERA2_HOST_STUB kept for back-compat; CAMERA2NDK_HYBRIS_HOST_STUB
# is the package-native name) — both are public so consumers can branch on them.
find_library(HYBRIS_COMMON_LIB hybris-common)
if(HYBRIS_COMMON_LIB)
    target_link_libraries(camera2ndk-hybris PUBLIC ${HYBRIS_COMMON_LIB})
    message(STATUS "camera2ndk-hybris: Halium host (${HYBRIS_COMMON_LIB})")
else()
    target_sources(camera2ndk-hybris PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/HostDlStub.c)
    target_include_directories(camera2ndk-hybris PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/host>)
    target_compile_definitions(camera2ndk-hybris PUBLIC
        CAMERA2NDK_HYBRIS_HOST_STUB=1 FURICAM2_CAMERA2_HOST_STUB=1)
    message(STATUS "camera2ndk-hybris: non-Halium host — shim inert (host stub, vendored binding.h)")
endif()

# ── Install (library always; dev files only for a standalone build) ──────────
install(TARGETS camera2ndk-hybris
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/camera2ndk-hybris
        FILES_MATCHING PATTERN "*.h")

    set(PKGCONFIG_LIBDIR     "${CMAKE_INSTALL_FULL_LIBDIR}")
    set(PKGCONFIG_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/camera2ndk-hybris.pc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/camera2ndk-hybris.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/camera2ndk-hybris.pc
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
