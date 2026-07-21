// ============================================================================
// context_evict.h — 聊天上下文淘汰（设计文档 §5.3 CtxEvict）
//
// 实现思路：
//   单会话上下文上限 1MB。超限后从最旧的一端删除整轮对话（按 '\n' 分轮，
//   对齐行边界避免截断半轮），直到剩余 50%（512KB）。
//   留 50% 而非贴线：避免每轮对话都触发淘汰的抖动。淘汰结果由心跳
//   写回 Redis（SETEX ctx:{uid}）。
//   纯函数无状态，直接可测。
// ============================================================================
#pragma once

#include <cstddef>
#include <string>

namespace mediator::session {

// 上下文淘汰（§5.3 CtxEvict）：超过 max_bytes 时删除最早对话，直到剩余 target_ratio
// 对话按 '\n' 分行（每行一轮对话），从头部（最早）删除。
// 返回淘汰后的上下文；若未超限原样返回。
std::string EvictContext(const std::string& ctx, size_t max_bytes, double target_ratio = 0.5);

} // namespace mediator::session
