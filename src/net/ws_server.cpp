// ============================================================================
// ws_server.cpp — WSS 接入服务器实现（同步 Beast API + 每连接线程）
//
// 读循环要点：
//   1. SSL 握手 → WS 握手 → 首帧认证（文本 token）
//   2. 上行二进制帧：解析 flags + G.711A → PCM
//      - 未标定：喂水印检测器，命中注入 kWmDetected
//      - 已标定且有声：on_audio 转发 ASR
//      - 所有帧都注入 kWsAudioFrame 维护引擎状态机（标定期引擎自行丢弃）
//   3. 上行文本帧：控制指令 → kWsControlCmd
//   4. 断连：注入 kWsDisconnected（引擎进入离线计时，3 分钟内可重连续聊）
// ============================================================================
#include "net/ws_server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <deque>

#include <cerrno>
#include <poll.h>

#include "audio/align.h"
#include "audio/despike.h"
#include "audio/g711.h"
#include "core/clock.h"
#include "core/log.h"
#include "engine/message.h"
#include "net/grpc_clients.h" // SidHex
#include "telemetry/telemetry.h"

namespace mediator::net {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

struct WsServer::Conn {
    // 读循环用 poll(2) 等“可读或 20ms 超时”轮转出站队列（beast blocking I/O
    // 不支持 expires_after，同步 read 一旦阻塞永不返回，队列会饿死）。
    // 注意：ssl_stream 不允许跨线程并发 sync read+write（OpenSSL 对象非线程安全，
    // 会在对端边发边收时把流搞坏，报 "unspecified system error"）——
    // 因此所有出站写入都排队，只由会话线程 DrainOutbound 落盘。
    using Stream = ws::stream<beast::ssl_stream<beast::tcp_stream>>;
    std::shared_ptr<Stream> stream;
    std::mutex wmtx; // 写操作串行化（会话线程 vs Stop()）
    struct OutFrame {
        bool is_text;
        std::vector<uint8_t> bytes;
    };
    std::mutex omtx; // 出站队列（任意线程 → 会话线程）
    std::deque<OutFrame> outq;
    std::string uid;
    SessionId sid{};
    uint64_t gen = 0;
    // 标定由内容锚定估计器完成（calib_estimator，下行语音本身做水印），
    // 接入层不再做特制水印检测——calibrated 恒 true 跳过本层标定路径
    bool calibrated = true;
    std::vector<int16_t> calib_buf;
    // 协议插件路径（协议解析由 wasm 插件完成，宿主零协议）
    bool use_plugin = false;
    std::unique_ptr<ext::ProtocolPlugin> plugin;
    std::vector<int16_t> calib_buf8k; // 8k 协议侧标定缓冲
    int64_t calib_start_ms = 0; // 首次喂标定缓冲的时间（超时兜底用）
    int calib_retries = 0;      // 已重试次数（真机首播常被 AGC 爬坡/通道预热吃掉脉冲）
    size_t calib_detect_len = 0; // 上次跑模板匹配时的缓冲长度（批量检测降 CPU）
    audio::WatermarkDetectResult last_det; // 最近一次检测结果（超时 WARN 诊断用）
    FILE* raw_dump = nullptr;   // 调试：apk 设备上行原始字节落盘（wasm 前）
};

namespace {
SteadyClock g_clock; // 接入层时间戳（引擎内部仍用注入时钟）

// 水印标定超时：超时未命中（无声学回放的设备，如模拟器）降级为 AEC 旁路，
// 否则标定缓冲无限增长、O(n·m) 检测拖死会话读线程（ping 无 pong 被客户端踢掉）。
// 每轮检测窗 3s（从水印下发时刻起算）：未识别则重发，最多 3 轮；
// 计时起点由 SendBinary 在水印 clip 下发时重置（连接后延迟首播的场景，
// 窗内一定覆盖水印播放 + 回环往返）
constexpr int64_t kWmCalibTimeoutMs = 3000;

// 标定超时重试上限：真机首播常被扬声器 AGC 爬坡/AudioTrack 预热吃掉前几
// 个脉冲（matched_slots=1），重发时音频通道已热，成功率显著提高；3 轮仍
// 失败才走 AEC 旁路
constexpr int kWmCalibMaxRetries = 2;

// 标定失败时落盘上行缓冲（8k PCM16），供离线分析真机上行内容
// （回声/静音/噪声/硬件 NS 判别）
void DumpCalibBuf(const std::string& uid, int round, const std::vector<int16_t>& buf) {
    if (buf.empty()) return;
    char path[256];
    std::snprintf(path, sizeof(path),
                  "/mnt/d/agent/wsl/creek-cluster/crash/calib_%s_r%d_%zu.pcm",
                  uid.c_str(), round, buf.size());
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(buf.data(), sizeof(int16_t), buf.size(), f);
        std::fclose(f);
    }
}

// 会话读循环 poll 轮转出站的等待时长（决定下行帧最坏额外时延）
constexpr auto kReadPollTimeout = std::chrono::milliseconds(20);

// 等“套接字可读或超时”：true=可读 / 出错需处理，false=超时。
// 出错（含对端 RST 的 POLLERR/POLLHUP）也返回 true，交由后续 blocking read 报错统一处理。
bool WaitReadable(tcp::socket::native_handle_type fd, int timeout_ms) {
    pollfd pfd{fd, POLLIN, 0};
    for (;;) {
        const int r = ::poll(&pfd, 1, timeout_ms);
        if (r == 0) return false;
        if (r > 0) return true;
        if (errno != EINTR) return true;
    }
}

uint32_t ReadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void WriteU32LE(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
} // namespace

WsServer::WsServer(uint16_t port, std::string cert_file, std::string key_file,
                   ext::AuthProvider* auth, WsCallbacks cb)
    : m_port(port), m_certFile(std::move(cert_file)), m_keyFile(std::move(key_file)),
      m_auth(auth), m_cb(std::move(cb)) {}

WsServer::~WsServer() { Stop(); }

void WsServer::Start() {
    m_running = true;
    m_acceptThread = std::thread([this] { AcceptLoop(); });
}

void WsServer::Stop() {
    m_running = false;
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& [k, c] : m_conns) {
        boost::system::error_code ec;
        c->stream->close(ws::close_code::normal, ec);
    }
    m_conns.clear();
    if (m_acceptThread.joinable()) m_acceptThread.join();
}

