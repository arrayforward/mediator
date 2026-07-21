// ============================================================================
// audio_pipeline.cpp — 每会话 actor 音频管线实现
//
// 关键逻辑：
//   LaneOf：按 sid 取/建会话 lane（map 锁保护；APM 实例随 lane 构造）。
//   Schedule：CAS 抢占执行权，成功才向 CPU 池投 Drain —— 保证同会话
//   任意时刻至多一个执行者（APM 单线程所有权）。
//   Drain：TryRecv 清空队列；结束前 CAS 释放执行权，并复查队列（与
//   Post 的竞态：释放后新任务会重新 Schedule，不丢任务）。
//   RemoveSession：lane 无人执行时可安全删除；正被执行则标记后由
//   Drain 结束时代删（会话超时 GC 场景无新任务，直接删除）。
// ============================================================================
#include "audio/audio_pipeline.h"

#include "core/log.h"
#include "net/grpc_clients.h" // SidHex

namespace mediator::audio {

AudioPipeline::AudioPipeline(int workers) { m_pool.Start(workers > 0 ? workers : 1); }

AudioPipeline::~AudioPipeline() { m_pool.Stop(); }

std::shared_ptr<AudioPipeline::SessionLane> AudioPipeline::LaneOf(const SessionId& sid) {
    std::lock_guard<std::mutex> lk(m_mtx);
    auto& slot = m_sessions[net::SidHex(sid)];
    if (!slot) slot = std::make_shared<SessionLane>();
    return slot;
}

void AudioPipeline::PostRender(const SessionId& sid, std::vector<int16_t> pcm) {
    auto lane = LaneOf(sid);
    if (lane->queue.Depth() > kRenderDropThreshold) {
        m_renderDropped.fetch_add(1); // 背压：丢 render，保 capture
        return;
    }
    AudioTask t;
    t.is_render = true;
    t.pcm = std::move(pcm);
    lane->queue.Send(std::move(t));
    Schedule(std::move(lane));
}

void AudioPipeline::PostCapture(const SessionId& sid, std::vector<int16_t> pcm,
                                std::function<void(const ApmResult&)> on_done) {
    auto lane = LaneOf(sid);
    AudioTask t;
    t.is_render = false;
    t.pcm = std::move(pcm);
    t.on_capture = std::move(on_done);
    lane->queue.Send(std::move(t));
    Schedule(std::move(lane));
}

void AudioPipeline::Schedule(std::shared_ptr<SessionLane> lane) {
    bool expected = false;
    if (!lane->scheduled.compare_exchange_strong(expected, true)) return; // 已有人在跑
    // shared_ptr 随任务持有：GC 删除 map 项也不会 UAF
    m_pool.Post([this, lane = std::move(lane)] { Drain(*lane); });
}

void AudioPipeline::Drain(SessionLane& lane) {
    AudioTask t;
    while (lane.queue.TryRecv(t)) {
        if (t.is_render) {
            lane.apm.ProcessRender(t.pcm);
        } else {
            const auto res = lane.apm.ProcessCapture(t.pcm);
            if (t.on_capture) t.on_capture(res);
        }
    }
    lane.scheduled.store(false);
    // 复查：释放执行权到此刻之间可能有新任务入队且未获调度
    if (lane.queue.Depth() > 0) {
        bool expected = false;
        if (lane.scheduled.compare_exchange_strong(expected, true))
            m_pool.Post([this, lane = lane.shared_from_this()] { Drain(*lane); });
    }
}

void AudioPipeline::SetCalib(const SessionId& sid, ApmCalib calib) {
    // ApmWrapper::SetCalib 内部有锁，与在跑的 Drain 安全并发
    LaneOf(sid)->apm.SetCalib(calib);
}

void AudioPipeline::RemoveSession(const SessionId& sid) {
    // 会话超时 GC：从 map 移除（lane 可能被在跑的 Drain 持有，shared_ptr
    // 保证其安全结束；APM 状态随最后引用销毁而清除）
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.erase(net::SidHex(sid));
    MDT_DEBUG("audio pipeline session removed");
}

size_t AudioPipeline::SessionCount() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_sessions.size();
}

} // namespace mediator::audio
