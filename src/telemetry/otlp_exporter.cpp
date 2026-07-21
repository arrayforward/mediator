// ============================================================================
// otlp_exporter.cpp — OTLP gRPC 指标导出实现
//
// 编码要点（opentelemetry-proto v1）：
//   ExportMetricsServiceRequest.resource_metrics[0]
//     .resource.attributes = {service.name, service.version, gw_instance_id}
//     .scope_metrics[0].scope = {name="mediator.telemetry"}
//     .metrics[] = Sum/Gauge/Histogram（cumulative temporality）
//   时间戳：start=进程启动、time=当前（unix nano）
//   Histogram bucket_counts 为单桶值，按 OTLP 要求转累积序列（末桶=+Inf）
// ============================================================================
#include "telemetry/otlp_exporter.h"

#include <chrono>

#include <grpcpp/grpcpp.h>

#include "core/log.h"
#include "telemetry/telemetry.h"

#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"

namespace mediator::telemetry {

namespace {
namespace otc = opentelemetry::proto::collector::metrics::v1;
namespace otm = opentelemetry::proto::metrics::v1;
namespace otc1 = opentelemetry::proto::common::v1;

uint64_t NowNano() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// "k1=\"v1\",k2=\"v2\"" → OTLP KeyValue 列表
void AddAttrs(const std::string& labels,
              ::google::protobuf::RepeatedPtrField<otc1::KeyValue>* out) {
    for (size_t pos = 0; pos < labels.size();) {
        const auto comma = labels.find(',', pos);
        const auto kv = labels.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? labels.size() : comma + 1;
        const auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string v = kv.substr(eq + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        auto* a = out->Add();
        a->set_key(kv.substr(0, eq));
        a->mutable_value()->set_string_value(v);
    }
}

void SetTime(otm::NumberDataPoint* dp, uint64_t start_nano, uint64_t now_nano) {
    dp->set_start_time_unix_nano(start_nano);
    dp->set_time_unix_nano(now_nano);
}
} // namespace

OtlpMetricsExporter::OtlpMetricsExporter(OtlpConfig cfg) : m_cfg(std::move(cfg)) {}

OtlpMetricsExporter::~OtlpMetricsExporter() { Stop(); }

void OtlpMetricsExporter::Start() {
    m_running = true;
    m_thread = std::thread([this] { Loop(); });
    MDT_INFO("otlp exporter -> {} every {}s", m_cfg.endpoint, m_cfg.interval_s);
}

void OtlpMetricsExporter::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void OtlpMetricsExporter::Loop() {
    while (m_running) {
        for (int i = 0; i < m_cfg.interval_s * 10 && m_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (m_running) ExportOnce();
    }
}

void OtlpMetricsExporter::ExportOnce() {
    static const uint64_t kStartNano = NowNano(); // 进程级 start
    const uint64_t now_nano = NowNano();

    // ---- 快照 Registry → OTLP 请求 ----
    std::vector<Registry::CounterSnap> counters;
    std::vector<Registry::GaugeSnap> gauges;
    std::vector<Registry::HistSnap> hists;
    Registry::Instance().Snapshot(counters, gauges, hists);

    otc::ExportMetricsServiceRequest req;
    auto* rm = req.add_resource_metrics();
    {
        auto* a1 = rm->mutable_resource()->add_attributes();
        a1->set_key("service.name");
        a1->mutable_value()->set_string_value(m_cfg.service_name);
        auto* a2 = rm->mutable_resource()->add_attributes();
        a2->set_key("service.version");
        a2->mutable_value()->set_string_value(m_cfg.service_version);
        auto* a3 = rm->mutable_resource()->add_attributes();
        a3->set_key("gw_instance_id");
        a3->mutable_value()->set_string_value(m_cfg.instance_id);
    }
    auto* sm = rm->add_scope_metrics();
    sm->mutable_scope()->set_name("mediator.telemetry");

    for (const auto& c : counters) {
        auto* m = sm->add_metrics();
        m->set_name(c.name);
        m->set_description(c.help);
        auto* sum = m->mutable_sum();
        sum->set_aggregation_temporality(
            otm::AGGREGATION_TEMPORALITY_CUMULATIVE);
        sum->set_is_monotonic(true);
        auto* dp = sum->add_data_points();
        AddAttrs(c.labels, dp->mutable_attributes());
        SetTime(dp, kStartNano, now_nano);
        dp->set_as_int(c.value);
    }
    for (const auto& g : gauges) {
        auto* m = sm->add_metrics();
        m->set_name(g.name);
        m->set_description(g.help);
        auto* dp = m->mutable_gauge()->add_data_points();
        AddAttrs(g.labels, dp->mutable_attributes());
        SetTime(dp, kStartNano, now_nano);
        dp->set_as_double(g.value);
    }
    for (const auto& h : hists) {
        auto* m = sm->add_metrics();
        m->set_name(h.name);
        m->set_description(h.help);
        auto* hist = m->mutable_histogram();
        hist->set_aggregation_temporality(otm::AGGREGATION_TEMPORALITY_CUMULATIVE);
        auto* dp = hist->add_data_points();
        AddAttrs(h.labels, dp->mutable_attributes());
        dp->set_start_time_unix_nano(kStartNano);
        dp->set_time_unix_nano(now_nano);
        dp->set_count(h.total);
        dp->set_sum(h.sum);
        // 单桶计数 → 累积（OTLP bucket_counts 为累积语义，末桶=+Inf）
        const auto& bounds = Histogram::Buckets();
        uint64_t cum = 0;
        for (size_t i = 0; i <= bounds.size(); ++i) {
            if (i < h.bucket_counts.size()) cum += h.bucket_counts[i];
            dp->add_bucket_counts(cum);
            if (i < bounds.size()) dp->add_explicit_bounds(bounds[i]);
        }
    }

    // ---- gRPC 推送（同步、短超时；失败丢弃不积压）----
    auto channel = grpc::CreateChannel(m_cfg.endpoint, grpc::InsecureChannelCredentials());
    auto stub = otc::MetricsService::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    otc::ExportMetricsServiceResponse resp;
    const auto st = stub->Export(&ctx, req, &resp);
    if (!st.ok())
        MDT_WARN("otlp export failed: {} (collector={})", st.error_message(), m_cfg.endpoint);
    else
        MDT_DEBUG("otlp exported {} metrics", sm->metrics_size());
}

} // namespace mediator::telemetry
