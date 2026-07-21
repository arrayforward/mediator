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

TEST_F(Fixture, ReconnectWithin3MinKeepsContextAndCalib) {
    ConnectAndCalibrate();
    auto d = Msg(MsgType::kWsDisconnected, clock.NowMs());
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
    EXPECT_TRUE(s->m_aecCalib.valid);   // 标定保留，免重标
    EXPECT_TRUE(cs.ws_sends.empty());   // 不重发水印
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

} // namespace
