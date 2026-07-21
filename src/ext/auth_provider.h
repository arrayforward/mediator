// ============================================================================
// auth_provider.h — 可插拔认证提供者（设计文档 §7.3 / §8）
//
// 实现思路：
//   认证逻辑抽象为 AuthProvider 接口，业务代码只面向接口：
//   - BuiltinJwtAuth：内置 JWT 验签（生产接 jwt-cpp；当前为结构解析 stub）
//   - WasmAuth（待接 Wasmtime）：外部验证方式以 wasm 扩展注入，
//     启动参数 --auth-provider=wasm:<name> 激活；fail-closed：
//     wasm 超时/trap/燃料耗尽一律视为拒绝。
//   - FnAuth：函数注入型，单测/E2E 模拟 wasm 的各种行为（allow/deny/
//     trap/slow），验证认证路径与熔断逻辑。
//
// 关键约定：session_id 即 JWT 的 uid claim。AuthResult 只返回 uid，
// 由宿主经 SessionIdFromUid 派生 16 字节 SessionId，内置与 wasm 路径
// 完全同构。认证在阶段1执行（连接建立时），结果仅产生 kWsConnected
// 或 拒绝+断开，不写共享状态——四阶段架构的特许点。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "engine/message.h"

namespace mediator::ext {

struct AuthRequest {
    std::string token;
    std::string peer_ip;
    int64_t now_ms = 0;
};

struct AuthResult {
    bool allow = false;
    std::string uid;        // session_id 即 uid（SessionIdFromUid 派生）
    int ttl_s = 0;
    std::string deny_reason;
};

// 认证提供者抽象（§7.3）：内置 JWT / wasm 扩展可插拔，fail-closed
class AuthProvider {
public:
    virtual ~AuthProvider() = default;
    virtual AuthResult Verify(const AuthRequest& req) = 0;
};

// 内置实现：JWT HS256 真实验签（HMAC-SHA256 + exp 校验，见 ext/hs256.h）。
// 验签失败/过期/无 uid 一律拒绝（fail-closed）。
//
// 调试后门（仅端到端调试用）：启动参数开启后，接受形如 "debug:<uid>" 的
// 特殊 token 直接放行（免签名），每次使用打 WARN 审计日志。
// 默认关闭；生产环境严禁开启（网关启动日志有显式告警）。
class BuiltinJwtAuth final : public AuthProvider {
public:
    explicit BuiltinJwtAuth(std::string secret, bool allow_debug_token = false)
        : m_secret(std::move(secret)), m_allowDebugToken(allow_debug_token) {}
    AuthResult Verify(const AuthRequest& req) override;

    // 调试 token 前缀："debug:<uid>"
    static constexpr const char* kDebugPrefix = "debug:";

private:
    std::string m_secret;
    bool m_allowDebugToken;
};

// 函数型认证提供者：单测/E2E 注入 wasm 行为（allow/deny/trap/slow）的替身
class FnAuth final : public AuthProvider {
public:
    using Fn = AuthResult (*)(const AuthRequest&);
    explicit FnAuth(Fn fn) : m_fn(fn) {}
    AuthResult Verify(const AuthRequest& req) override { return m_fn(req); }

private:
    Fn m_fn;
};

} // namespace mediator::ext
