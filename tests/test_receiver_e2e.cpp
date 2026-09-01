/**
 * @file    test_receiver_e2e.cpp
 * @brief   M3.5 三线程接收管线的端到端行为: 三种收场、渲染档位、统计自洽
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 和 test_transport_loopback.cpp 的分工:
 *       那边验的是 M2 的**内容**正确性(两端码流逐字节一致), 这边验的是 M3.5 的
 *       **收尾与生命周期**行为 —— 谁先退出、退出码是什么、退出时尾巴丢没丢。
 *       两者都需要真实 H.264, 所以都要拖上 llsender + llcapture。
 *
 * @note 全部用 renderKind = "null": 不开窗、不需要 DISPLAY, 但逐帧校验尺寸、
 *       像素格式和 captureMs/frameId 的单调性 —— 比"能跑完"强得多的判据。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "app/receiver/ReceiverPipeline.h"
#include "app/sender/SenderPipeline.h"
#include "common/Clock.h"
#include "modules/capture/NullSource.h"

using namespace std::chrono_literals;

namespace {
    /** 收尾必须在这个时间内完成, 否则算失败而不是"慢" */
    constexpr auto SHUTDOWN_BUDGET = 3s;

    ReceiverPipelineConfig receiverConfig(const std::string& renderKind = "null") {
        ReceiverPipelineConfig cfg;
        cfg.listen = Endpoint{"127.0.0.1", 0};  // 端口交给内核, 写死会在 CI 上撞车
        cfg.recvTimeoutMs = 50;
        cfg.idleTimeoutMs = 0;
        cfg.renderKind = renderKind;
        cfg.renderer.width = 320;
        cfg.renderer.height = 240;
        // 测试里发得比 30fps 快, 容量给足, 免得把背压本身误当成传输问题
        cfg.decodeQueueCapacity = 64;
        cfg.renderQueueCapacity = 64;
        cfg.statsIntervalMs = 0;  // 统计日志会污染用例输出, 这里不需要
        return cfg;
    }

    SenderPipelineConfig senderConfig(uint16_t targetPort, int frames) {
        SenderPipelineConfig cfg;
        cfg.source.width = 320;
        cfg.source.height = 240;
        // 必须是真实帧率: NullRenderer 要求 captureMs 严格递增, 加速到 1000fps
        // 会让相邻帧落在同一毫秒上, 那是**测试参数**违反了实时帧的不变量, 不是 bug。
        cfg.source.fps = 30;

        cfg.encoder.width = cfg.source.width;
        cfg.encoder.height = cfg.source.height;
        cfg.encoder.fps = cfg.source.fps;
        cfg.encoder.bitrateKbps = 1000;
        cfg.encoder.gop = 10;

        cfg.queueCapacity = 4;
        cfg.sendQueueCapacity = 4;
        cfg.maxFrames = frames;
        cfg.target = Endpoint{"127.0.0.1", targetPort};
        return cfg;
    }

    /** 每个统计口径都必须自洽, 无论这一趟是正常跑完还是被打断 */
    void expectStatsAreSelfConsistent(const ReceiverPipelineStats& rs) {
        // 采样只发生在渲染成功且 captureMs != 0 的帧上, 所以样本数不可能超过渲染数
        EXPECT_LE(rs.latency.samples, rs.renderer.framesRendered);
        // 进渲染器的帧非"接受"即"拒绝", 不能凭空多出来
        EXPECT_LE(rs.renderer.framesRendered + rs.renderer.framesRejected,
                  rs.decoder.framesOut);
        // 解码器吐出的帧不可能比收包线程收下的还多
        EXPECT_LE(rs.decoder.framesOut, rs.framesWritten);
        if (rs.latency.samples != 0) {
            EXPECT_LE(rs.latency.p50Ms, rs.latency.p95Ms);
            EXPECT_LE(rs.latency.p95Ms, rs.latency.p99Ms);
            EXPECT_LE(rs.latency.p99Ms, rs.latency.maxMs);
        } else {
            EXPECT_EQ(rs.latency.maxMs, 0u);
        }
    }
}  // namespace

// ---------- 收场一: 外部停止标志 ----------

