/**
 * @file    test_nack_tracker.cpp
 * @brief   M4.1 缺口跟踪与重传请求的契约测试
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 全部用裸数字, 不构造真包 —— NackTracker 不认识包格式, 这正是把它
 *       和 FrameAssembler 分开的收益之一。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "modules/transport/NackTracker.h"

namespace {
    constexpr uint16_t FRAGS_PER_FRAME = 8;  // 实测 640x480@2000kbps 约 7.75 包/帧

    NackTrackerConfig config() {
        NackTrackerConfig cfg;
        cfg.minReorderPackets = 4;
        cfg.maxReorderPackets = 64;
        cfg.maxRequestsPerSeq = 3;
        cfg.windowPackets = 256;
        return cfg;
    }

    /** 顺序喂 [from, to) 这一段 seq */
    void feedRange(NackTracker& t, uint32_t from, uint32_t to) {
        for (uint32_t s = from; s != to; ++s) t.onPacket(s, FRAGS_PER_FRAME, false);
    }

    /** 顺序喂 [from, to), 但跳过 skip 里列出的 seq */
    void feedRangeSkipping(NackTracker& t, uint32_t from, uint32_t to,
                           const std::vector<uint32_t>& skip) {
        for (uint32_t s = from; s != to; ++s) {
            if (std::find(skip.begin(), skip.end(), s) != skip.end()) continue;
            t.onPacket(s, FRAGS_PER_FRAME, false);
        }
    }

    std::vector<uint32_t> collect(NackTracker& t) {
        std::vector<uint32_t> out;
        t.collectNackTargets(out);
        return out;
    }
}  // namespace

// ---------- 基线 ----------

/**
 * 接收端从流的**中间**接进来是常态 —— 对端已经跑了一会儿。
 * 第一个 seq 是 5000 的话, 绝不能把 0..4999 判成丢包然后发五千条 NACK。
 * 这是这类代码最容易写出的第一个 bug, 而且它在从 seq=0 开始的测试里**看不出来**。
 */
TEST(NackTracker, TheFirstPacketOnlyEstablishesABaseline) {
    NackTracker tracker(config());
    tracker.onPacket(5000, FRAGS_PER_FRAME, false);
    EXPECT_TRUE(collect(tracker).empty()) << "把接入之前的 seq 全当成丢包了";
    EXPECT_EQ(tracker.stats().pending, 0u);
    EXPECT_EQ(tracker.stats().lostForReal, 0u);
}

TEST(NackTracker, AContiguousStreamProducesNoRequests) {
    NackTracker tracker(config());
    feedRange(tracker, 1000, 1200);
    EXPECT_TRUE(collect(tracker).empty());
    EXPECT_EQ(tracker.stats().pending, 0u);
    EXPECT_EQ(tracker.stats().nacksRequested, 0u);
    EXPECT_EQ(tracker.stats().packetsSeen, 200u);
}

// ---------- 乱序 vs 真丢 ----------

/**
 * 缺口刚出现时**不能**立刻请求 —— 那多半只是乱序, 包还在路上。
 * 判定标准是"又收了至少一帧的包数还没来"。
 */
TEST(NackTracker, AFreshGapIsNotRequestedYet) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    tracker.onPacket(106, FRAGS_PER_FRAME, false);  // 跳过 105
    tracker.onPacket(107, FRAGS_PER_FRAME, false);
    EXPECT_TRUE(collect(tracker).empty()) << "缺口才隔 2 个包就报, 乱序会被当成丢包";
    EXPECT_EQ(tracker.stats().pending, 1u) << "缺口本身要记下来";
}

TEST(NackTracker, AGapIsRequestedAfterAFrameWorthOfNewerPackets) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    feedRangeSkipping(tracker, 105, 105 + 2 * FRAGS_PER_FRAME, {105});

    const std::vector<uint32_t> targets = collect(tracker);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0], 105u);
    EXPECT_EQ(tracker.stats().nacksRequested, 1u);
}

