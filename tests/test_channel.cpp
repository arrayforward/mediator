// CSP Channel：值语义深拷贝、批取、关闭语义
#include <gtest/gtest.h>

#include <thread>

#include "core/channel.h"

using mediator::CopyChannel;

TEST(CopyChannel, DeepCopyOnSend) {
    CopyChannel<std::vector<int>> ch;
    std::vector<int> v{1, 2, 3};
    ch.Send(v);
    v.push_back(99); // 修改原对象不影响队内副本
    std::vector<int> out;
    ASSERT_TRUE(ch.TryRecv(out));
    EXPECT_EQ(out.size(), 3u);
}

TEST(CopyChannel, SwapOutAllDrainsQueue) {
    CopyChannel<int> ch;
    for (int i = 0; i < 5; ++i) ch.Send(i);
    std::vector<int> all;
    EXPECT_EQ(ch.SwapOutAll(all), 5u);
    EXPECT_EQ(ch.Depth(), 0u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(all[i], i);
}

TEST(CopyChannel, BlockingRecvUnblocksOnClose) {
    CopyChannel<int> ch;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ch.Close();
    });
    int v = 0;
    EXPECT_FALSE(ch.Recv(v)); // 关闭且空 → false
    t.join();
}