/**
 * D19: 点 × / Ctrl-C 是**正常结束**, 不是故障。
 *
 * 停止时另外两条线程正堵在 queueA_.push / queueB_.push 上, 被 close() 唤醒后
 * push 返回 false —— 那个 false 只有"队列已关闭"一个含义, 不能写成错误状态。
 * 写成错误的话每次正常退出的退出码都是 1, 而且第一个**真正的**故障会被这条
 * 收尾噪声按"上游优先"的规则盖掉。
 */
TEST(ReceiverE2E, StopFlagEndsAllThreeThreadsWithoutReportingAnError) {
    ReceiverPipeline receiver(receiverConfig());
    ASSERT_TRUE(receiver.open().isOk());

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    // 发得久一点, 保证停止发生在**流还在跑**的时候, 而不是收尾之后
    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(receiver.boundPort(), 300));
    std::atomic<bool> sendStop{false};
    auto sendDone = std::async(std::launch::async, [&] { return sender.run(sendStop); });

    // 让流真的跑起来再停 —— 停在"还没收到第一帧"的时刻等于什么都没测。
    // 这里只能靠等: stats() 返回的是 run() 结束后才填好的那份, 运行期读不到
    // 实时值(见文件末尾的 TODO)。
    std::this_thread::sleep_for(500ms);

    const uint64_t start = steadyNowMs();
    recvStop.store(true);
    ASSERT_EQ(recvDone.wait_for(SHUTDOWN_BUDGET), std::future_status::ready)
        << "置标志唤不醒阻塞中的线程 —— 退出 = 置标志 + 唤醒, 缺一不可";
    EXPECT_TRUE(recvDone.get().isOk()) << "正常退出不该报错";
    EXPECT_LT(steadyNowMs() - start, 1500u) << "收尾拖太久, 用户会以为程序卡死了";

    sendStop.store(true);
    ASSERT_EQ(sendDone.wait_for(SHUTDOWN_BUDGET), std::future_status::ready);
    // 发送端被中途叫停, 它返回什么不是这条用例的判据; 但 Status 带 [[nodiscard]],
    // 必须显式接住 —— 而且 future 的析构会阻塞, 不 get 就是在赌它已经结束了。
    const Status senderResult = sendDone.get();
    EXPECT_TRUE(senderResult.isOk()) << "发送端被外部停止也属于正常结束";

    const ReceiverPipelineStats& rs = receiver.stats();
    EXPECT_GT(rs.framesWritten, 0u) << "500ms 内一帧都没收到, 这条用例没测到东西";
    EXPECT_GT(rs.renderer.framesRendered, 0u) << "三条线程里渲染这条根本没跑起来";
    expectStatsAreSelfConsistent(rs);
}

// ---------- 收场二: 空闲超时(对端先退) ----------

/**
 * 对端跑完自己退出, 接收端靠 --idle-timeout 收场。
 *
 * 这条路和上面那条走的是**不同的触发源**(收包线程 vs 外部标志), 但汇进同一个
 * requestStop(); 两条都要单独验, 因为同一段收尾代码在两条路上的表现可以完全不同。
 */
TEST(ReceiverE2E, IdleTimeoutEndsARenderingPipelineCleanly) {
    constexpr int FRAMES = 20;

    ReceiverPipelineConfig cfg = receiverConfig();
    cfg.idleTimeoutMs = 400;
    ReceiverPipeline receiver(cfg);
    ASSERT_TRUE(receiver.open().isOk());

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(receiver.boundPort(), FRAMES));
    std::atomic<bool> sendStop{false};
    ASSERT_TRUE(sender.run(sendStop).isOk());

    ASSERT_EQ(recvDone.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(recvDone.get().isOk()) << "空闲退出是正常收尾, 不是错误";

    const ReceiverPipelineStats& rs = receiver.stats();
    const uint64_t encoded = sender.stats().encodedFrames;
    ASSERT_GT(encoded, 0u) << "发送端一帧都没编出来, 先查 M1";
    EXPECT_EQ(rs.framesWritten, encoded);
    EXPECT_EQ(rs.decoder.framesOut, encoded);
    EXPECT_EQ(rs.renderer.framesRendered, encoded) << "空闲超时前应当已经放行全部帧";
    EXPECT_EQ(rs.renderer.framesRejected, 0u);
    EXPECT_EQ(rs.latency.samples, encoded) << "每一帧都该有 captureMs, 一个哨兵都不该有";
    expectStatsAreSelfConsistent(rs);
}

