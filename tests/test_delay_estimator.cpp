/**
 * @file    test_delay_estimator.cpp
 * @brief   M4.3 抖动水位估计: 滑动窗口分位数 + 非对称跟随
 * @author  zzj
 * @date    2026-09-01
 *
 * @note 这一层能被完整测住的原因是它**不取时间也不认识帧**: (timestampMs, nowMs)
 *          两个数进、playAt 出。想造什么样的抖动分布就喂什么样的数对,
 *          不用 sleep、不用真网络。
 *
 * @note **但回环上测不出它有没有用**: 本机 d ≈ 0, 一个正确的实现和一个
 *          "直接 return 下限"的假实现输出完全一样。端到端那一半要靠
 *          `tc netem delay 20ms 10ms`, 归 M4.5。这里测的是**算法本身**。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "modules/transport/DelayEstimator.h"

namespace {

    /** 自适应模式的基准配置: 关掉冷启动保护和上下界, 让分位数直接暴露出来 */
    DelayEstimatorConfig adaptiveCfg() {
        DelayEstimatorConfig cfg;
        cfg.adaptive = true;
        cfg.targetDelayMs = 50;
        cfg.windowMs = 1000;
        cfg.delayPercentile = 95;
        cfg.floorPercentile = 5;
        cfg.minDelayMs = 0;
        cfg.maxDelayMs = 10000;
        cfg.downRateMsPerSec = 0;  // 默认关掉降速, 要测的用例自己打开
        cfg.minSamples = 1;
        return cfg;
    }

    /**
     * 喂 n 帧稳定到达的样本。
     *
     * @param transitMs 每帧的传输耗时; 发送时刻 = t0 + i*periodMs, 到达 = 发送 + transitMs
     * @return 最后一帧的 playAt
     */
    uint64_t feedSteady(DelayEstimator& est, int n, uint64_t& sendMs, uint64_t& nowMs,
                        int periodMs, int transitMs) {
        uint64_t last = 0;
        for (int i = 0; i < n; ++i) {
            nowMs = sendMs + static_cast<uint64_t>(transitMs);
            last = est.observe(static_cast<uint32_t>(sendMs), nowMs);
            sendMs += static_cast<uint64_t>(periodMs);
        }
        return last;
    }

}  // namespace

// ---------------------------------------------------------------- 非自适应模式

/**
 * 关掉自适应时必须**逐字**等于 M3: playAt = ts + 历史最小 offset + 固定水位。
 *
 * 这不是"向后兼容"这种客套话 —— 它是 M4.5 报告的对照组。对照组的行为一变,
 * "自适应把 framesTooLate 从 X 降到 Y" 这句话就没有意义了。
 */
TEST(DelayEstimatorFixed, ReproducesTheM3FormulaExactly) {
    DelayEstimatorConfig cfg;
    cfg.adaptive = false;
    cfg.targetDelayMs = 50;
    DelayEstimator est(cfg);

    // 第一帧: 传输 20ms -> offset = 20
    EXPECT_EQ(est.observe(1000, 1020), 1000u + 20u + 50u);
    // 第二帧走得更慢(offset 30): 最小偏移不变, 还是 20
    EXPECT_EQ(est.observe(2000, 2030), 2000u + 20u + 50u);
    // 第三帧走得更快(offset 5): 最小偏移降到 5, 且**只减不增**
    EXPECT_EQ(est.observe(3000, 3005), 3000u + 5u + 50u);
    EXPECT_EQ(est.observe(4000, 4040), 4000u + 5u + 50u);

    EXPECT_EQ(est.stats().currentDelayMs, 50) << "非自适应模式下水位恒等于配置值";
}

/**
 * 非自适应模式下水位**一动不动**, 哪怕抖动大得离谱。
 *
 * 这条是上一条的反面: 它挡住"顺手让固定模式也稍微适应一下"这种改动。
 */
