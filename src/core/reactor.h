// ============================================================================
// reactor.h — Reactor 调度器（Skill §3.1/§3.2）
//
// 实现思路：
//   阻塞队列 + 高精度定时器 + Task 对象。IO/CPU 双队列严格分离：
//   - IO 任务（网络收发、Redis）：线程多、慢阈值 1s
//   - CPU 任务（AEC/VAD/G.711/水印检测）：线程约 1.5x 核数、慢阈值 10ms
//
// 可观测性：
//   每个 Task 带自增 task_id 与任务名；执行耗时按任务名聚合统计
//   （runs/slow_runs/total/max），超阈值经 SlowHandler 回调告警（WARN 日志
//   或 OTel task_slow_total 指标）。
//
// 当前为单线程驱动版本（RunOne 供测试与初期集成），多工作者线程的
// 派发循环在此 API 之上扩展，不改变任务语义。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "core/channel.h"
#include "core/clock.h"
#include "core/priority_timer.h"

namespace mediator {

enum class PoolKind { kIo, kCpu };

struct Task {
    uint64_t task_id = 0;
    std::string name; // 未显式命名时使用函数标识
    PoolKind pool = PoolKind::kCpu;
    std::function<void()> fn;
};

struct TaskStats {
    uint64_t runs = 0;
    uint64_t slow_runs = 0;
    double total_ms = 0.0;
    double max_ms = 0.0;
};

// Reactor 调度器：IO/CPU 双阻塞队列 + 高精度定时器 + 慢任务统计。
// 慢任务阈值：CPU >10ms / IO >1000ms（通过 on_slow 回调告警）。
class Reactor {
public:
    using SlowHandler = std::function<void(const Task&, double elapsed_ms)>;

    explicit Reactor(const IClock& clock) : m_clock(clock) {}

    void SetSlowHandler(SlowHandler h) { m_onSlow = std::move(h); }

    uint64_t Post(PoolKind pool, std::string name, std::function<void()> fn);

    // 执行下一个任务（测试用，单线程驱动）
    bool RunOne(PoolKind pool);

    // 统计与慢任务判定（纯逻辑，可测）
    bool IsSlow(PoolKind pool, double elapsed_ms) const {
        return pool == PoolKind::kCpu ? elapsed_ms > 10.0 : elapsed_ms > 1000.0;
    }

    const TaskStats* Stats(const std::string& name) const {
        auto it = m_stats.find(name);
        return it == m_stats.end() ? nullptr : &it->second;
    }

    PriorityTimer& Timer() { return m_timer; }

private:
    void RecordRun(const Task& t, double elapsed_ms);

    const IClock& m_clock;
    std::atomic<uint64_t> m_nextTaskId{1};
    CopyChannel<Task> m_ioQueue;
    CopyChannel<Task> m_cpuQueue;
    PriorityTimer m_timer;
    SlowHandler m_onSlow;
    std::unordered_map<std::string, TaskStats> m_stats;
};

} // namespace mediator
