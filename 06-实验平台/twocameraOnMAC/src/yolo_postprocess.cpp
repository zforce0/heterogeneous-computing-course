#include "yolo_postprocess.h"
#include "frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

static float compute_iou(const float* a, const float* b)
{
    float x1 = std::max(a[0], b[0]);
    float y1 = std::max(a[1], b[1]);
    float x2 = std::min(a[2], b[2]);
    float y2 = std::min(a[3], b[3]);

    float inter_w = x2 - x1;
    float inter_h = y2 - y1;
    if (inter_w <= 0 || inter_h <= 0) return 0.0f;

    float inter_area = inter_w * inter_h;
    float area_a = (a[2] - a[0]) * (a[3] - a[1]);
    float area_b = (b[2] - b[0]) * (b[3] - b[1]);

    return inter_area / (area_a + area_b - inter_area + 1e-6f);
}

static void nms(std::vector<float>& boxes,
                 std::vector<float>& scores,
                 float iou_threshold,
                 std::vector<int>& keep_indices)
{
    keep_indices.clear();
    if (scores.empty()) return;

    const int n = static_cast<int>(scores.size());
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;

    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return scores[a] > scores[b];
    });

    std::vector<int> suppressed(n, 0);

    for (int idx = 0; idx < n; ++idx) {
        int i = order[idx];
        if (suppressed[i] == 1) continue;
        keep_indices.push_back(i);

        for (int j = idx + 1; j < n; ++j) {
            int k = order[j];
            if (suppressed[k] == 1) continue;

            if (compute_iou(&boxes[i * 4], &boxes[k * 4]) > iou_threshold) {
                suppressed[k] = 1;
            }
        }
    }
}

std::vector<Detection> yolo_postprocess(const float* output,
                                        int batch,
                                        int nc_plus_4,
                                        int num_anchors,
                                        float conf_threshold,
                                        float iou_threshold)
{
    std::vector<Detection> results;
    if (batch <= 0 || nc_plus_4 < 5 || num_anchors <= 0) return results;

    std::vector<float> boxes;
    std::vector<float> scores;

    int num_classes = nc_plus_4 - 4;

    for (int b = 0; b < batch; ++b) {
        for (int a = 0; a < num_anchors; ++a) {
            int offset = b * nc_plus_4 * num_anchors + a;
            float cx = output[offset];
            float cy = output[offset + num_anchors];
            float w = output[offset + 2 * num_anchors];
            float h = output[offset + 3 * num_anchors];

            float max_score = -1.0f;
            for (int c = 0; c < num_classes; ++c) {
                float score = output[offset + (4 + c) * num_anchors];
                if (score > max_score) {
                    max_score = score;
                }
            }

            if (max_score > conf_threshold) {
                float x1 = cx - w * 0.5f;
                float y1 = cy - h * 0.5f;
                float x2 = cx + w * 0.5f;
                float y2 = cy + h * 0.5f;

                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(x2);
                boxes.push_back(y2);
                scores.push_back(max_score);
            }
        }
    }

    std::vector<int> keep_indices;
    nms(boxes, scores, iou_threshold, keep_indices);

    results.reserve(keep_indices.size());
    for (int idx : keep_indices) {
        int base = idx * 4;
        float x1 = boxes[base];
        float y1 = boxes[base + 1];
        float x2 = boxes[base + 2];
        float y2 = boxes[base + 3];

        float bx = x1;
        float by = y1;
        float bw = x2 - x1;
        float bh = y2 - y1;

        Detection det;
        det.x = bx;
        det.y = by;
        det.w = bw;
        det.h = bh;
        det.confidence = scores[idx];
        det.class_id = 0;
        results.push_back(det);
    }

    return results;
}

/* 在 BgrFrame 上绘制一个矩形边框 (纯像素操作, 不依赖 OpenCV) */
static void draw_rect(BgrFrame& frame, int x1, int y1,
                      int x2, int y2, int box_thickness)
{
    if (box_thickness <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride;
    uint8_t* data = frame.data.empty()
                        ? nullptr
                        : frame.data.data();
    if (!data) return;

    x1 = std::max(0, std::min(x1, w - 1));
    y1 = std::max(0, std::min(y1, h - 1));
    x2 = std::max(0, std::min(x2, w - 1));
    y2 = std::max(0, std::min(y2, h - 1));
    if (x2 < x1 || y2 < y1) return;

    /* 绿色边框 (BGR) */
    const uint8_t color[3] = {0, 255, 0};

    auto draw_horizontal = [&](int y, int x_start, int x_end) {
        for (int x = x_start; x <= x_end; ++x) {
            for (int t = 0; t < box_thickness; ++t) {
                const int yy = std::min(y + t, h - 1);
                const int idx = yy * stride + x * 3;
                data[idx] = color[0];
                data[idx + 1] = color[1];
                data[idx + 2] = color[2];
            }
        }
    };

    auto draw_vertical = [&](int x, int y_start, int y_end) {
        for (int y = y_start; y <= y_end; ++y) {
            for (int t = 0; t < box_thickness; ++t) {
                const int xx = std::min(x + t, w - 1);
                const int idx = y * stride + xx * 3;
                data[idx] = color[0];
                data[idx + 1] = color[1];
                data[idx + 2] = color[2];
            }
        }
    };

    draw_horizontal(y1, x1, x2);
    draw_horizontal(y2, x1, x2);
    draw_vertical(x1, y1, y2);
    draw_vertical(x2, y1, y2);
}

void draw_detections(const BgrFrame& frame,
                     const std::vector<Detection>& detections,
                     int box_thickness)
{
    if (detections.empty()) return;

    for (const auto& det : detections) {
        int x1 = static_cast<int>(det.x);
        int y1 = static_cast<int>(det.y);
        int x2 = static_cast<int>(det.x + det.w);
        int y2 = static_cast<int>(det.y + det.h);
        draw_rect(const_cast<BgrFrame&>(frame),
                  x1, y1, x2, y2, box_thickness);
    }
}

void draw_detections_in_region(const BgrFrame& frame,
                               const std::vector<Detection>& detections,
                               int model_size,
                               int region_x, int region_y,
                               int region_width, int region_height,
                               int box_thickness)
{
    if (detections.empty()) return;
    if (model_size <= 0 || region_width <= 0 || region_height <= 0) {
        return;
    }

    const float scale_x =
        static_cast<float>(region_width) / model_size;
    const float scale_y =
        static_cast<float>(region_height) / model_size;

    for (const auto& det : detections) {
        /* 640×640 坐标 → 子画面坐标 (camerayolo.md 5.7) */
        const int x1 = region_x + static_cast<int>(det.x * scale_x);
        const int y1 = region_y + static_cast<int>(det.y * scale_y);
        const int x2 = region_x +
            static_cast<int>((det.x + det.w) * scale_x);
        const int y2 = region_y +
            static_cast<int>((det.y + det.h) * scale_y);
        draw_rect(const_cast<BgrFrame&>(frame),
                  x1, y1, x2, y2, box_thickness);
    }
}