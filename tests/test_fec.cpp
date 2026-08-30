/**
 * @file    test_fec.cpp
 * @brief   M4.2 FEC 头编解码 + 编码器 + 解码器的契约测试
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 端到端的那几条(丢一个能恢复、丢两个恢复不了)才是这套测试的核心 ——
 *       XOR 没有校验, 恢复出来的东西"看着像对的"是常态, 所以判据必须是
 *       **和原包逐字节相同**, 不能只看"恢复出来了"。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "modules/transport/FecDecoder.h"
#include "modules/transport/FecEncoder.h"
#include "modules/transport/Packet.h"
#include "modules/transport/Packetizer.h"

namespace {
    constexpr uint32_t BASE_SEQ = 5000;

    FecEncoderConfig encoderConfig(uint16_t groupSize = 4, uint16_t minGroupSize = 2) {
        FecEncoderConfig cfg;
        cfg.groupSize = groupSize;
        cfg.minGroupSize = minGroupSize;
        return cfg;
    }

    FecDecoderConfig decoderConfig(size_t recent = 64) {
        FecDecoderConfig cfg;
        cfg.recentPackets = recent;
        return cfg;
    }

    std::vector<uint8_t> makeFrame(size_t len, uint8_t salt) {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>(i * 37 + salt);
        return v;
    }

    /** 把一帧切成分片(完整 UDP 包), 顺带返回 view */
    std::vector<PacketBuffer> packetize(const std::vector<uint8_t>& data, uint32_t baseSeq,
                                        uint64_t frameId, bool isKey, uint64_t captureMs) {
        EncodedFrameView view;
        view.data = data.data();
        view.len = data.size();
        view.frameId = frameId;
        view.captureMs = captureMs;
        view.isKey = isKey;

        Packetizer packer(0, baseSeq);
        std::vector<PacketBuffer> out;
        EXPECT_TRUE(packer.packetize(view, out).isOk());
        return out;
    }

    uint32_t seqOf(const PacketBuffer& packet) {
        PacketHeader h;
        EXPECT_TRUE(decodePacketHeader(packet.data(), packet.size(), h).isOk());
        return h.seq;
    }
}  // namespace

// ---------- FecHeader 线上布局 ----------

TEST(FecHeader, WireLayoutAndRoundTrip) {
    FecHeader in;
    in.groupBaseSeq = 0x11223344;
    in.groupSize = 4;
    in.payloadLenXor = 0xBEEF;
    in.groupIndex = 2;
    in.groupCount = 7;
    in.reserved = 0;

    std::vector<uint8_t> buf(FEC_HEADER_SIZE + 1, 0xAB);
    ASSERT_TRUE(encodeFecHeader(in, buf.data(), FEC_HEADER_SIZE).isOk());

    // 逐字节核对偏移和大端序 —— 这条不过两端永远互通不了
    EXPECT_EQ(buf[0], 0x11); EXPECT_EQ(buf[1], 0x22);
    EXPECT_EQ(buf[2], 0x33); EXPECT_EQ(buf[3], 0x44);
    EXPECT_EQ(buf[4], 0x00); EXPECT_EQ(buf[5], 0x04);
    EXPECT_EQ(buf[6], 0xBE); EXPECT_EQ(buf[7], 0xEF);
    EXPECT_EQ(buf[8], 2);
    EXPECT_EQ(buf[9], 7);
    EXPECT_EQ(buf[FEC_HEADER_SIZE], 0xAB) << "写超了 FEC_HEADER_SIZE";

    FecHeader out;
    ASSERT_TRUE(decodeFecHeader(buf.data(), FEC_HEADER_SIZE, out).isOk());
    EXPECT_EQ(out.groupBaseSeq, in.groupBaseSeq);
    EXPECT_EQ(out.groupSize, in.groupSize);
    EXPECT_EQ(out.payloadLenXor, in.payloadLenXor);
    EXPECT_EQ(out.groupIndex, in.groupIndex);
    EXPECT_EQ(out.groupCount, in.groupCount);
}

