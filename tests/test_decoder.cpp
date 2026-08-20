/**
 * @file    test_decoder.cpp
 * @brief   Decoder 的契约测试: 帧数守恒、元信息透传、平面不写反、坏码流不崩
 * @author  zzj
 * @date    2026-08-18
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "modules/capture/NullSource.h"
#include "modules/decode/Decoder.h"
#include "modules/encode/Encoder.h"

namespace {
    constexpr int W = 320;
    constexpr int H = 240;

    EncoderConfig encCfg(int gop = 30) {
        EncoderConfig c;
        c.width = W;
        c.height = H;
        c.fps = 30;
        c.gop = gop;
        return c;
    }

    SourceConfig srcCfg() {
        SourceConfig c;
        c.width = W;
        c.height = H;
        c.fps = 1000;  // 单测不真等帧率
        return c;
    }

    /** 编码 n 帧, 顺带把编码前的原始帧留下来做对照 */
    struct Clip {
        std::vector<EncodedFrame> coded;
        std::vector<RawFrame> source;
    };

    Clip makeClip(int n, int gop = 30) {
        Clip clip;
        NullSource src;
        Encoder enc;
        EXPECT_TRUE(src.open(srcCfg()).isOk());
        EXPECT_TRUE(enc.open(encCfg(gop)).isOk());

        for (int i = 0; i < n; ++i) {
            RawFrame raw;
            EXPECT_TRUE(src.readFrame(raw).isOk());

            std::vector<EncodedFrame> got;
            EXPECT_TRUE(enc.encode(raw, got).isOk());
            for (auto& f : got) clip.coded.push_back(std::move(f));
            clip.source.push_back(std::move(raw));
        }
        std::vector<EncodedFrame> tail;
        EXPECT_TRUE(enc.flush(tail).isOk());
        for (auto& f : tail) clip.coded.push_back(std::move(f));
        return clip;
    }

    CodedFrameView view(const EncodedFrame& f) {
        CodedFrameView v;
        v.data = f.data.data();
        v.len = f.data.size();
        v.captureMs = f.captureMs;
        v.frameId = f.frameId;
        return v;
    }

    /** 解码整段, 返回所有图像帧 */
    std::vector<RawFrame> decodeClip(Decoder& dec, const Clip& clip) {
        std::vector<RawFrame> out;
        for (const auto& f : clip.coded) {
            EXPECT_TRUE(dec.decode(view(f), out).isOk());
        }
        EXPECT_TRUE(dec.flush(out).isOk());
        return out;
    }

    /** 某个平面上一块矩形区域的均值 */
    double meanOf(const uint8_t* plane, size_t stride, int x0, int y0, int w, int h) {
        double sum = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                sum += plane[(static_cast<size_t>(y0 + y)) * stride + (x0 + x)];
            }
        }
        return sum / (static_cast<double>(w) * h);
    }
}  // namespace

// ---------- 参数校验与生命周期 ----------

TEST(Decoder, RejectsNonPositiveThreadCount) {
    Decoder dec;
    DecoderConfig cfg;
    cfg.threads = 0;
    EXPECT_EQ(dec.open(cfg).code(), Code::InvalidArg);
}

TEST(Decoder, DecodeBeforeOpenIsClosedNotACrash) {
    Decoder dec;
    const std::vector<uint8_t> junk(64, 0);
    CodedFrameView v;
    v.data = junk.data();
    v.len = junk.size();

    std::vector<RawFrame> out;
    EXPECT_EQ(dec.decode(v, out).code(), Code::Closed);
}

TEST(Decoder, RejectsEmptyInput) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    std::vector<RawFrame> out;
    CodedFrameView empty;
    // 本端传错了, 不是网络的问题 —— 判据同 FrameAssembler::offer(nullptr)
    EXPECT_EQ(dec.decode(empty, out).code(), Code::InvalidArg);
}

TEST(Decoder, CloseIsIdempotent) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());
    dec.close();
    dec.close();  // 第二次不能 double free

    std::vector<RawFrame> out;
    EXPECT_EQ(dec.flush(out).code(), Code::Closed);
}

