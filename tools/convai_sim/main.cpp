// ============================================================================
// convai_sim — convai.v1 协议联调模拟器（替代真机 Android 端）
//
// 按 GoldieSettingsAndroid docs/DEV_PLAN.md §2.5 消息流跑全链路断言：
//   1. WSS 握手（请求子协议 convai.v1）→ 发 hello（含设备 Key）
//   2. 收 hello_ack / event:connected / status:listening
//   3. 收下行水印声纹 clip（0x11+0x10×N+0x12）→ 回环播放（标定回声）
//   4. 发语音帧 ×10 + 0x12 End
//   5. 收 status:thinking → text → 三个音频 clip（安抚/复述/答案，
//      解码验证非静音有节奏 = 测试音型）
//   6. 发 config_update → 收 function_call → 1s 内回 function_call_output
//   7. bye
// 用法：convai_sim <port> [device_key]
// ============================================================================
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "audio/g711.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono;

// ---- convai.v1 客户端侧协议工具（测试工具自带，不属于网关核心代码）----
namespace cv {
constexpr const char* kSubProtocol = "convai.v1";
constexpr size_t kAudioHeaderBytes = 13;

enum class AudioOp : uint8_t { kFrame = 0x10, kStart = 0x11, kEnd = 0x12, kCancel = 0x13 };

struct AudioHeader { AudioOp op; uint32_t seq; uint64_t ts_ms; };

inline void WriteU32BE(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}
inline void WriteU64BE(uint8_t* p, uint64_t v) {
    WriteU32BE(p, static_cast<uint32_t>(v >> 32));
    WriteU32BE(p + 4, static_cast<uint32_t>(v));
}
inline uint32_t ReadU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline std::vector<uint8_t> BuildAudioFrame(AudioOp op, uint32_t seq, uint64_t ts,
                                            const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out(kAudioHeaderBytes + len);
    out[0] = static_cast<uint8_t>(op);
    WriteU32BE(out.data() + 1, seq);
    WriteU64BE(out.data() + 5, ts);
    if (len) std::memcpy(out.data() + kAudioHeaderBytes, payload, len);
    return out;
}

inline bool ParseAudioHeader(const uint8_t* data, size_t len, AudioHeader& h) {
    if (!data || len < kAudioHeaderBytes) return false;
    h.op = static_cast<AudioOp>(data[0]);
    h.seq = ReadU32BE(data + 1);
    h.ts_ms = (uint64_t(ReadU32BE(data + 5)) << 32) | ReadU32BE(data + 9);
    return true;
}
} // namespace cv

namespace {

int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { std::printf("[PASS] %s\n", name); }                       \
        else { std::printf("[FAIL] %s\n", name); ++g_failures; }              \
    } while (0)

using WsStream = ws::stream<beast::ssl_stream<beast::tcp_stream>>;

struct Client {
    asio::io_context ioc;
    asio::ssl::context ssl{asio::ssl::context::tls_client};
    std::unique_ptr<WsStream> stream;
    uint32_t tx_seq = 1;

    Client() { ssl.set_verify_mode(asio::ssl::verify_none); }

    bool Connect(uint16_t port) {
        try {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve("127.0.0.1", std::to_string(port));
            stream = std::make_unique<WsStream>(ioc, ssl);
            stream->set_option(ws::stream_base::decorator(
                [](ws::request_type& req) {
                    req.set("Sec-WebSocket-Protocol", cv::kSubProtocol);
                }));
            beast::get_lowest_layer(*stream).connect(results);
            stream->next_layer().handshake(asio::ssl::stream_base::client);
            stream->handshake("127.0.0.1", "/");
            return true;
        } catch (const std::exception& e) {
            std::printf("connect error: %s\n", e.what());
            return false;
        }
    }

    void SendText(const std::string& s) {
        stream->text(true);
        stream->write(asio::buffer(s));
    }

    void SendAudio(cv::AudioOp op, const std::vector<uint8_t>& payload) {
        const auto frame = cv::BuildAudioFrame(
            op, tx_seq++,
            static_cast<uint64_t>(duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count()),
            payload.data(), payload.size());
        stream->binary(true);
        stream->write(asio::buffer(frame));
    }

