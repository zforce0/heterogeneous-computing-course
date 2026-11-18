// JSONL 事件流: 手写序列化(无第三方依赖), 每个被 YOLO 确认的提名一行
// 字段契约见实现方案 §8.1; 纪律: 不用 confidence 描述自身输出,
// score 只出现在 semantic 内部且标 filled_by

#include "eventlog.h"

#include <cstdio>
#include <cmath>

bool EventLog::open(const std::string& path)
{
    file_ = std::fopen(path.c_str(), "a");   // append, 不覆盖
    if (file_ == nullptr) {
        std::fprintf(stderr, "EventLog: cannot open %s\n", path.c_str());
        return false;
    }
    return true;
}

void EventLog::close()
{
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

// NaN/Inf 安全的浮点输出: 非有限值输出 null
static void jfloat(FILE* f, double v)
{
    if (std::isfinite(v)) {
        std::fprintf(f, "%.2f", v);
    } else {
        std::fprintf(f, "null");
    }
}

void EventLog::write(const Nomination& nom, float ir_temp_median,
                     int q1_count, int q2_count, int q3_count, int vis_w,
                     int vis_h)
{
    if (file_ == nullptr) {
        return;
    }
    // 时间戳用墙钟毫秒(事件流供外部消费, 单调钟对外无意义)
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    const long long wall_ms_real =
        static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;

    std::fprintf(file_,
                 "{\"v\":1,\"ts_ms\":%lld,\"id\":%llu,\"quadrant\":%d,"
                 "\"track\":%d,",
                 wall_ms_real,
                 static_cast<unsigned long long>(nom.id),
                 nom.quadrant, nom.track_id);
    std::fprintf(file_,
                 "\"ir_box\":[%d,%d,%d,%d],\"area_px\":%d,"
                 "\"centroid\":[%.1f,%.1f],",
                 nom.box_ir.x, nom.box_ir.y, nom.box_ir.width,
                 nom.box_ir.height, nom.area_px, nom.centroid_ir.x,
                 nom.centroid_ir.y);
    // 温度对象(情形 A, 辐射测温可用)
    std::fprintf(file_, "\"temp\":{\"t_max\":");
    jfloat(file_, nom.t_max);
    std::fprintf(file_, ",\"t_p99\":");
    jfloat(file_, nom.t_p99);
    std::fprintf(file_, ",\"t_ref\":");
    jfloat(file_, nom.t_ref);
    std::fprintf(file_, ",\"delta_t\":");
    jfloat(file_, nom.delta_t);
    std::fprintf(file_, ",\"t_lower_bound\":");
    jfloat(file_, nom.t_lower_bound);
    std::fprintf(file_, ",\"t_true_est\":");
    jfloat(file_, nom.t_true_est);
    std::fprintf(file_, ",\"gradient\":");
    jfloat(file_, nom.thermal_gradient);
    std::fprintf(file_, ",\"unit\":\"C\",\"radiometric\":true},");
    std::fprintf(file_,
                 "\"saliency\":{\"tir\":%.3f,\"rgb\":%.3f,\"diff\":%.3f},",
                 nom.sal_tir_mean, nom.sal_rgb_mean, nom.sal_diff);
    std::fprintf(file_,
                 "\"temporal\":{\"persistence_s\":%.2f,\"dT_dt\":%.3f,"
                 "\"t_std\":%.3f,\"micro_motion\":%.3f,"
                 "\"score_living\":%.2f},",
                 nom.persistence_s, nom.dT_dt, nom.t_std, nom.micro_motion,
                 nom.score_living);
    std::fprintf(file_,
                 "\"health\":{\"scene_contrast_k\":%.2f,"
                 "\"crossover_risk\":%d,\"valid\":%d,\"reason\":\"%s\"},",
                 nom.scene_contrast_k, nom.crossover_risk, nom.valid,
                 nom.invalid_reason.empty() ? "null" 
                                            : nom.invalid_reason.c_str());
    std::fprintf(file_,
                 "\"scene\":{\"ir_temp_median\":%.1f,\"q1_count\":%d,"
                 "\"q2_count\":%d,\"q3_count\":%d},",
                 ir_temp_median, q1_count, q2_count, q3_count);
    std::fprintf(file_,
                 "\"semantic\":{\"label\":\"%s\",\"score\":%.2f,"
                 "\"filled_by\":\"%s\"},",
                 nom.semantic.label.c_str(), nom.semantic.score,
                 nom.semantic.filled_by.c_str());
    std::fprintf(file_,
                 "\"roi_vis\":[%d,%d,%d,%d],\"vis_res\":[%d,%d]}\n",
                 nom.roi_vis.x, nom.roi_vis.y, nom.roi_vis.width,
                 nom.roi_vis.height, vis_w, vis_h);
    ++count_;
    std::fflush(file_);
}
