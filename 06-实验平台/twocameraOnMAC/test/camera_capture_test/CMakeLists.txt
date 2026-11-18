cmake_minimum_required(VERSION 3.16)
project(camera_capture_test CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# libir SDK 库目录 (与主程序 Makefile 默认一致: aarch64 native)
set(IR_SDK_LIB_DIR "" CACHE PATH
    "Directory containing the libir SDK libraries")
if(NOT IR_SDK_LIB_DIR)
    set(IR_SDK_LIB_DIR
        "/mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs")
endif()
if(NOT IS_DIRECTORY "${IR_SDK_LIB_DIR}")
    message(FATAL_ERROR
        "SDK library directory does not exist: ${IR_SDK_LIB_DIR}")
endif()

function(find_sdk_library output_name library_name)
    find_library(${output_name}
        NAMES "${library_name}"
        PATHS "${IR_SDK_LIB_DIR}"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT ${output_name})
        message(FATAL_ERROR
            "Cannot find lib${library_name} in ${IR_SDK_LIB_DIR}")
    endif()
    set(${output_name} "${${output_name}}" PARENT_SCOPE)
endfunction()

find_sdk_library(IRUVC_LIBRARY iruvc)
find_sdk_library(IRTEMP_LIBRARY irtemp)
find_sdk_library(IRPROCESS_LIBRARY irprocess)
find_sdk_library(IRPARSE_LIBRARY irparse)
find_sdk_library(USB_LIBRARY usb-1.0)

find_package(OpenCV REQUIRED COMPONENTS core highgui imgcodecs imgproc)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)

# 采集链路与主程序一致: ir_camera + visible_camera + mjpeg 解码 + 同步器
set(CAPTURE_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ir_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/visible_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/mjpeg_decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/synchronizer.cpp
)

add_executable(camera_capture_test ${CAPTURE_TEST_SOURCES})

target_include_directories(camera_capture_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
    ${OpenCV_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS}
)

target_compile_options(camera_capture_test PRIVATE
    -Wall -Wextra -Wpedantic
)

target_link_libraries(camera_capture_test PRIVATE
    "${IRUVC_LIBRARY}"
    "${IRTEMP_LIBRARY}"
    "${IRPROCESS_LIBRARY}"
    "${IRPARSE_LIBRARY}"
    "${USB_LIBRARY}"
    ${OpenCV_LIBS}
    ${JPEG_LIBRARIES}
    Threads::Threads
    m
)

# SDK so 拷贝到运行目录 + rpath, 与主程序一致
file(GLOB SDK_SHARED_LIBS "${IR_SDK_LIB_DIR}/*.so" "${IR_SDK_LIB_DIR}/*.so.*")
if(SDK_SHARED_LIBS)
    add_custom_command(TARGET camera_capture_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:camera_capture_test>/libs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${SDK_SHARED_LIBS}
            "$<TARGET_FILE_DIR:camera_capture_test>/libs"
        VERBATIM
    )
endif()

# SDK 运行时依赖的固件/快门文件 (与主程序一致)
set(SDK_RUNTIME_DATA
    "${CMAKE_CURRENT_SOURCE_DIR}/../../RS001_FW_new.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tau_H.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tau_L.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../new_tau_H.bin"
)
foreach(DATA_FILE IN LISTS SDK_RUNTIME_DATA)
    if(EXISTS "${DATA_FILE}")
        add_custom_command(TARGET camera_capture_test POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${DATA_FILE}"
                "$<TARGET_FILE_DIR:camera_capture_test>"
            VERBATIM
        )
    endif()
endforeach()

set_target_properties(camera_capture_test PROPERTIES
    BUILD_RPATH "$ORIGIN/libs;${IR_SDK_LIB_DIR}"
)
