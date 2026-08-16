/**
 * @file    test_frame_assembler.cpp
 * @brief   接收端组包器的契约测试, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-15
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "modules/transport/FrameAssembler.h"
#include "modules/transport/Packet.h"
#include "modules/transport/Packetizer.h"

namespace {
    using Bytes = std::vector<uint8_t>;

    /** 内容各不相同的假载荷, 拼错顺序就能被逐字节比对抓到 */
    Bytes fakePayload(size_t len, uint8_t salt = 0) {
        Bytes v(len);
        for (size_t i = 0; i < len; ++i) {
            v[i] = static_cast<uint8_t>(i * 7 + salt + 1);
        }
        return v;
    }

    /** 手工拼一个 DATA 包, 用于构造各种边界和畸形输入 */
    Bytes makePacket(uint32_t seq, uint32_t frameId, uint16_t fragIndex, uint16_t fragCount,
                     const Bytes& payload, bool isKey = false, uint32_t timestampMs = 1000) {
        Bytes pkt(PACKET_HEADER_SIZE + DATA_HEADER_SIZE + payload.size());

        PacketHeader ph;
        ph.type = PacketType::Data;
        ph.seq = seq;
        ph.timestampMs = timestampMs;
        EXPECT_TRUE(encodePacketHeader(ph, pkt.data(), pkt.size()).isOk());

        DataHeader dh;
        dh.frameId = frameId;
        dh.fragIndex = fragIndex;
        dh.fragCount = fragCount;
        dh.flags = isKey ? DataHeader::FLAG_KEYFRAME : uint8_t{0};
        EXPECT_TRUE(encodeDataHeader(dh, pkt.data() + PACKET_HEADER_SIZE,
                                     pkt.size() - PACKET_HEADER_SIZE)
                        .isOk());

        if (!payload.empty()) {
            std::memcpy(pkt.data() + PACKET_HEADER_SIZE + DATA_HEADER_SIZE, payload.data(),
                        payload.size());
        }
        return pkt;
    }

    Status offerPacket(FrameAssembler& fa, const Bytes& pkt) {
        return fa.offer(pkt.data(), pkt.size());
    }
}  // namespace

// ---------- 输入校验 ----------

TEST(FrameAssembler, EmptyAssemblerHasNothingToPop) {
    FrameAssembler fa;
    AssembledFrame frame;
    EXPECT_FALSE(fa.pop(frame));
    EXPECT_EQ(fa.pendingFrames(), 0u);
}

TEST(FrameAssembler, RejectsNullPacket) {
    FrameAssembler fa;
    // 本端传错了指针, 不是网络的问题
    EXPECT_EQ(fa.offer(nullptr, 100).code(), Code::InvalidArg);
    EXPECT_EQ(fa.stats().packetsMalformed, 0u);
}

TEST(FrameAssembler, TooShortPacketIsANetworkErrorNotAnArgumentError) {
    FrameAssembler fa;
    const Bytes stub(5, 0);

    // 底层 decodePacketHeader 对短缓冲区报 InvalidArg, 但那儿的缓冲区是本端给的;
    // 这里的长度是网线上来的, 责任方不同, 必须在这一层翻译成 NetError
    EXPECT_EQ(fa.offer(stub.data(), stub.size()).code(), Code::NetError);
    EXPECT_EQ(fa.stats().packetsMalformed, 1u);
}

TEST(FrameAssembler, MalformedHeaderIsCountedAndDropped) {
    FrameAssembler fa;
    Bytes pkt = makePacket(0, 1, 0, 1, fakePayload(10));
    pkt[0] = PROTOCOL_VERSION + 1;  // 版本不认识

    EXPECT_EQ(offerPacket(fa, pkt).code(), Code::NetError);
    EXPECT_EQ(fa.stats().packetsMalformed, 1u);
    EXPECT_EQ(fa.stats().packetsReceived, 0u);
}

