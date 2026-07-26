/**
 * @file    test_bounded_queue.cpp
 * @brief   BoundedQueue 单元测试 — 覆盖 M0.6 要求的全部用例
 * @author  zzj
 * @date    2026-07-26
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "common/BoundedQueue.h"

using namespace std::chrono_literals;

namespace {
    /**
     * 判定"确实阻塞了"用的观察窗口。
     *
     * 这个方向是安全的: 机器再慢也只会让它更容易 timeout, 不会假失败。
     * 真出现 ready 只有一种解释 —— 本该阻塞的调用返回了, 那就是 bug。
     */
    constexpr auto BLOCK_WINDOW = 100ms;

    /**
     * 判定"被成功唤醒"用的上限, 给得宽松些避免机器负载高时假失败。
     *
     * 一旦超时说明线程没被唤醒(死锁), 此时 future 的析构函数会永久阻塞,
     * 整个测试进程挂死 —— 兜底靠 CMake 里给测试目标设的 TIMEOUT。
     */
    constexpr auto WAKE_TIMEOUT = 2s;
}

// ========== 基本 FIFO ==========

TEST(BoundedQueue, FifoOrder) {
    BoundedQueue<int> q(4);
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));
    ASSERT_TRUE(q.push(3));

    int v = 0;
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 3);
    EXPECT_EQ(q.size(), 0u);
}

// ========== 队满时 push 阻塞 ==========

TEST(BoundedQueue, PushBlocksWhenFull) {
    BoundedQueue<int> q(2);
    ASSERT_TRUE(q.push(10));
    ASSERT_TRUE(q.push(20));

    // 容量 2 已满, 第 3 个 push 必须卡住
    auto fut = std::async(std::launch::async, [&]{ return q.push(30); });
    EXPECT_EQ(fut.wait_for(BLOCK_WINDOW), std::future_status::timeout);

    // 腾出一个位置, 阻塞的 push 才应该返回
    int v = 0;
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 10);

    ASSERT_EQ(fut.wait_for(WAKE_TIMEOUT), std::future_status::ready);
    EXPECT_TRUE(fut.get());
    EXPECT_EQ(q.size(), 2u);   // 20 和 30
}

// ========== 队空时 pop 阻塞 ==========

TEST(BoundedQueue, PopBlocksWhenEmpty) {
    BoundedQueue<int> q(2);

    auto fut = std::async(std::launch::async, [&]{
        int x = 0;
        return q.pop(x) ? x : -1;
    });
    EXPECT_EQ(fut.wait_for(BLOCK_WINDOW), std::future_status::timeout);

    ASSERT_TRUE(q.push(7));

    ASSERT_EQ(fut.wait_for(WAKE_TIMEOUT), std::future_status::ready);
    EXPECT_EQ(fut.get(), 7);
}

// ========== close 唤醒阻塞中的 push ==========

TEST(BoundedQueue, CloseWakesBlockedPush) {
    BoundedQueue<int> q(2);
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));

    auto fut = std::async(std::launch::async, [&]{ return q.push(3); });
    EXPECT_EQ(fut.wait_for(BLOCK_WINDOW), std::future_status::timeout);

    q.close();

    // 被唤醒并返回 false, 而不是继续睡 —— 谓词里少写 || closed_ 就死在这
    ASSERT_EQ(fut.wait_for(WAKE_TIMEOUT), std::future_status::ready);
    EXPECT_FALSE(fut.get());
    EXPECT_EQ(q.size(), 2u);   // 3 没有被塞进来
}

// ========== close 唤醒阻塞中的 pop ==========

TEST(BoundedQueue, CloseWakesBlockedPop) {
    BoundedQueue<int> q(2);

    auto fut = std::async(std::launch::async, [&]{
        int x = 0;
        return q.pop(x);
    });
    EXPECT_EQ(fut.wait_for(BLOCK_WINDOW), std::future_status::timeout);

    q.close();

    ASSERT_EQ(fut.wait_for(WAKE_TIMEOUT), std::future_status::ready);
    EXPECT_FALSE(fut.get());
}

// ========== close 后残留数据仍能取完 ==========

TEST(BoundedQueue, CloseDrainsRemaining) {
    BoundedQueue<int> q(4);
    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));

    q.close();

    // pop 判的是"空不空"而不是"关没关", 残留必须还能取出来
    int v = 0;
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.pop(v));    // 取空之后才返回 false

    // push 则是一关就停, 与 pop 不对称
    EXPECT_FALSE(q.push(3));
}

