// 提名器实现: 连通域 → 属性包(温度/显著性) → 时序链接 → 活体打分
// 参见实现方案 §4.5 / §4.7

#include "nominator.h"
#include "registration.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

// 同轨迹提名冷却(ms): 稳态热源 5 秒内不重复送 NPU 确认
static const double kNominateCooldownMs = 5000.0;

void Nominator::init(const NominatorConfig& cfg)
{
    cfg_ = cfg;
}

int Nominator::link_track(const cv::Point2f& centroid, double t_now_ms,
                          uint64_t ts_us)
{
    // 质心最近邻链接(IR 网格上距离 < link_radius)
    int best = -1;
    float best_d = static_cast<float>(cfg_.link_radius);
    const double kMaxGapMs = 1500.0;   // 超过 1.5s 未更新视为断链
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (t_now_ms - tracks_[i].last_update_ms > kMaxGapMs) {
            continue;
        }
        const float d = static_cast<float>(std::sqrt(
            (tracks_[i].centroid.x - centroid.x) *
                (tracks_[i].centroid.x - centroid.x) +
            (tracks_[i].centroid.y - centroid.y) *
                (tracks_[i].centroid.y - centroid.y)));
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        tracks_[best].centroid = centroid;
        tracks_[best].last_ts_us = ts_us;
        tracks_[best].last_update_ms = t_now_ms;
        return tracks_[best].id;
    }
    Track t;
    t.id = next_track_id_++;
    t.centroid = centroid;
    t.last_ts_us = ts_us;
    t.first_ms = t_now_ms;
    t.last_update_ms = t_now_ms;
    tracks_.push_back(t);
    // 轨迹表过期清理
    if (tracks_.size() > 64) {
        std::vector<Track> keep;
        keep.reserve(tracks_.size());
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (t_now_ms - tracks_[i].last_update_ms < kMaxGapMs) {
                keep.push_back(tracks_[i]);
            }
        }
        tracks_.swap(keep);
    }
    return t.id;
}

