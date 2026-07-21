// ============================================================================
// test_auth.cpp — 认证单测：真实 HS256 验签（含 RFC 4231 标准向量）、
// fail-closed 语义、uid→SessionId 派生（§7.3.3 / §8）
// ============================================================================
#include <gtest/gtest.h>

#include "engine/message.h"
#include "ext/auth_provider.h"
#include "ext/hs256.h"

using mediator::SessionIdFromUid;
using mediator::ext::AuthRequest;
using mediator::ext::AuthResult;
using mediator::ext::Base64UrlDecode;
using mediator::ext::BuiltinJwtAuth;
using mediator::ext::FnAuth;
using mediator::ext::HmacSha256;
using mediator::ext::Sha256;
using mediator::ext::VerifyHs256Jwt;

namespace {

std::string Hex(const std::vector<uint8_t>& v) {
    static const char* kH = "0123456789abcdef";
    std::string s;
    for (auto b : v) { s += kH[b >> 4]; s += kH[b & 0xF]; }
    return s;
}

std::string Base64UrlEncode(const std::string& in) {
    static const char* kT = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i+1]) << 8) | uint8_t(in[i+2]);
        out += kT[(v >> 18) & 63]; out += kT[(v >> 12) & 63];
        out += kT[(v >> 6) & 63];  out += kT[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out += kT[(v >> 18) & 63];
        if (i + 1 < in.size()) {
            v |= uint8_t(in[i+1]) << 8;
            out += kT[(v >> 12) & 63];
            out += kT[(v >> 6) & 63];
        } else {
            out += kT[(v >> 12) & 63];
        }
    }
    return out;
}

// 用共享密钥签一个 HS256 JWT（测试/E2E 客户端同款逻辑）
std::string SignJwt(const std::string& payload, const std::string& secret) {
    const std::string input =
        Base64UrlEncode("{\"alg\":\"HS256\",\"typ\":\"JWT\"}") + "." +
        Base64UrlEncode(payload);
    const auto mac = HmacSha256(secret, input);
    return input + "." +
           Base64UrlEncode(std::string(reinterpret_cast<const char*>(mac.data()), 32));
}

const char* kSecret = "dev-secret";
const int64_t kNow = 1700000000000; // 2023-11，固定时钟保证确定性

} // namespace

TEST(Hs256, Sha256StandardVector) {
    EXPECT_EQ(Hex(Sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Hex(Sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Hs256, HmacRfc4231Case1) {
    // RFC 4231 Test Case 1：key=0x0b*20, data="Hi There"
    const auto mac = HmacSha256(std::string(20, '\x0b'), "Hi There");
    EXPECT_EQ(Hex(mac),
              "b0344c61d8db38535ca8afceaf0bf12b"
              "881dc200c9833da726e9376c2e32cff7");
}

TEST(Hs256, Base64UrlRoundTrip) {
    EXPECT_EQ(Base64UrlDecode(Base64UrlEncode("hello?+/=_")), "hello?+/=_");
}

TEST(Auth, ValidSignedTokenAccepted) {
    BuiltinJwtAuth auth(kSecret);
    const auto tok = SignJwt("{\"uid\":\"user-1\",\"exp\":1700003600}", kSecret);
    const auto r = auth.Verify(AuthRequest{tok, "", kNow});
    ASSERT_TRUE(r.allow) << r.deny_reason;
    EXPECT_EQ(r.uid, "user-1");
}

TEST(Auth, WrongSignatureRejected) {
    BuiltinJwtAuth auth(kSecret);
    const auto tok = SignJwt("{\"uid\":\"user-1\"}", "other-secret");
    const auto r = auth.Verify(AuthRequest{tok, "", kNow});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "bad_signature");
}

TEST(Auth, TamperedPayloadRejected) {
    BuiltinJwtAuth auth(kSecret);
    auto tok = SignJwt("{\"uid\":\"user-1\"}", kSecret);
    // 篡改 payload 但保留原签名
    const auto dot = tok.rfind('.');
    tok = tok.substr(0, tok.find('.') + 1) +
          Base64UrlEncode("{\"uid\":\"admin\"}") + tok.substr(dot);
    const auto r = auth.Verify(AuthRequest{tok, "", kNow});
    EXPECT_FALSE(r.allow);
}

TEST(Auth, ExpiredTokenRejected) {
    BuiltinJwtAuth auth(kSecret);
    const auto tok = SignJwt("{\"uid\":\"user-1\",\"exp\":1600000000}", kSecret);
    const auto r = auth.Verify(AuthRequest{tok, "", kNow});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "expired");
}

TEST(Auth, MalformedRejected) {
    BuiltinJwtAuth auth(kSecret);
    EXPECT_FALSE(auth.Verify(AuthRequest{"not-a-token", "", kNow}).allow);
    EXPECT_EQ(VerifyHs256Jwt("a.b.c", "", kNow).error, "bad_secret");
}

TEST(Auth, DebugTokenOnlyWhenEnabled) {
    BuiltinJwtAuth off(kSecret, false);
    EXPECT_FALSE(off.Verify(AuthRequest{"debug:user-1", "", kNow}).allow);

    BuiltinJwtAuth on(kSecret, true);
    const auto r = on.Verify(AuthRequest{"debug:user-1", "", kNow});
    ASSERT_TRUE(r.allow);
    EXPECT_EQ(r.uid, "user-1");
    // 空 uid 的调试 token 拒绝
    EXPECT_FALSE(on.Verify(AuthRequest{"debug:", "", kNow}).allow);
}

TEST(Auth, FailClosedOnWasmTrap) {
    FnAuth auth([](const AuthRequest&) {
        return AuthResult{false, "", 0, "auth_internal"};
    });
    const auto r = auth.Verify(AuthRequest{"x", "", kNow});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "auth_internal");
}

TEST(Auth, UidDerivesDeterministicSessionId) {
    EXPECT_EQ(SessionIdFromUid("user-1"), SessionIdFromUid("user-1"));
    EXPECT_NE(SessionIdFromUid("user-1"), SessionIdFromUid("user-2"));
}