/**
 * 乱序的包晚一点到了, 缺口就该销掉, 而且**不能**再被请求。
 * 这条不成立的话, 每一次正常乱序都会变成一次白发的重传请求 ——
 * 而重传包是重复包, 会把丢包率压低(M2 账 ①)。
 */
TEST(NackTracker, ALateArrivalClosesTheGapAndStopsRequests) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    tracker.onPacket(106, FRAGS_PER_FRAME, false);
    tracker.onPacket(107, FRAGS_PER_FRAME, false);
    tracker.onPacket(105, FRAGS_PER_FRAME, false);  // 迟到, 但还没被请求过

    feedRange(tracker, 108, 108 + 3 * FRAGS_PER_FRAME);
    EXPECT_TRUE(collect(tracker).empty()) << "缺口已经补上了还在请求";
    EXPECT_EQ(tracker.stats().pending, 0u);
    EXPECT_EQ(tracker.stats().nacksRequested, 0u);
    EXPECT_EQ(tracker.stats().recovered, 0u)
        << "还没请求过就自己到了, 那是乱序不是'重传救回来的', 不该记功";
}

/** 请求之后重传包回来了 —— 这才是 recovered, 是"NACK 帮上忙了"的唯一证据 */
TEST(NackTracker, ARetransmissionAfterARequestCountsAsRecovered) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    feedRangeSkipping(tracker, 105, 105 + 2 * FRAGS_PER_FRAME, {105});
    ASSERT_EQ(collect(tracker).size(), 1u);

    tracker.onPacket(105, FRAGS_PER_FRAME, false);  // 重传包到了
    EXPECT_EQ(tracker.stats().recovered, 1u);
    EXPECT_EQ(tracker.stats().pending, 0u);
    EXPECT_EQ(tracker.stats().lostForReal, 0u) << "救回来了就不算丢";
    EXPECT_TRUE(collect(tracker).empty());
}

// ---------- 限流 ----------

/**
 * 反向通道自己也会丢包, 所以要重发; 但必须有上限。
 * 两个方向丢包率都是 p 时, 一次重传成功率只有 (1-p)^2 —— 10% 丢包下 81%,
 * 30% 下只剩 49%, 所以重发是必要的, 不是可选项。
 */
TEST(NackTracker, TheSameSeqIsRequestedAtMostMaxRequestsTimes) {
    NackTrackerConfig cfg = config();
    cfg.maxRequestsPerSeq = 3;
    NackTracker tracker(cfg);

    feedRange(tracker, 100, 105);
    uint32_t next = 105;
    size_t totalRequests = 0;
    // 一直喂新包, 每隔一帧的包数收一次
    for (int round = 0; round < 10; ++round) {
        feedRangeSkipping(tracker, next, next + 2 * FRAGS_PER_FRAME, {105});
        next += 2 * FRAGS_PER_FRAME;
        totalRequests += collect(tracker).size();
    }

    EXPECT_EQ(totalRequests, 3u) << "没有上限的话, 一个永远回不来的包会被无限请求";
    EXPECT_EQ(tracker.stats().nacksRequested, 3u);
    EXPECT_EQ(tracker.stats().givenUp, 1u);
    EXPECT_EQ(tracker.stats().lostForReal, 1u);
    EXPECT_EQ(tracker.stats().pending, 0u) << "放弃了就要从表里删掉, 别空占位置";
}

TEST(NackTracker, RequestsAreSpacedOutNotRepeatedEveryCall) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    feedRangeSkipping(tracker, 105, 105 + 2 * FRAGS_PER_FRAME, {105});

    ASSERT_EQ(collect(tracker).size(), 1u);
    // 一个新包都没来, 连着再取两次
    EXPECT_TRUE(collect(tracker).empty()) << "同一轮里被反复取出, NACK 会成风暴";
    EXPECT_TRUE(collect(tracker).empty());
    EXPECT_EQ(tracker.stats().nacksRequested, 1u);
}

