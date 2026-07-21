// ============================================================================
// wasm_bus.h — wasm 观察者消息总线（设计文档 §7.1/§6 扩展性）
//
// 实现思路：
//   观察者模块导出：
//     memory                                     —— 线性内存（消息载荷写入区）
//     on_message(type:i32, ptr:i32, len:i32)     —— 每条消息回调
//   宿主把 Message 的 payload 写入固定偏移（kPayloadOffset），调用
//   on_message(msg_type, offset, len)。v1 为只读观察者（审计/计数/
//   画像等），返回值忽略；产生新消息的双向 ABI（host_emit_message）
//   在同构接口上扩展。
//
// 资源与故障隔离（§7.2）：调用在宿主线程内但 wasm3 解释执行带栈约束；
// trap 计数熔断（连续 N 次 trap 后暂停该模块 30s，由 Notify 内自恢复）。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/message.h"
#include "ext/wasm3_host.h"

namespace mediator::ext {

class WasmBus {
public:
    // 订阅：加载模块并注册为观察者；模块需导出 on_message（缺失返回 false）
    bool Subscribe(const std::string& name, const std::string& path);
    void Unsubscribe(const std::string& name);

    // 派发到所有未熔断观察者（心跳后调用；trpa 自动计数熔断）
    void Notify(const Message& msg);

    size_t ObserverCount() const;
    size_t TrapCount(const std::string& name) const;

    static constexpr uint32_t kPayloadOffset = 4096;

private:
    struct Observer {
        std::unique_ptr<Wasm3Module> mod;
        int traps = 0;
        int64_t fuse_until_ms = 0; // 熔断到期（steady ms）
    };
    std::unordered_map<std::string, Observer> m_observers;
    static constexpr int kMaxTraps = 3;
    static constexpr int64_t kFuseMs = 30000;
};

} // namespace mediator::ext
