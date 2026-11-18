#include "detector_yolov8.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

/* letterbox 参数: 源帧(w×h) 等比缩放 + 居中填充到 size×size */
struct LetterBoxParams {
    float scale = 1.0f;  /* 等比缩放系数 (<=size/min(w,h)) */
    int pad_x = 0;       /* 水平填充像素 (左) */
    int pad_y = 0;       /* 垂直填充像素 (上) */
    int content_w = 0;   /* 缩放后内容宽度 */
    int content_h = 0;   /* 缩放后内容高度 */
    int src_w = 0;
    int src_h = 0;
    int size = 640;

    /* ultralytics LetterBox: r = min(size/w, size/h);
     * new_unpad = round(dim*r); pad = (size - new_unpad) / 2 */
    static LetterBoxParams compute(int src_w, int src_h, int size)
    {
        LetterBoxParams p;
        p.src_w = src_w;
        p.src_h = src_h;
        p.size = size;
        if (src_w <= 0 || src_h <= 0) {
            p.scale = 1.0f;
            p.content_w = size;
            p.content_h = size;
            return p;
        }
        p.scale = std::min(static_cast<float>(size) / src_w,
                           static_cast<float>(size) / src_h);
        p.content_w = static_cast<int>(src_w * p.scale + 0.5f);
        p.content_h = static_cast<int>(src_h * p.scale + 0.5f);
        p.content_w = std::max(1, std::min(p.content_w, size));
        p.content_h = std::max(1, std::min(p.content_h, size));
        p.pad_x = (size - p.content_w) / 2;
        p.pad_y = (size - p.content_h) / 2;
        return p;
    }
};

/* ---------- YOLOv8 自带预处理: LetterBox + BGR→RGB + /255 + CHW ---------- */

static void yolov8_preprocess(const BgrFrame& frame,
                              const LetterBoxParams& p,
                              float* dst_chw /* [3, size, size] */)
{
    const int size = p.size;
    const int plane = size * size;
    const uint8_t* src =
        frame.data.empty() ? nullptr : frame.data.data();
    const int stride = frame.stride;

    /* 双线性采样源坐标 (模型空间 → 源帧空间) */
    const float inv_scale = 1.0f / p.scale;
    const int max_x = frame.width - 1;
    const int max_y = frame.height - 1;

    for (int y = 0; y < size; ++y) {
        const bool in_y = (y >= p.pad_y && y < p.pad_y + p.content_h);
        for (int x = 0; x < size; ++x) {
            float r, g, b;
            const bool in_x = (x >= p.pad_x && x < p.pad_x + p.content_w);
            if (!src || !in_x || !in_y) {
                /* 灰边填充 114/255 */
                const float pad = 114.0f / 255.0f;
                r = g = b = pad;
            } else {
                /* 双线性插值 (等价 cv2.INTER_LINEAR) */
                const float sx = (x - p.pad_x) * inv_scale;
                const float sy = (y - p.pad_y) * inv_scale;
                const int x0 = std::min(
                    static_cast<int>(sx), max_x);
                const int y0 = std::min(
                    static_cast<int>(sy), max_y);
                const int x1 = std::min(x0 + 1, max_x);
                const int y1 = std::min(y0 + 1, max_y);
                const float fx =
                    std::max(0.0f, std::min(sx - x0, 1.0f));
                const float fy =
                    std::max(0.0f, std::min(sy - y0, 1.0f));

                const int idx00 = y0 * stride + x0 * 3;
                const int idx10 = y0 * stride + x1 * 3;
                const int idx01 = y1 * stride + x0 * 3;
                const int idx11 = y1 * stride + x1 * 3;

                const float w00 = (1 - fx) * (1 - fy);
                const float w10 = fx * (1 - fy);
                const float w01 = (1 - fx) * fy;
                const float w11 = fx * fy;

                b = (src[idx00] * w00 + src[idx10] * w10 +
                     src[idx01] * w01 + src[idx11] * w11) / 255.0f;
                g = (src[idx00 + 1] * w00 + src[idx10 + 1] * w10 +
                     src[idx01 + 1] * w01 + src[idx11 + 1] * w11) /
                    255.0f;
                r = (src[idx00 + 2] * w00 + src[idx10 + 2] * w10 +
                     src[idx01 + 2] * w01 + src[idx11 + 2] * w11) /
                    255.0f;
            }
            const int offset = y * size + x;
            dst_chw[0 * plane + offset] = r;
            dst_chw[1 * plane + offset] = g;
            dst_chw[2 * plane + offset] = b;
        }
    }
}

