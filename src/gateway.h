// ============================================================================
// gateway.h — 网关装配器：把四阶段架构接到真实网络
//
// 数据流（对应设计文档 §1.1）：
//   WSS 帧 → WsServer(认证/水印检测) → Inject → HeartbeatEngine(20ms 心跳)
//     → ChangeSet → Executor:
//         grpc_calls  → ThreadPool 同步调用 GrpcBackend，结果回注引擎
//         ws_sends    → WsServer.SendBinary/SendText
//         redis_ops   → 日志记录（Redis 实接待 hiredis）
//   上行有声帧：WsServer.on_audio → AsrStreamManager → mock ASR
//     → partial/final 回注引擎（阶段4→1 闭环）
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "core/clock.h"
#include "core/thread_pool.h"
#include "engine/heartbeat_engine.h"
#include "ext/auth_provider.h"
#include "ext/wasm3_host.h"
#include "ext/wasm_bus.h"
#include "net/grpc_clients.h"
#include "net/ws_server.h"
#include "session/redis_store.h"
#include "telemetry/metrics_server.h"
#include "telemetry/otlp_exporter.h"
#include "telemetry/telemetry.h"

namespace mediator::audio { class ApmWrapper; struct ApmCalib; class AudioPipeline; }

namespace mediator {

struct GatewayConfig {
    uint16_t ws_port = 9443;
    std::string cert_file, key_file;
    std::string backend_addr = "127.0.0.1:50051"; // mock 五合一地址
    std::string auth_provider = "builtin";        // builtin | wasm:<path>
    std::string jwt_secret = "dev-secret";
    bool allow_debug_token = false; // 调试后门 token（debug:<uid>），生产严禁开启
    int heartbeat_ms = 20;
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string otlp_endpoint;                    // OTel Collector，空=不导出
    int otlp_interval_s = 10;
    uint16_t metrics_port = 0;                    // /metrics 抓取端点，0=不启动
    std::string observers;                        // wasm 观察者：name:path,name:path
    bool enable_apm = true;                       // WebRTC APM（AECM/NS/VAD）
};

class Gateway {
public:
    explicit Gateway(GatewayConfig cfg);
    ~Gateway();

    void Run();      // 阻塞：心跳循环 + 事件分发
    void Stop();

private:
    void Dispatch(const ChangeSet& cs);
    void ExecGrpcCall(const GrpcCall& call);
    void OnAudioToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                      uint32_t flags);
    void WriteToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                    uint32_t flags);
    void Inject(MsgType type, const SessionId& sid, ClipId clip,
                std::string text = {}, std::vector<uint8_t> payload = {});

    GatewayConfig m_cfg;
    SteadyClock m_clock;
    HeartbeatEngine m_engine;
    ThreadPool m_pool{4};

    std::unique_ptr<ext::AuthProvider> m_auth;
    ext::WasmModuleManager m_wasmMgr;   // auth_provider=wasm 时使用
    std::unique_ptr<net::GrpcBackend> m_backend;
    std::unique_ptr<net::WsServer> m_ws;

    // 每会话 ASR 流
    std::mutex m_asrMtx;
    std::unordered_map<std::string, std::unique_ptr<net::GrpcBackend::AsrStream>>
        m_asrStreams;

    // 音频异步管线：每会话一个 APM 实例（actor 模型，CPU 池调度）。
    // APM 与连接上下文绑定，会话 3 分钟超时 GC 时一并清除（session_gc 信号）
    std::unique_ptr<audio::AudioPipeline> m_pipeline;
    void CleanupSessionResources(const SessionId& sid);

    std::unique_ptr<session::RedisStore> m_redis;
    std::unique_ptr<telemetry::OtlpMetricsExporter> m_otlp;
    telemetry::MetricsServer m_metrics;
    ext::WasmBus m_wasmBus;

    std::atomic<bool> m_running{false};
};

} // namespace mediator
