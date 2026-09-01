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
        // 这些是 M2 的落盘契约测试：既不需要也不能依赖 SDL/解码器。
        cfg.renderKind = "";
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
    // 这里**不能**用 maxFrames 收尾。垃圾包和数据帧来自两个不同的 socket
    // (sendFrame 自己开一个), 源端口不同就是两条流, 回环上到达顺序没有保证。
    // maxFrames=1 会让接收端在写完帧的那一刻立刻退出, 约 1/4 的情况下垃圾包
    // 还躺在内核缓冲里没被 recvFrom 取过 —— packetsMalformed 于是恒为 0。
    // 用 idleTimeoutMs 收尾: 静默 500ms 才退, 保证收到的东西都被读干净了。
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

// ---------- M4.1 反向通道: 对端地址跟踪 ----------

namespace {
    /** 和 sendFrame 一样发一帧, 但用调用方给的 socket —— 为了控制**源端口** */
    void sendFrameFrom(UdpSocket& sock, uint16_t port, const std::vector<uint8_t>& data,
                       uint32_t frameId) {
        Packetizer packer(0, frameId * 100);
        std::vector<PacketBuffer> packets;
        EncodedFrameView view;
        view.data = data.data();
        view.len = data.size();
        view.frameId = frameId;
        view.captureMs = 4242;
        view.isKey = true;
        ASSERT_TRUE(packer.packetize(view, packets).isOk());

        const Endpoint to{"127.0.0.1", port};
        for (const PacketBuffer& packet : packets) {
            ASSERT_TRUE(sock.sendTo(to, packet.data(), packet.size()).isOk());
        }
    }

    /** 开一个绑到随机端口的 socket, 这样源端口在发之前就是已知的 */
    void openBound(UdpSocket& sock) {
        ASSERT_TRUE(sock.open().isOk());
        ASSERT_TRUE(sock.bind(Endpoint{"127.0.0.1", 0}).isOk());
    }
}  // namespace

TEST(ReceiverPipeline, LearnsThePeerAddressFromDataPackets) {
    ReceiverPipelineConfig cfg = receiverConfig();
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    EXPECT_EQ(pipeline.peer().port, 0u) << "还没收到包就认定了对端";
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    UdpSocket sender;
    openBound(sender);
    const uint16_t senderPort = sender.localEndpoint().port;
    ASSERT_NE(senderPort, 0u);
    sendFrameFrom(sender, port, fakeStream(300), 1);

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());

    EXPECT_EQ(pipeline.peer().ip, "127.0.0.1");
    EXPECT_EQ(pipeline.peer().port, senderPort)
        << "反向通道没有目的地, NACK 和 PLI 都发不出去";
}

/**
 * 这条是"每个合法 DATA 包都更新"这个决定的**全部理由**。
 *
 * 发送端每次启动的源端口都由内核随机分配(UdpSocket::open() 不 bind), 而
 * "接收端挂着不动、反复跑 sender"是 M4 调参的标准工作流。锁定第一个源不再改的话,
 * 第二次开始 NACK 全发到一个死端口 —— 而 sendTo 到没人监听的端口**不报错**,
 * 统计和日志里一点痕迹都没有, 表现为"重传怎么一次都不成功"。
 */
TEST(ReceiverPipeline, FollowsThePeerWhenTheSenderRestartsOnANewPort) {
    ReceiverPipelineConfig cfg = receiverConfig();
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    // 第一次"运行"
    UdpSocket first;
    openBound(first);
    const uint16_t firstPort = first.localEndpoint().port;
    sendFrameFrom(first, port, fakeStream(300), 1);
    first.close();

    // 重启: 新进程 -> 新的源端口
    UdpSocket second;
    openBound(second);
    const uint16_t secondPort = second.localEndpoint().port;
    ASSERT_NE(firstPort, secondPort) << "两次拿到同一个端口, 这条用例测不到东西";
    sendFrameFrom(second, port, fakeStream(300), 2);

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());

    EXPECT_EQ(pipeline.peer().port, secondPort)
        << "对端锁死在第一个源上了, sender 重启之后反向通道就废了";
}

