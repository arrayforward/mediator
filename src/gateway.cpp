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

#include "audio/g711.h"
#include "core/log.h"
#include "net/grpc_clients.h"

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
        m_auth = std::make_unique<ext::BuiltinJwtAuth>(m_cfg.jwt_secret);
        MDT_INFO("auth provider: builtin");
    }

    m_backend = std::make_unique<net::GrpcBackend>(m_cfg.backend_addr);

    net::WsCallbacks cb;
    cb.inject = [this](Message&& m) { m_engine.Inject(std::move(m)); };
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
        if (out.is_text)
            m_ws->SendText(out.session_id, {out.bytes.begin(), out.bytes.end()});
        else
            m_ws->SendBinary(out.session_id, out.clip_id, out.bytes);
    }
    for (const auto& op : cs.redis_ops)
        MDT_DEBUG("redis {} {} (ttl={})", op.op, op.key, op.ttl_s);
    for (const auto& m : cs.new_messages) {
        Message copy = m;
        m_engine.Inject(std::move(copy));
    }
}

void Gateway::ExecGrpcCall(const GrpcCall& call) {
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

void Gateway::OnAudioToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                           uint32_t flags) {
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
    stream->Write(pcm, flags);
}

} // namespace mediator
