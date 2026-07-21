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

    // Prometheus 文本格式快照（调试用；生产走 OTLP 推送，见 otlp_exporter.h）
    std::string ExportPrometheus() const;

    // OTLP 导出快照（otlp_exporter 使用）
    struct CounterSnap { std::string name, labels, help; int64_t value; };
    struct GaugeSnap { std::string name, labels, help; double value; };
    struct HistSnap {
        std::string name, labels, help;
        std::vector<uint64_t> bucket_counts; // 单桶计数（导出方负责累积）
        uint64_t total = 0;
        double sum = 0;
    };
    void Snapshot(std::vector<CounterSnap>& counters, std::vector<GaugeSnap>& gauges,
                  std::vector<HistSnap>& histograms) const;

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

} // namespace mediator::telemetry
