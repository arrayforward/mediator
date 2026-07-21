// Reactor：任务命名/自增ID/慢任务统计与告警（虚拟时钟驱动）
#include <gtest/gtest.h>

#include "core/clock.h"
#include "core/reactor.h"

using mediator::PoolKind;
using mediator::Reactor;
using mediator::VirtualClock;

TEST(Reactor, TaskIdsIncreaseAndStatsRecorded) {
    VirtualClock clk;
    Reactor r(clk);
    const uint64_t id1 = r.Post(PoolKind::kCpu, "aec", [] {});
    const uint64_t id2 = r.Post(PoolKind::kCpu, "aec", [] {});
    EXPECT_LT(id1, id2);
    EXPECT_TRUE(r.RunOne(PoolKind::kCpu));
    EXPECT_TRUE(r.RunOne(PoolKind::kCpu));
    const auto* s = r.Stats("aec");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->runs, 2u);
}

TEST(Reactor, SlowCpuTaskTriggersWarn) {
    VirtualClock clk;
    Reactor r(clk);
    std::string slow_name;
    r.SetSlowHandler([&](const mediator::Task& t, double ms) {
        slow_name = t.name;
        (void)ms;
    });
    r.Post(PoolKind::kCpu, "heavy", [&] { clk.Advance(11); }); // 11ms > 10ms
    r.RunOne(PoolKind::kCpu);
    EXPECT_EQ(slow_name, "heavy");
    EXPECT_EQ(r.Stats("heavy")->slow_runs, 1u);
}

TEST(Reactor, IoTaskThresholdIs1s) {
    VirtualClock clk;
    Reactor r(clk);
    r.Post(PoolKind::kIo, "redis", [&] { clk.Advance(500); }); // 500ms 不算慢
    r.RunOne(PoolKind::kIo);
    EXPECT_EQ(r.Stats("redis")->slow_runs, 0u);
}
