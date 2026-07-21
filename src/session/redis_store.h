// ============================================================================
// redis_store.h — Redis 持久化（hiredis，设计文档 §5/§6.4）
//
// 实现思路：
//   - Execute(RedisOp)：心跳 ChangeSet 的 redis_ops 在线程池异步执行
//     （SETEX/DEL/GET 等），不阻塞心跳（阶段4 异步化）。
//   - Get(key)：同步读，用于连接建立时恢复 上下文(ctx:{uid}) 与
//     上一次占位音频(placeholder:{uid})——3 分钟断线重连续聊的数据源。
//   - 连接失败降级：所有操作返回 false/空并打 WARN，网关功能不中断
//     （Redis 不可用时退化為纯内存态，语义见 §11 风险4）。
// ============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "engine/message.h"

struct redisContext;

namespace mediator::session {

class RedisStore {
public:
    RedisStore(std::string host = "127.0.0.1", int port = 6379);
    ~RedisStore();

    bool Connect();
    bool IsConnected() const;

    bool Execute(const RedisOp& op);              // SETEX/DEL/SETNX/GET(丢弃结果)
    std::optional<std::string> Get(const std::string& key);

private:
    std::string m_host;
    int m_port;
    redisContext* m_ctx = nullptr;
    std::mutex m_mtx; // hiredis 非线程安全，串行化
};

} // namespace mediator::session