void WsServer::AcceptLoop() {
    try {
        asio::io_context ioc;
        auto acceptor = std::make_unique<tcp::acceptor>(
            ioc, tcp::endpoint(tcp::v4(), m_port));
        m_listenFd = acceptor->native_handle();

        asio::ssl::context ssl_ctx(asio::ssl::context::tls_server);
        ssl_ctx.use_certificate_chain_file(m_certFile);
        ssl_ctx.use_private_key_file(m_keyFile, asio::ssl::context::pem);

        MDT_INFO("wss server listening on port {}", m_port);
        while (m_running) {
            auto conn = std::make_shared<Conn>();
            beast::tcp_stream ts(ioc);
            boost::system::error_code ec;
            acceptor->accept(ts.socket(), ec);
            if (ec || !m_running) break;
            conn->stream = std::make_shared<Conn::Stream>(
                beast::ssl_stream<beast::tcp_stream>(std::move(ts), ssl_ctx));
            std::thread([this, conn] { SessionThread(conn); }).detach();
        }
    } catch (const std::exception& e) {
        MDT_ERROR("accept loop error: {}", e.what());
    }
}

// 出站统一排队（任意线程调用）；只由会话线程 DrainOutbound 实际写入，
// 避免跨线程并发 sync read+write 损坏 ssl_stream
void WsServer::QueueOutbound(const std::shared_ptr<Conn>& conn, bool is_text,
                             std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> lk(conn->omtx);
    conn->outq.push_back({is_text, std::move(bytes)});
}

void WsServer::DrainOutbound(const std::shared_ptr<Conn>& conn) {
    for (;;) {
        Conn::OutFrame f;
        {
            std::lock_guard<std::mutex> lk(conn->omtx);
            if (conn->outq.empty()) return;
            f = std::move(conn->outq.front());
            conn->outq.pop_front();
        }
        std::lock_guard<std::mutex> wlk(conn->wmtx);
        boost::system::error_code ec;
        if (f.is_text) conn->stream->text(true);
        else conn->stream->binary(true);
        conn->stream->write(asio::buffer(f.bytes), ec);
        if (ec) {
            MDT_DEBUG("ws outbound write failed: {}", ec.message());
            return;
        }
    }
}

