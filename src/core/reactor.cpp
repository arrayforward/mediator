#include "core/reactor.h"

namespace mediator {

uint64_t Reactor::Post(PoolKind pool, std::string name, std::function<void()> fn) {
    Task t;
    t.task_id = m_nextTaskId.fetch_add(1);
    t.name = std::move(name);
    t.pool = pool;
    t.fn = std::move(fn);
    const uint64_t id = t.task_id;
    if (pool == PoolKind::kIo)
        m_ioQueue.Send(std::move(t));
    else
        m_cpuQueue.Send(std::move(t));
    return id;
}

bool Reactor::RunOne(PoolKind pool) {
    Task t;
    auto& q = (pool == PoolKind::kIo) ? m_ioQueue : m_cpuQueue;
    if (!q.TryRecv(t)) return false;
    const int64_t start = m_clock.NowMs();
    if (t.fn) t.fn();
    const double elapsed = static_cast<double>(m_clock.NowMs() - start);
    RecordRun(t, elapsed);
    if (IsSlow(pool, elapsed) && m_onSlow) m_onSlow(t, elapsed);
    return true;
}

void Reactor::RecordRun(const Task& t, double elapsed_ms) {
    auto& s = m_stats[t.name];
    ++s.runs;
    s.total_ms += elapsed_ms;
    if (elapsed_ms > s.max_ms) s.max_ms = elapsed_ms;
    if (IsSlow(t.pool, elapsed_ms)) ++s.slow_runs;
}

} // namespace mediator
