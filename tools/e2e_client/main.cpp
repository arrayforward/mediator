// ============================================================================
// e2e_client — 端到端模拟端侧（设计文档 §9.3 E2E）
//
// 验证全链路（WSS + mock 五合一 gRPC）：
//   1. WSS 连接 + token 认证（错误 token 必被拒）
//   2. 首帧下行 = 水印（clip_id=1）→ 回环播放（模拟端侧放音→录音）
//   3. 发送有声帧 + VAD 断句 → 触发安抚音频；发送 ASR 断句 → mock ASR
//      回 final → 触发复述+答案
//   4. 断言下行 clip 顺序：安抚(2) → 复述(3) → 答案(4)，且安抚首包 < 3s
//   5. 控制指令 → 文本 ack
//   6. 断线 3 分钟内重连：免重发水印，会话继续
// 退出码 0=全过；用法：e2e_client <port> <token> [--expect-reject]
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

#include "ext/hs256.h"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono;

namespace {

int g_failures = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("[PASS] %s\n", name);                              \
        } else {                                                           \
            std::printf("[FAIL] %s\n", name);                              \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

uint32_t ReadU32LE(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

using WsStream = ws::stream<beast::ssl_stream<beast::tcp_stream>>;

struct Client {
    asio::io_context ioc;
    asio::ssl::context ssl{asio::ssl::context::tls_client};
    std::unique_ptr<WsStream> stream;

    Client() { ssl.set_verify_mode(asio::ssl::verify_none); } // 自签证书

    bool Connect(uint16_t port) {
        try {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve("127.0.0.1", std::to_string(port));
            stream = std::make_unique<WsStream>(ioc, ssl);
            beast::get_lowest_layer(*stream).connect(results);
            stream->next_layer().handshake(asio::ssl::stream_base::client);
            stream->handshake("127.0.0.1", "/");
            return true;
        } catch (const std::exception& e) {
            std::printf("connect error: %s\n", e.what());
            return false;
        }
    }

    bool Auth(const std::string& token) {
        stream->text(true);
        stream->write(asio::buffer(token));
        beast::flat_buffer buf;
        stream->read(buf);
        const std::string resp = beast::buffers_to_string(buf.data());
        return resp.find("\"ok\":true") != std::string::npos;
    }

    // 读一帧（带超时，beast::tcp_stream expires）；返回 {is_text, clip_id, payload}
    struct Frame { bool is_text; uint32_t clip; std::vector<uint8_t> bytes; };
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
            f.clip = 0;
            f.bytes.assign(p, p + buf.size());
        } else {
            f.clip = ReadU32LE(p);
            f.bytes.assign(p + 4, p + buf.size());
        }
        return true;
    }

    void SendAudio(uint32_t flags, const std::vector<uint8_t>& g711) {
        std::vector<uint8_t> frame(4 + g711.size());
        WriteU32LE(frame.data(), flags);
        std::memcpy(frame.data() + 4, g711.data(), g711.size());
        stream->binary(true);
        stream->write(asio::buffer(frame));
    }

    void SendText(const std::string& t) {
        stream->text(true);
        stream->write(asio::buffer(t));
    }

    void Close() {
        boost::system::error_code ec;
        stream->close(ws::close_code::normal, ec);
    }
};

// 伪语音帧：440Hz 正弦 G.711A（320 采样 = 20ms）
std::vector<uint8_t> VoiceFrameG711() {
    static std::vector<uint8_t> cached;
    if (cached.empty()) {
        for (int i = 0; i < 320; ++i) {
            const double v = 8000 * std::sin(2 * 3.14159265 * 440 * i / 16000);
            // 内联 A-law（与网关一致）
            int16_t sample = static_cast<int16_t>(v);
            int16_t pcm = sample >> 3, mask;
            if (pcm >= 0) mask = 0xD5;
            else { mask = 0x55; pcm = -pcm - 1; }
            static const int16_t seg_end[8] = {0x1F,0x3F,0x7F,0xFF,0x1FF,0x3FF,0x7FF,0xFFF};
            int seg = 0;
            while (seg < 8 && pcm > seg_end[seg]) ++seg;
            uint8_t aval;
            if (seg >= 8) aval = 0x7F;
            else {
                aval = seg << 4;
                aval |= (seg < 2) ? ((pcm >> 1) & 0x0F) : ((pcm >> seg) & 0x0F);
            }
            cached.push_back(aval ^ mask);
        }
    }
    return cached;
}

// "@sign[:uid]"：客户端现场签一个真实 HS256 JWT（密钥 dev-secret，exp=now+1h）
std::string ResolveToken(const std::string& arg) {
    if (arg.rfind("@sign", 0) != 0) return arg;
    const std::string uid = arg.size() > 6 ? arg.substr(6) : "user-1";
    const auto now_s = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string payload = "{\"uid\":\"" + uid + "\",\"exp\":" +
                                std::to_string(now_s + 3600) + "}";
    static const char* kT =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto b64 = [&](const std::string& in) {
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i+1]) << 8) | uint8_t(in[i+2]);
            out += kT[(v>>18)&63]; out += kT[(v>>12)&63]; out += kT[(v>>6)&63]; out += kT[v&63];
        }
        if (i < in.size()) {
            uint32_t v = uint8_t(in[i]) << 16;
            out += kT[(v>>18)&63];
            if (i + 1 < in.size()) { v |= uint8_t(in[i+1]) << 8; out += kT[(v>>12)&63]; out += kT[(v>>6)&63]; }
            else out += kT[(v>>12)&63];
        }
        return out;
    };
    const std::string input = b64("{\"alg\":\"HS256\",\"typ\":\"JWT\"}") + "." + b64(payload);
    const auto mac = mediator::ext::HmacSha256("dev-secret", input);
    return input + "." + b64(std::string(reinterpret_cast<const char*>(mac.data()), 32));
}

} // namespace

