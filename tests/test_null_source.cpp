/**
 * @file    test_null_source.cpp
 * @brief   NullSource 与 createSource 单元测试 — 覆盖 M1.2 要求的用例
 * @author  zzj
 * @date    2026-07-28
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

#include "modules/capture/ISource.h"
#include "modules/capture/NullSource.h"

using namespace std::chrono_literals;

namespace {
    /**
     * 单测里统一用高帧率, 免得每读一帧都真等 1/30 秒。
     * 节流本身的正确性由 RespectsFrameRate 单独测。
     */
    constexpr int FAST_FPS = 1000;

    SourceConfig cfg(int w = 64, int h = 32, int fps = FAST_FPS) {
        SourceConfig c;
        c.width  = w;
        c.height = h;
        c.fps    = fps;
        return c;
    }
}

// ========== 工厂 ==========

TEST(CreateSource, KnownKindReturnsInstance) {
    auto src = createSource("null");
    ASSERT_NE(src, nullptr);
}

TEST(CreateSource, UnknownKindReturnsNull) {
    // M1.1 验收: 配置写错必须在启动时炸掉, 而不是安静退回默认源 ——
    // 用户明确写了 --source=v4l3 却跑起 Null 源, 等他发现画面不对已经浪费半天
    EXPECT_EQ(createSource("bogus"), nullptr);
    EXPECT_EQ(createSource(""), nullptr);
}

// ========== 参数校验 ==========

TEST(NullSource, RejectsOddSize) {
    NullSource src;
    EXPECT_EQ(src.open(cfg(65, 32)).code(), Code::InvalidArg);
    EXPECT_EQ(src.open(cfg(64, 33)).code(), Code::InvalidArg);
    EXPECT_EQ(src.open(cfg(0, 32)).code(), Code::InvalidArg);
}

TEST(NullSource, RejectsNonPositiveFps) {
    NullSource src;
    EXPECT_EQ(src.open(cfg(64, 32, 0)).code(), Code::InvalidArg);
    EXPECT_EQ(src.open(cfg(64, 32, -1)).code(), Code::InvalidArg);
}

// ========== 生命周期 ==========

TEST(NullSource, ReadBeforeOpenReturnsClosed) {
    NullSource src;
    RawFrame f;
    EXPECT_EQ(src.readFrame(f).code(), Code::Closed);
}

TEST(NullSource, ReadAfterCloseReturnsClosed) {
    NullSource src;
    ASSERT_TRUE(src.open(cfg()).isOk());

    RawFrame f;
    ASSERT_TRUE(src.readFrame(f).isOk());

    src.close();
    src.close();  // 幂等
    EXPECT_EQ(src.readFrame(f).code(), Code::Closed);
}

TEST(NullSource, ReopenRestartsFrameId) {
    NullSource src;
    RawFrame f;

    ASSERT_TRUE(src.open(cfg()).isOk());
    ASSERT_TRUE(src.readFrame(f).isOk());
    ASSERT_TRUE(src.readFrame(f).isOk());
    EXPECT_EQ(f.frameId, 1u);
    src.close();

    ASSERT_TRUE(src.open(cfg()).isOk());
    ASSERT_TRUE(src.readFrame(f).isOk());
    EXPECT_EQ(f.frameId, 0u);
}

// ========== 产出的帧 ==========

TEST(NullSource, FrameHasRequestedGeometry) {
    NullSource src;
    ASSERT_TRUE(src.open(cfg(64, 32)).isOk());

    RawFrame f;
    ASSERT_TRUE(src.readFrame(f).isOk());
    EXPECT_EQ(f.width, 64);
    EXPECT_EQ(f.height, 32);
    EXPECT_EQ(f.fmt, PixelFormat::YUV420P);
    EXPECT_EQ(f.data.size(), frameBytes(64, 32));
}

