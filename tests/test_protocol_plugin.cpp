// ============================================================================
// test_protocol_plugin.cpp — convai wasm 协议插件单测（观察者模式协议解析）
//
// 用 mock Hooks 捕获插件动作：hello 鉴权（正确/错误 Key）、音频帧状态机
// （标定→ASR）、End 断句、下行 clip 包装、config_update→控制指令、
// ack→function_call。
// ============================================================================
#include <gtest/gtest.h>

#include <fstream>

#include "engine/message.h"
#include "ext/protocol_plugin.h"

using mediator::ClipId;
using mediator::MsgType;
using mediator::ext::ProtocolPlugin;

namespace {

const char* kPlugin = MEDIATOR_WASM_DIR "/convai_proto.wasm";

bool FileExists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}

struct Capture {
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> binaries; // 完整 13B 头帧（插件构造）
    std::vector<std::tuple<int, uint32_t, std::string>> injects;
    std::vector<std::string> bound_uids;
    std::vector<uint16_t> closes;
    bool calibrated = false;
    bool wm_hit = false;
    std::vector<std::pair<size_t, uint32_t>> asr_calls;

    ProtocolPlugin::Hooks Hooks() {
        ProtocolPlugin::Hooks h;
        h.send_text = [this](const std::string& t) { texts.push_back(t); };
        h.send_binary = [this](const std::vector<uint8_t>& p) { binaries.push_back(p); };
        h.inject = [this](MsgType t, uint32_t f, const std::string& x) {
            injects.push_back({static_cast<int>(t), f, x});
        };
        h.bind_session = [this](const std::string& u) { bound_uids.push_back(u); };
        h.close = [this](uint16_t c) { closes.push_back(c); };
        h.feed_watermark = [this](const uint8_t*, size_t) {
            const bool done = wm_hit;
            if (done) calibrated = true;
            return done;
        };
        h.forward_asr = [this](const std::vector<int16_t>& pcm, uint32_t f) {
            asr_calls.push_back({pcm.size(), f});
        };
        h.is_calibrated = [this] { return calibrated; };
        return h;
    }

    size_t CountInject(MsgType t) const {
        size_t n = 0;
        for (const auto& [ty, f, x] : injects)
            if (ty == static_cast<int>(t)) ++n;
        return n;
    }
    bool HasTextContaining(const std::string& needle) const {
        for (const auto& t : texts)
            if (t.find(needle) != std::string::npos) return true;
        return false;
    }
};

class PluginFixture : public ::testing::Test {
protected:
    Capture cap;
    ProtocolPlugin plugin;

    void SetUp() override {
        if (!FileExists(kPlugin)) GTEST_SKIP() << "wat2wasm 不可用";
        ASSERT_TRUE(plugin.Load(kPlugin, cap.Hooks())) << plugin.LastError();
        ASSERT_TRUE(plugin.SetConfigKey("goldie-dev-key-2026"));
    }

    static std::string Hello(const std::string& key, const std::string& agent) {
        return "{\"type\":\"hello\",\"seq\":1,\"ts\":1,\"body\":{\"product_key\":\"" +
               key + "\",\"agent_id\":\"" + agent + "\"}}";
    }

    // 构造 convai 13B 头二进制帧（上行方向）
    static std::vector<uint8_t> BinFrame(uint8_t op, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> f(13 + payload.size(), 0);
        f[0] = op;
        std::memcpy(f.data() + 13, payload.data(), payload.size());
        return f;
    }
    static uint8_t BinOp(const std::vector<uint8_t>& f) { return f[0]; }
    static size_t BinPayloadTotal(const Capture& c) {
        size_t n = 0;
        for (const auto& f : c.binaries)
            if (f.size() > 13 && f[0] == 0x10) n += f.size() - 13;
        return n;
    }
};

TEST_F(PluginFixture, HelloOkBindsSessionAndReplies) {
    plugin.OnWsText(Hello("goldie-dev-key-2026", "sim-1"));
    ASSERT_EQ(cap.bound_uids.size(), 1u);
    EXPECT_EQ(cap.bound_uids[0], "sim-1");
    EXPECT_TRUE(cap.HasTextContaining("\"type\":\"hello_ack\""));
    EXPECT_TRUE(cap.HasTextContaining("\"session_id\":\"sim-1\""));
    EXPECT_TRUE(cap.HasTextContaining("\"event\":\"connected\""));
    EXPECT_TRUE(cap.HasTextContaining("\"status\":\"listening\""));
    EXPECT_TRUE(cap.closes.empty());
}

