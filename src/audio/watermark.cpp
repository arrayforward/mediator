#include "audio/watermark.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mediator::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;

// 单音：线性扫频 + 5ms 起音斜坡 + 指数衰减（门铃听感，端点无爆音）。
// 扫频使信号非平稳——手机本地 NS/AEC 专杀平稳纯音，扫频音穿过后
// 仍保持高自相关（NCC 峰尖锐）
void AppendSweep(std::vector<double>& out, double f0, double f1, int ms,
                 double tau_ms, double amplitude, int sample_rate) {
    const int n = ms * sample_rate / 1000;
    const int attack = std::min(n, 5 * sample_rate / 1000);
    const double tau = tau_ms * sample_rate / 1000.0;
    const double t_total = static_cast<double>(n) / sample_rate;
    const double k = (f1 - f0) / t_total; // 线性扫频斜率
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double phase = 2.0 * kPi * (f0 * t + 0.5 * k * t * t);
        const double a = attack > 0 ? std::min(1.0, static_cast<double>(i) / attack) : 1.0;
        out.push_back(amplitude * a * std::exp(-i / tau) * std::sin(phase));
    }
}

std::vector<double> MakeDingDong(const WatermarkConfig& cfg) {
    std::vector<double> tmpl;
    AppendSweep(tmpl, cfg.ding_f0, cfg.ding_f1, cfg.ding_ms, cfg.ding_ms / 2.5,
                cfg.amplitude, cfg.sample_rate);
    tmpl.insert(tmpl.end(), cfg.tone_gap_ms * cfg.sample_rate / 1000, 0.0);
    AppendSweep(tmpl, cfg.dong_f0, cfg.dong_f1, cfg.dong_ms, cfg.dong_ms / 2.5,
                cfg.amplitude, cfg.sample_rate);
    return tmpl;
}
} // namespace

std::vector<int16_t> GenerateWatermark(const WatermarkConfig& cfg) {
    const auto tmpl = MakeDingDong(cfg);
    std::vector<int16_t> out(tmpl.size());
    for (size_t i = 0; i < tmpl.size(); ++i)
        out[i] = static_cast<int16_t>(tmpl[i] * 32767.0);
    return out;
}

WatermarkDetectResult DetectWatermark(const std::vector<int16_t>& capture,
                                      const WatermarkConfig& cfg) {
    WatermarkDetectResult res;
    const auto tmpl = MakeDingDong(cfg);
    const int m = static_cast<int>(tmpl.size());
    const int n = static_cast<int>(capture.size());
    if (n < m + 400) return res; // 模板全长 + 最小滑窗余量

    // 模板能量（零均值）
    double mean_t = std::accumulate(tmpl.begin(), tmpl.end(), 0.0) / m;
    double e_t = 0.0;
    std::vector<double> t0(m);
    for (int i = 0; i < m; ++i) {
        t0[i] = tmpl[i] - mean_t;
        e_t += t0[i] * t0[i];
    }
    if (e_t <= 0.0) return res;

    // 全量滑动 NCC
    const int span = n - m;
    std::vector<double> ncc(span + 1, 0.0);
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
    }

    // 过阈值局部峰（非极大抑制，窗口 m），主峰/次峰显著性校验——
    // 无真实水印时噪声相关峰平缓且多峰接近；真实"叮咚"回环峰尖锐孤立
    struct Peak { double pos; double v; };
    std::vector<Peak> peaks;
    for (int s = 0; s <= span; ++s) {
        if (ncc[s] < cfg.ncc_threshold) continue;
        const double prev = (s > 0) ? ncc[s - 1] : -1.0;
        const double next = (s < span) ? ncc[s + 1] : -1.0;
        if (ncc[s] < prev || ncc[s] < next) continue;
        double off = 0.0; // 抛物线亚采样插值
        if (s > 0 && s < span) {
            const double denom = ncc[s - 1] - 2.0 * ncc[s] + ncc[s + 1];
            if (denom != 0.0) off = 0.5 * (ncc[s - 1] - ncc[s + 1]) / denom;
        }
        if (!peaks.empty() && s - peaks.back().pos < m) {
            if (ncc[s] > peaks.back().v) peaks.back() = {s + off, ncc[s]};
            continue;
        }
        peaks.push_back({s + off, ncc[s]});
    }
    res.debug_peaks = static_cast<int>(peaks.size());
    if (peaks.empty()) return res;
    auto best_it = std::max_element(peaks.begin(), peaks.end(),
                                    [](const Peak& a, const Peak& b) { return a.v < b.v; });
    const Peak best = *best_it;
    peaks.erase(best_it);
    double second = 0.0;
    for (const auto& p : peaks) second = std::max(second, p.v);
    res.peak_ncc = best.v;
    res.debug_matched_slots = static_cast<int>((best.v - second) * 1000);
    if (best.v - second < cfg.min_peak_margin) return res;

    res.detected = true;
    res.p1 = static_cast<int64_t>(std::llround(best.pos));
    res.p2 = res.p1;
    res.skew = 0.0; // 单发水印不测漂移（实测值 <70ppm，对 AEC 无影响）
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
