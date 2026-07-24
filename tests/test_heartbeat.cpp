// ============================================================================
// test_heartbeat.cpp — 心跳引擎全量单测（设计文档 §9.3）
//
// 覆盖：连接/水印标定期丢帧、QuickResp 三触发条件、三段式流水线、
//       播放严格顺序、B/C 超时占位（生成/复用/stall）、会话 GC 与重连、
//       旧代际丢弃、上下文淘汰、确定性重放。
// 方法：VirtualClock + Inject + RunOnce 返回 ChangeSet 断言（§9.1 钩子）。
// ============================================================================
#include <gtest/gtest.h>

#include "core/clock.h"
#include "engine/heartbeat_engine.h"

using namespace mediator;

namespace {

constexpr const char* kUid = "user-1";
const SessionId kSid = SessionIdFromUid(kUid);

Message Msg(MsgType t, int64_t ts) {
    Message m;
    m.type = t;
    m.session_id = kSid;
    m.ts_ms = ts;
    return m;
}

struct Fixture : ::testing::Test {
    VirtualClock clock{100000};
    EngineConfig cfg;
    HeartbeatEngine engine{cfg, clock};

    // 建立连接并完成标定（内容锚定方案：不下发水印，标定由估计器完成，
    // 多数用例的前置）
    void ConnectAndCalibrate() {
        auto c = Msg(MsgType::kWsConnected, clock.NowMs());
        c.text = kUid;
        c.aux = 1; // gen=1
        engine.Inject(std::move(c));
        engine.RunOnce();
        auto w = Msg(MsgType::kWmDetected, clock.NowMs());
        w.aux = 4800;   // delay_samples
        w.dval = 1e-4;  // skew
        engine.Inject(std::move(w));
        engine.RunOnce();
    }

    // 发送一帧有声语音
    void Voice(int64_t ts, uint32_t flags = msgflag::kVoice) {
        auto f = Msg(MsgType::kWsAudioFrame, ts);
        f.flags = flags;
        engine.Inject(std::move(f));
    }

    // 语义打断：播放/思考期收到 ASR final → 引擎发 judge_interrupt 调用
    // （注入后需 RunOnce 一次），再注入 feino 判定结果（默认允许打断）
    void SendBargeFinal(const std::string& text = "新问题") {
        auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
        fin.text = text;
        engine.Inject(std::move(fin));
    }
    void SendVerdict(bool interrupt, const std::string& text = "新问题") {
        auto v = Msg(MsgType::kInterruptVerdict, clock.NowMs());
        v.text = text;
        v.aux = interrupt ? 1 : 0;
        engine.Inject(std::move(v));
    }
    void SemanticBarge(const std::string& text = "新问题") {
        SendBargeFinal(text);
        engine.RunOnce(); // → judge_interrupt grpc call
        SendVerdict(true, text);
    }

    static size_t CountGrpc(const ChangeSet& cs, const std::string& svc,
                            const std::string& method) {
        size_t n = 0;
        for (const auto& g : cs.grpc_calls)
            if (g.service == svc && g.method == method) ++n;
        return n;
    }
};

// ---- 连接与水印标定 ----

// 内容锚定方案：连接不下发任何水印 clip（下行语音本身做校准"水印"）
TEST_F(Fixture, ConnectSendsNoWatermark) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid;
    c.aux = 1;
    engine.Inject(std::move(c));
    const auto cs = engine.RunOnce();
    EXPECT_TRUE(cs.ws_sends.empty());

    clock.Advance(3000);
    const auto cs2 = engine.RunOnce();
    EXPECT_TRUE(cs2.ws_sends.empty()); // 任何时刻都不发水印

    Voice(clock.NowMs());
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->m_uttActive); // 无标定期丢帧门，语音直接生效
}

TEST_F(Fixture, WmDetectedCompletesCalib) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid; c.aux = 1;
    engine.Inject(std::move(c));
    engine.RunOnce();

    auto w = Msg(MsgType::kWmDetected, clock.NowMs());
    w.aux = 4800; w.dval = 1e-4;
    engine.Inject(std::move(w));
    engine.RunOnce();

    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_TRUE(s->m_aecCalib.valid);
    EXPECT_EQ(s->m_aecCalib.delay_samples, 4800);
    EXPECT_DOUBLE_EQ(s->m_aecCalib.skew, 1e-4);
    EXPECT_FALSE(s->m_wmPending);
}