TEST(DelayEstimatorFixed, TheLevelNeverMovesNoMatterTheJitter) {
    DelayEstimatorConfig cfg;
    cfg.adaptive = false;
    cfg.targetDelayMs = 50;
    DelayEstimator est(cfg);

    for (int i = 0; i < 200; ++i) {
        const uint64_t send = 1000 + static_cast<uint64_t>(i) * 33;
        est.observe(static_cast<uint32_t>(send), send + (i % 2 ? 300 : 10));
    }
    EXPECT_EQ(est.stats().currentDelayMs, 50);
}

// ---------------------------------------------------------------- 分位数取法

/**
 * 分位数的取法必须是**定死的** index = p*(n-1)/100, 不插值。
 *
 * 定死才能写出确定的期望值。"大约是 p95" 这种断言等于没测。
 *
 * 构造: 101 个样本, offset 分别是 0..100(传输耗时 0..100ms)。
 *   floor = p5  -> index = 5*100/100 = 5   -> offset 5
 *   high  = p95 -> index = 95*100/100 = 95 -> offset 95
 *   水位 = 95 - 5 = 90
 */
TEST(DelayEstimatorPercentile, IndexIsExactlyPTimesNMinusOneOver100) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.windowMs = 1000000;  // 不淘汰
    DelayEstimator est(cfg);

    for (int i = 0; i <= 100; ++i) {
        const uint64_t send = 100000 + static_cast<uint64_t>(i) * 10;
        est.observe(static_cast<uint32_t>(send), send + static_cast<uint64_t>(i));
    }

    EXPECT_EQ(est.stats().windowSize, 101u);
    EXPECT_EQ(est.stats().floorOffsetMs, 5) << "floor = p5 = 第 6 小的 offset";
    EXPECT_EQ(est.stats().currentDelayMs, 90) << "水位 = p95 - p5 = 95 - 5";
}

/**
 * 单个样本时两个分位落在同一个元素上, 水位 = 0 —— 这正是 minSamples 存在的理由。
 */
TEST(DelayEstimatorPercentile, ASingleSampleGivesAZeroWidthEstimate) {
    DelayEstimator est(adaptiveCfg());
    est.observe(1000, 1020);
    EXPECT_EQ(est.stats().floorOffsetMs, 20);
    EXPECT_EQ(est.stats().rawDelayMs, 0);
}

// ---------------------------------------------------------------- 冷启动

/**
 * 样本不够 minSamples 时水位**保持初值不动**。
 *
 * 不挡的话前几帧的偶然抖动会把水位锚死一整个窗口。
 */
TEST(DelayEstimatorColdStart, HoldsTheInitialLevelUntilEnoughSamples) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minSamples = 30;
    cfg.targetDelayMs = 50;
    DelayEstimator est(cfg);

    // 前 29 帧里塞一个 500ms 的巨大抖动 —— 水位不许动
    for (int i = 0; i < 29; ++i) {
        const uint64_t send = 100000 + static_cast<uint64_t>(i) * 10;
        est.observe(static_cast<uint32_t>(send), send + (i == 3 ? 500 : 10));
    }
    EXPECT_EQ(est.stats().currentDelayMs, 50) << "样本不足时水位必须纹丝不动";
    EXPECT_EQ(est.stats().windowSize, 29u);
}

/**
 * 窗口缩回 minSamples 以下时, 水位**保持不动**, 不许跳回初值。
 *
 * 跳回初值是一次瞬时收窄, 绕过了降速限制 —— 而限速存在的全部理由就是
 * 不让水位突然变窄(收窄 X 毫秒 = 把后面的帧整体提前 X 毫秒放出去, 画面"抽"一下)。
 *
 * 构造: 先在高抖动下把水位抬起来, 然后让流停顿一整个窗口, 再来一帧。
 * 那一帧到达时窗口里只剩它自己, 走的就是样本不足这一支。
 */