TEST(FrameAssembler, NonDataPacketIsTheCallersDispatchBug) {
    FrameAssembler fa;
    Bytes pkt = makePacket(0, 1, 0, 1, fakePayload(10));
    pkt[1] = static_cast<uint8_t>(PacketType::Nack);

    // NACK 是合法的包, 只是不归组包器管 —— 喂错了是调用方分派错了
    EXPECT_EQ(offerPacket(fa, pkt).code(), Code::InvalidArg);
    EXPECT_EQ(fa.stats().packetsMalformed, 0u) << "包本身没毛病, 不该算进畸形包";
}

// ---------- 组包 ----------

TEST(FrameAssembler, SingleFragmentFrameCompletesImmediately) {
    FrameAssembler fa;
    const Bytes payload = fakePayload(300);
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 7, 0, 1, payload)).isOk());

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.data, payload);
    EXPECT_EQ(frame.frameId, 7u);
    EXPECT_FALSE(fa.pop(frame));
}

TEST(FrameAssembler, MultiFragmentFrameCompletesOnlyOnTheLastFragment) {
    FrameAssembler fa;
    AssembledFrame frame;

    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 3, fakePayload(100, 0))).isOk());
    EXPECT_FALSE(fa.pop(frame)) << "才 1/3 片就交出去, 解码器拿到的是半帧";
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 1, 1, 3, fakePayload(100, 1))).isOk());
    EXPECT_FALSE(fa.pop(frame));
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 1, 2, 3, fakePayload(100, 2))).isOk());
    EXPECT_TRUE(fa.pop(frame));

    EXPECT_EQ(fa.stats().framesCompleted, 1u);
}

TEST(FrameAssembler, ReassemblesInFragmentIndexOrderRegardlessOfArrival) {
    FrameAssembler fa;
    const Bytes a = fakePayload(100, 0);
    const Bytes b = fakePayload(100, 1);
    const Bytes c = fakePayload(100, 2);

    // 乱序到达 —— UDP 上这是常态, 不是异常
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 1, 2, 3, c)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 3, a)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 1, 1, 3, b)).isOk());

    Bytes expected;
    expected.insert(expected.end(), a.begin(), a.end());
    expected.insert(expected.end(), b.begin(), b.end());
    expected.insert(expected.end(), c.begin(), c.end());

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.data, expected) << "必须按 fragIndex 排, 不是按到达顺序";
}

TEST(FrameAssembler, CarriesFrameMetadata) {
    FrameAssembler fa;
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 42, 0, 1, fakePayload(50), true, 987654)).isOk());

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.frameId, 42u);
    EXPECT_EQ(frame.timestampMs, 987654u) << "端到端延迟的起点, 必须原样透传";
    EXPECT_TRUE(frame.isKey);
}

TEST(FrameAssembler, InterleavedFramesAssembleIndependently) {
    FrameAssembler fa;
    const Bytes a0 = fakePayload(100, 10);
    const Bytes a1 = fakePayload(100, 11);
    const Bytes b0 = fakePayload(100, 20);
    const Bytes b1 = fakePayload(100, 21);

    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 2, a0)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 2, 0, 2, b0)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 1, 1, 2, a1)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(3, 2, 1, 2, b1)).isOk());

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.frameId, 1u);
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.frameId, 2u);
}

TEST(FrameAssembler, CompletesInArrivalOrderNotFrameIdOrder) {
    FrameAssembler fa;

    // 帧 1 缺一片, 帧 2 先齐 —— 就先交出帧 2。
    // 按显示顺序排是 M3 jitter buffer 的职责, 混进这里就再也没法单独测了
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 2, 0, 1, fakePayload(100))).isOk());

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.frameId, 2u);
}

// ---------- 重复包 ----------

TEST(FrameAssembler, DuplicateFragmentIsIgnoredNotAppended) {
    FrameAssembler fa;
    const Bytes a = fakePayload(100, 0);
    const Bytes b = fakePayload(100, 1);

    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 2, a)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 1, 0, 2, a)).isOk());  // 同一片又来一次
    EXPECT_EQ(fa.stats().packetsDuplicate, 1u);

    AssembledFrame frame;
    EXPECT_FALSE(fa.pop(frame)) << "重复片不能被当成新片凑数, 那样帧会提前'齐'";

    ASSERT_TRUE(offerPacket(fa, makePacket(2, 1, 1, 2, b)).isOk());
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.data.size(), a.size() + b.size());
}

