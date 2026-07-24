// 水印标定：合成"叮咚模板+已知延迟+噪声"验证检测精度与负例
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "audio/align.h"
#include "audio/g711.h"
#include "audio/watermark.h"

using mediator::audio::CalibResult;
using mediator::audio::ComputeCalib;
using mediator::audio::DetectWatermark;
using mediator::audio::GenerateWatermark;
using mediator::audio::ResampleLinear;
using mediator::audio::WatermarkConfig;

namespace {
// 按 skew 重采样（模拟端侧时钟漂移）后，嵌入 delay 偏移。
std::vector<int16_t> MakeCapture(const std::vector<int16_t>& wm, int delay, double skew,
                                 double noise_amp, uint32_t seed) {
    auto drifted = ResampleLinear(wm, -skew);
    std::vector<int16_t> cap(delay + drifted.size() + 1600, 0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, noise_amp);
    for (auto& s : cap) s = static_cast<int16_t>(nd(rng));
    for (size_t i = 0; i < drifted.size(); ++i) {
        const int v = cap[delay + i] + drifted[i];
        cap[delay + i] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
    }
    return cap;
}

int PeakAbs(const std::vector<int16_t>& v, size_t from, size_t to) {
    int peak = 0;
    for (size_t i = from; i < to && i < v.size(); ++i)
        peak = std::max(peak, std::abs(static_cast<int>(v[i])));
    return peak;
}
} // namespace

TEST(Watermark, DetectsDelayWithinTolerance) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int delay = 5000; // ~312ms@16k
    const auto cap = MakeCapture(wm, delay, 0.0002, 500.0, 42);

    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);

    const CalibResult cal = ComputeCalib(det, /*play_start_sample=*/0, cfg);
    // 延迟误差 < 1ms（16 采样）；单发水印不测漂移（恒 0）
    EXPECT_NEAR(cal.delay_samples, delay, 16);
    EXPECT_EQ(cal.skew, 0.0);
}

TEST(Watermark, NoFalsePositiveOnSilence) {
    WatermarkConfig cfg;
    std::vector<int16_t> silence(16000, 0);
    EXPECT_FALSE(DetectWatermark(silence, cfg).detected);
}

TEST(Watermark, NoFalsePositiveOnPureNoise) {
    WatermarkConfig cfg;
    std::vector<int16_t> noise(16000);
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> ud(-2000, 2000);
    for (auto& s : noise) s = static_cast<int16_t>(ud(rng));
    EXPECT_FALSE(DetectWatermark(noise, cfg).detected);
}

TEST(Watermark, DetectsAtBufferStart) {
    // 回归：水印恰好在缓冲起点（WSS 回环标定的真实场景），峰值扫描必须覆盖 s=0
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const auto cap = MakeCapture(wm, 0, 0.0, 0.0, 1);
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.p1, 0);
    // G.711A 回环（编码→解码）后仍可检测
    const auto loop = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    const auto cap2 = MakeCapture(loop, 0, 0.0, 0.0, 1);
    EXPECT_TRUE(DetectWatermark(cap2, cfg).detected);
}

TEST(Watermark, SurvivesG711Distortion) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    // 水印经 G.711A 往返（模拟下行编解码失真）
    const auto codec = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    const auto cap = MakeCapture(codec, 4000, 0.0, 300.0, 9);
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.skew, 0.0);
}

// 叮咚结构：叮(ding) + 间隔 + 咚(dong) 单发，总时长 <1s，双音均在位
TEST(Watermark, DingDongStructure) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int sr = cfg.sample_rate;
    const int ding = cfg.ding_ms * sr / 1000;
    const int gap = cfg.tone_gap_ms * sr / 1000;
    const int dong = cfg.dong_ms * sr / 1000;
    ASSERT_EQ(static_cast<int>(wm.size()), ding + gap + dong);
    EXPECT_LT(wm.size(), static_cast<size_t>(sr)); // <1s 不拖会话
    // 双音均非零（起音峰值 > 60% 满幅）
    EXPECT_GT(PeakAbs(wm, 0, ding / 4), 20000);
    EXPECT_GT(PeakAbs(wm, ding + gap, ding + gap + dong / 4), 20000);
    // 间隔段静音
    EXPECT_LT(PeakAbs(wm, ding + gap / 4, ding + gap * 3 / 4), 2000);
    // 幅度：接近满幅但留余量，不削波
    int peak = PeakAbs(wm, 0, wm.size());
    EXPECT_GT(peak, 26000);
    EXPECT_LT(peak, 32767);
    // 直接检测 + G.711A 回环检测均成立
    EXPECT_TRUE(DetectWatermark(MakeCapture(wm, 0, 0.0, 0.0, 1), cfg).detected);
    const auto loop = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    EXPECT_TRUE(DetectWatermark(MakeCapture(loop, 0, 0.0, 0.0, 1), cfg).detected);
}

// 模板未播完（前缀截断）不得误判：相关能量不足，等完整模板再判
TEST(Watermark, NoEarlyDetectionOnPartialTemplate) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    // 只到"叮"结束（缺"咚"）：NCC 峰弱且与次峰余量不足 → 拒绝
    const int ding = cfg.ding_ms * cfg.sample_rate / 1000;
    std::vector<int16_t> part(wm.begin(), wm.begin() + ding);
    part.resize(wm.size() * 2, 0); // 补零凑够最小检测长度
    EXPECT_FALSE(DetectWatermark(part, cfg).detected);
}

// 真机回环损耗：水印衰减到 25% 幅度 + 噪声仍可检测（扬声器→mic 高损耗）
TEST(Watermark, DetectsAttenuatedLoopback) {
    WatermarkConfig cfg;
    auto wm = GenerateWatermark(cfg);
    for (auto& s : wm) s = static_cast<int16_t>(s / 4);
    const auto cap = MakeCapture(wm, 6800, 0.0, 200.0, 5);
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_NEAR(det.p1, 6800, 16);
}