/**
 * 闸门: 只有**合法 DATA 包**才能认定对端。
 *
 * 端口上收到垃圾是常态(扫描器、旧版本对端)。让随便什么包都能设定 peer_,
 * 等于把反向通道的目的地交给任何一个往这个端口发过东西的人。
 */
TEST(ReceiverPipeline, GarbagePacketsDoNotBecomeThePeer) {
    ReceiverPipelineConfig cfg = receiverConfig();
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    UdpSocket noise;
    openBound(noise);
    const std::vector<uint8_t> garbage(64, 0xEE);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(noise.sendTo(Endpoint{"127.0.0.1", port}, garbage.data(), garbage.size())
                        .isOk());
    }

    ASSERT_EQ(done.wait_for(3s), std::future_status::ready);
    ASSERT_TRUE(done.get().isOk());

    EXPECT_GT(pipeline.stats().assembler.packetsMalformed, 0u) << "垃圾包压根没送到";
    EXPECT_EQ(pipeline.peer().port, 0u) << "垃圾包把反向通道的目的地设走了";
}


// ---------- M4.4 PLI 触发源 D: NACK 放弃了关键帧分片 ----------

namespace {
    /**
     * 用**同一个** Packetizer 连发多帧, 于是 seq 全程连续。
     *
     * 上面那个 sendFrame 每帧新建一个 Packetizer 且起始 seq = frameId*100,
     * 帧与帧之间会空出几十个 seq —— 对 NackTracker 来说那是几十个真缺口,
     * 一帧就够把缺口表撑爆。测 PLI 触发必须先把这个噪声去掉。
     */
    class SeqStream {
    public:
        SeqStream() : packer_(0, 1000) {
            EXPECT_TRUE(sock_.open().isOk());
        }

        /** @param skipIndex 不发的分片下标; SIZE_MAX 表示全发 */
        void send(uint16_t port, const std::vector<uint8_t>& data, uint32_t frameId,
                  bool isKey, size_t skipIndex = SIZE_MAX) {
            std::vector<PacketBuffer> packets;
            EncodedFrameView view;
            view.data = data.data();
            view.len = data.size();
            view.frameId = frameId;
            view.captureMs = 4242;
            view.isKey = isKey;
            ASSERT_TRUE(packer_.packetize(view, packets).isOk());

            const Endpoint to{"127.0.0.1", port};
            for (size_t i = 0; i < packets.size(); ++i) {
                if (i == skipIndex) continue;
                ASSERT_TRUE(sock_.sendTo(to, packets[i].data(), packets[i].size()).isOk());
            }
        }

    private:
        Packetizer packer_;
        UdpSocket sock_;
    };

    ReceiverPipelineConfig pliConfig() {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.idleTimeoutMs = 600;
        cfg.nack.windowPackets = 1024;
        cfg.nack.maxRequestsPerSeq = 1;  // 请求一次就放弃, 让 givenUp 快点发生
        cfg.fec.recentPackets = 0;       // 别让 FEC 把洞补上, 这里要的就是补不上
        cfg.pliMinIntervalMs = 1000;
        return cfg;
    }
}  // namespace

/**
 * NACK 对一个关键帧分片放弃之后, 必须发且**只发一次** PLI。
 *
 * keyFramesGivenUp 是**累计值**。直接判"非 0 就发"的话, 之后每收到一个包
 * 都会再走一次 requestKeyFrame —— 发不出去(被限流挡住), 但 pliSuppressed
 * 会随着包数线性涨。所以这条的判据不是 pliSent, 而是 **pliSuppressed**:
 * 它是电平触发和边沿触发唯一看得出区别的地方。
 */