TEST(FrameAssembler, RetransmittedCompleteFrameIsNotDeliveredTwice) {
    FrameAssembler fa;
    const Bytes payload = fakePayload(100);

    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 1, payload)).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 1, 0, 1, payload)).isOk());  // M4 的重传

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    // 墓碑挡住了整帧重传: 删掉条目的话这里会再出一帧, 解码器收到重复帧
    EXPECT_FALSE(fa.pop(frame));
    EXPECT_EQ(fa.stats().framesCompleted, 1u);
    EXPECT_EQ(fa.stats().packetsDuplicate, 1u);
}

TEST(FrameAssembler, RejectsFragCountThatContradictsTheFrame) {
    FrameAssembler fa;
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 3, fakePayload(100))).isOk());

    // 同一帧的第二片却声称总共 60000 片。照着新值 resize 的话, 伪造一个包就能
    // 让接收端分配一大片内存, 而那一帧永远不可能齐
    EXPECT_EQ(offerPacket(fa, makePacket(1, 1, 1, 60000, fakePayload(100))).code(),
              Code::NetError);
    EXPECT_EQ(fa.stats().packetsMalformed, 1u);
}

// ---------- 容量与淘汰 ----------

TEST(FrameAssembler, EvictsOldestPendingFrameWhenCapacityIsExceeded) {
    FrameAssembler fa(2);

    // 三个都缺片的帧, 容量只有 2
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 2, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 3, 0, 2, fakePayload(100))).isOk());

    EXPECT_LE(fa.pendingFrames(), 2u);
    EXPECT_EQ(fa.stats().framesDropped, 1u) << "被放弃的是最老的帧 1";
}

TEST(FrameAssembler, EvictedFrameIsNotResurrectedByALateFragment) {
    FrameAssembler fa(2);
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 2, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 3, 0, 2, fakePayload(100))).isOk());

    // 帧 1 的另一片现在才到。没有淘汰水位的话它会重新建出一个残帧,
    // 占着容量把真正在收的帧挤掉 —— 越丢包挤得越狠
    ASSERT_TRUE(offerPacket(fa, makePacket(3, 1, 1, 2, fakePayload(100))).isOk());
    EXPECT_EQ(fa.stats().packetsTooLate, 1u);
    EXPECT_LE(fa.pendingFrames(), 2u);

    AssembledFrame frame;
    EXPECT_FALSE(fa.pop(frame)) << "已经判死的帧不许复活交付";
}

TEST(FrameAssembler, PendingMemoryStaysBounded) {
    FrameAssembler fa(4);

    // 每帧都只到一片 —— 丢包时这就是常态。不设上限的话跑一晚上就是几个 GB,
    // 而且它不像队列积压那样会在延迟上表现出来, 只会安静地涨
    for (uint32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(offerPacket(fa, makePacket(i, i + 1, 0, 2, fakePayload(100))).isOk());
    }
    EXPECT_LE(fa.pendingFrames(), 4u);
    EXPECT_GT(fa.stats().framesDropped, 0u);
}

TEST(FrameAssembler, EvictionPicksTheOldestAcrossFrameIdWraparound) {
    FrameAssembler fa(2);

    // frameId 和 seq 一样是 32 位循环计数器: 回绕之后 0 比 0xFFFFFFFF 新。
    // 直接比大小会把刚到的帧 0 当成最老的淘汰掉, 现象是"每隔很久画面卡一下"
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 0xFFFFFFFEu, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 0xFFFFFFFFu, 0, 2, fakePayload(100))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 0u, 0, 2, fakePayload(100))).isOk());

    // 该被淘汰的是 0xFFFFFFFE; 帧 0 必须还在, 补齐后能交付
    ASSERT_TRUE(offerPacket(fa, makePacket(3, 0u, 1, 2, fakePayload(100))).isOk());
    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame)) << "回绕点上把新帧当老帧淘汰了";
    EXPECT_EQ(frame.frameId, 0u);
}

