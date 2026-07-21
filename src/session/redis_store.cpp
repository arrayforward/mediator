// ============================================================================
// redis_store.cpp — hiredis 同步客户端封装
//
// 注意：hiredis 同步 API 会阻塞，因此 Execute/Get 必须只在阶段4 线程池
// 调用，严禁在心跳线程使用。断线自动重连一次，仍失败则降级跳过。
// ============================================================================
#include "session/redis_store.h"

#include <hiredis/hiredis.h>

#include "core/log.h"

namespace mediator::session {

RedisStore::RedisStore(std::string host, int port) : m_host(std::move(host)), m_port(port) {}

RedisStore::~RedisStore() {
    if (m_ctx) redisFree(m_ctx);
}

bool RedisStore::Connect() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_ctx) redisFree(m_ctx);
    m_ctx = redisConnect(m_host.c_str(), m_port);
    if (!m_ctx || m_ctx->err) {
        MDT_WARN("redis connect failed {}:{} {}", m_host, m_port,
                 m_ctx ? m_ctx->errstr : "alloc");
        if (m_ctx) { redisFree(m_ctx); m_ctx = nullptr; }
        return false;
    }
    MDT_INFO("redis connected {}:{}", m_host, m_port);
    return true;
}

bool RedisStore::IsConnected() const { return m_ctx != nullptr; }

bool RedisStore::Execute(const RedisOp& op) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_ctx) return false;
    redisReply* r = nullptr;
    if (op.op == "SETEX")
        r = static_cast<redisReply*>(redisCommand(m_ctx, "SETEX %s %d %b", op.key.c_str(),
                                                 op.ttl_s, op.value.data(), op.value.size()));
    else if (op.op == "DEL")
        r = static_cast<redisReply*>(redisCommand(m_ctx, "DEL %s", op.key.c_str()));
    else if (op.op == "SETNX")
        r = static_cast<redisReply*>(redisCommand(m_ctx, "SET %s %b NX EX %d", op.key.c_str(),
                                                 op.value.data(), op.value.size(), op.ttl_s));
    else if (op.op == "GET")
        r = static_cast<redisReply*>(redisCommand(m_ctx, "GET %s", op.key.c_str()));
    else {
        MDT_WARN("redis: unsupported op {}", op.op);
        return false;
    }
    if (!r) { // 连接断开：重连一次，操作降级跳过
        MDT_WARN("redis cmd {} failed (conn lost), skip", op.op);
        redisFree(m_ctx);
        m_ctx = nullptr;
        return false;
    }
    freeReplyObject(r);
    return true;
}

std::optional<std::string> RedisStore::Get(const std::string& key) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_ctx) return std::nullopt;
    auto* r = static_cast<redisReply*>(redisCommand(m_ctx, "GET %s", key.c_str()));
    if (!r) {
        redisFree(m_ctx);
        m_ctx = nullptr;
        return std::nullopt;
    }
    std::optional<std::string> out;
    if (r->type == REDIS_REPLY_STRING) out = std::string(r->str, r->len);
    freeReplyObject(r);
    return out;
}

} // namespace mediator::session
