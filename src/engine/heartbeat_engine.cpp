#include "engine/heartbeat_engine.h"

#include <cstdio>

#include "audio/g711.h"
#include "audio/watermark.h"
#include "session/context_evict.h"

namespace mediator {

namespace {

// ---- 回声/碎片判定（AEC 未校准时防自我对话循环）----

// UTF-8 码点迭代（仅判定用，不做严格校验）
std::vector<uint32_t> DecodeRunes(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

// 归一化：仅保留字母/数字/ CJK（去空白、标点、符号）；ASCII 转小写
std::vector<uint32_t> NormalizeSpeech(const std::string& s) {
    std::vector<uint32_t> out;
    for (uint32_t cp : DecodeRunes(s)) {
        if (cp < 0x80) {
            if (cp >= 'A' && cp <= 'Z') cp += 32;
            if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9'))
                out.push_back(cp);
            continue;
        }
        if (cp >= 0x3000 && cp <= 0x303F) continue; // CJK 标点
        if (cp >= 0xFF00 && cp <= 0xFF65) continue; // 全角标点
        out.push_back(cp);
    }
    return out;
}

double BigramJaccard(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() < 2 || b.size() < 2) return 0;
    auto bigrams = [](const std::vector<uint32_t>& v) {
        std::unordered_set<uint64_t> set;
        for (size_t i = 0; i + 1 < v.size(); ++i)
            set.insert((static_cast<uint64_t>(v[i]) << 32) | v[i + 1]);
        return set;
    };
    const auto sa = bigrams(a), sb = bigrams(b);
    size_t inter = 0;
    for (const auto k : sa)
        if (sb.count(k)) ++inter;
    return static_cast<double>(inter) / (sa.size() + sb.size() - inter);
}

bool RunesEqual(const std::vector<uint32_t>& a, const char* b) {
    return a == DecodeRunes(b);
}

// 纯应声/感叹碎片：不构成新语句（倾听态回复它无意义，播放期更不应打断）
bool IsBareInterjection(const std::vector<uint32_t>& norm) {
    static const char* kInterjections[] = {
        "嗯", "哦", "啊", "呃", "唉", "唔", "哼", "喂",
        "嗯嗯", "哦哦", "啊啊", "呃呃",
        "oah", "ah", "oh", "eh", "hmm", "mhm", "aha", "halo",
        "hello", "hi", "hey",
    };
    if (norm.empty() || norm.size() > 6) return false;
    for (const char* w : kInterjections)
        if (RunesEqual(norm, w)) return true;
    return false;
}

// 回声：final 与本机最近播报文本高度相似（AEC 旁路时扬声器回放被 mic 收回）
bool IsEchoOfSpoken(const std::vector<uint32_t>& norm,
                    const std::vector<std::string>& spoken) {
    if (norm.size() < 4) return false;
    for (const auto& sp : spoken) {
        const auto ns = NormalizeSpeech(sp);
        if (ns.empty()) continue;
        // 包含判定（播报长句被部分识别）
        if (ns.size() >= norm.size()) {
            bool contained = false;
            for (size_t off = 0; off + norm.size() <= ns.size() && !contained; ++off) {
                contained = true;
                for (size_t i = 0; i < norm.size(); ++i)
                    if (ns[off + i] != norm[i]) { contained = false; break; }
            }
            if (contained) return true;
        }
        if (norm.size() >= 6 && BigramJaccard(norm, ns) >= 0.5) return true;
    }
    return false;
}

// 本地打断判定（原 feino JudgeInterrupt 规则移植，省一次 gRPC 往返）：
// 噪声幻听碎片（<4 字）/应声词/单字反复垃圾文本/回声均不打断；
// 真机噪声段 ASR 幻觉（"这个这方"/"没不了"/"hello"类）在此拦截
bool JudgeInterruptLocal(const std::vector<uint32_t>& norm,
                         const std::vector<std::string>& spoken,
                         const char** reason) {
    if (norm.size() < 4) { *reason = "too_short"; return false; }
    if (IsBareInterjection(norm)) { *reason = "filler"; return false; }
    // 垃圾：不同字数 ≤2，或单字重复占比过半（"证证证证"/"这个这方"类）
    {
        std::unordered_map<uint32_t, int> freq;
        int maxFreq = 0;
        for (const auto cp : norm) maxFreq = std::max(maxFreq, ++freq[cp]);
        if (freq.size() <= 2) { *reason = "garbage_low_diversity"; return false; }
        if (maxFreq * 2 >= static_cast<int>(norm.size())) {
            *reason = "garbage_repetition"; return false;
        }
    }
    if (IsEchoOfSpoken(norm, spoken)) { *reason = "echo"; return false; }
    return true;
}

// 默认 A 段承接语料：模糊、通用（只表"我在听"），3-5 秒朗读时长；
// 无缓存/缓存与上次重复时轮换使用，避免每次完全一样
const char* PickDefaultSoothe(uint32_t idx) {
    static const char* kLines[] = {
        "嗯嗯，我在听呢，你慢慢说。",
        "我在呢，你接着说，我听着。",
        "嗯，好的，我在，你继续讲。",
        "嗯嗯，不着急，慢慢来，我在听。",
        "好，我听着呢，你继续说。",
        "嗯，我在呢，慢慢说没关系。",
    };
    return kLines[idx % 6];
}

} // namespace

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
    case MsgType::kLlmFailed: OnLlmFailed(m, cs); break;
    case MsgType::kTtsAudioChunk: OnTtsChunk(m, cs); break;
    case MsgType::kWmDetected: OnWmDetected(m, cs); break;
    case MsgType::kAecBypass: OnAecBypass(m, cs); break;
    case MsgType::kVadUpdate: OnVadUpdate(m, cs); break;
    case MsgType::kAudioCancel: OnAudioCancel(m, cs); break;
    case MsgType::kInterruptVerdict: OnInterruptVerdict(m, cs); break;
    case MsgType::kWmRetry: {
        // 标定超时重试（ws 侧首轮脉冲常被真机 AGC 爬坡/通道预热吃掉）：
        // 仍在标定期 → 重发水印 clip 再测一轮
        auto* s = m_board.FindSession(m.session_id);
        if (s && s->m_wmPending &&
            static_cast<uint64_t>(m.aux) == s->m_connGeneration) {
            s->m_wmSendAtMs = 0; // 取消可能未到期的计划发送，防重复下发
            // 16k 生成（协议插件下行统一抽半 16k→8k），端侧播放与 8k
            // 检测模板频率/时长一致
            audio::WatermarkConfig wcfg;
            const auto pcm = audio::GenerateWatermark(wcfg);
            cs.ws_sends.push_back(WsOutbound{m.session_id, clip::kWatermark, false,
                                             audio::EncodeALaw(pcm)});
            m_board.MarkChanged(m.session_id);
        }
        break;
    }
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
    case MsgType::kAsrPartial: {
        // ASR 确认门：当前语句有真实语音（非空 partial）才允许触发 quick——
        // 纯噪声 VAD 段（空识别）不再误触发安抚（真机噪声 long_utt 根因）
        auto* s = m_board.FindSession(m.session_id);
        if (s && NormalizeSpeech(m.text).size() >= 2) s->m_uttAsrConfirmed = true;
        break;
    }
    default:
        break; // kTick*/kControlAck/kMemoryAck：本轮无心跳内动作
    }
}