TEST(DelayEstimatorColdStart, ShrinkingBackBelowMinSamplesHoldsTheLevel) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minSamples = 10;
    cfg.windowMs = 500;
    cfg.targetDelayMs = 20;
    cfg.downRateMsPerSec = 10;
    DelayEstimator est(cfg);

    // 一半快一半慢, 把水位抬到远高于初值
    uint64_t send = 100000;
    for (int i = 0; i < 40; ++i, send += 10) {
        est.observe(static_cast<uint32_t>(send), send + (i % 4 == 0 ? 300 : 10));
    }
    const int raised = est.stats().currentDelayMs;
    ASSERT_GT(raised, 100) << "构造没生效: 水位根本没被抬起来";

    // 停顿一整个窗口之后再来一帧 —— 窗口里只剩它自己
    send += 5000;
    est.observe(static_cast<uint32_t>(send), send + 10);

    ASSERT_LT(est.stats().windowSize, cfg.minSamples) << "构造没生效: 窗口没缩下去";
    EXPECT_EQ(est.stats().currentDelayMs, raised)
        << "样本不足时跳回了初值 " << cfg.targetDelayMs
        << " —— 这是一次绕过降速限制的瞬时收窄";
}

// ---------------------------------------------------------------- 非对称跟随

/**
 * 上调是**立刻**的: 一到分位数够格, 水位当帧就跳上去, 不受降速限制。
 *
 * 上调慢 = 帧到手就过期 = 掉帧, 不可逆; 所以这个方向不设任何闸门。
 *
 * @note 初值特意设成 0 —— 否则这条用例会和降速契约打架: 初值 50、降速 10ms/s、
 *          稳态段只跑了 0.99 秒, 水位最低只能降到 41, 而那正是
 *          ShrinksNoFasterThanTheConfiguredRate 要求的行为。
 *          这里要隔离的是**上调**那一支, 就不该让初值的衰减掺进来。
 */
TEST(DelayEstimatorAsymmetry, RaisesImmediatelyWithNoRateLimit) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.downRateMsPerSec = 10;
    cfg.windowMs = 1000000;
    cfg.targetDelayMs = 0;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 100, send, now, 10, 10);  // 全部 offset=10, 水位 0
    const int before = est.stats().currentDelayMs;
    EXPECT_EQ(before, 0);

    // 灌进一整批 200ms 的慢帧, 直到它们占满前 5%
    for (int i = 0; i < 100; ++i) {
        now = send + 200;
        est.observe(static_cast<uint32_t>(send), now);
        send += 10;
    }
    EXPECT_GT(est.stats().currentDelayMs, before);
    EXPECT_GT(est.stats().raises, 0u);
    EXPECT_EQ(est.stats().downRateLimited, 0u) << "上调不该走限速那一支";
}

/**
 * 下调受降速限制: 每秒最多缩 downRateMsPerSec 毫秒。
 *
 * 收窄水位等价于把后面的帧整体提前放出去 —— 观感是画面"抽"一下。
 * 限速把这个加速摊薄。
 */
TEST(DelayEstimatorAsymmetry, ShrinksNoFasterThanTheConfiguredRate) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.downRateMsPerSec = 10;
    cfg.minSamples = 1;
    cfg.windowMs = 100;  // 短窗口, 老样本很快滑出去
    cfg.targetDelayMs = 200;
    DelayEstimator est(cfg);

    // 一批完全没有抖动的样本 —— 原始估计立刻变成 0, 但水位只能慢慢下来
    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 10, send, now, 10, 10);

    EXPECT_EQ(est.stats().rawDelayMs, 0) << "原始估计应当已经到 0";
    EXPECT_GT(est.stats().currentDelayMs, 100)
        << "10ms/s 的降速下, 100 毫秒内最多只能缩 1ms";
    EXPECT_GT(est.stats().downRateLimited, 0u);
}

/**
 * 降速在**逐帧步进**下也必须真的降。
 *
 * 30fps 的 dt = 33ms, 而 `10 * 33 / 1000 = 0` —— 整数除法把每一帧的降速
 * 全抹成 0, 水位只涨不跌。先乘后除只是必要条件, **不充分**:
 * 真正让它工作的是把余数攒起来(downRateRemainder_), 攒够 1000 才降 1ms。
 *
 * 这个 bug 在 dt 很大的用例里完全看不出来 —— 一次喂 1 秒的间隔就正好绕过它。
 * 所以这条特意用一帧的步长, 而且要走够 300 步。
 */
