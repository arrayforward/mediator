// ============================================================================
// telemetry.h — 指标遥测（OTel 语义 API + Prometheus 导出）
//
// 实现思路：
//   OTel 风格的三类仪器（Counter/Gauge/Histogram），统一注册到 Registry，
//   内置 HTTP /metrics 端点输出 Prometheus 文本格式，由 Prometheus 或
//   OTel Collector（prometheus receiver）抓取后转 OTLP 上报。
//   选择此路径而非直接链 opentelemetry-cpp SDK 的原因：SDK 需源码构建
//   （Ubuntu 22.04 无包、FetchContent 全量构建 >15min 且引入 protobuf
//   版本耦合风险）；本实现接口与 OTel 语义对齐，未来可无缝替换为
//   opentelemetry-cpp 的 Meter API（仅改本文件实现）。
//
//   线程模型：所有仪器更新为原子操作（心跳线程只做 Add/Record，无 IO）；
//   /metrics 抓取在独立线程快照导出（设计文档 §7A.1 低开销要求）。
//
// 标签支持：单个 labels 字符串（如 {clip="3"}），简化序列化。
// ============================================================================
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mediator::telemetry {

class Counter {
public:
    void Add(int64_t delta = 1) { m_v.fetch_add(delta, std::memory_order_relaxed); }
    int64_t Value() const { return m_v.load(std::memory_order_relaxed); }

private:
    std::atomic<int64_t> m_v{0};
};

class Gauge {
public:
    void Set(double v) { m_bits.store(std::bit_cast<uint64_t>(v), std::memory_order_relaxed); }
    double Value() const { return std::bit_cast<double>(m_bits.load(std::memory_order_relaxed)); }

private:
    std::atomic<uint64_t> m_bits{0};
};

class Histogram {
public:
    void Record(double v);
    // Prometheus 直方图导出：固定桶边界（毫秒场景）
    static const std::vector<double>& Buckets();
    const std::vector<uint64_t>& Counts() const { return m_counts; }
    uint64_t TotalCount() const { return m_count.load(std::memory_order_relaxed); }
    double Sum() const { return m_sum; }

private:
    std::vector<uint64_t> m_counts;      // 各桶计数（锁内）
    mutable std::mutex m_mtx;
    std::atomic<uint64_t> m_count{0};
    double m_sum = 0.0;                  // 锁内更新（浮点不能用 fetch_add 位运算）
};

// 注册表：name+labels → 仪器。全局单例（GetRegistry()）。
class Registry {
public:
    Counter& MakeCounter(const std::string& name, const std::string& labels = "",
                         const std::string& help = "");
    Gauge& MakeGauge(const std::string& name, const std::string& labels = "",
                     const std::string& help = "");
    Histogram& MakeHistogram(const std::string& name, const std::string& labels = "",
                             const std::string& help = "");

    // Prometheus 文本格式快照（/metrics 端点内容）
    std::string ExportPrometheus() const;

    static Registry& Instance();

private:
    struct CounterEntry { std::string name, labels, help; std::shared_ptr<Counter> obj; };
    struct GaugeEntry { std::string name, labels, help; std::shared_ptr<Gauge> obj; };
    struct HistEntry { std::string name, labels, help; std::shared_ptr<Histogram> obj; };
    mutable std::mutex m_mtx;
    std::vector<CounterEntry> m_counters;
    std::vector<GaugeEntry> m_gauges;
    std::vector<HistEntry> m_histograms;
};

// 内嵌 /metrics HTTP 服务（独立线程，极简 HTTP/1.0 应答）
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