void Nominator::extract_components(
    const cv::Mat& mask, int quadrant, const Saliency::Output& sal,
    const cv::Mat& T_c, const cv::Mat& G_small, const cv::Mat& G_prev,
    const Registration& reg, uint64_t ts_pair_us, uint64_t ts_vis_us,
    double t_now_ms, std::vector<Nomination>& out)
{
    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(
        mask, labels, stats, centroids, 8, CV_32S);
    for (int i = 1; i < n; ++i) {   // 0 = 背景
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < cfg_.min_area) {
            continue;
        }
        const int x = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int y = stats.at<int>(i, cv::CC_STAT_TOP);
        const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const float cx = centroids.at<double>(i, 0);
        const float cy = centroids.at<double>(i, 1);

        Nomination nom;
        nom.quadrant = quadrant;
        nom.box_ir = cv::Rect(x, y, w, h);
        nom.centroid_ir = cv::Point2f(cx, cy);
        nom.area_px = area;
        nom.roi_vis = reg.box_ir_to_roi_vis(nom.box_ir);
        nom.ts_pair_us = ts_pair_us;
        nom.ts_vis_us = ts_vis_us;
        nom.t_emit_ms = t_now_ms;
        nom.scene_contrast_k = sal.scene_contrast_k;
        nom.crossover_risk =
            sal.scene_contrast_k < 1.5 ? 2 : (sal.scene_contrast_k < 3.0 ? 1 : 0);

        // ---- 温度属性(区域内/外环) ----
        {
            std::vector<float> inner;
            inner.reserve(static_cast<size_t>(area));
            // 外环: bbox 外扩 50% 的矩形环
            const int ex = x - w / 2, ey = y - h / 2;
            const int ew = w * 2, eh = h * 2;
            const int x0 = std::max(0, ex), y0 = std::max(0, ey);
            const int x1 = std::min(T_c.cols, ex + ew);
            const int y1 = std::min(T_c.rows, ey + eh);
            std::vector<float> outer;
            outer.reserve(static_cast<size_t>((x1 - x0) * (y1 - y0)));
            for (int r = y0; r < y1; ++r) {
                const float* trow = T_c.ptr<float>(r);
                const bool row_inner = (r >= y && r < y + h);
                const int32_t* lrow =
                    row_inner ? labels.ptr<int32_t>(r) : nullptr;
                for (int c = x0; c < x1; ++c) {
                    const bool inside = row_inner && c >= x && c < x + w &&
                                        lrow[c] == i;
                    if (inside) {
                        inner.push_back(trow[c]);
                    } else {
                        outer.push_back(trow[c]);
                    }
                }
            }
            if (!inner.empty()) {
                std::sort(inner.begin(), inner.end());
                nom.t_max = inner.back();
                nom.t_p99 = inner[static_cast<size_t>(
                    static_cast<double>(inner.size() - 1) * 0.99)];
                // thermal_gradient: 区内 P99-P1 / bbox 对角长(K/px, 近似)
                const float p1 = inner.front();
                const float diag = static_cast<float>(
                    std::sqrt((double)w * w + (double)h * h));
                nom.thermal_gradient =
                    (nom.t_p99 - p1) / std::max(diag, 1.0f);
            }
            if (!outer.empty()) {
                std::sort(outer.begin(), outer.end());
                nom.t_ref = outer[outer.size() / 2];   // 中位数
            }
            nom.delta_t = nom.t_max - nom.t_ref;
            nom.t_lower_bound = nom.t_max;   // 读数只会偏低不会偏高
            // SSE 保守修正(§4.5.1): 目标占像素越少低估越严重
            float f = static_cast<float>(area) / 25.0f;
            f = std::max(0.5f, std::min(1.0f, f));
            nom.t_true_est = nom.t_ref + (nom.t_max - nom.t_ref) / f;
        }

        // ---- 显著性均值(bbox 内) ----
        {
            double st = 0.0, sr = 0.0;
            const cv::Rect box = nom.box_ir &
                                 cv::Rect(0, 0, sal.sn_tir.cols,
                                          sal.sn_tir.rows);
            if (box.area() > 0) {
                cv::Scalar mt = cv::mean(sal.sn_tir(box));
                cv::Scalar mr = cv::mean(sal.sn_rgb(box));
                st = mt[0];
                sr = mr[0];
            }
            nom.sal_tir_mean = static_cast<float>(st);
            nom.sal_rgb_mean = static_cast<float>(sr);
            nom.sal_diff = nom.sal_tir_mean - nom.sal_rgb_mean;
        }

        // ---- 时序: 链接轨迹 + 滑动统计 ----
        {
            nom.track_id = link_track(nom.centroid_ir, t_now_ms,
                                      ts_pair_us);
            Track* tr = nullptr;
            for (size_t k = 0; k < tracks_.size(); ++k) {
                if (tracks_[k].id == nom.track_id) {
                    tr = &tracks_[k];
                    break;
                }
            }
            if (tr != nullptr) {
                tr->t_history.push_back(nom.t_max);
                while (tr->t_history.size() >
                       static_cast<size_t>(cfg_.history_len)) {
                    tr->t_history.pop_front();
                }
                nom.persistence_s =
                    static_cast<float>((t_now_ms - tr->first_ms) / 1000.0);
                if (tr->t_history.size() >= 8) {
                    // dT_dt: 滑动线性拟合斜率(K/s → K/min)
                    const size_t m = tr->t_history.size();
                    const double dt = (m - 1) / 25.0;   // 假设 25fps 采样
                    double sx = 0, sy = 0, sxx = 0, sxy = 0;
                    for (size_t k = 0; k < m; ++k) {
                        const double tk = k / 25.0;
                        const double yk = tr->t_history[k];
                        sx += tk; sy += yk; sxx += tk * tk; sxy += tk * yk;
                    }
                    const double denom = m * sxx - sx * sx;
                    if (std::fabs(denom) > 1.0e-9 && dt > 0) {
                        nom.dT_dt = static_cast<float>(
                            (m * sxy - sx * sy) / denom * 60.0);
                    }
                    // t_std
                    const double mean = sy / m;
                    double var = 0.0;
                    for (size_t k = 0; k < m; ++k) {
                        const double d = tr->t_history[k] - mean;
                        var += d * d;
                    }
                    nom.t_std = static_cast<float>(std::sqrt(var / m));
                }
            }
        }

        // ---- micro_motion: VIS 灰度帧间差(bbox 内均值) ----
        if (!G_prev.empty() && G_small.size() == G_prev.size()) {
            cv::Mat diff;
            cv::absdiff(G_small(nom.box_ir), G_prev(nom.box_ir), diff);
            cv::Scalar m = cv::mean(diff);
            nom.micro_motion = static_cast<float>(m[0]);
        }

        // ---- 活体打分(yard 模式显示用) ----
        {
            float score = 1.0f;
            bool in_band = nom.t_max >= cfg_.body_lo &&
                           nom.t_max <= cfg_.body_hi;
            score *= in_band ? 1.0f : 0.0f;
            score *= (std::fabs(nom.dT_dt) < cfg_.dt_thr) ? 1.0f : 0.0f;
            const bool alive = nom.micro_motion > cfg_.motion_thr ||
                               nom.t_std > cfg_.tstd_thr;
            score *= alive ? 1.0f : 0.0f;
            nom.score_living = score;
        }

        nom.id = next_id_++;

        // 同轨迹节流: 稳态热源每帧都会命中显著层, 若不节流,
        // 提名队列被同一目标塞满, NPU(85ms/个)消化不动 → 积压/丢最旧。
        // 统计计数(q1/q1_px)在节流前已累加, FAR 口径不受影响。
        bool throttled = false;
        for (size_t k = 0; k < tracks_.size(); ++k) {
            if (tracks_[k].id == nom.track_id) {
                if (t_now_ms - tracks_[k].last_nominate_ms <
                    kNominateCooldownMs) {
                    throttled = true;
                } else {
                    tracks_[k].last_nominate_ms = t_now_ms;
                }
                break;
            }
        }
        if (!throttled) {
            out.push_back(nom);
        }
    }
}