void WsServer::SessionThread(std::shared_ptr<Conn> conn) {
    auto& stream = *conn->stream;
    try {
        // TLS 握手 → 读 HTTP 升级请求 → 按 Sec-WebSocket-Protocol 分流
        stream.next_layer().handshake(asio::ssl::stream_base::server);
        beast::flat_buffer hbuf;
        boost::beast::http::request<boost::beast::http::string_body> req;
        boost::beast::http::read(stream.next_layer(), hbuf, req);
        const std::string subproto{req[boost::beast::http::field::sec_websocket_protocol]};
        const auto route = m_protocolRoutes.find(subproto);
        if (route != m_protocolRoutes.end()) {
            conn->use_plugin = true;
            // 应答子协议（回显客户端请求的 subproto）
            stream.set_option(ws::stream_base::decorator(
                [subproto](ws::response_type& res) {
                    res.set(boost::beast::http::field::sec_websocket_protocol, subproto);
                }));
            stream.accept(req);
            PluginSessionLoop(conn, route->second);
        } else {
            stream.accept(req);
            JwtSessionLoop(conn);
        }
    } catch (const beast::system_error& e) {
        if (e.code() != ws::error::closed)
            MDT_DEBUG("ws session ended: {}", e.code().message());
    } catch (const std::exception& e) {
        MDT_DEBUG("ws session error: {}", e.what());
    }
    CleanupConn(conn);
}

