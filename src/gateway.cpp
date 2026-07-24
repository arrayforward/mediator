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

#include <cmath>

#include "audio/apm_wrapper.h"
#include "audio/despike.h"
#include "audio/audio_pipeline.h"
#include "audio/g711.h"
#include "core/log.h"
#include "net/grpc_clients.h"
#include "telemetry/telemetry.h"

namespace mediator {

namespace {
// 服务端 VAD 断句迟滞帧数：30 × 20ms ≈ 600ms 连续静音判为一句结束
constexpr int kVadHangoverFrames = 30;
} // namespace

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
        // 水印标定结果注入音频管线（kWmDetected 同时进引擎标记标定完成）
        if (m.type == MsgType::kWmDetected && m_pipeline) {
            audio::ApmCalib calib{static_cast<int32_t>(m.aux), m.dval};
            m_pipeline->SetCalib(m.session_id, calib);
        }
        // 半双工状态跟踪：校准完成→全双工；新连接→回半双工并清空估计器
        if (m.type == MsgType::kWmDetected || m.type == MsgType::kWsConnected) {
            std::lock_guard<std::mutex> lk(m_duplexMtx);
            const auto key = net::SidHex(m.session_id);
            if (m.type == MsgType::kWmDetected) {
                m_calibrated[key] = true;
            } else {
                m_calibrated[key] = false;
                m_speakingUntilMs.erase(key);
                m_calibEst.erase(key);
            }
        }
        // 协议插件事件通知
        if (m.type == MsgType::kAsrFinal && m_ws) m_ws->NotifyThinking(m.session_id);
        if (m.type == MsgType::kLlmRestate && m_ws)
            m_ws->NotifyLlmText(m.session_id, m.text);
        m_wasmBus.Notify(m); // wasm 观察者（只读，trap 自动熔断）
        m_engine.Inject(std::move(m));
    };
    cb.on_audio = [this](const SessionId& sid, const std::vector<int16_t>& pcm,
                         uint32_t flags) { OnAudioToAsr(sid, pcm, flags); };
    // 音频管线：每会话 APM 实例 + CPU 池（1.5×核数，至少2线程）
    if (m_cfg.enable_apm) {
        const int workers = std::max(2, static_cast<int>(std::thread::hardware_concurrency() * 3 / 2));
        m_pipeline = std::make_unique<audio::AudioPipeline>(workers);
    }

    m_ws = std::make_unique<net::WsServer>(m_cfg.ws_port, m_cfg.cert_file,
                                           m_cfg.key_file, m_auth.get(), std::move(cb));
    // 协议路由（wasm 插件，配置格式 subproto:path,subproto:path,...）
    for (size_t pos = 0; pos < m_cfg.protocol_routes.size();) {
        const auto comma = m_cfg.protocol_routes.find(',', pos);
        const auto item = m_cfg.protocol_routes.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? m_cfg.protocol_routes.size() : comma + 1;
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            m_ws->SetProtocolRoute(item.substr(0, colon), item.substr(colon + 1));
            MDT_INFO("protocol route: {} -> {}", item.substr(0, colon), item.substr(colon + 1));
        }
    }
    if (!m_cfg.protocol_key.empty()) m_ws->SetProtocolKey(m_cfg.protocol_key);
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
                      std::string text, std::vector<uint8_t> payload, int64_t aux,
                      uint32_t flags) {
    Message m;
    m.type = type;
    m.session_id = sid;
    m.ts_ms = m_clock.NowMs();
    m.clip_id = clip;
    m.text = std::move(text);
    m.payload = std::move(payload);
    m.aux = aux;
    m.flags = flags;
    // 协议插件事件通知（gRPC 回调路径同样触发：thinking / text 帧）
    if (m_ws) {
        if (type == MsgType::kAsrFinal) m_ws->NotifyThinking(sid);
        if (type == MsgType::kLlmRestate) m_ws->NotifyLlmText(sid, m.text);
    }
    m_engine.Inject(std::move(m));
}

