/**
 * @file    test_jitter_buffer.cpp
 * @brief   抖动缓冲的契约测试: 排序、按时放行、起播门、容量上限
 * @author  zzj
 * @date    2026-08-18
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "modules/transport/JitterBuffer.h"

namespace {
    /**
     * 造一帧。载荷用 frameId 填满, pop 出来一比就知道有没有串帧。
     *
     * 时间戳与帧号解耦地传进来: 这两者在真实链路上是独立变量,
     * 测试里绑死就测不出"按时间排期"和"按帧号排序"哪个错了。
     */
    AssembledFrame makeFrame(uint32_t frameId, uint32_t timestampMs, bool isKey = false) {
        AssembledFrame frame;
        frame.frameId = frameId;
        frame.timestampMs = timestampMs;
        frame.isKey = isKey;
        frame.data.assign(8, static_cast<uint8_t>(frameId));
        return frame;
    }

    /** 不设起播门的配置, 用于专注测排序/时序的用例 */
    JitterBufferConfig noKeyGate(int targetDelayMs = 0) {
        JitterBufferConfig cfg;
        cfg.startOnKeyFrame = false;
        cfg.delay.targetDelayMs = targetDelayMs;
        return cfg;
    }

    /** pop 一帧并返回它的 frameId, 没取到返回 -1 */
    int64_t popId(JitterBuffer& jb, uint64_t nowMs) {
        AssembledFrame frame;
        if (!jb.pop(frame, nowMs)) return -1;
        return static_cast<int64_t>(frame.frameId);
    }
}  // namespace

// ---------- 空缓冲 ----------

TEST(JitterBuffer, EmptyBufferHasNothingToPop) {
    JitterBuffer jb;
    AssembledFrame frame;
    EXPECT_FALSE(jb.pop(frame, 1000));
    EXPECT_EQ(jb.size(), 0u);
}

TEST(JitterBuffer, EmptyBufferReportsNoDueTime) {
    JitterBuffer jb;
    // -1 而不是 0: 收包线程要靠它区分"没有待播帧, 可以安心等满 recvTimeout"
    // 和"有帧已经过期了, 赶紧回来放行"
    EXPECT_EQ(jb.msUntilNextDue(1000), -1);
}

// ---------- 起播门 ----------

TEST(JitterBuffer, DropsEverythingBeforeTheFirstKeyFrame) {
    JitterBuffer jb;  // 默认 startOnKeyFrame = true

    for (uint32_t i = 1; i <= 5; ++i) {
        jb.push(makeFrame(i, 1000 + 33 * i, /*isKey=*/false), 5000);
    }

    // 解码器拿到非 IDR 开头的码流只会吐 no frame! 然后花屏, 不如一帧都不给它
    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesBeforeKey, 5u);
    EXPECT_EQ(jb.stats().framesIn, 5u);
}

TEST(JitterBuffer, StartsPlayingFromTheFirstKeyFrame) {
    JitterBuffer jb;

    jb.push(makeFrame(1, 1000, /*isKey=*/false), 5000);
    jb.push(makeFrame(2, 1033, /*isKey=*/true), 5033);
    jb.push(makeFrame(3, 1066, /*isKey=*/false), 5066);

    EXPECT_EQ(jb.stats().framesBeforeKey, 1u);
    EXPECT_EQ(jb.size(), 2u);

    // 起播之后非关键帧照收不误 —— 门只在起播那一次生效
    EXPECT_EQ(popId(jb, 9000), 2);
    EXPECT_EQ(popId(jb, 9000), 3);
}

TEST(JitterBuffer, KeyFrameGateCanBeDisabled) {
    JitterBuffer jb(noKeyGate());

    jb.push(makeFrame(1, 1000, /*isKey=*/false), 5000);

    EXPECT_EQ(jb.size(), 1u);
    EXPECT_EQ(jb.stats().framesBeforeKey, 0u);
}

TEST(JitterBuffer, DropUntilKeyFrameCountsPendingFramesAndRearmsTheGate) {
    JitterBuffer jb;
    jb.push(makeFrame(1, 1000, /*isKey=*/true), 1000);
    jb.push(makeFrame(2, 1033, /*isKey=*/false), 1000);
    ASSERT_EQ(jb.size(), 2u);

    jb.dropUntilKeyFrame();
    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesDroppedForResync, 2u);

    jb.push(makeFrame(3, 1066, /*isKey=*/false), 1000);
    EXPECT_EQ(jb.stats().framesBeforeKey, 1u);
    jb.push(makeFrame(4, 1099, /*isKey=*/true), 1000);
    EXPECT_EQ(popId(jb, 9000), 4);
}

// ---------- 排序 ----------