/**
 * 流停了就不该再产生请求。
 *
 * 这是"按包数"而不是"按定时器"判定的核心收益: sender 用 --frames=N 跑完自己退出是
 * 日常工作流, 定时器判据会在那之后对着一个已经关掉的对端持续发 NACK。
 */
TEST(NackTracker, NoNewPacketsMeansNoNewRequests) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    feedRangeSkipping(tracker, 105, 105 + 2 * FRAGS_PER_FRAME, {105});
    ASSERT_EQ(collect(tracker).size(), 1u);

    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(collect(tracker).empty()) << "流已经停了还在发 NACK";
    }
}

// ---------- 有界 ----------

/**
 * 太老的 seq 直接放弃: 内存要有界, 而且重传回来也来不及了 ——
 * 帧早过了 playAt, JitterBuffer 会以 framesTooLate 计数然后扔掉, 白费一趟带宽。
 */
TEST(NackTracker, GapsOlderThanTheWindowAreGivenUp) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 64;
    cfg.maxRequestsPerSeq = 100;  // 排除"请求次数用完"这条退出路径
    NackTracker tracker(cfg);

    feedRange(tracker, 1000, 1005);
    feedRangeSkipping(tracker, 1005, 1005 + 500, {1005});
    (void)collect(tracker);

    EXPECT_EQ(tracker.stats().pending, 0u) << "缺口表没有上界, 跑一晚上就是内存泄漏";
    EXPECT_GE(tracker.stats().givenUp, 1u);
    EXPECT_GE(tracker.stats().lostForReal, 1u);
}

TEST(NackTracker, ThePendingSetStaysBoundedUnderHeavyLoss) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 128;
    NackTracker tracker(cfg);

    // 每 3 个只收 1 个, 连续两万个包
    for (uint32_t s = 0; s < 20000; ++s) {
        if (s % 3 != 0) continue;
        tracker.onPacket(s, FRAGS_PER_FRAME, false);
        (void)collect(tracker);
        ASSERT_LE(tracker.stats().pending, cfg.windowPackets)
            << "seq=" << s << " 时缺口表已经超过窗口";
    }
}

// ---------- 边界 ----------

TEST(NackTracker, DuplicatePacketsAreNotCountedAsRecovery) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 120);
    const uint64_t before = tracker.stats().recovered;
    for (int i = 0; i < 5; ++i) tracker.onPacket(110, FRAGS_PER_FRAME, false);
    EXPECT_EQ(tracker.stats().recovered, before) << "重复包不是'救回来的'";
    EXPECT_EQ(tracker.stats().pending, 0u);
}

/**
 * seq 是 32 位循环计数器, 一定会回绕。比较大小必须用 seqNewerThan 的有符号差值 ——
 * 直接 a > b 会在 0xFFFFFFFF -> 0 处把最新的包判成最老的, 表现为"跑了几十分钟之后
 * 突然满屏 NACK", 只在长时间运行时复现。
 */
TEST(NackTracker, SeqWrapAroundIsHandled) {
    NackTracker tracker(config());
    const uint32_t start = 0xFFFFFFFFu - 20;
    feedRange(tracker, start, start + 10);  // 回绕前
    feedRangeSkipping(tracker, start + 10, start + 10 + 3 * FRAGS_PER_FRAME,
                      {start + 15});  // 跨过 0, 中间丢一个

    const std::vector<uint32_t> targets = collect(tracker);
    ASSERT_EQ(targets.size(), 1u) << "回绕点上缺口判定失效";
    EXPECT_EQ(targets[0], start + 15);
}

