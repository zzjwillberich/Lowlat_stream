/**
 * @file    test_receiver_pipeline.cpp
 * @brief   M2 receiver 收包管线的契约测试, 全部跑在回环上
 * @author  zzj
 * @date    2026-08-15
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "app/receiver/ReceiverPipeline.h"
#include "common/Clock.h"
#include "modules/transport/Packet.h"
#include "modules/transport/Packetizer.h"
#include "modules/transport/UdpSocket.h"

using namespace std::chrono_literals;

namespace {
    class TempFile {
    public:
        explicit TempFile(const std::string& caseName) {
            path = std::filesystem::temp_directory_path() /
                   ("lowlat_recv_" + std::to_string(::getpid()) + "_" + caseName + ".h264");
            std::filesystem::remove(path);
        }
        ~TempFile() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        std::filesystem::path path;
    };

    ReceiverPipelineConfig receiverConfig(const std::string& dump = "") {
        ReceiverPipelineConfig cfg;
        cfg.listen = Endpoint{"127.0.0.1", 0};  // 端口交给内核, 写死会在 CI 上撞车
        cfg.h264DumpPath = dump;
        cfg.recvTimeoutMs = 50;
        cfg.idleTimeoutMs = 500;
        return cfg;
    }

    std::vector<uint8_t> fakeStream(size_t len) {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) {
            v[i] = static_cast<uint8_t>(i * 7 + 1);
        }
        return v;
    }

    std::vector<uint8_t> readFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }

    /** 把一帧切片后按给定顺序发到 port; order 为空表示顺序发 */
    void sendFrame(uint16_t port, const std::vector<uint8_t>& data, uint32_t frameId,
                   bool isKey = false, const std::vector<size_t>& order = {},
                   size_t skipIndex = SIZE_MAX) {
        Packetizer packer(0, frameId * 100);
        std::vector<PacketBuffer> packets;
        EncodedFrameView view;
        view.data = data.data();
        view.len = data.size();
        view.frameId = frameId;
        view.captureMs = 4242;
        view.isKey = isKey;
        ASSERT_TRUE(packer.packetize(view, packets).isOk());

        UdpSocket sock;
        ASSERT_TRUE(sock.open().isOk());
        const Endpoint to{"127.0.0.1", port};

        std::vector<size_t> indices = order;
        if (indices.empty()) {
            for (size_t i = 0; i < packets.size(); ++i) indices.push_back(i);
        }
        for (size_t i : indices) {
            if (i == skipIndex) continue;
            ASSERT_TRUE(sock.sendTo(to, packets[i].data(), packets[i].size()).isOk());
        }
    }
}  // namespace

// ---------- 配置与生命周期 ----------

TEST(ReceiverPipeline, RejectsInvalidConfig) {
    {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.recvTimeoutMs = 0;  // 忙轮询, 会烧满一个核
        ReceiverPipeline pipeline(cfg);
        EXPECT_EQ(pipeline.open().code(), Code::InvalidArg);
    }
    {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.maxFrames = -1;
        ReceiverPipeline pipeline(cfg);
        EXPECT_EQ(pipeline.open().code(), Code::InvalidArg);
    }
    {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.maxPendingFrames = 0;
        ReceiverPipeline pipeline(cfg);
        EXPECT_EQ(pipeline.open().code(), Code::InvalidArg);
    }
}

TEST(ReceiverPipeline, RunBeforeOpenIsClosed) {
    ReceiverPipeline pipeline(receiverConfig());
    std::atomic<bool> stop{false};
    EXPECT_EQ(pipeline.run(stop).code(), Code::Closed);
}

TEST(ReceiverPipeline, OpenReportsTheKernelAssignedPort) {
    ReceiverPipeline pipeline(receiverConfig());
    EXPECT_EQ(pipeline.boundPort(), 0u) << "open 之前不该有端口";
    ASSERT_TRUE(pipeline.open().isOk());
    // 端口就绪必须是个可等待的时刻, 否则测试只能 sleep 猜 —— 慢机器上必然偶发失败
    EXPECT_NE(pipeline.boundPort(), 0u);
}

TEST(ReceiverPipeline, ExitsOnIdleTimeoutWhenNobodySends) {
    ReceiverPipelineConfig cfg = receiverConfig();
    cfg.idleTimeoutMs = 150;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());

    std::atomic<bool> stop{false};
    const uint64_t start = steadyNowMs();
    EXPECT_TRUE(pipeline.run(stop).isOk()) << "空闲退出是正常收尾, 不是错误";
    const uint64_t elapsed = steadyNowMs() - start;
    EXPECT_GE(elapsed, 100u);
    EXPECT_LT(elapsed, 3000u);
    EXPECT_EQ(pipeline.stats().framesWritten, 0u);
}