TEST(JitterBuffer, DeliversInFrameIdOrderRegardlessOfArrivalOrder) {
    JitterBuffer jb(noKeyGate());

    // 到达顺序 3, 1, 4, 2 —— 全部同一时刻到, 排除时序因素, 只看排序
    jb.push(makeFrame(3, 1000), 5000);
    jb.push(makeFrame(1, 1000), 5000);
    jb.push(makeFrame(4, 1000), 5000);
    jb.push(makeFrame(2, 1000), 5000);

    EXPECT_EQ(popId(jb, 5000), 1);
    EXPECT_EQ(popId(jb, 5000), 2);
    EXPECT_EQ(popId(jb, 5000), 3);
    EXPECT_EQ(popId(jb, 5000), 4);
    EXPECT_EQ(popId(jb, 5000), -1);
    EXPECT_EQ(jb.stats().framesOut, 4u);
}

TEST(JitterBuffer, PayloadSurvivesTheRoundTrip) {
    JitterBuffer jb(noKeyGate());
    jb.push(makeFrame(7, 1000), 5000);

    AssembledFrame out;
    ASSERT_TRUE(jb.pop(out, 5000));
    EXPECT_EQ(out.frameId, 7u);
    EXPECT_EQ(out.timestampMs, 1000u);
    ASSERT_EQ(out.data.size(), 8u);
    EXPECT_EQ(out.data[0], 7u);  // 串帧的话这里就是别人的号
}

TEST(JitterBuffer, FrameIdWraparoundKeepsOrder) {
    JitterBuffer jb(noKeyGate());

    // 0xFFFFFFFE -> 0x00000001 跨过回绕点, 且故意乱序送入。
    // 直接比大小的实现会把 0 和 1 排到 0xFFFFFFFE 前面, 输出顺序整个反过来。
    jb.push(makeFrame(0xFFFFFFFEu, 1000), 5000);
    jb.push(makeFrame(0x00000000u, 1000), 5000);
    jb.push(makeFrame(0xFFFFFFFFu, 1000), 5000);
    jb.push(makeFrame(0x00000001u, 1000), 5000);

    EXPECT_EQ(popId(jb, 5000), static_cast<int64_t>(0xFFFFFFFEu));
    EXPECT_EQ(popId(jb, 5000), static_cast<int64_t>(0xFFFFFFFFu));
    EXPECT_EQ(popId(jb, 5000), 0);
    EXPECT_EQ(popId(jb, 5000), 1);
}

// ---------- 交付水位: 绝不回头 ----------

TEST(JitterBuffer, AFrameOlderThanTheLastDeliveredOneIsDropped) {
    JitterBuffer jb(noKeyGate());

    jb.push(makeFrame(5, 1000), 5000);
    ASSERT_EQ(popId(jb, 5000), 5);

    // 帧 4 姗姗来迟。解码器收到倒退的帧比收不到更糟, 只能丢
    jb.push(makeFrame(4, 1000), 5010);
    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesTooLate, 1u);

    // 但不能因此把后面的帧也拒了
    jb.push(makeFrame(6, 1033), 5033);
    EXPECT_EQ(jb.size(), 1u);
}

TEST(JitterBuffer, ReplayingTheLastDeliveredFrameIsAlsoTooLate) {
    JitterBuffer jb(noKeyGate());

    jb.push(makeFrame(5, 1000), 5000);
    ASSERT_EQ(popId(jb, 5000), 5);

    // 整帧重传会造出这种输入: 判据必须是 <= 而不是 <
    jb.push(makeFrame(5, 1000), 5010);
    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesTooLate, 1u);
}

TEST(JitterBuffer, DuplicateFrameIdIsDroppedWhileStillPending) {
    JitterBuffer jb(noKeyGate(100));

    jb.push(makeFrame(7, 1000), 5000);
    jb.push(makeFrame(7, 1000), 5005);

    EXPECT_EQ(jb.size(), 1u);
    EXPECT_EQ(jb.stats().framesDuplicate, 1u);
    EXPECT_EQ(jb.stats().framesTooLate, 0u);  // 还没交付过, 不算晚
}

// ---------- 按时放行 ----------

TEST(JitterBuffer, HoldsAFrameUntilItsPlayTime) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/50));

    // offset = 5000 - 1000 = 4000, playAt = 1000 + 4000 + 50 = 5050
    jb.push(makeFrame(1, 1000), 5000);

    AssembledFrame frame;
    EXPECT_FALSE(jb.pop(frame, 5049));
    EXPECT_EQ(jb.msUntilNextDue(5049), 1);
    EXPECT_TRUE(jb.pop(frame, 5050));
}

