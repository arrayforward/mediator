// ============================================================================
// metrics_server.h — /metrics 抓取端点（与 OTLP 推送并存）
//
// 实现思路：
//   指标双通道：OtlpMetricsExporter 主动推 Collector（OTLP gRPC，主路径），
//   MetricsServer 提供 Prometheus 文本格式 /metrics 抓取端点（调试与
//   Collector prometheus receiver 抓取的备用通道）。两个通道共享同一
//   Registry 快照，语义一致。
//   极简 HTTP/1.0：每连接一次应答，无长连接（Prometheus 抓取模型）。
// ============================================================================
#pragma once

#include <atomic>
#include <thread>

namespace mediator::telemetry {

class MetricsServer {
public:
    bool Start(uint16_t port); // 端口占用返回 false
    void Stop();

private:
    void Loop(int listen_fd);
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    int m_listenFd = -1;
};

} // namespace mediator::telemetry
