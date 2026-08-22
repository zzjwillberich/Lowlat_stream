/**
 * @file    test_renderer.cpp
 * @brief   M3.3 渲染器: 接口契约、NullRenderer 的不变量检查、SdlRenderer 的无头全流程
 * @author  zzj
 * @date    2026-08-22
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

#include "modules/render/IRenderer.h"
#include "modules/render/NullRenderer.h"
#include "modules/render/SdlRenderer.h"

namespace {

// 造一张合法的 YUV420P 帧。RawFrame 的拷贝构造被 = delete 了, 按值返回靠移动。
RawFrame makeFrame(int w, int h, uint64_t frameId, uint64_t captureMs) {
    RawFrame f;
    f.reset(w, h);
    std::fill(f.data.begin(), f.data.end(), static_cast<uint8_t>(16));
    f.frameId = frameId;
    f.captureMs = captureMs;
    return f;
}

RendererConfig testConfig() {
    RendererConfig cfg;
    cfg.title = "renderer unit test";
    cfg.width = 64;
    cfg.height = 48;
    // 单测里绝不开 vsync: RenderPresent 会阻塞到屏幕刷新, 测试时间变得不可预测
    cfg.vsync = false;
    return cfg;
}

/**
 * @brief 把 SDL 切到无头视频驱动
 *
 * dummy 是 SDL 自带的驱动: 不需要 DISPLAY 也不弹窗, 但 CreateWindow /
 * CreateTexture / UpdateYUVTexture / RenderPresent 全都真的走一遍。
 *
 * @note **不调它就绝不能拿合法参数调 SdlRenderer::open()** —— 开发机上有 DISPLAY,
 *          ctest 会真的弹出一堆窗口, 而且在 CI 上会因为没有 DISPLAY 直接红。
 * @note 用 setenv 而不是在 CMake 里设 ENVIRONMENT: gtest_discover_tests 是每个
 *          TEST 一个进程, 进程内设是自足的; 改 add_unit_test 反而会波及别的测试。
 * @note SdlRenderer 之所以不显式要 SDL_RENDERER_ACCELERATED, 就是为了留出这条路 ——
 *          dummy 驱动下没有加速渲染器, 显式要就是 "Couldn't find matching render driver"。
 */
void forceHeadlessSdl() {
    ASSERT_EQ(setenv("SDL_VIDEODRIVER", "dummy", 1), 0);
}

}  // namespace

// ---------------------------------------------------------------- 工厂

TEST(CreateRenderer, KnownKindsAreConstructible) {
    // 只构造不 open, 所以这里一行 SDL 都不会被调到
    EXPECT_NE(createRenderer("null"), nullptr);
    EXPECT_NE(createRenderer("sdl"), nullptr);
}

TEST(CreateRenderer, UnknownKindReturnsNullInsteadOfFallingBack) {
    // 不认识就返回 nullptr 而不是退回默认: 用户明确写了 --render=sdI 却安静地
    // 跑起 Null 渲染器, 等他发现没画面已经浪费半天了
    EXPECT_EQ(createRenderer("sdI"), nullptr);  // 大写 I 不是小写 l
    EXPECT_EQ(createRenderer("SDL"), nullptr);  // 大小写敏感
    EXPECT_EQ(createRenderer(""), nullptr);
}

// ------------------------------------------------- NullRenderer 生命周期

TEST(NullRenderer, RenderBeforeOpenIsClosedNotACrash) {
    NullRenderer r;
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 1, 100)).code(), Code::Closed);
}

TEST(NullRenderer, OpenRejectsNonPositiveGeometry) {
    NullRenderer r;
    RendererConfig cfg = testConfig();
    cfg.width = 0;
    EXPECT_EQ(r.open(cfg).code(), Code::InvalidArg);

    cfg = testConfig();
    cfg.height = -1;
    EXPECT_EQ(r.open(cfg).code(), Code::InvalidArg);
}

TEST(NullRenderer, RenderAfterCloseIsClosed) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    r.close();
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 2, 133)).code(), Code::Closed);
}

TEST(NullRenderer, CloseIsIdempotent) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    r.close();
    r.close();
    // 析构还会再调一次 —— 提前 return / 抛异常 / 忘了写显式 close 都会发生,
    // 析构是唯一不会被跳过的东西, 所以幂等是硬要求不是可选项
}

