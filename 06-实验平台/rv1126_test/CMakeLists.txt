cmake_minimum_required(VERSION 3.10)
project(rv1126_yolov8_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(JPEG REQUIRED)

# 优先 find_package;板端 OpenCV 无 OpenCVConfig.cmake 时回退到系统路径手动配置
find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs)
if(OpenCV_FOUND)
    message(STATUS "OpenCV found via CMake config: ${OpenCV_VERSION}")
else()
    message(STATUS "OpenCV config not found, fallback to /usr/include/opencv4")
    find_path(OPENCV_INCLUDE_DIR opencv2/opencv.hpp
        HINTS /usr/include/opencv4 /usr/local/include/opencv4)
    if(NOT OPENCV_INCLUDE_DIR)
        message(FATAL_ERROR "OpenCV headers not found")
    endif()
    set(OpenCV_INCLUDE_DIRS ${OPENCV_INCLUDE_DIR})
    set(OpenCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs)
endif()

file(GLOB LIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lib/*.cpp)

# 红外闭源 SDK(aarch64):libiruvc 依赖自带 libusb-1.0.so.0
set(IR_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/arm64)
link_directories(${IR_SDK_DIR})

add_executable(rv1126_yolov8_demo
    main.cpp
    ${LIB_SOURCES}
)

target_include_directories(rv1126_yolov8_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIR}
)

target_link_libraries(rv1126_yolov8_demo PRIVATE
    ${OpenCV_LIBS}
    ${JPEG_LIBRARIES}
    Threads::Threads
    rknnrt
    iruvc
    irparse
    irprocess
    m
)

# 运行时按二进制相对路径找闭源库: build/rv1126_yolov8_demo -> ../libs/arm64
set_target_properties(rv1126_yolov8_demo PROPERTIES
    BUILD_RPATH "$ORIGIN/../libs/arm64"
    INSTALL_RPATH "$ORIGIN/../libs/arm64"
)
