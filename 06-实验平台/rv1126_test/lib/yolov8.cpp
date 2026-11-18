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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "yolov8.h"
//#include "common.h"
#include "file_utils.h"
#include "perf_clock.h"

using namespace cv;

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static int letter_box(cv::Mat input_image, cv::Mat *output_image, int model_input_size)
{
    int input_width, input_height;

    input_width = input_image.cols;
    input_height = input_image.rows;
    float ratio;
    ratio = min((float)model_input_size / input_width, (float)model_input_size / input_height);

    int new_width, new_height;
    new_width = round(ratio * input_width );
    new_height = round(ratio * input_height);


    int height_padding = 0;
    int width_padding = 0;
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
    if( new_width >= new_height)
    {
        height_padding = new_width - new_height;
        if( (height_padding % 2) == 0 )
        {
            top = (int)((float)(height_padding/2));
            bottom = (int)((float)(height_padding/2));
        }
        else
        {
            top = (int)((float)(height_padding/2));
            bottom = (int)((float)(height_padding/2))+1;    
        }
    }
    else
    {
        width_padding = new_height - new_width;
        if( (width_padding % 2) == 0 )
        {
            left = (int)((float)(width_padding/2));
            right = (int)((float)(width_padding/2));
        }
        else
        {
            left = (int)((float)(width_padding/2));
            right = (int)((float)(width_padding/2))+1;
        }

    }

    cv::Mat resize_img;

    cv::resize(input_image, resize_img, cv::Size(new_width, new_height));
    cv::copyMakeBorder(resize_img, *output_image, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

    return 0;
}

static int scale_coords(detect_result_group_t *detect_result_group, int img_width, int img_height, int model_size)
{
    for (int i = 0; i < detect_result_group->count; i++)
    {
    detect_result_t *det_result = &(detect_result_group->results[i]);


    int x1 = det_result->box.left;
    int y1 = det_result->box.top;
    int x2 = det_result->box.right;
    int y2 = det_result->box.bottom;


    if( img_width >= img_height )
    {
      int image_max_len = img_width;
      float gain;
      gain = (float)model_size / image_max_len;
      int resized_height = img_height * gain;
      int height_pading = (model_size - resized_height)/2;
      y1 = (y1 - height_pading);
      y2 = (y2 - height_pading);
      x1 = int(x1 / gain);
      y1 = int(y1 / gain);
      x2 = int(x2 / gain);
      y2 = int(y2 / gain);

      det_result->box.left = x1;
      det_result->box.top = y1;
      det_result->box.right = x2;
      det_result->box.bottom = y2;
    }
    else
    {
      int image_max_len = img_height;
      float gain;
      gain = (float)model_size / image_max_len;
      int resized_width = img_width * gain;
      int width_pading = (model_size - resized_width)/2;
      x1 = (x1 - width_pading);
      x2 = (x2 - width_pading);
      x1 = int(x1 / gain);
      y1 = int(y1 / gain);
      x2 = int(x2 / gain);
      y2 = int(y2 / gain);

      det_result->box.left = x1;
      det_result->box.top = y1;
      det_result->box.right = x2;
      det_result->box.bottom = y2;  
    }

    }

    return 0;
}

//int init_yolov8_model(const char *model_path, rknn_app_context_t *app_ctx)
int yolov8_detect_init(const char *model_path, rknn_app_context_t *app_ctx)
{
    int ret;
    int model_len = 0;
    char* model_data;
    rknn_context ctx = 0;

    // Load model
    // Load RKNN Model
    model_len = read_data_from_file(model_path, &model_data);
    if (model_data == NULL) {
        printf("load_model fail!\n");
        return -1;
    }

    ret = rknn_init(&ctx, model_data, model_len, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }
    free(model_data);

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    //printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // Get Model Input Info
    //printf("input tensors:\n");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        //dump_tensor_attr(&(input_attrs[i]));
    }

    // Get Model Output Info
    //printf("output tensors:\n");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        //dump_tensor_attr(&(output_attrs[i]));
    }

    // TODO
    app_ctx->rknn_ctx = ctx;
    if (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && output_attrs[0].type == RKNN_TENSOR_INT8)
    {
        app_ctx->is_quant = true;
    }
    else
    {
        app_ctx->is_quant = false;
    }

    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        //printf("model is NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    }
    else
    {
        //printf("model is NHWC input fmt\n");
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    /*
    printf("model input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);
    */

    return 0;
}

int yolov8_detect_release(rknn_app_context_t *app_ctx)
{
    if (app_ctx->input_attrs != NULL)
    {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL)
    {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->rknn_ctx != 0)
    {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

int yolov8_detect_run(rknn_app_context_t* app_ctx, cv::Mat image, detect_result_group_t* od_results,
                      double* infer_ms)
{
    int ret;
    //letterbox_t letter_box;
    rknn_input inputs[app_ctx->io_num.n_input];
    rknn_output outputs[app_ctx->io_num.n_output];
    const float nms_threshold = NMS_THRESH;      // 默认的NMS阈值
    const float box_conf_threshold = BOX_THRESH; // 默认的置信度阈值
    int img_width = image.cols;
    int img_height = image.rows;

    const double t_begin = now_ms();

    memset(od_results, 0x00, sizeof(*od_results));
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    // 预处理:letterbox
    cv::Mat letter_image;
    letter_box(image, &letter_image, app_ctx->model_width);

    // Set Input Data
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = letter_image.data;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0)
    {
        printf("rknn_input_set fail! ret=%d\n", ret);
        return -1;
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0)
    {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    // Get Output
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < app_ctx->io_num.n_output; i++)
    {
        outputs[i].index = i;
        outputs[i].want_float = (!app_ctx->is_quant);
    }
    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0)
    {
        printf("rknn_outputs_get fail! ret=%d\n", ret);
    }

    // 后处理(DFL 解码+NMS+坐标逆映射)
    post_process(app_ctx, outputs, box_conf_threshold, nms_threshold, od_results);
    scale_coords(od_results, img_width, img_height, app_ctx->model_width);

    // Remeber to release rknn output
    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);

    if (infer_ms)
    {
        *infer_ms = now_ms() - t_begin;
    }

    return ret;
}