TEST(FecHeader, GroupSizeBelowTwoIsRejected) {
    FecHeader in;
    in.groupSize = 1;  // 1 个包的"FEC"是原包副本, 不是纠错
    std::vector<uint8_t> buf(FEC_HEADER_SIZE);
    EXPECT_EQ(encodeFecHeader(in, buf.data(), buf.size()).code(), Code::InvalidArg);

    in.groupSize = 2;
    ASSERT_TRUE(encodeFecHeader(in, buf.data(), buf.size()).isOk());
    buf[4] = 0; buf[5] = 1;  // 手改成 1, 模拟对端发错或包被损坏
    FecHeader out;
    EXPECT_EQ(decodeFecHeader(buf.data(), buf.size(), out).code(), Code::NetError)
        << "对端的问题该是 NetError(丢包+计数), 不是 InvalidArg(改代码)";
}

/** reserved 是给以后留的扩展位, 老接收端必须忽略而不是判成畸形 */
TEST(FecHeader, ReservedBitsAreIgnoredNotRejected) {
    std::vector<uint8_t> buf(FEC_HEADER_SIZE, 0);
    FecHeader in;
    in.groupSize = 4;
    ASSERT_TRUE(encodeFecHeader(in, buf.data(), buf.size()).isOk());
    buf[10] = 0xFF; buf[11] = 0xFF;

    FecHeader out;
    EXPECT_TRUE(decodeFecHeader(buf.data(), buf.size(), out).isOk());
}

TEST(FecHeader, RejectsShortBuffers) {
    FecHeader in;
    in.groupSize = 2;
    std::vector<uint8_t> buf(FEC_HEADER_SIZE);
    EXPECT_EQ(encodeFecHeader(in, buf.data(), FEC_HEADER_SIZE - 1).code(), Code::InvalidArg);
    EXPECT_EQ(encodeFecHeader(in, nullptr, FEC_HEADER_SIZE).code(), Code::InvalidArg);

    FecHeader out;
    EXPECT_EQ(decodeFecHeader(buf.data(), FEC_HEADER_SIZE - 1, out).code(), Code::InvalidArg);
}

/**
 * FEC 包比 DATA 包大 3 字节。收包缓冲按 MAX_DATA_PACKET_SIZE 开的话，
 * 满载的 FEC 包会被 recvfrom 的 MSG_TRUNC 判成"数据报大于缓冲区"——
 * 现象是"FEC 全部收不到而 DATA 一切正常"，排查方向会指向 FEC 的编码逻辑。
 */
TEST(FecHeader, MaxPacketSizeCoversTheLargestPacketType) {
    EXPECT_EQ(MAX_FEC_PACKET_SIZE, PACKET_HEADER_SIZE + FEC_HEADER_SIZE + MAX_PAYLOAD);
    EXPECT_GT(MAX_FEC_PACKET_SIZE, MAX_DATA_PACKET_SIZE) << "FEC 头比 DATA 头宽 3 字节";
    EXPECT_GE(MAX_PACKET_SIZE, MAX_FEC_PACKET_SIZE);
    EXPECT_GE(MAX_PACKET_SIZE, MAX_DATA_PACKET_SIZE);
    EXPECT_GE(MAX_PACKET_SIZE,
              PACKET_HEADER_SIZE + NACK_HEADER_SIZE + MAX_NACK_ENTRIES * NACK_ENTRY_SIZE);
}

// ---------- 编码器 ----------

TEST(FecEncoder, DisabledWhenGroupSizeIsZero) {
    FecEncoder encoder(encoderConfig(0));
    EXPECT_FALSE(encoder.enabled());

    std::vector<uint8_t> data = makeFrame(5000, 1);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());
    EXPECT_TRUE(fec.empty());
    EXPECT_EQ(encoder.stats().fecPacketsBuilt, 0u);
}

