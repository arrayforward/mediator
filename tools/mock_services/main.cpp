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
#include <string>

#include <grpcpp/grpcpp.h>

#include "core/log.h"
#include "mediator.grpc.pb.h"

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
                 [&] { auto m = ctx->client_metadata(); auto it = m.find("x-session-id");
                       return it != m.end() ? std::string(it->second.data()) : "?"; }());
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
    Status Synth(ServerContext*, const TtsRequest* req, TtsResponse* resp) override {
        // 正弦 PCM16：每字 400 采样（25ms），0.4s ~ 3s
        size_t n = req->text().size() * 400;
        n = std::max<size_t>(6400, std::min<size_t>(n, 48000));
        std::string pcm(n * 2, '\0');
        auto* s = reinterpret_cast<int16_t*>(pcm.data());
        for (size_t i = 0; i < n; ++i)
            s[i] = static_cast<int16_t>(8000 * std::sin(2 * 3.14159265 * 440 * i / 16000));
        resp->set_pcm(std::move(pcm));
        MDT_INFO("mock tts clip={} samples={}", req->clip_id(), n);
        return Status::OK;
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

    ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&asr);
    builder.RegisterService(&llm);
    builder.RegisterService(&tts);
    builder.RegisterService(&biz);
    builder.RegisterService(&mem);
    auto server = builder.BuildAndStart();
    MDT_INFO("mock services (5-in-1) listening on {}", addr);
    server->Wait();
    return 0;
}
