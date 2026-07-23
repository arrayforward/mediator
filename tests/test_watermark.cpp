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

TEST(Watermark, LongMultiPulseWithinHeadroom) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    // 结构：chirp_count 个脉冲等间隔，总时长 = (count-1)*gap + 单声 ≈ 0.98s
    //（真机体验优化：死窗 <1s；投票规则随 count 自适应）
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    const int one = cfg.chirp_ms * cfg.sample_rate / 1000;
    ASSERT_EQ(static_cast<int>(wm.size()), (cfg.chirp_count - 1) * gap + one);
    EXPECT_GT(wm.size(), static_cast<size_t>(cfg.sample_rate) / 2); // ≥0.5s 可闻可采
    EXPECT_LT(wm.size(), static_cast<size_t>(cfg.sample_rate));     // <1s 不拖会话
    // 每个脉冲起点非零（等间隔结构成立）
    for (int k = 0; k < cfg.chirp_count; ++k) {
        int peak = 0;
        for (int i = 0; i < one; ++i) peak = std::max(peak, std::abs(wm[k * gap + i]));
        EXPECT_GT(peak, 20000) << "pulse " << k << " missing";
    }
    // 幅度：接近满幅但留 ~10% 余量，不削波
    int peak = 0;
    for (const auto s : wm) peak = std::max(peak, std::abs(static_cast<int>(s)));
    EXPECT_GT(peak, 28000);
    EXPECT_LT(peak, 32767);
    // 直接检测 + G.711A 回环检测均成立（检测端兼容新结构）
    EXPECT_TRUE(DetectWatermark(wm, cfg).detected);
    const auto loop = mediator::audio::DecodeALaw(mediator::audio::EncodeALaw(wm));
    const auto det = DetectWatermark(loop, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.p1, 0); // 一致性投票胜出候选的首脉冲 = 第一个脉冲
}

// ---- 多脉冲误锁回归（真机 delay8k=9503 离群 / sim 回环 2.5min 无 ASR final）----

// 前 2 脉冲畸变缺失（扬声器 AGC 爬坡）：旧检测器会把脉冲3-4 当首对，
// delay 偏大 2×GAP（离群值）→ 一致性投票必须拒绝（缺尾槽）
TEST(Watermark, ShiftedPulseSubsetRejected) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    std::vector<int16_t> cap(wm.size() + 1600, 0);
    std::copy(wm.begin() + 2 * gap, wm.end(), cap.begin() + 2 * gap);
    const auto det = DetectWatermark(cap, cfg);
    EXPECT_FALSE(det.detected);
    EXPECT_GT(det.debug_matched_slots, 0); // 有局部命中但被尾槽规则否决
}

// 尾脉冲缺失（真机 AGC/截放下尾脉冲不稳定）：次尾槽(count-2)在位 → 接受
TEST(Watermark, MissingLastPulseAcceptedViaPenultimateSlot) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    std::vector<int16_t> cap(wm.begin(), wm.end() - gap); // 去掉最后一个脉冲
    const auto det = DetectWatermark(cap, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.p1, 0);
    EXPECT_NEAR(det.skew, 0.0, 2e-5);
}

// 末两脉冲都缺失（次尾槽也不在）→ 拒绝，走超时 bypass
TEST(Watermark, MissingLastTwoPulsesRejected) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    std::vector<int16_t> cap(wm.begin(), wm.end() - 2 * gap);
    EXPECT_FALSE(DetectWatermark(cap, cfg).detected);
}

// 中间 1 个脉冲畸变（置零）：3/4 槽命中且最深槽在位 → 仍标定，p1 不偏移
TEST(Watermark, MiddlePulseMangledStillDetected) {
    WatermarkConfig cfg;
    auto wm = GenerateWatermark(cfg);
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    const int one = cfg.chirp_ms * cfg.sample_rate / 1000;
    std::fill(wm.begin() + 1 * gap, wm.begin() + 1 * gap + one, 0); // 第 2 脉冲
    const auto det = DetectWatermark(wm, cfg);
    ASSERT_TRUE(det.detected);
    EXPECT_EQ(det.p1, 0);
    EXPECT_NEAR(det.skew, 0.0, 2e-5);
}

// 检测不得在次尾槽入缓冲前触发——否则触发点之后的 chirp 尾巴会漏进
// ASR 流毒化识别（convai_sim 回环 2.5min 无 final 的根因，2 脉冲时代
// 检测需双脉冲齐、触发点≈水印末尾，故无此问题）。次尾槽规则下残留
// 尾巴 ≤1 个脉冲（≤340ms），实测不毒化 ASR。
TEST(Watermark, NoEarlyDetectionBeforePenultimateSlot) {
    WatermarkConfig cfg;
    const auto wm = GenerateWatermark(cfg);
    const int gap = cfg.gap_ms * cfg.sample_rate / 1000;
    const int one = cfg.chirp_ms * cfg.sample_rate / 1000;
    for (int pulses = 2; pulses <= cfg.chirp_count - 2; ++pulses) {
        std::vector<int16_t> part(wm.begin(),
                                  wm.begin() + (pulses - 1) * gap + one + gap / 2);
        EXPECT_FALSE(DetectWatermark(part, cfg).detected) << "pulses=" << pulses;
    }
    // 次尾槽(count-2)在位即可触发（count=4 时即 3 脉冲前缀，~0.66s+回环）
    std::vector<int16_t> penult(wm.begin(),
                                wm.begin() + (cfg.chirp_count - 2) * gap + one + gap / 2);
    EXPECT_TRUE(DetectWatermark(penult, cfg).detected);
    EXPECT_TRUE(DetectWatermark(wm, cfg).detected); // 完整水印 → 触发
}
