// ============================================================================
// metrics_server.cpp — /metrics 抓取端点实现
// ============================================================================
#include "telemetry/metrics_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "telemetry/telemetry.h"

namespace mediator::telemetry {

bool MetricsServer::Start(uint16_t port) {
    m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) return false;
    int one = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(m_listenFd, 8) < 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    m_running = true;
    m_thread = std::thread([this] { Loop(m_listenFd); });
    MDT_INFO("metrics scrape endpoint listening on :{}/metrics", port);
    return true;
}

void MetricsServer::Stop() {
    m_running = false;
    if (m_listenFd >= 0) {
        ::shutdown(m_listenFd, SHUT_RDWR);
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    if (m_thread.joinable()) m_thread.join();
}

void MetricsServer::Loop(int listen_fd) {
    while (m_running) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) break;
        const std::string body = Registry::Instance().ExportPrometheus();
        char head[256];
        std::snprintf(head, sizeof(head),
                      "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      body.size());
        (void)::send(fd, head, std::strlen(head), 0);
        (void)::send(fd, body.data(), body.size(), 0);
        ::close(fd);
    }
}

} // namespace mediator::telemetry
