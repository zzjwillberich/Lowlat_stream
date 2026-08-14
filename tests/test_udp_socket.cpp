/**
 * @file    test_udp_socket.cpp
 * @brief   UdpSocket 的契约测试, 全部跑在回环地址上
 * @author  zzj
 * @date    2026-08-14
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/Clock.h"
#include "modules/transport/Packet.h"
#include "modules/transport/UdpSocket.h"

namespace {
    constexpr const char* LOOPBACK = "127.0.0.1";

    /** 端口一律由内核分配(bind 传 0), 写死端口在 CI 上会和别的进程/并行用例撞车 */
    Endpoint loopbackAny() {
        Endpoint ep;
        ep.ip = LOOPBACK;
        ep.port = 0;
        return ep;
    }

    /** 开一个已绑定的接收端, 顺带断言每一步都成功 */
    void openBound(UdpSocket& s) {
        ASSERT_TRUE(s.open().isOk());
        ASSERT_TRUE(s.bind(loopbackAny()).isOk());
        ASSERT_NE(s.localEndpoint().port, 0) << "bind(0) 之后必须能回读到内核分配的实际端口";
    }
}  // namespace

// ---------- 生命周期 ----------

TEST(UdpSocket, DefaultConstructedIsNotOpen) {
    const UdpSocket s;
    EXPECT_FALSE(s.isOpen());
    EXPECT_LT(s.fd(), 0);
}

TEST(UdpSocket, OperationsBeforeOpenReportClosed) {
    UdpSocket s;
    const uint8_t byte = 0;
    uint8_t buf[16] = {};
    size_t len = 0;
    Endpoint from;

    // Closed 而不是 InvalidArg: 参数没错, 是生命周期用错了, 两者要改的代码不在一处
    EXPECT_EQ(s.bind(loopbackAny()).code(), Code::Closed);
    EXPECT_EQ(s.sendTo(loopbackAny(), &byte, 1).code(), Code::Closed);
    EXPECT_EQ(s.recvFrom(buf, sizeof(buf), len, from, 0).code(), Code::Closed);
}

TEST(UdpSocket, OpenTwiceIsRejectedInsteadOfLeakingTheFirstFd) {
    UdpSocket s;
    ASSERT_TRUE(s.open().isOk());
    EXPECT_EQ(s.open().code(), Code::InvalidArg) << "覆盖 fd_ 会让第一个 fd 永远泄漏";
}

TEST(UdpSocket, CloseIsIdempotentAndSocketCanBeReopened) {
    UdpSocket s;
    ASSERT_TRUE(s.open().isOk());

    s.close();
    EXPECT_FALSE(s.isOpen());
    s.close();  // 析构还会再来一次, 第二次必须是空操作
    EXPECT_FALSE(s.isOpen());

    EXPECT_TRUE(s.open().isOk());
    EXPECT_TRUE(s.isOpen());
}

TEST(UdpSocket, MoveTransfersOwnershipAndLeavesSourceClosed) {
    UdpSocket src;
    ASSERT_NO_FATAL_FAILURE(openBound(src));
    const int fd = src.fd();
    const uint16_t port = src.localEndpoint().port;

    UdpSocket dst(std::move(src));

    EXPECT_FALSE(src.isOpen()) << "源对象还持有 fd 的话, 两个析构会 close 同一个 fd";
    EXPECT_LT(src.fd(), 0);
    EXPECT_EQ(dst.fd(), fd);
    EXPECT_EQ(dst.localEndpoint().port, port) << "移动之后套接字仍然绑在原来的端口上";
}

TEST(UdpSocket, MoveAssignmentSurvivesSelfAssignment) {
    UdpSocket s;
    ASSERT_NO_FATAL_FAILURE(openBound(s));
    const int fd = s.fd();

    UdpSocket& alias = s;
    s = std::move(alias);  // 先 close 再接管的写法会在这里关掉自己的 fd

    EXPECT_TRUE(s.isOpen());
    EXPECT_EQ(s.fd(), fd);
}

// ---------- 参数校验 ----------

TEST(UdpSocket, RejectsNullBufferAndEmptyPayload) {
    UdpSocket s;
    ASSERT_TRUE(s.open().isOk());

    const uint8_t byte = 0;
    uint8_t buf[16] = {};
    size_t len = 0;
    Endpoint from;
    Endpoint remote{LOOPBACK, 9999};

    EXPECT_EQ(s.sendTo(remote, nullptr, 1).code(), Code::InvalidArg);
    EXPECT_EQ(s.sendTo(remote, &byte, 0).code(), Code::InvalidArg);
    EXPECT_EQ(s.recvFrom(nullptr, sizeof(buf), len, from, 0).code(), Code::InvalidArg);
    EXPECT_EQ(s.recvFrom(buf, 0, len, from, 0).code(), Code::InvalidArg);
}

TEST(UdpSocket, RejectsUnparsableAddress) {
    UdpSocket s;
    ASSERT_TRUE(s.open().isOk());

    const uint8_t byte = 0;
    // "1.2.3" 这类残缺写法必须被拒绝 —— inet_addr 会接受它, inet_pton 不会
    for (const char* bad : {"", "not-an-ip", "1.2.3", "256.0.0.1"}) {
        Endpoint remote{bad, 9999};
        EXPECT_EQ(s.sendTo(remote, &byte, 1).code(), Code::InvalidArg) << "ip=" << bad;
    }
}

// ---------- 收发 ----------