TEST(DelayEstimatorAsymmetry, IntegerRateMathSurvivesOneFrameSteps) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.downRateMsPerSec = 10;
    cfg.minSamples = 1;
    cfg.windowMs = 50;
    cfg.targetDelayMs = 1000;
    DelayEstimator est(cfg);

    // 每步 33ms(一帧 @30fps), 走 300 步 = 9.9 秒 -> 应当缩掉约 99ms
    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 300, send, now, 33, 5);

    EXPECT_LT(est.stats().currentDelayMs, 1000)
        << "逐帧步进也必须能降 —— 不攒余数的话每帧都是 10*33/1000 = 0";
    EXPECT_GT(est.stats().currentDelayMs, 850)
        << "但也不能降过头: 9.9 秒 @10ms/s 只准缩约 99ms";
}

// ---------------------------------------------------------------- 窗口

/**
 * 老样本滑出窗口后不再影响估计 —— 这才叫"自适应"。
 *
 * 顺带还掉 M2 的账②: minOffset 只减不增。一个异常快的样本过期之后,
 * 基准必须能自己涨回来。
 */
TEST(DelayEstimatorWindow, TheFloorRecoversAfterAFastOutlierExpires) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.windowMs = 500;
    cfg.floorPercentile = 0;  // 取最小, 把"只减不增"的问题放到最大
    DelayEstimator est(cfg);

    // 一个走得飞快的帧(offset = 1)
    est.observe(100000, 100001);
    EXPECT_EQ(est.stats().floorOffsetMs, 1);

    // 之后全是 offset=50 的正常帧, 且时间推过一整个窗口
    uint64_t send = 100010;
    uint64_t now = 0;
    feedSteady(est, 100, send, now, 10, 50);

    EXPECT_EQ(est.stats().floorOffsetMs, 50)
        << "快样本滑出窗口后基准必须涨回来 —— 这正是 M3 做不到的那件事";
}

/**
 * 窗口按 nowMs 淘汰, 且淘汰要计数。
 */
TEST(DelayEstimatorWindow, EvictsByArrivalTimeAndCounts) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.windowMs = 100;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 50, send, now, 10, 5);

    EXPECT_LE(est.stats().windowSize, 11u) << "100ms 窗口 / 10ms 步进最多 11 个";
    EXPECT_GT(est.stats().samplesEvicted, 0u);
    EXPECT_EQ(est.stats().samplesSeen, 50u);
}

/**
 * nowMs 回退不能把窗口算成天文数字年龄。
 *
 * 单调时钟理论上不回退, 但 nowMs 是**调用方传进来的** —— 传什么都合法。
 * 无符号减法一绕, 整个窗口会被一次清空, 水位无声地冷启动。
 * RetransmitCache::evictExpired 已经为同一条踩过一次。
 *
 * @note 回退量必须跨过**队头**才测得到: 淘汰是从最老的一端扫、遇到够年轻的就停,
 *          所以只往回挪几毫秒的话, 队头还年轻, 循环第一轮就 break 了 ——
 *          会绕的那个减法根本执行不到。第一版这条用例就是这么写的, 变异测试
 *          (把 `atMs > nowMs` 那半个条件删掉)照样全绿。
 */
TEST(DelayEstimatorWindow, ABackwardNowDoesNotWipeTheWindow) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.windowMs = 100000;
    DelayEstimator est(cfg);

    const uint64_t firstArrival = 100005;  // feedSteady 的第一个样本落在这儿
    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 50, send, now, 10, 5);
    const size_t before = est.stats().windowSize;
    ASSERT_EQ(before, 50u);
    ASSERT_GT(now, firstArrival);

    // 倒退到比队头还早 —— 不设防的话 nowMs - atMs 会绕成天文数字, 一轮清空整个窗口
    est.observe(static_cast<uint32_t>(send), firstArrival - 1);
    EXPECT_GE(est.stats().windowSize, before)
        << "回退的 nowMs 不该触发一次全窗口淘汰";
    EXPECT_EQ(est.stats().samplesEvicted, 0u);
}

