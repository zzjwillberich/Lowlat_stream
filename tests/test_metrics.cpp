/**
 * @file    test_metrics.cpp
 * @brief   M3.4 LatencyRecorder: 百分位取值、边界、增量 max、排序后仍可继续累积
 * @author  zzj
 * @date    2026-08-24
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "common/Metrics.h"

namespace {

// 把 1..n 按给定顺序喂进去。默认顺序无所谓 —— percentile 自己会排,
// 所以这些用例特意用**乱序**输入, 顺带证明它真的排了
void feed(LatencyRecorder& r, const std::vector<uint32_t>& samples) {
    for (uint32_t s : samples) r.add(s);
}

std::vector<uint32_t> oneToN(uint32_t n) {
    std::vector<uint32_t> v(n);
    std::iota(v.begin(), v.end(), 1u);
    return v;
}

}  // namespace

TEST(LatencyRecorder, StartsEmpty) {
    LatencyRecorder r;
    EXPECT_EQ(r.count(), 0u);
    EXPECT_EQ(r.max(), 0u);
}

TEST(LatencyRecorder, PercentileOnAnEmptyRecorderDoesNotCrash) {
    LatencyRecorder r;
    // 返回值无意义(实现返回 0), 这里断言的是**不越界、不除零**。
    // 程序刚起来还没收到帧就被 Ctrl-C, 退出那行总账一定会调到它。
    EXPECT_EQ(r.percentile(50), 0u);
    EXPECT_EQ(r.percentile(0), 0u);
    EXPECT_EQ(r.percentile(100), 0u);
    EXPECT_EQ(r.count(), 0u);
}

TEST(LatencyRecorder, PercentileOfOneToHundred) {
    LatencyRecorder r;
    std::vector<uint32_t> v = oneToN(100);
    std::reverse(v.begin(), v.end());  // 倒序喂, 结果必须一样
    feed(r, v);

    ASSERT_EQ(r.count(), 100u);
    EXPECT_EQ(r.percentile(50), 50u);
    EXPECT_EQ(r.percentile(95), 95u);
    EXPECT_EQ(r.percentile(99), 99u);
    // p100 就是最大值, p0 夹到最小值
    EXPECT_EQ(r.percentile(100), 100u);
    EXPECT_EQ(r.percentile(0), 1u);
}

TEST(LatencyRecorder, PercentileWhenTheCountIsNotAMultipleOfHundred) {
    // rank 公式最容易差一格的地方: N 不是 100 的整数倍。
    // nearest-rank 定义是"第 ceil(p/100 * N) 个(1-based)"
    LatencyRecorder seven;
    feed(seven, {4, 1, 7, 3, 6, 2, 5});
    EXPECT_EQ(seven.percentile(50), 4u) << "7 个数的中位数是第 4 个";
    EXPECT_EQ(seven.percentile(100), 7u);
    EXPECT_EQ(seven.percentile(0), 1u);

    LatencyRecorder three;
    feed(three, {30, 10, 20});
    EXPECT_EQ(three.percentile(50), 20u);  // ceil(1.5) = 2 -> 第 2 个
    EXPECT_EQ(three.percentile(99), 30u);  // ceil(2.97) = 3 -> 第 3 个
    EXPECT_EQ(three.percentile(0), 10u);

    LatencyRecorder one;
    one.add(42);
    // 只有一个样本时, 任何百分位都只能是它
    EXPECT_EQ(one.percentile(0), 42u);
    EXPECT_EQ(one.percentile(50), 42u);
    EXPECT_EQ(one.percentile(100), 42u);
}

TEST(LatencyRecorder, PercentileClampsOutOfRangeArguments) {
    LatencyRecorder r;
    feed(r, oneToN(100));
    // 夹住而不是越界: p 是个 int, 调用方笔误传 99.9 * 100 之类的东西不该炸
    EXPECT_EQ(r.percentile(-5), r.percentile(0));
    EXPECT_EQ(r.percentile(200), r.percentile(100));
}

TEST(LatencyRecorder, MaxIsMaintainedIncrementallyNotBySorting) {
    LatencyRecorder r;
    feed(r, {10, 900, 30, 20});
    // **一次 percentile 都没调** —— max 不该依赖排序, 它在 add() 里就该是对的。
    // 这也是为什么它能是 const 且 O(1)
    EXPECT_EQ(r.max(), 900u);
    EXPECT_EQ(r.count(), 4u);
}

TEST(LatencyRecorder, MaxAgreesWithTheHundredthPercentile) {
    LatencyRecorder r;
    feed(r, {5, 4000, 12, 7, 33});
    EXPECT_EQ(r.max(), r.percentile(100));
    // 但它们回答的是两个问题: p99 说"除了最倒霉的 1% 之外有多快",
    // max 说"最惨的那一次有多惨"。一次卡顿能把 max 拉满而 p99 纹丝不动
    EXPECT_EQ(r.percentile(50), 12u);
}

TEST(LatencyRecorder, ASingleOutlierDoesNotMoveThePercentiles) {
    // 百分位是**按排名**取的, 所以对少数离群值免疫 —— 这正是不报 mean 的理由:
    // 99 个 20ms + 1 个 2000ms, 平均值是 39.8ms, 一个从来没发生过的延迟
    LatencyRecorder r;
    for (int i = 0; i < 99; ++i) r.add(20);
    r.add(2000);

    EXPECT_EQ(r.percentile(50), 20u);
    EXPECT_EQ(r.percentile(99), 20u);
    EXPECT_EQ(r.max(), 2000u) << "但 max 必须如实报出那一次卡顿";
}

TEST(LatencyRecorder, KeepsAccumulatingAfterPercentileHasSortedTheSamples) {
    LatencyRecorder r;
    feed(r, {30, 10, 20});
    ASSERT_EQ(r.percentile(50), 20u);  // 这一步把容器排序了

    // 排完之后再追加一个乱序元素。实现里但凡有"已排过就不再排"的偷懒缓存,
    // 这里就会红 —— 这个用例锁的是"以后也不许偷懒"
    r.add(5);
    EXPECT_EQ(r.count(), 4u);
    EXPECT_EQ(r.percentile(50), 10u) << "4 个数 {5,10,20,30} 的 p50 是第 2 个";
    EXPECT_EQ(r.percentile(100), 30u);
    EXPECT_EQ(r.max(), 30u);
}

TEST(LatencyRecorder, PercentileIsRepeatable) {
    LatencyRecorder r;
    feed(r, oneToN(100));
    // 连着取几次(退出时那行总账就是这么干的), 每次都得一样。
    // percentile 会改动容器, 所以"调用顺序影响结果"是个真实的失败模式
    EXPECT_EQ(r.percentile(99), 99u);
    EXPECT_EQ(r.percentile(50), 50u);
    EXPECT_EQ(r.percentile(99), 99u);
    EXPECT_EQ(r.percentile(95), 95u);
}

TEST(LatencyRecorder, ResetClearsEverythingAndTheRecorderStaysUsable) {
    LatencyRecorder r;
    feed(r, {10, 900, 30});
    ASSERT_EQ(r.count(), 3u);

    r.reset();
    EXPECT_EQ(r.count(), 0u);
    EXPECT_EQ(r.max(), 0u) << "max 也要归零, 否则上一轮的峰值会漏到下一轮";
    EXPECT_EQ(r.percentile(50), 0u);

    // reset 之后还能继续用 —— "最近一段"那个记录器每秒 reset 一次就是这么跑的
    feed(r, {7, 3});
    EXPECT_EQ(r.count(), 2u);
    EXPECT_EQ(r.max(), 7u);
    EXPECT_EQ(r.percentile(50), 3u);
}
