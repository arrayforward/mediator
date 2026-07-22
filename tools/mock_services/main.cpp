// ============================================================================
// mock_services — 五合一极简 mock gRPC 服务（设计文档 §9.2）
//
// ASR/LLM/TTS/Business/Memory 全部注册在同一端口，行为脚本化：
//   ASR(bidi)：每 3 帧回 partial；收到 flags&2(ASR语义断句) 回 final 并复位
//   LLM：      按 method 回固定文本（quick/restate/answer/quick_placeholder）
//   TTS：      文本 → 440Hz 正弦 PCM16（长度 ∝ 字数），验证时长预算
//   Business： ack = "ack:" + cmd
//   Memory：   记录调用并回 ok（GC 路径验证）
// 启动：mock_services [port]（默认 50051）
// ============================================================================
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "core/log.h"
#include "mediator.grpc.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::Status;

namespace mediator {

class MockAsr final : public AsrService::Service {
public:
    Status StreamingRecognize(
        ServerContext* ctx,
        ServerReaderWriter<AsrResponse, AsrRequest>* stream) override {
        MDT_INFO("mock asr stream open sid={}",
                 [&] {
                     auto m = ctx->client_metadata();
                     auto it = m.find("x-session-id");
                     return it != m.end()
                                ? std::string(it->second.data(), it->second.size())
                                : "?";
                 }());
        AsrRequest req;
        int frames = 0;
        while (stream->Read(&req)) {
            ++frames;
            if (req.flags() & 2) { // ASR 语义断句 → final
                AsrResponse r;
                r.set_text("今天天气怎么样");
                r.set_is_final(true);
                stream->Write(r);
                frames = 0;
            } else if (frames % 3 == 0) { // 每 3 帧一个 partial
                AsrResponse r;
                r.set_text("嗯");
                r.set_is_final(false);
                stream->Write(r);
            }
        }
        return Status::OK;
    }
};

class MockLlm final : public LlmService::Service {
public:
    Status Generate(ServerContext*, const LlmRequest* req,
                    LlmResponse* resp) override {
        if (req->method() == "quick")
            resp->set_text("明白，让我想一想");
        else if (req->method() == "restate")
            resp->set_text("你问的是：" + req->text());
        else if (req->method() == "answer")
            resp->set_text("答案是：" + req->text() + "，今天晴朗适合出行");
        else // quick_placeholder
            resp->set_text("稍等，我还在整理思路");
        MDT_INFO("mock llm method={}", req->method());
        return Status::OK;
    }
};

class MockTts final : public TtsService::Service {
public:
    // 按 clip_id 生成可听辨的测试音型（联调方案：链路通畅的可听证明）
    //   2 安抚：双音"叮-咚"      3 复述：do-mi-sol 上行三音
    //   4 答案："滴-滴-滴—长滴"   5 占位：单音
    Status Synth(ServerContext*, const TtsRequest* req, TtsResponse* resp) override {
        std::string pcm;
        switch (req->clip_id()) {
        case 2: pcm = ToneSeq({{880, 150}, {660, 200}}); break;
        case 3: pcm = ToneSeq({{523, 200}, {659, 200}, {784, 250}}); break;
        case 4: pcm = ToneSeq({{1000, 120}, {0, 80}, {1000, 120}, {0, 80},
                               {1000, 120}, {0, 120}, {1000, 600}});
            break;
        case 5: pcm = ToneSeq({{660, 300}}); break;
        default: pcm = ToneSeq({{440, 400}}); break;
        }
        resp->set_pcm(std::move(pcm));
        MDT_INFO("mock tts clip={} bytes={}", req->clip_id(), resp->pcm().size());
        return Status::OK;
    }

private:
    // {freq_hz(0=静音), ms} 序列 → PCM16 16kHz（10ms 淡入淡出防爆音）
    static std::string ToneSeq(std::initializer_list<std::pair<double, int>> seq) {
        std::string out;
        for (const auto& [freq, ms] : seq) {
            const int n = 16 * ms;
            const size_t base = out.size() / 2;
            out.resize(out.size() + n * 2);
            auto* s = reinterpret_cast<int16_t*>(out.data()) + base;
            for (int i = 0; i < n; ++i) {
                double env = 1.0;
                if (i < 160) env = i / 160.0;
                if (i > n - 160) env = (n - i) / 160.0;
                const double v = (freq > 0)
                    ? 8000 * env * std::sin(2 * 3.14159265 * freq * i / 16000) : 0;
                s[i] = static_cast<int16_t>(v);
            }
        }
        return out;
    }
};

class MockBusiness final : public BusinessService::Service {
public:
    Status Control(ServerContext*, const ControlRequest* req,
                   ControlResponse* resp) override {
        resp->set_ack("ack:" + req->cmd());
        MDT_INFO("mock business cmd={}", req->cmd());
        return Status::OK;
    }
};

class MockMemory final : public MemoryService::Service {
public:
    Status FetchContext(ServerContext*, const MemoryRequest* req,
                        MemoryResponse* resp) override {
        MDT_INFO("mock memory fetch key={}", req->redis_key());
        resp->set_ok(true);
        return Status::OK;
    }
};

// mock OTel Collector：接收 OTLP 指标并记录（E2E 断言数据源）
class MockOtlpCollector final
    : public opentelemetry::proto::collector::metrics::v1::MetricsService::Service {
public:
    grpc::Status Export(
        ServerContext*,
        const opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest*
            req,
        opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceResponse*)
        override {
        std::string svc;
        int metrics = 0;
        for (const auto& rm : req->resource_metrics()) {
            for (const auto& a : rm.resource().attributes())
                if (a.key() == "service.name") svc = a.value().string_value();
            for (const auto& sm : rm.scope_metrics()) metrics += sm.metrics_size();
        }
        MDT_INFO("otel export received: service={} metrics={}", svc, metrics);
        return grpc::Status::OK;
    }
};

} // namespace mediator

int main(int argc, char** argv) {
    mediator::Log::Init("info");
    const std::string addr =
        std::string("0.0.0.0:") + (argc > 1 ? argv[1] : "50051");

    mediator::MockAsr asr;
    mediator::MockLlm llm;
    mediator::MockTts tts;
    mediator::MockBusiness biz;
    mediator::MockMemory mem;
    mediator::MockOtlpCollector otel;

    ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&asr);
    builder.RegisterService(&llm);
    builder.RegisterService(&tts);
    builder.RegisterService(&biz);
    builder.RegisterService(&mem);
    builder.RegisterService(&otel);
    auto server = builder.BuildAndStart();
    MDT_INFO("mock services (5-in-1) listening on {}", addr);
    server->Wait();
    return 0;
}
