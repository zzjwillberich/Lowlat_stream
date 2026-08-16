/**
 * @file    test_transport_loopback.cpp
 * @brief   M2 验收: sender -> UDP 回环 -> receiver, 两端码流逐字节一致
 * @author  zzj
 * @date    2026-08-15
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "app/receiver/ReceiverPipeline.h"
#include "app/sender/SenderPipeline.h"
#include "modules/capture/NullSource.h"

using namespace std::chrono_literals;

namespace {
    constexpr int FRAMES = 30;

    class TempPair {
    public:
        explicit TempPair(const std::string& caseName) {
            const auto dir = std::filesystem::temp_directory_path();
            const std::string prefix = "lowlat_loopback_" + std::to_string(::getpid()) + "_" + caseName;
            sent = dir / (prefix + "_send.h264");
            received = dir / (prefix + "_recv.h264");
            std::filesystem::remove(sent);
            std::filesystem::remove(received);
        }
        ~TempPair() {
            std::error_code ignored;
            std::filesystem::remove(sent, ignored);
            std::filesystem::remove(received, ignored);
        }
        std::filesystem::path sent;
        std::filesystem::path received;
    };

    std::vector<uint8_t> readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }

    SenderPipelineConfig senderConfig(uint16_t targetPort, const std::string& dump) {
        SenderPipelineConfig cfg;
        cfg.source.width = 320;
        cfg.source.height = 240;
        cfg.source.fps = 1000;  // 不真的按 30fps 睡, 测试要快

        cfg.encoder.width = cfg.source.width;
        cfg.encoder.height = cfg.source.height;
        cfg.encoder.fps = cfg.source.fps;
        cfg.encoder.bitrateKbps = 1000;
        cfg.encoder.gop = 10;

        cfg.queueCapacity = 4;
        cfg.sendQueueCapacity = 4;
        cfg.maxFrames = FRAMES;
        cfg.h264DumpPath = dump;
        cfg.target = Endpoint{"127.0.0.1", targetPort};
        return cfg;
    }
}  // namespace

/**
 * M2 的验收标准: 本机回环、无丢包的条件下, 接收端还原出的码流必须和发送端
 * dump 出来的**逐字节一致**。
 *
 * 用一致性而不是"能播放"作为判据: 播放器对残缺码流很宽容, 花几帧屏照样播下去,
 * 靠肉眼看根本发现不了少了一片。字节比对是唯一能把"看起来对"和"真的对"分开的东西。
 */
TEST(TransportLoopback, ReceiverReproducesTheSenderStreamByteForByte) {
    TempPair files("byte_identical");

    ReceiverPipelineConfig recvCfg;
    recvCfg.listen = Endpoint{"127.0.0.1", 0};
    recvCfg.h264DumpPath = files.received.string();
    recvCfg.recvTimeoutMs = 50;
    recvCfg.idleTimeoutMs = 800;  // sender 发完就退出了, receiver 靠空闲判断收尾
    ReceiverPipeline receiver(recvCfg);
    ASSERT_TRUE(receiver.open().isOk());

    std::atomic<bool> recvStop{false};
    auto recvDone = std::async(std::launch::async, [&] { return receiver.run(recvStop); });

    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(receiver.boundPort(), files.sent.string()));
    std::atomic<bool> sendStop{false};
    ASSERT_TRUE(sender.run(sendStop).isOk());

    ASSERT_EQ(recvDone.wait_for(10s), std::future_status::ready);
    ASSERT_TRUE(recvDone.get().isOk());

    const auto sentBytes = readFile(files.sent);
    const auto receivedBytes = readFile(files.received);
    ASSERT_FALSE(sentBytes.empty()) << "发送端一个字节都没编出来, 先查 M1";
    EXPECT_EQ(receivedBytes, sentBytes);

    const SenderPipelineStats& ss = sender.stats();
    const ReceiverPipelineStats& rs = receiver.stats();
    EXPECT_EQ(ss.sendErrors, 0u) << "回环上不该发不出去";
    EXPECT_EQ(rs.framesWritten, ss.encodedFrames);
    EXPECT_EQ(rs.assembler.packetsReceived, ss.packetsSent);
    EXPECT_EQ(rs.assembler.packetsLost(), 0u) << "回环上丢包一定是自己写错了, 不是网络";
    EXPECT_EQ(rs.assembler.packetsMalformed, 0u);
}

/**
 * 不给 target 时必须保持 M1 的行为: 只 dump 不发送。
 *
 * 留着这条无网络的路径, 是为了出问题时能一句话把范围劈成两半 ——
 * 关掉发送还坏就是采集/编码, 关掉就好就是传输。
 */
TEST(TransportLoopback, SenderWithoutATargetStillOnlyDumpsLocally) {
    TempPair files("no_target");

    SenderPipelineConfig cfg = senderConfig(0, files.sent.string());
    cfg.target = Endpoint{};  // ip 为空 = 不发送

    SenderPipeline sender(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};
    ASSERT_TRUE(sender.run(stop).isOk());

    EXPECT_GT(sender.stats().encodedFrames, 0u);
    EXPECT_EQ(sender.stats().packetsSent, 0u);
    EXPECT_FALSE(readFile(files.sent).empty());
}

/**
 * 目标端口没人监听时, sender 不能挂掉也不能卡住。
 *
 * UDP 是无连接的, 发给一个没人听的端口通常"成功"(内核不知道对端在不在),
 * 但本机回环会回一个 ICMP port unreachable, 下一次 sendto 就可能报 ECONNREFUSED。
 * 这正是"单包失败不中断管线"要挡的情况 —— 否则接收端晚启动一秒, 发送端就死了。
 */
TEST(TransportLoopback, SenderSurvivesADeadTarget) {
    TempPair files("dead_target");

    // 先绑一个端口拿到号, 立刻放掉, 保证这个端口大概率没人听
    uint16_t deadPort = 0;
    {
        UdpSocket probe;
        ASSERT_TRUE(probe.open().isOk());
        ASSERT_TRUE(probe.bind(Endpoint{"127.0.0.1", 0}).isOk());
        deadPort = probe.localEndpoint().port;
    }
    ASSERT_NE(deadPort, 0u);

    SenderPipeline sender(std::make_unique<NullSource>(),
                          senderConfig(deadPort, files.sent.string()));
    std::atomic<bool> stop{false};
    EXPECT_TRUE(sender.run(stop).isOk()) << "对端不在不是发送端的错误";
    EXPECT_EQ(sender.stats().encodedFrames, static_cast<uint64_t>(FRAMES));
}