TEST(ReceiverPipeline, GivingUpAKeyFrameFragmentSendsExactlyOnePli) {
    ReceiverPipeline pipeline(pliConfig());
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);  // 约 7 个分片
    SeqStream stream;

    stream.send(port, payload, 0, /*isKey=*/true);                    // 建立基线与对端
    stream.send(port, payload, 1, /*isKey=*/true, /*skipIndex=*/2);   // 关键帧缺一片
    for (uint32_t f = 2; f < 12; ++f) {                               // 推进 seq, 逼 NACK 放弃
        stream.send(port, payload, f, /*isKey=*/false);
        std::this_thread::sleep_for(5ms);
    }

    done.wait();
    const ReceiverPipelineStats& s = pipeline.stats();

    EXPECT_GE(s.nack.keyFramesGivenUp, 1u) << "构造没生效: 根本没放弃过关键帧分片";
    EXPECT_EQ(s.pliSent, 1u);
    EXPECT_LE(s.pliSuppressed, 1u)
        << "pliSuppressed=" << s.pliSuppressed
        << " —— 电平触发了: 放弃之后每收一个包都在重新请求关键帧";
}

/**
 * 上一条的对照: 一个洞都没有时**一个 PLI 都不许发**。
 *
 * 没有这条的话, 一个"无条件每帧发 PLI"的实现也能过上一条 —— 限流会把
 * pliSent 压到 1, pliSuppressed 也可能恰好很小。
 */
TEST(ReceiverPipeline, ACleanStreamNeverAsksForAKeyFrame) {
    ReceiverPipeline pipeline(pliConfig());
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);
    SeqStream stream;
    for (uint32_t f = 0; f < 12; ++f) {
        stream.send(port, payload, f, /*isKey=*/f == 0);
        std::this_thread::sleep_for(5ms);
    }

    done.wait();
    EXPECT_EQ(pipeline.stats().pliSent, 0u);
    EXPECT_EQ(pipeline.stats().pliSuppressed, 0u);
    EXPECT_EQ(pipeline.stats().nack.keyFramesGivenUp, 0u);
}

/**
 * 丢的是 P 帧分片时不该发 PLI —— NACK 兜得住的事情不要惊动 IDR。
 *
 * 一个 IDR 大约是 P 帧的二十几倍大小。丢一片 P 帧就要一个 IDR 的话,
 * 弱网下会正反馈: 丢包 -> IDR -> IDR 更大更容易丢 -> 再来一个 IDR。
 */
TEST(ReceiverPipeline, LosingANonKeyFragmentDoesNotEscalateToPli) {
    ReceiverPipeline pipeline(pliConfig());
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);
    SeqStream stream;

    stream.send(port, payload, 0, /*isKey=*/true);
    stream.send(port, payload, 1, /*isKey=*/false, /*skipIndex=*/2);
    for (uint32_t f = 2; f < 12; ++f) {
        stream.send(port, payload, f, /*isKey=*/false);
        std::this_thread::sleep_for(5ms);
    }

    done.wait();
    const ReceiverPipelineStats& s = pipeline.stats();

    EXPECT_GE(s.nack.givenUp, 1u) << "构造没生效: 这个洞根本没被放弃";
    EXPECT_EQ(s.nack.keyFramesGivenUp, 0u) << "丢的是 P 帧分片, 不该记成关键帧";
    EXPECT_EQ(s.pliSent, 0u);
}


// ---------- M4.6 RTT 实测（NACK 发出 -> 重传回来） ----------

namespace {
    /**
     * 一条能**回话**的流：发数据、收 NACK、按要求补发重传包。
     *
     * 前面那个 SeqStream 只管发。要测 RTT 就得扮演发送端的另一半 ——
     * 收到 NACK、等一段可控的时间、再把缺的那一片带着 FLAG_RETRANSMIT 送回去。
     * 这个"等一段时间"就是被测的量。
     */
    class TalkingStream {
    public:
        TalkingStream() : packer_(0, 1000) {
            EXPECT_TRUE(sock_.open().isOk());
            EXPECT_TRUE(sock_.bind(Endpoint{"127.0.0.1", 0}).isOk());
        }