// ---------- 收场三: 收满 maxFrames ----------

/**
 * D21: "立刻停止"和"正常结束"的收尾动作不一样。
 *
 * maxFrames 数的是**收包线程写下的帧**, 而那一刻 jitter buffer 里还压着
 * targetDelayMs / 帧间隔 帧没到放行时刻。收满就直接 close(A) 的话这些帧全丢,
 * 表现为 rendered 比 frames 少几帧 —— 而且每次少的数量还不一样。
 *
 * 这里把 targetDelayMs 特意放大到 4 帧的量级, 让"尾巴"必然存在:
 * 没有排空逻辑的话这条用例一定失败, 而不是偶尔失败。
 */
TEST(ReceiverE2E, MaxFramesDrainsTheJitterBufferTail) {
    constexpr int FRAMES = 30;

    ReceiverPipelineConfig cfg = receiverConfig();
    cfg.maxFrames = FRAMES;
    cfg.jitter.delay.targetDelayMs = 133;  // ≈4 帧 @30fps
    ReceiverPipeline receiver(cfg);
    ASSERT_TRUE(receiver.open().isOk());

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(receiver.boundPort(), FRAMES));
    std::atomic<bool> sendStop{false};
    ASSERT_TRUE(sender.run(sendStop).isOk());

    ASSERT_EQ(recvDone.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(recvDone.get().isOk());

    const ReceiverPipelineStats& rs = receiver.stats();
    EXPECT_EQ(rs.framesWritten, static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(rs.renderer.framesRendered, static_cast<uint64_t>(FRAMES))
        << "收满就走人会把 jitter 里还没到点的尾帧一起丢掉";
    EXPECT_EQ(rs.jitter.framesDroppedForResync, 0u);
    expectStatsAreSelfConsistent(rs);
}

// ---------- 渲染档位 ----------

/**
 * 空 renderKind 保留 M2 的纯落盘模式: 连解码和渲染两级线程都不起。
 *
 * 这一档的价值是排查时能一句话把范围劈成两半 —— 关掉解码渲染还坏就是传输的事。
 * 所以"没起来"必须是可验证的, 不能只是"看起来没输出"。
 */
TEST(ReceiverE2E, EmptyRenderKindSkipsDecodeAndRenderEntirely) {
    constexpr int FRAMES = 10;

    ReceiverPipelineConfig cfg = receiverConfig("");
    cfg.idleTimeoutMs = 400;
    ReceiverPipeline receiver(cfg);
    ASSERT_TRUE(receiver.open().isOk());

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(receiver.boundPort(), FRAMES));
    std::atomic<bool> sendStop{false};
    ASSERT_TRUE(sender.run(sendStop).isOk());

    ASSERT_EQ(recvDone.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(recvDone.get().isOk());

    const ReceiverPipelineStats& rs = receiver.stats();
    EXPECT_EQ(rs.framesWritten, sender.stats().encodedFrames) << "收包这一级照常工作";
    EXPECT_EQ(rs.decoder.framesOut, 0u);
    EXPECT_EQ(rs.renderer.framesRendered, 0u);
    EXPECT_EQ(rs.latency.samples, 0u) << "没渲染就没有端到端延迟这个概念";
    EXPECT_EQ(rs.decodeQueuePeak, 0u) << "队列 A 一个元素都不该进过";
    EXPECT_EQ(rs.renderQueuePeak, 0u);
}

/**
 * 认不出的渲染器必须在 open() 就失败, 而不是等 run() 起了线程再炸。
 *
 * 早失败的理由是错误来源清晰: open() 里报的一定是配置问题, run() 里报的
 * 可能是配置也可能是运行时故障, 混在一起就没法一眼定位。
 */
TEST(ReceiverE2E, UnknownRenderKindFailsAtOpen) {
    ReceiverPipeline receiver(receiverConfig("xyz"));
    EXPECT_EQ(receiver.open().code(), Code::InvalidArg);
    // TODO: validateConfig() 的消息只列出了合法取值, 没回显用户实际输入的 "xyz";
    //       open() 里 createRenderer 返回 nullptr 那条分支反而带了值 —— 但那条
    //       被 validateConfig 挡在前面, 永远走不到。两处口径统一之后这里可以加一条
    //       "错误消息要带上用户输入的值"的断言。
}

/*
 * TODO(M6): ReceiverPipeline 没有**运行期**的统计读口。
 *
 * stats() 返回的 stats_ 只在 run() 结束、三条线程 join 完之后才填好; 运行期真正
 * 有效的是私有的 snapshotStats()(statsMu_ + shared_), 目前只有渲染线程自己打
 * 统计日志时用。外部想在跑的过程中看一眼当前 fps / 队列深度是做不到的 ——
 * 上面第一条用例只能靠 sleep 500ms 猜"应该已经收到帧了"。
 *
 * M6 要把指标推给 Redis / HTTP API 时必须补这个口子; 补上之后这条用例可以改成
 * 轮询"已收到第一帧"再触发停止, 从"靠等"变成"可等待的确定时刻"。
 */

// ---------- M4.0 丢包注入 ----------

/**
 * 注入器接进管线之后，端到端还必须是可复现的。
 *
 * 自由函数的纯函数性质在 test_loss_injector.cpp 里已经钉住了，但那和"接进管线之后
 * 还可复现"**不是一回事** —— 中间隔着 UDP 的到达顺序、recvFrom 的内核缓冲、
 * FrameAssembler 的状态。这条不成立的话，后面每一次「改了 FEC，看恢复率有没有变好」
 * 的对比结论都是悬空的：你分不清数字变了是因为代码改好了，还是因为这次丢的不是同一批包。
 */
TEST(ReceiverE2E, TheSameSeedDropsExactlyTheSamePackets) {
    constexpr int FRAMES = 40;
    constexpr uint32_t SEED = 20260828;

    // 同一份配置跑两遍，除了内核分配的端口不同，其它完全一致
    auto runOnce = [](uint32_t seed) {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.idleTimeoutMs = 400;
        cfg.loss.seed = seed;
        cfg.loss.lossPercent = 30;
        // 起播门会让丢掉 IDR 的那一段完全不出帧，那是正常行为；这条用例只看
        // 收包这一级的计数，所以不需要关掉它。
        ReceiverPipeline receiver(cfg);
        EXPECT_TRUE(receiver.open().isOk());

        std::atomic<bool> recvStop{false};
        auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

        SenderPipeline sender(std::make_unique<NullSource>(),
                              senderConfig(receiver.boundPort(), FRAMES));
        std::atomic<bool> sendStop{false};
        EXPECT_TRUE(sender.run(sendStop).isOk());

        EXPECT_EQ(recvDone.wait_for(10s), std::future_status::ready);
        EXPECT_TRUE(recvDone.get().isOk());
        return receiver.stats();
    };

    const ReceiverPipelineStats a = runOnce(SEED);
    const ReceiverPipelineStats b = runOnce(SEED);

    ASSERT_GT(a.injectedDrops, 0u) << "30% 丢包一个都没丢 —— 注入器根本没接上";
    // 发送端每次编出来的字节数一样，所以包数也一样；包数一样 + 种子一样
    // => 丢的必须是同一批包，一个不差
    EXPECT_EQ(a.assembler.packetsReceived + a.injectedDrops,
              b.assembler.packetsReceived + b.injectedDrops)
        << "两次收到的包总数就不一样, 后面的比较没有意义";
    EXPECT_EQ(a.injectedDrops, b.injectedDrops)
        << "同一个种子跑两遍丢的包数不同 —— 可复现性在管线这一级失效了";
    EXPECT_EQ(a.assembler.packetsReceived, b.assembler.packetsReceived);
}

/**
 * 换种子必须换一批包。
 *
 * 丢的**数量**会接近(都是 30%)，所以数量相等不能作为判据；这里比的是
 * "组包器实际收到了哪些"的下游后果 —— 完整帧数。同样的丢包率下，
 * 丢在不同位置，能拼齐的帧就不同。
 */
TEST(ReceiverE2E, ADifferentSeedDropsADifferentSetOfPackets) {
    constexpr int FRAMES = 40;

    auto runWithSeed = [](uint32_t seed) {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.idleTimeoutMs = 400;
        cfg.loss.seed = seed;
        cfg.loss.lossPercent = 30;
        ReceiverPipeline receiver(cfg);
        EXPECT_TRUE(receiver.open().isOk());

        std::atomic<bool> recvStop{false};
        auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });
        SenderPipeline sender(std::make_unique<NullSource>(),
                              senderConfig(receiver.boundPort(), FRAMES));
        std::atomic<bool> sendStop{false};
        EXPECT_TRUE(sender.run(sendStop).isOk());
        EXPECT_EQ(recvDone.wait_for(10s), std::future_status::ready);
        EXPECT_TRUE(recvDone.get().isOk());
        return receiver.stats();
    };

    const ReceiverPipelineStats a = runWithSeed(11111);
    const ReceiverPipelineStats b = runWithSeed(99999);

    ASSERT_GT(a.injectedDrops, 0u);
    ASSERT_GT(b.injectedDrops, 0u);
    // 两个种子丢掉同样多的包、又拼出同样多的完整帧, 概率极低 ——
    // 真出现了, 第一个要怀疑的是 seed 根本没参与混合
    EXPECT_FALSE(a.injectedDrops == b.injectedDrops &&
                 a.framesWritten == b.framesWritten)
        << "换了种子, 丢包数和成帧数都一模一样 —— seed 大概率没起作用";
}