// ---- QuickResp 三触发条件 ----

TEST_F(Fixture, QuickRespTriggersAfter5s) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce(); // 说话开始
    clock.Advance(5001);
    Voice(clock.NowMs());
    const auto cs = engine.RunOnce(); // 演进：>5s → llm quick
    EXPECT_EQ(CountGrpc(cs, "llm", "quick"), 1u);
}

TEST_F(Fixture, QuickRespTriggersOnVadEnd) {
    ConnectAndCalibrate();
    clock.Advance(1000); // 不足 5s
    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kVadEnd);
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "quick"), 1u);
}

TEST_F(Fixture, QuickRespTriggersOnAsrEndpoint) {
    ConnectAndCalibrate();
    clock.Advance(1000);
    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kAsrEndpoint);
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "quick"), 1u);
}

TEST_F(Fixture, QuickRespSubmittedOnlyOnce) {
    ConnectAndCalibrate();
    clock.Advance(1000);
    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kVadEnd);
    engine.RunOnce();
    clock.Advance(6000);
    Voice(clock.NowMs());
    const auto cs = engine.RunOnce(); // 已提交过，不再重复
    EXPECT_EQ(CountGrpc(cs, "llm", "quick"), 0u);
}

// ---- 三段式流水线 ----

TEST_F(Fixture, AsrFinalSubmitsRestateAndAnswer) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce();

    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "今天天气怎么样";
    engine.Inject(std::move(fin));
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "restate"), 1u);
    EXPECT_EQ(CountGrpc(cs, "llm", "answer"), 1u);
}

TEST_F(Fixture, LlmTextTriggersTtsPerClip) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();

    auto qa = Msg(MsgType::kLlmQuickResp, clock.NowMs());
    qa.text = "明白，我想想";
    engine.Inject(std::move(qa));
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "tts", "synth"), 1u); // TTS(安抚)

    auto rb = Msg(MsgType::kLlmRestate, clock.NowMs());
    rb.text = "你问的是问题";
    engine.Inject(std::move(rb));
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "tts", "synth"), 1u); // TTS(复述)
}

TEST_F(Fixture, PlaybackOrderSootheRestateAnswer) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();

    // 安抚文本与音频先就绪
    auto qa = Msg(MsgType::kLlmQuickResp, clock.NowMs());
    qa.text = "嗯我想想";
    engine.Inject(std::move(qa));
    engine.RunOnce();
    auto ta = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    ta.clip_id = clip::kSoothe;
    ta.payload = {1, 2, 3};
    engine.Inject(std::move(ta));
    const auto cs = engine.RunOnce();
    // 说话已结束（Thinking），安抚音频立即下发
    ASSERT_EQ(cs.ws_sends.size(), 1u);
    EXPECT_EQ(cs.ws_sends[0].bytes, (std::vector<uint8_t>{1, 2, 3}));

    // 复述就绪 → 下发
    auto tb = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    tb.clip_id = clip::kRestate;
    tb.payload = {4, 5};
    engine.Inject(std::move(tb));
    const auto cs2 = engine.RunOnce();
    ASSERT_EQ(cs2.ws_sends.size(), 1u);
    EXPECT_EQ(cs2.ws_sends[0].bytes, (std::vector<uint8_t>{4, 5}));

    // 答案就绪 → 下发
    auto tc = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    tc.clip_id = clip::kAnswer;
    tc.payload = {6};
    engine.Inject(std::move(tc));
    const auto cs3 = engine.RunOnce();
    ASSERT_EQ(cs3.ws_sends.size(), 1u);
    EXPECT_EQ(cs3.ws_sends[0].bytes, (std::vector<uint8_t>{6}));
}

// ---- 超时占位 ----

TEST_F(Fixture, RestateTimeoutGeneratesPlaceholder) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    // 安抚音已播完
    auto ta = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    ta.clip_id = clip::kSoothe; ta.payload = {1};
    engine.Inject(std::move(ta));
    engine.RunOnce();

    clock.Advance(8001); // 超复述预算 8s
    Voice(clock.NowMs()); // 触发 changed（任意消息）
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "quick_placeholder"), 1u);
}