void Nominator::process(const Saliency::Output& sal, const cv::Mat& T_c,
                        const cv::Mat& G_small, const cv::Mat& G_prev,
                        const Registration& reg, uint64_t ts_pair_us,
                        uint64_t ts_vis_us, double t_now_ms,
                        std::vector<Nomination>& out)
{
    if (!sal.valid) {
        return;
    }
    // 像素口径累计(FAR 判决用; 包含关系 q1_px ⊆ tir_px 恒成立)
    stats_.tir_px += static_cast<uint64_t>(sal.tir_px);
    stats_.q1_px += static_cast<uint64_t>(sal.q1_px);
    // 单模态热提名基线(FAR 分子): mask_tir 的连通域数(面积过滤后)
    {
        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(
            sal.mask_tir, labels, stats, centroids, 8, CV_32S);
        for (int i = 1; i < n; ++i) {
            if (stats.at<int>(i, cv::CC_STAT_AREA) >= cfg_.min_area) {
                ++stats_.tir_only;
            }
        }
    }

    // 象限像素计数 → 组件计数(q3 只计数不提名, §4.4 第③象限纪律)
    {
        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(
            sal.mask_q3, labels, stats, centroids, 8, CV_32S);
        for (int i = 1; i < n; ++i) {
            if (stats.at<int>(i, cv::CC_STAT_AREA) >= cfg_.min_area) {
                ++stats_.q3;
            }
        }
    }

    if (cfg_.card == "inspect" || cfg_.card == "all") {
        std::vector<Nomination> q1s;
        extract_components(sal.mask_q1, 1, sal, T_c, G_small, G_prev, reg,
                           ts_pair_us, ts_vis_us, t_now_ms, q1s);
        stats_.q1 += q1s.size();
        out.insert(out.end(), q1s.begin(), q1s.end());
    }
    if (cfg_.card == "yard" || cfg_.card == "all") {
        std::vector<Nomination> q2s;
        extract_components(sal.mask_q2, 2, sal, T_c, G_small, G_prev, reg,
                           ts_pair_us, ts_vis_us, t_now_ms, q2s);
        stats_.q2 += q2s.size();
        // yard 模式只推进活体得分高的提名; all 模式全部推进
        for (size_t i = 0; i < q2s.size(); ++i) {
            if (cfg_.card == "all" || q2s[i].score_living > 0.0f) {
                out.push_back(q2s[i]);
            }
        }
    }
}