// ---------------------------------------------------------------- 夹取

/**
 * 上界必须夹得住一次 timestampMs 回绕。
 *
 * timestampMs 是 32 位毫秒, 和 seq 一样**每个取值都合法**(NOTES D22)。
 * 分位数扛得住少量脏样本, 但 2^32 量级的坏值**保证**落在最大的 5% 里。
 * 没有上界, 一次回绕就是几分钟的延迟, 而且所有丢帧计数器都好看。
 */
TEST(DelayEstimatorClamp, AWrappedTimestampCannotBlowPastMaxDelay) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.maxDelayMs = 500;
    cfg.minSamples = 1;
    cfg.windowMs = 1000000;
    DelayEstimator est(cfg);

    // 20 个正常样本 + 20 个 timestampMs 已经回绕的样本(offset 变成负的天文数字,
    // 于是 p95 - p5 变成天文数字)
    uint64_t send = 3000000;
    for (int i = 0; i < 20; ++i, send += 10) {
        est.observe(static_cast<uint32_t>(send), send + 10);
    }
    for (int i = 0; i < 20; ++i, send += 10) {
        est.observe(static_cast<uint32_t>(send + 4000000000u), send + 10);
    }

    EXPECT_LE(est.stats().currentDelayMs, 500);
    EXPECT_GT(est.stats().clampedHigh, 0u) << "夹住了就要计数, 否则这种事静默发生";
}

/**
 * 下界挡住"链路太顺 -> 水位收到 0 -> 一乱序就掉帧"。
 */
TEST(DelayEstimatorClamp, APerfectLinkStillKeepsTheMinimumLevel) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 10;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 100, send, now, 10, 7);  // 零抖动

    EXPECT_EQ(est.stats().currentDelayMs, 10);
    EXPECT_EQ(est.stats().rawDelayMs, 0) << "rawDelayMs 记的是夹取之前的原始估计";
    EXPECT_GT(est.stats().clampedLow, 0u);
}

/**
 * 上下界写反了不能是 UB —— 构造时归一化。
 *
 * std::clamp(v, lo, hi) 在 lo > hi 时是未定义行为, 而这两个数来自命令行。
 */
TEST(DelayEstimatorClamp, SwappedBoundsAreNormalizedAtConstruction) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 400;
    cfg.maxDelayMs = 20;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 50, send, now, 10, 5);

    EXPECT_GE(est.stats().currentDelayMs, 20);
    EXPECT_LE(est.stats().currentDelayMs, 400);
}

// ---------------------------------------------------------------- playAt

/**
 * playAt = ts + floor + 水位, 且**永不返回负数绕成的天文数字**。
 *
 * playAt 是无符号的; 让它绕一圈等于"这一帧要等 49 天",
 * 而 msUntilNextDue 会把 poll 超时顶到 INT_MAX —— 整个接收端挂死。
 */
TEST(DelayEstimatorPlayAt, NeverWrapsWhenTheOffsetIsVeryNegative) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    // 对端时钟远远超前本机: nowMs 很小而 timestampMs 很大 -> offset 是大负数
    const uint64_t playAt = est.observe(4000000000u, 1000);
    EXPECT_LT(playAt, 1000000u) << "负的 playAt 必须夹成 0, 不能绕成天文数字";
}

/**
 * 同一帧的 playAt 在自适应模式下也必须只算一次 —— 由 JitterBuffer 保证,
 * 这里只钉住估计器这一侧: 相同输入、相同状态, 输出可复现。
 */