void Gateway::Dispatch(const ChangeSet& cs) {
    for (const auto& b : cs.board_writes) {
        if (b.field == "session_gc") CleanupSessionResources(b.session_id);
        // 回声/碎片 final 被引擎丢弃（AEC 未校准防自我对话）
        if (b.field == "final_dropped")
            MDT_INFO("asr final dropped (echo/interjection) sid={} text={}",
                     net::SidHex(b.session_id), b.value);
        // 打断来源追踪（真机双打断杀新语句问题定位）
        if (b.field == "barge_src")
            MDT_INFO("barge-in source sid={} src={}",
                     net::SidHex(b.session_id), b.value);
        // 旧代际 LLM 结果丢弃（TTS 未收到的直接证据）
        if (b.field == "llm_stale_drop")
            MDT_WARN("llm result dropped (stale gen) sid={} text={}",
                     net::SidHex(b.session_id), b.value);
        // 打断：通知端侧丢弃本地播放缓冲（协议插件转换为状态帧）
        if (b.field == "interrupted" && m_ws) {
            MDT_INFO("barge-in: notify interrupted");
            m_ws->NotifyInterrupted(b.session_id);
        }
    }
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
        if (out.is_text) {
            m_ws->SendText(out.session_id, {out.bytes.begin(), out.bytes.end()});
        } else {
            // 内容锚定校准：下发内容计入 render 时间线（g711 8k 解码）；
            // 半双工：按字节数累计播放时长（g711 8k：1B=0.125ms）+200ms 尾
            const auto key = net::SidHex(out.session_id);
            std::lock_guard<std::mutex> lk(m_duplexMtx);
            m_calibEst[key].FeedRender(NowMs(), audio::DecodeALaw(out.bytes));
            auto& until = m_speakingUntilMs[key];
            until = std::max(until, NowMs()) +
                    static_cast<int64_t>(out.bytes.size() / 8) + 200;
            m_ws->SendBinary(out.session_id, out.clip_id, out.bytes);
        }
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
                else if (op.key.rfind("aeccalib:", 0) == 0) {
                    // 历史标定回读（"delay16k,skew"）：水印检测失败时用近似
                    // 延迟做 AEC——远优于完全旁路（回声自我对话根因）
                    int32_t delay = 0;
                    double skew = 0.0;
                    if (std::sscanf(val->c_str(), "%d,%lf", &delay, &skew) == 2 &&
                        delay > 0) {
                        if (m_pipeline)
                            m_pipeline->SetCalib(sid, audio::ApmCalib{delay, skew});
                        MDT_INFO("aec calib restored from cache sid={} delay16k={} "
                                 "skew={:.6f}",
                                  net::SidHex(sid), delay, skew);
                        // 引擎标记标定有效（单发水印 skew 恒 0，aux=delay16k）
                        Inject(MsgType::kWmDetected, sid, clip::kNone, {}, {}, delay);
                    }
                }
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

    if (call.service == "llm" && call.method == "judge_interrupt") {
        // 语义打断判定：RPC 失败默认不打断（噪声环境下宁丢真打断，不秒杀回复）
        bool interrupt = false;
        const bool ok = m_backend->InterruptJudge(call.request_bytes, call.session_id,
                                                  &interrupt);
        MDT_INFO("judge_interrupt sid={} ok={} interrupt={} text={}",
                 net::SidHex(call.session_id), ok, interrupt, call.request_bytes);
        Inject(MsgType::kInterruptVerdict, call.session_id, clip::kNone,
               call.request_bytes, {}, (ok && interrupt) ? 1 : 0);
        return;
    }
    if (call.service == "llm") {
        const std::string text = m_backend->LlmGenerate(call.method, call.request_bytes,
                                                        call.session_id);
        if (text.empty()) {
            // 失败/超时回注引擎：A 段(quick)/P 段(quick_placeholder)兜底由
            // 心跳立即触发（B/C 失败仍由演进组件的超时预算兜底）
            MDT_WARN("llm {} failed sid={} -> inject kLlmFailed", call.method,
                     net::SidHex(call.session_id));
            Inject(MsgType::kLlmFailed, call.session_id, call.clip_id, call.method,
                   {}, call.aux);
            return;
        }
        if (call.method == "restate")
            Inject(MsgType::kLlmRestate, call.session_id, call.clip_id, text, {}, call.aux);
        else if (call.method == "answer")
            Inject(MsgType::kLlmFinalAnswer, call.session_id, call.clip_id, text, {}, call.aux);
        else // quick / quick_placeholder → 安抚或占位文本
            Inject(MsgType::kLlmQuickResp, call.session_id, call.clip_id, text, {}, call.aux);
    } else if (call.service == "tts") {
        const std::string pcm = m_backend->TtsSynth(call.request_bytes, call.clip_id,
                                                    call.session_id);
        if (pcm.empty()) return;
        std::vector<int16_t> samples(pcm.size() / 2);
        std::memcpy(samples.data(), pcm.data(), samples.size() * 2);
        // 下行 PCM 投渲染任务（会话 FIFO 保证先 reverse 后 capture）
        if (m_pipeline) m_pipeline->PostRender(call.session_id, samples);
        Inject(MsgType::kTtsAudioChunk, call.session_id, call.clip_id, {},
               audio::EncodeALaw(samples), call.aux);
    } else if (call.service == "business") {
        const std::string ack = m_backend->BusinessControl(call.request_bytes,
                                                           call.session_id);
        Inject(MsgType::kControlAck, call.session_id, clip::kNone, ack);
    } else if (call.service == "memory") {
        const bool ok = m_backend->MemoryFetch(call.request_bytes, call.session_id);
        MDT_INFO("memory FetchContext key={} ok={}", call.request_bytes, ok);
    }
}

