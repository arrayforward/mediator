#include "audio/align.h"

#include <cmath>

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

} // namespace mediator::audio