TEST(NullRenderer, OpenResetsTheCounters) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    r.close();

    ASSERT_TRUE(r.open(testConfig()).isOk());
    EXPECT_EQ(r.stats().framesRendered, 0u);
    EXPECT_EQ(r.stats().texturesRebuilt, 0u);
    // 跨帧基准也要跟着清: 重新 open 之后第 1 帧不该跟上一轮的最后一帧比
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
}

TEST(NullRenderer, CloseKeepsTheCountersReadable) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 2, 133)).isOk());
    r.close();

    // close() **不清**计数器: 调用方总是先收尾再读统计做断言。
    // 在这里清零会让"跑完一帧坏的都没有"和"根本没跑"变得一样绿。
    EXPECT_EQ(r.stats().framesRendered, 2u);
}

// ------------------------------------------- NullRenderer 单帧不变量

TEST(NullRenderer, RejectsNonPositiveGeometry) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    RawFrame f = makeFrame(64, 48, 1, 100);
    f.width = 0;
    EXPECT_EQ(r.renderFrame(f).code(), Code::InvalidArg);
    EXPECT_EQ(r.stats().framesRejected, 1u);
}

TEST(NullRenderer, RejectsOddGeometry) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    // YUV420P 的色度平面是 1/2 下采样, 奇数宽高除不尽。
    // (偶数检查排在 data.size() 检查之前, 所以这里命中的确实是奇数那条。)
    RawFrame odd = makeFrame(64, 48, 1, 100);
    odd.width = 65;
    EXPECT_EQ(r.renderFrame(odd).code(), Code::InvalidArg);

    RawFrame oddH = makeFrame(64, 48, 2, 133);
    oddH.height = 49;
    EXPECT_EQ(r.renderFrame(oddH).code(), Code::InvalidArg);
    EXPECT_EQ(r.stats().framesRejected, 2u);
}

TEST(NullRenderer, RejectsEmptyData) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    RawFrame f = makeFrame(64, 48, 1, 100);
    f.data.clear();
    EXPECT_EQ(r.renderFrame(f).code(), Code::InvalidArg);
}

TEST(NullRenderer, RejectsDataThatIsOffByOneByte) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    // 这条才是真正的检查。把实现换成 `if (data.empty())` 上面那个用例照样全绿,
    // 而这个会红 —— 一个 64x48 的帧带着 4607 字节同样是坏帧。
    RawFrame shortByOne = makeFrame(64, 48, 1, 100);
    shortByOne.data.pop_back();
    EXPECT_EQ(r.renderFrame(shortByOne).code(), Code::InvalidArg);

    RawFrame longByOne = makeFrame(64, 48, 2, 133);
    longByOne.data.push_back(0);
    EXPECT_EQ(r.renderFrame(longByOne).code(), Code::InvalidArg);
    EXPECT_EQ(r.stats().framesRejected, 2u);
}

TEST(NullRenderer, RejectsAnUnknownPixelFormat) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    // PixelFormat 目前只有 YUV420P 一个取值, 但 enum class 的底层类型是固定的
    // (默认 int), 所以这样转是良定义的, 不是 UB —— 这条检查因此测得到。
    // 它守的是"将来加了 BGRA 这类打包格式, 而渲染器还按三平面去解读"。
    RawFrame f = makeFrame(64, 48, 1, 100);
    f.fmt = static_cast<PixelFormat>(99);
    EXPECT_EQ(r.renderFrame(f).code(), Code::InvalidArg);
}

// ------------------------------------------- NullRenderer 跨帧不变量

TEST(NullRenderer, RejectsCaptureTimeGoingBackwards) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 200)).isOk());
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 2, 100)).code(), Code::InvalidArg);
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 3, 200)).code(), Code::InvalidArg);
}

TEST(NullRenderer, RejectsAFrameIdThatDoesNotAdvance) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 7, 100)).isOk());

    // JitterBuffer 保证按序且不重复放行, 这里出现重复/倒退就说明上游坏了
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 7, 133)).code(), Code::InvalidArg);
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 6, 166)).code(), Code::InvalidArg);
    EXPECT_EQ(r.stats().framesRejected, 2u);
}

TEST(NullRenderer, AcceptsAGapInFrameIds) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    // 丢帧就会跳号。**递增**是不变量, **连续**不是 —— 要求 +1 会把正常的丢包
    // 变成一次误报
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 9, 400)).isOk());
    EXPECT_EQ(r.stats().framesRejected, 0u);
}

