/**
 * @file    test_nack_packet.cpp
 * @brief   M4.1 NACK 包(位图编码)的编解码契约
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 位图形状取自 RFC 4585 的 Generic NACK: 每条 6 字节 = uint32 pid + uint16 blp,
 *       覆盖 17 个连续 seq 位。这里测的是**线上布局和往返一致**, 不是分条的最优性 ——
 *       贪心分条就够, 最优解省不下几个字节。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "modules/transport/Packet.h"

namespace {
    PacketHeader nackHeader(uint32_t seq = 7) {
        PacketHeader h;
        h.type = PacketType::Nack;
        h.streamId = 0;
        h.seq = seq;
        h.timestampMs = 123456;
        return h;
    }

    /** 编码后再解回来, 断言往返一致; 返回线上长度 */
    size_t roundTrip(const std::vector<uint32_t>& missing, std::vector<uint32_t>& out) {
        std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
        size_t len = 0;
        EXPECT_TRUE(encodeNackPacket(nackHeader(), missing, buf.data(), buf.size(), len).isOk());
        EXPECT_GT(len, 0u);
        EXPECT_TRUE(decodeNackPacket(buf.data(), len, out).isOk());
        return len;
    }

    uint16_t readU16(const uint8_t* p) {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }
    uint32_t readU32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
}  // namespace

// ---------- 线上布局 ----------

/**
 * 逐字节核对偏移和大端序。这条不过, 两端就永远互通不了 ——
 * 而 `#pragma pack` 之类的东西一个都挡不住这个错(见 NOTES M2 第 18 条)。
 */
TEST(NackPacket, WireLayoutOfASingleEntry) {
    const std::vector<uint32_t> missing = {0x11223344u};
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE, 0xAB);
    size_t len = 0;
    ASSERT_TRUE(encodeNackPacket(nackHeader(), missing, buf.data(), buf.size(), len).isOk());

    EXPECT_EQ(len, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + NACK_ENTRY_SIZE);
    EXPECT_EQ(buf[0], PROTOCOL_VERSION);
    EXPECT_EQ(buf[1], static_cast<uint8_t>(PacketType::Nack));
    EXPECT_EQ(readU16(&buf[PACKET_HEADER_SIZE]), 1u) << "entryCount 在偏移 12, 2 字节大端";
    EXPECT_EQ(readU32(&buf[PACKET_HEADER_SIZE + 2]), 0x11223344u) << "pid 在偏移 14";
    EXPECT_EQ(readU16(&buf[PACKET_HEADER_SIZE + 6]), 0u) << "只有 pid 一个缺口, blp 全 0";
    EXPECT_EQ(buf[len], 0xAB) << "写超了, 越过了它自己报告的长度";
}

/** blp 的第 i 位对应 pid + 1 + i, 这个映射必须钉死, 反了两端就对不上 */
TEST(NackPacket, BlpBitIMapsToPidPlusOnePlusI) {
    const std::vector<uint32_t> missing = {1000, 1001, 1003, 1016};
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
    size_t len = 0;
    ASSERT_TRUE(encodeNackPacket(nackHeader(), missing, buf.data(), buf.size(), len).isOk());

    ASSERT_EQ(readU16(&buf[PACKET_HEADER_SIZE]), 1u) << "1000..1016 相距 16, 一条就够";
    EXPECT_EQ(readU32(&buf[PACKET_HEADER_SIZE + 2]), 1000u);
    // 1001 -> bit0, 1003 -> bit2, 1016 -> bit15
    const uint16_t expected = (1u << 0) | (1u << 2) | (1u << 15);
    EXPECT_EQ(readU16(&buf[PACKET_HEADER_SIZE + 6]), expected);
}

// ---------- 往返 ----------

TEST(NackPacket, RoundTripsASingleSeq) {
    std::vector<uint32_t> out;
    roundTrip({42}, out);
    EXPECT_EQ(out, std::vector<uint32_t>({42}));
}

