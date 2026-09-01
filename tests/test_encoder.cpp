/**
 * @file    test_encoder.cpp
 * @brief   Encoder 单元测试 — 覆盖 M1.3 要求的用例
 * @author  zzj
 * @date    2026-07-28
 */
#include <gtest/gtest.h>

#include <vector>

#include "modules/capture/NullSource.h"
#include "modules/encode/Encoder.h"

namespace {
    constexpr int W   = 320;
    constexpr int H   = 240;
    constexpr int FPS = 1000;  // 单测不真等帧率, 节流的正确性由 NullSource 那边测

    EncoderConfig encCfg(int gop = 30) {
        EncoderConfig c;
        c.width  = W;
        c.height = H;
        c.fps    = 30;
        c.gop    = gop;
        return c;
    }

    SourceConfig srcCfg() {
        SourceConfig c;
        c.width  = W;
        c.height = H;
        c.fps    = FPS;
        return c;
    }

    /**
     * @brief 编码 n 帧, 返回所有产出的码流帧
     *
     * @note 顺带覆盖了"编码器可能一次吐 0 个包"的情况: 调用方必须把 out 当数组用。
     */
    std::vector<EncodedFrame> encodeFrames(Encoder& enc, NullSource& src, int n) {
        std::vector<EncodedFrame> all;
        RawFrame f;
        for (int i = 0; i < n; ++i) {
            EXPECT_TRUE(src.readFrame(f).isOk());
            std::vector<EncodedFrame> got;
            EXPECT_TRUE(enc.encode(f, got).isOk());
            for (auto& p : got) all.push_back(std::move(p));
        }
        std::vector<EncodedFrame> tail;
        EXPECT_TRUE(enc.flush(tail).isOk());
        for (auto& p : tail) all.push_back(std::move(p));
        return all;
    }
}  // namespace

// ========== 参数校验与生命周期 ==========

TEST(Encoder, RejectsInvalidConfig) {
    Encoder enc;

    EncoderConfig c = encCfg();
    c.width = 321;
    EXPECT_EQ(enc.open(c).code(), Code::InvalidArg);

    c = encCfg();
    c.fps = 0;
    EXPECT_EQ(enc.open(c).code(), Code::InvalidArg);

    c = encCfg();
    c.bitrateKbps = 0;
    EXPECT_EQ(enc.open(c).code(), Code::InvalidArg);

    c = encCfg();
    c.gop = 0;
    EXPECT_EQ(enc.open(c).code(), Code::InvalidArg);
}

TEST(Encoder, EncodeBeforeOpenFails) {
    Encoder enc;
    RawFrame f;
    f.reset(W, H);
    std::vector<EncodedFrame> out;
    EXPECT_EQ(enc.encode(f, out).code(), Code::Internal);
}

TEST(Encoder, RejectsGeometryMismatch) {
    Encoder enc;
    ASSERT_TRUE(enc.open(encCfg()).isOk());

    RawFrame f;
    f.reset(W / 2, H);  // 分辨率对不上 open 时的约定
    std::vector<EncodedFrame> out;
    EXPECT_EQ(enc.encode(f, out).code(), Code::InvalidArg);
}

TEST(Encoder, ReopenIsAllowed) {
    Encoder enc;
    ASSERT_TRUE(enc.open(encCfg()).isOk());
    ASSERT_TRUE(enc.open(encCfg()).isOk());  // 内部先 close 掉上一份资源
    enc.close();
    enc.close();  // 幂等
}

// ========== 码流格式 ==========

TEST(Encoder, StreamStartsWithAnnexBSps) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg()).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    auto all = encodeFrames(enc, src, 5);
    ASSERT_FALSE(all.empty());

    // 00 00 00 01 = Annex B 起始码, 0x67 = SPS。
    // 如果这里是裸 AVCC(前 4 字节是长度)或者开头不是 SPS, ffplay 直接播不了,
    // M5 中途加入的观众也永远解不出画面
    const auto& first = all.front().data;
    ASSERT_GE(first.size(), 5u);
    EXPECT_EQ(first[0], 0x00);
    EXPECT_EQ(first[1], 0x00);
    EXPECT_EQ(first[2], 0x00);
    EXPECT_EQ(first[3], 0x01);
    EXPECT_EQ(first[4] & 0x1F, 7) << "第一个 NAL 必须是 SPS(type 7)";
}

TEST(Encoder, RepeatsSpsBeforeEveryKeyFrame) {
    constexpr int GOP    = 5;
    constexpr int FRAMES = 20;

    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg(GOP)).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    auto all = encodeFrames(enc, src, FRAMES);

    int keys = 0;
    int keysWithSps = 0;
    for (const auto& p : all) {
        if (!p.isKey) continue;
        ++keys;
        // 每个关键帧自己都要带 SPS: 观众可能从任意一个 IDR 开始收
        if (p.data.size() >= 5 && (p.data[4] & 0x1F) == 7) ++keysWithSps;
    }
    EXPECT_GT(keys, 1);
    EXPECT_EQ(keysWithSps, keys);
}

TEST(Encoder, KeyFrameIntervalMatchesGop) {
    constexpr int GOP    = 10;
    constexpr int FRAMES = 60;

    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg(GOP)).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    auto all = encodeFrames(enc, src, FRAMES);

    int keys = 0;
    for (const auto& p : all) {
        if (p.isKey) ++keys;
    }
    // 期望 FRAMES/GOP 个; 编码器有权额外插 IDR(场景切换), 所以只卡下界和一个宽松上界
    EXPECT_GE(keys, FRAMES / GOP);
    EXPECT_LE(keys, FRAMES / GOP + 2);
}