static int RunClient(int argc, char** argv);

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // 无缓冲：管道下实时可见
    if (argc < 3) {
        std::printf("usage: e2e_client <port> <token|@sign[:uid]|debug:uid> [--expect-reject]\n");
        return 2;
    }
    try {
        return RunClient(argc, argv);
    } catch (const std::exception& e) {
        // 网关中途崩溃/断连等：打印并失败退出，不让异常穿透
        std::printf("[FAIL] client exception: %s\n", e.what());
        return 1;
    }
}

static int RunClient(int argc, char** argv) {
    const uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
    const std::string token = ResolveToken(argv[2]);
    const bool expect_reject = argc > 3 && std::string(argv[3]) == "--expect-reject";

    // ---- 场景0：错误 token 必被拒 ----
    if (expect_reject) {
        Client c;
        if (!c.Connect(port)) { std::printf("[FAIL] connect\n"); return 1; }
        CHECK(!c.Auth("definitely-wrong-token"), "reject wrong token");
        return g_failures ? 1 : 0;
    }

    Client c;
    if (!c.Connect(port)) return 1;
    CHECK(c.Auth(token), "auth ok");

    // ---- 1. 首帧下行 = 水印 ----
    Client::Frame wm;
    CHECK(c.ReadFrame(wm, 5000), "recv watermark frame");
    CHECK(!wm.is_text && wm.clip == 1, "first frame is watermark (clip=1)");
    CHECK(wm.bytes.size() > 4000, "watermark has payload");

    // ---- 2. 回环播放水印（模拟端侧放音→录音回采）----
    for (size_t off = 0; off < wm.bytes.size(); off += 320) {
        const size_t n = std::min<size_t>(320, wm.bytes.size() - off);
        c.SendAudio(0, {wm.bytes.begin() + off, wm.bytes.begin() + off + n});
    }
    std::this_thread::sleep_for(milliseconds(300)); // 等标定完成

    // ---- 3. 说话：有声帧 + VAD 断句（触发安抚音频）----
    const auto t_voice = steady_clock::now();
    const auto voice = VoiceFrameG711();
    for (int i = 0; i < 10; ++i) {
        c.SendAudio(4, voice); // kVoice
        std::this_thread::sleep_for(milliseconds(20));
    }
    c.SendAudio(4 | 1, voice); // kVoice | kVadEnd → quick 触发

    // 等安抚音频（clip=2）
    Client::Frame f;
    const auto t_soothe_deadline = t_voice + milliseconds(3000);
    bool got_soothe = false;
    while (steady_clock::now() < t_soothe_deadline) {
        if (!c.ReadFrame(f, 1000)) break;
        if (!f.is_text && f.clip == 2) { got_soothe = true; break; }
    }
    CHECK(got_soothe, "soothe audio (clip=2) within 3s");
    if (got_soothe) {
        const auto ms = duration_cast<milliseconds>(steady_clock::now() - t_voice).count();
        std::printf("       soothe first-bytes latency: %lld ms, %zu bytes\n",
                    static_cast<long long>(ms), f.bytes.size());
    }

    // ---- 4. ASR 断句 → final → 复述(3) → 答案(4) ----
    for (int i = 0; i < 3; ++i) c.SendAudio(4, voice);
    c.SendAudio(4 | 2, voice); // kVoice | kAsrEndpoint → mock ASR final

    bool got_restate = false, got_answer = false;
    const auto t_deadline = steady_clock::now() + milliseconds(15000);
    while (steady_clock::now() < t_deadline && !(got_restate && got_answer)) {
        if (!c.ReadFrame(f, 2000)) break;
        if (f.is_text) continue;
        if (f.clip == 3) got_restate = true;
        if (f.clip == 4) got_answer = true;
    }
    CHECK(got_restate, "restate audio (clip=3)");
    CHECK(got_answer, "answer audio (clip=4)");

    // ---- 5. 控制指令 → ack ----
    c.SendText("volume_up");
    bool got_ack = false;
    const auto t_ack = steady_clock::now() + milliseconds(3000);
    while (steady_clock::now() < t_ack) {
        if (!c.ReadFrame(f, 1000)) break;
        if (f.is_text && std::string(f.bytes.begin(), f.bytes.end()).find("ack:volume_up") != std::string::npos) {
            got_ack = true;
            break;
        }
    }
    CHECK(got_ack, "control ack received");

    // ---- 6. 断线重连（3 分钟内）：免水印、会话继续 ----
    c.Close();
    std::this_thread::sleep_for(milliseconds(500));

    Client c2;
    CHECK(c2.Connect(port), "reconnect");
    CHECK(c2.Auth(token), "re-auth ok");
    // 重连后不应再发水印：发控制指令验证会话活着
    c2.SendText("volume_down");
    bool got_ack2 = false, watermark_resent = false;
    const auto t2 = steady_clock::now() + milliseconds(3000);
    while (steady_clock::now() < t2) {
        if (!c2.ReadFrame(f, 1000)) break;
        if (!f.is_text && f.clip == 1) watermark_resent = true;
        if (f.is_text && std::string(f.bytes.begin(), f.bytes.end()).find("ack:volume_down") != std::string::npos) {
            got_ack2 = true;
            break;
        }
    }
    CHECK(!watermark_resent, "no watermark resent after reconnect (calib kept)");
    CHECK(got_ack2, "session alive after reconnect");
    c2.Close();

    std::printf("%s\n", g_failures ? "E2E FAILED" : "E2E PASSED");
    return g_failures ? 1 : 0;
}