/* ---------- YOLOv8 自带后处理: 解码 + NMS + 反 LetterBox ---------- */

/* xyxy IoU */
static float box_iou(const float* a, const float* b)
{
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[2], b[2]);
    const float y2 = std::min(a[3], b[3]);
    const float iw = x2 - x1;
    const float ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.0f;
    const float inter = iw * ih;
    const float area_a =
        (a[2] - a[0]) * (a[3] - a[1]);
    const float area_b =
        (b[2] - b[0]) * (b[3] - b[1]);
    return inter / (area_a + area_b - inter + 1e-9f);
}

static std::vector<Detection> yolov8_postprocess(
    const float* output,      /* [1, nc+4, anchors] */
    int nc_plus_4,
    int num_anchors,
    const LetterBoxParams& p,
    float conf_threshold,
    float iou_threshold)
{
    std::vector<Detection> results;
    const int num_classes = nc_plus_4 - 4;
    if (num_classes <= 0 || num_anchors <= 0) return results;

    /* 1. 逐 anchor 解码: xywh + 最大类别分数 */
    std::vector<float> boxes;    /* xyxy */
    std::vector<float> scores;
    std::vector<int> class_ids;
    for (int a = 0; a < num_anchors; ++a) {
        float max_score = -1.0f;
        int max_class = 0;
        for (int c = 0; c < num_classes; ++c) {
            const float score =
                output[(4 + c) * num_anchors + a];
            if (score > max_score) {
                max_score = score;
                max_class = c;
            }
        }
        if (max_score <= conf_threshold) continue;

        const float cx = output[a];
        const float cy = output[num_anchors + a];
        const float w = output[2 * num_anchors + a];
        const float h = output[3 * num_anchors + a];

        boxes.push_back(cx - w * 0.5f);
        boxes.push_back(cy - h * 0.5f);
        boxes.push_back(cx + w * 0.5f);
        boxes.push_back(cy + h * 0.5f);
        scores.push_back(max_score);
        class_ids.push_back(max_class);
    }

    /* 2. NMS (按分数降序, IoU > 阈值抑制) */
    const int n = static_cast<int>(scores.size());
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) {
                  return scores[a] > scores[b];
              });
    std::vector<int> suppressed(n, 0);
    std::vector<int> keep;
    for (int i = 0; i < n; ++i) {
        const int idx = order[i];
        if (suppressed[idx]) continue;
        keep.push_back(idx);
        for (int j = i + 1; j < n; ++j) {
            const int other = order[j];
            if (suppressed[other]) continue;
            if (box_iou(&boxes[idx * 4], &boxes[other * 4]) >
                iou_threshold) {
                suppressed[other] = 1;
            }
        }
    }

    /* 3. 反 LetterBox: 模型坐标 → 源帧像素坐标,
     *    再换算到 size×size 正方形空间 (供子画面按比例映射) */
    const int size = p.size;
    const float inv_scale = 1.0f / p.scale;
    const float to_square_x =
        p.src_w > 0
            ? static_cast<float>(size) / p.src_w
            : 1.0f;
    const float to_square_y =
        p.src_h > 0
            ? static_cast<float>(size) / p.src_h
            : 1.0f;

    results.reserve(keep.size());
    for (int idx : keep) {
        const float* bb = &boxes[idx * 4];
        /* clamp 到 letterbox 内容区, 避免反变换后越界 */
        float x1 = std::max(
            bb[0], static_cast<float>(p.pad_x));
        float y1 = std::max(
            bb[1], static_cast<float>(p.pad_y));
        float x2 = std::min(
            bb[2], static_cast<float>(p.pad_x + p.content_w));
        float y2 = std::min(
            bb[3], static_cast<float>(p.pad_y + p.content_h));
        if (x2 <= x1 || y2 <= y1) continue;

        /* 源帧像素坐标 */
        x1 = (x1 - p.pad_x) * inv_scale;
        y1 = (y1 - p.pad_y) * inv_scale;
        x2 = (x2 - p.pad_x) * inv_scale;
        y2 = (y2 - p.pad_y) * inv_scale;
        x1 = std::max(0.0f, std::min(x1, p.src_w - 1.0f));
        y1 = std::max(0.0f, std::min(y1, p.src_h - 1.0f));
        x2 = std::max(0.0f, std::min(x2, p.src_w - 1.0f));
        y2 = std::max(0.0f, std::min(y2, p.src_h - 1.0f));

        Detection det;
        det.x = x1 * to_square_x;
        det.y = y1 * to_square_y;
        det.w = (x2 - x1) * to_square_x;
        det.h = (y2 - y1) * to_square_y;
        det.confidence = scores[idx];
        det.class_id = class_ids[idx];
        results.push_back(det);
    }
    return results;
}

