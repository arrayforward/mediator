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
    std::shared_ptr<ws::stream<beast::ssl_stream<tcp::socket>>> stream;
    std::mutex wmtx; // Beast 流写操作串行化
    std::string uid;
    SessionId sid{};
    uint64_t gen = 0;
    bool calibrated = false;
    std::vector<int16_t> calib_buf;
};

namespace {
SteadyClock g_clock; // 接入层时间戳（引擎内部仍用注入时钟）

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
            tcp::socket sock(ioc);
            boost::system::error_code ec;
            acceptor->accept(sock, ec);
            if (ec || !m_running) break;
            conn->stream = std::make_shared<ws::stream<beast::ssl_stream<tcp::socket>>>(
                beast::ssl_stream<tcp::socket>(std::move(sock), ssl_ctx));
            std::thread([this, conn] { SessionThread(conn); }).detach();
        }
    } catch (const std::exception& e) {
        MDT_ERROR("accept loop error: {}", e.what());
    }
}

void WsServer::SessionThread(std::shared_ptr<Conn> conn) {
    auto& stream = *conn->stream;
    try {
        // TLS + WS 握手
        stream.next_layer().handshake(asio::ssl::stream_base::server);
        stream.accept();

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

        // ---- 读循环 ----
        for (;;) {
            buf.consume(buf.size());
            stream.read(buf);
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
            const auto pcm = audio::DecodeALaw(g711);

            MDT_DEBUG("ws frame uid={} flags={} g711_bytes={}", conn->uid, flags,
                      g711.size());
            // 水印标定（仅未标定会话）
            if (!conn->calibrated) {
                conn->calib_buf.insert(conn->calib_buf.end(), pcm.begin(), pcm.end());
                const auto det = audio::DetectWatermark(conn->calib_buf, m_wmCfg);
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
    // ---- 断连清理 ----
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // 仅当当前登记的仍是本连接（没被重登踢掉）时移除
        auto it = m_conns.find(SidHex(conn->sid));
        if (it != m_conns.end() && it->second.get() == conn.get())
            m_conns.erase(it);
    }
    if (!conn->uid.empty()) {
        Message d;
        d.type = MsgType::kWsDisconnected;
        d.session_id = conn->sid;
        d.ts_ms = g_clock.NowMs();
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
    std::vector<uint8_t> frame(4 + bytes.size());
    WriteU32LE(frame.data(), clip);
    std::memcpy(frame.data() + 4, bytes.data(), bytes.size());
    std::lock_guard<std::mutex> wlk(conn->wmtx);
    boost::system::error_code ec;
    conn->stream->binary(true);
    conn->stream->write(asio::buffer(frame), ec);
    if (ec) MDT_DEBUG("ws send binary failed: {}", ec.message());
}

void WsServer::SendText(const SessionId& sid, const std::string& text) {
    std::shared_ptr<Conn> conn;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_conns.find(SidHex(sid));
        if (it == m_conns.end()) return;
        conn = it->second;
    }
    std::lock_guard<std::mutex> wlk(conn->wmtx);
    boost::system::error_code ec;
    conn->stream->text(true);
    conn->stream->write(asio::buffer(text), ec);
}

size_t WsServer::ConnectionCount() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_conns.size();
}

} // namespace mediator::net