TEST(DelayEstimatorPlayAt, IsDeterministicForTheSameSampleStream) {
    DelayEstimator a(adaptiveCfg());
    DelayEstimator b(adaptiveCfg());

    std::vector<uint64_t> outA, outB;
    for (int i = 0; i < 200; ++i) {
        const uint64_t send = 100000 + static_cast<uint64_t>(i) * 10;
        const uint64_t now = send + static_cast<uint64_t>((i * 37) % 60);
        outA.push_back(a.observe(static_cast<uint32_t>(send), now));
        outB.push_back(b.observe(static_cast<uint32_t>(send), now));
    }
    EXPECT_EQ(outA, outB);
}

// ---------------------------------------------------------------- reset

/**
 * reset() 清干净, 包括计数器, 并把水位放回初值。
 */
TEST(DelayEstimatorReset, ReturnsToTheFreshlyConstructedState) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.targetDelayMs = 50;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 100, send, now, 10, 80);
    ASSERT_GT(est.stats().samplesSeen, 0u);

    est.reset();
    EXPECT_EQ(est.stats().samplesSeen, 0u);
    EXPECT_EQ(est.stats().windowSize, 0u);
    EXPECT_EQ(est.stats().currentDelayMs, 50);
    EXPECT_EQ(est.stats().floorOffsetMs, 0);
}

// ---------------------------------------------------------------- 响应时间

/**
 * 上调响应时间 ≈ (100 - delayPercentile)% × windowMs。
 *
 * 这条把头文件里那句推导钉成可执行的断言 —— 它是选窗口和分位数的**唯一依据**,
 * 注释里写着而代码不满足的话, 那段推导就是装饰。
 *
 * 构造: 窗口 1000ms, p95 -> 5% × 1000ms = 50ms。10ms 一帧, 即 5 帧内跟上。
 */
TEST(DelayEstimatorResponse, RisesWithinTheDocumentedWindowFraction) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.windowMs = 1000;
    cfg.delayPercentile = 95;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 100, send, now, 10, 10);  // 稳态: 100 个样本, 零抖动
    ASSERT_EQ(est.stats().currentDelayMs, 0);

    // 网络阶跃变差 100ms, 只喂 6 帧(= 60ms, 略多于 5% 窗口)
    for (int i = 0; i < 6; ++i, send += 10) {
        est.observe(static_cast<uint32_t>(send), send + 110);
    }
    EXPECT_GE(est.stats().currentDelayMs, 90)
        << "5% 的窗口时间内水位就该跟上, 这是选 windowMs 的依据";
}


// ---------------------------------------------------------------- M4.6 重传预算下限

/**
 * 帧周期是量出来的, 不是配的。
 *
 * 33ms 一帧喂进去, 帧周期就该是 33 —— 它是水位下限的一半原料。
 */
TEST(DelayEstimatorBudget, MeasuresTheFrameIntervalFromTimestamps) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 60, send, now, 33, 5);

    EXPECT_NEAR(est.stats().frameIntervalMs, 33, 1);
}

/**
 * 帧周期用**中位数**, 一次乱序歪不了它。
 *
 * 乱序时相邻两帧的时间戳差会变成 0 或两倍, 用均值会被拖偏。
 */
TEST(DelayEstimatorBudget, TheFrameIntervalSurvivesReordering) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    DelayEstimator est(cfg);

    // 33ms 一帧, 但每 5 帧把相邻两帧调个个儿
    uint64_t base = 100000;
    for (int i = 0; i < 60; ++i) {
        int j = (i % 5 == 3) ? i + 1 : (i % 5 == 4) ? i - 1 : i;
        const uint64_t ts = base + static_cast<uint64_t>(j) * 33;
        est.observe(static_cast<uint32_t>(ts), base + static_cast<uint64_t>(i) * 33 + 5);
    }
    EXPECT_NEAR(est.stats().frameIntervalMs, 33, 4);
}

