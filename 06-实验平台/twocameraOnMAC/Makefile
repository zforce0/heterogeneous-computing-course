SHELL := /bin/sh

CMAKE ?= cmake
JOBS ?= $(shell nproc 2>/dev/null || printf '1')

NATIVE_BUILD_DIR ?= build/native
# 本机 (aarch64 Ubuntu) 的 libir SDK 库目录 (见 202608201547.md)
TWOCAMERA_SDK_LIB_DIR ?= /mnt/hgfs/share/code/linux_ac010/aarch64-linux-gnu_libs
ONNXRUNTIME_DIR ?= /tmp/onnxruntime-linux-aarch64-1.21.0

# 默认启用 YOLO 双模态检测 (需要 ONNXRUNTIME_DIR 存在);
# 纯采集预览: make native YOLO_CMAKE_ARGS=
YOLO_CMAKE_ARGS ?= \
	-DTWOCAMERA_ENABLE_YOLO=ON \
	-DTWOCAMERA_ONNXRUNTIME_DIR="$(ONNXRUNTIME_DIR)"

NATIVE_CMAKE_ARGS ?=
RUN_ARGS ?=

.PHONY: all native run clean help

all: native

native:
	$(CMAKE) -S . -B "$(NATIVE_BUILD_DIR)" \
		-DTWOCAMERA_SDK_LIB_DIR="$(TWOCAMERA_SDK_LIB_DIR)" \
		$(YOLO_CMAKE_ARGS) $(NATIVE_CMAKE_ARGS)
	$(CMAKE) --build "$(NATIVE_BUILD_DIR)" --parallel "$(JOBS)"

run: native
	"$(NATIVE_BUILD_DIR)/sample" $(RUN_ARGS)

clean:
	$(CMAKE) -E remove_directory build

help:
	@printf '%s\n' \
		"make native                                   # 默认启用 YOLO" \
		"make native YOLO_CMAKE_ARGS=                  # 纯采集预览 (无检测)" \
		"make run RUN_ARGS='--visible /dev/video1'" \
		"make clean"
