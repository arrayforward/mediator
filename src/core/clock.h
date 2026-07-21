// ============================================================================
// clock.h — 可注入时钟抽象（Skill §6 可测试性强制项）
//
// 实现思路：
//   全系统禁止直接持有真实时钟，统一通过 IClock 接口取时间。
//   生产环境注入 SteadyClock（单调时钟，不受系统时间回拨影响）；
//   单元测试注入 VirtualClock，用 Advance() 推进虚拟时间，
//   从而可以在不真实等待的情况下测试"3 分钟会话 GC"、"5 秒触发音频A"、
//   "B/C 超时占位"等时间敏感逻辑，并保证心跳的确定性重放。
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>

namespace mediator {

class IClock {
public:
    virtual ~IClock() = default;
    virtual int64_t NowMs() const = 0;
};

class SteadyClock final : public IClock {
public:
    int64_t NowMs() const override;
};

class VirtualClock final : public IClock {
public:
    explicit VirtualClock(int64_t start_ms = 0) : m_nowMs(start_ms) {}

    int64_t NowMs() const override { return m_nowMs; }
    void Advance(int64_t delta_ms) { m_nowMs += delta_ms; }
    void Set(int64_t now_ms) { m_nowMs = now_ms; }

private:
    int64_t m_nowMs;
};

} // namespace mediator
