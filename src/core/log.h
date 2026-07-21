// ============================================================================
// log.h — 系统日志封装（spdlog）
//
// 实现思路：
//   全系统统一经 mediator::Log 输出，底层 spdlog（异步可配）。
//   业务代码只使用 MDT_* 宏，禁止直接依赖 spdlog 头文件，便于替换实现
//   或在测试中关闭输出。
//   宏自动携带 文件:行号；任务 ID/会话 ID 由调用方格式化进消息。
// ============================================================================
#pragma once

#include <memory>

// 编译期放开所有级别（默认 INFO 会把 MDT_DEBUG 裁掉）
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace mediator {

class Log {
public:
    // 进程启动时调用；可重复调用（幂等）。level: trace/debug/info/warn/err
    static void Init(const std::string& level = "info") {
        auto logger = spdlog::get("mediator");
        if (!logger) logger = spdlog::stdout_color_mt("mediator");
        logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%s:%#] %v");
        logger->set_level(spdlog::level::from_str(level));
        spdlog::set_default_logger(logger);
    }
};

} // namespace mediator

#define MDT_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MDT_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define MDT_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define MDT_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define MDT_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
