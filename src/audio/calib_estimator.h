// ============================================================================
// calib_estimator.h — 内容锚定 AEC 校准估计器
//
// 背景：特制水印音（chirp/叮咚）在真机上被硬件 NS/AGC/削波/自聊播放
// 多重绞杀，单发模板匹配成功率极低。本方案不用任何特制音——下行的
// TTS 语音本身就是宽带、持续、高辨识度的"水印"：
//
//   render 时间线（按下发时刻铺开的播放内容） × capture 上行采集
//   → 滑动 NCC 互相关 → 主峰 = "渲染→采集"全链路真实延迟
//   （网络下行/App 解码播放/声学回环/采集上行，全部包含）
//
// 采纳条件：主/次峰显著 + 播放窗能量足够 + 连续两次估计一致（防抖）。
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace mediator::audio {

class CalibEstimator {
public:
    struct Result {
        bool ready = false;      // 连续稳定，可采纳
        int32_t delay16k = 0;    // 全链路延迟（16k 采样）
        double ncc = 0.0;
    };

    // 下行 clip 下发时刻计入（8k PCM，时间线按播放时刻铺开）
    void FeedRender(int64_t ts_ms, const std::vector<int16_t>& pcm8k) {
        m_renders.push_back({ts_ms, pcm8k});
        // 只保留近 4s
        while (!m_renders.empty() &&
               m_renders.front().ts_ms +
                       static_cast<int64_t>(m_renders.front().pcm.size() / 8) <
                   ts_ms - 4000)
            m_renders.pop_front();
    }

    // 上行采集实时流入（16k PCM + 到达时刻；电话流基本连续）
    void FeedCapture(int64_t ts_ms, const std::vector<int16_t>& pcm16k) {
        if (m_capture.empty()) {
            m_capStartMs = ts_ms;
        } else {
            // 时间锚定校验：到达时刻与推断时刻偏差过大 → 补零对齐
            const int64_t expect = m_capStartMs +
                static_cast<int64_t>(m_capture.size()) / 16;
            const int64_t drift = ts_ms - expect;
            if (drift > 100) {
                m_capture.insert(m_capture.end(),
                                 static_cast<size_t>(drift * 16), 0);
            } else if (drift < -100) {
                // 时钟回跳/会话重启：清空重锚
                m_capture.clear();
                m_capStartMs = ts_ms;
            }
        }
        m_capture.insert(m_capture.end(), pcm16k.begin(), pcm16k.end());
        const size_t kMax = 48000; // 3s@16k
        if (m_capture.size() > kMax) {
            const size_t drop = m_capture.size() - kMax;
            m_capture.erase(m_capture.begin(), m_capture.begin() + drop);
            m_capStartMs += static_cast<int64_t>(drop / 16);
        }
    }

    // 数据充分时做互相关（调用方按任意频率驱动，内部节流 1s）
    Result TryEstimate(int64_t now_ms) {
        Result r;
        if (now_ms - m_lastTryMs < 1000) return r;
        m_lastTryMs = now_ms;
        constexpr size_t kCap = 8000;   // 采集窗 0.5s
        constexpr size_t kMaxLag = 24000; // 滞后窗 1.5s
        constexpr size_t kRend = kCap + kMaxLag + 8000; // render 时间线长 2.5s
        if (m_capture.size() < kCap || m_renders.empty()) return r;

        const int64_t t_end = m_capStartMs +
            static_cast<int64_t>(m_capture.size()) / 16; // capture 末尾时刻
        const int64_t t0 = t_end - static_cast<int64_t>(kRend / 16); // 时间线起点
        // 铺 render 时间线（16k，间隙补零）
        std::vector<double> rend(kRend, 0.0);
        for (const auto& c : m_renders) {
            int64_t pos = (c.ts_ms - t0) * 16;
            for (size_t i = 0; i < c.pcm.size(); ++i) {
                // 8k→16k 线性插值（与播放端听感一致即可）
                for (int k = 0; k < 2; ++k) {
                    const int64_t p = pos + static_cast<int64_t>(i) * 2 + k;
                    if (p >= 0 && p < static_cast<int64_t>(kRend)) rend[p] = c.pcm[i];
                }
            }
        }
        // 采集窗（最近 0.5s）
        const size_t coff = m_capture.size() - kCap;
        std::vector<double> cap(kCap);
        double ce = 0.0;
        for (size_t i = 0; i < kCap; ++i) {
            cap[i] = m_capture[coff + i];
            ce += cap[i] * cap[i];
        }
        if (ce / kCap < 400.0 * 400.0) return r; // 上行太静（无回声可测）

        // 滑动 NCC：lag L 表示 render 需向前平移 L 采样（延迟）
        double best = 0.0, second = 0.0;
        int32_t bestLag = -1;
        const double capMean = Mean(cap);
        const double capE = Energy(cap, capMean);
        for (int32_t lag = 0; lag + kCap <= kRend; lag += 4) {
            // render 播放能量门：该滞后窗内必须有真实播放内容
            double re = 0.0;
            for (size_t i = 0; i < kCap; ++i) re += rend[lag + i] * rend[lag + i];
            if (re / kCap < 500.0 * 500.0) continue;
            const double rMean = Mean(rend, lag, kCap);
            double dot = 0.0, re2 = 0.0;
            for (size_t i = 0; i < kCap; ++i) {
                const double x = rend[lag + i] - rMean;
                re2 += x * x;
                dot += x * (cap[i] - capMean);
            }
            if (re2 <= 0.0) continue;
            const double v = dot / std::sqrt(re2 * capE);
            if (v > best) {
                second = best;
                best = v;
                bestLag = lag;
            } else if (v > second && (bestLag < 0 || std::abs(lag - bestLag) > 320)) {
                second = v;
            }
        }
        r.ncc = best;
        if (bestLag < 0 || best < 0.30 || best - second < 0.08) {
            m_stable = 0;
            return r;
        }
        // render 时间线中的 lag → 全链路延迟 = t_end - 0.5s - (t0 + lag/16)
        const int32_t delay =
            static_cast<int32_t>((t_end - static_cast<int64_t>(kCap / 16) - t0) * 16) -
            bestLag;
        if (delay < 0 || delay > static_cast<int32_t>(kMaxLag)) {
            m_stable = 0;
            return r;
        }
        if (m_lastDelay >= 0 && std::abs(delay - m_lastDelay) <= 320) {
            if (++m_stable >= 1) {
                r.ready = true;
                r.delay16k = delay;
            }
        } else {
            m_stable = 0;
        }
        m_lastDelay = delay;
        return r;
    }

private:
    struct Clip {
        int64_t ts_ms;
        std::vector<int16_t> pcm;
    };

    static double Mean(const std::vector<double>& v) {
        double s = 0.0;
        for (const double x : v) s += x;
        return s / v.size();
    }
    static double Mean(const std::vector<double>& v, size_t off, size_t n) {
        double s = 0.0;
        for (size_t i = 0; i < n; ++i) s += v[off + i];
        return s / n;
    }
    static double Energy(const std::vector<double>& v, double mean) {
        double e = 0.0;
        for (const double x : v) e += (x - mean) * (x - mean);
        return e;
    }

    std::deque<Clip> m_renders;
    std::deque<int16_t> m_capture;
    int64_t m_capStartMs = -1;
    int64_t m_lastTryMs = 0;
    int32_t m_lastDelay = -1;
    int m_stable = 0;
};

} // namespace mediator::audio
