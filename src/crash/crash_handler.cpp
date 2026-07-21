// ============================================================================
// crash_handler.cpp — 崩溃信号处理器实现
//
// 信号上下文只调用 async-signal-safe 函数：backtrace /
// backtrace_symbols_fd / write / open / _exit（禁用 snprintf 之外的 libc
// 格式化）。文件名用固定缓冲手工拼，避免堆分配。
// ============================================================================
#include "crash/crash_handler.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "core/log.h"

namespace mediator::crash {

namespace {
CrashConfig g_cfg;
char g_dumpPath[512];
char g_metaPath[512];

const char* SigName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    default: return "SIG?";
    }
}

void Handler(int sig, siginfo_t* info, void*) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    const int fd = ::open(g_dumpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char head[256];
        const int hn = ::snprintf(head, sizeof(head),
                                  "signal=%s(%d) addr=%p pid=%d\n--- backtrace ---\n",
                                  SigName(sig), sig, info ? info->si_addr : nullptr,
                                  ::getpid());
        (void)::write(fd, head, hn);
        backtrace_symbols_fd(frames, n, fd);
        (void)::write(fd, "\n", 1);
        ::close(fd);
    }
    const int mf = ::open(g_metaPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mf >= 0) {
        char meta[256];
        const int mn = ::snprintf(meta, sizeof(meta), "version=%s instance=%s signal=%s\n",
                                  g_cfg.version.c_str(), g_cfg.instance_id.c_str(),
                                  SigName(sig));
        (void)::write(mf, meta, mn);
        ::close(mf);
    }
    // 恢复默认行为并重新触发，保留 coredump 兜底（§7B.1）
    ::signal(sig, SIG_DFL);
    ::raise(sig);
    ::_exit(128 + sig);
}
} // namespace

bool InstallCrashHandler(const CrashConfig& cfg) {
    g_cfg = cfg;
    std::error_code ec;
    std::filesystem::create_directories(cfg.dump_dir, ec);
    if (ec) {
        MDT_WARN("crash dump dir not writable: {}", cfg.dump_dir);
        return false;
    }
    ::snprintf(g_dumpPath, sizeof(g_dumpPath), "%s/crash_%d.txt", cfg.dump_dir.c_str(),
               ::getpid());
    ::snprintf(g_metaPath, sizeof(g_metaPath), "%s/crash_%d.meta", cfg.dump_dir.c_str(),
               ::getpid());

    struct sigaction sa {};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) sigaction(sig, &sa, nullptr);
    MDT_INFO("crash handler installed, dump dir={}", cfg.dump_dir);
    return true;
}

size_t CountExistingDumps(const std::string& dump_dir) {
    std::error_code ec;
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dump_dir, ec))
        if (e.path().filename().string().rfind("crash_", 0) == 0 &&
            e.path().extension() == ".txt")
            ++n;
    return n;
}

} // namespace mediator::crash
