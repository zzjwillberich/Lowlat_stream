/**
 * @file    test_packetizer.cpp
 * @brief   分片打包器的契约测试, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-15
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "modules/transport/Packet.h"
#include "modules/transport/Packetizer.h"

namespace {
    /** 内容各不相同的假码流, 拼错顺序就能被逐字节比对抓到 */
    std::vector<uint8_t> fakeStream(size_t len) {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) {
            v[i] = static_cast<uint8_t>(i * 7 + 1);
        }
        return v;
    }

    EncodedFrameView viewOf(const std::vector<uint8_t>& data, uint64_t frameId = 1,
                            uint64_t captureMs = 12345, bool isKey = false) {
        EncodedFrameView f;
        f.data = data.data();
        f.len = data.size();
        f.frameId = frameId;
        f.captureMs = captureMs;
        f.isKey = isKey;
        return f;
    }

    struct ParsedPacket {
        PacketHeader ph;
        DataHeader dh;
        std::vector<uint8_t> payload;
    };

    /** 用真正的 decode 解回来, 顺便把编解码两边接在一起验一遍 */
    ParsedPacket parse(const PacketBuffer& pkt) {
        ParsedPacket p;
        EXPECT_GE(pkt.size(), PACKET_HEADER_SIZE + DATA_HEADER_SIZE);
        EXPECT_TRUE(decodePacketHeader(pkt.data(), pkt.size(), p.ph).isOk());
        EXPECT_TRUE(decodeDataHeader(pkt.data() + PACKET_HEADER_SIZE,
                                     pkt.size() - PACKET_HEADER_SIZE, p.dh)
                        .isOk());
        p.payload.assign(pkt.begin() + PACKET_HEADER_SIZE + DATA_HEADER_SIZE, pkt.end());
        return p;
    }

    /** 按 fragIndex 顺序把载荷拼回来 */
    std::vector<uint8_t> reassemble(const std::vector<PacketBuffer>& pkts) {
        std::vector<uint8_t> out;
        for (const auto& pkt : pkts) {
            const ParsedPacket p = parse(pkt);
            out.insert(out.end(), p.payload.begin(), p.payload.end());
        }
        return out;
    }
}  // namespace

// ---------- 参数校验 ----------

TEST(Packetizer, RejectsEmptyFrame) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const std::vector<uint8_t> empty;

    // 空帧不是"0 个包": fragCount 恒 >= 1, 空载荷的 DATA 包在接收端和"整帧丢了"分不开
    EXPECT_EQ(p.packetize(viewOf(empty), out).code(), Code::InvalidArg);
    EXPECT_EQ(p.nextSeq(), 0u) << "失败了还推进 seq, 对端会把这段算成丢包";
}

TEST(Packetizer, RejectsNullData) {
    Packetizer p;
    std::vector<PacketBuffer> out;

    EncodedFrameView f;
    f.data = nullptr;
    f.len = 100;
    EXPECT_EQ(p.packetize(f, out).code(), Code::InvalidArg);
}

// ---------- 分片边界 ----------

TEST(Packetizer, SmallFrameBecomesOneFragment) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(100);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    ASSERT_EQ(out.size(), 1u);

    const ParsedPacket parsed = parse(out[0]);
    EXPECT_EQ(parsed.dh.fragIndex, 0u);
    EXPECT_EQ(parsed.dh.fragCount, 1u);
    EXPECT_EQ(parsed.payload.size(), 100u);
}

TEST(Packetizer, ExactMultipleOfMaxPayloadDoesNotEmitAnEmptyTail) {
    // 最容易写错的一条: len / MAX_PAYLOAD + 1 会在这里多切一片空的。
    // 而这种帧长偶尔才撞上一次, 现象是"偶发花屏", 极难复现
    for (size_t n : {1u, 2u, 3u}) {
        Packetizer p;
        std::vector<PacketBuffer> out;
        const auto data = fakeStream(MAX_PAYLOAD * n);

        ASSERT_TRUE(p.packetize(viewOf(data), out).isOk()) << "n=" << n;
        EXPECT_EQ(out.size(), n) << "帧长正好是 MAX_PAYLOAD 的 " << n << " 倍";
        for (const auto& pkt : out) {
            EXPECT_EQ(parse(pkt).payload.size(), MAX_PAYLOAD);
        }
    }
}

TEST(Packetizer, OneByteOverTheBoundaryAddsAFragment) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD + 1);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(parse(out[0]).payload.size(), MAX_PAYLOAD);
    EXPECT_EQ(parse(out[1]).payload.size(), 1u) << "最后一片可以很短, 不许补零凑满";
}

TEST(Packetizer, EveryPacketStaysWithinTheMtuBudget) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 4 + 7);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    for (const auto& pkt : out) {
        EXPECT_LE(pkt.size(), MAX_DATA_PACKET_SIZE) << "超了就会触发 IP 分片";
    }
}

TEST(Packetizer, FragmentIndicesAreContiguousAndCountIsConsistent) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 3 + 500);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    ASSERT_EQ(out.size(), 4u);

    for (size_t i = 0; i < out.size(); ++i) {
        const ParsedPacket parsed = parse(out[i]);
        EXPECT_EQ(parsed.dh.fragIndex, i);
        EXPECT_EQ(parsed.dh.fragCount, out.size()) << "每一片都要带总数, 接收端才知道等几片";
    }
}