TEST(NackPacket, RoundTripsAClusterWithinOneEntry) {
    const std::vector<uint32_t> missing = {500, 501, 502, 510, 516};
    std::vector<uint32_t> out;
    const size_t len = roundTrip(missing, out);
    EXPECT_EQ(out, missing);
    EXPECT_EQ(len, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + NACK_ENTRY_SIZE)
        << "跨度 16 以内应当只用一条";
}

/** 跨度超过 16 就必须开新条 —— 边界是 pid+16 在内、pid+17 在外 */
TEST(NackPacket, ACrossingOfSeventeenStartsANewEntry) {
    std::vector<uint32_t> out;
    const size_t oneEntry = roundTrip({100, 116}, out);
    EXPECT_EQ(out, std::vector<uint32_t>({100, 116}));
    EXPECT_EQ(oneEntry, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + NACK_ENTRY_SIZE);

    out.clear();
    const size_t twoEntries = roundTrip({100, 117}, out);
    EXPECT_EQ(out, std::vector<uint32_t>({100, 117}));
    EXPECT_EQ(twoEntries, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + 2 * NACK_ENTRY_SIZE);
}

TEST(NackPacket, RoundTripsScatteredSeqs) {
    std::vector<uint32_t> missing;
    for (uint32_t s = 0; s < 60; ++s) missing.push_back(1000 + s * 40);  // 全部孤立
    std::vector<uint32_t> out;
    const size_t len = roundTrip(missing, out);
    EXPECT_EQ(out, missing);
    EXPECT_EQ(len, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + 60 * NACK_ENTRY_SIZE)
        << "散开的缺口每个都要单独一条 —— 这正是位图相对列表更费的场景";
}

/**
 * seq 会回绕, 分条时算"距离"必须用无符号减法。
 * 直接比大小会在回绕点把一簇连续缺口拆成两条, 甚至漏掉。
 */
TEST(NackPacket, RoundTripsAcrossSeqWrapAround) {
    const uint32_t base = 0xFFFFFFFFu - 3;
    const std::vector<uint32_t> missing = {base, base + 1, base + 2, base + 3,
                                           base + 4, base + 5};  // 跨过 0
    std::vector<uint32_t> out;
    const size_t len = roundTrip(missing, out);
    EXPECT_EQ(out, missing);
    EXPECT_EQ(len, PACKET_HEADER_SIZE + NACK_HEADER_SIZE + NACK_ENTRY_SIZE)
        << "回绕点上把一簇连续缺口拆成了多条";
}

// ---------- 容量 ----------

/**
 * 默认跟踪窗口(1024)下, 一个 NACK 包必须装得下**整个窗口**, 不管缺口怎么分布。
 * 1024 个 seq 位最多需要 ceil(1024/17) = 61 条 = 366 字节, 远低于 1221。
 * 这条成立, 截断路径就走不到。
 */
TEST(NackPacket, AFullTrackingWindowAlwaysFitsInOnePacket) {
    std::vector<uint32_t> missing;
    for (uint32_t s = 0; s < 1024; ++s) missing.push_back(10000 + s);  // 整窗口全丢

    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
    size_t len = 0;
    ASSERT_TRUE(encodeNackPacket(nackHeader(), missing, buf.data(), buf.size(), len).isOk());
    EXPECT_LE(len, MAX_DATA_PACKET_SIZE);
    EXPECT_LE(readU16(&buf[PACKET_HEADER_SIZE]), 61u);

    std::vector<uint32_t> out;
    ASSERT_TRUE(decodeNackPacket(buf.data(), len, out).isOk());
    EXPECT_EQ(out, missing);
}

/**
 * 装不下要报 Internal 而不是 NetError: 缓冲是本端按线上长度算好的, 装不下只可能是
 * 本端算错了或者调用方没先截断 —— 那是 bug, 不是坏输入。两者的反应完全不同
 * (改代码 vs 丢包继续), 混成一个错误码就分不清。
 */
TEST(NackPacket, TooManyEntriesIsAnInternalErrorNotAProtocolError) {
    std::vector<uint32_t> missing;
    // 全部孤立 -> 每个一条; 超过 MAX_NACK_ENTRIES
    for (size_t i = 0; i <= MAX_NACK_ENTRIES; ++i) {
        missing.push_back(static_cast<uint32_t>(1000 + i * 40));
    }
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
    size_t len = 0;
    EXPECT_EQ(encodeNackPacket(nackHeader(), missing, buf.data(), buf.size(), len).code(),
              Code::Internal);
}