        /**
         * @param skipIndex 不发的分片下标；SIZE_MAX 表示全发
         *
         * @note 扣下的那一片要**立刻**把补发所需的东西整份存下来。第一版把
         *       "最后一次 packetize 的结果"当成待补发的片, 结果后面那些
         *       用来推进 seq 的帧把它冲掉了, resendHeld 一个包都没发出去 ——
         *       而测试的表现是"RTT 一次都没量到", 看起来像被测代码的错。
         */
        void send(uint16_t port, const std::vector<uint8_t>& data, uint32_t frameId,
                  bool isKey, size_t skipIndex = SIZE_MAX) {
            std::vector<PacketBuffer> packets;
            EncodedFrameView view;
            view.data = data.data();
            view.len = data.size();
            view.frameId = frameId;
            view.captureMs = 4242;
            view.isKey = isKey;
            const uint32_t baseSeq = packer_.nextSeq();
            ASSERT_TRUE(packer_.packetize(view, packets).isOk());

            if (skipIndex != SIZE_MAX && skipIndex < packets.size()) {
                heldData_ = data;                       // 整份拷走, 不留悬垂指针
                heldFrameId_ = frameId;
                heldIsKey_ = isKey;
                heldSeq_ = baseSeq + static_cast<uint32_t>(skipIndex);
                heldIndex_ = static_cast<uint16_t>(skipIndex);
                heldCount_ = static_cast<uint16_t>(packets.size());
                hasHeld_ = true;
            }

            const Endpoint to{"127.0.0.1", port};
            for (size_t i = 0; i < packets.size(); ++i) {
                if (i == skipIndex) continue;
                ASSERT_TRUE(sock_.sendTo(to, packets[i].data(), packets[i].size()).isOk());
            }
        }

        /**
         * @brief 等一个 NACK，返回它请求的 seq 列表；超时返回空
         */
        std::vector<uint32_t> waitForNack(int timeoutMs) {
            std::vector<uint8_t> buf(MAX_PACKET_SIZE);
            size_t len = 0;
            Endpoint from;
            std::vector<uint32_t> seqs;
            if (!sock_.recvFrom(buf.data(), buf.size(), len, from, timeoutMs).isOk()) {
                return seqs;
            }
            PacketHeader header;
            if (!decodePacketHeader(buf.data(), len, header).isOk() ||
                header.type != PacketType::Nack) {
                return seqs;
            }
            (void)decodeNackPacket(buf.data(), len, seqs);
            return seqs;
        }

        /** @brief 把先前扣下的那一片补发出去 */
        void resendHeld(uint16_t port, bool markAsRetransmit) {
            ASSERT_TRUE(hasHeld_) << "没有扣下过任何分片, 补发无从谈起";
            EncodedFrameView view;
            view.data = heldData_.data();
            view.len = heldData_.size();
            view.frameId = heldFrameId_;
            view.captureMs = 4242;
            view.isKey = heldIsKey_;

            PacketBuffer packet;
            ASSERT_TRUE(packetizeOneFragment(0, heldSeq_, view, heldIndex_, heldCount_,
                                             markAsRetransmit, packet)
                            .isOk());
            ASSERT_TRUE(
                sock_.sendTo(Endpoint{"127.0.0.1", port}, packet.data(), packet.size())
                    .isOk());
        }

        uint32_t heldSeq() const { return heldSeq_; }

    private:
        Packetizer packer_;
        UdpSocket sock_;

        // 扣下那一片所需的全部信息, 整份拷贝 —— 后面的 send() 不会碰它
        std::vector<uint8_t> heldData_;
        uint32_t heldFrameId_ = 0;
        bool heldIsKey_ = false;
        uint32_t heldSeq_ = 0;
        uint16_t heldIndex_ = 0;
        uint16_t heldCount_ = 0;
        bool hasHeld_ = false;
    };