TEST(FecEncoder, SplitsAFrameIntoGroupsOfK) {
    FecEncoder encoder(encoderConfig(4));
    std::vector<uint8_t> data = makeFrame(1200 * 8, 3);  // 正好 8 片
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    ASSERT_EQ(packets.size(), 8u);

    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());
    ASSERT_EQ(fec.size(), 2u) << "8 片 / 每组 4 = 2 组";

    for (size_t g = 0; g < fec.size(); ++g) {
        PacketHeader ph;
        ASSERT_TRUE(decodePacketHeader(fec[g].data(), fec[g].size(), ph).isOk());
        EXPECT_EQ(ph.type, PacketType::Fec);
        EXPECT_EQ(ph.seq, static_cast<uint32_t>(g)) << "FEC 用自己的计数器, 从 0 开始";

        FecHeader fh;
        ASSERT_TRUE(decodeFecHeader(fec[g].data() + PACKET_HEADER_SIZE,
                                    fec[g].size() - PACKET_HEADER_SIZE, fh)
                        .isOk());
        EXPECT_EQ(fh.groupBaseSeq, BASE_SEQ + g * 4);
        EXPECT_EQ(fh.groupSize, 4u);
        EXPECT_EQ(fh.groupIndex, g);
        EXPECT_EQ(fh.groupCount, 2u);
    }
    EXPECT_EQ(encoder.nextFecSeq(), 2u);
}

/**
 * FEC 的 seq 和 DATA 的 seq 是两个独立空间。
 * 共用的话接收端的 packetsLost() = seq 跨度 − 组包器实收 会把每个 FEC 包
 * 都算成丢包（组包器只收 DATA），丢包率被永久虚高 1/K。
 */
TEST(FecEncoder, FecUsesItsOwnSeqSpace) {
    FecEncoder encoder(encoderConfig(4));
    std::vector<uint8_t> data = makeFrame(1200 * 4, 5);
    std::vector<PacketBuffer> fec;

    ASSERT_TRUE(encoder
                    .buildForFrame(packetize(data, 100000, 1, true, 1000), 100000, 0, 1000, fec)
                    .isOk());
    ASSERT_EQ(fec.size(), 1u);
    EXPECT_EQ(seqOf(fec[0]), 0u) << "FEC 的第一个包 seq 是 0, 和 DATA 的 100000 无关";

    ASSERT_TRUE(encoder
                    .buildForFrame(packetize(data, 200000, 2, false, 2000), 200000, 0, 2000, fec)
                    .isOk());
    ASSERT_EQ(fec.size(), 1u);
    EXPECT_EQ(seqOf(fec[0]), 1u);
}

TEST(FecEncoder, SkipsGroupsSmallerThanTheMinimum) {
    FecEncoder encoder(encoderConfig(4, 2));
    // 5 片 -> 组0 = 4 片(发), 组1 = 1 片(跳过)
    std::vector<uint8_t> data = makeFrame(1200 * 4 + 100, 7);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    ASSERT_EQ(packets.size(), 5u);

    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());
    EXPECT_EQ(fec.size(), 1u) << "只有 1 个包的组不该发 FEC —— 那是原包副本, 不是纠错";
    EXPECT_EQ(encoder.stats().groupsSkipped, 1u);
}

TEST(FecEncoder, ASingleFragmentFrameProducesNoFec) {
    FecEncoder encoder(encoderConfig(4, 2));
    std::vector<uint8_t> data = makeFrame(500, 9);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    ASSERT_EQ(packets.size(), 1u);

    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());
    EXPECT_TRUE(fec.empty());
}

/** FEC 包不能超过 MAX_FEC_PACKET_SIZE，否则会触发 IP 分片或被对端判成畸形 */
TEST(FecEncoder, FecPacketsNeverExceedTheWireLimit) {
    FecEncoder encoder(encoderConfig(4));
    std::vector<uint8_t> data = makeFrame(1200 * 12, 11);  // 全是满载分片
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder
                    .buildForFrame(packetize(data, BASE_SEQ, 1, true, 1000), BASE_SEQ, 0, 1000,
                                   fec)
                    .isOk());
    ASSERT_FALSE(fec.empty());
    for (const PacketBuffer& p : fec) {
        EXPECT_LE(p.size(), MAX_FEC_PACKET_SIZE);
        EXPECT_GT(p.size(), PACKET_HEADER_SIZE + FEC_HEADER_SIZE);
    }
}