TEST(Decoder, ReopenDoesNotCarryStateOver) {
    Decoder dec;
    const Clip clip = makeClip(5);

    ASSERT_TRUE(dec.open({}).isOk());
    std::vector<RawFrame> first;
    for (const auto& f : clip.coded) ASSERT_TRUE(dec.decode(view(f), first).isOk());

    ASSERT_TRUE(dec.open({}).isOk());  // 重开: 内部要先 close 掉上一份资源
    std::vector<RawFrame> second;
    for (const auto& f : clip.coded) ASSERT_TRUE(dec.decode(view(f), second).isOk());
    EXPECT_TRUE(dec.flush(second).isOk());

    EXPECT_FALSE(second.empty());
}

TEST(Decoder, DecodingAfterFlushIsRejected) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());
    const Clip clip = makeClip(3);

    std::vector<RawFrame> out;
    ASSERT_TRUE(dec.decode(view(clip.coded[0]), out).isOk());
    ASSERT_TRUE(dec.flush(out).isOk());

    // 排空之后再喂是调用方的错, 应当报出来而不是悄悄接受(同 Encoder)
    EXPECT_EQ(dec.decode(view(clip.coded[1]), out).code(), Code::Closed);
}

// ---------- 帧数与尺寸 ----------

TEST(Decoder, DecodesEveryFrameTheEncoderProduced) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(30);
    const std::vector<RawFrame> got = decodeClip(dec, clip);

    // 解码器的核心契约: 进多少帧出多少帧。少了是漏了 receive 循环,
    // 多了是 flush 把已经吐过的又吐了一遍
    EXPECT_EQ(got.size(), clip.coded.size());
    EXPECT_EQ(dec.stats().framesIn, clip.coded.size());
    EXPECT_EQ(dec.stats().framesOut, got.size());
}

TEST(Decoder, OutputSizeMatchesTheStream) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(5);
    const std::vector<RawFrame> got = decodeClip(dec, clip);
    ASSERT_FALSE(got.empty());

    for (const RawFrame& f : got) {
        EXPECT_EQ(f.width, W);
        EXPECT_EQ(f.height, H);
        EXPECT_EQ(f.fmt, PixelFormat::YUV420P);
        // 紧凑排布。按 linesize 整块拷的实现在这里就露馅 —— 尺寸会大出一截
        EXPECT_EQ(f.data.size(), static_cast<size_t>(W) * H * 3 / 2);
    }
}

// ---------- 元信息透传 ----------

TEST(Decoder, CarriesCaptureTimeAndFrameIdThrough) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(10);
    const std::vector<RawFrame> got = decodeClip(dec, clip);
    ASSERT_EQ(got.size(), clip.coded.size());

    // 端到端延迟全靠这一路透传不断链。这里错了, M3.4 量出来的延迟就是个假数字,
    // 而且它"看着挺合理", 不会有任何报错
    for (size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i].captureMs, clip.coded[i].captureMs) << "第 " << i << " 帧";
        EXPECT_EQ(got[i].frameId, clip.coded[i].frameId) << "第 " << i << " 帧";
    }
}

TEST(Decoder, DoesNotStampItsOwnTime) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(3);

    // 塞一个绝不可能是"当前时刻"的值; 解码器重新取时间的话它会被冲掉
    const uint64_t sentinel = 123456789;
    CodedFrameView v = view(clip.coded[0]);
    v.captureMs = sentinel;
    v.frameId = 42;

    std::vector<RawFrame> out;
    ASSERT_TRUE(dec.decode(v, out).isOk());
    ASSERT_TRUE(dec.flush(out).isOk());

    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0].captureMs, sentinel);
    EXPECT_EQ(out[0].frameId, 42u);
}

// ---------- 画面内容 ----------

TEST(Decoder, DecodedLumaResemblesTheSource) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(10);
    const std::vector<RawFrame> got = decodeClip(dec, clip);
    ASSERT_EQ(got.size(), clip.source.size());

    // 有损压缩, 不能逐字节比。比均值: 它对压缩很稳, 却能抓住"整片黑"、
    // "平面错位"、"尺寸算错"这类真正的翻车
    for (size_t i = 0; i < got.size(); ++i) {
        const double a = meanOf(clip.source[i].data.data(), W, 0, 0, W, H);
        const double b = meanOf(got[i].data.data(), W, 0, 0, W, H);
        EXPECT_NEAR(a, b, 8.0) << "第 " << i << " 帧的亮度均值偏差过大";
    }
}

