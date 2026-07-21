// ============================================================================
// audio_pipeline.h — 音频 DSP 异步执行管线（每会话 actor + CPU 池调度）
//
// 架构决策（设计文档 §3.1 CPU 池-音频）：
//   - **一个会话上下文一个 APM 实例**：AEC 滤波器收敛、DelayLine、
//     水印标定 {delay,skew} 均为会话私有且连续演化，webrtc 不支持状态
//     导出/迁移 → APM 实例不池化，随会话 3 分钟超时 GC 一并清除。
//   - **每会话一条 CopyChannel 任务队列**（actor 模型，无哈希车道）：
//     同会话任务严格 FIFO → APM 只被单个执行者触碰（webrtc 非线程安全
//     约束天然满足），render/capture 的"先 reverse 后 capture"由 FIFO
//     保证；不同会话由 CPU 池并行执行，无任何哈希碰撞/共享车道。
//   - 调度：任务入队时若该会话无在执行者（scheduled 标志 CAS），向
//     ThreadPool 投递 drain 任务；drain 清空队列后释放标志。并发上限
//     即池大小，会话数不受限（队列随会话创建/销毁）。
//   - 背压：会话渲染任务积压超阈值丢最老 render（AEC 容忍少量参考
//     缺失）并计数；capture 永不丢弃（消息不可跳过原则）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio/apm_wrapper.h"
#include "core/channel.h"
#include "core/thread_pool.h"
#include "engine/message.h"

namespace mediator::audio {

struct AudioTask {
    bool is_render = false;                  // true=下行参考，false=上行采集
    std::vector<int16_t> pcm;
    std::function<void(const ApmResult&)> on_capture; // 上行结果回调
};

class AudioPipeline {
public:
    // workers: CPU 池线程数（建议 1.5×核数）
    explicit AudioPipeline(int workers);
    ~AudioPipeline();

    static constexpr size_t kRenderDropThreshold = 256; // 会话渲染任务积压阈值

    void PostRender(const SessionId& sid, std::vector<int16_t> pcm);
    void PostCapture(const SessionId& sid, std::vector<int16_t> pcm,
                     std::function<void(const ApmResult&)> on_done);

    // 水印标定结果注入（kWmDetected 时调用，可在任务执行前更新参数）
    void SetCalib(const SessionId& sid, ApmCalib calib);

    // 会话超时 GC：销毁其 APM 实例与任务队列（调用方保证该会话已无新任务）
    void RemoveSession(const SessionId& sid);

    uint64_t RenderDropped() const { return m_renderDropped.load(); }
    size_t SessionCount() const;

private:
    // lane 用 shared_ptr：GC 删除 map 项时在跑的 Drain 仍安全持有
    struct SessionLane : std::enable_shared_from_this<SessionLane> {
        ApmWrapper apm;              // 会话私有 APM 实例
        CopyChannel<AudioTask> queue;
        std::atomic<bool> scheduled{false};
        SessionLane() : apm(audio::ApmCalib{}) {}
    };

    std::shared_ptr<SessionLane> LaneOf(const SessionId& sid);
    void Schedule(std::shared_ptr<SessionLane> lane);
    void Drain(SessionLane& lane);

    ThreadPool m_pool;
    mutable std::mutex m_mtx; // 仅保护 map（lane 本体单执行者）
    std::unordered_map<std::string, std::shared_ptr<SessionLane>> m_sessions;
    std::atomic<uint64_t> m_renderDropped{0};
};

} // namespace mediator::audio