TEST(NullSource, FrameIdIncrementsAndTimeAdvances) {
    NullSource src;
    ASSERT_TRUE(src.open(cfg()).isOk());

    RawFrame f;
    uint64_t prevMs = 0;
    for (uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(src.readFrame(f).isOk());
        EXPECT_EQ(f.frameId, i);
        EXPECT_GT(f.captureMs, 0u);
        EXPECT_GE(f.captureMs, prevMs);  // 单调不减; 同一毫秒内连读多帧会相等
        prevMs = f.captureMs;
    }
}

TEST(NullSource, ContentChangesBetweenFrames) {
    NullSource src;
    ASSERT_TRUE(src.open(cfg()).isOk());

    RawFrame a, b;
    ASSERT_TRUE(src.readFrame(a).isOk());
    ASSERT_TRUE(src.readFrame(b).isOk());

    // 画面必须真的在动: 静止画面下"卡住了"和"跑得好好的"看起来一模一样
    EXPECT_NE(a.data, b.data);
}

TEST(NullSource, ScrollStaysUniformAcrossPhaseWrap) {
    constexpr int W = 64, H = 32;
    constexpr int SHIFT = 3;   // drawBackground 里每帧的平移量

    NullSource src;
    ASSERT_TRUE(src.open(cfg(W, H)).isOk());

    RawFrame prev, cur;
    ASSERT_TRUE(src.readFrame(prev).isOk());

    // 帧数要盖过 phase 的回绕点。模数取得和条纹周期不相干时(曾经是 256), 回绕那一帧
    // 会猛跳一次 —— 看起来和丢帧一模一样, 而这条纹恰恰是用来判断丢帧的
    for (int n = 1; n < 120; ++n) {
        ASSERT_TRUE(src.readFrame(cur).isOk());

        // 底行不会被左上角的帧号盖住, 是干净的背景
        const uint8_t* p = prev.y() + static_cast<size_t>(H - 1) * prev.yStride();
        const uint8_t* c = cur.y()  + static_cast<size_t>(H - 1) * cur.yStride();
        for (int x = 0; x + SHIFT < W; ++x) {
            ASSERT_EQ(c[x], p[x + SHIFT]) << "frame " << n << ", x " << x;
        }
        std::swap(prev, cur);
    }
}

TEST(NullSource, FrameNumberIsDrawnOnThePicture) {
    NullSource src;
    ASSERT_TRUE(src.open(cfg(640, 480)).isOk());

    RawFrame f;
    ASSERT_TRUE(src.readFrame(f).isOk());

    // 左上角的帧号区域里必须同时有底色(暗)和笔画(亮), 证明点阵真的画上去了,
    // 而不是只有一块底 —— 后面所有靠肉眼对帧号的验证都依赖这个
    bool hasInk = false;
    bool hasBackdrop = false;
    for (int r = 0; r < 60; ++r) {
        const uint8_t* row = f.y() + static_cast<size_t>(r) * f.yStride();
        for (int c = 0; c < 60; ++c) {
            if (row[c] >= 235) hasInk = true;
            if (row[c] <= 16) hasBackdrop = true;
        }
    }
    EXPECT_TRUE(hasInk);
    EXPECT_TRUE(hasBackdrop);
}

// ========== 帧率节流 ==========

TEST(NullSource, RespectsFrameRate) {
    constexpr int FPS = 50;   // 20ms 一帧
    constexpr int N   = 5;

    NullSource src;
    ASSERT_TRUE(src.open(cfg(64, 32, FPS)).isOk());

    RawFrame f;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(src.readFrame(f).isOk());
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    // 第 0 帧不等待, 所以 N 帧之间只有 N-1 个间隔。
    // **只卡下界**: 机器负载高时睡过头很正常, 那不是 bug; 而"没睡够"一定是 bug。
    const auto expected = std::chrono::milliseconds((N - 1) * 1000 / FPS);
    EXPECT_GE(elapsed, expected);
}