void HeartbeatEngine::OnWsConnected(const Message& m, ChangeSet& cs) {
    // text = uid（来自 AuthProvider，session_id = SessionIdFromUid(uid)）
    auto& s = m_board.GetOrCreateSession(m.session_id);
    const uint64_t gen = static_cast<uint64_t>(m.aux);
    if (gen < s.m_connGeneration) return; // 旧代际消息丢弃
    // 新物理连接接管会话：AEC 标定随连接失效——delay/skew 与设备声学
    // 环境/摆放相关，每连接重新标定；同时解锁"标定中途断连"或"标定超时
    // 旁路"遗留的 m_wmPending（否则锁存 true，后续连接永不重发水印）
    if (gen > s.m_connGeneration) {
        s.m_aecCalib = SessionContext::AecCalib{};
        s.m_wmPending = false;
        s.m_wmSendAtMs = 0;
    }
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
    // AEC 标定走内容锚定（calib_estimator，下行语音本身就是水印），
    // 不再下发特制水印音；连接即尝试回读该设备的历史标定缓存
    if (!s.m_aecCalib.valid && !s.m_uid.empty())
        cs.redis_ops.push_back(RedisOp{"GET", "aeccalib:" + s.m_uid, "", 0});
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnWsDisconnected(const Message& m, ChangeSet& cs) {
    (void)cs;
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 旧代际连接的迟到断连（快速重连时新连接已接管会话）→ 忽略，
    // 防误标离线、防误清新连接的标定状态
    if (static_cast<uint64_t>(m.aux) < s->m_connGeneration) return;
    s->m_state = SessionState::kOffline;
    s->m_lastSeenMs = m.ts_ms;
    // 标定随连接失效：复位 m_wmPending（含"标定中途断连"场景），
    // 重连时由 OnWsConnected 重新下发水印完成每连接标定
    s->m_aecCalib.valid = false;
    s->m_wmPending = false;
    s->m_wmSendAtMs = 0;
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
            s->m_aFallbackDone = false;      // 新一轮允许再次失败兜底
            s->m_degradedSent = false;
            s->m_uttAsrConfirmed = false;    // 新语句需重新获得 ASR 确认
        }
        s->m_lastVoiceMs = m.ts_ms;
        s->m_state = SessionState::kListening;
    }
    if (m.flags & (msgflag::kVadEnd | msgflag::kAsrEndpoint)) s->m_vadEndpoint = true;
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnAsrFinal(const Message& m, ChangeSet& cs) {
    auto& s = m_board.GetOrCreateSession(m.session_id);
    // 回声/碎片/空文本过滤：空文本与单字 final 不提交 LLM（噪声段
    // 占绝大多数）；正在播放的文本与最近播报文本被 ASR 识别回来的
    // final 一律丢弃——绝不提交 feino/LLM，否则自己和自己说个没完
    const auto norm = NormalizeSpeech(m.text);
    if (norm.size() >= 2) s.m_uttAsrConfirmed = true; // final 非空：语句确认有真实语音
    std::vector<std::string> spoken(s.m_recentSpoken.begin(), s.m_recentSpoken.end());
    for (const auto& cl : s.m_playQueue)
        if (!cl.text.empty()) spoken.push_back(cl.text);
    if (norm.size() < 2 || IsBareInterjection(norm) || IsEchoOfSpoken(norm, spoken)) {
        cs.board_writes.push_back(
            BoardMutation{m.session_id, "final_dropped", m.text.substr(0, 64)});
        return;
    }
    // 播放/思考期间收到的 final 是打断候选：本地语义判定（原 feino
    // JudgeInterrupt 规则，挪回 mediator 省 gRPC 往返），噪声幻听/回声/
    // 应声词在此过滤，不再秒杀回复
    if (s.m_state == SessionState::kThinking || !s.m_playQueue.empty()) {
        // 回复启动保护窗：同一句话被 VAD 切成两段时，后到的碎片 final
        // 不得打断刚启动的回复流水线（真机"今天天气如何"被切成
        // "今天天气如何"+"今天天气"，碎片把正解杀掉的根因）
        if (s.m_thinkingStartMs > 0 &&
            m.ts_ms - s.m_thinkingStartMs < 2500) {
            cs.board_writes.push_back(
                BoardMutation{m.session_id, "final_dropped",
                              m.text.substr(0, 64)});
            return;
        }
        const char* reason = nullptr;
        if (!JudgeInterruptLocal(norm, spoken, &reason)) {
            cs.board_writes.push_back(
                BoardMutation{m.session_id, "final_dropped",
                              std::string("judge:") + reason + " " +
                                  m.text.substr(0, 48)});
            return;
        }
        // 判定为真打断：立即执行（与 OnInterruptVerdict 路径等价，无 RPC 延迟）
        cs.board_writes.push_back(
            BoardMutation{m.session_id, "barge_src", "asr_final " + m.text.substr(0, 32)});
        DoBargeIn(s, m.ts_ms, cs);
        s.m_vadEndpoint = true; // 打断语句已说完，补偿断句标志让 A 段安抚照常触发
        StartUtterance(s, m.text, m.ts_ms, cs);
        return;
    }
    StartUtterance(s, m.text, m.ts_ms, cs);
}

