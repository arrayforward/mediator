// 认证提供者：内置 JWT 结构解析、fail-closed 语义、uid→SessionId 派生（§7.3.3）
#include <gtest/gtest.h>

#include "engine/message.h"
#include "ext/auth_provider.h"

using mediator::SessionIdFromUid;
using mediator::ext::AuthRequest;
using mediator::ext::AuthResult;
using mediator::ext::BuiltinJwtAuth;
using mediator::ext::FnAuth;

namespace {
std::string MakeToken(const std::string& payload) {
    return "h." + payload + ".sig";
}
} // namespace

TEST(Auth, BuiltinExtractsUid) {
    BuiltinJwtAuth auth("secret");
    const auto r = auth.Verify(AuthRequest{MakeToken("{\"uid\":\"user-1\"}")});
    ASSERT_TRUE(r.allow);
    EXPECT_EQ(r.uid, "user-1");
}

TEST(Auth, BuiltinRejectsBadFormat) {
    BuiltinJwtAuth auth("secret");
    EXPECT_FALSE(auth.Verify(AuthRequest{"not-a-token"}).allow);
    EXPECT_FALSE(auth.Verify(AuthRequest{MakeToken("{\"sub\":\"x\"}")}).allow);
}

TEST(Auth, FailClosedOnWasmTrap) {
    // 模拟 wasm trap：返回 allow=false + auth_internal（§7.3.2 fail-closed）
    FnAuth auth([](const AuthRequest&) {
        return AuthResult{false, "", 0, "auth_internal"};
    });
    const auto r = auth.Verify(AuthRequest{MakeToken("{\"uid\":\"u\"}")});
    EXPECT_FALSE(r.allow);
    EXPECT_EQ(r.deny_reason, "auth_internal");
}

TEST(Auth, UidDerivesDeterministicSessionId) {
    // session_id 即 uid：同一 uid 派生同一 SessionId，不同 uid 不同
    EXPECT_EQ(SessionIdFromUid("user-1"), SessionIdFromUid("user-1"));
    EXPECT_NE(SessionIdFromUid("user-1"), SessionIdFromUid("user-2"));
}