void WsServer::JwtSessionLoop(std::shared_ptr<Conn> conn) {
    auto& stream = *conn->stream;
    try {
        // ---- 首帧认证 ----
        beast::flat_buffer buf;
        stream.read(buf);
        const std::string token = beast::buffers_to_string(buf.data());
        ext::AuthRequest areq{token, "", g_clock.NowMs()};
        const auto auth_res = m_auth->Verify(areq);
        if (!auth_res.allow) {
            telemetry::Registry::Instance()
                .MakeCounter("ws_auth_failures_total", "reason=\"" + auth_res.deny_reason + "\"",
                             "JWT/wasm auth failures")
                .Add();
            MDT_WARN("auth failed: {}", auth_res.deny_reason);
            stream.text(true);
            stream.write(asio::buffer(std::string("{\"ok\":false,\"reason\":\"") +
                                      auth_res.deny_reason + "\"}"));
            stream.close(ws::close_code::policy_error);
            return;
        }
        conn->uid = auth_res.uid;
        conn->sid = SessionIdFromUid(conn->uid);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            conn->gen = ++m_genByUid[conn->uid]; // 代际号：重连 +1
            m_conns[SidHex(conn->sid)] = conn;
        }
        stream.text(true);
        stream.write(asio::buffer(std::string("{\"ok\":true,\"uid\":\"") + conn->uid +
                                  "\",\"gen\":" + std::to_string(conn->gen) + "}"));
        MDT_INFO("session connected uid={} gen={}", conn->uid, conn->gen);
        telemetry::Registry::Instance()
            .MakeGauge("ws_connections_active", "", "active WSS connections")
            .Set(static_cast<double>(ConnectionCount()));

        Message c;
        c.type = MsgType::kWsConnected;
        c.session_id = conn->sid;
        c.ts_ms = g_clock.NowMs();
        c.text = conn->uid;
        c.aux = static_cast<int64_t>(conn->gen);
        m_cb.inject(std::move(c));

        // ---- 读循环（poll 等可读或 20ms 超时，轮转落盘出站队列）----
        const auto fd = beast::get_lowest_layer(stream).socket().native_handle();
        for (;;) {
            buf.consume(buf.size());
            if (!WaitReadable(fd, static_cast<int>(kReadPollTimeout.count()))) {
                DrainOutbound(conn);
                continue;
            }
            boost::system::error_code ec;
            stream.read(buf, ec);
            if (ec) {
                if (ec != ws::error::closed)
                    MDT_DEBUG("ws session ended: {}", ec.message());
                break;
            }
            DrainOutbound(conn);
            if (stream.got_text()) {
                Message m;
                m.type = MsgType::kWsControlCmd;
                m.session_id = conn->sid;
                m.ts_ms = g_clock.NowMs();
                m.aux = static_cast<int64_t>(conn->gen);
                m.text = beast::buffers_to_string(buf.data());
                m_cb.inject(std::move(m));
                continue;
            }
            // 二进制帧：[flags:u32][G.711A]
            auto data = static_cast<const uint8_t*>(buf.data().data());
            const size_t n = buf.size();
            if (n < 4) continue;
            const uint32_t flags = ReadU32LE(data);
            const std::vector<uint8_t> g711(data + 4, data + n);
            auto pcm = audio::DecodeALaw(g711);
            audio::Despike(pcm); // 真机 AGC/削波满幅脉冲（水印 NCC 毒化根因）

            MDT_DEBUG("ws frame uid={} flags={} g711_bytes={}", conn->uid, flags,
                      g711.size());
            // 水印标定（仅未标定会话）
            if (!conn->calibrated) {
                const int64_t now = g_clock.NowMs();
                if (conn->calib_start_ms == 0) conn->calib_start_ms = now;
                if (now - conn->calib_start_ms > kWmCalibTimeoutMs) {
                    if (conn->calib_retries < kWmCalibMaxRetries) {
                        ++conn->calib_retries;
                        conn->calib_start_ms = 0;
                        conn->calib_buf.clear();
                        conn->calib_buf.shrink_to_fit();
                        conn->calib_buf8k.clear();
                        conn->calib_buf8k.shrink_to_fit();
                        MDT_WARN("aec calib timeout uid={} -> retry {}/{} "
                                 "(ncc={:.3f} peaks={} matched_slots={})",
                                 conn->uid, conn->calib_retries, kWmCalibMaxRetries,
                                 conn->last_det.peak_ncc,
                                 conn->last_det.debug_peaks,
                                 conn->last_det.debug_matched_slots);
                        InjectWmRetry(conn); // 引擎重发水印 clip 再测一轮
                    } else {
                    conn->calibrated = true; // 超时兜底：AEC 旁路
                    conn->calib_buf.clear();
                    conn->calib_buf.shrink_to_fit();
                    MDT_WARN("aec calib timeout uid={} -> bypass aec "
                             "(ncc={:.3f} peaks={} matched_slots={})",
                             conn->uid, conn->last_det.peak_ncc,
                             conn->last_det.debug_peaks,
                             conn->last_det.debug_matched_slots);
                    InjectAecBypass(conn); // 解锁引擎上行丢帧门（AEC 直通）
                    }
                } else {
                conn->calib_buf.insert(conn->calib_buf.end(), pcm.begin(), pcm.end());
                // 批量检测：缓冲每增长 ~0.25s 跑一轮模板匹配（逐帧全量 NCC 太贵；
                // 阈值必须小于回环水印长度，否则短回环永远凑不够批量）
                if (conn->calib_buf.size() - conn->calib_detect_len >= 4000) {
                    conn->calib_detect_len = conn->calib_buf.size();
                    const auto det = audio::DetectWatermark(conn->calib_buf, m_wmCfg);
                    conn->last_det = det;
                    MDT_DEBUG("wm detect buf={} ncc={:.3f} detected={}",
                              conn->calib_buf.size(), det.peak_ncc, det.detected);
                    if (det.detected) {
                        conn->calibrated = true;
                        MDT_INFO("aec calibrated uid={} delay={} skew={:.6f}", conn->uid,
                                 det.p1, det.skew);
                        Message w;
                        w.type = MsgType::kWmDetected;
                        w.session_id = conn->sid;
                        w.ts_ms = g_clock.NowMs();
                        w.aux = det.p1;
                        w.dval = det.skew;
                        m_cb.inject(std::move(w));
                    }
                }
                }
            } else if ((flags & msgflag::kVoice) && m_cb.on_audio) {
                m_cb.on_audio(conn->sid, pcm, flags); // 转发 ASR
            }

            Message f;
            f.type = MsgType::kWsAudioFrame;
            f.session_id = conn->sid;
            f.ts_ms = g_clock.NowMs();
            f.flags = flags;
            f.aux = static_cast<int64_t>(conn->gen);
            m_cb.inject(std::move(f));
        }
    } catch (const beast::system_error& e) {
        if (e.code() != ws::error::closed)
            MDT_DEBUG("ws session ended: {}", e.code().message());
    } catch (const std::exception& e) {
        MDT_DEBUG("ws session error: {}", e.what());
    }
}