TEST_F(Fixture, PlaceholderReusedWhenSootheNotReady) {
    ConnectAndCalibrate();
    // 预置"上一次对话的占位音"缓存
    auto* s = engine.Board().FindSession(kSid);
    ClipBuffer last;
    last.id = clip::kPlaceholder;
    last.g711 = {9, 9, 9};
    last.audio_ready = true;
    s->m_lastPlaceholder = last;
    s->m_hasLastPlaceholder = true;

    Voice(clock.NowMs());
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce(); // 安抚文本未生成（A 未就绪）

    clock.Advance(8001);
    Voice(clock.NowMs());
    const auto cs = engine.RunOnce();
    // 走复用路径：不发 quick_placeholder，直接下发缓存音频
    EXPECT_EQ(CountGrpc(cs, "llm", "quick_placeholder"), 0u);
    bool reused = false;
    for (const auto& b : cs.board_writes)
        if (b.field == "placeholder_reused") reused = true;
    EXPECT_TRUE(reused);
    // 下一轮心跳：复用的占位音下发
    const auto cs2 = engine.RunOnce();
    ASSERT_FALSE(cs2.ws_sends.empty());
    EXPECT_EQ(cs2.ws_sends[0].bytes, (std::vector<uint8_t>{9, 9, 9}));
}

TEST_F(Fixture, StallsAfterMaxPlaceholderRounds) {
    ConnectAndCalibrate();
    Voice(clock.NowMs());
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    auto ta = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    ta.clip_id = clip::kSoothe; ta.payload = {1};
    engine.Inject(std::move(ta));
    engine.RunOnce();

    // 第1轮：超时 → 生成占位音（A 未就绪但无缓存，只能生成）
    clock.Advance(8001);
    Voice(clock.NowMs());
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "quick_placeholder"), 1u);
    auto tp = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    tp.clip_id = clip::kPlaceholder; tp.payload = {7};
    engine.Inject(std::move(tp));
    engine.RunOnce(); // 占位音播完；同时缓存为"上一次占位音"

    // 第2轮：超时 → A 仍未就绪 → 复用缓存（计入轮次，不再生成）
    clock.Advance(8001);
    Voice(clock.NowMs());
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "llm", "quick_placeholder"), 0u);
    engine.RunOnce(); // 复用的占位音播完

    // 第3次超时：占位轮次耗尽 → stall
    clock.Advance(8001);
    Voice(clock.NowMs());
    const auto cs3 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs3, "llm", "quick_placeholder"), 0u);
    bool stalled = false;
    for (const auto& b : cs3.board_writes)
        if (b.field == "clip_stalled") stalled = true;
    EXPECT_TRUE(stalled);
}

// ---- 会话生命周期 ----

TEST_F(Fixture, GcAfter3MinOfflineCallsMemoryAndCleans) {
    ConnectAndCalibrate();
    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
    d.aux = 1; // 断连事件携带连接代际（ws_server CleanupConn 注入）
    engine.Inject(std::move(d));
    engine.RunOnce();

    clock.Advance(180001); // >3min
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "memory", "FetchContext"), 1u);
    bool redis_del = false;
    for (const auto& r : cs.redis_ops)
        if (r.op == "DEL") redis_del = true;
    EXPECT_TRUE(redis_del);
    EXPECT_EQ(engine.Board().FindSession(kSid), nullptr);
}

TEST_F(Fixture, ReconnectKeepsContextAndRecalibratesAec) {
    ConnectAndCalibrate();
    engine.Board().FindSession(kSid)->m_chatCtx = "U:历史\n";
    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
    d.aux = 1;
    engine.Inject(std::move(d));
    engine.RunOnce();

    clock.Advance(60000); // 1min < 3min
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid; c.aux = 2; // gen+1
    engine.Inject(std::move(c));
    const auto cs = engine.RunOnce();

    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->m_connGeneration, 2u);
    EXPECT_EQ(s->m_chatCtx, "U:历史\n");       // 上下文保留（断线续聊）
    EXPECT_FALSE(s->m_aecCalib.valid);         // 标定随连接失效，每连接重标
    EXPECT_TRUE(cs.ws_sends.empty());          // 内容锚定：不下发水印
}

