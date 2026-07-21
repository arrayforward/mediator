// ============================================================================
// auth_provider.cpp — 认证提供者实现
//
// BuiltinJwtAuth：委托 ext/hs256 做真实 HS256 验签（HMAC-SHA256 常时间比较
// + exp 过期校验），通过后取 payload 的 uid 作为会话身份。
// ============================================================================
#include "ext/auth_provider.h"

#include "core/log.h"
#include "ext/hs256.h"

#include <cstring>

namespace mediator::ext {

AuthResult BuiltinJwtAuth::Verify(const AuthRequest& req) {
    AuthResult r;
    // 调试后门（仅 E2E 调试，启动参数显式开启才生效）
    if (m_allowDebugToken && req.token.rfind(kDebugPrefix, 0) == 0) {
        const std::string uid = req.token.substr(std::strlen(kDebugPrefix));
        if (!uid.empty()) {
            MDT_WARN("DEBUG TOKEN used, uid={} (disable --allow-debug-token in prod!)",
                     uid);
            r.allow = true;
            r.uid = uid;
            r.ttl_s = 600;
            return r;
        }
    }
    const auto claims = VerifyHs256Jwt(req.token, m_secret, req.now_ms);
    if (!claims.valid) {
        r.deny_reason = claims.error;
        return r;
    }
    r.allow = true;
    r.uid = claims.uid;
    r.ttl_s = claims.exp > 0 ? static_cast<int>(claims.exp - req.now_ms / 1000) : 3600;
    return r;
}

} // namespace mediator::ext