/* ---------- ONNX Runtime 会话 ---------- */

struct Yolov8OnnxDetector::Impl {
    Ort::Env env;
    Ort::SessionOptions session_options;
    Ort::Session session;
    Ort::AllocatorWithDefaultOptions allocator;
    std::string input_name;
    std::string output_name;
    int input_size;
    int num_classes;
    int num_anchors;
    int nc_plus_4;

    Impl()
        : env(nullptr), session_options(nullptr), session(nullptr),
          input_size(640), num_classes(1), num_anchors(8400),
          nc_plus_4(5) {}
};

Yolov8OnnxDetector::Yolov8OnnxDetector()
    : impl_(nullptr), input_size_(640),
      conf_threshold_(0.25f), iou_threshold_(0.45f)
{
}

Yolov8OnnxDetector::~Yolov8OnnxDetector()
{
    delete impl_;
}

bool Yolov8OnnxDetector::load(const std::string& model_path)
{
    try {
        delete impl_;
        impl_ = new Impl();

        impl_->env =
            Ort::Env(ORT_LOGGING_LEVEL_WARNING,
                     "Yolov8OnnxDetector");
        impl_->session_options = Ort::SessionOptions();
        impl_->session_options.SetIntraOpNumThreads(1);
        impl_->session = Ort::Session(impl_->env,
                                      model_path.c_str(),
                                      impl_->session_options);

        auto input_name_ptr = impl_->session.GetInputNameAllocated(
            0, impl_->allocator);
        auto output_name_ptr =
            impl_->session.GetOutputNameAllocated(
                0, impl_->allocator);
        impl_->input_name = std::string(input_name_ptr.get());
        impl_->output_name = std::string(output_name_ptr.get());

        Ort::TypeInfo input_type_info =
            impl_->session.GetInputTypeInfo(0);
        auto input_shape = input_type_info
                               .GetTensorTypeAndShapeInfo()
                               .GetShape();
        /* 仅支持 3 通道可见光输入 (6 通道为 YOLOFuse) */
        const int channels =
            static_cast<int>(input_shape[1]);
        if (channels != 3) {
            std::fprintf(stderr,
                         "[Yolov8] unsupported input channels "
                         "%d (expect 3)\n",
                         channels);
            delete impl_;
            impl_ = nullptr;
            return false;
        }
        impl_->input_size =
            static_cast<int>(input_shape[2]);

        Ort::TypeInfo output_type_info =
            impl_->session.GetOutputTypeInfo(0);
        auto output_shape = output_type_info
                                .GetTensorTypeAndShapeInfo()
                                .GetShape();
        impl_->nc_plus_4 =
            static_cast<int>(output_shape[1]);
        impl_->num_anchors =
            static_cast<int>(output_shape[2]);
        impl_->num_classes =
            impl_->nc_plus_4 - 4;

        input_size_ = impl_->input_size;
        return true;
    } catch (const std::exception&) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
}

bool Yolov8OnnxDetector::detect(
    const BgrFrame& visible, const BgrFrame& infrared,
    std::vector<Detection>& results)
{
    (void)infrared; /* 可见光单模态: IR 帧不参与推理 */
    if (!impl_) return false;

    try {
        const int is = input_size_;
        const LetterBoxParams params =
            LetterBoxParams::compute(visible.width,
                                     visible.height, is);

        std::vector<float> input_buffer(
            1 * 3 * is * is, 0.0f);
        yolov8_preprocess(visible, params,
                          input_buffer.data());

        const int64_t input_shape_data[4] = {
            1, 3, is, is};
        auto memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor =
            Ort::Value::CreateTensor<float>(
                memory_info, input_buffer.data(),
                input_buffer.size(), input_shape_data, 4);

        const char* input_names[] = {
            impl_->input_name.c_str()};
        const char* output_names[] = {
            impl_->output_name.c_str()};

        Ort::RunOptions run_options;
        auto output_tensors = impl_->session.Run(
            run_options, input_names, &input_tensor, 1,
            output_names, 1);
        if (output_tensors.empty()) return false;

        Ort::Value& output_tensor = output_tensors[0];
        const float* output_data =
            output_tensor.GetTensorData<float>();

        results = yolov8_postprocess(
            output_data, impl_->nc_plus_4,
            impl_->num_anchors, params,
            conf_threshold_, iou_threshold_);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