// 标定随连接失效：重连后 aecCalib 复位、回读历史标定缓存（内容锚定方案）
TEST_F(Fixture, ReconnectResetsCalibAndRestoresCache) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid; c.aux = 1;
    engine.Inject(std::move(c));
    engine.RunOnce();
    auto w = Msg(MsgType::kWmDetected, clock.NowMs());
    w.aux = 4800; w.dval = 0.0;
    engine.Inject(std::move(w));
    engine.RunOnce();

    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
    d.aux = 1;
    engine.Inject(std::move(d));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->m_aecCalib.valid); // 断连标定失效

    auto c2 = Msg(MsgType::kWsConnected, clock.NowMs());
    c2.text = kUid; c2.aux = 2;
    engine.Inject(std::move(c2));
    const auto cs = engine.RunOnce();
    // 重连回读该设备历史标定缓存（GET aeccalib:{uid}）
    bool get_calib = false;
    for (const auto& op : cs.redis_ops)
        if (op.op == "GET" && op.key == "aeccalib:" + std::string(kUid))
            get_calib = true;
    EXPECT_TRUE(get_calib);
}

// 连续重连竞态：旧连接的断连事件晚于新连接的建立到达 → 忽略，
// 不误标离线、不清新连接的标定期
TEST_F(Fixture, StaleDisconnectIgnoredAfterReconnect) {
    ConnectAndCalibrate();
    auto c2 = Msg(MsgType::kWsConnected, clock.NowMs());
    c2.text = kUid; c2.aux = 2;
    engine.Inject(std::move(c2));
    engine.RunOnce(); // 新连接接管（gen=2），重发水印进入标定期

    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
    d.aux = 1; // 旧 gen=1 连接的迟到断连
    engine.Inject(std::move(d));
    engine.RunOnce();

    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->m_state, SessionState::kOffline);
    EXPECT_EQ(s->m_connGeneration, 2u);
}

// ---- A 段失败兜底（kLlmFailed：A→P→缓存占位→degraded）----

// A 段(quick)失败 → 立即触发 P 段占位生成；同语句不重复触发；P 成功正常下发
TEST_F(Fixture, QuickFailureTriggersPlaceholderFallback) {
    ConnectAndCalibrate();
    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kVadEnd);
    const auto cs1 = engine.RunOnce(); // 断句触发 llm quick
    ASSERT_EQ(CountGrpc(cs1, "llm", "quick"), 1u);

    auto f = Msg(MsgType::kLlmFailed, clock.NowMs());
    f.text = "quick"; f.aux = 0; // 语句代际 0
    engine.Inject(std::move(f));
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "llm", "quick_placeholder"), 1u); // P 段被触发
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    ASSERT_FALSE(s->m_playQueue.empty());
    EXPECT_EQ(s->m_playQueue.front().id, clip::kPlaceholder);

    // 同语句再次失败 → 不重复触发（占位只播一次）
    auto f2 = Msg(MsgType::kLlmFailed, clock.NowMs());
    f2.text = "quick"; f2.aux = 0;
    engine.Inject(std::move(f2));
    const auto cs3 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs3, "llm", "quick_placeholder"), 0u);

    // P 成功（文本→TTS→音频）→ 占位音正常下发
    auto t = Msg(MsgType::kLlmQuickResp, clock.NowMs());
    t.clip_id = clip::kPlaceholder; t.text = "请稍等"; t.aux = 0;
    engine.Inject(std::move(t));
    auto a = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    a.clip_id = clip::kPlaceholder; a.payload = {1, 2, 3}; a.aux = 0;
    engine.Inject(std::move(a));
    const auto cs4 = engine.RunOnce();
    bool sent = false;
    for (const auto& w : cs4.ws_sends)
        if (w.clip_id == clip::kPlaceholder && w.bytes.size() == 3u) sent = true;
    EXPECT_TRUE(sent);
}

// A 失败且有上一次占位缓存 → 直接复用（离线兜底，不调 P 段）
TEST_F(Fixture, QuickFailureWithCacheReusesPlaceholder) {
    ConnectAndCalibrate();
    auto* s0 = engine.Board().FindSession(kSid);
    ASSERT_NE(s0, nullptr);
    s0->m_lastPlaceholder.id = clip::kPlaceholder;
    s0->m_lastPlaceholder.g711 = {7, 7, 7};
    s0->m_lastPlaceholder.audio_ready = true;
    s0->m_hasLastPlaceholder = true;

    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kVadEnd);
    engine.RunOnce();
    auto f = Msg(MsgType::kLlmFailed, clock.NowMs());
    f.text = "quick"; f.aux = 0;
    engine.Inject(std::move(f));
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "quick_placeholder"), 0u); // 不生成
    bool sent = false;
    for (const auto& w : cs.ws_sends)
        if (w.clip_id == clip::kPlaceholder &&
            w.bytes == std::vector<uint8_t>({7, 7, 7}))
            sent = true;
    EXPECT_TRUE(sent); // 缓存占位音直接下发
}