TEST(NackTracker, ResetForgetsEverything) {
    NackTracker tracker(config());
    feedRange(tracker, 100, 105);
    feedRangeSkipping(tracker, 105, 105 + 2 * FRAGS_PER_FRAME, {105});
    ASSERT_EQ(tracker.stats().pending, 1u);

    tracker.reset();
    EXPECT_EQ(tracker.stats().pending, 0u);
    EXPECT_TRUE(collect(tracker).empty());

    // reset 之后第一个包重新建立基线, 不能把新流的开头判成缺口
    tracker.onPacket(90000, FRAGS_PER_FRAME, false);
    EXPECT_TRUE(collect(tracker).empty());
}

// ---------- 流不连续 ----------

/**
 * seq 是包头里**唯一没有任何校验的字段**: 版本、类型、分片自洽性 M2 都查了,
 * 但 seq 的每一个 32 位取值都合法, 所以它能带着任意值穿过全部现有闸门。
 *
 * 而 seqNewerThan 只保证差值为正, 上界是 2^31 —— 一个位翻转、一个上一轮残留的包、
 * 同端口上另一个进程, 都能让 forwardDistance 变成十亿级。不堵的话:
 *   - lostForReal 被永久污染(实测一个包就从 0 跳到 999998977),
 *     而它正是这个类存在的理由 —— M4 的"X% 丢包下不花屏"里那个 X 靠它;
 *   - 为一整个窗口的**根本不存在的包**登记幻影缺口, 接下来是一场 NACK 风暴。
 */
TEST(NackTracker, AHugeSeqJumpIsTreatedAsANewStreamNotAsMassiveLoss) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 256;
    NackTracker tracker(cfg);

    feedRange(tracker, 1000, 1100);
    ASSERT_EQ(tracker.stats().lostForReal, 0u);
    ASSERT_EQ(tracker.stats().pending, 0u);

    tracker.onPacket(1100u + 1000000000u, FRAGS_PER_FRAME, false);

    EXPECT_EQ(tracker.stats().lostForReal, 0u) << "把一次流中断记成了十亿个丢包";
    EXPECT_EQ(tracker.stats().givenUp, 0u);
    EXPECT_EQ(tracker.stats().pending, 0u) << "为不存在的包登记了一整窗口的幻影缺口";
    EXPECT_EQ(tracker.stats().discontinuities, 1u) << "重建基线必须报出来, 不能静默";
    EXPECT_TRUE(collect(tracker).empty()) << "幻影缺口会变成一场 NACK 风暴";
}

/** 重建基线之后要能正常继续工作, 不是只把状态清了了事 */
TEST(NackTracker, TrackingResumesNormallyAfterADiscontinuity) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 256;
    NackTracker tracker(cfg);

    feedRange(tracker, 1000, 1100);
    const uint32_t newBase = 1100u + 1000000000u;
    tracker.onPacket(newBase, FRAGS_PER_FRAME, false);
    ASSERT_EQ(tracker.stats().discontinuities, 1u);

    // 新流上正常丢一个包, 应当照常被检出
    feedRangeSkipping(tracker, newBase + 1, newBase + 1 + 2 * FRAGS_PER_FRAME,
                      {newBase + 3});
    const std::vector<uint32_t> targets = collect(tracker);
    ASSERT_EQ(targets.size(), 1u) << "重建基线之后就再也不工作了";
    EXPECT_EQ(targets[0], newBase + 3);
}

/**
 * 边界: 跳号**恰好等于**窗口是不连续, 差一个是正常的重丢包。
 * 这两种情况的处置完全不同(一个清账、一个记账), 分界点必须钉死。
 */
TEST(NackTracker, AJumpJustUnderTheWindowIsStillCountedAsLoss) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 256;
    cfg.maxRequestsPerSeq = 100;
    NackTracker tracker(cfg);

    tracker.onPacket(1000, FRAGS_PER_FRAME, false);
    tracker.onPacket(1000 + 255, FRAGS_PER_FRAME, false);  // 跳 255 < 256

    EXPECT_EQ(tracker.stats().discontinuities, 0u) << "窗口以内的跳号是真丢包, 不是换流";
    EXPECT_GT(tracker.stats().pending + tracker.stats().lostForReal, 0u)
        << "254 个中间的包被静默吞掉了";
}

