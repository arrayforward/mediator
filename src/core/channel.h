// ============================================================================
// channel.h — CSP 风格值语义消息队列（Skill §5.2）
//
// 实现思路：
//   跨线程传递消息的唯一合法通道。入队时深拷贝（或移动构造）进内部缓冲区，
//   出队时再移动交付给消费线程，从语义上切断跨线程指针共享。
//
// 关键接口：
//   Send(const T&)/Send(T&&)  —— 拷贝/移动入队，通知一个等待者
//   Recv / TryRecv            —— 阻塞/非阻塞取出
//   SwapOutAll                —— 心跳批处理专用：一次性取空队列（§阶段2→3 契约
//                                "std::vector<Message> 整批交付"）
//
// 内存策略：以 deque 为缓冲区，空闲时 shrink 超基线部分（泡泡对象快速释放，
// 基线容量保留避免高频分配）。
// ============================================================================
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace mediator {
template <typename T>
class CopyChannel {
public:
    explicit CopyChannel(size_t baseline_capacity = 64) : m_baseline(baseline_capacity) {}

    CopyChannel(const CopyChannel&) = delete;
    CopyChannel& operator=(const CopyChannel&) = delete;

    void Send(const T& item) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.push_back(item);
        }
        m_cv.notify_one();
    }

    void Send(T&& item) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.push_back(std::move(item));
        }
        m_cv.notify_one();
    }

    bool Recv(T& out) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cv.wait(lk, [this] { return !m_queue.empty() || m_closed; });
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    bool TryRecv(T& out) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_queue.empty()) return false;
        out = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    // 一次性取出全部未处理消息（心跳批处理用）
    size_t SwapOutAll(std::vector<T>& out) {
        std::lock_guard<std::mutex> lk(m_mtx);
        const size_t n = m_queue.size();
        out.reserve(out.size() + n);
        while (!m_queue.empty()) {
            out.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        ShrinkIfIdleLocked();
        return n;
    }

    size_t Depth() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_queue.size();
    }

    void Close() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closed = true;
        }
        m_cv.notify_all();
    }

private:
    void ShrinkIfIdleLocked() {
        // 空闲时收缩超基线部分（deque 自动管理，此处保留语义钩子）
        if (m_queue.size() < m_baseline) m_queue.shrink_to_fit();
    }

    size_t m_baseline;
    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
    bool m_closed = false;
};

} // namespace mediator
