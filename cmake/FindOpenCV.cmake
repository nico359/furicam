# ponytail: minimal OpenCV finder — full libopencv-dev pulls Qt5, we only need core/imgproc/photo.
# Replace with upstream OpenCVConfig.cmake if the distro ships Qt6-compatible opencv packages.

if(NOT OpenCV_FIND_COMPONENTS)
    set(OpenCV_FIND_COMPONENTS core imgproc photo)
endif()

set(OpenCV_INCLUDE_DIR /usr/include/opencv4)
set(OpenCV_LIB_DIR /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE})

set(OpenCV_INCLUDE_DIRS ${OpenCV_INCLUDE_DIR})

foreach(comp ${OpenCV_FIND_COMPONENTS})
    find_library(OpenCV_${comp}_LIBRARY
        NAMES opencv_${comp}
        PATHS ${OpenCV_LIB_DIR}
        NO_DEFAULT_PATH)
    if(NOT OpenCV_${comp}_LIBRARY)
        message(FATAL_ERROR "OpenCV component ${comp} not found in ${OpenCV_LIB_DIR}")
    endif()
    list(APPEND OpenCV_LIBS ${OpenCV_${comp}_LIBRARY})
endforeach()

# Resolve transitive link deps via pkg-config (if available) or hardcode the known ones.
find_package(ZLIB REQUIRED)
list(APPEND OpenCV_LIBS ZLIB::ZLIB)

set(OpenCV_FOUND TRUE)
message(STATUS "Found OpenCV (ponytail): ${OpenCV_LIBS}")
