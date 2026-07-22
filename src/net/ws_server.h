// ============================================================================
// ws_server.h — WSS 接入服务器（Boost.Beast + OpenSSL，设计文档 §8）
//
// 实现思路：
//   TLS 1.2+ 之上的 WebSocket。每连接一个线程（E2E 规模足够；生产按 §3.1
//   IO 池异步化扩展，接口不变）。
//
// 协议（与 e2e_client 约定）：
//   文本帧1：认证 token（裸字符串）。AuthProvider 验签：
//     通过 → 回 {"ok":true,"uid":"..."}，注入 kWsConnected(text=uid, aux=gen)
//     失败 → 回 {"ok":false,"reason":"..."} 立即关闭（fail-closed）
//   二进制上行帧：[flags:u32 LE][G.711A 音频]；flags 同 msgflag
//   二进制下行帧：[clip_id:u32 LE][G.711A 音频]（WsOutbound.is_text=false）
//   文本下行帧：控制回执等（is_text=true）
//
// AEC 水印标定（§6.5 在接入层的落点）：
//   会话未标定时，上行帧解码 G.711→PCM 累积进标定缓冲，逐块跑
//   DetectWatermark；命中后注入 kWmDetected(delay, skew)。
//   标定期语音同时被引擎丢弃（m_wmPending），不送 ASR——双保险。
//   注：检测当前在连接读线程执行（E2E 规模）；生产按 §3.1 分发 CPU 池。
//
// 代际号 gen：服务器按 uid 单调递增分配，随 kWsConnected 传给引擎，
// 旧连接（小 gen）消息被引擎丢弃，防重登污染。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "audio/watermark.h"
#include "engine/message.h"
#include "ext/auth_provider.h"
#include "ext/protocol_plugin.h"

namespace mediator::net {

struct WsCallbacks {
    std::function<void(Message&&)> inject; // → 引擎入站队列
    // 已标定会话的有声帧 → ASR 流（阶段2 轻量转发）
    std::function<void(const SessionId&, const std::vector<int16_t>&, uint32_t)> on_audio;
};

class WsServer {
public:
    WsServer(uint16_t port, std::string cert_file, std::string key_file,
             ext::AuthProvider* auth, WsCallbacks cb);
    ~WsServer();

    void Start();
    void Stop();

    // 协议路由：Sec-WebSocket-Protocol → wasm 插件路径（核心零协议，
    // 协议解析全部在插件内，观察者模式）；key 为插件配置槽内容（如设备 Key）
    void SetProtocolRoute(std::string subprotocol, std::string plugin_path) {
        m_protocolRoutes[std::move(subprotocol)] = std::move(plugin_path);
    }
    void SetProtocolKey(std::string key) { m_protocolKey = std::move(key); }

    void SendBinary(const SessionId& sid, ClipId clip, const std::vector<uint8_t>& bytes);
    void SendText(const SessionId& sid, const std::string& text);
    // 引擎事件通知（协议插件连接转换为下行帧；普通连接忽略）
    void NotifyThinking(const SessionId& sid);
    void NotifyLlmText(const SessionId& sid, const std::string& text);
    size_t ConnectionCount() const;

private:
    struct Conn;
    void AcceptLoop();
    void SessionThread(std::shared_ptr<Conn> conn);
    void PluginSessionLoop(std::shared_ptr<Conn> conn, const std::string& plugin_path);
    void JwtSessionLoop(std::shared_ptr<Conn> conn);    // 现有 JWT 路径
    void CleanupConn(std::shared_ptr<Conn> conn);

    uint16_t m_port;
    std::string m_certFile, m_keyFile;
    ext::AuthProvider* m_auth;
    WsCallbacks m_cb;
    std::unordered_map<std::string, std::string> m_protocolRoutes; // 子协议→插件
    std::string m_protocolKey; // 插件配置槽（设备 Key 等）

    std::thread m_acceptThread;
    std::atomic<bool> m_running{false};
    int m_listenFd = -1;

    mutable std::mutex m_mtx;
    std::unordered_map<std::string, std::shared_ptr<Conn>> m_conns; // key: SidHex
    std::unordered_map<std::string, uint64_t> m_genByUid;

    audio::WatermarkConfig m_wmCfg;
    // 协议侧 8k 标定配置：降采样率不改变 Hz 频率，模板 chirp 与原 16k
    // 一致（1k~4kHz）；延迟按 8k 采样测得后 ×2 换算回内部 16k
    audio::WatermarkConfig m_wmCfg8k{
        /*sample_rate*/ 8000, /*chirp_f0*/ 1000.0, /*chirp_f1*/ 4000.0,
        /*chirp_ms*/ 20, /*gap_ms*/ 320, /*amplitude*/ 0.5,
        /*ncc_threshold*/ 0.55, /*interval_tolerance_ms*/ 2};
};

} // namespace mediator::net
