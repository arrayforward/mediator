// ============================================================================
// crash_handler.h — 崩溃转储与符号化调试（设计文档 §7B）
//
// 实现思路（Breakpad 的轻量替代，零外部依赖）：
//   - 安装 SIGSEGV/SIGABRT/SIGFPE/SIGILL 信号处理器，崩溃时：
//       1. backtrace() 抓取调用栈（ glibc execinfo，信号上下文可用）
//       2. backtrace_symbols_fd 直接写文件（避免 malloc，异步信号安全较好）
//       3. 写入 .meta（版本/git sha/实例ID）→ _exit(128+sig)
//   - 转储目录默认 /var/crash/mediator（可用 MEDIATOR_CRASH_DIR 覆盖，
//     测试用临时目录）。
//   - 符号化：RelWithDebInfo 构建自带调试符号，线下用
//       addr2line -e mediator <addr>  或  scripts/symbolize.sh 还原行号；
//     CMake target `symbols` 执行 objcopy 抽符号 + strip（§7B.2 流水线）。
//   - 与 Breakpad 的差异：无 minidump 格式/跨平台符号服务；如需
//     minidump_stackwalk 生态，接口层（InstallCrashHandler）可替换实现。
//   - crash_dumps_total 指标在下次启动扫描转储目录补报。
// ============================================================================
#pragma once

#include <string>

namespace mediator::crash {

struct CrashConfig {
    std::string dump_dir = "/var/crash/mediator";
    std::string version = "dev";
    std::string instance_id = "gw-local";
};

// 安装信号处理器；返回 false 表示目录不可写（不阻断启动）
bool InstallCrashHandler(const CrashConfig& cfg);

// 统计目录中已有转储数量（启动时补报指标/告警）
size_t CountExistingDumps(const std::string& dump_dir);

} // namespace mediator::crash