// ---------- 参数与畸形 ----------

TEST(NackPacket, EncodeRejectsBadArguments) {
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
    size_t len = 0;
    EXPECT_EQ(encodeNackPacket(nackHeader(), {}, buf.data(), buf.size(), len).code(),
              Code::InvalidArg)
        << "空 NACK 没有意义";
    EXPECT_EQ(encodeNackPacket(nackHeader(), {1}, nullptr, buf.size(), len).code(),
              Code::InvalidArg);

    PacketHeader wrongType = nackHeader();
    wrongType.type = PacketType::Data;
    EXPECT_EQ(encodeNackPacket(wrongType, {1}, buf.data(), buf.size(), len).code(),
              Code::InvalidArg)
        << "本端把 type 写错时, 现象会是'对端收到 NACK 不理睬', 排查方向指向对端";
}

/**
 * 长度必须和 entryCount **精确**相符 —— 多一字节少一字节都算畸形。
 * 拿一个不可信的 entryCount 去循环读数组, 就是一次越界读。
 */
TEST(NackPacket, DecodeRejectsALengthThatContradictsEntryCount) {
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE);
    size_t len = 0;
    ASSERT_TRUE(encodeNackPacket(nackHeader(), {100, 200, 300}, buf.data(), buf.size(), len)
                    .isOk());

    std::vector<uint32_t> out;
    EXPECT_EQ(decodeNackPacket(buf.data(), len - 1, out).code(), Code::NetError) << "短一字节";
    EXPECT_EQ(decodeNackPacket(buf.data(), len + 1, out).code(), Code::NetError) << "长一字节";
    EXPECT_EQ(decodeNackPacket(buf.data(), PACKET_HEADER_SIZE, out).code(), Code::NetError)
        << "只有通用头, 连 entryCount 都读不出来";
}

TEST(NackPacket, DecodeRejectsAZeroOrOversizedEntryCount) {
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE, 0);
    PacketHeader h = nackHeader();
    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());

    std::vector<uint32_t> out;
    // entryCount = 0
    buf[PACKET_HEADER_SIZE] = 0;
    buf[PACKET_HEADER_SIZE + 1] = 0;
    EXPECT_EQ(decodeNackPacket(buf.data(), PACKET_HEADER_SIZE + NACK_HEADER_SIZE, out).code(),
              Code::NetError);

    // entryCount 大得离谱: 长度对不上, 必须在读数组之前就被挡住
    buf[PACKET_HEADER_SIZE] = 0xFF;
    buf[PACKET_HEADER_SIZE + 1] = 0xFF;
    EXPECT_EQ(decodeNackPacket(buf.data(), PACKET_HEADER_SIZE + NACK_HEADER_SIZE, out).code(),
              Code::NetError);
}

TEST(NackPacket, DecodeRejectsAWrongPacketType) {
    std::vector<uint8_t> buf(MAX_DATA_PACKET_SIZE, 0);
    PacketHeader h;
    h.type = PacketType::Data;  // 不是 NACK
    ASSERT_TRUE(encodePacketHeader(h, buf.data(), buf.size()).isOk());
    buf[PACKET_HEADER_SIZE] = 0;
    buf[PACKET_HEADER_SIZE + 1] = 1;

    std::vector<uint32_t> out;
    EXPECT_EQ(decodeNackPacket(buf.data(), PACKET_HEADER_SIZE + NACK_HEADER_SIZE +
                                               NACK_ENTRY_SIZE, out)
                  .code(),
              Code::NetError)
        << "对端发错类型是网络/对端的问题, 不是本端参数错";
}

TEST(NackPacket, DecodeClearsTheOutputBeforeFilling) {
    std::vector<uint32_t> out = {999, 998, 997};
    roundTrip({7}, out);
    EXPECT_EQ(out, std::vector<uint32_t>({7})) << "调用方复用这个 vector, 残留会变成假缺口";
}
