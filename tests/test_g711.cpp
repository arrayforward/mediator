// G.711A：往返误差、端点值、矢量批量
#include <gtest/gtest.h>

#include <cmath>

#include "audio/g711.h"

using mediator::audio::ALawToPcm;
using mediator::audio::DecodeALaw;
using mediator::audio::EncodeALaw;
using mediator::audio::PcmToALaw;

TEST(G711, RoundTripSmallError) {
    // A-law 是有损压扩：小信号误差 ≤ 1 LSB 级，大信号按比例
    for (int v = -30000; v <= 30000; v += 997) {
        const int16_t pcm = static_cast<int16_t>(v);
        const int16_t back = ALawToPcm(PcmToALaw(pcm));
        const double tol = std::max(64.0, std::abs(v) / 8.0);
        EXPECT_LE(std::abs(back - pcm), tol) << "v=" << v;
    }
}

TEST(G711, ZeroRoundTrip) {
    EXPECT_LE(std::abs(ALawToPcm(PcmToALaw(0))), 8);
}

TEST(G711, VectorCodecLength) {
    std::vector<int16_t> pcm(320, 1000); // 20ms@16k
    const auto enc = EncodeALaw(pcm);
    EXPECT_EQ(enc.size(), 320u);
    const auto dec = DecodeALaw(enc);
    EXPECT_EQ(dec.size(), 320u);
}
