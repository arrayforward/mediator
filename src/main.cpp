// ============================================================================
// main.cpp — 进程入口
//
// Linux/WSL：完整网关（WSS + gRPC + wasm 认证可选）。
// Windows：仅框架冒烟（网络层依赖 Boost/gRPC，暂不构建）。
//
// 启动参数：
//   --port=9443               WSS 监听端口
//   --cert=server.crt         TLS 证书（PEM）
//   --key=server.key          TLS 私钥（PEM）
//   --backend=127.0.0.1:50051 后端 gRPC 地址（mock 五合一）
//   --auth-provider=builtin | wasm:<path>   认证提供者（§7.3.1）
//   --jwt-secret=xxx          内置验签密钥
//   --log-level=info
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__unix__) || defined(__linux__)

#include "core/log.h"
#include "crash/crash_handler.h"
#include "gateway.h"

namespace {
std::string ArgVal(int argc, char** argv, const std::string& key,
                   const std::string& def) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(key + "=", 0) == 0) return a.substr(key.size() + 1);
    }
    return def;
}
} // namespace

int main(int argc, char** argv) {
    mediator::GatewayConfig cfg;
    cfg.ws_port = static_cast<uint16_t>(std::stoi(ArgVal(argc, argv, "--port", "9443")));
    cfg.cert_file = ArgVal(argc, argv, "--cert", "server.crt");
    cfg.key_file = ArgVal(argc, argv, "--key", "server.key");
    cfg.backend_addr = ArgVal(argc, argv, "--backend", "127.0.0.1:50051");
    cfg.auth_provider = ArgVal(argc, argv, "--auth-provider", "builtin");
    cfg.jwt_secret = ArgVal(argc, argv, "--jwt-secret", "dev-secret");
    cfg.allow_debug_token = ArgVal(argc, argv, "--allow-debug-token", "false") == "true";
    cfg.redis_host = ArgVal(argc, argv, "--redis-host", "127.0.0.1");
    cfg.redis_port = std::stoi(ArgVal(argc, argv, "--redis-port", "6379"));
    cfg.metrics_port = static_cast<uint16_t>(std::stoi(ArgVal(argc, argv, "--metrics-port", "0")));
    cfg.observers = ArgVal(argc, argv, "--observers", "");
    cfg.enable_apm = ArgVal(argc, argv, "--enable-apm", "true") == "true";

    mediator::Log::Init(ArgVal(argc, argv, "--log-level", "info"));

    // 崩溃转储（§7B）：安装信号处理器 + 补报历史崩溃
    mediator::crash::CrashConfig ccfg;
    ccfg.dump_dir = ArgVal(argc, argv, "--crash-dir", "/var/crash/mediator");
    if (const auto prev = mediator::crash::CountExistingDumps(ccfg.dump_dir); prev > 0)
        MDT_WARN("found {} previous crash dumps in {}", prev, ccfg.dump_dir);
    mediator::crash::InstallCrashHandler(ccfg);

    try {
        mediator::Gateway gw(std::move(cfg));
        gw.Run();
    } catch (const std::exception& e) {
        MDT_ERROR("gateway fatal: {}", e.what());
        return 1;
    }
    return 0;
}

#else // Windows：框架冒烟

#include "core/clock.h"
#include "engine/heartbeat_engine.h"

int main() {
    mediator::SteadyClock clock;
    mediator::HeartbeatEngine engine(mediator::EngineConfig{}, clock);
    std::printf("mediator core ready (windows smoke), inbound depth=%zu\n",
                engine.InboundDepth());
    return 0;
}

#endif
