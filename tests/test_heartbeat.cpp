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

    // 建立连接并完成水印标定（多数用例的前置）
    void ConnectAndCalibrate() {
        auto c = Msg(MsgType::kWsConnected, clock.NowMs());
        c.text = kUid;
        c.aux = 1; // gen=1
        engine.Inject(std::move(c));
        auto cs = engine.RunOnce();
        // 连接即下发水印
        EXPECT_FALSE(cs.ws_sends.empty());
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

    static size_t CountGrpc(const ChangeSet& cs, const std::string& svc,
                            const std::string& method) {
        size_t n = 0;
        for (const auto& g : cs.grpc_calls)
            if (g.service == svc && g.method == method) ++n;
        return n;
    }
};

// ---- 连接与水印标定 ----

TEST_F(Fixture, ConnectSendsWatermarkAndDropsFrames) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid;
    c.aux = 1;
    engine.Inject(std::move(c));
    const auto cs = engine.RunOnce();
    ASSERT_EQ(cs.ws_sends.size(), 1u); // 水印下行

    Voice(clock.NowMs()); // 标定期语音帧
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->m_droppedFramesWm, 1u);  // 直接丢弃，不识别
    EXPECT_FALSE(s->m_uttActive);
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
    EXPECT_TRUE(s->m_wmPending);               // 重新进入标定期
    ASSERT_EQ(cs.ws_sends.size(), 1u);         // 重发水印
    EXPECT_EQ(cs.ws_sends[0].clip_id, clip::kWatermark);
}

// 标定中途断连：m_wmPending 不得锁存——重连必须重新下发水印
// （修复前：wmPending 卡 true，后续所有连接不再发水印，AEC 永久旁路）
TEST_F(Fixture, DisconnectDuringCalibThenReconnectResendsWatermark) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid; c.aux = 1;
    engine.Inject(std::move(c));
    engine.RunOnce(); // 下发水印，进入标定期（未完成标定）

    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
    d.aux = 1;
    engine.Inject(std::move(d));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->m_wmPending); // 断连复位锁存状态

    auto c2 = Msg(MsgType::kWsConnected, clock.NowMs());
    c2.text = kUid; c2.aux = 2;
    engine.Inject(std::move(c2));
    const auto cs = engine.RunOnce();
    ASSERT_EQ(cs.ws_sends.size(), 1u); // 重连重新下发水印
    EXPECT_EQ(cs.ws_sends[0].clip_id, clip::kWatermark);
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
    EXPECT_TRUE(s->m_wmPending);
    EXPECT_EQ(s->m_connGeneration, 2u);
}

// ---- 标定超时旁路（kAecBypass：解锁上行丢帧门）----

// 超时 bypass 后上行帧不再被丢：m_wmPending 复位、valid 保持 false（AEC 直通）
TEST_F(Fixture, AecBypassUnlocksUplinkDropGate) {
    auto c = Msg(MsgType::kWsConnected, clock.NowMs());
    c.text = kUid; c.aux = 1;
    engine.Inject(std::move(c));
    engine.RunOnce(); // 下发水印，进入标定期
    Voice(clock.NowMs());
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->m_droppedFramesWm, 1u); // 标定期丢帧中

    auto b = Msg(MsgType::kAecBypass, clock.NowMs());
    b.aux = 1; // ws_server 超时注入（aux=连接 gen）
    engine.Inject(std::move(b));
    engine.RunOnce();
    EXPECT_FALSE(s->m_wmPending);      // 丢帧门关闭
    EXPECT_FALSE(s->m_aecCalib.valid); // AEC 直通（非伪标定）

    Voice(clock.NowMs());
    engine.RunOnce();
    EXPECT_EQ(s->m_droppedFramesWm, 1u); // 不再丢帧
    EXPECT_TRUE(s->m_uttActive);         // 正常进入语句状态机
}

