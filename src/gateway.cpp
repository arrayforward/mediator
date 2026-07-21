// ============================================================================
// gateway.cpp — 网关装配实现
//
// 关键逻辑：
//   1. 认证装配：--auth-provider=builtin 用 BuiltinJwtAuth；
//      =wasm:<path> 用 wasm3 加载模块（加载失败拒绝启动，见 §7.3.1）。
//   2. 心跳循环：固定节拍 RunOnce() → Dispatch(ChangeSet)。
//      心跳线程是共享数据唯一写者；gRPC 全部在线程池异步执行。
//   3. LLM method → 消息映射：quick→kLlmQuickResp(安抚)、
//      restate→kLlmRestate、answer→kLlmFinalAnswer、
//      quick_placeholder→kLlmQuickResp(clip=kPlaceholder)。
//   4. TTS 返回 PCM → 本机 G.711A 编码 → kTtsAudioChunk 回注。
// ============================================================================
#include "gateway.h"

#include "audio/apm_wrapper.h"
#include "audio/g711.h"
#include "core/log.h"
#include "net/grpc_clients.h"
#include "telemetry/telemetry.h"

namespace mediator {

Gateway::Gateway(GatewayConfig cfg)
    : m_cfg(std::move(cfg)), m_engine(EngineConfig{}, m_clock) {
    // ---- 认证装配（fail-closed：wasm 加载失败拒绝启动）----
    if (m_cfg.auth_provider.rfind("wasm:", 0) == 0) {
        const std::string path = m_cfg.auth_provider.substr(5);
        if (!m_wasmMgr.Load("auth", path)) {
            MDT_ERROR("wasm auth module load failed: {}", path);
            throw std::runtime_error("wasm auth module load failed: " + path);
        }
        m_auth = std::make_unique<ext::WasmAuth>(m_wasmMgr.Find("auth"));
        MDT_INFO("auth provider: wasm ({})", path);
    } else {
        m_auth = std::make_unique<ext::BuiltinJwtAuth>(m_cfg.jwt_secret,
                                                       m_cfg.allow_debug_token);
        MDT_INFO("auth provider: builtin (HS256)");
        if (m_cfg.allow_debug_token)
            MDT_WARN("!! debug token enabled (debug:<uid>) — 生产环境严禁开启 !!");
    }

    m_backend = std::make_unique<net::GrpcBackend>(m_cfg.backend_addr);

    // ---- Redis（连接失败降级为纯内存态，WARN 日志）----
    m_redis = std::make_unique<session::RedisStore>(m_cfg.redis_host, m_cfg.redis_port);
    m_redis->Connect();

    // ---- OTLP 指标导出（OTel Collector）----
    if (!m_cfg.otlp_endpoint.empty()) {
        telemetry::OtlpConfig oc;
        oc.endpoint = m_cfg.otlp_endpoint;
        oc.interval_s = m_cfg.otlp_interval_s;
        oc.instance_id = EngineConfig{}.gw_id;
        m_otlp = std::make_unique<telemetry::OtlpMetricsExporter>(std::move(oc));
        m_otlp->Start();
    }

    // ---- /metrics 抓取端点（备用通道，调试/Collector prometheus receiver）----
    if (m_cfg.metrics_port != 0) m_metrics.Start(m_cfg.metrics_port);

    // ---- wasm 观察者订阅（name:path,name:path）----
    for (size_t pos = 0; pos < m_cfg.observers.size();) {
        const auto comma = m_cfg.observers.find(',', pos);
        const auto item = m_cfg.observers.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? m_cfg.observers.size() : comma + 1;
        const auto colon = item.find(':');
        if (colon != std::string::npos)
            m_wasmBus.Subscribe(item.substr(0, colon), item.substr(colon + 1));
    }

    net::WsCallbacks cb;
    cb.inject = [this](Message&& m) {
        m_wasmBus.Notify(m); // wasm 观察者（只读，trap 自动熔断）
        m_engine.Inject(std::move(m));
    };
    cb.on_audio = [this](const SessionId& sid, const std::vector<int16_t>& pcm,
                         uint32_t flags) { OnAudioToAsr(sid, pcm, flags); };
    m_ws = std::make_unique<net::WsServer>(m_cfg.ws_port, m_cfg.cert_file,
                                           m_cfg.key_file, m_auth.get(), std::move(cb));
}

Gateway::~Gateway() { Stop(); }

void Gateway::Run() {
    m_running = true;
    m_ws->Start();
    MDT_INFO("gateway running, backend={}", m_cfg.backend_addr);
    while (m_running) {
        const auto cs = m_engine.RunOnce();
        Dispatch(cs);
        std::this_thread::sleep_for(std::chrono::milliseconds(m_cfg.heartbeat_ms));
    }
}

void Gateway::Stop() {
    m_running = false;
    if (m_ws) m_ws->Stop();
    {
        std::lock_guard<std::mutex> lk(m_asrMtx);
        m_asrStreams.clear();
    }
    m_pool.Stop();
}

void Gateway::Inject(MsgType type, const SessionId& sid, ClipId clip,
                     std::string text, std::vector<uint8_t> payload) {
    Message m;
    m.type = type;
    m.session_id = sid;
    m.ts_ms = m_clock.NowMs();
    m.clip_id = clip;
    m.text = std::move(text);
    m.payload = std::move(payload);
    m_engine.Inject(std::move(m));
}

void Gateway::Dispatch(const ChangeSet& cs) {
    for (const auto& call : cs.grpc_calls)
        m_pool.Post([this, call] { ExecGrpcCall(call); });
    for (const auto& out : cs.ws_sends) {
        MDT_DEBUG("ws_send clip={} text={} bytes={}", out.clip_id, out.is_text,
                  out.bytes.size());
        if (!out.is_text && out.clip_id != clip::kNone)
            telemetry::Registry::Instance()
                .MakeCounter("clip_sent_total", "clip=\"" + std::to_string(out.clip_id) + "\"",
                             "audio clips sent to device")
                .Add();
        if (out.is_text)
            m_ws->SendText(out.session_id, {out.bytes.begin(), out.bytes.end()});
        else
            m_ws->SendBinary(out.session_id, out.clip_id, out.bytes);
    }
    for (const auto& op : cs.redis_ops) {
        // GET 类：恢复数据回注引擎；其余异步执行（阶段4，不阻塞心跳）
        m_pool.Post([this, op] {
            if (op.op == "GET") {
                const auto val = m_redis->Get(op.key);
                if (!val) return;
                // key 形如 ctx:{uid} / placeholder:{uid}，恢复目标会话
                const auto colon = op.key.find(':');
                if (colon == std::string::npos) return;
                const auto sid = SessionIdFromUid(op.key.substr(colon + 1));
                if (op.key.rfind("ctx:", 0) == 0)
                    Inject(MsgType::kCtxRestored, sid, clip::kNone, *val);
                else if (op.key.rfind("placeholder:", 0) == 0)
                    Inject(MsgType::kPlaceholderRestored, sid, clip::kPlaceholder, {},
                           {val->begin(), val->end()});
            } else {
                m_redis->Execute(op);
            }
        });
    }
    for (const auto& m : cs.new_messages) {
        Message copy = m;
        m_engine.Inject(std::move(copy));
    }
}

void Gateway::ExecGrpcCall(const GrpcCall& call) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto record_latency = [&] {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        telemetry::Registry::Instance()
            .MakeHistogram("grpc_call_duration_ms",
                           "service=\"" + call.service + "\",method=\"" + call.method + "\"",
                           "backend grpc latency")
            .Record(ms);
    };
    struct LatencyGuard {
        std::function<void()> f;
        ~LatencyGuard() { f(); }
    } guard{record_latency};

