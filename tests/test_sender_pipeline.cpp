/**
 * @file    test_sender_pipeline.cpp
 * @brief   M1.4 sender 两线程管线验收测试
 * @author  zzj
 * @date    2026-08-04
 */

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "app/sender/SenderPipeline.h"
#include "common/Clock.h"
#include "modules/capture/NullSource.h"
#include "modules/transport/Packet.h"
#include "modules/transport/UdpSocket.h"

using namespace std::chrono_literals;

namespace {
    class TempOutputs {
    public:
        explicit TempOutputs(const std::string& caseName) {
            const std::string prefix = "lowlat_pipeline_" + std::to_string(::getpid()) + "_" + caseName;
            raw = std::filesystem::temp_directory_path() / (prefix + ".yuv");
            h264 = std::filesystem::temp_directory_path() / (prefix + ".h264");
            std::filesystem::remove(raw);
            std::filesystem::remove(h264);
        }

        ~TempOutputs() {
            std::error_code ignored;
            std::filesystem::remove(raw, ignored);
            std::filesystem::remove(h264, ignored);
        }

        std::filesystem::path raw;
        std::filesystem::path h264;
    };

    SenderPipelineConfig pipelineConfig(int frames = 30, int fps = 1000, int cap = 4) {
        SenderPipelineConfig cfg;
        cfg.source.width = 320;
        cfg.source.height = 240;
        cfg.source.fps = fps;

        cfg.encoder.width = cfg.source.width;
        cfg.encoder.height = cfg.source.height;
        cfg.encoder.fps = cfg.source.fps;
        cfg.encoder.bitrateKbps = 1000;
        cfg.encoder.gop = 10;

        cfg.queueCapacity = cap;
        cfg.maxFrames = frames;
        return cfg;
    }

    /**
     * 模拟“驱动没有接受请求分辨率”的采集源。
     *
     * open 请求 640x480，但实际只提供 320x240。管线必须在 source open 后读取
     * actualConfig()，再以 320x240 打开 encoder；继续使用请求值会在 encode() 时报尺寸不匹配。
     */
    class NegotiatingSource : public ISource {
    public:
        Status open(const SourceConfig& requested) override {
            actual_ = requested;
            actual_.width = 320;
            actual_.height = 240;
            opened_ = true;
            frameId_ = 0;
            return Status::ok();
        }

        const SourceConfig& actualConfig() const override { return actual_; }

        Status readFrame(RawFrame& out) override {
            if (!opened_) return Status::error(Code::Closed, "NegotiatingSource: not opened");

            out.reset(actual_.width, actual_.height);
            std::fill(out.data.begin(), out.data.end(), static_cast<uint8_t>(128));
            out.captureMs = steadyNowMs();
            out.frameId = frameId_++;
            return Status::ok();
        }

        void close() override { opened_ = false; }

    private:
        SourceConfig actual_;
        bool opened_ = false;
        uint64_t frameId_ = 0;
    };
}  // namespace

TEST(SenderPipeline, RejectsInvalidQueueCapacity) {
    SenderPipelineConfig cfg = pipelineConfig();
    cfg.queueCapacity = 0;

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
}

TEST(SenderPipeline, RejectsAnIncompleteSendTarget) {
    {
        SenderPipelineConfig cfg = pipelineConfig();
        cfg.target = Endpoint{"127.0.0.1", 0};  // 端口没填
        SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
        const std::atomic<bool> stop{false};
        // 让它在启动时炸掉, 而不是跑起来之后每个包都失败
        EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
    }
    {
        SenderPipelineConfig cfg = pipelineConfig();
        cfg.target = Endpoint{"127.0.0.1", 9000};
        cfg.sendQueueCapacity = 0;  // 同 queueCapacity, 负数/0 会变成巨大容量
        SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
        const std::atomic<bool> stop{false};
        EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
    }
}

