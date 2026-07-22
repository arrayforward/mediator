// ============================================================================
// align.h — AEC 参考信号对齐（设计文档 §6.5.3）
//
// 实现思路：
//   水印标定得到 {D, skew} 后，下行音频回灌 ProcessReverseStream 之前：
//   1. DelayLine：环形缓冲按 D（环路延迟采样数）延迟 render 参考，
//      使其与 capture 流时间对齐；
//   2. ResampleLinear：按 skew 做异步重采样（线性插值），补偿两端
//      时钟漂移（典型 <500ppm，线性插值足够）。
//   会话期间每 60s 由 kTickAecRecal 巡检复核漂移，超阈值重标定。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace mediator::audio {

// AEC 参考信号对齐（§6.5.3）：延迟补偿 + 异步重采样（线性插值）
// skew > 0 表示采集时钟偏快，需要拉伸 render 参考。
std::vector<int16_t> ResampleLinear(const std::vector<int16_t>& in, double skew);

// 环形延迟缓冲：按 delay_samples 延迟 render 参考后输出
class DelayLine {
public:
    explicit DelayLine(int32_t delay_samples);
    std::vector<int16_t> Process(const std::vector<int16_t>& in);
    int32_t Delay() const { return m_delay; }

private:
    int32_t m_delay;
    std::vector<int16_t> m_buf;
    size_t m_pos = 0;
};

// 泛型 G.711A 重采样（协议无关）：解码 → 整数倍线性重采样 → 编码。
// 支持 from/to 为 2:1 或 1:2（如 16k↔8k），其他比例走 ResampleLinear。
std::vector<uint8_t> ResampleG711(const std::vector<uint8_t>& g711,
                                  int from_rate, int to_rate);

// G.711A 解码 + 重采样输出 PCM（上行用）
std::vector<int16_t> G711ToPcmResampled(const uint8_t* g711, size_t len,
                                        int from_rate, int to_rate);

} // namespace mediator::audio