void HeartbeatEngine::OnInterruptVerdict(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s || s->m_state == SessionState::kOffline) return;
    if (m.aux == 0) return; // feino 判定不打断（噪声/回声/应声）→ 丢弃该 final
    // 判定到达时回复可能已播完：仅仍在思考/播放才需要打断动作
    if (s->m_state == SessionState::kThinking || !s->m_playQueue.empty()) {
        cs.board_writes.push_back(
            BoardMutation{m.session_id, "barge_src", "verdict " + m.text.substr(0, 32)});
        DoBargeIn(*s, m.ts_ms, cs);
        // 打断语句已说完（final 即证据）：补偿被 DoBargeIn 复位的断句标志，
        // 让新语句的 A 段安抚照常触发（与倾听态 final 流程对齐）
        s->m_vadEndpoint = true;
    }
    StartUtterance(*s, m.text, m.ts_ms, cs);
}

void HeartbeatEngine::StartUtterance(SessionContext& s, const std::string& text,
                                     int64_t now, ChangeSet& cs) {
    s.m_state = SessionState::kThinking;
    s.m_uttActive = false;
    // 回复新一轮：复位连续有声计数/计时——用户自己的语音不得计入
    // "打断自己回复"的判定（真机 final 后 50ms 噪声秒杀回复的根因）；
    // 记录保护窗起点并清开播标记（A 未开播前的保护窗内不取消）
    s.m_voiceRun = 0;
    s.m_voiceRunStartMs = 0;
    s.m_thinkingStartMs = now;
    s.m_replyStartedMs = 0;
    // 注意：不再清除 m_vadEndpoint —— VAD 帧与 Final 可能同心跳到达，
    // QuickRespTrigger 依赖该标志在本心跳内触发安抚音频（竞态修复）
    // 上下文累积（Redis 持久化走 CtxEvict/同步路径）
    s.m_chatCtx += "U:" + text + "\n";
    // 并行：复述 B + 完整答案 C
    ClipBuffer b; b.id = clip::kRestate; b.requested_ms = now;
    ClipBuffer c; c.id = clip::kAnswer; c.requested_ms = now;
    s.m_playQueue.push_back(std::move(b));
    s.m_playQueue.push_back(std::move(c));
    const int64_t gen = static_cast<int64_t>(s.m_uttGen);
    cs.grpc_calls.push_back(GrpcCall{"llm", "restate", s.m_sessionId, clip::kRestate, text, gen});
    cs.grpc_calls.push_back(GrpcCall{"llm", "answer", s.m_sessionId, clip::kAnswer, text, gen});
    m_board.MarkChanged(s.m_sessionId);
}