TEST(JitterBuffer, ZeroTargetDelayDeliversAsSoonAsItArrives) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/0));

    jb.push(makeFrame(1, 1000), 5000);

    // 水位为零就是"不容忍任何乱序": 帧一到就该走, 后面来的更老的帧只能丢。
    // 这个模式是用来量基线的, 不是日常配置。
    EXPECT_EQ(jb.msUntilNextDue(5000), 0);
    EXPECT_EQ(popId(jb, 5000), 1);
}

TEST(JitterBuffer, ALateFrameDoesNotWaitAnyLonger) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/50));

    jb.push(makeFrame(1, 1000), 5000);  // 建立 offset = 4000
    ASSERT_EQ(popId(jb, 5050), 1);

    // 帧 2 该在 1033 发出, 却拖到本地 5150 才到(比顺畅时晚了 117ms)。
    // 它的档期 1033+4000+50 = 5083 早过了, 必须立刻放行而不是再等 50ms ——
    // 这正是"吸收抖动"与"统一加延迟"的区别。
    jb.push(makeFrame(2, 1033), 5150);
    EXPECT_EQ(jb.msUntilNextDue(5150), 0);
    EXPECT_EQ(popId(jb, 5150), 2);
}

TEST(JitterBuffer, AnEarlyFrameWaitsLonger) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/50));

    jb.push(makeFrame(1, 1000), 5000);  // offset = 4000
    ASSERT_EQ(popId(jb, 5050), 1);

    // 帧 2 走得比帧 1 顺(offset 4017 > 4000 不更新最小值), 档期仍是 5083,
    // 于是它得多等 33ms —— 早到的帧多等, 才能把间隔还原成发送端的节奏
    jb.push(makeFrame(2, 1033), 5050);
    EXPECT_EQ(jb.msUntilNextDue(5050), 33);
    EXPECT_EQ(popId(jb, 5050), -1);
    EXPECT_EQ(popId(jb, 5083), 2);
}

TEST(JitterBuffer, TheClockMappingFollowsTheFastestTripSeenSoFar) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/50));

    // 第一帧恰好赶上一次慢传输(offset 4200)。只锚定第一帧的实现会被它钉死,
    // 之后每一帧都"已经过期", 水位形同虚设。
    jb.push(makeFrame(1, 1000), 5200);
    EXPECT_EQ(jb.msUntilNextDue(5200), 50);  // playAt = 1000 + 4200 + 50

    // 第二帧走得快些(offset 4177), 最小偏移收窄, 后续排期跟着往前挪:
    // playAt = 1033 + 4177 + 50 = 5260, 而不是按 4200 算出来的 5283
    jb.push(makeFrame(2, 1033), 5210);
    ASSERT_EQ(popId(jb, 5250), 1);
    EXPECT_EQ(jb.msUntilNextDue(5250), 10);
    EXPECT_EQ(popId(jb, 5259), -1);
    EXPECT_EQ(popId(jb, 5260), 2);
}

TEST(JitterBuffer, DueTimeCountsDownAndClampsAtZero) {
    JitterBuffer jb(noKeyGate(/*targetDelayMs=*/100));

    jb.push(makeFrame(1, 1000), 5000);  // playAt = 5100

    EXPECT_EQ(jb.msUntilNextDue(5000), 100);
    EXPECT_EQ(jb.msUntilNextDue(5060), 40);
    EXPECT_EQ(jb.msUntilNextDue(5100), 0);
    // 过期了返回 0 而不是负数: 这个值要直接喂给 poll 的超时参数
    EXPECT_EQ(jb.msUntilNextDue(9999), 0);
}

// ---------- 容量上限 ----------

TEST(JitterBuffer, SoftLimitReleasesTheOldestFrameEarlyInsteadOfDroppingIt) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/1000);  // 谁都还没到点
    cfg.maxFrames = 4;
    cfg.hardLimitFrames = 100;
    JitterBuffer jb(cfg);

    for (uint32_t i = 1; i <= 5; ++i) {
        jb.push(makeFrame(i, 1000 + 33 * i), 5000);
    }

    // 攒到第 5 帧说明消费端跟不上或者映射漂了。最老的那帧已经在手里,
    // 丢了必然花屏, 播早了至少画面是对的
    EXPECT_EQ(jb.stats().framesForcedEarly, 1u);
    EXPECT_EQ(jb.stats().framesDropped, 0u);
    EXPECT_EQ(jb.msUntilNextDue(5000), 0);
    EXPECT_EQ(popId(jb, 5000), 1);
    EXPECT_EQ(popId(jb, 5000), -1);  // 其余的档期没变, 仍然得等
}

