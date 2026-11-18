#include "detector.h"
#include "detector_onnx.h"
#include "detector_yolov8.h"
#include "frame.h"
#include "yolo_postprocess.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool load_bgr_image(const std::string& path, BgrFrame& frame)
{
    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
    if (img.empty()) return false;

    frame.width = img.cols;
    frame.height = img.rows;
    frame.stride = img.cols * 3;
    frame.data.assign(img.datastart, img.datastart + img.total() * img.elemSize());
    frame.timestamp_us = 0;
    frame.sequence = 0;
    return true;
}

static void print_usage(const char* prog)
{
    std::printf("Usage: %s [options]\n", prog);
    std::printf("  --model <path>       ONNX model path (default: models/yolov8n/yolov8n_person.onnx)\n");
    std::printf("  --backend <name>     yolov8 (default, 3ch visible only) | fuse (YOLOFuse 6ch)\n");
    std::printf("  --rgb <path>         RGB image path\n");
    std::printf("  --ir <path>          IR image path (yolov8 backend optional)\n");
    std::printf("  --conf <threshold>   Confidence threshold (default: 0.25)\n");
    std::printf("  --iou <threshold>    NMS IoU threshold (default: 0.45)\n");
    std::printf("  --help               Show this help\n");
}

int main(int argc, char* argv[])
{
    std::string model_path = "models/yolov8n/yolov8n_person.onnx";
    std::string backend = "yolov8";
    std::string rgb_path = "captures/images/000.jpg";
    std::string ir_path = "";
    float conf_threshold = 0.25f;
    float iou_threshold = 0.45f;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_path = argv[++i];
        } else if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (std::strcmp(argv[i], "--rgb") == 0 && i + 1 < argc) {
            rgb_path = argv[++i];
        } else if (std::strcmp(argv[i], "--ir") == 0 && i + 1 < argc) {
            ir_path = argv[++i];
        } else if (std::strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {
            conf_threshold = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--iou") == 0 && i + 1 < argc) {
            iou_threshold = std::atof(argv[++i]);
        }
    }

    const bool use_yolov8 = (backend != "fuse");

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== detector_test ===\n");
    std::printf("  Backend: %s\n", use_yolov8 ? "yolov8 (visible only)" : "fuse (IR+RGB)");
    std::printf("  Model:  %s\n", model_path.c_str());
    std::printf("  RGB:    %s\n", rgb_path.c_str());
    std::printf("  IR:     %s\n",
                ir_path.empty() ? "(unused by yolov8 backend)" : ir_path.c_str());
    std::printf("  Conf:   %.2f\n", conf_threshold);
    std::printf("  IoU:    %.2f\n", iou_threshold);

    BgrFrame rgb_frame, ir_frame;
    if (!load_bgr_image(rgb_path, rgb_frame)) {
        std::fprintf(stderr, "ERROR: Cannot load RGB image: %s\n", rgb_path.c_str());
        return 1;
    }
    if (!use_yolov8) {
        if (!load_bgr_image(ir_path, ir_frame)) {
            std::fprintf(stderr, "ERROR: Cannot load IR image: %s\n", ir_path.c_str());
            return 1;
        }
    } else if (!ir_path.empty()) {
        load_bgr_image(ir_path, ir_frame); /* yolov8 后端忽略, 仅加载打印 */
    }

    std::printf("\nLoaded images:\n");
    std::printf("  RGB: %dx%d, stride=%d, data_size=%zu\n",
                rgb_frame.width, rgb_frame.height,
                rgb_frame.stride, rgb_frame.data.size());
    std::printf("  IR:  %dx%d, stride=%d, data_size=%zu\n",
                ir_frame.width, ir_frame.height,
                ir_frame.stride, ir_frame.data.size());

    /* 双后端共用 IDetector 接口 */
    Yolov8OnnxDetector yolov8_detector;
    OnnxDetector fuse_detector;
    IDetector* detector = nullptr;
    if (use_yolov8) {
        yolov8_detector.set_conf_threshold(conf_threshold);
        yolov8_detector.set_iou_threshold(iou_threshold);
        detector = &yolov8_detector;
    } else {
        fuse_detector.set_conf_threshold(conf_threshold);
        fuse_detector.set_iou_threshold(iou_threshold);
        detector = &fuse_detector;
    }

    std::fprintf(stderr, "\nLoading ONNX model...\n");
    auto t_start = std::chrono::high_resolution_clock::now();
    if (!detector->load(model_path)) {
        std::fprintf(stderr, "ERROR: Failed to load model: %s\n", model_path.c_str());
        return 1;
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    std::printf("  Model loaded in %lld ms\n", (long long)load_ms);
    std::printf("  Input size: %d\n", detector->model_size());

    std::fprintf(stderr, "\nRunning inference...\n");
    std::vector<Detection> results;

    t_start = std::chrono::high_resolution_clock::now();
    if (!detector->detect(rgb_frame, ir_frame, results)) {
        std::fprintf(stderr, "ERROR: Inference failed\n");
        return 1;
    }
    t_end = std::chrono::high_resolution_clock::now();
    auto infer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    std::printf("  Inference completed in %lld ms\n", (long long)infer_ms);

    std::printf("\n=== Detection Results (%dx%d square coordinates) ===\n",
                detector->model_size(), detector->model_size());
    std::printf("  Total detections: %zu\n", results.size());

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& det = results[i];
        std::printf("  [%zu] class=%d, conf=%.3f, box=[%.1f, %.1f, %.1f, %.1f] "
                    "(x=%.1f, y=%.1f, w=%.1f, h=%.1f)\n",
                    i, det.class_id, det.confidence,
                    det.x, det.y, det.x + det.w, det.y + det.h,
                    det.x, det.y, det.w, det.h);
    }

    BgrFrame display_frame = rgb_frame;
    /* 检测框在 model_size 正方形空间, 按源帧宽高逐轴映射回原图 */
    draw_detections_in_region(display_frame, results,
                              detector->model_size(),
                              0, 0,
                              display_frame.width,
                              display_frame.height, 3);

    cv::Mat display(display_frame.height, display_frame.width, CV_8UC3, display_frame.data.data());
    std::string output_path = "/tmp/detector_test_result.jpg";
    cv::imwrite(output_path, display);
    std::printf("\n  Visualization saved to: %s\n", output_path.c_str());

    if (!results.empty()) {
        Detection best = results[0];
        for (const auto& det : results) {
            if (det.confidence > best.confidence) best = det;
        }
        std::printf("\n  Best detection: class=%d, conf=%.3f, w=%.1f, h=%.1f\n",
                    best.class_id, best.confidence, best.w, best.h);
        if (best.w > 0 && best.h > 0) {
            std::printf("  ✓ Model produced valid detections\n");
        } else {
            std::printf("  ✗ Detection has zero width/height\n");
        }
    } else {
        std::printf("\n  ✗ No detections found (check conf_threshold or model)\n");
    }

    std::printf("\n=== detector_test done ===\n");
    return 0;
}