/**
 * @file    test_loss_injector.cpp
 * @brief   M4.0 丢包注入器的契约测试
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 这些用例定的是**契约**, 不是实现细节: 它们不关心用了哪个哈希,
 *       只关心"同样输入同样输出""相邻 seq 不相关""换种子换一批"。
 *       换掉混合函数时这套用例应当照样全绿。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "modules/transport/LossInjector.h"

namespace {
    LossConfig cfg(uint32_t seed, int lossPercent) {
        LossConfig c;
        c.seed = seed;
        c.lossPercent = lossPercent;
        return c;
    }

    /** 收集 [0, n) 里被判定为丢弃的 seq */
    std::vector<uint32_t> droppedSeqs(const LossConfig& c, uint32_t n) {
        std::vector<uint32_t> out;
        for (uint32_t s = 0; s < n; ++s) {
            if (shouldDropPacket(c, s, PacketType::Data, false)) out.push_back(s);
        }
        return out;
    }
}  // namespace

// ---------- 开关 ----------

TEST(LossInjector, DisabledByDefault) {
    const LossConfig c;  // 什么都不给
    EXPECT_FALSE(lossInjectionEnabled(c)) << "测试工具的默认必须是不生效";
    for (uint32_t s = 0; s < 1000; ++s) {
        EXPECT_FALSE(shouldDropPacket(c, s, PacketType::Data, false));
    }
}

TEST(LossInjector, SentinelSeedDisablesInjectionEvenWithALossRate) {
    // 这个组合是**配置错误**, 调用方必须在 validateConfig 里挡下来。
    // 注入器本身的行为是"什么都不做" —— 危险的正是它太安静, 所以要有这条用例
    // 钉住行为, 免得以后有人把它改成"用默认种子", 让上层的校验形同虚设。
    const LossConfig c = cfg(LOSS_SEED_DISABLED, 50);
    EXPECT_FALSE(lossInjectionEnabled(c));
    EXPECT_EQ(droppedSeqs(c, 2000).size(), 0u);
}

TEST(LossInjector, ZeroPercentDropsNothing) {
    const LossConfig c = cfg(12345, 0);
    EXPECT_FALSE(lossInjectionEnabled(c));
    EXPECT_EQ(droppedSeqs(c, 2000).size(), 0u);
}

TEST(LossInjector, HundredPercentDropsEverything) {
    const LossConfig c = cfg(12345, 100);
    EXPECT_TRUE(lossInjectionEnabled(c));
    EXPECT_EQ(droppedSeqs(c, 2000).size(), 2000u);
}

// ---------- 纯函数 ----------

TEST(LossInjector, SameSeedAndSeqAlwaysGiveTheSameAnswer) {
    const LossConfig c = cfg(0xC0FFEE, 30);
    for (uint32_t s = 0; s < 500; ++s) {
        const bool first = shouldDropPacket(c, s, PacketType::Data, false);
        for (int repeat = 0; repeat < 5; ++repeat) {
            EXPECT_EQ(shouldDropPacket(c, s, PacketType::Data, false), first) << "seq=" << s;
        }
    }
}

/**
 * 这条是注入器存在的**全部意义**: 丢包序列和调用顺序无关。
 *
 * 反面写法(持有一个 mt19937, 每来一个包掷一次骰)会在这条上失败 ——
 * 而它恰好在你写出 NACK、重传包开始插队的那一刻才失败。
 */
TEST(LossInjector, TheAnswerDoesNotDependOnCallOrder) {
    const LossConfig c = cfg(777, 40);
    const std::vector<uint32_t> forward = droppedSeqs(c, 1000);

    std::set<uint32_t> backward;
    for (uint32_t s = 1000; s-- > 0;) {
        if (shouldDropPacket(c, s, PacketType::Data, false)) backward.insert(s);
    }

    // 中间穿插一堆无关的调用, 模拟重传包插队
    for (uint32_t s = 0; s < 5000; ++s) (void)shouldDropPacket(c, s * 7 + 3, PacketType::Data, false);

    const std::vector<uint32_t> again = droppedSeqs(c, 1000);
    EXPECT_EQ(again, forward) << "插了别的调用之后结果变了 —— 说明内部有状态";
    EXPECT_EQ(std::set<uint32_t>(forward.begin(), forward.end()), backward);
}

