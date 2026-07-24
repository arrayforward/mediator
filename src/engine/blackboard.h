// ============================================================================
// blackboard.h — 数据黑板（Skill §4 Blackboard Pattern）
//
// 实现思路：
//   所有可能被多线程访问的数据集中在 DataBoard 单例，按业务对象分区：
//   - SessionContext（每会话一个）：会话全生命周期状态（见下）
//   - OnlineRegistry：uid → {网关实例, 代际号}，防多重登录
//
// 并发规则（Skill §4.2）：
//   黑板数据只允许心跳线程写入（单写者天然无冲突）；其他线程只读快照
//   或通过 Message 传值。MarkChanged/TakeChangedForEvolve 实现"单层数据
//   演进"：本轮心跳的变更集在演进开始时被取走，演进过程中新产生的变更
//   留待下一轮，防止连锁触发（ECA 单层原则）。
//
// SessionContext 关键状态：
//   m_connGeneration  连接代际：重连 gen+1，旧 gen 消息（含迟到的断连）直接丢弃
//   m_wmPending       水印标定期：上行语音帧直接丢弃，不识别（随连接复位）
//   m_aecCalib        AEC 对齐参数 {delay_samples, skew}（水印标定结果，随连接失效重标）
//   m_playQueue       下行播放队列，严格按 水印/占位/安抚/复述/答案 顺序下发
//   m_lastPlaceholder 上一次占位音频缓存（A 未就绪时复用，时延≈0）
//   m_chatCtx         聊天上下文 ≤1MB，超限淘汰最早对话至 50%
// ============================================================================
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/message.h"

namespace mediator {

enum class SessionState { kIdle, kListening, kThinking, kSpeaking, kOffline };

struct ClipBuffer {
    ClipId id = clip::kNone;
    std::string text;               // 对应文本（占位音频复用时需要）
    std::vector<uint8_t> g711;      // 编码后的音频
    size_t sent_bytes = 0;
    bool text_ready = false;
    bool audio_ready = false;
    int64_t requested_ms = 0;       // LLM 请求时刻（超时检测）
};

struct SessionContext {
    SessionId m_sessionId{};
    std::string m_uid;
    uint64_t m_connGeneration = 0;
    SessionState m_state = SessionState::kIdle;

    // 上行
    int64_t m_uttStartMs = 0;
    int64_t m_lastVoiceMs = 0;
    bool m_uttActive = false;
    bool m_vadEndpoint = false;
    bool m_quickRespSubmitted = false;
    bool m_aFallbackDone = false;   // A 段失败兜底已触发（每语句一次，防重复占位）
    bool m_degradedSent = false;    // 全失败 degraded 状态已下发（每语句一次）
    uint64_t m_droppedFramesWm = 0;
    uint64_t m_uttGen = 0;   // 语句代际：打断(barge-in)时 +1，迟到 LLM/TTS 结果丢弃
    int m_voiceRun = 0;      // 连续有声帧计数（APM VAD，打断触发依据）
    int64_t m_voiceRunStartMs = 0; // 当前连续有声段起点（打断最短持续计时）
    int64_t m_thinkingStartMs = 0; // 最近一次 AsrFinal 时刻（回复保护窗起点）
    int64_t m_replyStartedMs = 0;  // 回复开播时刻（首个非占位 clip 下发；0=未开播）

    // AEC 标定（§6.5）
    struct AecCalib {
        bool valid = false;
        int32_t delay_samples = 0;
        double skew = 0.0;
    } m_aecCalib;
    bool m_wmPending = false; // 水印标定进行中：上行语音直接丢弃
    int64_t m_wmSendAtMs = 0; // 水印计划下发时刻（连接后延迟首播：等端侧
                              // 录音通道/AGC 稳定；0=无待发水印）

    // 下行播放队列（严格顺序 WM/A/B/C/P）
    std::deque<ClipBuffer> m_playQueue;
    ClipId m_nextClipSeq = clip::kSoothe;
    int m_placeholderRounds = 0;

    // 占位音频复用缓存（上一次对话的占位提示音）
    ClipBuffer m_lastPlaceholder;
    bool m_hasLastPlaceholder = false;

    // 上下文（≤1MB）
    std::string m_chatCtx;
    int64_t m_lastSeenMs = 0;

    // 最近播报文本环形缓冲（≤8 条，A/B/C/P 全量）——回声抑制：
    // AEC 未校准/旁路时扬声器回放被 mic 收回，ASR 会识别出自己刚播的话，
    // 与该缓冲高度相似的 final 判定为回声直接丢弃（防自我对话循环）
    std::deque<std::string> m_recentSpoken;
};

struct OnlineEntry {
    std::string gw_id;
    uint64_t generation = 0;
};

struct SessionIdHash {
    size_t operator()(const SessionId& s) const {
        size_t h = 0;
        for (auto b : s) h = h * 131 + b;
        return h;
    }
};

// 数据黑板：共享数据唯一写者为心跳线程（Engine 内部调用）。
class DataBoard {
public:
    static constexpr size_t kMaxCtxBytes = 1u << 20; // 1MB

    SessionContext& GetOrCreateSession(const SessionId& sid);
    SessionContext* FindSession(const SessionId& sid);
    void RemoveSession(const SessionId& sid);
    size_t SessionCount() const { return m_sessions.size(); }

    // 在线注册（防重登）
    bool RegisterOnline(const std::string& uid, const std::string& gw, uint64_t gen);
    void SetOffline(const std::string& uid);
    std::optional<OnlineEntry> Lookup(const std::string& uid) const;

    // 演进单层触发：本轮变更集
    void MarkChanged(const SessionId& sid) { m_changed.insert(sid); }
    std::unordered_set<SessionId, SessionIdHash> TakeChangedForEvolve();

    // 测试钩子
    void Inject(const SessionId& sid, const SessionContext& ctx);

    // 全表遍历（GC 巡检等；仅心跳线程调用）
    template <typename Fn>
    void ForEachSession(Fn&& fn) {
        for (auto& [sid, ctx] : m_sessions) fn(sid, ctx);
    }

private:
    std::unordered_map<SessionId, SessionContext, SessionIdHash> m_sessions;
    std::unordered_map<std::string, OnlineEntry> m_online;
    std::unordered_set<SessionId, SessionIdHash> m_changed;
};

} // namespace mediator
