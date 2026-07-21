// 上下文淘汰：未超限不动、超限删最早对话至 50%、行边界对齐
#include <gtest/gtest.h>

#include "session/context_evict.h"

using mediator::session::EvictContext;

TEST(CtxEvict, UnderLimitUnchanged) {
    const std::string ctx = "U:你好\nA:你好呀\n";
    EXPECT_EQ(EvictContext(ctx, 1024), ctx);
}

TEST(CtxEvict, OverLimitDropsOldestToHalf) {
    // 每行 10 字节，共 200 行 = 2000 字节；上限 1000 → 淘汰至 ≤500，行边界对齐
    std::string ctx;
    for (int i = 0; i < 200; ++i) ctx += "012345678\n"; // 10B/行
    ASSERT_EQ(ctx.size(), 2000u);
    const auto out = EvictContext(ctx, 1000);
    EXPECT_LE(out.size(), 500u);          // 不超过 50%
    EXPECT_GT(out.size(), 450u);          // 且尽量接近（只按整行对齐损耗）
    EXPECT_EQ(out, ctx.substr(ctx.size() - out.size())); // 保留最新部分
}

TEST(CtxEvict, AlignsToLineBoundary) {
    // 淘汰点落在行中间时必须对齐到下一行行首（不截断半轮对话）
    std::string ctx;
    for (int i = 0; i < 100; ++i) ctx += std::string(19, 'a' + (i % 26)) + "\n"; // 20B/行
    ASSERT_EQ(ctx.size(), 2000u);
    const auto out = EvictContext(ctx, 1000);
    EXPECT_LE(out.size(), 500u);
    const size_t cut = ctx.size() - out.size();
    EXPECT_EQ(ctx[cut - 1], '\n'); // 切割点恰好在行首
}

TEST(CtxEvict, NoNewlineKeepsTail) {
    const std::string ctx(2000, 'x');
    const auto out = EvictContext(ctx, 1000);
    EXPECT_EQ(out.size(), 500u);
}
