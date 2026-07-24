// ============================================================================
// grpc_clients.cpp — gRPC 客户端实现
//
// 关键点：
//   - 每个调用新建 ClientContext 并 AddMetadata("x-session-id", hex)
//   - ASR 读线程循环 Read()：is_final → on_final 并结束；否则 on_partial
//   - 调用失败（网络错/服务不可用）返回空串/false，由网关侧按超时/降级处理
// ============================================================================
#include "net/grpc_clients.h"

#include "core/log.h"

namespace mediator::net {

std::string SidHex(const SessionId& sid) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (auto b : sid) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

GrpcBackend::GrpcBackend(const std::string& addr)
    : m_channel(grpc::CreateChannel(addr, grpc::InsecureChannelCredentials())),
      m_asr(AsrService::NewStub(m_channel)),
      m_llm(LlmService::NewStub(m_channel)),
      m_tts(TtsService::NewStub(m_channel)),
      m_business(BusinessService::NewStub(m_channel)),
      m_memory(MemoryService::NewStub(m_channel)) {}

std::string GrpcBackend::LlmGenerate(const std::string& method,
                                     const std::string& text, const SessionId& sid) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("x-session-id", SidHex(sid));
    LlmRequest req;
    req.set_method(method);
    req.set_text(text);
    req.set_session_id(SidHex(sid));
    LlmResponse resp;
    const auto st = m_llm->Generate(&ctx, req, &resp);
    if (!st.ok()) {
        MDT_WARN("llm {} rpc failed: {}", method, st.error_message());
        return {};
    }
    return resp.text();
}

bool GrpcBackend::InterruptJudge(const std::string& text, const SessionId& sid,
                                 bool* interrupt) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("x-session-id", SidHex(sid));
    InterruptRequest req;
    req.set_text(text);
    req.set_session_id(SidHex(sid));
    InterruptResponse resp;
    const auto st = m_llm->JudgeInterrupt(&ctx, req, &resp);
    if (!st.ok()) {
        MDT_WARN("llm judge_interrupt rpc failed: {}", st.error_message());
        return false;
    }
    *interrupt = resp.interrupt();
    return true;
}

std::string GrpcBackend::TtsSynth(const std::string& text, ClipId clip,
                                  const SessionId& sid) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("x-session-id", SidHex(sid));
    TtsRequest req;
    req.set_text(text);
    req.set_session_id(SidHex(sid));
    req.set_clip_id(clip);
    TtsResponse resp;
    const auto st = m_tts->Synth(&ctx, req, &resp);
    if (!st.ok()) {
        MDT_WARN("tts synth rpc failed: {}", st.error_message());
        return {};
    }
    return resp.pcm();
}

std::string GrpcBackend::BusinessControl(const std::string& cmd, const SessionId& sid) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("x-session-id", SidHex(sid));
    ControlRequest req;
    req.set_cmd(cmd);
    req.set_session_id(SidHex(sid));
    ControlResponse resp;
    const auto st = m_business->Control(&ctx, req, &resp);
    if (!st.ok()) return {};
    return resp.ack();
}

bool GrpcBackend::MemoryFetch(const std::string& redis_key, const SessionId& sid) {
    grpc::ClientContext ctx;
    ctx.AddMetadata("x-session-id", SidHex(sid));
    MemoryRequest req;
    req.set_redis_key(redis_key);
    req.set_session_id(SidHex(sid));
    MemoryResponse resp;
    const auto st = m_memory->FetchContext(&ctx, req, &resp);
    return st.ok() && resp.ok();
}

// ---- AsrStream ----

GrpcBackend::AsrStream::AsrStream(AsrService::Stub* stub, const SessionId& sid,
                                  std::function<void(std::string)> on_partial,
                                  std::function<void(std::string)> on_final) {
    m_ctx.AddMetadata("x-session-id", SidHex(sid));
    m_stream = stub->StreamingRecognize(&m_ctx);
    m_reader = std::thread([this, op = std::move(on_partial), of = std::move(on_final)] {
        AsrResponse resp;
        // 读完 final 必须继续读：多轮对话/打断复用同一条流，
        // 提前退出会让后续 partial/final 无人接收（会话变"聋"）
        while (m_stream->Read(&resp)) {
            if (resp.is_final()) of(resp.text());
            else op(resp.text());
        }
    });
}

GrpcBackend::AsrStream::~AsrStream() {
    m_done = true;
    m_ctx.TryCancel();
    if (m_reader.joinable()) m_reader.join();
}

bool GrpcBackend::AsrStream::Write(const std::vector<int16_t>& pcm, uint32_t flags) {
    AsrRequest req;
    req.set_pcm(pcm.data(), pcm.size() * sizeof(int16_t));
    req.set_flags(flags);
    return m_stream->Write(req);
}

void GrpcBackend::AsrStream::WritesDone() { m_stream->WritesDone(); }

std::unique_ptr<GrpcBackend::AsrStream> GrpcBackend::NewAsrStream(
    const SessionId& sid, std::function<void(std::string)> on_partial,
    std::function<void(std::string)> on_final) {
    return std::make_unique<AsrStream>(m_asr.get(), sid, std::move(on_partial),
                                       std::move(on_final));
}

} // namespace mediator::net
