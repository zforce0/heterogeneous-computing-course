// Copyright (c) 2023 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef _RKNN_DEMO_YOLOV8_H_
#define _RKNN_DEMO_YOLOV8_H_

#include "rknn_api.h"
#include <opencv2/opencv.hpp>


typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;

    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} rknn_app_context_t;

#include "postprocess.h"

int yolov8_detect_init(const char* model_path, rknn_app_context_t* app_ctx);

int yolov8_detect_release(rknn_app_context_t* app_ctx);

// 推理总耗时输出(可空):letterbox+提交+NPU+取回+后处理的整体时长(毫秒)
int yolov8_detect_run(rknn_app_context_t* app_ctx, cv::Mat image, detect_result_group_t* od_results,
                      double* infer_ms = 0);

#endif //_RKNN_DEMO_YOLOV8_H_