void Gateway::CleanupSessionResources(const SessionId& sid) {
    // 会话 3 分钟超时 GC：清除 APM 实例与 ASR 流（随上下文一并释放）
    if (m_pipeline) m_pipeline->RemoveSession(sid);
    {
        std::lock_guard<std::mutex> lk(m_asrMtx);
        m_asrStreams.erase(net::SidHex(sid));
    }
    {
        std::lock_guard<std::mutex> lk(m_vadMtx);
        m_vad.erase(net::SidHex(sid));
    }
    {
        std::lock_guard<std::mutex> lk(m_aiMtx);
        const auto key = net::SidHex(sid);
        m_aiChains.erase(key);
        m_aiInitFailed.erase(key);
    }
    {
        std::lock_guard<std::mutex> lk(m_duplexMtx);
        const auto key = net::SidHex(sid);
        m_calibrated.erase(key);
        m_speakingUntilMs.erase(key);
        m_calibEst.erase(key);
    }
}

int64_t Gateway::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Gateway::OnAudioToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                           uint32_t flags) {
    // APM 路径：异步投采集任务，净语音+VAD 回调中写 ASR（会话 FIFO 保序）
    if (m_pipeline) {
        const auto key = net::SidHex(sid);
        // 内容锚定校准：上行采集进估计器（半双工静音也不阻断——
        // 校准恰恰需要播放期的回声采集）
        {
            std::lock_guard<std::mutex> lk(m_duplexMtx);
            auto& est = m_calibEst[key];
            est.FeedCapture(NowMs(), pcm);
            const auto r = est.TryEstimate(NowMs());
            if (r.ready && !m_calibrated[key]) {
                m_calibrated[key] = true;
                MDT_INFO("aec calibrated (content-anchored) sid={} delay16k={} "
                         "ncc={:.3f}",
                         key, r.delay16k, r.ncc);
                m_pipeline->SetCalib(sid, audio::ApmCalib{r.delay16k, 0.0});
                // 引擎标记标定有效（顺带按设备缓存，下次连接秒恢复）
                Inject(MsgType::kWmDetected, sid, clip::kNone, {}, {}, r.delay16k);
            }
        }
        auto clean = pcm;
        audio::Despike(clean); // 真机 AGC/削波满幅脉冲（VAD/能量门毒化根因）
        m_pipeline->PostCapture(sid, std::move(clean), [this, sid, flags](const audio::ApmResult& res) {
            // 半双工（未校准设备）：播放期间上行不送 ASR——回声直灌会形成
            // 自我对话循环（校准成功后全双工）
            {
                std::lock_guard<std::mutex> lk(m_duplexMtx);
                const auto k = net::SidHex(sid);
                const auto cal = m_calibrated.find(k);
                const bool calibrated = cal != m_calibrated.end() && cal->second;
                const auto sp = m_speakingUntilMs.find(k);
                if (!calibrated && sp != m_speakingUntilMs.end() &&
                    NowMs() < sp->second)
                    return;
            }
            // AI 音频链：APM(AEC) 输出 → GTCRN 降噪 → Silero VAD 断句
            audio::AiAudioChain* chain = nullptr;
            {
                std::lock_guard<std::mutex> lk(m_aiMtx);
                const auto k = net::SidHex(sid);
                auto& c = m_aiChains[k];
                if (!c.Valid() && !m_aiInitFailed[k]) {
                    std::string err;
                    if (!c.Init(m_cfg.gtcrn_model, m_cfg.silero_model, &err)) {
                        m_aiInitFailed[k] = true;
                        MDT_WARN("ai audio chain init failed sid={}: {}", k, err);
                    } else {
                        MDT_INFO("ai audio chain ready sid={} (gtcrn+silero)", k);
                    }
                }
                if (c.Valid()) chain = &c;
            }
            uint32_t f = flags;
            uint32_t vf = 0u;
            bool endpoint = false;
            std::vector<int16_t> out_pcm = res.pcm;
            if (chain) {
                const auto aout = chain->Process(res.pcm);
                out_pcm = aout.pcm;
                if (aout.speech) {
                    f |= msgflag::kVoice;
                    vf |= msgflag::kVoice;
                }
                if (aout.endpoint) {
                    endpoint = true;
                    MDT_INFO("vad endpoint (silero)");
                }
            } else if (res.has_voice) { // AI 链不可用时退回 APM VAD
                f |= msgflag::kVoice;
                vf |= msgflag::kVoice;
            }
            WriteToAsr(sid, out_pcm.empty() ? std::vector<int16_t>{} : out_pcm, f);
            if (endpoint) {
                WriteToAsr(sid, {}, f | msgflag::kVadEnd | msgflag::kAsrEndpoint);
                vf |= msgflag::kVadEnd | msgflag::kAsrEndpoint;
            }
            // VAD 结果注引擎（倾听状态机/安抚触发；端侧 flags 不可信，
            // convai 插件每帧都标 kVoice）
            Inject(MsgType::kVadUpdate, sid, clip::kNone, {}, {}, 0, vf);
        });
        return;
    }
    WriteToAsr(sid, pcm, flags);
}