    if (call.service == "llm") {
        const std::string text = m_backend->LlmGenerate(call.method, call.request_bytes,
                                                        call.session_id);
        if (text.empty()) return; // 超时路径由演进组件兜底
        if (call.method == "restate")
            Inject(MsgType::kLlmRestate, call.session_id, call.clip_id, text);
        else if (call.method == "answer")
            Inject(MsgType::kLlmFinalAnswer, call.session_id, call.clip_id, text);
        else // quick / quick_placeholder → 安抚或占位文本
            Inject(MsgType::kLlmQuickResp, call.session_id, call.clip_id, text);
    } else if (call.service == "tts") {
        const std::string pcm = m_backend->TtsSynth(call.request_bytes, call.clip_id,
                                                    call.session_id);
        if (pcm.empty()) return;
        std::vector<int16_t> samples(pcm.size() / 2);
        std::memcpy(samples.data(), pcm.data(), samples.size() * 2);
        // 下行 PCM 回灌 AEC 参考（先 reverse 后 capture 的 AEC 用法）
        if (m_cfg.enable_apm) {
            if (auto* apm = GetApmSession(call.session_id)) apm->ProcessRender(samples);
        }
        Inject(MsgType::kTtsAudioChunk, call.session_id, call.clip_id, {},
               audio::EncodeALaw(samples));
    } else if (call.service == "business") {
        const std::string ack = m_backend->BusinessControl(call.request_bytes,
                                                           call.session_id);
        Inject(MsgType::kControlAck, call.session_id, clip::kNone, ack);
    } else if (call.service == "memory") {
        const bool ok = m_backend->MemoryFetch(call.request_bytes, call.session_id);
        MDT_INFO("memory FetchContext key={} ok={}", call.request_bytes, ok);
    }
}

audio::ApmWrapper* Gateway::GetApmSession(const SessionId& sid) {
    if (!m_cfg.enable_apm) return nullptr;
    std::lock_guard<std::mutex> lk(m_apmMtx);
    auto& slot = m_apmSessions[net::SidHex(sid)];
    if (!slot) {
        audio::ApmCalib calib{};
        // 从黑板读取水印标定结果（延迟/漂移）注入 APM
        if (const auto* s = m_engine.Board().FindSession(sid)) {
            calib.delay_samples = s->m_aecCalib.delay_samples;
            calib.skew = s->m_aecCalib.skew;
        }
        slot = std::make_unique<audio::ApmWrapper>(calib);
    }
    return slot.get();
}

void Gateway::OnAudioToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                           uint32_t flags) {
    // WebRTC APM：回声消除 + 降噪 + VAD（净语音送 ASR）
    std::vector<int16_t> clean = pcm;
    uint32_t f = flags;
    if (auto* apm = GetApmSession(sid)) {
        auto res = apm->ProcessCapture(pcm);
        if (!res.pcm.empty()) clean = std::move(res.pcm);
        if (res.has_voice) f |= msgflag::kVoice; // APM VAD 与端侧 flags 取并集
    }

    net::GrpcBackend::AsrStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_asrMtx);
        auto& slot = m_asrStreams[net::SidHex(sid)];
        if (!slot) {
            slot = m_backend->NewAsrStream(
                sid,
                [this, sid](std::string text) { // partial：当前仅记录
                    MDT_DEBUG("asr partial: {}", text);
                },
                [this, sid](std::string text) { // final → 触发三段式流水线
                    Inject(MsgType::kAsrFinal, sid, clip::kNone, std::move(text));
                });
        }
        stream = slot.get();
    }
    stream->Write(clean, f);
}

} // namespace mediator
