#include "audio/watermark.h"

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
    const int total = gap_samples + static_cast<int>(chirp.size());
    std::vector<int16_t> out(total, 0);
    for (size_t i = 0; i < chirp.size(); ++i) {
        out[i] = static_cast<int16_t>(chirp[i] * 32767.0);
        out[gap_samples + i] = static_cast<int16_t>(chirp[i] * 32767.0);
    }
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
    for (int s = 1; s < span; ++s) {
        if (ncc[s] < cfg.ncc_threshold) continue;
        if (ncc[s] < ncc[s - 1] || ncc[s] < ncc[s + 1]) continue; // 局部最大
        if (!peaks.empty() && s - peaks.back().pos < m) {          // NMS：留更高者
            if (ncc[s] > peaks.back().v) {
                // 抛物线插值：y = a x^2 + b x + c，顶点 x = 0.5*(y-1 - y+1)/(y-1 - 2y0 + y+1)
                const double denom = ncc[s - 1] - 2.0 * ncc[s] + ncc[s + 1];
                const double off = (denom != 0.0)
                                       ? 0.5 * (ncc[s - 1] - ncc[s + 1]) / denom
                                       : 0.0;
                peaks.back() = {s + off, ncc[s]};
            }
            continue;
        }
        const double denom = ncc[s - 1] - 2.0 * ncc[s] + ncc[s + 1];
        const double off =
            (denom != 0.0) ? 0.5 * (ncc[s - 1] - ncc[s + 1]) / denom : 0.0;
        peaks.push_back({s + off, ncc[s]});
    }
    if (peaks.size() < 2) return res;

    const int expect = cfg.gap_ms * cfg.sample_rate / 1000;
    const int tol = cfg.interval_tolerance_ms * cfg.sample_rate / 1000;
    for (size_t i = 0; i + 1 < peaks.size(); ++i) {
        const double d = peaks[i + 1].pos - peaks[i].pos;
        if (std::abs(d - expect) <= tol) {
            res.detected = true;
            res.p1 = static_cast<int64_t>(std::llround(peaks[i].pos));
            res.p2 = static_cast<int64_t>(std::llround(peaks[i + 1].pos));
            res.skew = d / expect - 1.0;
            return res;
        }
    }
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