TEST(BoundedQueue, CloseIsIdempotent) {
    BoundedQueue<int> q(2);
    q.close();
    q.close();                 // 重复调用不应崩溃或死锁

    int v = 0;
    EXPECT_FALSE(q.pop(v));
    EXPECT_FALSE(q.push(1));
}

// ========== tryPush 不阻塞 ==========

TEST(BoundedQueue, TryPushFailsInsteadOfBlocking) {
    BoundedQueue<int> q(2);
    ASSERT_TRUE(q.tryPush(1));
    ASSERT_TRUE(q.tryPush(2));

    // 满了直接失败, 不阻塞 —— 整个用例跑在测试线程上, 会卡住就说明实现错了
    EXPECT_FALSE(q.tryPush(3));

    int v = 0;
    ASSERT_TRUE(q.pop(v));
    EXPECT_TRUE(q.tryPush(3));

    q.close();
    EXPECT_FALSE(q.tryPush(4));
}

// ========== peak 水位 ==========

TEST(BoundedQueue, PeakTracksHighWaterMark) {
    BoundedQueue<int> q(4);
    EXPECT_EQ(q.peak(), 0u);

    ASSERT_TRUE(q.push(1));
    ASSERT_TRUE(q.push(2));
    ASSERT_TRUE(q.push(3));
    EXPECT_EQ(q.peak(), 3u);

    int v = 0;
    ASSERT_TRUE(q.pop(v));
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(q.size(), 1u);
    EXPECT_EQ(q.peak(), 3u);   // 出队不降低历史水位

    ASSERT_TRUE(q.push(4));
    EXPECT_EQ(q.peak(), 3u);   // 没超过历史值就不更新
    ASSERT_TRUE(q.push(5));
    ASSERT_TRUE(q.push(6));
    EXPECT_EQ(q.peak(), 4u);
}

// ========== move-only 类型 ==========

TEST(BoundedQueue, SupportsMoveOnlyType) {
    BoundedQueue<std::unique_ptr<int>> q(2);

    ASSERT_TRUE(q.push(std::make_unique<int>(42)));
    ASSERT_TRUE(q.push(std::make_unique<int>(43)));

    std::unique_ptr<int> p;
    ASSERT_TRUE(q.pop(p));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);

    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(*p, 43);
}

TEST(BoundedQueue, MoveOnlyItemIsConsumedOnFailedPush) {
    BoundedQueue<std::unique_ptr<int>> q(1);
    q.close();

    auto p = std::make_unique<int>(1);
    EXPECT_FALSE(q.push(std::move(p)));

    // 契约: push 失败时 item 已被消耗, 调用方拿不回来。
    // 这不是缺陷而是明确约定, 用例把它固定下来防止以后被"顺手改掉"
    EXPECT_EQ(p, nullptr);
}

// ========== 多生产者多消费者: 不丢不重 ==========

TEST(BoundedQueue, MultiProducerMultiConsumerConservesItems) {
    constexpr int PRODUCERS = 3;
    constexpr int CONSUMERS = 3;
    constexpr int PER_PRODUCER = 2000;

    BoundedQueue<int> q(16);            // 故意开小, 逼出满/空两种阻塞
    std::atomic<long long> sum{0};
    std::atomic<int> popped{0};

    std::vector<std::thread> consumers;
    for(int c = 0; c < CONSUMERS; ++c){
        consumers.emplace_back([&]{
            int v = 0;
            while(q.pop(v)){
                sum += v;
                popped++;
            }
        });
    }

    std::vector<std::thread> producers;
    for(int p = 0; p < PRODUCERS; ++p){
        producers.emplace_back([&, p]{
            for(int i = 0; i < PER_PRODUCER; ++i){
                ASSERT_TRUE(q.push(p * PER_PRODUCER + i));
            }
        });
    }

    for(auto& t : producers) t.join();
    q.close();                          // 生产者收工后关闭, 消费者取空即退出
    for(auto& t : consumers) t.join();

    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;
    EXPECT_EQ(popped.load(), TOTAL);
    // 每个元素恰好被取走一次: 求和等于 0+1+...+(TOTAL-1)
    EXPECT_EQ(sum.load(), static_cast<long long>(TOTAL) * (TOTAL - 1) / 2);
    EXPECT_LE(q.peak(), 16u);           // 水位不可能超过容量
}
