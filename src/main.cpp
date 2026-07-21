// ============================================================================
// main.cpp — 进程入口
//
// 当前为框架装配骨架：创建时钟/引擎/Reactor，解析启动参数
// （--auth-provider 等，见设计文档 §7.3.1）。
// 网络层（Boost.Beast WSS / gRPC CQ / Redis / OTel / Breakpad）按 design.md
// 接口逐个接入，接入前 main 仅演示心跳装配。
// ============================================================================
#include <cstdio>
#include <cstring>
#include <string>

#include "core/clock.h"
#include "engine/heartbeat_engine.h"
#include "ext/auth_provider.h"

int main(int argc, char** argv) {
    mediator::EngineConfig cfg;
    std::string auth_provider = "builtin";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const std::string key = "--auth-provider=";
        if (a.rfind(key, 0) == 0) auth_provider = a.substr(key.size());
    }
    std::printf("mediator gateway booting, auth-provider=%s gw_id=%s\n",
                auth_provider.c_str(), cfg.gw_id.c_str());

    mediator::SteadyClock clock;
    mediator::HeartbeatEngine engine(cfg, clock);
    std::printf("engine ready, inbound depth=%zu\n", engine.InboundDepth());
    return 0;
}
