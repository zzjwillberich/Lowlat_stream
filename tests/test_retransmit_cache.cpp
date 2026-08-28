/**
 * @file    test_retransmit_cache.cpp
 * @brief   M4.1 发送端重传缓存 + 单分片重编的契约测试
 * @author  zzj
 * @date    2026-08-28
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "modules/transport/Packet.h"
#include "modules/transport/Packetizer.h"
#include "modules/transport/RetransmitCache.h"

namespace {
    RetransmitCacheConfig config(int retentionMs = 200, size_t maxFrames = 256) {
        RetransmitCacheConfig cfg;
        cfg.retentionMs = retentionMs;
        cfg.maxFrames = maxFrames;
        return cfg;
    }

    std::vector<uint8_t> makeFrame(size_t len, uint8_t salt) {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>(i * 31 + salt);
        return v;
    }

    EncodedFrameView viewOf(const std::vector<uint8_t>& data, uint64_t frameId, bool isKey) {
        EncodedFrameView v;
        v.data = data.data();
        v.len = data.size();
        v.frameId = frameId;
        v.captureMs = 1000 + frameId;
        v.isKey = isKey;
        return v;
    }

    /** 一帧切成几片 —— 和 Packetizer 的算法一致, 向上取整且不多切空片 */
    uint16_t fragCountOf(size_t len) {
        return static_cast<uint16_t>(len / MAX_PAYLOAD + (len % MAX_PAYLOAD != 0));
    }
}  // namespace

// ---------- 存取 ----------

TEST(RetransmitCache, StoresAFrameAndFindsEveryFragment) {
    RetransmitCache cache(config());
    std::vector<uint8_t> data = makeFrame(3000, 7);  // 3 片
    const uint16_t frags = fragCountOf(data.size());
    ASSERT_EQ(frags, 3u);
    const EncodedFrameView view = viewOf(data, 42, true);

    cache.store(view, data, 500, frags, 1000);

    for (uint16_t i = 0; i < frags; ++i) {
        EncodedFrameView out;
        uint16_t fragIndex = 0xFFFF, fragCount = 0;
        ASSERT_TRUE(cache.find(500 + i, out, fragIndex, fragCount)) << "第 " << i << " 片查不到";
        EXPECT_EQ(fragIndex, i);
        EXPECT_EQ(fragCount, frags);
        EXPECT_EQ(out.len, data.size());
        EXPECT_EQ(out.frameId, 42u);
        EXPECT_EQ(out.captureMs, 1042u) << "captureMs 必须原样带回, 重传包的时间戳不能变";
        EXPECT_TRUE(out.isKey);
    }
    EXPECT_EQ(cache.stats().hits, frags);
    EXPECT_EQ(cache.stats().misses, 0u);
}

TEST(RetransmitCache, SeqsOutsideAnyFrameAreMisses) {
    RetransmitCache cache(config());
    std::vector<uint8_t> data = makeFrame(3000, 7);
    cache.store(viewOf(data, 1, true), data, 500, fragCountOf(data.size()), 1000);

    EncodedFrameView out;
    uint16_t fragIndex = 0, fragCount = 0;
    EXPECT_FALSE(cache.find(499, out, fragIndex, fragCount)) << "比 baseSeq 还小";
    EXPECT_FALSE(cache.find(503, out, fragIndex, fragCount)) << "超出 fragCount";
    EXPECT_EQ(cache.stats().misses, 2u)
        << "miss 必须计数 —— 它是唯一能区分'NACK 没发出去'和'发出去了但这边没货'的线索";
}

/**
 * 存进来的数据必须是**移入**的，不是拷贝。
 * 这是选"存帧而不是存整包"的真正理由（省内存只有 1.9%，见 D23）。
 */
TEST(RetransmitCache, TheFrameDataIsMovedInNotCopied) {
    RetransmitCache cache(config());
    std::vector<uint8_t> data = makeFrame(3000, 7);
    const uint8_t* originalPtr = data.data();
    const EncodedFrameView view = viewOf(data, 1, true);

    cache.store(view, std::move(data), 500, 3, 1000);

    EncodedFrameView out;
    uint16_t fragIndex = 0, fragCount = 0;
    ASSERT_TRUE(cache.find(500, out, fragIndex, fragCount));
    EXPECT_EQ(out.data, originalPtr)
        << "缓存里的字节应当就是原来那块堆内存 —— 走了一次拷贝的话地址会变";
}

// ---------- 按时间淘汰 ----------

/**
 * 按时间淘汰，不按帧数也不按字节数：这个缓存存在的理由本来就是一段时间
 * （覆盖 NACK 的最晚到达时刻）。按帧数定的话 IDR 和 P 帧差一个数量级，
 * 同样 8 帧可能是 30KB 也可能是 300KB。
 */
