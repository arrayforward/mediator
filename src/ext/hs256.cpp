// ============================================================================
// hs256.cpp — SHA-256 / HMAC-SHA256 / base64url / JWT 验签实现
//
// SHA-256：FIPS 180-4 标准实现（K 常量 + 消息调度 + 8 工作寄存器压缩）。
// HMAC：RFC 2104（ipad 0x36 / opad 0x5C，块长 64）。
// 常时间比较：逐字节异或累积，不提前退出。
// ============================================================================
#include "ext/hs256.h"

#include <cstring>

namespace mediator::ext {

namespace {

// ---- SHA-256（FIPS 180-4）----
class Sha256Ctx {
public:
    Sha256Ctx() {
        m_h[0]=0x6a09e667; m_h[1]=0xbb67ae85; m_h[2]=0x3c6ef372; m_h[3]=0xa54ff53a;
        m_h[4]=0x510e527f; m_h[5]=0x9b05688c; m_h[6]=0x1f83d9ab; m_h[7]=0x5be0cd19;
    }

    void Update(const uint8_t* data, size_t len) {
        m_total += len;
        while (len > 0) {
            const size_t take = std::min(len, 64 - m_bufLen);
            std::memcpy(m_buf + m_bufLen, data, take);
            m_bufLen += take;
            data += take;
            len -= take;
            if (m_bufLen == 64) {
                Compress(m_buf);
                m_bufLen = 0;
            }
        }
    }

    std::vector<uint8_t> Final() {
        uint64_t bits = m_total * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t zero = 0;
        while (m_bufLen != 56) Update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[7 - i] = (bits >> (i * 8)) & 0xFF;
        // 直接压缩最后一块（不用 Update 以免重复计入总长）
        std::memcpy(m_buf + 56, lenb, 8);
        Compress(m_buf);
        std::vector<uint8_t> out(32);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = (m_h[i] >> (24 - j * 8)) & 0xFF;
        return out;
    }

private:
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Compress(const uint8_t* block) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i-15], 7) ^ Rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const uint32_t s1 = Rotr(w[i-2], 17) ^ Rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=m_h[0],b=m_h[1],c=m_h[2],d=m_h[3],e=m_h[4],f=m_h[5],g=m_h[6],h=m_h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Rotr(e,6) ^ Rotr(e,11) ^ Rotr(e,25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Rotr(a,2) ^ Rotr(a,13) ^ Rotr(a,22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        m_h[0]+=a; m_h[1]+=b; m_h[2]+=c; m_h[3]+=d;
        m_h[4]+=e; m_h[5]+=f; m_h[6]+=g; m_h[7]+=h;
    }

    uint32_t m_h[8];
    uint8_t m_buf[64]{};
    size_t m_bufLen = 0;
    uint64_t m_total = 0;
};

// JSON payload 极简字段提取："uid":"..." / "exp":12345
std::string JsonStr(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size();
    auto end = json.find('"', pos);
    return end == std::string::npos ? std::string{} : json.substr(pos, end - pos);
}
int64_t JsonInt(const std::string& json, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return 0;
    pos += pat.size();
    while (pos < json.size() && (json[pos] == ' ')) ++pos;
    return std::strtoll(json.c_str() + pos, nullptr, 10);
}
} // namespace

std::vector<uint8_t> Sha256(const std::string& data) {
    Sha256Ctx ctx;
    ctx.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return ctx.Final();
}

std::vector<uint8_t> HmacSha256(const std::string& key, const std::string& data) {
    std::string k = key;
    if (k.size() > 64) {
        const auto digest = Sha256(k);
        k.assign(reinterpret_cast<const char*>(digest.data()), digest.size());
    }
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    Sha256Ctx inner;
    inner.Update(reinterpret_cast<const uint8_t*>(ipad.data()), 64);
    inner.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    const auto ih = inner.Final();
    Sha256Ctx outer;
    outer.Update(reinterpret_cast<const uint8_t*>(opad.data()), 64);
    outer.Update(ih.data(), ih.size());
    return outer.Final();
}

std::string Base64UrlDecode(const std::string& in) {
    static const auto val_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val_of(c);
        if (v < 0) return {};
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

JwtClaims VerifyHs256Jwt(const std::string& token, const std::string& secret,
                         int64_t now_ms) {
    JwtClaims c;
    if (secret.empty()) {
        c.error = "bad_secret";
        return c;
    }
    const auto dot1 = token.find('.');
    const auto dot2 = token.rfind('.');
    if (dot1 == std::string::npos || dot2 == dot1) {
        c.error = "bad_format";
        return c;
    }
    const std::string signing_input = token.substr(0, dot2);
    const std::string sig = Base64UrlDecode(token.substr(dot2 + 1));
    const std::string payload = Base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
    if (sig.size() != 32 || payload.empty()) {
        c.error = "bad_format";
        return c;
    }
    // 常时间比较（防时序侧信道）
    const auto expect = HmacSha256(secret, signing_input);
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i)
        diff |= expect[i] ^ static_cast<uint8_t>(sig[i]);
    if (diff != 0) {
        c.error = "bad_signature";
        return c;
    }
    c.uid = JsonStr(payload, "uid");
    c.exp = JsonInt(payload, "exp");
    if (c.uid.empty()) {
        c.error = "no_uid";
        return c;
    }
    if (c.exp != 0 && now_ms / 1000 >= c.exp) {
        c.error = "expired";
        return c;
    }
    c.valid = true;
    return c;
}

} // namespace mediator::ext