// ---- 协议插件会话循环：帧事件全部派发 wasm 插件（观察者模式，宿主零协议）----
void WsServer::PluginSessionLoop(std::shared_ptr<Conn> conn,
                               const std::string& plugin_path) {
    auto& stream = *conn->stream;
    conn->plugin = std::make_unique<ext::ProtocolPlugin>();

    ext::ProtocolPlugin::Hooks hooks;
    hooks.send_text = [this, conn](const std::string& text) {
        QueueOutbound(conn, true, {text.begin(), text.end()});
    };
    hooks.send_binary = [this, conn](const std::vector<uint8_t>& payload) {
        // 裸帧下发（协议头由插件自行构造）
        QueueOutbound(conn, false, payload);
    };
    hooks.inject = [this, conn](MsgType type, uint32_t flags, const std::string& text) {
        Message m;
        m.type = type;
        m.session_id = conn->sid;
        m.ts_ms = g_clock.NowMs();
        m.flags = flags;
        m.text = text;
        m.aux = static_cast<int64_t>(conn->gen);
        m_cb.inject(std::move(m));
    };
    hooks.bind_session = [this, conn](const std::string& uid) {
        conn->uid = uid;
        conn->sid = SessionIdFromUid(uid);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            conn->gen = ++m_genByUid[uid];
            m_conns[SidHex(conn->sid)] = conn;
        }
        MDT_INFO("plugin session connected uid={} gen={}", uid, conn->gen);
        telemetry::Registry::Instance()
            .MakeGauge("ws_connections_active", "", "active WSS connections")
            .Set(static_cast<double>(ConnectionCount()));
        Message c;
        c.type = MsgType::kWsConnected;
        c.session_id = conn->sid;
        c.ts_ms = g_clock.NowMs();
        c.text = uid;
        c.aux = static_cast<int64_t>(conn->gen);
        m_cb.inject(std::move(c));
    };
    hooks.close = [conn](uint16_t code) {
        boost::system::error_code ec;
        conn->stream->close(static_cast<ws::close_code>(code), ec);
    };
    hooks.feed_watermark = [this, conn](const uint8_t* data, size_t len) -> bool {
        if (conn->calibrated) return true;
        const int64_t now = g_clock.NowMs();
        if (conn->calib_start_ms == 0) conn->calib_start_ms = now;
        if (now - conn->calib_start_ms > kWmCalibTimeoutMs) {
            if (conn->calib_retries < kWmCalibMaxRetries) {
                ++conn->calib_retries;
                DumpCalibBuf(conn->uid, conn->calib_retries, conn->calib_buf8k);
                conn->calib_start_ms = 0;
                conn->calib_buf.clear();
                conn->calib_buf.shrink_to_fit();
                conn->calib_buf8k.clear();
                conn->calib_buf8k.shrink_to_fit();
                MDT_WARN("aec calib timeout uid={} -> retry {}/{} "
                         "(ncc={:.3f} peaks={} matched_slots={})",
                         conn->uid, conn->calib_retries, kWmCalibMaxRetries,
                         conn->last_det.peak_ncc,
                         conn->last_det.debug_peaks,
                         conn->last_det.debug_matched_slots);
                InjectWmRetry(conn); // 引擎重发水印 clip 再测一轮
                return true;
            }
            conn->calibrated = true; // 超时兜底：无回声标定，AEC 旁路
            DumpCalibBuf(conn->uid, 99, conn->calib_buf8k);
            conn->calib_buf8k.clear();
            conn->calib_buf8k.shrink_to_fit();
            MDT_WARN("aec calib timeout uid={} -> bypass aec "
                     "(ncc={:.3f} peaks={} matched_slots={})",
                     conn->uid, conn->last_det.peak_ncc,
                     conn->last_det.debug_peaks,
                     conn->last_det.debug_matched_slots);
            InjectAecBypass(conn); // 解锁引擎上行丢帧门（AEC 直通）
            return true;
        }
        // 协议侧 8k 直接检测（叮咚模板匹配），延迟按 8k 采样测得后 ×2 换算回 16k
        auto pcm = audio::DecodeALaw(std::vector<uint8_t>(data, data + len));
        audio::Despike(pcm); // 真机 AGC/削波满幅脉冲（水印 NCC 毒化根因）
        conn->calib_buf8k.insert(conn->calib_buf8k.end(), pcm.begin(), pcm.end());
        // 批量检测：缓冲每增长 ~0.25s 跑一轮模板匹配（逐帧全量 NCC 太贵；
        // 阈值必须小于回环水印长度，否则短回环永远凑不够批量）
        if (conn->calib_buf8k.size() - conn->calib_detect_len < 2000) return false;
        conn->calib_detect_len = conn->calib_buf8k.size();
        const auto det = audio::DetectWatermark(conn->calib_buf8k, m_wmCfg8k);
        conn->last_det = det;
        int peak = 0;
        for (const auto s : conn->calib_buf8k) peak = std::max(peak, std::abs(s));
        MDT_DEBUG("wm detect8k uid={} buf={} peak={} ncc={:.3f} peaks={} margin={} detected={}",
                  conn->uid, conn->calib_buf8k.size(), peak, det.peak_ncc,
                  det.debug_peaks, det.debug_matched_slots, det.detected);
        if (!det.detected) return false;
        conn->calibrated = true;
        MDT_INFO("aec calibrated uid={} delay8k={} skew={:.6f}", conn->uid,
                 det.p1, det.skew);
        Message w;
        w.type = MsgType::kWmDetected;
        w.session_id = conn->sid;
        w.ts_ms = g_clock.NowMs();
        w.aux = det.p1 * 2; // 8k 采样延迟 → 16k
        w.dval = det.skew;
        m_cb.inject(std::move(w));
        return true;
    };
    hooks.forward_asr = [this, conn](const std::vector<int16_t>& pcm, uint32_t flags) {
        if ((flags & msgflag::kVoice) && m_cb.on_audio)
            m_cb.on_audio(conn->sid, pcm, flags);
        Message f;
        f.type = MsgType::kWsAudioFrame;
        f.session_id = conn->sid;
        f.ts_ms = g_clock.NowMs();
        // 剥离 kVoice：convai 端侧无本地 VAD，插件每帧盲标 kVoice；
        // 倾听/打断状态机以 kVadUpdate（APM VAD）为准，此处只保留断句位
        f.flags = flags & ~msgflag::kVoice;
        f.aux = static_cast<int64_t>(conn->gen);
        m_cb.inject(std::move(f));
    };
    hooks.is_calibrated = [conn] { return conn->calibrated; };

    if (!conn->plugin->Load(plugin_path, std::move(hooks))) {
        MDT_ERROR("protocol plugin load failed: {}", conn->plugin->LastError());
        boost::system::error_code ec;
        stream.close(ws::close_code::internal_error, ec);
        return;
    }
    conn->plugin->SetConfigKey(m_protocolKey); // 配置槽注入（设备 Key 等）

    // ---- 读循环：原始帧 → 插件（头部解析也在插件内）；poll 轮转出站 ----
    try {
        beast::flat_buffer buf;
        const auto fd = beast::get_lowest_layer(stream).socket().native_handle();
        for (;;) {
            buf.consume(buf.size());
            if (!WaitReadable(fd, static_cast<int>(kReadPollTimeout.count()))) {
                DrainOutbound(conn);
                continue;
            }
            boost::system::error_code ec;
            stream.read(buf, ec);
            if (ec) {
                if (ec != ws::error::closed)
                    MDT_DEBUG("plugin session ended: {}", ec.message());
                break;
            }
            DrainOutbound(conn);
            if (stream.got_text()) {
                conn->plugin->OnWsText(beast::buffers_to_string(buf.data()));
                continue;
            }
            auto data = static_cast<const uint8_t*>(buf.data().data());
            // 调试：apk 设备上行原始字节落盘（wasm 前，定位满幅毛刺来源）
            if (conn->uid.find("apk") != std::string::npos) {
                if (!conn->raw_dump) {
                    char p[256];
                    std::snprintf(p, sizeof(p),
                                  "/mnt/d/agent/wsl/creek-cluster/crash/uplink_raw_%s.bin",
                                  conn->uid.c_str());
                    conn->raw_dump = std::fopen(p, "ab");
                }
                if (conn->raw_dump && buf.size() > 4) {
                    const uint32_t fn = static_cast<uint32_t>(buf.size());
                    std::fwrite(&fn, 4, 1, conn->raw_dump);
                    std::fwrite(data, 1, fn, conn->raw_dump);
                    std::fflush(conn->raw_dump);
                }
            }
            conn->plugin->OnWsBinary(data, buf.size());
        }
    } catch (const std::exception& e) {
        MDT_DEBUG("plugin session error: {}", e.what());
    }
}

