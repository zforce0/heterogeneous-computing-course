#include "detector_onnx.h"
#include "yolo_postprocess.h"

#include <onnxruntime_cxx_api.h>

#include <cstring>
#include <stdexcept>
#include <vector>

struct OnnxDetector::Impl {
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
              input_size(640), num_classes(1), num_anchors(8400), nc_plus_4(5) {}
};

OnnxDetector::OnnxDetector()
    : impl_(nullptr), input_size_(640)
{
}

OnnxDetector::~OnnxDetector()
{
    delete impl_;
}

bool OnnxDetector::load(const std::string& model_path)
{
    try {
        delete impl_;
        impl_ = new Impl();

        impl_->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "OnnxDetector");
        impl_->session_options = Ort::SessionOptions();
        impl_->session_options.SetIntraOpNumThreads(1);
        impl_->session = Ort::Session(impl_->env, model_path.c_str(), impl_->session_options);

        auto input_name_ptr = impl_->session.GetInputNameAllocated(0, impl_->allocator);
        auto output_name_ptr = impl_->session.GetOutputNameAllocated(0, impl_->allocator);
        impl_->input_name = std::string(input_name_ptr.get());
        impl_->output_name = std::string(output_name_ptr.get());

        Ort::TypeInfo input_type_info = impl_->session.GetInputTypeInfo(0);
        auto input_shape = input_type_info.GetTensorTypeAndShapeInfo().GetShape();
        impl_->input_size = static_cast<int>(input_shape[2]);

        Ort::TypeInfo output_type_info = impl_->session.GetOutputTypeInfo(0);
        auto output_shape = output_type_info.GetTensorTypeAndShapeInfo().GetShape();
        int nc4 = static_cast<int>(output_shape[1]);
        int anchors = static_cast<int>(output_shape[2]);

        impl_->nc_plus_4 = nc4;
        impl_->num_classes = nc4 - 4;
        impl_->num_anchors = anchors;

        input_size_ = impl_->input_size;
        config_.model_size = impl_->input_size;
        config_.num_classes = impl_->num_classes;

        return true;
    } catch (const std::exception&) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
}

bool OnnxDetector::detect(const BgrFrame& visible,
                          const BgrFrame& infrared,
                          std::vector<Detection>& results)
{
    if (!impl_) return false;

    try {
        int is = input_size_;
        int ch = 6;

        std::vector<float> input_buffer(1 * ch * is * is, 0.0f);

        auto preprocess_one = [&](const BgrFrame& frame, float* dst_chw, int dst_ch) {
            int fw = frame.width;
            int fh = frame.height;
            int stride = frame.stride;
            const uint8_t* src = frame.data.empty() ? nullptr : frame.data.data();

            if (!src || fw <= 0 || fh <= 0) return;

            float scale_x = static_cast<float>(fw) / is;
            float scale_y = static_cast<float>(fh) / is;

            for (int y = 0; y < is; ++y) {
                int sy = static_cast<int>(y * scale_y);
                if (sy >= fh) sy = fh - 1;

                for (int x = 0; x < is; ++x) {
                    int sx = static_cast<int>(x * scale_x);
                    if (sx >= fw) sx = fw - 1;

                    int src_idx = sy * stride + sx * 3;
                    float b = src[src_idx];
                    float g = src[src_idx + 1];
                    float r = src[src_idx + 2];

                    dst_chw[dst_ch * is * is + y * is + x] = r / 255.0f;
                    dst_chw[(dst_ch + 1) * is * is + y * is + x] = g / 255.0f;
                    dst_chw[(dst_ch + 2) * is * is + y * is + x] = b / 255.0f;
                }
            }
        };

        /* 通道拼接 (camerayolo.md 5.2, 与训练 loaders.py 一致):
         * cv2.merge((ir, rgb)) → 前 3 通道 = IR, 后 3 通道 = RGB */
        float* ir_chw = input_buffer.data() + 0;
        float* rgb_chw = input_buffer.data() + 3 * is * is;

        preprocess_one(infrared, ir_chw, 0);
        preprocess_one(visible, rgb_chw, 0);

        int64_t input_shape_data[4] = {1, ch, is, is};
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            input_buffer.data(),
            input_buffer.size(),
            input_shape_data,
            4);

        const char* input_names[] = {impl_->input_name.c_str()};
        const char* output_names[] = {impl_->output_name.c_str()};

        Ort::RunOptions run_options;
        auto output_tensors = impl_->session.Run(
            run_options,
            input_names, &input_tensor, 1,
            output_names, 1);

        if (output_tensors.empty()) return false;

        Ort::Value& output_tensor = output_tensors[0];
        auto output_info = output_tensor.GetTensorTypeAndShapeInfo();
        auto output_shape = output_info.GetShape();
        const float* output_data = output_tensor.GetTensorData<float>();

        int batch = static_cast<int>(output_shape[0]);
        int nc4 = static_cast<int>(output_shape[1]);
        int anchors = static_cast<int>(output_shape[2]);

        results = yolo_postprocess(
            output_data,
            batch,
            nc4,
            anchors,
            config_.conf_threshold,
            config_.iou_threshold);

        return true;
    } catch (const std::exception&) {
        return false;
    }
}