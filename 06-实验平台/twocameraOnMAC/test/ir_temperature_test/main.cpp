/*
 * ir_temperature_test/main.cpp — 红外测温功能验证程序
 *
 * 目的:
 *   确认 twocameraOnMAC 中 IrCamera 在 IMAGE_AND_TEMP_OUTPUT 模式下,
 *   能否成功获取 256×192 的温度数据图,并把原始 uint16_t (kelvin*16)
 *   转换成摄氏度输出。
 *
 * 复用 src/ir_camera.cpp + src/temperature.cpp, 不开可见光, 不合成显示。
 *
 * 用法:
 *   ./ir_temperature_test [stream_seconds]
 *     stream_seconds: 采集时长(秒), 默认 30
 *
 * 输出:
 *   - IR camera 打开/启动日志 (SDK 自带)
 *   - 每秒一次的温度帧统计: 尺寸/原始范围/摄氏范围/中心点温度/全帧统计
 *   - 退出时汇总: 总帧数、有温度帧数
 */

#include "ir_camera.h"
#include "temperature.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

/* 把 uint16_t 温度原始值 (kelvin*16) 转为摄氏度 */
static float to_celsius(uint16_t raw)
{
    return static_cast<float>(raw) / 64.0f - 273.15f;
}

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    int stream_seconds = 30;
    if (argc > 1) {
        stream_seconds = std::atoi(argv[1]);
        if (stream_seconds <= 0) stream_seconds = 30;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("=== ir_temperature_test start, duration=%ds ===\n",
                stream_seconds);

    /* 1. 打开红外相机 (IrCameraConfig 默认 IMAGE_AND_TEMP_OUTPUT) */
    IrCameraConfig config;
    config.vendor_id = 0x0BDA;
    config.product_id = 0x5840;
    /* stream_mode 默认 ImageAndTemperature, 由 sample_config.h 控制 */

    IrCamera infrared;
    std::fprintf(stderr, "[STEP] opening IR camera...\n");
    if (!infrared.open(config)) {
        std::fprintf(stderr, "Failed to open IR camera\n");
        return 1;
    }
    std::fprintf(stderr,
                "[STEP] IR opened: %dx%d @ %dfps\n",
                infrared.width(), infrared.height(), infrared.fps());

    /* 2. 启动红外流 */
    if (!infrared.start()) {
        std::fprintf(stderr, "Failed to start IR stream\n");
        return 1;
    }
    std::fprintf(stderr,
                "[STEP] IR stream started, expected temp frame: %dx%d uint16_t\n",
                infrared.width(), infrared.height());

    /* 3. 采集循环: 每秒打印一次温度统计 */
    const auto start = std::chrono::steady_clock::now();
    auto next_report = start + std::chrono::seconds(1);

    uint64_t total_frames = 0;
    uint64_t temp_frames = 0;
    uint64_t last_sequence = 0;
    bool first_frame_printed = false;

    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        if (now - start >= std::chrono::seconds(stream_seconds)) {
            break;
        }

        /* 取最新温度帧 */
        TemperatureFrame frame;
        if (infrared.copy_latest_temperature(frame)) {
            ++total_frames;
            if (frame.sequence != last_sequence) {
                last_sequence = frame.sequence;
                ++temp_frames;
            }

            /* 第一帧: 详细打印, 确认尺寸 = 256*192 */
            if (!first_frame_printed) {
                first_frame_printed = true;
                std::printf("[FIRST] temp frame: width=%d height=%d "
                            "data_size=%zu uint16_t, sequence=%llu\n",
                            frame.width, frame.height,
                            frame.data.size(),
                            static_cast<unsigned long long>(
                                frame.sequence));

                /* 关键验证: 尺寸必须是 256*192 */
                const int expected_w = 256;
                const int expected_h = 192;
                if (frame.width != expected_w ||
                    frame.height != expected_h) {
                    std::fprintf(stderr,
                                "[ERROR] unexpected temp frame size: "
                                "%dx%d, expected %dx%d\n",
                                frame.width, frame.height,
                                expected_w, expected_h);
                } else {
                    std::printf("[OK] temperature frame size = %dx%d "
                                "(=%d pixels, %zu bytes)\n",
                                frame.width, frame.height,
                                frame.width * frame.height,
                                frame.data.size() * sizeof(uint16_t));
                }
            }

            /* 每秒一次统计 */
            if (now >= next_report) {
                next_report = now + std::chrono::seconds(1);

                /* 扫一遍 min/max/sum (原始值, kelvin*16) */
                uint16_t raw_min = 0xFFFF;
                uint16_t raw_max = 0;
                double raw_sum = 0.0;
                for (size_t i = 0; i < frame.data.size(); ++i) {
                    const uint16_t v = frame.data[i];
                    if (v < raw_min) raw_min = v;
                    if (v > raw_max) raw_max = v;
                    raw_sum += static_cast<double>(v);
                }
                const double raw_avg =
                    frame.data.empty() ? 0.0
                                       : raw_sum / frame.data.size();

                const float c_min = to_celsius(raw_min);
                const float c_max = to_celsius(raw_max);
                const float c_avg = static_cast<float>(
                    raw_avg / 64.0 - 273.15);

                /* 中心点温度 (用 libirtemp 的 get_point_temp) */
                const int cx = frame.width / 2;
                const int cy = frame.height / 2;
                float center_c = 0.0f;
                bool center_ok = point_temperature(frame, cx, cy,
                                                   center_c);

                /* 全帧温度统计 (libirtemp 的 get_rect_temp) */
                TempInfo_t full_info = {0};
                bool full_ok = full_frame_temperature(frame,
                                                      full_info);

                std::printf("[STAT] seq=%llu raw[%u..%u] avg=%u"
                            "  Celsius[%.2f..%.2f] avg=%.2f"
                            "  center(%d,%d)=%.2fC (%s)"
                            "  full: min=%.2fC max=%.2fC avg=%.2fC (%s)\n",
                            static_cast<unsigned long long>(
                                frame.sequence),
                            raw_min, raw_max,
                            static_cast<unsigned>(raw_avg),
                            c_min, c_max, c_avg,
                            cx, cy,
                            center_ok ? center_c : -999.0f,
                            center_ok ? "ok" : "FAIL",
                            full_ok ? to_celsius(full_info.min_temp) : -999.0f,
                            full_ok ? to_celsius(full_info.max_temp) : -999.0f,
                            full_ok ? to_celsius(full_info.avr_temp) : -999.0f,
                            full_ok ? "ok" : "FAIL");
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
    }

    /* 4. 退出前再做一次整体汇总 */
    std::printf("=== ir_temperature_test end ===\n");
    std::printf("total copy_latest_temperature calls: %llu\n",
                static_cast<unsigned long long>(total_frames));
    std::printf("distinct temperature sequences: %llu\n",
                static_cast<unsigned long long>(temp_frames));
    if (temp_frames == 0) {
        std::fprintf(stderr,
                    "[FAIL] no temperature frames captured — "
                    "check IR camera stream/permissions\n");
        infrared.close();
        return 2;
    }
    std::printf("[PASS] temperature pipeline is working: "
                "%llu frames received\n",
                static_cast<unsigned long long>(temp_frames));

    infrared.close();
    return 0;
}
