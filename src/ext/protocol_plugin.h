// ============================================================================
// protocol_plugin.h — wasm 协议插件宿主（协议路由 + 观察者模式）
//
// 架构（联调方案修订：核心代码零协议，协议全部在 wasm 插件内）：
//   WsServer 握手时按 Sec-WebSocket-Protocol 查配置路由表
//   （--protocol=<subproto>:<plugin.wasm>），命中即进入插件会话：
//   宿主只做"裸帧搬运 + 协议无关原语"，不认识任何协议字段。
//
//   宿主派发（全部原始载荷，不解析）：
//     on_ws_text(ptr,len)              —— 文本帧原文
//     on_ws_binary(ptr,len)            —— 二进制帧原文（头也由插件解析）
//     on_outbound_clip(clip,ptr,len)   —— 引擎下行 clip（16k G.711A）
//     on_outbound_text(ptr,len)        —— 引擎下行文本
//     on_llm_text(ptr,len)             —— LLM 文本
//     on_thinking()                    —— ASR Final
//
//   env.* 原语（协议无关）：
//     host_send_text / host_send_binary     —— 裸帧下发
//     host_inject(type,flags,ptr,len)       —— 注引擎消息
//     host_json_get(jp,jl,kp,kl,out)        —— JSON 取字段（泛型工具）
//     host_resample_g711(in,len,from,to,out)—— 泛型 G.711A 重采样
//     host_bind_session / host_close / host_feed_watermark / host_uplink_asr
//     host_now_ms() -> i64
//
// 内存布局约定（插件 ABI 一部分）：
//   [8192,...)    宿主写入的入站载荷（调用导出前）
//   [12288,...)   转换输出（重采样等）
//   [1024,1088)   配置槽：[u32 len][bytes]（如设备 Key，SetConfigKey 注入）
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/message.h"
#include "ext/wasm3_host.h"

namespace mediator::ext {

class ProtocolPlugin {
public:
    ProtocolPlugin();
    ~ProtocolPlugin();

    // 每连接上下文（由 WsServer 提供，全部在同一线程调用）
    struct Hooks {
        std::function<void(const std::string&)> send_text;          // 裸文本帧
        std::function<void(const std::vector<uint8_t>&)> send_binary; // 裸二进制帧
        std::function<void(MsgType, uint32_t, const std::string&)> inject; // 注引擎
        std::function<void(const std::string&)> bind_session;       // 绑定 uid
        std::function<void(uint16_t)> close;                        // 关闭连接
        std::function<bool(const uint8_t*, size_t)> feed_watermark; // 标定: true=完成
        std::function<void(const std::vector<int16_t>&, uint32_t)> forward_asr;
        std::function<bool()> is_calibrated;
    };

    bool Load(const std::string& path, Hooks hooks);
    // 注入配置槽内容（如设备 Key；插件 ABI 约定偏移，≤64B）
    bool SetConfigKey(const std::string& key);
    bool Loaded() const { return m_mod != nullptr; }
    const std::string& LastError() const { return m_lastError; }

    // ---- 帧事件派发（WsServer 调用，原始载荷）----
    void OnWsText(const std::string& data);
    void OnWsBinary(const uint8_t* data, size_t len);
    void OnOutboundClip(ClipId clip, const std::vector<uint8_t>& g711_16k);
    void OnOutboundText(const std::string& text);
    void OnLlmText(const std::string& text);
    void OnThinking();
    void OnInterrupted(); // 打断：端侧丢弃本地播放缓冲

    static constexpr uint32_t kInOffset = 8192;
    static constexpr uint32_t kConvertOffset = 12288;
    static constexpr uint32_t kConfigKeyOffset = 1024; // [u32 len][bytes]

    // ---- 宿主原语实现（env.*，供 wasm3 raw 函数回调）----
    int32_t HostSendText(uint32_t ptr, uint32_t len);
    int32_t HostSendBinary(uint32_t ptr, uint32_t len);
    int32_t HostInject(uint32_t type, uint32_t flags, const std::string& text);
    int32_t HostJsonGet(const std::string& json, const std::string& key, uint32_t out);
    int32_t HostResampleG711(uint32_t ip, uint32_t il, uint32_t from, uint32_t to,
                             uint32_t out);
    int32_t HostBindSession(const std::string& uid);
    int32_t HostClose(uint16_t code);
    int32_t HostUplinkAsr(uint32_t flags, uint32_t ptr, uint32_t len);
    bool HostFeedWatermark(uint32_t ptr, uint32_t len);
    int64_t HostNowMs() const;

    // raw 函数需要访问本实例（wasm3 无 userdata，用 thread_local）
    void SetActiveThis();

private:
    bool WriteIn(const void* data, size_t len);

    std::unique_ptr<Wasm3Module> m_mod;
    Hooks m_hooks;
    std::string m_lastError;
};

} // namespace mediator::ext