void WsServer::InjectWmRetry(const std::shared_ptr<Conn>& conn) {
    // 标定超时重试：引擎据此重发水印 clip（m_wmPending 保持 true，
    // 上行帧继续丢弃直到标定成功或重试耗尽走 bypass）
    Message r;
    r.type = MsgType::kWmRetry;
    r.session_id = conn->sid;
    r.ts_ms = g_clock.NowMs();
    r.aux = static_cast<int64_t>(conn->gen);
    m_cb.inject(std::move(r));
}

void WsServer::InjectAecBypass(const std::shared_ptr<Conn>& conn) {
    // 标定超时旁路：引擎据此停止丢弃上行帧（m_wmPending=false），
    // AEC 直通（不注入 kWmDetected，valid 保持 false，APM 不做对齐）
    Message b;
    b.type = MsgType::kAecBypass;
    b.session_id = conn->sid;
    b.ts_ms = g_clock.NowMs();
    b.aux = static_cast<int64_t>(conn->gen); // 代际号：引擎忽略旧连接迟到的 bypass
    m_cb.inject(std::move(b));
}

void WsServer::CleanupConn(std::shared_ptr<Conn> conn) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // 仅当当前登记的仍是本连接（没被重登踢掉）时移除
        auto it = m_conns.find(SidHex(conn->sid));
        if (it != m_conns.end() && it->second.get() == conn.get())
            m_conns.erase(it);
    }
    if (conn->raw_dump) {
        std::fclose(conn->raw_dump);
        conn->raw_dump = nullptr;
    }
    if (!conn->uid.empty()) {
        Message d;
        d.type = MsgType::kWsDisconnected;
        d.session_id = conn->sid;
        d.ts_ms = g_clock.NowMs();
        d.aux = static_cast<int64_t>(conn->gen); // 代际号：引擎据此忽略旧连接迟到的断连
        m_cb.inject(std::move(d));
        MDT_INFO("session disconnected uid={}", conn->uid);
        telemetry::Registry::Instance()
            .MakeGauge("ws_connections_active", "", "active WSS connections")
            .Set(static_cast<double>(ConnectionCount()));
    }
}