// ========== 元信息透传 ==========

TEST(Encoder, CarriesCaptureTimeAndFrameId) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg()).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    auto all = encodeFrames(enc, src, 10);
    ASSERT_FALSE(all.empty());

    // 帧号必须原样透传且保持递增 —— 端到端延迟和丢帧统计全靠它对齐
    uint64_t expectId = 0;
    for (const auto& p : all) {
        EXPECT_EQ(p.frameId, expectId) << "帧号必须与采集侧一一对应";
        EXPECT_GT(p.captureMs, 0u) << "captureMs 应从 RawFrame 透传, 不是编码完才取";
        ++expectId;
    }
}

TEST(Encoder, ProducesOnePacketPerFrameUnderZeroLatency) {
    constexpr int FRAMES = 30;

    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg()).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    auto all = encodeFrames(enc, src, FRAMES);

    // zerolatency + 无 B 帧下不该有任何缓冲: 进多少帧就出多少帧。
    // 这条一旦红了, 说明低延迟参数没生效 —— 而那是靠肉眼看画面发现不了的
    EXPECT_EQ(all.size(), static_cast<size_t>(FRAMES));
}

TEST(Encoder, LongRunStaysStable) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg()).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    // 跑够帧数, 让 pending_ 表的增删循环起来: 只进不出的话这里会一路涨
    RawFrame f;
    for (int i = 0; i < 300; ++i) {
        ASSERT_TRUE(src.readFrame(f).isOk());
        std::vector<EncodedFrame> out;
        ASSERT_TRUE(enc.encode(f, out).isOk());
    }
    std::vector<EncodedFrame> tail;
    EXPECT_TRUE(enc.flush(tail).isOk());
}


// ========== M4.4 关键帧请求 ==========

/**
 * requestKeyFrame() 之后的**下一帧**必须是关键帧, 哪怕 GOP 还远没到。
 *
 * 这是 PLI 这条链路的终点: 接收端花屏 -> PLI -> 发送端 -> 这里。
 * 链路上任何一环断了, 现象都是"画面一直花着直到下一个 GOP", 而 GOP 默认
 * 就是 fps —— 一秒。慢得能看见, 但看不出是哪一环断的。
 */
TEST(Encoder, RequestKeyFrameForcesTheVeryNextFrame) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg(/*gop=*/1000)).isOk());  // GOP 大到不会自己来 IDR
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    RawFrame f;
    std::vector<EncodedFrame> got;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(src.readFrame(f).isOk());
        got.clear();
        ASSERT_TRUE(enc.encode(f, got).isOk());
    }

    enc.requestKeyFrame();
    ASSERT_TRUE(src.readFrame(f).isOk());
    got.clear();
    ASSERT_TRUE(enc.encode(f, got).isOk());

    ASSERT_FALSE(got.empty()) << "零延迟配置下一帧进一帧出";
    EXPECT_TRUE(got.front().isKey) << "PLI 到了却还是 P 帧, 整条链路白搭";
}

/**
 * 一次请求只买一个关键帧。
 *
 * 这条钉的是 pict_type 用完必须置回 AV_PICTURE_TYPE_NONE。frame_ 是复用的成员,
 * 不置回的话**此后每一帧都是 IDR** —— 码率翻好几倍而画质没有变好,
 * 而且只在**收到过一次 PLI 之后**才发作。不看码率根本注意不到。
 */
TEST(Encoder, RequestKeyFrameIsConsumedExactlyOnce) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg(/*gop=*/1000)).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    enc.requestKeyFrame();

    int keys = 0;
    RawFrame f;
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(src.readFrame(f).isOk());
        std::vector<EncodedFrame> got;
        ASSERT_TRUE(enc.encode(f, got).isOk());
        for (const auto& p : got) {
            if (p.isKey) ++keys;
        }
    }
    EXPECT_EQ(keys, 1) << "20 帧里出了 " << keys
                       << " 个 IDR —— pict_type 没有置回 NONE, 码率会翻几倍";
}

/**
 * 没有人请求时, 关键帧只按 GOP 来 —— 上一条的对照。
 *
 * 单独一条是因为上一条如果实现成"永远只发一个 IDR"也能过。
 */
TEST(Encoder, WithoutARequestKeyFramesStillFollowTheGop) {
    Encoder enc;
    NullSource src;
    ASSERT_TRUE(enc.open(encCfg(/*gop=*/5)).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    const auto all = encodeFrames(enc, src, 20);
    int keys = 0;
    for (const auto& p : all) {
        if (p.isKey) ++keys;
    }
    EXPECT_GE(keys, 3) << "gop=5 跑 20 帧, IDR 不该只有一两个";
}

/**
 * requestKeyFrame() 在 open() 之前调不许崩, 也不许把请求丢掉。
 *
 * 它是**跨线程**调用的(发送线程收到 PLI -> 编码线程消费), 调用方无法保证
 * 编码器一定已经 open。
 */
TEST(Encoder, RequestKeyFrameBeforeOpenIsHarmlessAndStillHonored) {
    Encoder enc;
    NullSource src;
    enc.requestKeyFrame();

    ASSERT_TRUE(enc.open(encCfg(/*gop=*/1000)).isOk());
    ASSERT_TRUE(src.open(srcCfg()).isOk());

    RawFrame f;
    ASSERT_TRUE(src.readFrame(f).isOk());
    std::vector<EncodedFrame> got;
    ASSERT_TRUE(enc.encode(f, got).isOk());
    ASSERT_FALSE(got.empty());
    EXPECT_TRUE(got.front().isKey);
}
