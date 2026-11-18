cmake_minimum_required(VERSION 3.16)
project(detector_test CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(ONNXRUNTIME_DIR "" CACHE PATH
    "Directory containing onnxruntime include/ and lib/")
if(NOT ONNXRUNTIME_DIR)
    set(ONNXRUNTIME_DIR "/tmp/onnxruntime-linux-aarch64-1.21.0")
endif()
if(NOT IS_DIRECTORY "${ONNXRUNTIME_DIR}")
    message(FATAL_ERROR
        "ONNX Runtime directory does not exist: ${ONNXRUNTIME_DIR}")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATHS "${ONNXRUNTIME_DIR}/include"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    PATHS "${ONNXRUNTIME_DIR}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR "ONNX Runtime not found under ${ONNXRUNTIME_DIR}")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

set(DETECTOR_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/detector_onnx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/detector_yolov8.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/yolo_postprocess.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_executable(detector_test ${DETECTOR_TEST_SOURCES})

target_include_directories(detector_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${ONNXRUNTIME_INCLUDE_DIR}"
    ${OpenCV_INCLUDE_DIRS}
)

target_compile_options(detector_test PRIVATE
    -Wall -Wextra -Wpedantic
)
# ONNX Runtime 头文件 (float16 constexpr) 触发 -Wpedantic 告警, 单独关闭
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/detector_onnx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/detector_yolov8.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-pedantic"
)

target_link_libraries(detector_test PRIVATE
    "${ONNXRUNTIME_LIB}"
    ${OpenCV_LIBS}
)

set_target_properties(detector_test PROPERTIES
    BUILD_RPATH "${ONNXRUNTIME_DIR}/lib"
)