    ReceiverPipelineConfig rttConfig() {
        ReceiverPipelineConfig cfg = receiverConfig();
        cfg.idleTimeoutMs = 800;
        cfg.nack.windowPackets = 1024;
        cfg.nack.maxRequestsPerSeq = 5;
        cfg.fec.recentPackets = 0;   // 别让 FEC 把洞补上, 那样就不会有重传
        cfg.pliMinIntervalMs = 0;
        return cfg;
    }
}  // namespace

/**
 * NACK 发出到重传回来的时间，必须被量成 RTT。
 *
 * 这是整条链路上**唯一**的 RTT 数字。M4.5 第四轮发现"重传一直在成功、
 * 只是回来得太晚"时，手上没有任何 RTT 的量，只能拿 netem 的配置值反推 ——
 * 而那等于用输入去解释输出。
 */
TEST(ReceiverPipeline, MeasuresRttFromNackToRetransmit) {
    ReceiverPipeline pipeline(rttConfig());
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);
    TalkingStream stream;
    stream.send(port, payload, 0, /*isKey=*/true);
    stream.send(port, payload, 1, /*isKey=*/false, /*skipIndex=*/2);

    std::vector<uint32_t> asked;
    for (uint32_t f = 2; f < 12 && asked.empty(); ++f) {
        stream.send(port, payload, f, /*isKey=*/false);
        asked = stream.waitForNack(200);
    }
    ASSERT_FALSE(asked.empty()) << "接收端没有请求重传, 后面的断言不成立";

    std::this_thread::sleep_for(60ms);
    stream.resendHeld(port, /*markAsRetransmit=*/true);
    std::this_thread::sleep_for(50ms);

    done.wait();
    const ReceiverPipelineStats& s = pipeline.stats();

    EXPECT_GE(s.rttSamples, 1u) << "一次 RTT 都没量到";
    EXPECT_GE(s.rttMs, 40) << "量出来的 RTT 明显小于实际等待的 60ms";
    EXPECT_LE(s.rttMs, 400);
}

/**
 * 不带 FLAG_RETRANSMIT 的包不算重传，一次 RTT 都不许量。
 *
 * 请求刚发出去、原包正好到达是常事。把它当成重传会量出接近 0 的 RTT，
 * 而这个数要去当水位下限 —— **低估的下限比没有下限更糟**，
 * 因为它看起来像是量过的。
 */
TEST(ReceiverPipeline, APlainPacketIsNotMistakenForARetransmit) {
    ReceiverPipeline pipeline(rttConfig());
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);
    TalkingStream stream;
    stream.send(port, payload, 0, /*isKey=*/true);
    stream.send(port, payload, 1, /*isKey=*/false, /*skipIndex=*/2);

    std::vector<uint32_t> asked;
    for (uint32_t f = 2; f < 12 && asked.empty(); ++f) {
        stream.send(port, payload, f, /*isKey=*/false);
        asked = stream.waitForNack(200);
    }
    ASSERT_FALSE(asked.empty());

    std::this_thread::sleep_for(60ms);
    stream.resendHeld(port, /*markAsRetransmit=*/false);  // 没打重传位
    std::this_thread::sleep_for(50ms);

    done.wait();
    EXPECT_EQ(pipeline.stats().rttSamples, 0u)
        << "把一个普通包当成了重传 —— RTT 会被低估到接近 0";
}

/**
 * 待回包的请求表不许无限涨。
 *
 * 请求出去的包可能永远回不来（那正是 givenUp 的含义）。不设上限就是安静地涨内存,
 * 而"安静"意味着没有任何测试能发现它 —— 所以专门加了 rttPending 这个可观测量。
 */
