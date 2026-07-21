// ============================================================================
// wasm_bus.cpp — 观察者消息总线实现
//
// 熔断：单模块连续 trap 达 kMaxTraps 次 → 熔断 kFuseMs（期间 Notify 跳过），
// 到期自动半开（下次调用成功则清零计数）。时间用 steady_clock。
// ============================================================================
#include "ext/wasm_bus.h"

#include <chrono>

#include "core/log.h"

namespace mediator::ext {

namespace {
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

bool WasmBus::Subscribe(const std::string& name, const std::string& path) {
    auto mod = std::make_unique<Wasm3Module>();
    if (!mod->LoadFromFile(path)) {
        MDT_WARN("wasm observer {} load failed: {}", name, mod->LastError());
        return false;
    }
    if (!mod->HasFunction("on_message")) {
        MDT_WARN("wasm observer {} missing export on_message", name);
        return false;
    }
    m_observers[name] = Observer{std::move(mod), 0, 0};
    MDT_INFO("wasm observer subscribed: {}", name);
    return true;
}

void WasmBus::Unsubscribe(const std::string& name) { m_observers.erase(name); }

void WasmBus::Notify(const Message& msg) {
    const int64_t now = NowMs();
    for (auto& [name, ob] : m_observers) {
        if (ob.fuse_until_ms > now) continue; // 熔断期跳过
        const bool ok = ob.mod->CallOnMessage(static_cast<int32_t>(msg.type),
                                              msg.payload, kPayloadOffset);
        if (ok) {
            ob.traps = 0;
            ob.fuse_until_ms = 0;
        } else {
            ++ob.traps;
            MDT_WARN("wasm observer {} trap {}/{}", name, ob.traps, kMaxTraps);
            if (ob.traps >= kMaxTraps) {
                ob.fuse_until_ms = now + kFuseMs;
                MDT_ERROR("wasm observer {} fused for {}ms", name, kFuseMs);
            }
        }
    }
}

size_t WasmBus::ObserverCount() const { return m_observers.size(); }

size_t WasmBus::TrapCount(const std::string& name) const {
    auto it = m_observers.find(name);
    return it == m_observers.end() ? 0 : static_cast<size_t>(it->second.traps);
}

} // namespace mediator::ext