void HeartbeatEngine::OnLlmText(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 打断后迟到的旧代际结果直接丢弃（防止已取消语句复活）
    if (static_cast<uint64_t>(m.aux) != s->m_uttGen) {
        cs.board_writes.push_back(
            BoardMutation{m.session_id, "llm_stale_drop",
                          m.text.substr(0, 48)});
        return;
    }
    ClipId id = clip::kNone;
    switch (m.type) {
    case MsgType::kLlmQuickResp: id = (m.clip_id == clip::kPlaceholder) ? clip::kPlaceholder : clip::kSoothe; break;
    case MsgType::kLlmRestate: id = clip::kRestate; break;
    case MsgType::kLlmFinalAnswer: id = clip::kAnswer; break;
    default: break;
    }
    if (m.type == MsgType::kLlmQuickResp && id == clip::kSoothe) {
        // A 段播出已由缓存/默认语承担（EvolveQuickResp 零等待路径）——
        // LLM 结果只刷新缓存：单独合成（kSootheCache，不进播放队列），
        // 供下一次触发复用，不与 B/C 内容重复
        s->m_lastSoothe.id = clip::kSoothe;
        s->m_lastSoothe.text = m.text;
        s->m_lastSoothe.text_ready = true;
        s->m_hasLastSoothe = true;
        EmitTts(*s, clip::kSootheCache, m.text, cs);
    } else if (auto* cl = FindClip(*s, id)) {
        cl->text = m.text;
        cl->text_ready = true;
        EmitTts(*s, id, m.text, cs);
    }
    // 记录最近播报文本（回声抑制：AEC 旁路时识别到自己声音的 final 将被丢弃）
    if (!m.text.empty()) {
        s->m_recentSpoken.push_back(m.text);
        while (s->m_recentSpoken.size() > 8) s->m_recentSpoken.pop_front();
    }
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::EmitTts(SessionContext& s, ClipId clip, const std::string& text,
                              ChangeSet& cs) {
    cs.grpc_calls.push_back(GrpcCall{"tts", "synth", s.m_sessionId, clip, text,
                                     static_cast<int64_t>(s.m_uttGen)});
}

// ---- LLM 失败兜底（A 段 → P 段 → 缓存占位 → degraded）----
// A 段(quick)失败/超时：立即走占位兜底（每语句一次，不与 B/C 慢重复）——
// 有上一次占位音缓存直接复用（离线可播），无缓存则请求 P 段生成；
// P 再失败：摘掉永不就绪的占位 clip（防堵死队首 B/C），有缓存复用，
// 无缓存给端侧明确 degraded 状态帧（不死寂）。B/C 生成不受影响照常并行。
void HeartbeatEngine::OnLlmFailed(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s || s->m_state == SessionState::kOffline) return;
    if (static_cast<uint64_t>(m.aux) != s->m_uttGen) return; // 旧代际失败忽略
    if (m.text == "quick") {
        if (s->m_aFallbackDone) return;
        s->m_aFallbackDone = true;
        TriggerPlaceholderFallback(*s, cs);
    } else if (m.text == "quick_placeholder") {
        for (auto it = s->m_playQueue.begin(); it != s->m_playQueue.end(); ++it) {
            if (it->id == clip::kPlaceholder && !it->audio_ready) {
                s->m_playQueue.erase(it);
                break;
            }
        }
        if (s->m_hasLastPlaceholder) {
            if (!FindClip(*s, clip::kPlaceholder)) EnqueueReusePlaceholder(*s, cs);
        } else if (!s->m_degradedSent) {
            s->m_degradedSent = true;
            const std::string t =
                R"(status:{"status":"degraded","reason":"llm_unavailable"})";
            cs.ws_sends.push_back(
                WsOutbound{m.session_id, clip::kNone, true, {t.begin(), t.end()}});
        }
    }
    // restate/answer 失败：由 EvolvePlayback 的 B/C 超时预算走占位路径，此处不处理
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::TriggerPlaceholderFallback(SessionContext& s, ChangeSet& cs) {
    if (FindClip(s, clip::kPlaceholder)) return; // 已有占位在途（如 B 超时触发）
    if (s.m_hasLastPlaceholder) {
        EnqueueReusePlaceholder(s, cs); // 离线兜底：直接播上一次占位音
        return;
    }
    ClipBuffer p; p.id = clip::kPlaceholder; p.requested_ms = 0;
    s.m_playQueue.push_front(std::move(p));
    cs.grpc_calls.push_back(GrpcCall{"llm", "quick_placeholder", s.m_sessionId,
                                     clip::kPlaceholder, s.m_chatCtx,
                                     static_cast<int64_t>(s.m_uttGen)});
}

void HeartbeatEngine::OnTtsChunk(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 打断后迟到的旧代际音频直接丢弃（防止已取消语句继续播放）
    if (static_cast<uint64_t>(m.aux) != s->m_uttGen) return;
    // A 段缓存刷新合成：只沉淀缓存（文本已在 OnLlmText 写入），不进播放队列
    if (m.clip_id == clip::kSootheCache) {
        s->m_lastSoothe.g711.assign(m.payload.begin(), m.payload.end());
        s->m_lastSoothe.audio_ready = true;
        s->m_hasLastSoothe = true;
        m_board.MarkChanged(m.session_id);
        return;
    }
    auto* cl = FindClip(*s, m.clip_id);
    if (!cl) {
        ClipBuffer nb; nb.id = m.clip_id;
        s->m_playQueue.push_back(std::move(nb));
        cl = FindClip(*s, m.clip_id);
    }
    cl->g711.insert(cl->g711.end(), m.payload.begin(), m.payload.end());
    cl->audio_ready = true;
    // 默认承接语的合成音同样沉淀进 A 段缓存（LLM quick 失败时的兜底缓存）
    if (m.clip_id == clip::kSoothe && !s->m_hasLastSoothe) {
        s->m_lastSoothe = *cl;
        s->m_hasLastSoothe = true;
    }
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
    s.m_lastBargeMs = now;
    s.m_placeholderRounds = 0;
    s.m_quickRespSubmitted = false; // 新语句允许再次触发安抚
    s.m_aFallbackDone = false;      // 新语句允许再次失败兜底
    s.m_degradedSent = false;
    s.m_vadEndpoint = false;
    s.m_voiceRun = 0;          // 打断后重新累计持续语音（防连锁打断）
    s.m_voiceRunStartMs = 0;
    s.m_uttActive = true;
    s.m_uttStartMs = now;
    s.m_state = SessionState::kListening;
    cs.board_writes.push_back(BoardMutation{s.m_sessionId, "interrupted", "1"});
    m_board.MarkChanged(s.m_sessionId);
}

void HeartbeatEngine::OnVadUpdate(const Message& m, ChangeSet& cs) {
    (void)cs;
    auto* s = m_board.FindSession(m.session_id);
    if (!s || s->m_state == SessionState::kOffline) return;
    if (s->m_wmPending) return; // 标定期不识别
    if (m.flags & msgflag::kVoice) {
        ++s->m_voiceRun;
        if (s->m_voiceRun == 1) s->m_voiceRunStartMs = m.ts_ms; // 连续有声段起点
        // 注意：VAD 帧不再触发打断——真机噪声/回声下能量型打断会误杀回复
        // （6s 周期性误打断根因）。打断统一走 ASR final → feino 语义判定
        // （OnAsrFinal → judge_interrupt → OnInterruptVerdict）
        if (s->m_state != SessionState::kThinking && s->m_playQueue.empty()) {
            s->m_state = SessionState::kListening;
        }
        if (!s->m_uttActive) {
            s->m_uttActive = true;
            s->m_uttStartMs = m.ts_ms;
            s->m_vadEndpoint = false;      // 新语句开始才重置断句标志
            s->m_quickRespSubmitted = false;
            s->m_aFallbackDone = false;
            s->m_degradedSent = false;
            s->m_uttAsrConfirmed = false;  // 新语句需重新获得 ASR 确认
        }
        s->m_lastVoiceMs = m.ts_ms;
    } else {
        s->m_voiceRun = 0;
        s->m_voiceRunStartMs = 0; // 静音中断：持续计时一并复位
    }
    if (m.flags & (msgflag::kVadEnd | msgflag::kAsrEndpoint)) s->m_vadEndpoint = true;
    m_board.MarkChanged(m.session_id);
}

void HeartbeatEngine::OnAudioCancel(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    // 端侧主动取消（如本地 VAD 打断）：与 barge-in 同语义，空闲时幂等无操作。
    // 去重窗口：端侧收到 interrupted 状态后会回送 cancel（ConvaiBridge 不区分
    // 打断来源），1.5s 内的重复 cancel 不得再次打断——否则刚提交的新语句
    // restate/answer 被二次 gen++ 判死（真机"问好吃的无回复"根因）
    if (m.ts_ms - s->m_lastBargeMs < 1500) return;
    if (s->m_state == SessionState::kThinking || !s->m_playQueue.empty()) {
        cs.board_writes.push_back(
            BoardMutation{m.session_id, "barge_src", "audio_cancel"});
        DoBargeIn(*s, m.ts_ms, cs);
    }
}

void HeartbeatEngine::OnWmDetected(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    s->m_aecCalib.valid = true;
    s->m_aecCalib.delay_samples = static_cast<int32_t>(m.aux);
    s->m_aecCalib.skew = m.dval;
    s->m_wmPending = false;
    // 按设备缓存标定结果：真机水印检测常因 AGC 爬坡/回声污染失败，
    // 同一设备的声学延迟高度稳定（实测 ±60 采样），下次标定失败时
    // 回读缓存做 AEC（近似延迟远优于完全旁路）
    if (!s->m_uid.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d,%.8f", s->m_aecCalib.delay_samples,
                      s->m_aecCalib.skew);
        cs.redis_ops.push_back(
            RedisOp{"SETEX", "aeccalib:" + s->m_uid, buf, 7 * 24 * 3600});
    }
    m_board.MarkChanged(m.session_id);
}

