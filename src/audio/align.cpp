#include "audio/align.h"

#include <cmath>

#include "audio/g711.h"

namespace mediator::audio {

std::vector<int16_t> ResampleLinear(const std::vector<int16_t>& in, double skew) {
    if (in.empty()) return {};
    const double ratio = 1.0 + skew;
    const size_t out_n = static_cast<size_t>(std::llround(in.size() / ratio));
    std::vector<int16_t> out(out_n);
    for (size_t i = 0; i < out_n; ++i) {
        const double src = i * ratio;
        const size_t i0 = static_cast<size_t>(src);
        const double frac = src - i0;
        const double a = in[i0];
        const double b = (i0 + 1 < in.size()) ? in[i0 + 1] : in[i0];
        out[i] = static_cast<int16_t>(a + frac * (b - a));
    }
    return out;
}

DelayLine::DelayLine(int32_t delay_samples)
    : m_delay(delay_samples < 0 ? 0 : delay_samples),
      m_buf(static_cast<size_t>(m_delay > 0 ? m_delay : 1), 0) {}

std::vector<int16_t> DelayLine::Process(const std::vector<int16_t>& in) {
    std::vector<int16_t> out(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        out[i] = m_buf[m_pos];
        m_buf[m_pos] = in[i];
        m_pos = (m_pos + 1) % m_buf.size();
    }
    return out;
}

namespace {
// 整数倍重采样：2:1 均值抽取 / 1:2 线性插值；其他比例走 ResampleLinear
std::vector<int16_t> ResampleIntRatio(const std::vector<int16_t>& in, int from, int to) {
    if (from == to || in.empty()) return in;
    if (from == to * 2) { // 2:1 抽取
        std::vector<int16_t> out(in.size() / 2);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = static_cast<int16_t>(
                (static_cast<int>(in[i * 2]) + static_cast<int>(in[i * 2 + 1])) / 2);
        return out;
    }
    if (to == from * 2) { // 1:2 插值
        std::vector<int16_t> out(in.size() * 2);
        for (size_t i = 0; i < in.size(); ++i) {
            const int16_t a = in[i];
            const int16_t b = (i + 1 < in.size()) ? in[i + 1] : a;
            out[i * 2] = a;
            out[i * 2 + 1] = static_cast<int16_t>((static_cast<int>(a) + b) / 2);
        }
        return out;
    }
    return ResampleLinear(in, static_cast<double>(from) / to - 1.0);
}
} // namespace

std::vector<uint8_t> ResampleG711(const std::vector<uint8_t>& g711,
                                  int from_rate, int to_rate) {
    const auto pcm = DecodeALaw(g711);
    return EncodeALaw(ResampleIntRatio(pcm, from_rate, to_rate));
}

std::vector<int16_t> G711ToPcmResampled(const uint8_t* g711, size_t len,
                                        int from_rate, int to_rate) {
    const std::vector<uint8_t> bytes(g711, g711 + len);
    return ResampleIntRatio(DecodeALaw(bytes), from_rate, to_rate);
}

} // namespace mediator::audio
