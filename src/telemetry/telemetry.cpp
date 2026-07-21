// ============================================================================
// telemetry.cpp — 指标注册表 / Prometheus 导出 / 内嵌 HTTP 服务实现
//
// Histogram 桶：1/5/10/20/50/100/250/500/1000/5000/15000 ms +Inf。
// /metrics：每连接一次 HTTP/1.0 应答（Prometheus 抓取模型，无需长连接）。
// ============================================================================
#include "telemetry/telemetry.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace mediator::telemetry {

// ---- Histogram ----

const std::vector<double>& Histogram::Buckets() {
    static const std::vector<double> kB = {1, 5, 10, 20, 50, 100, 250, 500, 1000, 5000, 15000};
    return kB;
}

void Histogram::Record(double v) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_counts.empty()) m_counts.resize(Buckets().size() + 1, 0); // 末桶 = +Inf
    size_t idx = Buckets().size();
    for (size_t i = 0; i < Buckets().size(); ++i)
        if (v <= Buckets()[i]) { idx = i; break; }
    // 累积语义在导出时计算；此处记单桶
    ++m_counts[idx];
    m_sum += v;
    m_count.fetch_add(1, std::memory_order_relaxed);
}

// ---- Registry ----

Registry& Registry::Instance() {
    static Registry r;
    return r;
}

Counter& Registry::MakeCounter(const std::string& name, const std::string& labels,
                               const std::string& help) {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& e : m_counters)
        if (e.name == name && e.labels == labels) return *e.obj;
    m_counters.push_back({name, labels, help, std::make_shared<Counter>()});
    return *m_counters.back().obj;
}

Gauge& Registry::MakeGauge(const std::string& name, const std::string& labels,
                           const std::string& help) {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& e : m_gauges)
        if (e.name == name && e.labels == labels) return *e.obj;
    m_gauges.push_back({name, labels, help, std::make_shared<Gauge>()});
    return *m_gauges.back().obj;
}

Histogram& Registry::MakeHistogram(const std::string& name, const std::string& labels,
                                   const std::string& help) {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto& e : m_histograms)
        if (e.name == name && e.labels == labels) return *e.obj;
    m_histograms.push_back({name, labels, help, std::make_shared<Histogram>()});
    return *m_histograms.back().obj;
}

namespace {
std::string LabelsSuffix(const std::string& labels) {
    return labels.empty() ? std::string{} : "{" + labels + "}";
}
} // namespace

std::string Registry::ExportPrometheus() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::string out;
    char buf[512];
    for (const auto& e : m_counters) {
        out += "# HELP " + e.name + " " + e.help + "\n# TYPE " + e.name + " counter\n";
        std::snprintf(buf, sizeof(buf), "%s%s %lld\n", e.name.c_str(),
                      LabelsSuffix(e.labels).c_str(), static_cast<long long>(e.obj->Value()));
        out += buf;
    }
    for (const auto& e : m_gauges) {
        out += "# HELP " + e.name + " " + e.help + "\n# TYPE " + e.name + " gauge\n";
        std::snprintf(buf, sizeof(buf), "%s%s %.4f\n", e.name.c_str(),
                      LabelsSuffix(e.labels).c_str(), e.obj->Value());
        out += buf;
    }
    for (const auto& e : m_histograms) {
        out += "# HELP " + e.name + " " + e.help + "\n# TYPE " + e.name + " histogram\n";
        const auto& buckets = Histogram::Buckets();
        const auto& counts = e.obj->Counts();
        uint64_t cum = 0;
        for (size_t i = 0; i < buckets.size(); ++i) {
            if (i < counts.size()) cum += counts[i];
            std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"%.0f\"%s%s} %llu\n", e.name.c_str(),
                          buckets[i], e.labels.empty() ? "" : ",", e.labels.c_str(),
                          static_cast<unsigned long long>(cum));
            out += buf;
        }
        if (!counts.empty()) cum += counts.back();
        std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"+Inf\"%s%s} %llu\n", e.name.c_str(),
                      e.labels.empty() ? "" : ",", e.labels.c_str(),
                      static_cast<unsigned long long>(cum));
        out += buf;
        std::snprintf(buf, sizeof(buf), "%s_count%s %llu\n%s_sum%s %.4f\n", e.name.c_str(),
                      LabelsSuffix(e.labels).c_str(),
                      static_cast<unsigned long long>(e.obj->TotalCount()), e.name.c_str(),
                      LabelsSuffix(e.labels).c_str(), e.obj->Sum());
        out += buf;
    }
    return out;
}

// ---- MetricsServer ----

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
    MDT_INFO("metrics endpoint listening on :{}/metrics", port);
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
