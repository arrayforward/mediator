#include "engine/heartbeat_engine.h"

#include "audio/g711.h"
#include "audio/watermark.h"
#include "session/context_evict.h"

namespace mediator {

ChangeSet HeartbeatEngine::RunOnce() {
    ChangeSet cs;
    std::vector<Message> batch;
    m_inbound.SwapOutAll(batch);
    for (auto& m : batch) {
        if (m.msg_id == 0) m.msg_id = m_nextMsgId++;
        ProcessOne(m, cs);
    }
    EvolveOnce(cs);
    return cs;
}

void HeartbeatEngine::ProcessOne(const Message& m, ChangeSet& cs) {
    switch (m.type) {
    case MsgType::kWsConnected: OnWsConnected(m, cs); break;
    case MsgType::kWsDisconnected: OnWsDisconnected(m, cs); break;
    case MsgType::kWsAudioFrame: OnAudioFrame(m, cs); break;
    case MsgType::kAsrFinal: OnAsrFinal(m, cs); break;
    case MsgType::kLlmQuickResp:
    case MsgType::kLlmRestate:
    case MsgType::kLlmFinalAnswer: OnLlmText(m, cs); break;
    case MsgType::kTtsAudioChunk: OnTtsChunk(m, cs); break;
    case MsgType::kWmDetected: OnWmDetected(m, cs); break;
    case MsgType::kVadUpdate: OnVadUpdate(m, cs); break;
    case MsgType::kAudioCancel: OnAudioCancel(m, cs); break;
    case MsgType::kCtxRestored: {
        // Redis 恢复：断线重连后找回聊天上下文
        auto* s = m_board.FindSession(m.session_id);
        if (s && !m.text.empty()) {
            s->m_chatCtx = m.text;
            m_board.MarkChanged(m.session_id);
        }
        break;
    }
    case MsgType::kPlaceholderRestored: {
        // Redis 恢复：上一次占位音频（供安抚文本未就绪时复用）
        auto* s = m_board.FindSession(m.session_id);
        if (s && !m.payload.empty()) {
            s->m_lastPlaceholder.id = clip::kPlaceholder;
            s->m_lastPlaceholder.g711 = m.payload;
            s->m_lastPlaceholder.audio_ready = true;
            s->m_hasLastPlaceholder = true;
        }
        break;
    }
    case MsgType::kWsControlCmd:
        cs.grpc_calls.push_back(
            GrpcCall{"business", "control", m.session_id, clip::kNone, m.text});
        break;
    case MsgType::kControlAck:
        // 业务控制回执 → 文本帧回写端侧
        cs.ws_sends.push_back(
            WsOutbound{m.session_id, clip::kNone, true,
                       {m.text.begin(), m.text.end()}});
        break;
    default:
        break; // kAsrPartial/kTick*/kControlAck/kMemoryAck：本轮无心跳内动作
    }
}

void HeartbeatEngine::OnWsConnected(const Message& m, ChangeSet& cs) {
    // text = uid（来自 AuthProvider，session_id = SessionIdFromUid(uid)）
    auto& s = m_board.GetOrCreateSession(m.session_id);
    const uint64_t gen = static_cast<uint64_t>(m.aux);
    if (gen < s.m_connGeneration) return; // 旧代际消息丢弃
    s.m_connGeneration = gen;
    s.m_uid = m.text;
    s.m_state = SessionState::kIdle;
    s.m_lastSeenMs = m.ts_ms;
    m_board.RegisterOnline(m.text, m_cfg.gw_id, gen);
    cs.redis_ops.push_back(
        RedisOp{"SETEX", "online:" + m.text, m_cfg.gw_id, 300});
    // 连接建立（含重连）：异步恢复上下文与上一次占位音频（§6.4 断线续聊）
    cs.redis_ops.push_back(RedisOp{"GET", "ctx:" + m.text, "", 0});
    cs.redis_ops.push_back(RedisOp{"GET", "placeholder:" + m.text, "", 0});
    // 未完成 AEC 标定 → 发水印并进入标定期（上行语音直接丢弃）
    if (!s.m_aecCalib.valid && !s.m_wmPending) {
        s.m_wmPending = true;
        audio::WatermarkConfig wcfg;
        const auto pcm = audio::GenerateWatermark(wcfg);
        cs.ws_sends.push_back(WsOutbound{m.session_id, clip::kWatermark, false, audio::EncodeALaw(pcm)});
    }
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnWsDisconnected(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    s->m_state = SessionState::kOffline;
    s->m_lastSeenMs = m.ts_ms;
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnAudioFrame(const Message& m, ChangeSet& cs) {
    (void)cs;
    auto* s = m_board.FindSession(m.session_id);
    if (!s || s->m_state == SessionState::kOffline) return;
    if (s->m_wmPending) {
        ++s->m_droppedFramesWm; // 水印标定期：直接丢弃，不识别
        return;
    }
    if (m.flags & msgflag::kVoice) {
        if (!s->m_uttActive) {
            s->m_uttActive = true;
            s->m_uttStartMs = m.ts_ms;
            s->m_vadEndpoint = false;      // 新语句开始才重置断句标志
            s->m_quickRespSubmitted = false; // 新一轮允许再次触发安抚
        }
        s->m_lastVoiceMs = m.ts_ms;
        s->m_state = SessionState::kListening;
    }
    if (m.flags & (msgflag::kVadEnd | msgflag::kAsrEndpoint)) s->m_vadEndpoint = true;
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnAsrFinal(const Message& m, ChangeSet& cs) {
    auto& s = m_board.GetOrCreateSession(m.session_id);
    s.m_state = SessionState::kThinking;
    s.m_uttActive = false;
    // 注意：不再清除 m_vadEndpoint —— VAD 帧与 Final 可能同心跳到达，
    // QuickRespTrigger 依赖该标志在本心跳内触发安抚音频（竞态修复）
    // 上下文累积（Redis 持久化走 CtxEvict/同步路径）
    s.m_chatCtx += "U:" + m.text + "\n";
    // 并行：复述 B + 完整答案 C
    const int64_t now = m.ts_ms;
    ClipBuffer b; b.id = clip::kRestate; b.requested_ms = now;
    ClipBuffer c; c.id = clip::kAnswer; c.requested_ms = now;
    s.m_playQueue.push_back(std::move(b));
    s.m_playQueue.push_back(std::move(c));
    const int64_t gen = static_cast<int64_t>(s.m_uttGen);
    cs.grpc_calls.push_back(GrpcCall{"llm", "restate", m.session_id, clip::kRestate, m.text, gen});
    cs.grpc_calls.push_back(GrpcCall{"llm", "answer", m.session_id, clip::kAnswer, m.text, gen});
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnLlmText(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 打断后迟到的旧代际结果直接丢弃（防止已取消语句复活）
    if (static_cast<uint64_t>(m.aux) != s->m_uttGen) return;
    ClipId id = clip::kNone;
    switch (m.type) {
    case MsgType::kLlmQuickResp: id = (m.clip_id == clip::kPlaceholder) ? clip::kPlaceholder : clip::kSoothe; break;
    case MsgType::kLlmRestate: id = clip::kRestate; break;
    case MsgType::kLlmFinalAnswer: id = clip::kAnswer; break;
    default: break;
    }
    if (m.type == MsgType::kLlmQuickResp && id == clip::kSoothe) {
        // A 文本就绪 → TTS(A)（A clip 可能尚未入队，直接建）
        if (!FindClip(*s, clip::kSoothe)) {
            ClipBuffer a; a.id = clip::kSoothe; a.text = m.text; a.text_ready = true;
            s->m_playQueue.push_front(std::move(a)); // A 插到队首，优先于 B/C
        } else {
            auto* a = FindClip(*s, clip::kSoothe);
            a->text = m.text; a->text_ready = true;
        }
        EmitTts(*s, clip::kSoothe, m.text, cs);
    } else if (auto* cl = FindClip(*s, id)) {
        cl->text = m.text;
        cl->text_ready = true;
        EmitTts(*s, id, m.text, cs);
    }
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::EmitTts(SessionContext& s, ClipId clip, const std::string& text,
                              ChangeSet& cs) {
    cs.grpc_calls.push_back(GrpcCall{"tts", "synth", s.m_sessionId, clip, text,
                                     static_cast<int64_t>(s.m_uttGen)});
}

void HeartbeatEngine::OnTtsChunk(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 打断后迟到的旧代际音频直接丢弃（防止已取消语句继续播放）
    if (static_cast<uint64_t>(m.aux) != s->m_uttGen) return;
    auto* cl = FindClip(*s, m.clip_id);
    if (!cl) {
        ClipBuffer nb; nb.id = m.clip_id;
        s->m_playQueue.push_back(std::move(nb));
        cl = FindClip(*s, m.clip_id);
    }
    cl->g711.insert(cl->g711.end(), m.payload.begin(), m.payload.end());
    cl->audio_ready = true;
    // 占位音频持久缓存：供下次 A 未就绪时复用（内存 + Redis）
    if (m.clip_id == clip::kPlaceholder) {
        s->m_lastPlaceholder = *cl;
        s->m_hasLastPlaceholder = true;
        cs.redis_ops.push_back(RedisOp{"SETEX", "placeholder:" + s->m_uid,
                                       std::string(cl->g711.begin(), cl->g711.end()),
                                       24 * 3600});
    }
    m_board.MarkChanged(m.session_id);
}

// ---- 打断（barge-in）----
// 播放/思考期间用户说话：清空播放队列、语句代际+1（迟到的旧 LLM/TTS 结果
// 全部丢弃）、回到倾听态。聊天上下文 m_chatCtx 保留——新语句的复述/答案
// 依然携带历史，保证"结合历史响应现在的语音"。
void HeartbeatEngine::DoBargeIn(SessionContext& s, int64_t now, ChangeSet& cs) {
    s.m_playQueue.clear();
    ++s.m_uttGen;
    s.m_placeholderRounds = 0;
    s.m_quickRespSubmitted = false; // 新语句允许再次触发安抚
    s.m_vadEndpoint = false;
    s.m_uttActive = true;
    s.m_uttStartMs = now;
    s.m_state = SessionState::kListening;
    cs.board_writes.push_back(BoardMutation{s.m_sessionId, "interrupted", "1"});
    m_board.MarkChanged(s.m_sessionId);
}

void HeartbeatEngine::OnVadUpdate(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s || s->m_state == SessionState::kOffline) return;
    if (s->m_wmPending) return; // 标定期不识别
    if (m.flags & msgflag::kVoice) {
        ++s->m_voiceRun;
        // 打断触发：思考/播放期间检测到持续语音（防单帧噪声误触）。
        // 未达阈值前必须保持 kThinking——否则首帧就把状态翻成倾听，
        // 打断窗口被自己关掉
        if (s->m_voiceRun >= m_cfg.barge_in_frames &&
            (s->m_state == SessionState::kThinking || !s->m_playQueue.empty())) {
            DoBargeIn(*s, m.ts_ms, cs); // 内部置 kListening
        } else if (s->m_state != SessionState::kThinking && s->m_playQueue.empty()) {
            s->m_state = SessionState::kListening;
        }
        if (!s->m_uttActive) {
            s->m_uttActive = true;
            s->m_uttStartMs = m.ts_ms;
            s->m_vadEndpoint = false;      // 新语句开始才重置断句标志
            s->m_quickRespSubmitted = false;
        }
        s->m_lastVoiceMs = m.ts_ms;
    } else {
        s->m_voiceRun = 0;
    }
    if (m.flags & (msgflag::kVadEnd | msgflag::kAsrEndpoint)) s->m_vadEndpoint = true;
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnAudioCancel(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 端侧主动取消（如本地 VAD 打断）：与 barge-in 同语义，空闲时幂等无操作
    if (s->m_state == SessionState::kThinking || !s->m_playQueue.empty())
        DoBargeIn(*s, m.ts_ms, cs);
}

void HeartbeatEngine::OnWmDetected(const Message& m, ChangeSet& cs) {
    (void)cs;
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    s->m_aecCalib.valid = true;
    s->m_aecCalib.delay_samples = static_cast<int32_t>(m.aux);
    s->m_aecCalib.skew = m.dval;
    s->m_wmPending = false;
    m_board.MarkChanged(m.session_id);
}

// ---- 单层数据演进 ----

void HeartbeatEngine::EvolveOnce(ChangeSet& cs) {
    const int64_t now = m_clock.NowMs();
    auto changed = m_board.TakeChangedForEvolve(); // 快照：演进新变更留待下轮
    for (const auto& sid : changed) {
        auto* s = m_board.FindSession(sid);
        if (!s) continue;
        EvolveQuickResp(*s, now, cs);
        EvolvePlayback(*s, now, cs);
        EvolveCtxEvict(*s, cs);
    }
    EvolveSessionGc(now, cs);
}

void HeartbeatEngine::EvolveQuickResp(SessionContext& s, int64_t now, ChangeSet& cs) {
    if (s.m_quickRespSubmitted) return;
    // VAD 帧与 ASR Final 可能同心跳到达（Final 会清 uttActive）——
    // 断句标志本身即触发条件，不依赖 uttActive（竞态修复）
    const bool long_utt = s.m_uttActive && (now - s.m_uttStartMs) > m_cfg.utt_quick_ms;
    if (!long_utt && !s.m_vadEndpoint) return;
    if (s.m_state == SessionState::kOffline) return;
    s.m_quickRespSubmitted = true;
    cs.grpc_calls.push_back(
        GrpcCall{"llm", "quick", s.m_sessionId, clip::kSoothe, s.m_chatCtx,
                 static_cast<int64_t>(s.m_uttGen)});
}

void HeartbeatEngine::EvolvePlayback(SessionContext& s, int64_t now, ChangeSet& cs) {
    // 队首 clip 就绪 → 下发剩余字节
    while (!s.m_playQueue.empty()) {
        auto& front = s.m_playQueue.front();
        if (front.audio_ready) {
            if (front.sent_bytes < front.g711.size()) {
                WsOutbound out{s.m_sessionId, front.id, false,
                               {front.g711.begin() + static_cast<long>(front.sent_bytes),
                                front.g711.end()}};
                front.sent_bytes = front.g711.size();
                cs.ws_sends.push_back(std::move(out));
            }
            const ClipId popped = front.id;
            s.m_playQueue.pop_front();
            // 只有正主（安抚/复述/答案）播完才重置占位轮次；占位音播完不重置
            if (popped != clip::kPlaceholder) s.m_placeholderRounds = 0;
            continue;
        }
        break;
    }
    if (s.m_playQueue.empty()) return;

    // 队首未就绪：超时检查（仅 B/C 有预算；A 由 QuickResp 保证）
    auto& front = s.m_playQueue.front();
    const int64_t budget = (front.id == clip::kRestate)   ? m_cfg.b_timeout_ms
                           : (front.id == clip::kAnswer) ? m_cfg.c_timeout_ms
                                                    : 0;
    if (budget <= 0 || front.requested_ms <= 0) return;
    if (now - front.requested_ms <= budget) return;

    // 超时 → 占位音频（§6.2.1）
    // 规则：A 文本未生成完毕 → 直接复用上一次对话的占位音（时延≈0）；
    //       A 已就绪 → 走 quick 路径按当前上下文生成场景化占位音。
    // 复用与生成都计入占位轮次，超上限（默认2轮）→ stall 并上报。
    if (s.m_placeholderRounds >= m_cfg.max_placeholder_rounds) {
        cs.board_writes.push_back(BoardMutation{s.m_sessionId, "clip_stalled",
                                                std::to_string(front.id)});
        return;
    }
    const auto* soothe = FindClip(s, clip::kSoothe);
    const bool soothe_text_ready = (soothe && soothe->text_ready);
    if (!soothe_text_ready && s.m_hasLastPlaceholder) {
        ++s.m_placeholderRounds;
        EnqueueReusePlaceholder(s, cs); // A 未生成完：复用上一次占位音
        return;
    }
    ++s.m_placeholderRounds;
    front.requested_ms = now; // 重置计时，等待占位生成
    ClipBuffer p; p.id = clip::kPlaceholder; p.requested_ms = 0;
    s.m_playQueue.push_front(std::move(p));
    cs.grpc_calls.push_back(GrpcCall{"llm", "quick_placeholder", s.m_sessionId,
                                     clip::kPlaceholder, s.m_chatCtx,
                                     static_cast<int64_t>(s.m_uttGen)});
}

void HeartbeatEngine::EnqueueReusePlaceholder(SessionContext& s, ChangeSet& cs) {
    ClipBuffer p = s.m_lastPlaceholder;
    p.id = clip::kPlaceholder;
    p.sent_bytes = 0;
    s.m_playQueue.push_front(std::move(p));
    cs.board_writes.push_back(
        BoardMutation{s.m_sessionId, "placeholder_reused", "1"});
    m_board.MarkChanged(s.m_sessionId);
}

void HeartbeatEngine::EvolveSessionGc(int64_t now, ChangeSet& cs) {
    std::vector<SessionId> dead;
    m_board.ForEachSession([&](const SessionId& sid, SessionContext& s) {
        if (s.m_state == SessionState::kOffline &&
            now - s.m_lastSeenMs > m_cfg.session_gc_ms) {
            cs.grpc_calls.push_back(
                GrpcCall{"memory", "FetchContext", sid, clip::kNone, "ctx:" + s.m_uid});
            cs.redis_ops.push_back(RedisOp{"DEL", "ctx:" + s.m_uid, "", 0});
            // 通知执行层清理会话附属资源（APM 实例/ASR 流，随上下文一并超时）
            cs.board_writes.push_back(BoardMutation{sid, "session_gc", s.m_uid});
            dead.push_back(sid);
        }
    });
    for (const auto& sid : dead) m_board.RemoveSession(sid);
}

void HeartbeatEngine::EvolveCtxEvict(SessionContext& s, ChangeSet& cs) {
    if (s.m_chatCtx.size() <= m_cfg.max_ctx_bytes) return;
    s.m_chatCtx = session::EvictContext(s.m_chatCtx, m_cfg.max_ctx_bytes,
                                        m_cfg.ctx_target_ratio);
    cs.redis_ops.push_back(
        RedisOp{"SETEX", "ctx:" + s.m_uid, s.m_chatCtx, 7 * 24 * 3600});
}

ClipBuffer* HeartbeatEngine::FindClip(SessionContext& s, ClipId id) {
    for (auto& c : s.m_playQueue)
        if (c.id == id) return &c;
    return nullptr;
}

} // namespace mediator
