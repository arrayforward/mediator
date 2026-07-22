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
//       convai_sim [--wav <16k_pcm16_mono.wav>] [--realtime|--burst] [port] [device_key]
//   --wav      真实语音上行模式：读 wav → 重采样到 8k → G.711A → 20ms/160B 帧上送，
//              断言放宽为「非空 text + 3 个非空 clip」，超时放宽到 60s；
//              无 --wav 时行为与原版完全一致（mock 后端 e2e 路径）。
//    pacing    --wav 模式默认 realtime（20ms/帧，真实 ASR 管线按实时节奏消费）；
//              --burst 尽快突发发送。
// ============================================================================
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
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

    ~Client() {
        // 强制解除后台读线程的阻塞读并汇合
        if (stream) {
            boost::system::error_code ec;
            beast::get_lowest_layer(*stream).socket().close(ec);
        }
        if (m_reader.joinable()) m_reader.join();
    }

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
            m_reader = std::thread([this] { ReaderLoop(); });
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

    // 带超时取一帧。注意：beast::tcp_stream 的 expires_after 只对异步操作生效，
    // 同步 read 会永远阻塞（旧实现依赖它，超时从未真正生效）。因此用后台线程
    // 做无超时同步读入队，此处以条件变量定时等待取帧，流完整性不受取消影响。
    bool ReadFrame(Frame& f, int timeout_ms) {
        std::unique_lock<std::mutex> lk(m_mq);
        if (!m_cv.wait_for(lk, milliseconds(timeout_ms),
                           [&] { return !m_q.empty() || m_read_err; }))
            return false;
        if (m_q.empty()) return false; // 读线程已出错退出
        f = std::move(m_q.front());
        m_q.pop_front();
        return true;
    }

    // 等待含某子串的文本帧（跳过其他帧，音频帧收集到 pending_audio）
    bool WaitText(const std::string& needle, int timeout_ms,
                  std::vector<Frame>* audio_sink = nullptr,
                  std::string* matched = nullptr) {
        const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
        Frame f;
        while (steady_clock::now() < deadline) {
            const int remain = static_cast<int>(
                duration_cast<milliseconds>(deadline - steady_clock::now()).count());
            if (remain <= 0 || !ReadFrame(f, remain)) return false;
            if (f.is_text) {
                if (f.text.find(needle) != std::string::npos) {
                    if (matched) *matched = f.text;
                    return true;
                }
            } else if (audio_sink) {
                audio_sink->push_back(std::move(f));
            }
        }
        return false;
    }

    void Close() {
        // 读线程常驻：ws 优雅关闭握手内部也要读，会与读线程并发读同一流
        // （beast 不允许）。直接关 TCP，服务端按断连清理会话即可。
        boost::system::error_code ec;
        beast::get_lowest_layer(*stream).socket().shutdown(tcp::socket::shutdown_both, ec);
        beast::get_lowest_layer(*stream).socket().close(ec);
    }

private:
    // 后台读线程：无超时同步读，帧入队；出错置 m_read_err
    void ReaderLoop() {
        for (;;) {
            beast::flat_buffer buf;
            boost::system::error_code ec;
            stream->read(buf, ec);
            if (ec) {
                { std::lock_guard<std::mutex> lk(m_mq); m_read_err = true; }
                m_cv.notify_all();
                return;
            }
            Frame fr;
            fr.is_text = stream->got_text();
            auto* p = static_cast<const uint8_t*>(buf.data().data());
            if (fr.is_text) {
                fr.text.assign(reinterpret_cast<const char*>(p), buf.size());
            } else {
                if (!cv::ParseAudioHeader(p, buf.size(), fr.hdr)) continue;
                fr.payload.assign(p + cv::kAudioHeaderBytes, p + buf.size());
            }
            { std::lock_guard<std::mutex> lk(m_mq); m_q.push_back(std::move(fr)); }
            m_cv.notify_one();
        }
    }

    std::thread m_reader;
    std::mutex m_mq;
    std::condition_variable m_cv;
    std::deque<Frame> m_q;
    bool m_read_err = false;
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

// 类语音帧（8k，160B G.711A）：谐波堆叠 + 音节率调幅 + 少量噪声。
// 纯正弦会被 APM NS(kHigh) 当稳态音调噪声抑制且 VAD 判非语音；
// 打断测试需要 APM VAD(kModerateLikelihood) 判有声的信号。
std::vector<uint8_t> SpeechFrame8k(int seed) {
    std::vector<int16_t> pcm(160);
    for (int i = 0; i < 160; ++i) {
        const double t = (seed * 160 + i) / 8000.0;
        const double env = 0.6 + 0.4 * std::sin(2 * 3.14159265 * 5 * t); // 5Hz 音节包络
        double s = 0.45 * std::sin(2 * 3.14159265 * 140 * t)
                 + 0.25 * std::sin(2 * 3.14159265 * 280 * t)
                 + 0.15 * std::sin(2 * 3.14159265 * 700 * t)
                 + 0.10 * std::sin(2 * 3.14159265 * 1100 * t)
                 + 0.05 * std::sin(2 * 3.14159265 * 2377 * t); // 宽频扰动
        pcm[i] = static_cast<int16_t>(12000 * env * s);
    }
    return mediator::audio::EncodeALaw(pcm);
}

