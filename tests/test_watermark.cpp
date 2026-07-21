// 水印标定：合成"模板+已知延迟+已知skew+噪声"验证检测精度与负例（§6.5.4）
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
// 符号约定：skew>0 表示采集时钟偏快（单位时间采样更多 → 间隔变大）。
// ResampleLinear(in, s) 的输出长度为 n/(1+s)，故传 -skew 得到正向漂移。
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
} // namespace

TEST(Watermark, DetectsDelayAndSkewWithinTolerance) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int delay = 5000; // ~312ms@16k
    const double skew = 0.0002; // +200ppm
    const auto cap = MakeCapture(wm, delay, skew, 500.0, 42);

    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);

    const CalibResult cal = ComputeCalib(det, /*play_start_sample=*/0, cfg);
    // 延迟误差 < 1ms（16 采样）
    EXPECT_NEAR(cal.delay_samples, delay, 16);
    // skew 误差 < 20ppm
    EXPECT_NEAR(cal.skew, skew, 2e-5);
}

TEST(Watermark, NegativeSkewDetected) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const auto cap = MakeCapture(wm, 3000, -0.0002, 300.0, 7);
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_NEAR(det.skew, -0.0002, 2e-5);
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

TEST(Watermark, SingleChirpRejected) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    // 只保留第一声（截掉第二声），不应误判
    const int one_len = cfg.gap_ms * cfg.sample_rate / 1000;
    std::vector<int16_t> cap(3000 + one_len + 1600, 0);
    std::copy(wm.begin(), wm.begin() + one_len, cap.begin() + 3000);
    EXPECT_FALSE(DetectWatermark(cap, cfg).detected);
}

TEST(Watermark, DetectsAtBufferStart) {
    // 回归：水印恰好在缓冲起点（WSS 回环标定的真实场景），峰值扫描必须覆盖 s=0
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const auto det = DetectWatermark(wm, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.p1, 0);
    // G.711A 回环（编码→解码）后仍可检测
    const auto loop = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    EXPECT_TRUE(DetectWatermark(loop, cfg).detected);
}

TEST(Watermark, SurvivesG711Distortion) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    // 水印经 G.711A 往返（模拟下行编解码失真）
    const auto codec = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    const auto cap = MakeCapture(codec, 4000, 0.0, 300.0, 9);
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_NEAR(det.skew, 0.0, 2e-5);
}