void WsServer::SendBinary(const SessionId& sid, ClipId clip,
                          const std::vector<uint8_t>& bytes) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    // 水印 clip 下发时刻 = 标定计时起点（含重发）：每轮 3s 检测窗独立，
    // 不受"连接早、水印晚发"影响（连接 3s 后才首播水印的调度下必需）
    if (clip == clip::kWatermark) {
        conn->calib_start_ms = 0;
        conn->calib_buf.clear();
        conn->calib_buf8k.clear();
        conn->calib_detect_len = 0;
    }
    // 协议插件连接：clip 包装由插件完成
    if (conn->use_plugin) {
        if (conn->plugin) conn->plugin->OnOutboundClip(clip, bytes);
        return;
    }
    std::vector<uint8_t> frame(4 + bytes.size());
    WriteU32LE(frame.data(), clip);
    std::memcpy(frame.data() + 4, bytes.data(), bytes.size());
    QueueOutbound(conn, false, std::move(frame));
}

void WsServer::SendText(const SessionId& sid, const std::string& text) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    // 协议插件连接：文本 outbound 经插件映射
    if (conn->use_plugin) {
        if (conn->plugin) conn->plugin->OnOutboundText(text);
        return;
    }
    QueueOutbound(conn, true, {text.begin(), text.end()});
}

void WsServer::NotifyThinking(const SessionId& sid) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    if (conn->use_plugin && conn->plugin) conn->plugin->OnThinking();
}

void WsServer::NotifyInterrupted(const SessionId& sid) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    if (conn->use_plugin && conn->plugin) conn->plugin->OnInterrupted();
}

void WsServer::NotifyLlmText(const SessionId& sid, const std::string& text) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    if (conn->use_plugin && conn->plugin) conn->plugin->OnLlmText(text);
}

size_t WsServer::ConnectionCount() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_conns.size();
}

} // namespace mediator::net
