// ============================================================================
// thread_pool.h — 极简线程池（阶段4 副作用异步执行用）
//
// 实现思路：固定 N 个工作者线程从 CopyChannel 阻塞取任务执行。
// 阶段4 的 gRPC 同步调用在此池执行，回调完成后再 Inject 消息回引擎，
// 保证心跳线程永不阻塞（Skill §1 阶段4 异步化要求）。
// ============================================================================
#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "core/channel.h"

namespace mediator {

class ThreadPool {
public:
    explicit ThreadPool(int workers) { Start(workers); }
    ThreadPool() = default;
    ~ThreadPool() { Stop(); }

    void Start(int workers) {
        if (!m_threads.empty()) return;
        for (int i = 0; i < workers; ++i)
            m_threads.emplace_back([this] {
                std::function<void()> fn;
                while (m_queue.Recv(fn)) {
                    if (fn) fn();
                }
            });
    }

    void Post(std::function<void()> fn) { m_queue.Send(std::move(fn)); }

    void Stop() {
        m_queue.Close();
        for (auto& t : m_threads)
            if (t.joinable()) t.join();
        m_threads.clear();
    }

private:
    CopyChannel<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
};

} // namespace mediator
