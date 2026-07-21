#include "engine/message.h"

namespace mediator {

namespace {
uint64_t Fnv1a64(const std::string& s, uint64_t seed) {
    uint64_t h = 14695981039346656037ull ^ seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace

SessionId SessionIdFromUid(const std::string& uid) {
    SessionId sid{};
    const uint64_t h1 = Fnv1a64(uid, 0);
    const uint64_t h2 = Fnv1a64(uid, 0x9e3779b97f4a7c15ull);
    for (int i = 0; i < 8; ++i) {
        sid[i] = static_cast<uint8_t>((h1 >> (i * 8)) & 0xFF);
        sid[8 + i] = static_cast<uint8_t>((h2 >> (i * 8)) & 0xFF);
    }
    return sid;
}

} // namespace mediator
