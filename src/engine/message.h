// ============================================================================
// message.h — 四阶段架构的数据类型契约（Skill §7）
//
// 实现思路：
//   阶段间只允许通过以下纯值类型通信，禁止共享内存隐式传信：
//     阶段1→2: Message        —— 纯值消息（无任何指针成员）
//     阶段3→4: ChangeSet      —— 本轮心跳的全部副作用（黑板变更/gRPC 调用/
//                               WSS 下发/Redis 操作/待发新消息）
//   阶段4 执行 gRPC 等异步调用，回调完成后再包装成 Message 回到阶段1，形成闭环。
//
// session_id 约定：
//   session_id 即 JWT 的 uid（用户明确指定），16 字节 SessionId 由 uid 经
//   FNV-1a 双哈希派生（SessionIdFromUid），全链路（内存黑板、gRPC
//   x-session-id metadata、Redis key）统一使用。
//
// clip_id 约定（以业务语义命名，对应设计文档 §6.2 的 A/B/C 概念）：
//   kSoothe      安抚音频（A）：说话中预生成，情绪承接，说完立即播放
//   kRestate     复述音频（B）：复述用户问题，表明理解，争取时间
//   kAnswer      答案音频（C）：大模型完整答案，最终结果
//   kPlaceholder 场景化占位音：B/C 超时兜底，由 quick 路径按上下文生成
//   kWatermark   AEC 标定水印：会话首个下行音频，不参与播放语义
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mediator {

using SessionId = std::array<uint8_t, 16>;
using ClipId = uint32_t;

// session_id 即 JWT 的 uid：16 字节 SessionId = FNV-1a(uid) 双哈希（无外部依赖）
SessionId SessionIdFromUid(const std::string& uid);

// 音频 clip 归属（业务语义命名，见文件头注释）；0=无
namespace clip {
constexpr ClipId kNone = 0;
constexpr ClipId kWatermark = 1;
constexpr ClipId kSoothe = 2;      // 安抚音频（概念 A）
constexpr ClipId kRestate = 3;     // 复述音频（概念 B）
constexpr ClipId kAnswer = 4;      // 答案音频（概念 C）
constexpr ClipId kPlaceholder = 5; // 场景化占位音
} // namespace clip

enum class MsgType : uint16_t {
    kWsAudioFrame,
    kWsControlCmd,
    kWsConnected,
    kWsDisconnected,
    kAsrPartial,
    kAsrFinal,
    kLlmQuickResp,
    kLlmRestate,
    kLlmFinalAnswer,
    kTtsAudioChunk,
    kControlAck,
    kMemoryAck,
    kWmDetected,
    kCtxRestored,          // Redis 恢复聊天上下文（text=ctx）
    kPlaceholderRestored,  // Redis 恢复上一次占位音频（payload=g711）
    kTickSessionGC,
    kTickRedisSync,
    kTickMetrics,
    kTickAecRecal,
};

// 音频帧 flags
namespace msgflag {
constexpr uint32_t kVadEnd = 1u << 0;    // VAD 断句
constexpr uint32_t kAsrEndpoint = 1u << 1; // ASR 语义断句
constexpr uint32_t kVoice = 1u << 2;     // 本帧有声
} // namespace msgflag

// 纯值语义消息：不允许指针成员（阶段1→2 契约）
struct Message {
    uint64_t msg_id = 0;
    MsgType type = MsgType::kWsAudioFrame;
    SessionId session_id{};
    int64_t ts_ms = 0;
    ClipId clip_id = clip::kNone;
    uint32_t flags = 0;
    int64_t aux = 0; // 泛用数值载荷（帧采样数 / 代际号 / 延迟采样数等）
    double dval = 0.0; // 泛用浮点载荷（skew 等）
    std::string text;
    std::vector<uint8_t> payload;
};

// ---- 阶段3→4 契约：ChangeSet ----

struct BoardMutation {
    SessionId session_id{};
    std::string field;
    std::string value;
};

struct GrpcCall {
    std::string service; // asr/llm/tts/business/memory
    std::string method;  // quick/restate/answer/quick_placeholder/...
    SessionId session_id{};
    ClipId clip_id = clip::kNone;
    std::string request_bytes;
};

struct WsOutbound {
    SessionId session_id{};
    ClipId clip_id = clip::kNone; // 下行音频归属（水印/安抚/复述/答案/占位）
    bool is_text = false;         // true=文本帧（控制ack等），false=二进制音频帧
    std::vector<uint8_t> bytes;
};

struct RedisOp {
    std::string op; // SETEX/DEL/GET/SETNX
    std::string key;
    std::string value;
    int ttl_s = 0;
};

struct ChangeSet {
    std::vector<BoardMutation> board_writes;
    std::vector<GrpcCall> grpc_calls;
    std::vector<WsOutbound> ws_sends;
    std::vector<RedisOp> redis_ops;
    std::vector<Message> new_messages;
};

} // namespace mediator