TEST(RetransmitCache, EntriesOlderThanRetentionAreEvicted) {
    RetransmitCache cache(config(200));
    std::vector<uint8_t> a = makeFrame(1000, 1);
    std::vector<uint8_t> b = makeFrame(1000, 2);
    cache.store(viewOf(a, 1, true), a, 100, 1, 1000);
    cache.store(viewOf(b, 2, false), b, 101, 1, 1150);

    EncodedFrameView out;
    uint16_t fi = 0, fc = 0;
    ASSERT_TRUE(cache.find(100, out, fi, fc)) << "才过 150ms, 不该被淘汰";

    cache.evictExpired(1250);  // 第一帧已经 250ms > 200ms
    EXPECT_FALSE(cache.find(100, out, fi, fc));
    EXPECT_TRUE(cache.find(101, out, fi, fc)) << "第二帧才 100ms, 不该跟着被清掉";
    EXPECT_EQ(cache.stats().framesEvicted, 1u);
    EXPECT_EQ(cache.stats().frames, 1u);
}

TEST(RetransmitCache, StoreItselfEvictsExpiredEntries) {
    RetransmitCache cache(config(100));
    std::vector<uint8_t> a = makeFrame(1000, 1);
    cache.store(viewOf(a, 1, true), a, 100, 1, 1000);
    std::vector<uint8_t> b = makeFrame(1000, 2);
    cache.store(viewOf(b, 2, false), b, 200, 1, 1500);  // 500ms 之后

    EXPECT_EQ(cache.stats().frames, 1u) << "store 该顺手淘汰, 不必等调用方单独调";
    EXPECT_EQ(cache.stats().bytes, 1000u);
}

/**
 * 时间是主判据，帧数上限是兜底：码率异常或时钟出问题时不能让内存无界。
 * 任何按外部输入增长的容器都要有一个不依赖那个输入的上限（同 FrameAssembler）。
 */
TEST(RetransmitCache, MaxFramesCapsMemoryEvenIfNothingExpires) {
    RetransmitCache cache(config(1000000, 4));  // 保留时长长到永不过期
    for (uint32_t i = 0; i < 20; ++i) {
        std::vector<uint8_t> f = makeFrame(1000, static_cast<uint8_t>(i));
        cache.store(viewOf(f, i, false), f, 100 + i, 1, 1000 + i);
    }
    EXPECT_EQ(cache.stats().frames, 4u);
    EXPECT_EQ(cache.stats().bytes, 4000u);

    EncodedFrameView out;
    uint16_t fi = 0, fc = 0;
    EXPECT_FALSE(cache.find(100, out, fi, fc)) << "最老的该被挤掉";
    EXPECT_TRUE(cache.find(119, out, fi, fc)) << "最新的必须还在";
}

TEST(RetransmitCache, ABackwardClockDoesNotWipeTheCache) {
    RetransmitCache cache(config(200));
    std::vector<uint8_t> a = makeFrame(1000, 1);
    cache.store(viewOf(a, 1, true), a, 100, 1, 5000);

    cache.evictExpired(4000);  // 时间倒流(不该发生, 但无符号减法会算出天文数字的年龄)
    EncodedFrameView out;
    uint16_t fi = 0, fc = 0;
    EXPECT_TRUE(cache.find(100, out, fi, fc)) << "无符号减法回绕把整个缓存清空了";
}

// ---------- 边界 ----------

TEST(RetransmitCache, SeqWrapAroundIsHandled) {
    RetransmitCache cache(config());
    std::vector<uint8_t> data = makeFrame(3000, 9);
    const uint32_t base = 0xFFFFFFFFu - 1;  // 这一帧的 3 片跨过 0
    cache.store(viewOf(data, 1, true), data, base, 3, 1000);

    for (uint16_t i = 0; i < 3; ++i) {
        EncodedFrameView out;
        uint16_t fragIndex = 0xFFFF, fragCount = 0;
        ASSERT_TRUE(cache.find(base + i, out, fragIndex, fragCount))
            << "回绕点上查不到, 现象是'重传偶发失效, 跑久了才出现'";
        EXPECT_EQ(fragIndex, i);
    }
}

TEST(RetransmitCache, StoringAnEmptyOrZeroFragFrameIsANoOp) {
    RetransmitCache cache(config());
    std::vector<uint8_t> empty;
    cache.store(viewOf(empty, 1, true), empty, 100, 1, 1000);
    std::vector<uint8_t> data = makeFrame(1000, 1);
    cache.store(viewOf(data, 2, true), data, 200, 0, 1000);

    EXPECT_EQ(cache.stats().frames, 0u);
    EXPECT_EQ(cache.stats().bytes, 0u);
}