TEST(JitterBuffer, ForcedFramesAreNotCountedTwice) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/1000);
    cfg.maxFrames = 4;
    cfg.hardLimitFrames = 100;
    JitterBuffer jb(cfg);

    for (uint32_t i = 1; i <= 6; ++i) {
        jb.push(makeFrame(i, 1000 + 33 * i), 5000);
    }

    // 每超一帧只该记一笔; 把"已经是 0 的也重新标一次"会让这个数每收一帧涨一次
    EXPECT_EQ(jb.stats().framesForcedEarly, 2u);
}

TEST(JitterBuffer, HardLimitKeepsMemoryBounded) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/1000);
    cfg.maxFrames = 4;
    cfg.hardLimitFrames = 6;
    JitterBuffer jb(cfg);

    for (uint32_t i = 1; i <= 8; ++i) {
        jb.push(makeFrame(i, 1000 + 33 * i), 5000);
    }

    // 软上限只改"什么时候该放", 不减少条目数。消费端彻底卡住时,
    // 光靠它内存会一直涨 —— 这就是硬上限存在的理由
    EXPECT_EQ(jb.size(), 6u);
    EXPECT_EQ(jb.stats().framesDropped, 2u);
    EXPECT_EQ(popId(jb, 9000), 3);  // 1 和 2 被丢了
}

TEST(JitterBuffer, HardLimitDefaultsToTwiceTheSoftLimit) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/1000);
    cfg.maxFrames = 3;
    cfg.hardLimitFrames = 0;  // 0 表示取 maxFrames * 2
    JitterBuffer jb(cfg);

    for (uint32_t i = 1; i <= 10; ++i) {
        jb.push(makeFrame(i, 1000 + 33 * i), 5000);
    }

    // 用 EXPECT_EQ 而不是 EXPECT_LE: 后者在"一帧都没存下"的空实现上也是绿的,
    // 而一条永远不红的测试比没有测试更糟 —— 它会让人以为这里已经验过了
    EXPECT_EQ(jb.size(), 6u);
    EXPECT_EQ(jb.stats().framesDropped, 4u);
}

TEST(JitterBuffer, DroppedFramesDoNotAdvanceTheDeliveryWatermark) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/1000);
    cfg.maxFrames = 2;
    cfg.hardLimitFrames = 2;
    JitterBuffer jb(cfg);

    jb.push(makeFrame(1, 1000), 5000);
    jb.push(makeFrame(2, 1033), 5000);
    jb.push(makeFrame(3, 1066), 5000);  // 挤掉帧 1

    ASSERT_EQ(jb.stats().framesDropped, 1u);

    // 水位的含义是"已经交付到哪儿"。被丢的帧从来没交付过,
    // 拿它推水位会把后面正常的帧一起判成太晚
    EXPECT_EQ(popId(jb, 9000), 2);
    EXPECT_EQ(popId(jb, 9000), 3);
}

// ---------- reset ----------

TEST(JitterBuffer, ResetClearsFramesStatsAndTheKeyFrameGate) {
    JitterBuffer jb;  // 带起播门

    jb.push(makeFrame(1, 1000, /*isKey=*/true), 5000);
    jb.push(makeFrame(2, 1033), 5033);
    ASSERT_EQ(jb.size(), 2u);

    jb.reset();

    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesIn, 0u);
    EXPECT_EQ(jb.stats().framesOut, 0u);
    EXPECT_EQ(jb.msUntilNextDue(5000), -1);

    // 起播门要重新武装: 重连之后同样得从关键帧开始, 否则新会话第一眼就是花屏
    jb.push(makeFrame(3, 2000, /*isKey=*/false), 6000);
    EXPECT_EQ(jb.size(), 0u);
    EXPECT_EQ(jb.stats().framesBeforeKey, 1u);
}

// ---------- 计数器总账 ----------

TEST(JitterBuffer, EveryFrameIsAccountedForExactlyOnce) {
    JitterBufferConfig cfg = noKeyGate(/*targetDelayMs=*/0);
    cfg.maxFrames = 4;
    cfg.hardLimitFrames = 4;
    JitterBuffer jb(cfg);

    jb.push(makeFrame(1, 1000), 5000);
    jb.push(makeFrame(1, 1000), 5000);  // 重复
    ASSERT_EQ(popId(jb, 5000), 1);
    jb.push(makeFrame(1, 1000), 5001);  // 太晚
    jb.push(makeFrame(2, 1033), 5033);

    const JitterBufferStats& s = jb.stats();
    // 进来的每一帧都必须落在某个去向上, 否则统计对不上账时没人知道帧去哪了
    EXPECT_EQ(s.framesIn, 4u);
    EXPECT_EQ(s.framesOut + s.framesTooLate + s.framesDuplicate + s.framesDropped +
                  s.framesBeforeKey + jb.size(),
              s.framesIn);
}