// 标定超时旁路（ws_server 15s 兜底）：只解锁上行丢帧门——m_aecCalib.valid
// 保持 false（AEC 直通，不是伪标定；APM 不收 SetCalib，render/capture 不
// 对齐即旁路）。旧代际连接的迟到 bypass（重连已重新进入标定期）不解锁。
void HeartbeatEngine::OnAecBypass(const Message& m, ChangeSet& cs) {
    auto* s = m_board.FindSession(m.session_id);
    if (!s) return;
    if (static_cast<uint64_t>(m.aux) < s->m_connGeneration) return;
    s->m_wmPending = false;
    // 旁路前尝试回读该设备的历史标定（近似延迟做 AEC，优于完全旁路）；
    // 命中后由网关回注 kWmDetected 落地 calib
    if (!s->m_aecCalib.valid && !s->m_uid.empty())
        cs.redis_ops.push_back(RedisOp{"GET", "aeccalib:" + s->m_uid, "", 0});
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
    // 噪声门：ASR 未确认当前语句有真实语音（partial/final 非空）→ 纯噪声
    // VAD 段不提交 quick（真机噪声段 5s long_utt 误触发安抚的根因）
    if (!s.m_uttAsrConfirmed) return;
    // 回复已开播或已有就绪文本（B/C/A）：安抚语再到只会与复述/答案重复
    // （真机 quick 比 answer 还晚到、内容与复述撞车的根因）
    if (s.m_replyStartedMs > 0) return;
    for (const auto& cl : s.m_playQueue)
        if (cl.text_ready) return;
    s.m_quickRespSubmitted = true;
    // 立即播出，不等 LLM：有缓存音且与上次播出不同 → 0 等待复用；
    // 否则默认模糊承接语轮换（仅 TTS 延迟）。LLM quick 照常提交，
    // 结果只刷新缓存（kSootheCache 合成），供下一次使用
    if (s.m_hasLastSoothe && s.m_lastSoothe.audio_ready &&
        s.m_lastSoothe.text != s.m_prevSootheText) {
        ClipBuffer a = s.m_lastSoothe;
        a.id = clip::kSoothe;
        a.sent_bytes = 0;
        s.m_prevSootheText = a.text;
        s.m_playQueue.push_front(std::move(a));
    } else {
        const char* line = PickDefaultSoothe(s.m_sootheIdx);
        if (s.m_prevSootheText == line) line = PickDefaultSoothe(s.m_sootheIdx + 1);
        ++s.m_sootheIdx;
        ClipBuffer a;
        a.id = clip::kSoothe;
        a.text = line;
        a.text_ready = true;
        s.m_prevSootheText = line;
        s.m_playQueue.push_front(std::move(a));
        EmitTts(s, clip::kSoothe, line, cs);
    }
    cs.grpc_calls.push_back(
        GrpcCall{"llm", "quick", s.m_sessionId, clip::kSoothe, s.m_chatCtx,
                 static_cast<int64_t>(s.m_uttGen)});
    m_board.MarkChanged(s.m_sessionId);
}