TEST(ReceiverPipeline, StopRequestExitsPromptly) {
    ReceiverPipelineConfig cfg = receiverConfig();
    cfg.idleTimeoutMs = 0;  // 不靠空闲退出, 只能靠停止标志
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    std::this_thread::sleep_for(100ms);
    stop.store(true);
    // recvTimeoutMs 决定停止响应的最坏延迟; 无限阻塞的话这里会挂死
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready) << "收包循环没有定期检查停止标志";
    EXPECT_TRUE(done.get().isOk());
}

// ---------- 收包与组包 ----------

TEST(ReceiverPipeline, WritesAFrameThatArrivedInOrder) {
    TempFile dump("in_order");
    ReceiverPipelineConfig cfg = receiverConfig(dump.path.string());
    cfg.maxFrames = 1;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    const std::vector<uint8_t> frame = fakeStream(5000);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    sendFrame(port, frame, 1, true);

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());
    EXPECT_EQ(pipeline.stats().framesWritten, 1u);
    EXPECT_EQ(pipeline.stats().keyFrames, 1u);
    EXPECT_EQ(readFile(dump.path), frame) << "落盘的必须和发出的逐字节一致";
}

TEST(ReceiverPipeline, WritesAFrameThatArrivedOutOfOrder) {
    TempFile dump("out_of_order");
    ReceiverPipelineConfig cfg = receiverConfig(dump.path.string());
    cfg.maxFrames = 1;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    const std::vector<uint8_t> frame = fakeStream(MAX_PAYLOAD * 3);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    sendFrame(port, frame, 1, false, {2, 0, 1});  // 乱序在 UDP 上是常态

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());
    EXPECT_EQ(readFile(dump.path), frame);
}

TEST(ReceiverPipeline, AFrameMissingAFragmentIsNeverWritten) {
    TempFile dump("missing_fragment");
    ReceiverPipelineConfig cfg = receiverConfig(dump.path.string());
    cfg.idleTimeoutMs = 300;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    const std::vector<uint8_t> frame = fakeStream(MAX_PAYLOAD * 3);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    sendFrame(port, frame, 1, false, {}, /*skipIndex=*/1);  // 中间那片丢了

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());
    // 宁可不出帧也不能出半帧: 缺一片的码流喂进解码器是花屏, 而且看不出是网络丢的
    EXPECT_EQ(pipeline.stats().framesWritten, 0u);
    EXPECT_EQ(readFile(dump.path).size(), 0u);
    EXPECT_GT(pipeline.stats().assembler.packetsLost(), 0u);
}

TEST(ReceiverPipeline, KeepsRunningAfterAGarbagePacket) {
    TempFile dump("garbage");
    ReceiverPipelineConfig cfg = receiverConfig(dump.path.string());
    cfg.maxFrames = 1;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    // 端口上收到垃圾是常态(扫描器、旧版本对端)。为一个坏包退出整个接收端,
    // 等于把对端的 bug 变成自己的可用性问题
    UdpSocket sock;
    ASSERT_TRUE(sock.open().isOk());
    const std::vector<uint8_t> garbage(64, 0xEE);
    ASSERT_TRUE(sock.sendTo(Endpoint{"127.0.0.1", port}, garbage.data(), garbage.size()).isOk());

    const std::vector<uint8_t> frame = fakeStream(300);
    sendFrame(port, frame, 1);

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());
    EXPECT_EQ(readFile(dump.path), frame) << "坏包之后正常的帧还得收得上来";
    EXPECT_GT(pipeline.stats().assembler.packetsMalformed, 0u);
}

TEST(ReceiverPipeline, StopsAfterMaxFrames) {
    ReceiverPipelineConfig cfg = receiverConfig();
    cfg.maxFrames = 2;
    cfg.idleTimeoutMs = 0;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    for (uint32_t i = 1; i <= 4; ++i) {
        sendFrame(port, fakeStream(200), i);
    }

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready) << "收满 maxFrames 必须自己退出";
    ASSERT_TRUE(done.get().isOk());
    EXPECT_EQ(pipeline.stats().framesWritten, 2u);
}

TEST(ReceiverPipeline, CountsFramesEvenWithoutADumpFile) {
    ReceiverPipelineConfig cfg = receiverConfig();  // 不给 dump 路径
    cfg.maxFrames = 1;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    sendFrame(port, fakeStream(300), 1);

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());
    // 不写盘不等于没收到 —— 去掉 --dump 之后统计全是 0 的话, 压测时就没法看了
    EXPECT_EQ(pipeline.stats().framesWritten, 1u);
    EXPECT_GT(pipeline.stats().bytesWritten, 0u);
}