TEST(SenderPipeline, CapturesEncodesAndDrainsEveryFrame) {
    constexpr int FRAMES = 30;
    TempOutputs output("drain");
    SenderPipelineConfig cfg = pipelineConfig(FRAMES);
    cfg.rawDumpPath = output.raw.string();
    cfg.h264DumpPath = output.h264.string();

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    ASSERT_TRUE(pipeline.run(stop).isOk());
    const SenderPipelineStats& stats = pipeline.stats();

    EXPECT_EQ(stats.capturedFrames, static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(stats.encodedFrames, stats.capturedFrames)
        << "队列关闭后必须先排空残留帧，再结束编码线程";
    EXPECT_GT(stats.encodedBytes, 0u);
    EXPECT_GT(stats.keyFrames, 0u);
    EXPECT_GT(stats.queuePeak, 0u);
    EXPECT_LE(stats.queuePeak, static_cast<size_t>(cfg.queueCapacity));

    ASSERT_TRUE(std::filesystem::exists(output.raw));
    ASSERT_TRUE(std::filesystem::exists(output.h264));
    EXPECT_EQ(std::filesystem::file_size(output.raw),
              frameBytes(cfg.source.width, cfg.source.height) * static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(std::filesystem::file_size(output.h264), stats.encodedBytes);
}

TEST(SenderPipeline, StopRequestExitsWithinOneSecondAndDrainsQueue) {
    SenderPipelineConfig cfg = pipelineConfig(0, 30);  // 0 表示持续运行，等待停止标志
    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};

    auto result = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    std::this_thread::sleep_for(100ms);
    stop.store(true);

    ASSERT_EQ(result.wait_for(1s), std::future_status::ready)
        << "停止请求后 1 秒内必须完成 close、drain、flush 和 join";
    EXPECT_TRUE(result.get().isOk());
    EXPECT_EQ(pipeline.stats().capturedFrames, pipeline.stats().encodedFrames);
}

TEST(SenderPipeline, CannotRunTwiceAfterQueueHasClosed) {
    SenderPipelineConfig cfg = pipelineConfig(3);
    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    ASSERT_TRUE(pipeline.run(stop).isOk());
    EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
}

TEST(SenderPipeline, OpensEncoderWithSourceNegotiatedGeometry) {
    constexpr int FRAMES = 5;
    SenderPipelineConfig cfg = pipelineConfig(FRAMES);
    cfg.source.width = 640;
    cfg.source.height = 480;
    cfg.encoder.width = cfg.source.width;
    cfg.encoder.height = cfg.source.height;

    SenderPipeline pipeline(std::make_unique<NegotiatingSource>(), cfg);
    const std::atomic<bool> stop{false};

    ASSERT_TRUE(pipeline.run(stop).isOk())
        << "source open 后应使用 actualConfig() 覆盖 encoder 的宽高和帧率";
    EXPECT_EQ(pipeline.stats().capturedFrames, static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(pipeline.stats().encodedFrames, static_cast<uint64_t>(FRAMES));
}


// ========== M4.4 反向通道按类型分流 ==========

namespace {
    /**
     * 一个假接收端: 绑一个口, 收 sender 发来的数据包, 并能往回发裸包。
     *
     * 用真 socket 而不是直接调 drainReverseChannel(), 是因为要测的恰恰是
     * **"从线上来的字节"** 这条路径 —— 类型分流、长度校验都只在那里发生。
     */
    class FakePeer {
    public:
        Status open() {
            Status st = sock_.open();
            if (!st.isOk()) return st;
            return sock_.bind(Endpoint{"127.0.0.1", 0});
        }

        uint16_t port() const { return sock_.localEndpoint().port; }

        /** @brief 收一个包; 返回 false 表示超时 */
        bool recv(std::vector<uint8_t>& out, int timeoutMs) {
            out.resize(MAX_PACKET_SIZE);
            size_t len = 0;
            const Status st = sock_.recvFrom(out.data(), out.size(), len, from_, timeoutMs);
            if (!st.isOk()) return false;
            out.resize(len);
            seenPeer_ = true;
            return true;
        }

        bool sendRaw(const uint8_t* data, size_t len) {
            return seenPeer_ && sock_.sendTo(from_, data, len).isOk();
        }

    private:
        UdpSocket sock_;
        Endpoint from_;
        bool seenPeer_ = false;
    };

    /** @brief 组一个只有通用头、没有载荷的包 */
    std::vector<uint8_t> makeHeaderOnly(PacketType type) {
        std::vector<uint8_t> buf(PACKET_HEADER_SIZE);
        PacketHeader h;
        h.type = type;
        h.streamId = 0;
        h.seq = 1;
        h.timestampMs = 0;
        EXPECT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());
        return buf;
    }

    /** @brief 这个包是不是一个关键帧分片 */
    bool isKeyFragment(const std::vector<uint8_t>& pkt) {
        PacketHeader h;
        if (!decodePacketHeader(pkt.data(), pkt.size(), h).isOk()) return false;
        if (h.type != PacketType::Data) return false;
        DataHeader d;
        if (!decodeDataHeader(pkt.data() + PACKET_HEADER_SIZE,
                              pkt.size() - PACKET_HEADER_SIZE, d).isOk()) {
            return false;
        }
        return (d.flags & DataHeader::FLAG_KEYFRAME) != 0;
    }
}  // namespace

/**
 * 一个只有通用头的 Pli 包必须走到 encoder_.requestKeyFrame(), 并计入 plisReceived。
 *
 * 这条钉的是 drainReverseChannel 的**类型分流**。少了这一支的现象是
 * "PLI 发出去了、对端也收到了, 但画面还是花着" —— 而接收端的 pli_sent 是正常的,
 * 排查时很容易一直盯着接收端。
 */
TEST(SenderPipeline, APliOnTheReverseChannelForcesAKeyFrame) {
    FakePeer peer;
    ASSERT_TRUE(peer.open().isOk());

    SenderPipelineConfig cfg = pipelineConfig(/*frames=*/400, /*fps=*/1000);
    cfg.encoder.gop = 100000;  // 大到跑完都不会自己来第二个 IDR
    cfg.target = Endpoint{"127.0.0.1", peer.port()};

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    // 先把第一个(自然的)IDR 整个收完, 再发 PLI
    std::vector<uint8_t> pkt;
    int nonKeyRun = 0;
    while (nonKeyRun < 20 && peer.recv(pkt, 2000)) {
        nonKeyRun = isKeyFragment(pkt) ? 0 : nonKeyRun + 1;
    }
    ASSERT_EQ(nonKeyRun, 20) << "没等到稳定的 P 帧段, 后面的断言就不成立";

    const std::vector<uint8_t> pli = makeHeaderOnly(PacketType::Pli);
    ASSERT_TRUE(peer.sendRaw(pli.data(), pli.size()));

    bool sawKeyAfterPli = false;
    for (int i = 0; i < 400 && peer.recv(pkt, 2000); ++i) {
        if (isKeyFragment(pkt)) {
            sawKeyAfterPli = true;
            break;
        }
    }

    stop.store(true);
    done.wait();

    EXPECT_TRUE(sawKeyAfterPli) << "收到 PLI 之后没有出现新的 IDR";
    EXPECT_EQ(pipeline.stats().plisReceived, 1u);
    EXPECT_EQ(pipeline.stats().reverseMalformed, 0u);
}

/**
 * 带载荷的 Pli 是畸形包: 不许触发 IDR, 要计入 reverseMalformed。
 *
 * PLI 没有载荷, 多出来的字节只可能是别人在往这个端口上发东西, 或者上一轮的残留。
 * 不查的话它会安静地当成合法 PLI 触发一个 IDR —— 而 IDR 大约是 P 帧的
 * 二十几倍大小, 一个乱发包的进程就能把码率顶穿。
 */
TEST(SenderPipeline, APliWithATrailingPayloadIsRejected) {
    FakePeer peer;
    ASSERT_TRUE(peer.open().isOk());

    SenderPipelineConfig cfg = pipelineConfig(/*frames=*/200, /*fps=*/1000);
    cfg.encoder.gop = 100000;
    cfg.target = Endpoint{"127.0.0.1", peer.port()};

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    std::vector<uint8_t> pkt;
    ASSERT_TRUE(peer.recv(pkt, 2000));

    std::vector<uint8_t> bad = makeHeaderOnly(PacketType::Pli);
    bad.push_back(0xAB);  // 一个字节的赘肉就够
    ASSERT_TRUE(peer.sendRaw(bad.data(), bad.size()));

    for (int i = 0; i < 100 && peer.recv(pkt, 1000); ++i) {
    }

    stop.store(true);
    done.wait();

    EXPECT_EQ(pipeline.stats().plisReceived, 0u);
    EXPECT_GE(pipeline.stats().reverseMalformed, 1u);
}

/**
 * 反向通道上来一个既不是 Nack 也不是 Pli 的包 —— 计 malformed, 不许崩。
 *
 * 监听的是发送端自己的端口, 谁都能往上面发东西。
 */
TEST(SenderPipeline, AnUnknownReversePacketTypeIsCountedNotObeyed) {
    FakePeer peer;
    ASSERT_TRUE(peer.open().isOk());

    SenderPipelineConfig cfg = pipelineConfig(/*frames=*/200, /*fps=*/1000);
    cfg.encoder.gop = 100000;
    cfg.target = Endpoint{"127.0.0.1", peer.port()};

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    std::vector<uint8_t> pkt;
    ASSERT_TRUE(peer.recv(pkt, 2000));

    const std::vector<uint8_t> data = makeHeaderOnly(PacketType::Data);
    ASSERT_TRUE(peer.sendRaw(data.data(), data.size()));

    for (int i = 0; i < 100 && peer.recv(pkt, 1000); ++i) {
    }

    stop.store(true);
    done.wait();

    EXPECT_EQ(pipeline.stats().plisReceived, 0u);
    EXPECT_GE(pipeline.stats().reverseMalformed, 1u);
}