// ---------- 内容还原 ----------

TEST(Packetizer, PayloadReassemblesToTheOriginalFrame) {
    // M2 的验收标准就是这条: 无丢包时收到的必须和发出的逐字节一致
    for (size_t len : {1u, 1199u, 1200u, 1201u, 5000u}) {
        Packetizer p;
        std::vector<PacketBuffer> out;
        const auto data = fakeStream(len);

        ASSERT_TRUE(p.packetize(viewOf(data), out).isOk()) << "len=" << len;
        EXPECT_EQ(reassemble(out), data) << "len=" << len;
    }
}

// ---------- seq ----------

TEST(Packetizer, SeqIncrementsAcrossFramesWithoutGaps) {
    Packetizer p;
    std::vector<PacketBuffer> first;
    std::vector<PacketBuffer> second;

    const auto a = fakeStream(MAX_PAYLOAD * 2);  // 2 片
    const auto b = fakeStream(100);              // 1 片
    ASSERT_TRUE(p.packetize(viewOf(a, 1), first).isOk());
    ASSERT_TRUE(p.packetize(viewOf(b, 2), second).isOk());

    // seq 不因为换了一帧就重来 —— 它只回答"网络层面丢没丢", 必须一路数下去
    EXPECT_EQ(parse(first[0]).ph.seq, 0u);
    EXPECT_EQ(parse(first[1]).ph.seq, 1u);
    EXPECT_EQ(parse(second[0]).ph.seq, 2u);
    EXPECT_EQ(p.nextSeq(), 3u);

    // 与之相对, frameId 是按帧走的
    EXPECT_EQ(parse(first[0]).dh.frameId, 1u);
    EXPECT_EQ(parse(second[0]).dh.frameId, 2u);
}

TEST(Packetizer, SeqWrapsAroundWithoutSkippingZero) {
    Packetizer p(0, 0xFFFFFFFE);
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 3);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    ASSERT_EQ(out.size(), 3u);

    EXPECT_EQ(parse(out[0]).ph.seq, 0xFFFFFFFEu);
    EXPECT_EQ(parse(out[1]).ph.seq, 0xFFFFFFFFu);
    EXPECT_EQ(parse(out[2]).ph.seq, 0u) << "回绕就是回绕, 不要跳过 0 也不要停在最大值";
    EXPECT_EQ(p.nextSeq(), 1u);
}

// ---------- 头字段 ----------

TEST(Packetizer, AllFragmentsShareTheCaptureTimestamp) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 3);

    ASSERT_TRUE(p.packetize(viewOf(data, 1, 999999), out).isOk());
    for (const auto& pkt : out) {
        // 打包时重新取时间的话, 端到端延迟里就少算了编码耗时 —— 那正是要测的东西之一
        EXPECT_EQ(parse(pkt).ph.timestampMs, 999999u);
    }
}

TEST(Packetizer, KeyframeFlagIsSetOnEveryFragment) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 3);

    ASSERT_TRUE(p.packetize(viewOf(data, 1, 0, true), out).isOk());
    for (const auto& pkt : out) {
        // 只打第一片的话, M4 按包丢的背压策略无从判断中间那些包该保该丢,
        // 结果是 IDR 被丢掉一半, 花屏一直撑到下一个 IDR
        EXPECT_TRUE(parse(pkt).dh.flags & DataHeader::FLAG_KEYFRAME);
    }

    std::vector<PacketBuffer> nonKey;
    ASSERT_TRUE(p.packetize(viewOf(data, 2, 0, false), nonKey).isOk());
    EXPECT_FALSE(parse(nonKey[0]).dh.flags & DataHeader::FLAG_KEYFRAME);
}

TEST(Packetizer, FrameIdIsTruncatedToThirtyTwoBits) {
    Packetizer p;
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(100);

    // 线上字段就 4 字节, 截断是有意的: 组包只需要"最近这些帧里各不相同"
    ASSERT_TRUE(p.packetize(viewOf(data, 0x1'0000'0007ULL), out).isOk());
    EXPECT_EQ(parse(out[0]).dh.frameId, 7u);
}

TEST(Packetizer, StreamIdIsCarriedOnEveryPacket) {
    Packetizer p(0x1234);
    std::vector<PacketBuffer> out;
    const auto data = fakeStream(MAX_PAYLOAD * 2);

    ASSERT_TRUE(p.packetize(viewOf(data), out).isOk());
    EXPECT_EQ(p.streamId(), 0x1234u);
    for (const auto& pkt : out) {
        EXPECT_EQ(parse(pkt).ph.streamId, 0x1234u);
        EXPECT_EQ(parse(pkt).ph.type, PacketType::Data);
    }
}

// ---------- 出参复用 ----------

TEST(Packetizer, OutputIsResetOnEachCall) {
    Packetizer p;
    std::vector<PacketBuffer> out;

    const auto big = fakeStream(MAX_PAYLOAD * 4);
    ASSERT_TRUE(p.packetize(viewOf(big, 1), out).isOk());
    ASSERT_EQ(out.size(), 4u);

    // 复用出参不等于往后追加: 忘了重置, 包会一帧比一帧多, 越发越慢直到发不动
    const auto small = fakeStream(100);
    ASSERT_TRUE(p.packetize(viewOf(small, 2), out).isOk());
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(reassemble(out), small);
}
