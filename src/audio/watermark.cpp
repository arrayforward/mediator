#include "audio/watermark.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mediator::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::vector<double> MakeChirp(const WatermarkConfig& cfg) {
    const int n = cfg.chirp_ms * cfg.sample_rate / 1000;
    std::vector<double> chirp(n);
    const double t_total = static_cast<double>(n) / cfg.sample_rate;
    const double k = (cfg.chirp_f1 - cfg.chirp_f0) / t_total; // 线性扫频斜率
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / cfg.sample_rate;
        const double phase = 2.0 * kPi * (cfg.chirp_f0 * t + 0.5 * k * t * t);
        // 汉宁窗包络，降低端点不连续
        const double win = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (n - 1)));
        chirp[i] = cfg.amplitude * win * std::sin(phase);
    }
    return chirp;
}
} // namespace

std::vector<int16_t> GenerateWatermark(const WatermarkConfig& cfg) {
    const auto chirp = MakeChirp(cfg);
    const int gap_samples = cfg.gap_ms * cfg.sample_rate / 1000; // 起点到起点
    const int count = std::max(cfg.chirp_count, 1);
    const int total = (count - 1) * gap_samples + static_cast<int>(chirp.size());
    std::vector<int16_t> out(total, 0);
    // 多脉冲等间隔重复：检测端识别任意相邻双峰即可，冗余提高真机回环命中率
    for (int k = 0; k < count; ++k)
        for (size_t i = 0; i < chirp.size(); ++i)
            out[k * gap_samples + i] = static_cast<int16_t>(chirp[i] * 32767.0);
    return out;
}

