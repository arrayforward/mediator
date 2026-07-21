// 对齐：线性重采样长度/波形保持、延迟线精确延迟
#include <gtest/gtest.h>

#include <cmath>

#include "audio/align.h"

using mediator::audio::DelayLine;
using mediator::audio::ResampleLinear;

TEST(Align, ResampleLengthFollowsSkew) {
    std::vector<int16_t> in(16000, 100);
    EXPECT_EQ(ResampleLinear(in, 0.001).size(), 15984u);  // +1000ppm → 变短
    EXPECT_EQ(ResampleLinear(in, -0.001).size(), 16016u); // -1000ppm → 变长
    EXPECT_EQ(ResampleLinear(in, 0.0).size(), 16000u);
}

TEST(Align, ResamplePreservesSine) {
    // 重采样后正弦过零间隔近似不变（误差 < 1%）
    std::vector<int16_t> in(16000);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<int16_t>(10000 * std::sin(2 * 3.14159265 * 440 * i / 16000));
    const auto out = ResampleLinear(in, 0.0005);
    ASSERT_EQ(out.size(), 15992u);
    // 峰值幅度保持
    int16_t mx = 0;
    for (auto s : out) mx = std::max(mx, s);
    EXPECT_GT(mx, 9800);
}

TEST(Align, DelayLineShiftsExactly) {
    DelayLine dl(100);
    std::vector<int16_t> in(300, 7);
    const auto out = dl.Process(in);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(out[i], 0);
    for (int i = 100; i < 300; ++i) EXPECT_EQ(out[i], 7);
}