// ---- 真实语音模式（--wav）：wav 读取 + 线性插值重采样到 8k ----

uint32_t ReadU32LE(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t ReadU16LE(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

// 读 RIFF/WAVE（PCM16），取第一个声道为 mono；失败返回 false
bool LoadWavMono(const std::string& path, std::vector<int16_t>& pcm, int& sample_rate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::printf("wav open failed: %s\n", path.c_str()); return false; }
    const std::vector<uint8_t> buf{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
    if (buf.size() < 44 || std::memcmp(buf.data(), "RIFF", 4) ||
        std::memcmp(buf.data() + 8, "WAVE", 4)) {
        std::printf("not a RIFF/WAVE file: %s\n", path.c_str());
        return false;
    }
    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    for (size_t off = 12; off + 8 <= buf.size();) {
        const uint32_t csize = ReadU32LE(buf.data() + off + 4);
        if (off + 8 + csize > buf.size()) break;
        if (!std::memcmp(buf.data() + off, "fmt ", 4) && csize >= 16) {
            fmt_tag = ReadU16LE(buf.data() + off + 8);
            channels = ReadU16LE(buf.data() + off + 10);
            rate = ReadU32LE(buf.data() + off + 12);
            bits = ReadU16LE(buf.data() + off + 22);
        } else if (!std::memcmp(buf.data() + off, "data", 4)) {
            data = buf.data() + off + 8;
            data_len = csize;
        }
        off += 8 + csize + (csize & 1); // chunk 按 2 字节对齐
    }
    if (!data || fmt_tag != 1 || bits != 16 || channels < 1 || rate == 0) {
        std::printf("unsupported wav (need PCM16): %s fmt=%u ch=%u bits=%u rate=%u\n",
                    path.c_str(), fmt_tag, channels, bits, rate);
        return false;
    }
    const size_t frames = data_len / (2 * channels);
    pcm.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* s = data + i * 2 * channels; // 第一个声道
        pcm[i] = static_cast<int16_t>(uint16_t(s[0]) | (uint16_t(s[1]) << 8));
    }
    sample_rate = static_cast<int>(rate);
    return true;
}

// 线性插值重采样到 8k（16k→8k 等任意整比/非整比通用）
std::vector<int16_t> ResampleTo8k(const std::vector<int16_t>& in, int src_rate) {
    if (src_rate == 8000 || in.empty()) return in;
    std::vector<int16_t> out;
    const double step = src_rate / 8000.0;
    const size_t n = static_cast<size_t>((in.size() - 1) / step);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const double pos = i * step;
        const size_t i0 = static_cast<size_t>(pos);
        const double frac = pos - i0;
        const double a = in[i0];
        const double b = in[std::min(i0 + 1, in.size() - 1)];
        out.push_back(static_cast<int16_t>(a + (b - a) * frac));
    }
    return out;
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
    // 参数：选项（--wav/--realtime/--burst）可出现在任意位置；位置参数 [port] [device_key]，
    // 首个位置参数全为数字则视为端口，否则视为 device_key（端口默认 9443）。
    uint16_t port = 9443;
    std::string key = "goldie-dev-key-2026";
    std::string wav_path;
    bool realtime = true; // --wav 模式默认实时节奏（真实 ASR 管线按 20ms/帧消费）；
                          // mock 合成路径不使用本标志
    {
        std::vector<std::string> pos;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--wav" && i + 1 < argc) wav_path = argv[++i];
            else if (a == "--realtime") realtime = true;
            else if (a == "--burst") realtime = false;
            else pos.push_back(a);
        }
        if (pos.empty() && wav_path.empty()) {
            std::printf("usage: convai_sim <port> [device_key]\n"
                        "       convai_sim [--wav <16k_pcm16_mono.wav>] [--realtime|--burst] "
                        "[port] [device_key]\n");
            return 2;
        }
        size_t next = 0;
        if (next < pos.size() &&
            pos[next].find_first_not_of("0123456789") == std::string::npos)
            port = static_cast<uint16_t>(std::stoi(pos[next++]));
        if (next < pos.size()) key = pos[next++];
    }
    const bool real_mode = !wav_path.empty();

    // 真实语音模式：预读 wav → 重采样 8k → G.711A
    std::vector<uint8_t> wav_g711;
    if (real_mode) {
        std::vector<int16_t> pcm;
        int rate = 0;
        if (!LoadWavMono(wav_path, pcm, rate)) return 2;
        const auto pcm8k = ResampleTo8k(pcm, rate);
        wav_g711 = mediator::audio::EncodeALaw(pcm8k);
        std::printf("real-voice mode: %s %dHz %zu samples -> 8k %zu samples, "
                    "%zu g711 bytes (%zu frames), pacing=%s\n",
                    wav_path.c_str(), rate, pcm.size(), pcm8k.size(), wav_g711.size(),
                    wav_g711.size() / 160, realtime ? "realtime" : "burst");
        if (wav_g711.size() < 160) { std::printf("wav too short\n"); return 2; }
    }

    try {
        Client c;
        if (!c.Connect(port)) return 1;

        // ---- 1. hello（设备 Key 鉴权）----
        // 真实模式用唯一 agent_id：真实后端按 uid 维护会话，重复 uid 会撞上
        // 上一次会话尚未清理完的管线状态（ watermark/ASR 静默）。
        const std::string agent_id =
            real_mode ? "sim-" + std::to_string(duration_cast<milliseconds>(
                            system_clock::now().time_since_epoch()).count() % 1000000)
                      : "sim-1";
        c.SendText("{\"type\":\"hello\",\"seq\":1,\"ts\":1700000000000,\"body\":{"
                   "\"product_id\":\"goldie\",\"product_key\":\"" + key +
                   "\",\"product_secret\":\"s\",\"device_name\":\"convai-sim\","
                   "\"agent_id\":\"" + agent_id + "\",\"audio_codec\":1,\"sample_rate\":8000}}");

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

        // ---- 3. 说话：10 帧语音 + End（真实模式：整段 wav 上送）----
        const auto t_voice = steady_clock::now();
        if (real_mode) {
            for (size_t off = 0; off < wav_g711.size(); off += 160) {
                const size_t n = std::min<size_t>(160, wav_g711.size() - off);
                c.SendAudio(cv::AudioOp::kFrame,
                            {wav_g711.begin() + off, wav_g711.begin() + off + n});
                if (realtime) std::this_thread::sleep_for(milliseconds(20));
            }
        } else {
            const auto voice = VoiceFrame8k();
            for (int i = 0; i < 10; ++i) {
                c.SendAudio(cv::AudioOp::kFrame, voice);
                std::this_thread::sleep_for(milliseconds(20));
            }
        }
        c.SendAudio(cv::AudioOp::kEnd, {});

        // ---- 4. status:thinking → text → 三个音频 clip ----
        // 真实模式：LLM 生成 + TTS 合成 8-15s 正常，超时放宽到 60s
        const int wait_ms = real_mode ? 60000 : 8000;
        std::vector<Client::Frame> audio;
        CHECK(c.WaitText("\"status\":\"thinking\"", wait_ms, &audio), "status thinking");
        std::string text_frame;
        CHECK(c.WaitText("\"type\":\"text\"", wait_ms, &audio, &text_frame), "text frame");
        if (real_mode && !text_frame.empty()) {
            // body.text 内容非空即过（真实 LLM 回复内容不固定）
            const auto tp = text_frame.find("\"text\":\"");
            const auto te = tp == std::string::npos
                                ? std::string::npos
                                : text_frame.find('"', tp + 8);
            const bool nonempty = tp != std::string::npos && te != std::string::npos && te > tp + 8;
            CHECK(nonempty, "real: text body non-empty");
            std::printf("       llm text: %.120s%s\n", text_frame.c_str(),
                        text_frame.size() > 120 ? "..." : "");
        }

        // 收集 clip（按 0x11/0x12 分组），收满 want 个完整 clip（含 End）
        struct Clip { std::vector<uint8_t> g711; bool done = false; };
        auto collect_clips = [&](size_t want, std::vector<Client::Frame> pre,
                                 int timeout_ms = 12000) {
            std::vector<Clip> clips;
            Clip* cur = nullptr;
            auto eat = [&](Client::Frame& fr) {
                if (fr.is_text) return;
                if (getenv("SIM_DEBUG"))
                    std::printf("       [dbg-eat] op=0x%02x payload=%zu\n",
                                static_cast<unsigned>(fr.hdr.op), fr.payload.size());
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
            for (auto& fr : pre) eat(fr);
            const auto deadline = steady_clock::now() + milliseconds(timeout_ms);
            while (steady_clock::now() < deadline && completed() < want) {
                // 用剩余总时长做读超时：真实 TTS 合成期 clip 间可能间隔数秒，
                // 不能用短的逐帧超时，否则会误判为收齐失败
                const int remain = static_cast<int>(
                    duration_cast<milliseconds>(deadline - steady_clock::now()).count());
                if (remain <= 0 || !c.ReadFrame(f, remain)) break;
                if (getenv("SIM_DEBUG") && !f.is_text)
                    std::printf("       [dbg] op=0x%02x seq=%u payload=%zu clips=%zu done=%zu\n",
                                static_cast<unsigned>(f.hdr.op), f.hdr.seq,
                                f.payload.size(), clips.size(), completed());
                if (getenv("SIM_DEBUG") && f.is_text)
                    std::printf("       [dbg] text: %.80s\n", f.text.c_str());
                eat(f);
            }
            return clips;
        };
        auto clips = collect_clips(3, std::move(audio), real_mode ? 60000 : 12000);
        CHECK(clips.size() >= 3, "three clips received (soothe/restate/answer)");
        if (clips.size() >= 3) {
            if (real_mode) {
                // 真实 TTS 内容不固定：三个 clip 均非空即过
                CHECK(!clips[0].g711.empty(), "real: clip1 soothe non-empty");
                CHECK(!clips[1].g711.empty(), "real: clip2 restate non-empty");
                CHECK(!clips[2].g711.empty(), "real: clip3 answer non-empty");
            } else {
                CHECK(IsRhythmicAudio(clips[0].g711), "clip1 soothe rhythmic (叮-咚)");
                CHECK(IsRhythmicAudio(clips[1].g711), "clip2 restate rhythmic (do-mi-sol)");
                CHECK(IsRhythmicAudio(clips[2].g711), "clip3 answer rhythmic (滴滴滴-长滴)");
            }
            std::printf("       clip bytes: %zu/%zu/%zu\n", clips[0].g711.size(),
                        clips[1].g711.size(), clips[2].g711.size());
            const auto ms = duration_cast<milliseconds>(steady_clock::now() - t_voice).count();
            std::printf("       voice-to-clips latency: %lld ms\n", static_cast<long long>(ms));
        }

        if (real_mode) {
            // 真实模式到此为止：barge-in/VAD 断句/config_update 依赖 mock 后端的
            // 固定测试音型与即时回复，真实后端不适用，直接 bye 收尾。
            c.SendText("{\"type\":\"bye\",\"seq\":101,\"ts\":1700000000003,\"body\":{}}");
            c.Close();
            std::printf("%s\n", g_failures ? "CONVAI SIM FAILED" : "CONVAI SIM PASSED");
            return g_failures ? 1 : 0;
        }

        // ---- 4B. 打断：AI 播放期（引擎处 Thinking）用户说话 → interrupted
        //          → 服务端 VAD 断句/End → 第二轮三段式（历史上下文保留）----
        for (int i = 0; i < 50; ++i) { // 1s 类语音：VAD 适应后进入稳定有声段（探针实测）
            c.SendAudio(cv::AudioOp::kFrame, SpeechFrame8k(i));
            std::this_thread::sleep_for(milliseconds(20));
        }
        CHECK(c.WaitText("\"status\":\"interrupted\"", 5000), "barge-in: status interrupted");
        CHECK(c.WaitText("\"status\":\"listening\"", 3000), "barge-in: back to listening");
        c.SendAudio(cv::AudioOp::kEnd, {});
        std::vector<Client::Frame> audio2;
        CHECK(c.WaitText("\"status\":\"thinking\"", 8000, &audio2),
              "barge-in: new utterance thinking");
        auto clips2 = collect_clips(3, std::move(audio2));
        CHECK(clips2.size() >= 3, "barge-in: second round three clips");
        if (clips2.size() >= 3)
            CHECK(IsRhythmicAudio(clips2[2].g711), "barge-in: new answer rhythmic");

        // ---- 4C. 端侧主动取消（AudioOp 0x13）：幂等，连接保持 ----
        c.SendAudio(cv::AudioOp::kCancel, {});
        std::this_thread::sleep_for(milliseconds(300));

        // ---- 4D. 服务端 VAD 断句：不发 End，说话后持续静音帧 →
        //          600ms 迟滞 → 自动 final → 第三轮三段式（真实 App 路径）----
        for (int i = 0; i < 40; ++i) {
            c.SendAudio(cv::AudioOp::kFrame, SpeechFrame8k(100 + i));
            std::this_thread::sleep_for(milliseconds(20));
        }
        static const std::vector<uint8_t> kSilence =
            mediator::audio::EncodeALaw(std::vector<int16_t>(160, 0));
        for (int i = 0; i < 50; ++i) { // 1s 静音帧 > 600ms 断句迟滞
            c.SendAudio(cv::AudioOp::kFrame, kSilence);
            std::this_thread::sleep_for(milliseconds(20));
        }
        std::vector<Client::Frame> audio3;
        CHECK(c.WaitText("\"status\":\"thinking\"", 10000, &audio3),
              "server vad endpoint: thinking (no End op)");
        auto clips3 = collect_clips(3, std::move(audio3));
        CHECK(clips3.size() >= 3, "server vad endpoint: third round three clips");

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