TEST(RetransmitCache, ResetClearsContentButKeepsCumulativeStats) {
    RetransmitCache cache(config());
    std::vector<uint8_t> data = makeFrame(1000, 1);
    cache.store(viewOf(data, 1, true), data, 100, 1, 1000);
    EncodedFrameView out;
    uint16_t fi = 0, fc = 0;
    ASSERT_TRUE(cache.find(100, out, fi, fc));

    cache.reset();
    EXPECT_EQ(cache.stats().frames, 0u);
    EXPECT_EQ(cache.stats().bytes, 0u);
    EXPECT_FALSE(cache.find(100, out, fi, fc));
    EXPECT_EQ(cache.stats().framesStored, 1u) << "累计统计是整轮运行的账, reset 不该抹掉";
    EXPECT_EQ(cache.stats().hits, 1u);
}

// ---------- 重传包必须和原发包逐字节一致 ----------

/**
 * 这条是整个重传路径的核心：重编出来的分片必须和当初那个**逐字节相同**，
 * 只有 flags 多一个 FLAG_RETRANSMIT。
 *
 * 不一致的现象很难查：接收端把重传片和原发片拼进同一帧，拼出来的是坏数据，
 * 表现为偶发花屏，而重传统计一切正常。
 */
TEST(RetransmitCache, ARefragmentedPacketMatchesTheOriginalByteForByte) {
    std::vector<uint8_t> data = makeFrame(3000, 5);
    const EncodedFrameView view = viewOf(data, 77, true);
    const uint16_t frags = fragCountOf(data.size());

    Packetizer packer(0, 900);
    std::vector<PacketBuffer> original;
    ASSERT_TRUE(packer.packetize(view, original).isOk());
    ASSERT_EQ(original.size(), frags);

    RetransmitCache cache(config());
    cache.store(view, data, 900, frags, 1000);

    for (uint16_t i = 0; i < frags; ++i) {
        EncodedFrameView cached;
        uint16_t fragIndex = 0, fragCount = 0;
        ASSERT_TRUE(cache.find(900 + i, cached, fragIndex, fragCount));

        PacketBuffer retx;
        ASSERT_TRUE(packetizeOneFragment(0, 900 + i, cached, fragIndex, fragCount, true, retx)
                        .isOk());

        ASSERT_EQ(retx.size(), original[i].size()) << "第 " << i << " 片长度就不一样";
        for (size_t b = 0; b < retx.size(); ++b) {
            const bool isFlagsByte = (b == PACKET_HEADER_SIZE + 8);
            if (isFlagsByte) {
                EXPECT_EQ(retx[b], original[i][b] | DataHeader::FLAG_RETRANSMIT)
                    << "flags 应当只是多了重传位";
            } else {
                EXPECT_EQ(retx[b], original[i][b])
                    << "第 " << i << " 片第 " << b << " 字节不一致";
            }
        }
    }
}

/**
 * 重传绝不能推进 Packetizer 的 seq。
 *
 * 推进了的话，后续原发包的 seq 就跳号，接收端把跳号当成丢包，
 * 触发一轮**真正的** NACK 风暴 —— 一次重传引发一片重传。
 * packetizeOneFragment 写成自由函数就是为了让这条成为结构上的保证。
 */
TEST(RetransmitCache, RetransmittingDoesNotAdvanceThePacketizerSeq) {
    std::vector<uint8_t> data = makeFrame(3000, 5);
    const EncodedFrameView view = viewOf(data, 1, true);

    Packetizer packer(0, 900);
    std::vector<PacketBuffer> out;
    ASSERT_TRUE(packer.packetize(view, out).isOk());
    const uint32_t seqAfterFirstFrame = packer.nextSeq();

    PacketBuffer retx;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(packetizeOneFragment(0, 900, view, 0, 3, true, retx).isOk());
    }
    EXPECT_EQ(packer.nextSeq(), seqAfterFirstFrame) << "重传把发送端的 seq 推进了";
}

TEST(RetransmitCache, PacketizeOneFragmentRejectsBadArguments) {
    std::vector<uint8_t> data = makeFrame(3000, 5);
    const EncodedFrameView view = viewOf(data, 1, true);
    PacketBuffer out;

    EXPECT_EQ(packetizeOneFragment(0, 1, view, 0, 0, false, out).code(), Code::InvalidArg)
        << "fragCount 为 0";
    EXPECT_EQ(packetizeOneFragment(0, 1, view, 3, 3, false, out).code(), Code::InvalidArg)
        << "fragIndex >= fragCount";

    EncodedFrameView empty = view;
    empty.data = nullptr;
    EXPECT_EQ(packetizeOneFragment(0, 1, empty, 0, 3, false, out).code(), Code::InvalidArg);
}
