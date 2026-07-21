// ============================================================================
// g711.h — G.711 A-law 编解码（ITU-T G.711）
//
// 实现思路：
//   标准 A-law 13bit 折线压缩算法（查表+分段），无外部依赖。
//   下行链路：TTS 返回的 PCM16(16kHz) → EncodeALaw → 包装发端侧；
//   上行链路：端侧 G.711A → DecodeALaw → AEC/NS/VAD → ASR。
//   每个采样 16bit ↔ 8bit，码率减半。
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace mediator::audio {

// G.711 A-law 编解码（ITU-T G.711 查表法），PCM16 LE ↔ A-law 字节
uint8_t PcmToALaw(int16_t sample);
int16_t ALawToPcm(uint8_t alaw);

std::vector<uint8_t> EncodeALaw(const std::vector<int16_t>& pcm);
std::vector<int16_t> DecodeALaw(const std::vector<uint8_t>& alaw);

} // namespace mediator::audio
