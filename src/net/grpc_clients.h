// ============================================================================
// grpc_clients.h — 后端微服务 gRPC 客户端（阶段4 执行层）
//
// 实现思路：
//   - ASR：每会话一条双向流（AsrStream），网关在收到有声帧时惰性创建；
//     独立读线程把 partial/final 转成 Message 回注引擎（阶段4→1 闭环）。
//   - LLM/TTS/Business/Memory：一元调用，同步 stub 在 ThreadPool 执行，
//     结果同样回注引擎。
//   - 所有调用经 client context metadata 携带 x-session-id（hex），
//     供边车做一致性哈希粘性路由（设计文档 §8）。
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "engine/message.h"
#include "mediator.grpc.pb.h"

namespace mediator::net {

// SessionId → "x-session-id" metadata 用的 hex 串
std::string SidHex(const SessionId& sid);

class GrpcBackend {
public:
    explicit GrpcBackend(const std::string& addr);

    // ---- 一元调用（同步，调用方负责在线程池执行）----
    std::string LlmGenerate(const std::string& method, const std::string& text,
                            const SessionId& sid);
    // feino 语义打断判定：RPC 成功返回 true 并写 *interrupt
    bool InterruptJudge(const std::string& text, const SessionId& sid, bool* interrupt);
    // 返回 PCM16 字节流
    std::string TtsSynth(const std::string& text, ClipId clip, const SessionId& sid);
    std::string BusinessControl(const std::string& cmd, const SessionId& sid);
    bool MemoryFetch(const std::string& redis_key, const SessionId& sid);

    // ---- ASR 双向流 ----
    // on_final/on_partial 由独立读线程回调（注入引擎）
    class AsrStream {
    public:
        AsrStream(AsrService::Stub* stub, const SessionId& sid,
                  std::function<void(std::string)> on_partial,
                  std::function<void(std::string)> on_final);
        ~AsrStream();
        bool Write(const std::vector<int16_t>& pcm, uint32_t flags);
        void WritesDone();

    private:
        grpc::ClientContext m_ctx;
        std::unique_ptr<grpc::ClientReaderWriter<AsrRequest, AsrResponse>> m_stream;
        std::thread m_reader;
        std::atomic<bool> m_done{false};
    };
    std::unique_ptr<AsrStream> NewAsrStream(const SessionId& sid,
                                            std::function<void(std::string)> on_partial,
                                            std::function<void(std::string)> on_final);

private:
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<AsrService::Stub> m_asr;
    std::unique_ptr<LlmService::Stub> m_llm;
    std::unique_ptr<TtsService::Stub> m_tts;
    std::unique_ptr<BusinessService::Stub> m_business;
    std::unique_ptr<MemoryService::Stub> m_memory;
};

} // namespace mediator::net