// ---------- M4.4: 关键帧分片的放弃是 PLI 的触发信号 ----------

namespace {
    /** 顺序喂 [from, to), 跳过 skip; 全部标成关键帧分片 */
    void feedKeyRangeSkipping(NackTracker& t, uint32_t from, uint32_t to,
                              const std::vector<uint32_t>& skip) {
        for (uint32_t s = from; s != to; ++s) {
            if (std::find(skip.begin(), skip.end(), s) != skip.end()) continue;
            t.onPacket(s, FRAGS_PER_FRAME, true);
        }
    }
}  // namespace

/**
 * 丢掉的那个包接收端根本没收到，**无从知道它是不是关键帧分片**。
 * 做法是把"揭示这个缺口的那个包"的 isKey 记下来——seq 在一帧内连续，
 * 所以缺口两侧的包极大概率同帧。这是推断不是事实，所以只拿来触发 PLI。
 */
TEST(NackTracker, GivingUpAKeyFrameFragmentIsReportedSeparately) {
    NackTrackerConfig cfg = config();
    cfg.maxRequestsPerSeq = 1;  // 请求一次就放弃, 缩短用例
    NackTracker tracker(cfg);

    feedKeyRangeSkipping(tracker, 100, 100 + 3 * FRAGS_PER_FRAME, {105});
    ASSERT_EQ(collect(tracker).size(), 1u);  // 第一次请求后立刻放弃

    EXPECT_EQ(tracker.stats().givenUp, 1u);
    EXPECT_EQ(tracker.stats().keyFramesGivenUp, 1u)
        << "关键帧分片放弃了却没报出来 —— PLI 的触发信号就没了";
}

TEST(NackTracker, GivingUpANonKeyFragmentDoesNotTriggerTheKeyFrameCounter) {
    NackTrackerConfig cfg = config();
    cfg.maxRequestsPerSeq = 1;
    NackTracker tracker(cfg);

    feedRangeSkipping(tracker, 100, 100 + 3 * FRAGS_PER_FRAME, {105});  // isKey = false
    ASSERT_EQ(collect(tracker).size(), 1u);

    EXPECT_EQ(tracker.stats().givenUp, 1u);
    EXPECT_EQ(tracker.stats().keyFramesGivenUp, 0u)
        << "P 帧分片也触发 PLI 的话, 一有丢包就会不停要 IDR";
}

/**
 * 两条 givenUp 路径都要计：请求次数用完，和滑出窗口。
 * 漏掉后者的现象是"丢包一多 PLI 反而不发了"——因为丢得越多越容易走滑出窗口那条。
 */
TEST(NackTracker, KeyFramesGivenUpCountsTheWindowEvictionPathToo) {
    NackTrackerConfig cfg = config();
    cfg.windowPackets = 64;
    cfg.maxRequestsPerSeq = 100;  // 堵死"请求次数用完"这条路
    NackTracker tracker(cfg);

    feedKeyRangeSkipping(tracker, 1000, 1000 + 500, {1005});
    (void)collect(tracker);

    EXPECT_GE(tracker.stats().givenUp, 1u);
    EXPECT_GE(tracker.stats().keyFramesGivenUp, 1u)
        << "只在'请求用完'那条路上计, 丢包一多 PLI 就不发了";
}

TEST(NackTracker, RecoveredKeyFragmentsAreNotCountedAsGivenUp) {
    NackTracker tracker(config());
    feedKeyRangeSkipping(tracker, 100, 100 + 2 * FRAGS_PER_FRAME, {105});
    ASSERT_EQ(collect(tracker).size(), 1u);

    tracker.onPacket(105, FRAGS_PER_FRAME, true);  // 重传回来了
    EXPECT_EQ(tracker.stats().recovered, 1u);
    EXPECT_EQ(tracker.stats().keyFramesGivenUp, 0u) << "救回来了还要 IDR, 那是白发";
}