void HeartbeatEngine::EvolvePlayback(SessionContext& s, int64_t now, ChangeSet& cs) {
    // 队首 clip 就绪 → 下发剩余字节
    while (!s.m_playQueue.empty()) {
        auto& front = s.m_playQueue.front();
        if (front.audio_ready) {
            if (front.sent_bytes < front.g711.size()) {
                // 回复开播标记：首个非占位 clip 下发即解除 A 段保护窗
                if (front.id != clip::kPlaceholder && s.m_replyStartedMs == 0)
                    s.m_replyStartedMs = now;
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

// 到期的计划水印下发（连接后 3s 首播；重试由 kWmRetry 立即下发不经此路）
void HeartbeatEngine::EvolveWatermarkSend(int64_t now, ChangeSet& cs) {
    m_board.ForEachSession([&](const SessionId& sid, SessionContext& s) {
        if (!s.m_wmPending || s.m_wmSendAtMs == 0 || now < s.m_wmSendAtMs) return;
        s.m_wmSendAtMs = 0;
        audio::WatermarkConfig wcfg; // 16k 生成（插件下行抽半，见首播处注释）
        const auto pcm = audio::GenerateWatermark(wcfg);
        cs.ws_sends.push_back(WsOutbound{sid, clip::kWatermark, false,
                                         audio::EncodeALaw(pcm)});
        m_board.MarkChanged(sid);
    });
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