// ---------- 端到端恢复 ----------

namespace {
    /** 把一帧发一遍, dropIndex 里列出的分片"丢掉"; 返回恢复出来的包 */
    std::vector<PacketBuffer> runFrame(FecEncoder& encoder, FecDecoder& decoder,
                                       const std::vector<PacketBuffer>& packets,
                                       const std::vector<PacketBuffer>& fec,
                                       const std::vector<size_t>& dropIndex) {
        for (size_t i = 0; i < packets.size(); ++i) {
            if (std::find(dropIndex.begin(), dropIndex.end(), i) != dropIndex.end()) continue;
            decoder.onDataPacket(packets[i].data(), packets[i].size());
        }
        std::vector<PacketBuffer> recovered;
        for (const PacketBuffer& f : fec) {
            PacketBuffer out;
            if (decoder.onFecPacket(f.data(), f.size(), out)) recovered.push_back(out);
        }
        (void)encoder;
        return recovered;
    }
}  // namespace

/**
 * 核心用例：丢一个能恢复，而且恢复出来的必须和原包**逐字节相同**。
 *
 * 判据不能只是"恢复出来了"——XOR 没有校验，异或错组也会给出一串看着合法的字节，
 * 长度也对，组包器照收不误，最后表现为偶发花屏而没有任何报错。
 */
TEST(FecEndToEnd, OneLostPacketPerGroupIsRecoveredByteForByte) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    std::vector<uint8_t> data = makeFrame(1200 * 3 + 500, 13);  // 4 片, 最后一片短
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 42, true, 77777);
    ASSERT_EQ(packets.size(), 4u);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 77777, fec).isOk());
    ASSERT_EQ(fec.size(), 1u);

    for (size_t dropped = 0; dropped < packets.size(); ++dropped) {
        FecDecoder d(decoderConfig());
        const std::vector<PacketBuffer> recovered = runFrame(encoder, d, packets, fec, {dropped});
        ASSERT_EQ(recovered.size(), 1u) << "丢第 " << dropped << " 片没恢复出来";
        EXPECT_EQ(recovered[0], packets[dropped])
            << "第 " << dropped << " 片恢复出来了但和原包不一致";
        EXPECT_EQ(d.stats().groupsRecovered, 1u);
        EXPECT_EQ(d.stats().packetsRecovered, 1u);
    }
    (void)decoder;
}

/** 最后一片是短的：长度必须从 payloadLenXor 反解，用异或缓冲的长度是错的 */
TEST(FecEndToEnd, TheShortLastFragmentRecoversWithTheRightLength) {
    FecEncoder encoder(encoderConfig(4));
    std::vector<uint8_t> data = makeFrame(1200 * 3 + 7, 17);  // 最后一片只有 7 字节
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, false, 1000);
    ASSERT_EQ(packets.back().size(), PACKET_HEADER_SIZE + DATA_HEADER_SIZE + 7);

    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());

    FecDecoder decoder(decoderConfig());
    const std::vector<PacketBuffer> recovered =
        runFrame(encoder, decoder, packets, fec, {packets.size() - 1});
    ASSERT_EQ(recovered.size(), 1u);
    EXPECT_EQ(recovered[0].size(), packets.back().size())
        << "长度取成了组内最长的那片 —— payloadLenXor 没用上";
    EXPECT_EQ(recovered[0], packets.back());
}

TEST(FecEndToEnd, TwoLostPacketsInOneGroupCannotBeRecovered) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    std::vector<uint8_t> data = makeFrame(1200 * 4, 19);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());

    const std::vector<PacketBuffer> recovered = runFrame(encoder, decoder, packets, fec, {0, 2});
    EXPECT_TRUE(recovered.empty());
    EXPECT_EQ(decoder.stats().groupsUnrecoverable, 1u);
    EXPECT_EQ(decoder.stats().groupsRecovered, 0u);
}