WatermarkDetectResult DetectWatermark(const std::vector<int16_t>& capture,
                                      const WatermarkConfig& cfg) {
    WatermarkDetectResult res;
    const auto chirp = MakeChirp(cfg);
    const int m = static_cast<int>(chirp.size());
    const int n = static_cast<int>(capture.size());
    if (n < m * 2) return res;

    // 模板能量（零均值）
    double mean_t = std::accumulate(chirp.begin(), chirp.end(), 0.0) / m;
    double e_t = 0.0;
    std::vector<double> t0(m);
    for (int i = 0; i < m; ++i) {
        t0[i] = chirp[i] - mean_t;
        e_t += t0[i] * t0[i];
    }
    if (e_t <= 0.0) return res;

    // 第一遍：全量滑动 NCC
    const int span = n - m;
    std::vector<double> ncc(span + 1, 0.0);
    double best = 0.0;
    for (int s = 0; s <= span; ++s) {
        double e_x = 0.0, dot = 0.0, mean_x = 0.0;
        for (int i = 0; i < m; ++i) mean_x += capture[s + i];
        mean_x /= m;
        for (int i = 0; i < m; ++i) {
            const double x = capture[s + i] - mean_x;
            e_x += x * x;
            dot += x * t0[i];
        }
        if (e_x <= 0.0) continue;
        ncc[s] = dot / std::sqrt(e_x * e_t);
        if (ncc[s] > best) best = ncc[s];
    }
    res.peak_ncc = best;

    // 第二遍：局部最大值 + 非极大抑制（窗口 m），抛物线插值得到亚采样峰位
    // —— 直接取"过阈值点"会落在上升沿；且 200ppm 漂移在间隔上不足 1 个采样，
    //    必须亚采样插值才能达到设计文档 <20ppm 的精度要求。
    struct Peak { double pos; double v; };
    std::vector<Peak> peaks;
    for (int s = 0; s <= span; ++s) { // 注意覆盖边界：水印可能正好在缓冲起点
        if (ncc[s] < cfg.ncc_threshold) continue;
        const double prev = (s > 0) ? ncc[s - 1] : -1.0;
        const double next = (s < span) ? ncc[s + 1] : -1.0;
        if (ncc[s] < prev || ncc[s] < next) continue; // 局部最大
        // 抛物线插值（仅内部点）：顶点 x = 0.5*(y-1 - y+1)/(y-1 - 2y0 + y+1)
        double off = 0.0;
        if (s > 0 && s < span) {
            const double denom = ncc[s - 1] - 2.0 * ncc[s] + ncc[s + 1];
            if (denom != 0.0) off = 0.5 * (ncc[s - 1] - ncc[s + 1]) / denom;
        }
        if (!peaks.empty() && s - peaks.back().pos < m) { // NMS：留更高者
            if (ncc[s] > peaks.back().v) peaks.back() = {s + off, ncc[s]};
            continue;
        }
        peaks.push_back({s + off, ncc[s]});
    }
    if (peaks.size() < 2) return res;
    res.debug_peaks = static_cast<int>(peaks.size());

    const int expect = cfg.gap_ms * cfg.sample_rate / 1000;
    const int tol = cfg.interval_tolerance_ms * cfg.sample_rate / 1000;
    // 多脉冲一致性投票（真机误锁回归修复）：旧策略取"首个间隔≈GAP 的相邻
    // 峰对"，8 脉冲冗余下部分脉冲畸变（扬声器 AGC 爬坡/抢话/回声伪峰）时会
    // 锁到偏移的脉冲子序列——delay 离群（实测 9503 vs 正常 ~4229）导致 AEC
    // 延迟线错位、语音被消。现改为：以每个峰为候选首脉冲，在其栅格
    // p+k*GAP（k=0..count-1）上统计命中峰数，要求命中 ≥ count-2（允许至多
    // 2 个脉冲缺失/畸变）且最深命中槽 ≥ count-2（尾槽或次尾槽在位）——
    // 偏移子序列的最深槽到不了 count-2，仍被拒；尾槽在位要求同时把检测触发
    // 点压到水印末尾（尾脉冲真机不稳定，放宽到次尾槽后残留尾巴 ≤1 脉冲，
    // 不会毒化 ASR 流——sim 回归根因）。
    const int count = std::max(cfg.chirp_count, 2);
    const int need = std::max(2, count - 2);
    int best_matched = 0, best_last_k = -1;
    double best_p1 = 0.0;
    std::vector<double> best_slots;
    for (size_t c = 0; c < peaks.size(); ++c) {
        std::vector<double> slots(count, -1.0);
        int matched = 0, last_k = -1;
        for (const auto& pk : peaks) {
            const double rel = pk.pos - peaks[c].pos;
            const long k = std::lround(rel / expect);
            if (k < 0 || k >= count) continue;
            if (std::abs(rel - static_cast<double>(k) * expect) > tol) continue;
            if (slots[k] < 0.0) {
                slots[k] = pk.pos;
                ++matched;
                if (k > last_k) last_k = static_cast<int>(k);
            }
        }
        if (matched > best_matched ||
            (matched == best_matched && last_k > best_last_k)) {
            best_matched = matched;
            best_last_k = last_k;
            best_p1 = peaks[c].pos;
            best_slots = std::move(slots);
        }
    }
    res.debug_matched_slots = best_matched;
    if (best_matched < need || best_last_k < count - 2) return res;

    // skew：相邻命中槽间隔序列取中位数（抗单个脉冲畸变/伪峰）
    std::vector<double> iv;
    for (int k = 0; k + 1 < count; ++k)
        if (best_slots[k] >= 0.0 && best_slots[k + 1] >= 0.0)
            iv.push_back(best_slots[k + 1] - best_slots[k]);
    double med = expect;
    if (!iv.empty()) {
        std::sort(iv.begin(), iv.end());
        med = iv[iv.size() / 2];
    }
    res.detected = true;
    res.p1 = static_cast<int64_t>(std::llround(best_p1));
    res.p2 = static_cast<int64_t>(
        std::llround(best_slots.size() > 1 && best_slots[1] >= 0.0 ? best_slots[1]
                                                                   : best_p1 + expect));
    res.skew = med / expect - 1.0;
    return res;
}

CalibResult ComputeCalib(const WatermarkDetectResult& det, int64_t play_start_sample,
                         const WatermarkConfig& cfg) {
    (void)cfg;
    CalibResult r{};
    r.delay_samples = static_cast<int32_t>(det.p1 - play_start_sample);
    r.skew = det.skew;
    return r;
}

} // namespace mediator::audio
