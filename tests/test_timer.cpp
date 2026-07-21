// 高精度定时器：到期执行、skippable 跳过策略、消息任务永不跳过
#include <gtest/gtest.h>

#include "core/priority_timer.h"

using mediator::PriorityTimer;

TEST(PriorityTimer, ExecutesInDueOrder) {
    PriorityTimer t;
    std::vector<int> order;
    t.Schedule(300, "c", false, [&] { order.push_back(3); });
    t.Schedule(100, "a", false, [&] { order.push_back(1); });
    t.Schedule(200, "b", false, [&] { order.push_back(2); });
    auto [exec, skip] = t.RunExpired(1000);
    EXPECT_EQ(exec, 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(PriorityTimer, SkippableTaskDroppedWhenLateOver1s) {
    PriorityTimer t; // 阈值 1000ms
    bool ran = false;
    t.Schedule(0, "gc_tick", /*skippable=*/true, [&] { ran = true; });
    auto [exec, skip] = t.RunExpired(1501); // 迟到 1501ms > 1000ms
    EXPECT_EQ(exec, 0u);
    EXPECT_EQ(skip, 1u);
    EXPECT_FALSE(ran);
}

TEST(PriorityTimer, MessageTaskNeverSkipped) {
    PriorityTimer t;
    bool ran = false;
    t.Schedule(0, "msg", /*skippable=*/false, [&] { ran = true; });
    auto [exec, skip] = t.RunExpired(999999);
    EXPECT_EQ(exec, 1u);
    EXPECT_EQ(skip, 0u);
    EXPECT_TRUE(ran);
}

TEST(PriorityTimer, SkippableRunsWhenWithinThreshold) {
    PriorityTimer t;
    bool ran = false;
    t.Schedule(0, "tick", true, [&] { ran = true; });
    t.RunExpired(999);
    EXPECT_TRUE(ran);
}
