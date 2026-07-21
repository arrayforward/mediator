// ============================================================================
// heartbeat_engine.h — 消息处理心跳引擎（阶段3，系统核心）
//
// 实现思路（Skill §1/§2 Game Loop + ECA）：
//   RunOnce() 为一轮完整心跳，分三步：
//     1. SwapOutAll 一次性取走入站队列全部消息（整批交付）
//     2. ProcessOne 逐条处理：更新黑板、收集副作用进 ChangeSet
//     3. EvolveOnce 单层数据演进：只扫描本轮 TakeChangedForEvolve 快照
//   本轮产生的新消息/新变更一律留待下一轮心跳（无递归、复杂度可控）。
//
// 核心业务逻辑分布：
//   消息处理 —— 连接建立(发AEC水印)、音频帧(标定期丢弃/有声累积/VAD断句)、
//              ASR Final(并行请求 LLM restate+answer)、LLM 文本(触发 TTS)、
//              TTS 音频块(入播放队列/占位音持久缓存)、水印检测结果
//   演进组件 —— QuickRespTrigger(>5s或首次断句→请求安抚音频A)、
//              PlaybackScheduler(严格按序下发; B/C 超时→场景化占位;
//              A 未就绪→复用上一次占位音; 超2轮→stall)、
//              SessionGC(离线>3min→记忆服务+清上下文)、
//              CtxEvict(>1MB→淘汰至50%并写Redis)
//
// 确定性保证（Skill §6）：同一初始黑板 + 同一消息批 → 相同 ChangeSet；
// 时间全部来自注入的 IClock，无任何随机数与真实时钟泄漏。
// ============================================================================
#pragma once

#include <cstdint>

#include "core/channel.h"
#include "core/clock.h"
#include "engine/blackboard.h"
#include "engine/message.h"

namespace mediator {

struct EngineConfig {
    int64_t utt_quick_ms = 5000;        // 说话超此时长触发音频A
    int64_t b_timeout_ms = 8000;        // 音频B超时阈值
    int64_t c_timeout_ms = 15000;       // 音频C超时阈值
    int64_t session_gc_ms = 180000;     // 3分钟离线清理
    int max_placeholder_rounds = 2;     // 占位音频最大轮数
    size_t max_ctx_bytes = 1u << 20;    // 1MB 上下文
    double ctx_target_ratio = 0.5;      // 淘汰至 50%
    std::string gw_id = "gw-local";
};

// 心跳引擎（阶段3）：单线程唯一共享数据写者。
// RunOnce() 为测试钩子：取全部未处理消息 → 批处理 → 单层演进 → 返回 ChangeSet。
class HeartbeatEngine {
public:
    HeartbeatEngine(EngineConfig cfg, const IClock& clock)
        : m_cfg(std::move(cfg)), m_clock(clock) {}

    // 阶段1 注入入口（测试可直接 Inject）
    void Inject(const Message& m) { m_inbound.Send(m); }
    void Inject(Message&& m) { m_inbound.Send(std::move(m)); }

    // 执行一轮心跳，返回本轮副作用集合（确定性：同输入同输出）
    ChangeSet RunOnce();

    DataBoard& Board() { return m_board; }
    size_t InboundDepth() const { return m_inbound.Depth(); }

private:
    void ProcessOne(const Message& m, ChangeSet& cs);
    void EvolveOnce(ChangeSet& cs);

    // 消息处理子例程
    void OnWsConnected(const Message& m, ChangeSet& cs);
    void OnWsDisconnected(const Message& m, ChangeSet& cs);
    void OnAudioFrame(const Message& m, ChangeSet& cs);
    void OnAsrFinal(const Message& m, ChangeSet& cs);
    void OnLlmText(const Message& m, ChangeSet& cs);
    void OnTtsChunk(const Message& m, ChangeSet& cs);
    void OnWmDetected(const Message& m, ChangeSet& cs);

    // 演进组件（单层：只处理 TakeChangedForEvolve 的快照）
    void EvolveQuickResp(SessionContext& s, int64_t now, ChangeSet& cs);
    void EvolvePlayback(SessionContext& s, int64_t now, ChangeSet& cs);
    void EvolveSessionGc(int64_t now, ChangeSet& cs);
    void EvolveCtxEvict(SessionContext& s, ChangeSet& cs);

    void EmitTts(SessionContext& s, ClipId clip, const std::string& text, ChangeSet& cs);
    static ClipBuffer* FindClip(SessionContext& s, ClipId id);
    void EnqueueReusePlaceholder(SessionContext& s, ChangeSet& cs);

    EngineConfig m_cfg;
    const IClock& m_clock;
    DataBoard m_board;
    CopyChannel<Message> m_inbound;
    uint64_t m_nextMsgId = 1;
};

} // namespace mediator