// 全失败（A→P 均失败且无缓存）→ 端侧收到 degraded 状态；未就绪占位摘除不堵队首
TEST_F(Fixture, AllLlmFailedSendsDegradedAndUnblocksQueue) {
    ConnectAndCalibrate();
    Voice(clock.NowMs(), msgflag::kVoice | msgflag::kVadEnd);
    engine.RunOnce(); // quick submitted
    auto f1 = Msg(MsgType::kLlmFailed, clock.NowMs());
    f1.text = "quick"; f1.aux = 0;
    engine.Inject(std::move(f1));
    engine.RunOnce(); // → P 段提交

    auto f2 = Msg(MsgType::kLlmFailed, clock.NowMs());
    f2.text = "quick_placeholder"; f2.aux = 0;
    engine.Inject(std::move(f2));
    const auto cs = engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->m_playQueue.empty()); // 未就绪占位被摘除（防堵死 B/C）
    bool degraded = false;
    for (const auto& w : cs.ws_sends)
        if (w.is_text &&
            std::string(w.bytes.begin(), w.bytes.end()).find("degraded") !=
                std::string::npos)
            degraded = true;
    EXPECT_TRUE(degraded);

    // 再次 P 失败 → degraded 每语句只发一次
    auto f3 = Msg(MsgType::kLlmFailed, clock.NowMs());
    f3.text = "quick_placeholder"; f3.aux = 0;
    engine.Inject(std::move(f3));
    const auto cs2 = engine.RunOnce();
    EXPECT_TRUE(cs2.ws_sends.empty());
}

// 打断后迟到的旧代际失败消息 → 忽略，不触发兜底
TEST_F(Fixture, StaleGenLlmFailedIgnored) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    SemanticBarge(); // 打断：uttGen 0→1
    engine.RunOnce();
    ASSERT_EQ(engine.Board().FindSession(kSid)->m_uttGen, 1u);

    auto f = Msg(MsgType::kLlmFailed, clock.NowMs());
    f.text = "quick"; f.aux = 0; // 旧代际
    engine.Inject(std::move(f));
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "llm", "quick_placeholder"), 0u);
    EXPECT_TRUE(cs.ws_sends.empty());
}

TEST_F(Fixture, OldGenerationMessageDiscarded) {
    ConnectAndCalibrate();
    auto c2 = Msg(MsgType::kWsConnected, clock.NowMs());
    c2.text = kUid; c2.aux = 5;
    engine.Inject(std::move(c2));
    engine.RunOnce();
    // 旧 gen=1 的连接事件再来 → 不覆盖
    auto c1 = Msg(MsgType::kWsConnected, clock.NowMs());
    c1.text = kUid; c1.aux = 1;
    engine.Inject(std::move(c1));
    engine.RunOnce();
    EXPECT_EQ(engine.Board().FindSession(kSid)->m_connGeneration, 5u);
}

// ---- 上下文淘汰 ----

TEST_F(Fixture, CtxEvictedOver1MbAndSyncedToRedis) {
    ConnectAndCalibrate();
    auto* s = engine.Board().FindSession(kSid);
    s->m_chatCtx.assign(1200 * 1024, 'x'); // 超 1MB，无换行 → 保留尾部 50%
    Voice(clock.NowMs());
    const auto cs = engine.RunOnce();
    const auto* s2 = engine.Board().FindSession(kSid);
    EXPECT_LE(s2->m_chatCtx.size(), (600u * 1024));
    bool synced = false;
    for (const auto& r : cs.redis_ops)
        if (r.op == "SETEX" && r.key.rfind("ctx:", 0) == 0) synced = true;
    EXPECT_TRUE(synced);
}

// ---- 确定性重放 ----