TEST(UdpSocket, LoopbackRoundTripDeliversExactBytesAndSenderAddress) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));

    UdpSocket sender;
    ASSERT_TRUE(sender.open().isOk());

    const std::vector<uint8_t> payload{0x00, 0x01, 0xFE, 0xFF, 0x42};
    Endpoint to{LOOPBACK, receiver.localEndpoint().port};
    ASSERT_TRUE(sender.sendTo(to, payload.data(), payload.size()).isOk());

    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE, 0);
    size_t len = 0;
    Endpoint from;
    ASSERT_TRUE(receiver.recvFrom(buf.data(), buf.size(), len, from, 1000).isOk());

    ASSERT_EQ(len, payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), buf.begin()));

    // 发送端没 bind, 源端口由内核在首次 sendto 时分配; NACK/PLI 要靠这个地址回包
    EXPECT_EQ(from.ip, LOOPBACK);
    EXPECT_NE(from.port, 0);
    EXPECT_EQ(from.port, sender.localEndpoint().port);
}

TEST(UdpSocket, CarriesAFullSizedProtocolPacket) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));
    UdpSocket sender;
    ASSERT_TRUE(sender.open().isOk());

    // 协议规定的最大包长必须能整包过去, 否则 MAX_PAYLOAD 的取值就是错的
    const std::vector<uint8_t> payload(MAX_DATA_PACKET_SIZE, 0xA5);
    Endpoint to{LOOPBACK, receiver.localEndpoint().port};
    ASSERT_TRUE(sender.sendTo(to, payload.data(), payload.size()).isOk());

    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE, 0);
    size_t len = 0;
    Endpoint from;
    ASSERT_TRUE(receiver.recvFrom(buf.data(), buf.size(), len, from, 1000).isOk());
    EXPECT_EQ(len, MAX_DATA_PACKET_SIZE);
}

TEST(UdpSocket, PreservesDatagramBoundaries) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));
    UdpSocket sender;
    ASSERT_TRUE(sender.open().isOk());
    Endpoint to{LOOPBACK, receiver.localEndpoint().port};

    const std::vector<uint8_t> first{1, 1, 1};
    const std::vector<uint8_t> second{2, 2, 2, 2, 2};
    ASSERT_TRUE(sender.sendTo(to, first.data(), first.size()).isOk());
    ASSERT_TRUE(sender.sendTo(to, second.data(), second.size()).isOk());

    // UDP 不是字节流: 两次 send 必须收成两个包, 绝不会粘成 8 字节
    std::vector<uint8_t> buf(64, 0);
    size_t len = 0;
    Endpoint from;
    ASSERT_TRUE(receiver.recvFrom(buf.data(), buf.size(), len, from, 1000).isOk());
    EXPECT_EQ(len, first.size());
    ASSERT_TRUE(receiver.recvFrom(buf.data(), buf.size(), len, from, 1000).isOk());
    EXPECT_EQ(len, second.size());
}

TEST(UdpSocket, RejectsDatagramLargerThanTheBufferInsteadOfTruncating) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));
    UdpSocket sender;
    ASSERT_TRUE(sender.open().isOk());

    const std::vector<uint8_t> payload(200, 0x5A);
    Endpoint to{LOOPBACK, receiver.localEndpoint().port};
    ASSERT_TRUE(sender.sendTo(to, payload.data(), payload.size()).isOk());

    uint8_t small[64] = {};
    size_t len = 0;
    Endpoint from;
    // 悄悄截断的话, 包头校验能过、载荷少一截, 最终表现为解码花屏, 得从解码器一路倒查
    EXPECT_EQ(receiver.recvFrom(small, sizeof(small), len, from, 1000).code(), Code::NetError);
}

// ---------- 超时 ----------

TEST(UdpSocket, RecvFromReportsTimeoutRatherThanAnError) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));

    uint8_t buf[64] = {};
    size_t len = 0;
    Endpoint from;

    const uint64_t start = steadyNowMs();
    const Status st = receiver.recvFrom(buf, sizeof(buf), len, from, 100);
    const uint64_t elapsed = steadyNowMs() - start;

    // Timeout 是正常情况: 收包线程靠它定期回到循环顶部检查停止标志
    EXPECT_EQ(st.code(), Code::Timeout);
    EXPECT_GE(elapsed, 80u) << "没有真的等 —— 收包循环会退化成忙轮询, 空转烧满一个核";
    EXPECT_LT(elapsed, 2000u) << "等过头了";
}

TEST(UdpSocket, ZeroTimeoutReturnsImmediately) {
    UdpSocket receiver;
    ASSERT_NO_FATAL_FAILURE(openBound(receiver));

    uint8_t buf[64] = {};
    size_t len = 0;
    Endpoint from;

    const uint64_t start = steadyNowMs();
    EXPECT_EQ(receiver.recvFrom(buf, sizeof(buf), len, from, 0).code(), Code::Timeout);
    EXPECT_LT(steadyNowMs() - start, 50u) << "timeoutMs = 0 是探一下就走, 不许阻塞";
}

// ---------- 端点 ----------

TEST(UdpSocket, EndpointToStringIsLogFriendly) {
    Endpoint ep{LOOPBACK, 9000};
    EXPECT_EQ(ep.toString(), "127.0.0.1:9000");

    Endpoint any{"", 9000};
    EXPECT_EQ(any.toString(), "*:9000") << "空 ip 表示所有网卡, 日志里要看得出来";
}