TEST(NullRenderer, AcceptsTheMissingMetadataSentinel) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 5, 100)).isOk());

    // Decoder::drainFrames() 查不到 pending_ 元信息时, captureMs 和 frameId 会
    // **一起**留在 0 出去(DecoderStats::framesMissingMeta 就是数这个的)。
    // 这不是坏帧, 是元信息丢了的好帧 —— 画面照样得显示
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 0, 0)).isOk());

    // 但哨兵帧不能把基准冲掉: 紧跟它的帧仍要按上一个**真实**帧号来检查
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 3, 200)).code(), Code::InvalidArg);
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 6, 200)).isOk());
    EXPECT_EQ(r.stats().framesRejected, 1u);
    EXPECT_EQ(r.stats().framesRendered, 3u);
}

TEST(NullRenderer, AcceptsAResolutionChangeAndCountsARebuild) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    EXPECT_EQ(r.stats().texturesRebuilt, 1u);

    // H.264 码流自带 SPS, 对端重开编码器时分辨率会变。
    // "宽高跨帧一致"是个诱人的**错误**不变量: 写成断言, 对端重启一次就误报一次,
    // 而那恰恰是渲染器该正确处理的场景(重建纹理)
    EXPECT_TRUE(r.renderFrame(makeFrame(32, 24, 2, 133)).isOk());
    EXPECT_EQ(r.stats().texturesRebuilt, 2u);
    EXPECT_EQ(r.stats().framesRejected, 0u);
}

// ------------------------------------------- NullRenderer 计数器语义

TEST(NullRenderer, RejectedFramesDoNotCountAsRendered) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());

    RawFrame bad = makeFrame(64, 48, 1, 100);
    bad.data.pop_back();
    ASSERT_EQ(r.renderFrame(bad).code(), Code::InvalidArg);

    EXPECT_EQ(r.stats().framesRejected, 1u);
    EXPECT_EQ(r.stats().framesRendered, 0u);
    // 坏帧也不该触发一次"重建": 它压根没被显示过
    EXPECT_EQ(r.stats().texturesRebuilt, 0u);
}

TEST(NullRenderer, ARejectedFrameDoesNotPoisonTheCrossFrameState) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());

    RawFrame bad = makeFrame(64, 48, 2, 133);
    bad.data.pop_back();
    ASSERT_EQ(r.renderFrame(bad).code(), Code::InvalidArg);

    // 被拒的帧不更新基准, 所以同一个帧号重新来一遍仍然合法 ——
    // 否则下一帧会被拿去跟一个**从没显示过**的帧比较
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 2, 133)).isOk());
    EXPECT_EQ(r.stats().framesRendered, 2u);
    EXPECT_EQ(r.stats().framesRejected, 1u);
}

TEST(NullRenderer, RebuildsTheTextureOnlyOnceForAConstantResolution) {
    NullRenderer r;
    ASSERT_TRUE(r.open(testConfig()).isOk());
    for (uint64_t i = 1; i <= 30; ++i) {
        ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, i, 100 + i * 33)).isOk()) << "第 " << i << " 帧";
    }

    // 稳态下只在首帧建一次。这个数持续增长 = 纹理在每帧重建 = 每帧一次显存分配,
    // "连续跑 5 分钟内存不涨"那条验收就是靠它定位的
    EXPECT_EQ(r.stats().texturesRebuilt, 1u);
    EXPECT_EQ(r.stats().framesRendered, 30u);
    EXPECT_EQ(r.stats().framesRejected, 0u);
}

TEST(NullRenderer, PumpEventsNeverAsksToQuit) {
    NullRenderer r;
    EXPECT_FALSE(r.pumpEvents());
    ASSERT_TRUE(r.open(testConfig()).isOk());
    EXPECT_FALSE(r.pumpEvents());
    // 恒 false 不是空实现: 它**真的**没有事件源。没有窗口就没有 SDL_QUIT,
    // 退出只能由 Ctrl-C 或发送端静默超时来触发
}

// --------------------------------- SdlRenderer: 走不到 SDL 的那部分
// 参数校验和空指针检查都发生在 SDL_Init 之前, 所以这一组不需要显示器, 也不弹窗。

TEST(SdlRenderer, OpenRejectsNonPositiveGeometryBeforeTouchingSdl) {
    SdlRenderer r;
    RendererConfig cfg = testConfig();
    cfg.width = 0;
    EXPECT_EQ(r.open(cfg).code(), Code::InvalidArg);

    cfg = testConfig();
    cfg.height = -1;
    EXPECT_EQ(r.open(cfg).code(), Code::InvalidArg);
}