TEST(Decoder, DecodedFrameIsNotFlat) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(3);
    const std::vector<RawFrame> got = decodeClip(dec, clip);
    ASSERT_FALSE(got.empty());

    // 不能比较左右半幅的均值: 条纹周期是 219, 320 宽画面各取 160 像素时会恰好
    // 接近抵消。取两个不跨周期的局部区域，纯灰画面必然失败，正常渐变则有明显差异。
    const double near = meanOf(got[0].data.data(), W, 16, 96, 32, 32);
    const double far = meanOf(got[0].data.data(), W, 80, 96, 32, 32);
    EXPECT_GT(std::abs(near - far), 3.0) << "解出来的画面是平的, 多半根本没解出内容";
}

TEST(Decoder, ChromaPlanesAreNotSwapped) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(3);
    const std::vector<RawFrame> got = decodeClip(dec, clip);
    ASSERT_FALSE(got.empty());

    const int cw = W / 2;
    const int ch = H / 2;
    const uint8_t* u = got[0].data.data() + static_cast<size_t>(W) * H;
    const uint8_t* v = u + static_cast<size_t>(cw) * ch;

    // NullSource 有意让 U 按**列**渐变、V 按**行**渐变, 就是为了让色度平面写反能被发现。
    // U/V 搞反是这一层的经典 bug(SDL 那边 IYUV/YV12 同理), 现象是画面形状正常、
    // 颜色诡异, 很容易被误判成"解码错了"
    const double uLeft = meanOf(u, cw, 0, 0, cw / 2, ch);
    const double uRight = meanOf(u, cw, cw / 2, 0, cw / 2, ch);
    EXPECT_GT(std::abs(uLeft - uRight), 10.0) << "U 平面横向没有渐变, 多半和 V 写反了";

    const double vTop = meanOf(v, cw, 0, 0, cw, ch / 2);
    const double vBottom = meanOf(v, cw, 0, ch / 2, cw, ch / 2);
    EXPECT_GT(std::abs(vTop - vBottom), 10.0) << "V 平面纵向没有渐变, 多半和 U 写反了";
}

// ---------- 坏码流 ----------

TEST(Decoder, SurvivesATruncatedFrame) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(5);
    ASSERT_GT(clip.coded[0].data.size(), 32u);

    // 砍掉后半截喂进去。丢包场景下这是常态, 解码器必须扛住
    CodedFrameView v = view(clip.coded[0]);
    v.len = clip.coded[0].data.size() / 2;

    std::vector<RawFrame> out;
    const Status st = dec.decode(v, out);
    EXPECT_TRUE(st.isOk() || st.code() == Code::NetError);

    // 而且不能因为一帧坏了就再也解不出东西
    for (size_t i = 1; i < clip.coded.size(); ++i) {
        EXPECT_TRUE(dec.decode(view(clip.coded[i]), out).isOk());
    }
}

TEST(Decoder, SurvivesGarbageInput) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    std::vector<uint8_t> junk(512);
    std::iota(junk.begin(), junk.end(), static_cast<uint8_t>(0));
    CodedFrameView v;
    v.data = junk.data();
    v.len = junk.size();

    std::vector<RawFrame> out;
    // 垃圾数据不是本端的错(端口上什么都可能来), 所以要么 Ok 要么 NetError,
    // 不该是 Internal, 更不该崩
    const Status st = dec.decode(v, out);
    EXPECT_TRUE(st.isOk() || st.code() == Code::NetError);
}

TEST(Decoder, StartingFromANonKeyFrameDoesNotCrash) {
    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(10);
    ASSERT_GT(clip.coded.size(), 3u);

    // 跳过 IDR 从第 4 帧开始喂 —— 起播门没做好时接收端就是这个样子。
    // 解出 0 帧完全可以接受, 崩或者报 Internal 不行
    std::vector<RawFrame> out;
    for (size_t i = 3; i < clip.coded.size(); ++i) {
        const Status st = dec.decode(view(clip.coded[i]), out);
        EXPECT_TRUE(st.isOk() || st.code() == Code::NetError);
    }
    EXPECT_TRUE(dec.flush(out).isOk());
}

// ---------- 日志桥 ----------

TEST(Decoder, LogBridgeCanBeInstalledMoreThanOnce) {
    // 多个管线各装一次是常态, 不能因此打架
    Decoder::installFfmpegLogBridge();
    Decoder::installFfmpegLogBridge();

    Decoder dec;
    ASSERT_TRUE(dec.open({}).isOk());

    const Clip clip = makeClip(3);
    std::vector<RawFrame> out;
    for (const auto& f : clip.coded) EXPECT_TRUE(dec.decode(view(f), out).isOk());
}