    struct Frame { bool is_text; std::string text; cv::AudioHeader hdr; std::vector<uint8_t> payload; };
    bool ReadFrame(Frame& f, int timeout_ms) {
        beast::get_lowest_layer(*stream).expires_after(milliseconds(timeout_ms));
        beast::flat_buffer buf;
        boost::system::error_code ec;
        stream->read(buf, ec);
        beast::get_lowest_layer(*stream).expires_never();
        if (ec) return false;
        f.is_text = stream->got_text();
        auto* p = static_cast<const uint8_t*>(buf.data().data());
        if (f.is_text) {
            f.text.assign(reinterpret_cast<const char*>(p), buf.size());
        } else {
            if (!cv::ParseAudioHeader(p, buf.size(), f.hdr)) return false;
            f.payload.assign(p + cv::kAudioHeaderBytes, p + buf.size());
        }
        return true;
    }

    // 等待含某子串的文本帧（跳过其他帧，音频帧收集到 pending_audio）
    bool WaitText(const std::string& needle, int timeout_ms,
                  std::vector<Frame>* audio_sink = nullptr) {
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        Frame f;
        while (steady_clock::now() < deadline) {
            const int remain = static_cast<int>(
                duration_cast<milliseconds>(deadline - steady_clock::now()).count());
            if (remain <= 0 || !ReadFrame(f, remain)) return false;
            if (f.is_text) {
                if (f.text.find(needle) != std::string::npos) return true;
            } else if (audio_sink) {
                audio_sink->push_back(std::move(f));
            }
        }
        return false;
    }

    void Close() {
        boost::system::error_code ec;
        stream->close(ws::close_code::normal, ec);
    }
};

// 8k 440Hz 语音帧（160B G.711A）
std::vector<uint8_t> VoiceFrame8k() {
    static std::vector<uint8_t> cached;
    if (cached.empty()) {
        std::vector<int16_t> pcm(160);
        for (int i = 0; i < 160; ++i)
            pcm[i] = static_cast<int16_t>(8000 * std::sin(2 * 3.14159265 * 440 * i / 8000));
        cached = mediator::audio::EncodeALaw(pcm);
    }
    return cached;
}