TEST(FecEndToEnd, AGroupWithNoLossIsCountedSeparately) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    std::vector<uint8_t> data = makeFrame(1200 * 4, 23);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());

    const std::vector<PacketBuffer> recovered = runFrame(encoder, decoder, packets, fec, {});
    EXPECT_TRUE(recovered.empty());
    EXPECT_EQ(decoder.stats().groupsComplete, 1u)
        << "一个不缺和救不回来必须分开数 —— 前者是常态, 后者是 XOR 的天花板";
    EXPECT_EQ(decoder.stats().groupsUnrecoverable, 0u);
}

/** FEC 包自己丢了：什么都不该发生，也不该被算成"救不回来" */
TEST(FecEndToEnd, ALostFecPacketJustMeansNoRecovery) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    std::vector<uint8_t> data = makeFrame(1200 * 4, 29);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());

    const std::vector<PacketBuffer> recovered = runFrame(encoder, decoder, packets, {}, {1});
    EXPECT_TRUE(recovered.empty());
    EXPECT_EQ(decoder.stats().fecPacketsReceived, 0u);
    EXPECT_EQ(decoder.stats().groupsUnrecoverable, 0u);
}

TEST(FecEndToEnd, RecoveryWorksAcrossSeqWrapAround) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    const uint32_t base = 0xFFFFFFFFu - 1;  // 这一帧的分片跨过 0
    std::vector<uint8_t> data = makeFrame(1200 * 3 + 200, 31);
    const std::vector<PacketBuffer> packets = packetize(data, base, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, base, 0, 1000, fec).isOk());

    const std::vector<PacketBuffer> recovered = runFrame(encoder, decoder, packets, fec, {2});
    ASSERT_EQ(recovered.size(), 1u) << "回绕点上组成员匹配失效";
    EXPECT_EQ(recovered[0], packets[2]);
}

TEST(FecDecoderBasics, MalformedFecPacketsAreCountedNotCrashed) {
    FecDecoder decoder(decoderConfig());
    PacketBuffer out;

    const std::vector<uint8_t> garbage(64, 0xEE);
    EXPECT_FALSE(decoder.onFecPacket(garbage.data(), garbage.size(), out));

    // 合法包头但类型是 DATA
    std::vector<uint8_t> wrongType(PACKET_HEADER_SIZE + FEC_HEADER_SIZE + 10, 0);
    PacketHeader h;
    h.type = PacketType::Data;
    ASSERT_TRUE(encodePacketHeader(h, wrongType.data(), wrongType.size()).isOk());
    EXPECT_FALSE(decoder.onFecPacket(wrongType.data(), wrongType.size(), out));

    EXPECT_GE(decoder.stats().fecPacketsMalformed, 2u);
    EXPECT_EQ(decoder.stats().groupsRecovered, 0u);
}

TEST(FecDecoderBasics, TheRecentBufferStaysBounded) {
    FecDecoder decoder(decoderConfig(8));
    std::vector<uint8_t> data = makeFrame(1200 * 20, 37);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    ASSERT_GT(packets.size(), 8u);

    for (const PacketBuffer& p : packets) decoder.onDataPacket(p.data(), p.size());
    EXPECT_LE(decoder.stats().recentPackets, 8u) << "最近包缓冲没有上界就是内存泄漏";
}

TEST(FecDecoderBasics, ResetClearsTheBufferButKeepsCumulativeStats) {
    FecEncoder encoder(encoderConfig(4));
    FecDecoder decoder(decoderConfig());

    std::vector<uint8_t> data = makeFrame(1200 * 4, 41);
    const std::vector<PacketBuffer> packets = packetize(data, BASE_SEQ, 1, true, 1000);
    std::vector<PacketBuffer> fec;
    ASSERT_TRUE(encoder.buildForFrame(packets, BASE_SEQ, 0, 1000, fec).isOk());
    ASSERT_EQ(runFrame(encoder, decoder, packets, fec, {1}).size(), 1u);

    decoder.reset();
    EXPECT_EQ(decoder.stats().recentPackets, 0u);
    EXPECT_EQ(decoder.stats().groupsRecovered, 1u) << "累计统计是整轮运行的账, reset 不该抹掉";
}