void Gateway::WriteToAsr(const SessionId& sid, const std::vector<int16_t>& pcm,
                         uint32_t flags) {
    // 空载荷但带断句/端点标志（如 convai AudioOp.End）也必须写流——
    // mock/真实 ASR 依赖 flags 触发 final
    if (pcm.empty() && !(flags & (msgflag::kVadEnd | msgflag::kAsrEndpoint)))
        return;
    const auto key = net::SidHex(sid);
    auto open_stream = [this, &sid] {
        return m_backend->NewAsrStream(
            sid,
            [this, sid](std::string text) { // partial → 引擎 ASR 确认门（quick 触发依据）
                MDT_DEBUG("asr partial: {}", text);
                if (!text.empty())
                    Inject(MsgType::kAsrPartial, sid, clip::kNone, std::move(text));
            },
            [this, sid](std::string text) { // final → 触发三段式流水线
                MDT_INFO("asr final: {}", text);
                Inject(MsgType::kAsrFinal, sid, clip::kNone, std::move(text));
            });
    };
    net::GrpcBackend::AsrStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_asrMtx);
        auto& slot = m_asrStreams[key];
        if (!slot) slot = open_stream();
        stream = slot.get();
    }
    if (stream->Write(pcm, flags)) return;
    // 流已死（后端重启/网格清扫/对端关闭）：丢弃尸体重建一次，
    // 否则该设备会话永久"变聋"，直到 mediator 重启
    MDT_WARN("asr stream write failed sid={} -> rebuild stream", key);
    {
        std::lock_guard<std::mutex> lk(m_asrMtx);
        auto& slot = m_asrStreams[key];
        if (slot.get() == stream) slot = open_stream();
        stream = slot.get();
    }
    if (!stream->Write(pcm, flags))
        MDT_WARN("asr stream write failed again after rebuild sid={}", key);
}

} // namespace mediator