TEST_F(Fixture, DeterministicReplay) {
    auto run_script = [](HeartbeatEngine& e, VirtualClock& clk) {
        Message c; c.type = MsgType::kWsConnected; c.session_id = kSid;
        c.text = kUid; c.aux = 1; c.ts_ms = clk.NowMs();
        e.Inject(std::move(c));
        e.RunOnce();
        Message w; w.type = MsgType::kWmDetected; w.session_id = kSid;
        w.aux = 4800; w.dval = 1e-4; w.ts_ms = clk.NowMs();
        e.Inject(std::move(w));
        e.RunOnce();
        Message f; f.type = MsgType::kWsAudioFrame; f.session_id = kSid;
        f.flags = msgflag::kVoice | msgflag::kVadEnd; f.ts_ms = clk.NowMs();
        e.Inject(std::move(f));
        return e.RunOnce(); // 应触发 quick
    };
    VirtualClock c1{5000}, c2{5000};
    HeartbeatEngine e1(cfg, c1), e2(cfg, c2);
    const auto r1 = run_script(e1, c1);
    const auto r2 = run_script(e2, c2);
    ASSERT_EQ(r1.grpc_calls.size(), r2.grpc_calls.size());
    for (size_t i = 0; i < r1.grpc_calls.size(); ++i) {
        EXPECT_EQ(r1.grpc_calls[i].service, r2.grpc_calls[i].service);
        EXPECT_EQ(r1.grpc_calls[i].method, r2.grpc_calls[i].method);
    }
    EXPECT_EQ(r1.ws_sends.size(), r2.ws_sends.size());
}

// ---- 打断（barge-in）----

TEST_F(Fixture, BargeInClearsQueueAndNotifiesInterrupted) {
    ConnectAndCalibrate();
    // 语句结束进入思考态，复述/答案在队列中
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_state, SessionState::kThinking);
    ASSERT_EQ(s->m_playQueue.size(), 2u); // restate + answer
    ASSERT_EQ(s->m_uttGen, 0u);

    // 播放中收到新 final：不直接打断，先提交 feino 语义判定
    clock.Advance(2600); // 越过回复启动保护窗
    SendBargeFinal("新问题");
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "judge_interrupt"), 1u);
    EXPECT_EQ(s->m_uttGen, 0u);          // 判定回来前不打断
    EXPECT_EQ(s->m_playQueue.size(), 2u);

    // feino 判定打断 → 旧队列清空、代际+1、通知端侧、新语句流水线启动
    SendVerdict(true, "新问题");
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 1u);                          // 代际 +1
    EXPECT_EQ(s->m_state, SessionState::kThinking);      // 新语句思考中
    EXPECT_EQ(s->m_playQueue.size(), 2u);                // 新语句的 B/C
    bool notified = false;
    for (const auto& b : cs2.board_writes)
        if (b.field == "interrupted") notified = true;
    EXPECT_TRUE(notified);                               // 通知端侧丢缓冲
    EXPECT_EQ(CountGrpc(cs2, "llm", "restate"), 1u);     // 新语句流水线
    EXPECT_EQ(CountGrpc(cs2, "llm", "answer"), 1u);
    for (const auto& g : cs2.grpc_calls) EXPECT_EQ(g.aux, 1); // 新代际
    // 历史上下文保留并累积新语句（结合历史响应新语音）
    EXPECT_NE(s->m_chatCtx.find("U:问题"), std::string::npos);
    EXPECT_NE(s->m_chatCtx.find("U:新问题"), std::string::npos);
}

TEST_F(Fixture, BargeInDeniedByFeinoKeepsReply) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_playQueue.size(), 2u);

    // 播放中收到噪声 final（feino 判定不打断）→ 回复不受任何影响
    clock.Advance(2600); // 越过回复启动保护窗
    SendBargeFinal("证证证证证证");
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "judge_interrupt"), 1u);
    SendVerdict(false, "证证证证证证");
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 0u);
    EXPECT_EQ(s->m_playQueue.size(), 2u);
    EXPECT_EQ(s->m_state, SessionState::kThinking);
    EXPECT_TRUE(cs2.grpc_calls.empty());               // 不发起新流水线
    EXPECT_EQ(s->m_chatCtx.find("证证"), std::string::npos); // 噪声不入上下文
}

