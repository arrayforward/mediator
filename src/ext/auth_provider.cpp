#include "ext/auth_provider.h"

namespace mediator::ext {

namespace {
// 极简 payload 字段提取（测试用）：查找 "uid":"xxx"
std::string ExtractUid(const std::string& payload) {
    const std::string key = "\"uid\":\"";
    auto pos = payload.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    auto end = payload.find('"', pos);
    if (end == std::string::npos) return {};
    return payload.substr(pos, end - pos);
}
} // namespace

AuthResult BuiltinJwtAuth::Verify(const AuthRequest& req) {
    AuthResult r;
    const auto dot1 = req.token.find('.');
    const auto dot2 = req.token.rfind('.');
    if (dot1 == std::string::npos || dot2 == dot1 || m_secret.empty()) {
        r.deny_reason = "bad_format";
        return r;
    }
    // TODO(prod): HS256/RS256 真实验签（jwt-cpp）。当前仅结构解析，禁止上线。
    const std::string payload = req.token.substr(dot1 + 1, dot2 - dot1 - 1);
    r.uid = ExtractUid(payload);
    if (r.uid.empty()) {
        r.deny_reason = "no_uid";
        return r;
    }
    r.allow = true;
    r.ttl_s = 3600;
    return r;
}

} // namespace mediator::ext
