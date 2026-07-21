// ============================================================================
// test_telemetry.cpp — 指标注册表与 Prometheus 导出单测（§7A）
//
// 用例：Counter/Gauge/Histogram 语义、导出文本格式（TYPE/bucket/le/count/sum）、
// /metrics HTTP 端点抓取。UNIX only。
// ============================================================================
#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "telemetry/metrics_server.h"
#include "telemetry/otlp_exporter.h"
#include "telemetry/telemetry.h"

using mediator::telemetry::Histogram;
using mediator::telemetry::MetricsServer;
using mediator::telemetry::OtlpConfig;
using mediator::telemetry::OtlpMetricsExporter;
using mediator::telemetry::Registry;

namespace otc = opentelemetry::proto::collector::metrics::v1;

TEST(Telemetry, CounterGaugeHistogramSemantics) {
    Registry reg;
    auto& c = reg.MakeCounter("test_counter", "k=\"v\"", "help");
    c.Add();
    c.Add(4);
    EXPECT_EQ(c.Value(), 5);

    auto& g = reg.MakeGauge("test_gauge", "", "help");
    g.Set(3.5);
    EXPECT_DOUBLE_EQ(g.Value(), 3.5);

    auto& h = reg.MakeHistogram("test_hist_ms", "", "help");
    h.Record(3);   // ≤5 桶
    h.Record(600); // ≤1000 桶
    EXPECT_EQ(h.TotalCount(), 2u);
    EXPECT_DOUBLE_EQ(h.Sum(), 603.0);
}

TEST(Telemetry, PrometheusExportFormat) {
    Registry reg;
    reg.MakeCounter("exp_counter", "a=\"1\"", "c help").Add(7);
    reg.MakeGauge("exp_gauge", "", "g help").Set(1.0);
    reg.MakeHistogram("exp_hist", "", "h help").Record(10);
    const std::string out = reg.ExportPrometheus();
    EXPECT_NE(out.find("# TYPE exp_counter counter"), std::string::npos);
    EXPECT_NE(out.find("exp_counter{a=\"1\"} 7"), std::string::npos);
    EXPECT_NE(out.find("exp_gauge 1.0"), std::string::npos);
    EXPECT_NE(out.find("exp_hist_bucket{le=\"10\"} 1"), std::string::npos);
    EXPECT_NE(out.find("exp_hist_bucket{le=\"+Inf\"} 1"), std::string::npos);
    EXPECT_NE(out.find("exp_hist_count 1"), std::string::npos);
}

namespace {
std::string HttpGet(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
    ::close(fd);
    return out;
}

// 进程内 mock Collector：捕获 OTLP 请求内容供断言
class TestCollector final : public otc::MetricsService::Service {
public:
    grpc::Status Export(grpc::ServerContext*,
                        const otc::ExportMetricsServiceRequest* req,
                        otc::ExportMetricsServiceResponse*) override {
        last_request = *req;
        ++calls;
        return grpc::Status::OK;
    }
    otc::ExportMetricsServiceRequest last_request;
    int calls = 0;
};
} // namespace

TEST(Telemetry, MetricsServerServesScrape) {
    Registry::Instance().MakeCounter("scrape_probe_total", "", "probe").Add(42);
    MetricsServer srv;
    ASSERT_TRUE(srv.Start(19091));
    const std::string body = HttpGet(19091);
    srv.Stop();
    EXPECT_NE(body.find("HTTP/1.0 200 OK"), std::string::npos);
    EXPECT_NE(body.find("scrape_probe_total 42"), std::string::npos);
}

TEST(Telemetry, OtlpExporterPushesToCollector) {
    Registry::Instance().MakeCounter("otlp_probe_total", "k=\"v\"", "probe").Add(42);
    Registry::Instance().MakeGauge("otlp_gauge_probe", "", "probe").Set(2.5);

    TestCollector collector;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:14317", grpc::InsecureServerCredentials());
    builder.RegisterService(&collector);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    OtlpConfig cfg;
    cfg.endpoint = "127.0.0.1:14317";
    OtlpMetricsExporter exporter(cfg);
    exporter.ExportOnce(); // 同步推送一次
    server->Shutdown();

    ASSERT_EQ(collector.calls, 1);
    const auto& rm = collector.last_request.resource_metrics(0);
    // 资源标签
    std::string svc;
    for (const auto& a : rm.resource().attributes())
        if (a.key() == "service.name") svc = a.value().string_value();
    EXPECT_EQ(svc, "mediator");
    // 指标映射：Counter→Sum(monotonic) / Gauge→Gauge
    bool found_counter = false, found_gauge = false;
    for (const auto& sm : rm.scope_metrics())
        for (const auto& m : sm.metrics()) {
            if (m.name() == "otlp_probe_total") {
                found_counter = true;
                EXPECT_TRUE(m.has_sum());
                EXPECT_TRUE(m.sum().is_monotonic());
                EXPECT_EQ(m.sum().data_points(0).as_int(), 42);
                EXPECT_EQ(m.sum().data_points(0).attributes(0).key(), "k");
            }
            if (m.name() == "otlp_gauge_probe") {
                found_gauge = true;
                EXPECT_DOUBLE_EQ(m.gauge().data_points(0).as_double(), 2.5);
            }
        }
    EXPECT_TRUE(found_counter);
    EXPECT_TRUE(found_gauge);
}
