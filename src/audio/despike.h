// ============================================================================
// despike.h — 上行脉冲噪声滤波
//
// 真机现象：喇叭大声播放时 mic 削波 + 硬件 AGC 分块调增益，增益切换边界
// 产生满幅脉冲（实测手机上行 11.7% 样本 |s|>16000，含多样本连簇，上下文
// 却是 ±8 静音电平）。这些脉冲毒化水印 NCC（永不命中）、VAD 能量门
// （断句卡死）。
//
// 滤波规则：自适应包络钳位——滚动 RMS（50ms）估计局部电平，
// 包络 = max(3000, 6×RMS)，超限样本钳到包络（孤立/成簇脉冲均有效，
// 语音包络随 RMS 自适应不受伤）。
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace mediator::audio {

inline void Despike(std::vector<int16_t>& pcm) {
    const size_t n = pcm.size();
    if (n < 16) return;
    constexpr size_t kWin = 400; // 50ms@8k 滚动窗
    double e = 0.0;
    std::vector<double> env(n);
    // 前向滚动 RMS（窗尾随当前样本）
    for (size_t i = 0; i < n; ++i) {
        const double s = pcm[i];
        e += s * s;
        if (i >= kWin) {
            const double o = pcm[i - kWin];
            e -= o * o;
        }
        const double rms = std::sqrt(std::max(0.0, e) / std::min(i + 1, kWin));
        env[i] = std::max(3000.0, 6.0 * rms);
    }
    for (size_t i = 0; i < n; ++i) {
        const int s = pcm[i];
        if (std::abs(s) > env[i])
            pcm[i] = static_cast<int16_t>(s > 0 ? env[i] : -env[i]);
    }
}

} // namespace mediator::audio
