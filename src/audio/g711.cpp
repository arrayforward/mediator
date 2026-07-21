#include "audio/g711.h"

namespace mediator::audio {

namespace {
constexpr int16_t kALawMax = 0x7FFF;

// 标准 A-law 压缩表（256 个线性段端点生成）
int16_t Search(int16_t val, const int16_t* table, int size) {
    for (int i = 0; i < size; ++i) {
        if (val <= table[i]) return static_cast<int16_t>(i);
    }
    return static_cast<int16_t>(size);
}

constexpr int16_t kSegEnd[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
} // namespace

uint8_t PcmToALaw(int16_t sample) {
    int16_t mask;
    int16_t pcm = sample >> 3; // 13bit
    if (pcm >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        pcm = static_cast<int16_t>(-pcm - 1);
    }
    int16_t seg = Search(pcm, kSegEnd, 8);
    if (seg >= 8) return static_cast<uint8_t>(0x7F ^ mask);
    uint8_t aval = static_cast<uint8_t>(seg << 4);
    if (seg < 2)
        aval |= static_cast<uint8_t>((pcm >> 1) & 0x0F);
    else
        aval |= static_cast<uint8_t>((pcm >> seg) & 0x0F);
    return static_cast<uint8_t>(aval ^ mask);
}

int16_t ALawToPcm(uint8_t alaw) {
    alaw ^= 0x55;
    int16_t t = static_cast<int16_t>((alaw & 0x0F) << 4);
    int seg = (alaw & 0x70) >> 4;
    switch (seg) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default:
        t += 0x108;
        t <<= (seg - 1);
    }
    return static_cast<int16_t>((alaw & 0x80) ? t : -t);
}

std::vector<uint8_t> EncodeALaw(const std::vector<int16_t>& pcm) {
    std::vector<uint8_t> out;
    out.reserve(pcm.size());
    for (int16_t s : pcm) out.push_back(PcmToALaw(s));
    return out;
}

std::vector<int16_t> DecodeALaw(const std::vector<uint8_t>& alaw) {
    std::vector<int16_t> out;
    out.reserve(alaw.size());
    for (uint8_t a : alaw) out.push_back(ALawToPcm(a));
    return out;
}

} // namespace mediator::audio