// 旧代际的迟到 bypass（重连后新连接正在标定）→ 不解锁
TEST_F(Fixture, StaleGenAecBypassIgnored) {
    ConnectAndCalibrate();
    auto c2 = Msg(MsgType::kWsConnected, clock.NowMs());
    c2.text = kUid; c2.aux = 2;
    engine.Inject(std::move(c2));
    engine.RunOnce(); // 新连接 gen=2，重新进入标定期
    const auto* s = engine.Board().FindSession(kSid);
    ASSERT_TRUE(s->m_wmPending);

    auto b = Msg(MsgType::kAecBypass, clock.NowMs());
    b.aux = 1; // 旧 gen=1 连接的迟到超时消息
    engine.Inject(std::move(b));
    engine.RunOnce();
    EXPECT_TRUE(s->m_wmPending); // 新连接标定期不被打扰
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
    for (int i = 0; i < 10; ++i) { // 打断：uttGen 0→1
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
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

    // 播放中用户说话：连续有声帧（APM VAD）达阈值 → 打断
    ChangeSet cs;
    for (int i = 0; i < 10; ++i) {
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    cs = engine.RunOnce();
    EXPECT_TRUE(s->m_playQueue.empty());          // 播放队列清空
    EXPECT_EQ(s->m_uttGen, 1u);                   // 代际 +1
    EXPECT_EQ(s->m_state, SessionState::kListening); // 回到倾听
    EXPECT_TRUE(s->m_uttActive);
    bool notified = false;
    for (const auto& b : cs.board_writes)
        if (b.field == "interrupted") notified = true;
    EXPECT_TRUE(notified);                        // 通知端侧丢缓冲
    // 历史上下文保留（结合历史响应新语音）
    EXPECT_NE(s->m_chatCtx.find("U:问题"), std::string::npos);
}

TEST_F(Fixture, BargeInRequiresSustainedVoice) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    const auto* s = engine.Board().FindSession(kSid);

    // 9 帧（低于阈值 10）不触发；中间夹静音帧重置计数
    for (int i = 0; i < 9; ++i) {
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();
    EXPECT_EQ(s->m_playQueue.size(), 2u);
    EXPECT_EQ(s->m_uttGen, 0u);
    auto silence = Msg(MsgType::kVadUpdate, clock.NowMs()); // flags=0
    engine.Inject(std::move(silence));
    for (int i = 0; i < 9; ++i) {
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();
    EXPECT_EQ(s->m_uttGen, 0u); // 计数被静音重置，仍不触发
}

TEST_F(Fixture, StaleGenResultsDroppedAfterBargeIn) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题";
    engine.Inject(std::move(fin));
    engine.RunOnce();

    // 打断：代际 0 → 1
    for (int i = 0; i < 10; ++i) {
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
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
    EXPECT_TRUE(s->m_playQueue.empty());          // 旧音频不入队
    EXPECT_TRUE(cs.ws_sends.empty());             // 不下发任何音频
}

TEST_F(Fixture, NewUtteranceAfterBargeInRunsPipeline) {
    ConnectAndCalibrate();
    auto fin = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin.text = "问题一";
    engine.Inject(std::move(fin));
    engine.RunOnce();
    for (int i = 0; i < 10; ++i) { // 打断
        auto v = Msg(MsgType::kVadUpdate, clock.NowMs());
        v.flags = msgflag::kVoice;
        engine.Inject(std::move(v));
    }
    engine.RunOnce();

    // 新语句：VAD 断句 → 安抚；Final → 复述+答案（均携带新代际 gen=1）
    auto end = Msg(MsgType::kVadUpdate, clock.NowMs());
    end.flags = msgflag::kVadEnd | msgflag::kAsrEndpoint;
    engine.Inject(std::move(end));
    auto cs1 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs1, "llm", "quick"), 1u);
    auto fin2 = Msg(MsgType::kAsrFinal, clock.NowMs());
    fin2.text = "问题二";
    engine.Inject(std::move(fin2));
    const auto cs2 = engine.RunOnce();
    EXPECT_EQ(CountGrpc(cs2, "llm", "restate"), 1u);
    EXPECT_EQ(CountGrpc(cs2, "llm", "answer"), 1u);
    for (const auto& g : cs2.grpc_calls) EXPECT_EQ(g.aux, 1); // 新代际
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
