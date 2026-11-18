cmake_minimum_required(VERSION 3.16)

project(twocamera LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(TWOCAMERA_SDK_LIB_DIR "" CACHE PATH
    "Directory containing the libir SDK libraries")
set(TWOCAMERA_JPEG_ROOT "" CACHE PATH
    "Optional libjpeg root containing include/ and lib/")

if(NOT DEFINED TWOCAMERA_DISPLAY_BACKEND)
    set(TWOCAMERA_DISPLAY_BACKEND "OPENCV" CACHE STRING
        "Display backend: OPENCV or FBDEV")
endif()
set_property(CACHE TWOCAMERA_DISPLAY_BACKEND
    PROPERTY STRINGS OPENCV FBDEV)
string(TOUPPER "${TWOCAMERA_DISPLAY_BACKEND}"
    TWOCAMERA_DISPLAY_BACKEND_NORMALIZED)

option(TWOCAMERA_NO_JPEG
    "Use YUYV instead of MJPG (avoids libjpeg dependency)" OFF)
mark_as_advanced(TWOCAMERA_NO_JPEG)

# ---------------------------------------------------------------------------
# YOLO 双模态检测 (camerayolo.md 阶段二: C++ ONNX 集成)
# ---------------------------------------------------------------------------
option(TWOCAMERA_ENABLE_YOLO "Enable YOLO detection" OFF)
set(TWOCAMERA_ONNXRUNTIME_DIR "" CACHE PATH
    "ONNX Runtime directory containing include/ and lib/")

# ---------------------------------------------------------------------------
# ByteTrack 多目标跟踪 (camerayolo.md 3.4: Kalman + Hungarian + 运动意图)
# ---------------------------------------------------------------------------
option(TWOCAMERA_ENABLE_TRACKER "Enable ByteTrack tracking" ON)

set(TWOCAMERA_SOURCES
    src/cmd.cpp
    src/ir_camera.cpp
    src/sample.cpp
    src/temperature.cpp
    src/visible_camera.cpp
)
set(TWOCAMERA_NEEDS_JPEG ON)
set(TWOCAMERA_NEEDS_COMPOSITOR ON)

if(TWOCAMERA_DISPLAY_BACKEND_NORMALIZED STREQUAL "OPENCV")
    find_package(OpenCV REQUIRED COMPONENTS core highgui)
    list(APPEND TWOCAMERA_SOURCES src/display_opencv.cpp)
elseif(TWOCAMERA_DISPLAY_BACKEND_NORMALIZED STREQUAL "FBDEV")
    list(APPEND TWOCAMERA_SOURCES src/display_fbdev.cpp)
    if(NOT TWOCAMERA_NO_JPEG)
        set(TWOCAMERA_NEEDS_JPEG ON)
    endif()
else()
    message(FATAL_ERROR
        "TWOCAMERA_DISPLAY_BACKEND must be OPENCV or FBDEV")
endif()

if(TWOCAMERA_NEEDS_COMPOSITOR)
    # 同步 → 配准 → 并排合成 管线 (frame_compositor.* 为旧 PiP 模式, 保留源码不编译)
    list(APPEND TWOCAMERA_SOURCES
        src/image_resize.cpp
        src/image_registrator.cpp
        src/side_by_side_compositor.cpp
        src/synchronizer.cpp
    )
endif()
if(TWOCAMERA_NEEDS_JPEG)
    list(APPEND TWOCAMERA_SOURCES src/mjpeg_decoder.cpp)
endif()

if(TWOCAMERA_ENABLE_YOLO)
    # IDetector 推理后端: 预处理 + ONNX Runtime 推理 + NMS 后处理 + 伪彩
    # detector_onnx.*    = YOLOFuse 双模态 (IR+RGB 6ch, 直拉缩放预处理)
    # detector_yolov8.*  = YOLOv8n 可见光人像 (3ch, LetterBox 预处理, 暂时方案)
    list(APPEND TWOCAMERA_SOURCES
        src/detector_onnx.cpp
        src/detector_yolov8.cpp
        src/yolo_postprocess.cpp
        src/pseudo_color.cpp
    )
    if(TWOCAMERA_ENABLE_TRACKER)
        # ByteTrack: Kalman 滤波 + Hungarian 匹配 + 跟踪叠加
        # + 新目标提示音 (AlertNotifier, dlopen ALSA 无链接依赖)
        list(APPEND TWOCAMERA_SOURCES
            src/kalman_filter.cpp
            src/tracker_bytetrack.cpp
            src/tracking_overlay.cpp
            src/alert_notifier.cpp
        )
    endif()
    # ONNX Runtime 头文件 (float16 constexpr) 触发 -Wpedantic 告警, 单独关闭
    set_source_files_properties(src/detector_onnx.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-pedantic"
    )
    set_source_files_properties(src/detector_yolov8.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-pedantic"
    )
endif()

add_executable(twocamera ${TWOCAMERA_SOURCES})
set_target_properties(twocamera PROPERTIES OUTPUT_NAME sample)
if(TWOCAMERA_NO_JPEG)
    target_compile_definitions(twocamera PRIVATE
        TWOCAMERA_NO_JPEG=1
    )
endif()

target_include_directories(twocamera PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(twocamera PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

if(NOT TWOCAMERA_SDK_LIB_DIR)
    set(TWOCAMERA_SDK_LIB_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/native")
endif()
if(NOT IS_DIRECTORY "${TWOCAMERA_SDK_LIB_DIR}")
    message(FATAL_ERROR
        "SDK library directory does not exist: ${TWOCAMERA_SDK_LIB_DIR}")
endif()

function(twocamera_find_sdk_library output_name library_name)
    find_library(${output_name}
        NAMES "${library_name}"
        PATHS "${TWOCAMERA_SDK_LIB_DIR}"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT ${output_name})
        message(FATAL_ERROR
            "Cannot find lib${library_name} in ${TWOCAMERA_SDK_LIB_DIR}")
    endif()
    set(${output_name} "${${output_name}}" PARENT_SCOPE)
endfunction()

twocamera_find_sdk_library(IRUVC_LIBRARY iruvc)
twocamera_find_sdk_library(IRTEMP_LIBRARY irtemp)
twocamera_find_sdk_library(IRPROCESS_LIBRARY irprocess)
twocamera_find_sdk_library(IRPARSE_LIBRARY irparse)
twocamera_find_sdk_library(USB_LIBRARY usb-1.0)

if(TWOCAMERA_NEEDS_JPEG)
    if(TWOCAMERA_JPEG_ROOT)
        find_path(JPEG_INCLUDE_DIR
            NAMES jpeglib.h
            PATHS "${TWOCAMERA_JPEG_ROOT}"
            PATH_SUFFIXES include usr/include
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        find_library(JPEG_LIBRARY
            NAMES jpeg
            PATHS "${TWOCAMERA_JPEG_ROOT}"
            PATH_SUFFIXES lib lib64 usr/lib usr/lib64
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        if(NOT JPEG_INCLUDE_DIR OR NOT JPEG_LIBRARY)
            message(FATAL_ERROR
                "libjpeg was not found under TWOCAMERA_JPEG_ROOT=${TWOCAMERA_JPEG_ROOT}")
        endif()
        set(JPEG_INCLUDE_DIRS "${JPEG_INCLUDE_DIR}")
        set(JPEG_LIBRARIES "${JPEG_LIBRARY}")
    else()
        find_package(JPEG REQUIRED)
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(twocamera PRIVATE
    "${IRUVC_LIBRARY}"
    "${IRTEMP_LIBRARY}"
    "${IRPROCESS_LIBRARY}"
    "${IRPARSE_LIBRARY}"
    Threads::Threads
    ${CMAKE_DL_LIBS}
    "${USB_LIBRARY}"
    m
)
if(TWOCAMERA_NEEDS_JPEG)
    target_include_directories(twocamera PRIVATE
        ${JPEG_INCLUDE_DIRS}
        ${JPEG_INCLUDE_DIR}
    )
    target_link_libraries(twocamera PRIVATE
        ${JPEG_LIBRARIES}
    )
endif()

if(TWOCAMERA_DISPLAY_BACKEND_NORMALIZED STREQUAL "OPENCV")
    target_include_directories(twocamera PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(twocamera PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_OPENCV=1)
endif()

if(TWOCAMERA_ENABLE_YOLO)
    if(NOT TWOCAMERA_ONNXRUNTIME_DIR)
        message(FATAL_ERROR
            "TWOCAMERA_ENABLE_YOLO=ON requires TWOCAMERA_ONNXRUNTIME_DIR "
            "pointing to the ONNX Runtime directory (include/ + lib/)")
    endif()
    find_path(ONNXRUNTIME_INCLUDE_DIR
        NAMES onnxruntime_cxx_api.h
        PATHS "${TWOCAMERA_ONNXRUNTIME_DIR}/include"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    find_library(ONNXRUNTIME_LIB
        NAMES onnxruntime
        PATHS "${TWOCAMERA_ONNXRUNTIME_DIR}/lib"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
    )
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
        message(FATAL_ERROR
            "ONNX Runtime not found under ${TWOCAMERA_ONNXRUNTIME_DIR}")
    endif()
    target_include_directories(twocamera PRIVATE
        "${ONNXRUNTIME_INCLUDE_DIR}"
    )
    target_link_libraries(twocamera PRIVATE "${ONNXRUNTIME_LIB}")
    target_compile_definitions(twocamera PRIVATE TWOCAMERA_HAS_YOLO=1)
    if(TWOCAMERA_ENABLE_TRACKER)
        target_compile_definitions(twocamera PRIVATE
            TWOCAMERA_HAS_TRACKER=1)
    endif()
    list(APPEND TWOCAMERA_EXTRA_RPATH
        "${TWOCAMERA_ONNXRUNTIME_DIR}/lib")
endif()

file(GLOB TWOCAMERA_SDK_SHARED_LIBS
    "${TWOCAMERA_SDK_LIB_DIR}/*.so"
    "${TWOCAMERA_SDK_LIB_DIR}/*.so.*"
)
set(TWOCAMERA_RUNTIME_LIBS ${TWOCAMERA_SDK_SHARED_LIBS})
foreach(JPEG_LINK_ITEM IN LISTS JPEG_LIBRARIES)
    if(EXISTS "${JPEG_LINK_ITEM}" AND
       "${JPEG_LINK_ITEM}" MATCHES "\\.so(\\..*)?$")
        get_filename_component(JPEG_LIBRARY_DIR
            "${JPEG_LINK_ITEM}" DIRECTORY)
        file(GLOB JPEG_SHARED_LIBS
            "${JPEG_LIBRARY_DIR}/libjpeg.so"
            "${JPEG_LIBRARY_DIR}/libjpeg.so.*")
        list(APPEND TWOCAMERA_RUNTIME_LIBS ${JPEG_SHARED_LIBS})
    endif()
endforeach()
if(TWOCAMERA_RUNTIME_LIBS)
    add_custom_command(TARGET twocamera POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:twocamera>/libs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${TWOCAMERA_RUNTIME_LIBS}
            "$<TARGET_FILE_DIR:twocamera>/libs"
        VERBATIM
    )
endif()

set(TWOCAMERA_COMPATIBILITY_DATA
    "${CMAKE_CURRENT_SOURCE_DIR}/RS001_FW_new.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/tau_H.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/tau_L.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/new_tau_H.bin"
)
foreach(DATA_FILE IN LISTS TWOCAMERA_COMPATIBILITY_DATA)
    if(EXISTS "${DATA_FILE}")
        add_custom_command(TARGET twocamera POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${DATA_FILE}"
                "$<TARGET_FILE_DIR:twocamera>"
            VERBATIM
        )
    endif()
endforeach()

set_target_properties(twocamera PROPERTIES
    BUILD_RPATH "$ORIGIN/libs;${TWOCAMERA_SDK_LIB_DIR};${TWOCAMERA_EXTRA_RPATH}"
)
