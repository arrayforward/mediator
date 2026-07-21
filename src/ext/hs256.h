// ============================================================================
// hs256.h — JWT HS256 真实验签（设计文档 §8，替换结构解析 stub）
//
// 实现思路：
//   自实现 SHA-256 + HMAC-SHA256（FIPS 180-4 / RFC 2104），零外部依赖、
//   跨平台、确定性可测。验签流程：
//     1. 按 '.' 拆分 header.payload.signature
//     2. base64url 解码 signature
//     3. HMAC_SHA256(secret, header + "." + payload) 与 signature 做
//        常时间比较（防时序侧信道）
//     4. payload（base64url 解码）提取 "uid" 与 "exp"，exp 必须 > now
//
// 安全说明：仅支持 HS256（对称密钥）。RS256 等非对称算法需接 jwt-cpp，
// 接口不变。密钥为空直接拒绝（fail-closed）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mediator::ext {

// SHA-256 摘要（32 字节）
std::vector<uint8_t> Sha256(const std::string& data);

// HMAC-SHA256(key, message)（32 字节）
std::vector<uint8_t> HmacSha256(const std::string& key, const std::string& data);

// base64url 解码（无 padding 容忍）；失败返回空
std::string Base64UrlDecode(const std::string& in);

// JWT HS256 验签结果
struct JwtClaims {
    bool valid = false;
    std::string uid;
    int64_t exp = 0;
    std::string error; // 失败原因（bad_format/bad_signature/no_uid/expired）
};

// 验签 + 提取 uid/exp + 过期检查（now_ms 注入时钟，测试可确定性验证过期）
JwtClaims VerifyHs256Jwt(const std::string& token, const std::string& secret,
                         int64_t now_ms);

} // namespace mediator::ext