// 验证音频 clip 非静音且有节奏变化（测试音型，而非全零/直流）
bool IsRhythmicAudio(const std::vector<uint8_t>& g711) {
    if (g711.size() < 320) return false;
    const auto pcm = mediator::audio::DecodeALaw(g711);
    long long energy = 0;
    int sign_changes = 0;
    int16_t prev = 0;
    for (auto s : pcm) {
        energy += std::abs(s);
        if ((s > 0) != (prev > 0) && prev != 0) ++sign_changes;
        prev = s;
    }
    return energy / static_cast<long long>(pcm.size()) > 500 && sign_changes > 50;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: convai_sim <port> [device_key]\n");
        return 2;
    }
    const uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
    const std::string key = argc > 2 ? argv[2] : "goldie-dev-key-2026";

    try {
        Client c;
        if (!c.Connect(port)) return 1;

        // ---- 1. hello（设备 Key 鉴权）----
        c.SendText("{\"type\":\"hello\",\"seq\":1,\"ts\":1700000000000,\"body\":{"
                   "\"product_id\":\"goldie\",\"product_key\":\"" + key +
                   "\",\"product_secret\":\"s\",\"device_name\":\"convai-sim\","
                   "\"agent_id\":\"sim-1\",\"audio_codec\":1,\"sample_rate\":8000}}");

        Client::Frame f;
        CHECK(c.WaitText("\"type\":\"hello_ack\"", 5000), "hello_ack");
        CHECK(c.WaitText("\"event\":\"connected\"", 3000), "event connected");
        CHECK(c.WaitText("\"status\":\"listening\"", 3000), "status listening");

        // ---- 2. 水印声纹 clip：0x11 + 0x10×N + 0x12，回环播放 ----
        std::vector<uint8_t> wm;
        bool started = false, ended = false;
        const auto wm_deadline = steady_clock::now() + milliseconds(5000);
        while (steady_clock::now() < wm_deadline && !ended) {
            if (!c.ReadFrame(f, 1000)) break;
            if (f.is_text) continue;
            if (f.hdr.op == cv::AudioOp::kStart) started = true;
            if (started && f.hdr.op == cv::AudioOp::kFrame)
                wm.insert(wm.end(), f.payload.begin(), f.payload.end());
            if (f.hdr.op == cv::AudioOp::kEnd) ended = true;
        }
        CHECK(started && ended && wm.size() > 1000, "watermark clip wrapped (start/frames/end)");
        std::printf("       watermark voiceprint: %zu g711 bytes (双 chirp 声纹)\n", wm.size());

        // 回环：逐 160B 帧上送（标定回声路径）
        for (size_t off = 0; off < wm.size(); off += 160) {
            const size_t n = std::min<size_t>(160, wm.size() - off);
            c.SendAudio(cv::AudioOp::kFrame, {wm.begin() + off, wm.begin() + off + n});
        }
        std::this_thread::sleep_for(milliseconds(400)); // 等标定完成

        // ---- 3. 说话：10 帧语音 + End ----
        const auto voice = VoiceFrame8k();
        const auto t_voice = steady_clock::now();
        for (int i = 0; i < 10; ++i) {
            c.SendAudio(cv::AudioOp::kFrame, voice);
            std::this_thread::sleep_for(milliseconds(20));
        }
        c.SendAudio(cv::AudioOp::kEnd, {});

        // ---- 4. status:thinking → text → 三个音频 clip ----
        std::vector<Client::Frame> audio;
        CHECK(c.WaitText("\"status\":\"thinking\"", 8000, &audio), "status thinking");
        CHECK(c.WaitText("\"type\":\"text\"", 8000, &audio), "text frame");

        // 收集 clip（按 0x11/0x12 分组），需收满 3 个完整 clip（含 End）
        struct Clip { std::vector<uint8_t> g711; bool done = false; };
        std::vector<Clip> clips;
        {
            Clip* cur = nullptr;
            auto eat = [&](Client::Frame& fr) {
                if (fr.is_text) return;
                if (fr.hdr.op == cv::AudioOp::kStart) { clips.push_back({}); cur = &clips.back(); }
                if (cur && fr.hdr.op == cv::AudioOp::kFrame)
                    cur->g711.insert(cur->g711.end(), fr.payload.begin(), fr.payload.end());
                if (cur && fr.hdr.op == cv::AudioOp::kEnd) { cur->done = true; cur = nullptr; }
            };
            auto completed = [&] {
                size_t n = 0;
                for (const auto& cl : clips) n += cl.done ? 1 : 0;
                return n;
            };
            for (auto& fr : audio) eat(fr);
            const auto deadline = steady_clock::now() + milliseconds(12000);
            while (steady_clock::now() < deadline && completed() < 3) {
                if (!c.ReadFrame(f, 2000)) break;
                eat(f);
            }
        }
        CHECK(clips.size() >= 3, "three clips received (soothe/restate/answer)");
        if (clips.size() >= 3) {
            CHECK(IsRhythmicAudio(clips[0].g711), "clip1 soothe rhythmic (叮-咚)");
            CHECK(IsRhythmicAudio(clips[1].g711), "clip2 restate rhythmic (do-mi-sol)");
            CHECK(IsRhythmicAudio(clips[2].g711), "clip3 answer rhythmic (滴滴滴-长滴)");
            std::printf("       clip bytes: %zu/%zu/%zu\n", clips[0].g711.size(),
                        clips[1].g711.size(), clips[2].g711.size());
            const auto ms = duration_cast<milliseconds>(steady_clock::now() - t_voice).count();
            std::printf("       voice-to-clips latency: %lld ms\n", static_cast<long long>(ms));
        }

        // ---- 5. config_update → function_call → 1s 内回执 ----
        c.SendText("{\"type\":\"config_update\",\"seq\":99,\"ts\":1700000000001,"
                   "\"body\":{\"cmd\":\"volume_up\"}}");
        const auto t_fc = steady_clock::now();
        std::string fc_frame;
        {
            const auto deadline = t_fc + milliseconds(5000);
            bool got = false;
            while (steady_clock::now() < deadline && !got) {
                if (!c.ReadFrame(f, 2000)) break;
                if (f.is_text && f.text.find("\"function_call\"") != std::string::npos) {
                    fc_frame = f.text;
                    got = true;
                }
            }
            CHECK(got, "function_call received");
        }
        if (!fc_frame.empty()) {
            const auto latency = duration_cast<milliseconds>(steady_clock::now() - t_fc).count();
            CHECK(latency < 1000, "function_call latency < 1s");
            c.SendText("{\"type\":\"function_call_output\",\"seq\":100,\"ts\":1700000000002,"
                       "\"body\":{\"items\":[{\"type\":\"function_call_output\","
                       "\"call_id\":\"fc-1\",\"output\":\"{\\\"result\\\":\\\"success\\\"}\"}]}}");
            std::printf("       function_call_output replied in %lld ms\n",
                        static_cast<long long>(latency));
        }

        // ---- 6. bye ----
        c.SendText("{\"type\":\"bye\",\"seq\":101,\"ts\":1700000000003,\"body\":{}}");
        c.Close();
    } catch (const std::exception& e) {
        std::printf("[FAIL] sim exception: %s\n", e.what());
        return 1;
    }

    std::printf("%s\n", g_failures ? "CONVAI SIM FAILED" : "CONVAI SIM PASSED");
    return g_failures ? 1 : 0;
}