/**
 * 注入器开着的时候，畸形包统计不能被它吃掉。
 *
 * 注入器为了拿 seq 会先解一次包头。解不出来的包必须**原样交给** FrameAssembler ——
 * 在注入器这一层就丢掉的话，packetsMalformed 恒为 0，而那是排查"对端在乱发还是
 * 版本对不上"的唯一线索。测试工具吃掉诊断信息，比测试工具本身出错更难查。
 */
TEST(ReceiverE2E, InjectionDoesNotSwallowMalformedPackets) {
    ReceiverPipelineConfig cfg = receiverConfig("");  // 纯落盘, 不需要解码渲染
    cfg.idleTimeoutMs = 400;
    cfg.loss.seed = 4242;
    cfg.loss.lossPercent = 50;  // 一半的合法包会被丢, 垃圾包一个都不该被丢
    ReceiverPipeline receiver(cfg);
    ASSERT_TRUE(receiver.open().isOk());
    const uint16_t port = receiver.boundPort();

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    // 版本号和类型都不认识的垃圾, decodePacketHeader 必然失败
    UdpSocket sock;
    ASSERT_TRUE(sock.open().isOk());
    const std::vector<uint8_t> garbage(64, 0xEE);
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(sock.sendTo(Endpoint{"127.0.0.1", port}, garbage.data(), garbage.size()).isOk());
    }

    ASSERT_EQ(recvDone.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(recvDone.get().isOk());

    const ReceiverPipelineStats& rs = receiver.stats();
    EXPECT_EQ(rs.injectedDrops, 0u) << "解不出包头的包不该由注入器处置";
    EXPECT_EQ(rs.assembler.packetsMalformed, 20u)
        << "畸形包被注入器吃掉了 —— 诊断信息没了";
}

/**
 * --loss 给了但 --seed 没给，必须在 open() 就报错。
 *
 * 这个组合会静默空转: 你以为测了 N% 丢包, 其实一个包都没丢, 然后得出
 * "弱网下表现很好"的结论。测试工具最不该有的就是这个 —— 宁可吵, 不可静。
 */
TEST(ReceiverE2E, LossWithoutASeedIsRejectedAtOpen) {
    {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.loss.lossPercent = 10;
        cfg.loss.seed = LOSS_SEED_DISABLED;
        ReceiverPipeline receiver(cfg);
        EXPECT_EQ(receiver.open().code(), Code::InvalidArg);
    }
    {   // 反过来是合法的: 种子给了、丢包率为 0 = 注入器关着
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.loss.lossPercent = 0;
        cfg.loss.seed = 12345;
        ReceiverPipeline receiver(cfg);
        EXPECT_TRUE(receiver.open().isOk())
            << "脚本里把 --seed 写死、只调 --loss 是正常用法";
    }
    {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.loss.lossPercent = 101;
        cfg.loss.seed = 12345;
        ReceiverPipeline receiver(cfg);
        EXPECT_EQ(receiver.open().code(), Code::InvalidArg);
    }
}