// ---------- 丢包统计 ----------

TEST(FrameAssembler, LossIsDerivedFromTheSeqRangeSoReorderingCancelsIt) {
    FrameAssembler fa;

    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 1, fakePayload(10))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(1, 2, 0, 1, fakePayload(10))).isOk());
    ASSERT_TRUE(offerPacket(fa, makePacket(3, 4, 0, 1, fakePayload(10))).isOk());

    EXPECT_EQ(fa.stats().expectedPackets(), 4u);
    EXPECT_EQ(fa.stats().packetsReceived, 3u);
    EXPECT_EQ(fa.stats().packetsLost(), 1u);

    // seq 2 只是晚到了。在发现缺口的当场就记一笔丢包的话, 这一笔再也减不回去,
    // 丢包率只会虚高 —— 然后照着虚高的数字去降码率, 越降越糟
    ASSERT_TRUE(offerPacket(fa, makePacket(2, 3, 0, 1, fakePayload(10))).isOk());
    EXPECT_EQ(fa.stats().packetsLost(), 0u);
    EXPECT_EQ(fa.stats().highestSeq, 3u) << "老包不能把 highestSeq 拉回去";
}

TEST(FrameAssembler, MalformedPacketsAreNotCountedAsLoss) {
    FrameAssembler fa;
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 1, fakePayload(10))).isOk());

    Bytes bad = makePacket(1, 2, 0, 1, fakePayload(10));
    bad[0] = PROTOCOL_VERSION + 1;
    EXPECT_EQ(offerPacket(fa, bad).code(), Code::NetError);

    // 畸形包连 seq 都不该采信, 更不能并进丢包率:
    // 丢包说明网络差, 畸形包说明有人乱发或版本不匹配, 两者的处置完全不同
    EXPECT_EQ(fa.stats().packetsLost(), 0u);
    EXPECT_EQ(fa.stats().packetsMalformed, 1u);
    EXPECT_EQ(fa.stats().expectedPackets(), 1u);
}

// ---------- 重置 ----------

TEST(FrameAssembler, ResetClearsTheWatermarkSoANewSessionIsNotBlocked) {
    FrameAssembler fa(2);
    for (uint32_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(offerPacket(fa, makePacket(i, 1000 + i, 0, 2, fakePayload(100))).isOk());
    }
    ASSERT_GT(fa.stats().framesDropped, 0u);

    fa.reset();
    EXPECT_EQ(fa.pendingFrames(), 0u);
    EXPECT_EQ(fa.stats().packetsReceived, 0u);

    // 重连后新会话从小 frameId 重新开始。水位没清的话它会被上一段挡住,
    // 现象是"重连之后一直黑屏"
    ASSERT_TRUE(offerPacket(fa, makePacket(0, 1, 0, 1, fakePayload(10))).isOk());
    AssembledFrame frame;
    EXPECT_TRUE(fa.pop(frame));
}

// ---------- 与 Packetizer 对接 ----------

TEST(FrameAssembler, RoundTripThroughPacketizerRebuildsTheFrame) {
    // M2 的验收标准缩影: 无丢包时收到的必须和发出的逐字节一致
    Packetizer packer;
    std::vector<PacketBuffer> packets;
    const Bytes original = fakePayload(5000);

    EncodedFrameView view;
    view.data = original.data();
    view.len = original.size();
    view.frameId = 9;
    view.captureMs = 555;
    view.isKey = true;
    ASSERT_TRUE(packer.packetize(view, packets).isOk());
    ASSERT_GT(packets.size(), 1u);

    FrameAssembler fa;
    for (const auto& pkt : packets) {
        ASSERT_TRUE(fa.offer(pkt.data(), pkt.size()).isOk());
    }

    AssembledFrame frame;
    ASSERT_TRUE(fa.pop(frame));
    EXPECT_EQ(frame.data, original);
    EXPECT_EQ(frame.frameId, 9u);
    EXPECT_EQ(frame.timestampMs, 555u);
    EXPECT_TRUE(frame.isKey);
    EXPECT_EQ(fa.stats().packetsLost(), 0u);
}
