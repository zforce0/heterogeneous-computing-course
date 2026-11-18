cmake_minimum_required(VERSION 3.16)
project(ir_temperature_test CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# ──────────────────────────────────────────────────────────
# 用户可覆盖的缓存变量
# ──────────────────────────────────────────────────────────
set(IR_TEMP_SDK_LIB_DIR "" CACHE PATH
    "Directory containing libir SDK libraries (libiruvc/libirtemp/libirprocess/libirparse/libusb-1.0)")
if(NOT IR_TEMP_SDK_LIB_DIR)
    if(CMAKE_CROSSCOMPILING)
        set(IR_TEMP_SDK_LIB_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/arm-openwrt-linux-gnueabi")
    else()
        set(IR_TEMP_SDK_LIB_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/native")
    endif()
endif()
if(NOT IS_DIRECTORY "${IR_TEMP_SDK_LIB_DIR}")
    message(FATAL_ERROR
        "SDK library directory does not exist: ${IR_TEMP_SDK_LIB_DIR}")
endif()

# ──────────────────────────────────────────────────────────
# 复用项目 src/ 下的 ir_camera.cpp + temperature.cpp
# 注意: sample.cpp / cmd.cpp 不能复用, 它们各自有 main / 全局符号冲突
# ──────────────────────────────────────────────────────────
set(IR_TEMP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/ir_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/temperature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_executable(ir_temperature_test ${IR_TEMP_SOURCES})

target_include_directories(ir_temperature_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)

# sample_config.h 在 src/ 目录内 (ir_camera.h 通过 #include "sample_config.h")
# 已经被上面的 ${CMAKE_CURRENT_SOURCE_DIR}/../../src 覆盖

target_compile_options(ir_temperature_test PRIVATE
    -Wall -Wextra -Wpedantic
)

# ──────────────────────────────────────────────────────────
# 查找 SDK 库 (复用主项目的 twocamera_find_sdk_library 写法)
# ──────────────────────────────────────────────────────────
function(ir_temp_find_sdk_library out_var lib_name)
    find_library(${out_var}
        NAMES "${lib_name}"
        PATHS "${IR_TEMP_SDK_LIB_DIR}"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT ${out_var})
        message(FATAL_ERROR
            "Cannot find lib${lib_name} in ${IR_TEMP_SDK_LIB_DIR}")
    endif()
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

ir_temp_find_sdk_library(IRUVC_LIBRARY  iruvc)
ir_temp_find_sdk_library(IRTEMP_LIBRARY irtemp)
ir_temp_find_sdk_library(IRPROCESS_LIBRARY irprocess)
ir_temp_find_sdk_library(IRPARSE_LIBRARY irparse)
ir_temp_find_sdk_library(USB_LIBRARY    usb-1.0)

find_package(Threads REQUIRED)
target_link_libraries(ir_temperature_test PRIVATE
    "${IRUVC_LIBRARY}"
    "${IRTEMP_LIBRARY}"
    "${IRPROCESS_LIBRARY}"
    "${IRPARSE_LIBRARY}"
    "${USB_LIBRARY}"
    Threads::Threads
    m
)

# ──────────────────────────────────────────────────────────
# RPATH: 让产物能在 build 目录直接运行 (找同目录 libs/*.so)
# ──────────────────────────────────────────────────────────
set_target_properties(ir_temperature_test PROPERTIES
    BUILD_RPATH "$ORIGIN/libs;${IR_TEMP_SDK_LIB_DIR}"
)

# ──────────────────────────────────────────────────────────
# POST_BUILD: 拷贝 SDK .so + 固件 .bin 到产物目录
# (ir_camera 需要 tau_H.bin / tau_L.bin 等仅在命令行调用时使用,
#  这里为完整性一并拷贝; RS001_FW_new.bin 用于 UPDATE_FW 模式)
# ──────────────────────────────────────────────────────────
file(GLOB IR_TEMP_SDK_SHARED_LIBS
    "${IR_TEMP_SDK_LIB_DIR}/*.so"
    "${IR_TEMP_SDK_LIB_DIR}/*.so.*"
)
if(IR_TEMP_SDK_SHARED_LIBS)
    add_custom_command(TARGET ir_temperature_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:ir_temperature_test>/libs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${IR_TEMP_SDK_SHARED_LIBS}
            "$<TARGET_FILE_DIR:ir_temperature_test>/libs"
        VERBATIM
    )
endif()

set(IR_TEMP_COMPAT_DATA
    "${CMAKE_CURRENT_SOURCE_DIR}/../../RS001_FW_new.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tau_H.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tau_L.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../new_tau_H.bin"
)
foreach(DATA_FILE IN LISTS IR_TEMP_COMPAT_DATA)
    if(EXISTS "${DATA_FILE}")
        add_custom_command(TARGET ir_temperature_test POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${DATA_FILE}"
                "$<TARGET_FILE_DIR:ir_temperature_test>"
            VERBATIM
        )
    endif()
endforeach()