/**
 * timestampMs 回绕不能把帧周期顶到天上。
 *
 * @note 这条测的是**结果**, 不是某一层实现。挡住它的其实是中位数:
 *          一次回绕只污染一个样本, 15 个样本的中位数不动。
 *          变异验证确认过 —— 把 int32 做差换成 int64, 这条照样全绿。
 *          int32 那一层是把回绕计数器的语义写对, 不是这条的防线。
 *          最初的设计注释把功劳记反了, 留着这条注释免得下次又反过来。
 */
TEST(DelayEstimatorBudget, AWrappedTimestampDoesNotBlowTheFrameInterval) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    DelayEstimator est(cfg);

    // 跨过 2^32 边界: ...FFFFFF9C, FFFFFFBD, ..., 然后回绕到 0x0000000E
    uint32_t ts = 0xFFFFFF00u;
    for (int i = 0; i < 40; ++i, ts += 33) {
        est.observe(ts, 100000 + static_cast<uint64_t>(i) * 33);
    }
    EXPECT_NEAR(est.stats().frameIntervalMs, 33, 2)
        << "回绕点用无符号减法, 一个样本就能把帧周期顶到十亿";
}

/**
 * 给了 RTT 之后, 水位下限抬到 帧周期 + RTT。
 *
 * 这是这一层存在的理由: 一帧缺片时补齐它要 检测延迟(一个帧周期) + RTT,
 * 水位低于这个和, 重传回来必然已经过了 playAt —— 白跑一趟带宽,
 * 而丢的那一帧会连累到下一个 IDR 为止的所有帧。
 */
TEST(DelayEstimatorBudget, TheFloorRisesToOneFramePlusRtt) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 5;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    est.setRttMs(50);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 60, send, now, 33, 5);  // 零抖动 -> 原始估计是 0

    EXPECT_EQ(est.stats().rawDelayMs, 0) << "链路是干净的, 原始估计该是 0";
    EXPECT_NEAR(est.stats().currentDelayMs, 33 + 50, 2)
        << "水位应当被重传预算托住, 而不是掉到 minDelayMs";
    EXPECT_TRUE(est.stats().floorFromBudget);
    EXPECT_NEAR(est.stats().effectiveMinDelayMs, 33 + 50, 2);
}

/**
 * 没给 RTT 时下限就是 minDelayMs —— 预算不许凭空出现。
 */
TEST(DelayEstimatorBudget, WithoutAnRttMeasurementTheFloorIsJustMinDelay) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 5;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 60, send, now, 33, 5);

    EXPECT_EQ(est.stats().currentDelayMs, 5);
    EXPECT_FALSE(est.stats().floorFromBudget);
}

/**
 * 预算再大也不许突破 maxDelayMs。
 *
 * RTT 一旦测歪(比如把一次超时重传当成正常往返), 预算能算出几秒。
 * 而 maxDelayMs 是**产品定义**的上限——过 500ms README 第一行就不成立,
 * 不能被一个测量值突破。
 */
TEST(DelayEstimatorBudget, TheBudgetCannotBreakThroughMaxDelay) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 5;
    cfg.maxDelayMs = 200;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    est.setRttMs(5000);  // 测歪了

    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 60, send, now, 33, 5);

    EXPECT_LE(est.stats().currentDelayMs, 200);
}

/**
 * setRttMs(0) 表示"还不知道", 不该把已经量到的 RTT 抹掉。
 */
TEST(DelayEstimatorBudget, ANonPositiveRttIsIgnoredNotStored) {
    DelayEstimatorConfig cfg = adaptiveCfg();
    cfg.minDelayMs = 5;
    cfg.minSamples = 1;
    DelayEstimator est(cfg);

    est.setRttMs(40);
    uint64_t send = 100000;
    uint64_t now = 0;
    feedSteady(est, 40, send, now, 33, 5);
    const int withRtt = est.stats().currentDelayMs;
    ASSERT_GT(withRtt, 5);

    est.setRttMs(0);
    est.setRttMs(-1);
    feedSteady(est, 40, send, now, 33, 5);
    EXPECT_EQ(est.stats().currentDelayMs, withRtt) << "0 / 负数是'不知道', 不是'清零'";
}