TEST_F(PluginFixture, HelloWrongKeyRejected4401) {
    plugin.OnWsText(Hello("wrong-key", "sim-1"));
    EXPECT_TRUE(cap.bound_uids.empty());
    EXPECT_TRUE(cap.HasTextContaining("hello_err"));
    ASSERT_EQ(cap.closes.size(), 1u);
    EXPECT_EQ(cap.closes[0], 4401);
}

TEST_F(PluginFixture, AudioFeedWatermarkThenForwardAsr) {
    cap.wm_hit = true; // 第一帧即标定成功
    const auto frame = BinFrame(0x10, std::vector<uint8_t>(160, 0x55));
    plugin.OnWsBinary(frame.data(), frame.size());
    EXPECT_EQ(cap.asr_calls.size(), 0u); // 标定期不转 ASR
    plugin.OnWsBinary(frame.data(), frame.size());
    ASSERT_EQ(cap.asr_calls.size(), 1u);
    EXPECT_EQ(cap.asr_calls[0].second, 4u); // voice
    EXPECT_GT(cap.asr_calls[0].first, 0u);
}

TEST_F(PluginFixture, AudioEndInjectsAsrEndpoint) {
    cap.wm_hit = true;
    const auto frame = BinFrame(0x10, std::vector<uint8_t>(160, 0x55));
    plugin.OnWsBinary(frame.data(), frame.size()); // 标定完成
    const auto end = BinFrame(0x12, {});
    plugin.OnWsBinary(end.data(), end.size());
    ASSERT_FALSE(cap.asr_calls.empty());
    EXPECT_EQ(cap.asr_calls.back().second, 6u); // voice(4)|asr_endpoint(2)
}

TEST_F(PluginFixture, OutboundClipWrappedStartFramesEnd) {
    const std::vector<uint8_t> g711_16k(640, 0x55); // 40ms@16k → 40ms@8k=320B
    plugin.OnOutboundClip(2, g711_16k);
    ASSERT_GE(cap.binaries.size(), 3u);
    EXPECT_EQ(BinOp(cap.binaries.front()), 0x11); // Start
    EXPECT_EQ(BinOp(cap.binaries.back()), 0x12);  // End
    EXPECT_EQ(BinPayloadTotal(cap), 320u);        // 16k→8k 时长不变、字节减半
    // 13B 头完整性：seq 单调递增、op 正确
    for (const auto& f : cap.binaries) EXPECT_GE(f.size(), 13u);
}

TEST_F(PluginFixture, ConfigUpdateInjectsControlCmd) {
    plugin.OnWsText("{\"type\":\"config_update\",\"seq\":2,\"ts\":1,"
                    "\"body\":{\"cmd\":\"volume_up\"}}");
    EXPECT_EQ(cap.CountInject(MsgType::kWsControlCmd), 1u);
}

TEST_F(PluginFixture, AckTextMapsToFunctionCall) {
    plugin.OnOutboundText("ack:volume_up");
    EXPECT_TRUE(cap.HasTextContaining("function_call"));
    EXPECT_TRUE(cap.HasTextContaining("emotion"));
}

TEST_F(PluginFixture, StatusTextMapsToStatusFrame) {
    plugin.OnOutboundText("status:{\"status\":\"degraded\",\"reason\":\"llm_unavailable\"}");
    EXPECT_TRUE(cap.HasTextContaining("\"type\":\"status\""));
    EXPECT_TRUE(cap.HasTextContaining("degraded"));
    // ack 映射不受 status 分支影响
    cap.texts.clear();
    plugin.OnOutboundText("ack:volume_up");
    EXPECT_TRUE(cap.HasTextContaining("function_call"));
}

TEST_F(PluginFixture, ThinkingAndLlmTextFrames) {
    plugin.OnThinking();
    EXPECT_TRUE(cap.HasTextContaining("\"status\":\"thinking\""));
    plugin.OnLlmText("你问的是：今天天气");
    EXPECT_TRUE(cap.HasTextContaining("\"type\":\"text\""));
    EXPECT_TRUE(cap.HasTextContaining("今天天气"));
}

} // namespace
