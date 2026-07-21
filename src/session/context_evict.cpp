#include "session/context_evict.h"

namespace mediator::session {

std::string EvictContext(const std::string& ctx, size_t max_bytes, double target_ratio) {
    if (ctx.size() <= max_bytes) return ctx;
    const size_t target = static_cast<size_t>(max_bytes * target_ratio);
    size_t drop = ctx.size() - target;
    // 对齐到行边界，避免截断半轮对话
    size_t pos = ctx.find('\n', drop);
    if (pos == std::string::npos) {
        // 无换行：直接保留尾部 target 字节
        return ctx.substr(ctx.size() - target);
    }
    return ctx.substr(pos + 1);
}

} // namespace mediator::session