// 核心回归（真机 final 后 50ms 噪声秒杀回复）：final 复位连续有声计数
// 与计时——用户自己的语音不得计入"打断自己回复"的判定
TEST_F(Fixture, NoiseRightAfterFinalDoesNotCancelReply) {
    ConnectAndCalibrate();
    // 用户说话：持续有声，voiceRun 累积 ≥10
    for (int i = 0; i < 12; ++i) {
        clock.Advance(50);
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_state, SessionState::kThinking);
    ASSERT_EQ(s->m_playQueue.size(), 2u);

    // final 后 50ms 单帧噪声：voiceRun 已复位（=1<10 且持续 0ms<400ms），
    // 且在回复保护窗内 → 回复不得被秒杀
    clock.Advance(50);
    auto n = Msg(MsgType::kVadUpdate, clock.NowMs());
    n.flags = msgflag::kVoice;
    engine.Inject(std::move(n));
    engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 0u);
    EXPECT_EQ(s->m_playQueue.size(), 2u);
    EXPECT_EQ(s->m_state, SessionState::kThinking);
}

// 能量/VAD 帧打断已移除（真机噪声/回声误杀回复的根因）：无论保护窗内外、
// 持续多久的有声帧都不得取消回复；打断只走 final → feino 语义判定
TEST_F(Fixture, VadFramesNeverInterrupt) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);

    clock.Advance(2100); // 越过原回复保护窗
    for (int i = 0; i < 30; ++i) { // 30 帧 × 50ms = 1.5s 持续有声
        clock.Advance(50);
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 0u);
    EXPECT_EQ(s->m_playQueue.size(), 2u);
}

// 回复开播后持续有声帧同样不得打断（回声场景：自己的 TTS 被mic收回）
TEST_F(Fixture, VadFramesDuringPlaybackNeverInterrupt) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    // A 段文本+音频就绪 → EvolvePlayback 开播（m_replyStartedMs 设置）
    auto t = Msg(MsgType::kLlmQuickResp, clock.NowMs());
    t.clip_id = clip::kSoothe; t.text = "嗯"; t.aux = 0;
    engine.Inject(std::move(t));
    auto a = Msg(MsgType::kTtsAudioChunk, clock.NowMs());
    a.clip_id = clip::kSoothe; a.payload = {1, 2, 3}; a.aux = 0;
    engine.Inject(std::move(a));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_GT(s->m_replyStartedMs, 0);

    for (int i = 0; i < 30; ++i) {
        clock.Advance(50);
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 0u);
    EXPECT_FALSE(s->m_playQueue.empty());
}

TEST_F(Fixture, StaleGenResultsDroppedAfterBargeIn) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();

    // 打断：代际 0 → 1
    SemanticBarge();
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_uttGen, 1u);

    // 旧代际(aux=0)的 LLM 文本/TTS 音频迟到 → 丢弃
    auto rb = Msg(MsgType::kLlmRestate, clock.NowMs()); // aux=0
    rb.text = "旧复述";
    engine.Inject(std::move(rb));
    auto ta = Msg(MsgType::kTtsAudioChunk, clock.NowMs()); // aux=0
    ta.clip_id = clip::kSoothe;
    ta.payload = {9, 9};
    engine.Inject(std::move(ta));
    const auto cs = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs, "tts", "synth"), 0u); // 不为旧文本发起 TTS
    EXPECT_EQ(s->m_playQueue.size(), 2u);         // 旧音频不入队（仅新语句 B/C）
    EXPECT_TRUE(cs.ws_sends.empty());             // 不下发任何音频
}

TEST_F(Fixture, NewUtteranceAfterBargeInRunsPipeline) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题一";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    SemanticBarge("问题二"); // 打断 + 新语句
    const auto cs2 = engine.RunOnce();

    // 打断即开启新语句流水线：复述+答案携带新代际 gen=1
    EXPECT_EQ(CountGrpc(cs2, "llm", "restate"), 1u);
    EXPECT_EQ(CountGrpc(cs2, "llm", "answer"), 1u);
    for (const auto& g : cs2.grpc_calls) EXPECT_EQ(g.aux, 1); // 新代际
    // A 段安抚由补偿的断句标志触发（与倾听态 final 流程对齐；
    // 可能在判定心跳或下一心跳演进）
    const auto cs1 = engine.RunOnce();
    const size_t quick = CountGrpc(cs2, "llm", "quick") + CountGrpc(cs1, "llm", "quick");
    EXPECT_EQ(quick, 1u);
    // 新代际(aux=1)结果正常接收
    auto rb = Msg(MsgType::kLlmRestate, clock.NowMs());
    rb.text = "你问的是问题二";
    rb.aux = 1;
    engine.Inject(std::move(rb));
    const auto cs3 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs3, "tts", "synth"), 1u);
    // 历史包含两轮对话
    const auto* s = engine.Board().FindSession(kSid);
    EXPECT_NE(s->m_chatCtx.find("U:问题一"), std::string::npos);
    EXPECT_NE(s->m_chatCtx.find("U:问题二"), std::string::npos);
}