TEST(LossInjector, RetransmittedPacketsAreNeverDropped) {
    const LossConfig c = cfg(999, 100);  // 连 100% 都不许丢重传包
    for (uint32_t s = 0; s < 1000; ++s) {
        EXPECT_TRUE(shouldDropPacket(c, s, PacketType::Data, false)) << "原发包该丢, seq=" << s;
        EXPECT_FALSE(shouldDropPacket(c, s, PacketType::Data, true)) << "重传包不该丢, seq=" << s;
    }
}

// ---------- 分布 ----------

TEST(LossInjector, ActualRateIsCloseToTheConfiguredRate) {
    constexpr uint32_t N = 20000;
    for (int percent : {5, 10, 25, 50, 90}) {
        const size_t dropped = droppedSeqs(cfg(4242, percent), N).size();
        const double actual = 100.0 * static_cast<double>(dropped) / N;
        // 2 个百分点的容差: 这是分布检查, 不是精确计数, 别写成 EXPECT_EQ
        EXPECT_NEAR(actual, percent, 2.0) << "lossPercent=" << percent;
    }
}

/**
 * 相邻 seq 的结果必须不相关。
 *
 * `(seed + seq) % 100 < lossPercent` 这种线性写法会丢出**连续的一整段**,
 * 那不是随机丢包而是周期性突发丢包 —— FEC 恰好最怕这个(组内丢 >= 2 就救不回来),
 * 测出来的恢复率会莫名其妙地低, 而你会以为是 FEC 写错了。
 *
 * 判据: 10% 丢包率下, 连续丢 4 个及以上的段不该出现。
 * (独立同分布时单点概率 1e-4, 20000 个样本里期望约 2 段, 这里放到 6 段兜住抖动。)
 */
TEST(LossInjector, ConsecutiveSeqsAreNotCorrelated) {
    const LossConfig c = cfg(0xABCDEF, 10);
    size_t run = 0, longRuns = 0;
    for (uint32_t s = 0; s < 20000; ++s) {
        if (shouldDropPacket(c, s, PacketType::Data, false)) {
            if (++run >= 4) ++longRuns;
        } else {
            run = 0;
        }
    }
    EXPECT_LE(longRuns, 6u) << "丢包扎堆了 —— 混合函数对相邻 seq 不够打散";
}

TEST(LossInjector, DifferentSeedsDropADifferentSetOfPackets) {
    const std::vector<uint32_t> a = droppedSeqs(cfg(1, 20), 5000);
    const std::vector<uint32_t> b = droppedSeqs(cfg(2, 20), 5000);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());

    const std::set<uint32_t> sa(a.begin(), a.end());
    size_t overlap = 0;
    for (uint32_t s : b) {
        if (sa.count(s) != 0) ++overlap;
    }
    // 两个独立的 20% 集合, 期望重叠约 20%; 超过一半说明种子没起作用
    EXPECT_LT(overlap, b.size() / 2)
        << "换了种子丢的还是同一批包 —— seed 没有真正参与混合";
}

// ---------- 边界 ----------

TEST(LossInjector, SeqWrapAroundNeedsNoSpecialCase) {
    const LossConfig c = cfg(555, 30);
    // 回绕点两侧各取一段, 只要求它不崩、不退化成全丢或全不丢
    size_t dropped = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        const uint32_t seq = 0xFFFFFFFFu - 1000 + i;  // 故意跨过 0
        if (shouldDropPacket(c, seq, PacketType::Data, false)) ++dropped;
    }
    EXPECT_GT(dropped, 0u);
    EXPECT_LT(dropped, 2000u);
}