TEST(SdlRenderer, RenderBeforeOpenIsClosedNotACrash) {
    SdlRenderer r;
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 1, 100)).code(), Code::Closed);
}

TEST(SdlRenderer, CloseOnANeverOpenedRendererIsSafe) {
    SdlRenderer r;
    r.close();
    r.close();
    // 三个句柄都是 nullptr, 而 SDL_Quit() 被 SDL_WasInit(0) 挡住了 ——
    // 不会去关一个还没开的 SDL。析构还会再调一次
    EXPECT_EQ(r.stats().framesRendered, 0u);
}

// ------------------------------------ SdlRenderer: 无头跑完整条路径
// 这一组才真正执行 CreateWindow / CreateTexture / UpdateYUVTexture /
// RenderPresent / Destroy*。没有它们, 这个类里真正有逻辑的部分一行都测不到。

TEST(SdlRendererHeadless, OpensRendersAndClosesWithoutADisplay) {
    forceHeadlessSdl();
    SdlRenderer r;
    const Status st = r.open(testConfig());
    ASSERT_TRUE(st.isOk()) << st.message();

    for (uint64_t i = 1; i <= 10; ++i) {
        const Status rendered = r.renderFrame(makeFrame(64, 48, i, 100 + i * 33));
        ASSERT_TRUE(rendered.isOk()) << "第 " << i << " 帧: " << rendered.message();
    }

    EXPECT_EQ(r.stats().framesRendered, 10u);
    EXPECT_EQ(r.stats().framesRejected, 0u);
    EXPECT_EQ(r.stats().texturesRebuilt, 1u);
    r.close();
}

TEST(SdlRendererHeadless, RebuildsTheTextureOnlyWhenTheResolutionChanges) {
    forceHeadlessSdl();
    SdlRenderer r;
    const Status st = r.open(testConfig());
    ASSERT_TRUE(st.isOk()) << st.message();

    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 2, 133)).isOk());
    EXPECT_EQ(r.stats().texturesRebuilt, 1u) << "尺寸没变却重建了纹理";

    // 尺寸变了必须重建, 而且只重建这一次
    ASSERT_TRUE(r.renderFrame(makeFrame(32, 24, 3, 166)).isOk());
    ASSERT_TRUE(r.renderFrame(makeFrame(32, 24, 4, 200)).isOk());
    EXPECT_EQ(r.stats().texturesRebuilt, 2u);
    EXPECT_EQ(r.stats().framesRendered, 4u);
}

TEST(SdlRendererHeadless, KeepsGoingAfterAMalformedFrame) {
    forceHeadlessSdl();
    SdlRenderer r;
    const Status st = r.open(testConfig());
    ASSERT_TRUE(st.isOk()) << st.message();

    // 数据比几何声明的短。这里的检查不是内容校验(那是 NullRenderer 的活),
    // 是防止 SDL_UpdateYUVTexture 照着 width/height 越界读
    RawFrame bad = makeFrame(64, 48, 1, 100);
    bad.data.resize(10);
    EXPECT_EQ(r.renderFrame(bad).code(), Code::InvalidArg);
    EXPECT_EQ(r.stats().framesRejected, 1u);

    // InvalidArg 的含义是"这帧画不了, 但下一帧可能是好的" —— 所以还得能继续
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 2, 133)).isOk());
    EXPECT_EQ(r.stats().framesRendered, 1u);
}

TEST(SdlRendererHeadless, CloseIsIdempotentAndRenderAfterCloseIsClosed) {
    forceHeadlessSdl();
    SdlRenderer r;
    const Status st = r.open(testConfig());
    ASSERT_TRUE(st.isOk()) << st.message();
    ASSERT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());

    r.close();
    r.close();  // Texture -> Renderer -> Window -> SDL_Quit, 第二遍全是空指针
    EXPECT_EQ(r.renderFrame(makeFrame(64, 48, 2, 133)).code(), Code::Closed);

    // close() 不清计数器, 收尾之后仍读得到
    EXPECT_EQ(r.stats().framesRendered, 1u);

    // 关完还能再开 —— SDL_Init/SDL_Quit 是可以配对重来的
    const Status again = r.open(testConfig());
    ASSERT_TRUE(again.isOk()) << again.message();
    EXPECT_TRUE(r.renderFrame(makeFrame(64, 48, 1, 100)).isOk());
}
