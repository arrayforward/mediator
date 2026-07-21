// ============================================================================
// otlp_exporter.h — OpenTelemetry OTLP gRPC 指标导出（设计文档 §7A，正统 OTel 路径）
//
// 实现思路：
//   周期性（默认 10s）把 Registry 的快照编码为
//   opentelemetry.proto.collector.metrics.v1.ExportMetricsServiceRequest，
//   通过 gRPC 推送给 OTel Collector（默认 127.0.0.1:4317）。
//   - Counter   → Sum（monotonic, cumulative temporality）
//   - Gauge     → Gauge
//   - Histogram → Histogram（explicit_bounds + bucket_counts 累积语义）
//   - 资源标签：service.name=mediator、service.version、gw_instance_id
//   - labels 字符串（k="v",k2="v2"）解析为 OTLP KeyValue attributes
//
// 线程模型：导出线程独立，心跳/业务线程只做原子累加（§7A.1 低开销要求）。
// Collector 不可达：本次导出丢弃 + WARN，不阻塞、不重试积压。
// ============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace mediator::telemetry {

struct OtlpConfig {
    std::string endpoint = "127.0.0.1:4317";
    int interval_s = 10;
    std::string service_name = "mediator";
    std::string service_version = "dev";
    std::string instance_id = "gw-local";
};

class OtlpMetricsExporter {
public:
    explicit OtlpMetricsExporter(OtlpConfig cfg);
    ~OtlpMetricsExporter();

    void Start();
    void Stop();
    void ExportOnce(); // 测试钩子：立即导出一次

private:
    void Loop();

    OtlpConfig m_cfg;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace mediator::telemetry