// 回声抑制：final 与本机最近播报文本相似 → 丢弃（AEC 未校准防自我对话）
TEST_F(Fixture, EchoFinalDropped) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "今天天气怎么样";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    // B 段文本就绪（播报内容进入 recentSpoken）
    auto rb = Msg(MsgType::kLlmRestate, clock.NowMs());
    rb.text = "你问的是今天天气怎么样"; rb.aux = 0;
    engine.Inject(std::move(rb));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_playQueue.size(), 2u);

    // 回声 final（播报内容的片段）→ 丢弃，不打断、不进判定
    auto echo = Msg(MsgType::kAsrFinal, clock.NowMs());
    echo.text = "今天天气怎么样";
    engine.Inject(std::move(echo));
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "judge_interrupt"), 0u);
    EXPECT_EQ(s->m_uttGen, 0u);
    bool dropped = false;
    for (const auto& b : cs1.board_writes)
        if (b.field == "final_dropped") dropped = true;
    EXPECT_TRUE(dropped);

    // 纯应声碎片 → 同样丢弃
    auto interj = Msg(MsgType::kAsrFinal, clock.NowMs());
    interj.text = "oah";
    engine.Inject(std::move(interj));
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "llm", "judge_interrupt"), 0u);
    EXPECT_EQ(s->m_uttGen, 0u);

    // 真实新语句（与播报无关）→ 正常提交 feino 判定
    clock.Advance(2600); // 越过回复启动保护窗
    auto real = Msg(MsgType::kAsrFinal, clock.NowMs());
    real.text = "明天会下雨吗";
    engine.Inject(std::move(real));
    const auto cs3 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs3, "llm", "judge_interrupt"), 1u);
}

// 回复启动保护窗：同一句话被切成两个 final 时，后到的碎片不得打断
// 刚启动的回复流水线（真机"今天天气如何"+"今天天气"碎片杀正解的回归）
TEST_F(Fixture, FragmentFinalWithinProtectWindowDropped) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "今天天气如何";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_state, SessionState::kThinking);
    ASSERT_EQ(s->m_playQueue.size(), 2u);

    // 2.5s 保护窗内：碎片 final 直接丢弃（不提交 feino 判定、不打断）
    clock.Advance(700);
    SendBargeFinal("今天天气");
    const auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "judge_interrupt"), 0u);
    EXPECT_EQ(s->m_uttGen, 0u);
    EXPECT_EQ(s->m_playQueue.size(), 2u);
    bool dropped = false;
    for (const auto& b : cs1.board_writes)
        if (b.field == "final_dropped") dropped = true;
    EXPECT_TRUE(dropped);

    // 越过保护窗：真实新语句正常提交 feino 判定
    clock.Advance(2000);
    SendBargeFinal("明天会下雨吗");
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "llm", "judge_interrupt"), 1u);
}

TEST_F(Fixture, ClientCancelTriggersBargeIn) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_EQ(s->m_playQueue.size(), 2u);

    auto cancel = Msg(MsgType::kAudioCancel, clock.NowMs());
    engine.Inject(std::move(cancel));
    const auto cs = engine.RunOnce();
    EXPECT_TRUE(s->m_playQueue.empty());
    EXPECT_EQ(s->m_uttGen, 1u);
    bool notified = false;
    for (const auto& b : cs.board_writes)
        if (b.field == "interrupted") notified = true;
    EXPECT_TRUE(notified);

    // 空闲时取消：幂等无操作
    auto cancel2 = Msg(MsgType::kAudioCancel, clock.NowMs());
    engine.Inject(std::move(cancel2));
    engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 1u);
}

} // namespace
