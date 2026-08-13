/**
 * @file    test_packet.cpp
 * @brief   协议包头编解码的契约测试, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-13
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "modules/transport/Packet.h"

namespace {
    PacketHeader sampleHeader() {
        PacketHeader h;
        h.version     = PROTOCOL_VERSION;
        h.type        = PacketType::Data;
        h.streamId    = 0x1234;
        h.seq         = 0xDEADBEEF;
        h.timestampMs = 0x01020304;
        return h;
    }

    DataHeader sampleDataHeader() {
        DataHeader d;
        d.frameId   = 0xAABBCCDD;
        d.fragIndex = 2;
        d.fragCount = 5;
        d.flags     = DataHeader::FLAG_KEYFRAME;
        return d;
    }
}  // namespace

// ---------- 线上布局 ----------

TEST(PacketHeader, OnWireSizesMatchProtocolDoc) {
    // 这两个数字是协议的一部分, 改了就是改协议, 必须先改 docs/PROTOCOL.md
    EXPECT_EQ(PACKET_HEADER_SIZE, 12u);
    EXPECT_EQ(DATA_HEADER_SIZE, 9u);
}

TEST(PacketHeader, MaxDataPacketStaysUnderTypicalMtu) {
    // 1500(以太网 MTU) - 40(IPv6) - 8(UDP) = 1452, 留出的余量用于容忍隧道封装。
    // 超过就会触发 IP 分片, 那一层的丢失我们的 FEC/NACK 完全看不见。
    EXPECT_LE(MAX_DATA_PACKET_SIZE, 1452u);
}

TEST(PacketHeader, EncodesFieldsInNetworkByteOrderAtFixedOffsets) {
    PacketHeader h = sampleHeader();
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);

    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());

    EXPECT_EQ(buf[0], PROTOCOL_VERSION);
    EXPECT_EQ(buf[1], static_cast<uint8_t>(PacketType::Data));
    // 大端: 高位字节在前
    EXPECT_EQ(buf[2], 0x12);
    EXPECT_EQ(buf[3], 0x34);
    EXPECT_EQ(buf[4], 0xDE);
    EXPECT_EQ(buf[5], 0xAD);
    EXPECT_EQ(buf[6], 0xBE);
    EXPECT_EQ(buf[7], 0xEF);
    EXPECT_EQ(buf[8], 0x01);
    EXPECT_EQ(buf[9], 0x02);
    EXPECT_EQ(buf[10], 0x03);
    EXPECT_EQ(buf[11], 0x04);
}

TEST(DataHeader, EncodesFieldsInNetworkByteOrderAtFixedOffsets) {
    DataHeader d = sampleDataHeader();
    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);

    ASSERT_TRUE(encodeDataHeader(d, buf.data(), buf.size()).isOk());

    EXPECT_EQ(buf[0], 0xAA);
    EXPECT_EQ(buf[1], 0xBB);
    EXPECT_EQ(buf[2], 0xCC);
    EXPECT_EQ(buf[3], 0xDD);
    EXPECT_EQ(buf[4], 0x00);
    EXPECT_EQ(buf[5], 0x02);
    EXPECT_EQ(buf[6], 0x00);
    EXPECT_EQ(buf[7], 0x05);
    EXPECT_EQ(buf[8], DataHeader::FLAG_KEYFRAME);
}

// ---------- 往返一致 ----------

TEST(PacketHeader, RoundTripPreservesEveryField) {
    const PacketHeader in = sampleHeader();
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);
    ASSERT_TRUE(encodePacketHeader(in, buf.data(), buf.size()).isOk());

    PacketHeader out;
    ASSERT_TRUE(decodePacketHeader(buf.data(), buf.size(), out).isOk());

    EXPECT_EQ(out.version, in.version);
    EXPECT_EQ(out.type, in.type);
    EXPECT_EQ(out.streamId, in.streamId);
    EXPECT_EQ(out.seq, in.seq);
    EXPECT_EQ(out.timestampMs, in.timestampMs);
}

TEST(PacketHeader, RoundTripHandlesBoundaryValues) {
    for (uint32_t v : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
        PacketHeader in = sampleHeader();
        in.seq         = v;
        in.timestampMs = v;
        in.streamId    = static_cast<uint16_t>(v);

        std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);
        ASSERT_TRUE(encodePacketHeader(in, buf.data(), buf.size()).isOk()) << "seq=" << v;

        PacketHeader out;
        ASSERT_TRUE(decodePacketHeader(buf.data(), buf.size(), out).isOk()) << "seq=" << v;
        EXPECT_EQ(out.seq, in.seq);
        EXPECT_EQ(out.timestampMs, in.timestampMs);
        EXPECT_EQ(out.streamId, in.streamId);
    }
}

TEST(PacketHeader, RoundTripCoversEveryPacketType) {
    for (uint8_t t = 1; t <= 6; ++t) {
        PacketHeader in = sampleHeader();
        in.type = static_cast<PacketType>(t);

        std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);
        ASSERT_TRUE(encodePacketHeader(in, buf.data(), buf.size()).isOk()) << "type=" << int(t);

        PacketHeader out;
        ASSERT_TRUE(decodePacketHeader(buf.data(), buf.size(), out).isOk()) << "type=" << int(t);
        EXPECT_EQ(static_cast<uint8_t>(out.type), t);
    }
}

TEST(DataHeader, RoundTripPreservesEveryField) {
    const DataHeader in = sampleDataHeader();
    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);
    ASSERT_TRUE(encodeDataHeader(in, buf.data(), buf.size()).isOk());

    DataHeader out;
    ASSERT_TRUE(decodeDataHeader(buf.data(), buf.size(), out).isOk());

    EXPECT_EQ(out.frameId, in.frameId);
    EXPECT_EQ(out.fragIndex, in.fragIndex);
    EXPECT_EQ(out.fragCount, in.fragCount);
    EXPECT_EQ(out.flags, in.flags);
}

TEST(DataHeader, SingleFragmentFrameIsValid) {
    DataHeader in = sampleDataHeader();
    in.fragIndex = 0;
    in.fragCount = 1;

    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);
    ASSERT_TRUE(encodeDataHeader(in, buf.data(), buf.size()).isOk());

    DataHeader out;
    ASSERT_TRUE(decodeDataHeader(buf.data(), buf.size(), out).isOk());
    EXPECT_EQ(out.fragCount, 1u);
}

// ---------- 缓冲区边界 ----------

TEST(PacketHeader, RejectsNullOrShortBuffer) {
    const PacketHeader h = sampleHeader();
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);

    EXPECT_EQ(encodePacketHeader(h, nullptr, PACKET_HEADER_SIZE).code(), Code::InvalidArg);
    EXPECT_EQ(encodePacketHeader(h, buf.data(), PACKET_HEADER_SIZE - 1).code(), Code::InvalidArg);

    PacketHeader out;
    EXPECT_EQ(decodePacketHeader(nullptr, PACKET_HEADER_SIZE, out).code(), Code::InvalidArg);
    EXPECT_EQ(decodePacketHeader(buf.data(), PACKET_HEADER_SIZE - 1, out).code(), Code::InvalidArg);
}

TEST(PacketHeader, EncodeWritesExactlyHeaderSizeBytes) {
    const PacketHeader h = sampleHeader();
    // 多给一个字节的余量, 确认实现不会越界写到第 13 字节
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE + 1, 0xA5);

    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());
    EXPECT_EQ(buf[PACKET_HEADER_SIZE], 0xA5) << "encode 越界写入了包头之后的字节";
}

TEST(DataHeader, RejectsNullOrShortBuffer) {
    const DataHeader d = sampleDataHeader();
    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);

    EXPECT_EQ(encodeDataHeader(d, nullptr, DATA_HEADER_SIZE).code(), Code::InvalidArg);
    EXPECT_EQ(encodeDataHeader(d, buf.data(), DATA_HEADER_SIZE - 1).code(), Code::InvalidArg);

    DataHeader out;
    EXPECT_EQ(decodeDataHeader(nullptr, DATA_HEADER_SIZE, out).code(), Code::InvalidArg);
    EXPECT_EQ(decodeDataHeader(buf.data(), DATA_HEADER_SIZE - 1, out).code(), Code::InvalidArg);
}

// ---------- 畸形包 ----------

TEST(PacketHeader, DecodeRejectsUnknownVersionAsNetError) {
    PacketHeader h = sampleHeader();
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);
    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());

    buf[0] = PROTOCOL_VERSION + 1;

    PacketHeader out;
    // NetError 而不是 InvalidArg: 这是对端/网络的问题, 调用方应丢包计数而不是改参数重试
    EXPECT_EQ(decodePacketHeader(buf.data(), buf.size(), out).code(), Code::NetError);
}

TEST(PacketHeader, DecodeRejectsUnknownTypeAsNetError) {
    PacketHeader h = sampleHeader();
    std::vector<uint8_t> buf(PACKET_HEADER_SIZE, 0);
    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());

    PacketHeader out;
    for (uint8_t bad : {0, 7, 200, 255}) {
        buf[1] = bad;
        EXPECT_EQ(decodePacketHeader(buf.data(), buf.size(), out).code(), Code::NetError)
            << "type=" << int(bad) << " 不在协议定义内, 必须拒绝";
    }
}

TEST(DataHeader, EncodeRejectsInconsistentFragmentation) {
    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);

    DataHeader zeroCount = sampleDataHeader();
    zeroCount.fragCount = 0;
    EXPECT_EQ(encodeDataHeader(zeroCount, buf.data(), buf.size()).code(), Code::InvalidArg);

    DataHeader outOfRange = sampleDataHeader();
    outOfRange.fragIndex = 5;
    outOfRange.fragCount = 5;
    EXPECT_EQ(encodeDataHeader(outOfRange, buf.data(), buf.size()).code(), Code::InvalidArg);
}

TEST(DataHeader, DecodeRejectsInconsistentFragmentationAsNetError) {
    DataHeader d = sampleDataHeader();
    std::vector<uint8_t> buf(DATA_HEADER_SIZE, 0);
    ASSERT_TRUE(encodeDataHeader(d, buf.data(), buf.size()).isOk());

    DataHeader out;

    // fragCount = 0
    buf[6] = 0x00;
    buf[7] = 0x00;
    EXPECT_EQ(decodeDataHeader(buf.data(), buf.size(), out).code(), Code::NetError);

    // fragIndex >= fragCount —— 组包器会拿它索引数组, 必须在入口挡住
    buf[4] = 0x00;
    buf[5] = 0x09;
    buf[6] = 0x00;
    buf[7] = 0x05;
    EXPECT_EQ(decodeDataHeader(buf.data(), buf.size(), out).code(), Code::NetError);
}

// ---------- seq 回绕 ----------

TEST(SeqCompare, HandlesNormalOrdering) {
    EXPECT_TRUE(seqNewerThan(2, 1));
    EXPECT_FALSE(seqNewerThan(1, 2));
    EXPECT_FALSE(seqNewerThan(1, 1)) << "相等不算更新";
}

TEST(SeqCompare, HandlesWraparound) {
    // 直接写 a > b 时这两条都会判反 —— 跑几十分钟后回绕, 表现为"突然全是乱序"
    EXPECT_TRUE(seqNewerThan(0, 0xFFFFFFFF));
    EXPECT_TRUE(seqNewerThan(5, 0xFFFFFFFA));
    EXPECT_FALSE(seqNewerThan(0xFFFFFFFF, 0));
    EXPECT_FALSE(seqNewerThan(0xFFFFFFFA, 5));
}