TEST(ReceiverPipeline, ThePendingRttTableStaysBounded) {
    ReceiverPipelineConfig cfg = rttConfig();
    cfg.nack.maxRequestsPerSeq = 1;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    // 每一帧都扣下一片, 而且一个都不补 —— 请求只出不进。
    // 帧数必须**大于 kRttPendingMax(256)**, 否则表根本涨不到上限,
    // 这条用例就成了一句空话: 第一版只发 120 帧, 把上限去掉照样全绿。
    const std::vector<uint8_t> payload = fakeStream(8000);
    TalkingStream stream;
    stream.send(port, payload, 0, /*isKey=*/true);
    for (uint32_t f = 1; f < 340; ++f) {
        stream.send(port, payload, f, /*isKey=*/false, /*skipIndex=*/f % 5);
        std::this_thread::sleep_for(1ms);
    }

    done.wait();
    const ReceiverPipelineStats& s = pipeline.stats();
    EXPECT_GT(s.nackPacketsSent, 0u) << "构造没生效: 一个 NACK 都没发";
    EXPECT_LE(s.rttPending, 256u)
        << "rttPending=" << s.rttPending << " —— 待回包的请求表在无限增长";
}

/**
 * 同一个 seq 被请求第二次时，RTT 的起点仍然是**第一次**请求的时刻。
 *
 * 覆盖成第二次的时刻，量出来的是"最后一次请求到回包"的时间，比真实往返短。
 * 而这个数要去当水位下限 —— 低估的下限比没有下限更糟，因为它看起来像是量过的。
 *
 * 构造：把帧发得很慢（100ms 一帧），于是两次请求之间隔了约 100ms；
 * 等到第一次请求之后约 300ms 才补发。用第一次时刻算是 ~300，用第二次算是 ~200。
 */
TEST(ReceiverPipeline, RttCountsFromTheFirstRequestNotTheRetry) {
    ReceiverPipelineConfig cfg = rttConfig();
    cfg.nack.maxRequestsPerSeq = 5;
    cfg.idleTimeoutMs = 2000;
    ReceiverPipeline pipeline(cfg);
    ASSERT_TRUE(pipeline.open().isOk());
    const uint16_t port = pipeline.boundPort();

    std::atomic<bool> stop{false};
    auto done = std::async(std::launch::async, [&] { return pipeline.run(stop); });

    const std::vector<uint8_t> payload = fakeStream(8000);
    TalkingStream stream;
    stream.send(port, payload, 0, /*isKey=*/true);
    stream.send(port, payload, 1, /*isKey=*/false, /*skipIndex=*/2);

    // 等第一次请求
    std::vector<uint32_t> asked;
    for (uint32_t f = 2; f < 8 && asked.empty(); ++f) {
        stream.send(port, payload, f, /*isKey=*/false);
        asked = stream.waitForNack(300);
    }
    ASSERT_FALSE(asked.empty());
    const uint64_t firstAskMs = steadyNowMs();

    // 慢慢再发几帧, 逼出第二次、第三次请求
    for (uint32_t f = 8; f < 11; ++f) {
        std::this_thread::sleep_for(100ms);
        stream.send(port, payload, f, /*isKey=*/false);
        (void)stream.waitForNack(50);
    }

    const uint64_t waited = steadyNowMs() - firstAskMs;
    ASSERT_GE(waited, 250u) << "构造没生效: 等的时间不够长, 两种算法分不开";
    stream.resendHeld(port, /*markAsRetransmit=*/true);
    std::this_thread::sleep_for(80ms);

    done.wait();
    const ReceiverPipelineStats& s = pipeline.stats();
    ASSERT_GE(s.rttSamples, 1u);
    EXPECT_GE(s.rttMs, static_cast<int>(waited) - 60)
        << "RTT=" << s.rttMs << "ms 而距第一次请求已经过了 " << waited
        << "ms —— 起点被重发覆盖成了最后一次请求";
}
