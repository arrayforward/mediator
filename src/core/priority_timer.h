// ============================================================================
// priority_timer.h — 高精度定时器（Skill §3.3）
//
// 实现思路：
//   基于 std::priority_queue（按到期时间小顶堆），RunExpired(now) 弹堆执行
//   所有到期任务。
//
// 跳过策略（自适应压力保护）：
//   任务分两类——skippable（巡检类：会话GC扫描、AEC重标定、指标同步）与
//   不可跳过（消息处理）。系统繁忙导致任务延迟超过阈值（默认 1s）时，
//   skippable 任务直接丢弃本次执行；消息处理任务无论多晚都必须执行。
//   依据：定时任务不携带业务数据，只触发"去黑板查"的动作，跳过不破坏
//   业务正确性；而消息按序处理是正确性的底线。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <string>
#include <vector>

namespace mediator {
class PriorityTimer {
public:
    struct TimerTask {
        uint64_t id;
        int64_t due_ms;
        std::string name;
        bool skippable;
        std::function<void()> fn;
    };

    struct Greater {
        bool operator()(const TimerTask& a, const TimerTask& b) const {
            return a.due_ms > b.due_ms;
        }
    };

    explicit PriorityTimer(int64_t skip_threshold_ms = 1000)
        : m_skipThresholdMs(skip_threshold_ms) {}

    uint64_t Schedule(int64_t due_ms, std::string name, bool skippable,
                      std::function<void()> fn) {
        const uint64_t id = m_nextId++;
        m_pq.push(TimerTask{id, due_ms, std::move(name), skippable, std::move(fn)});
        return id;
    }

    // 取回到期任务并执行；返回 {executed, skipped} 计数
    std::pair<size_t, size_t> RunExpired(int64_t now_ms) {
        size_t executed = 0, skipped = 0;
        while (!m_pq.empty() && m_pq.top().due_ms <= now_ms) {
            TimerTask t = std::move(const_cast<TimerTask&>(m_pq.top()));
            m_pq.pop();
            const int64_t lateness = now_ms - t.due_ms;
            if (t.skippable && lateness > m_skipThresholdMs) {
                ++skipped;
                continue;
            }
            if (t.fn) t.fn();
            ++executed;
        }
        return {executed, skipped};
    }

    size_t Pending() const { return m_pq.size(); }

private:
    int64_t m_skipThresholdMs;
    uint64_t m_nextId = 1;
    std::priority_queue<TimerTask, std::vector<TimerTask>, Greater> m_pq;
};

} // namespace mediator
