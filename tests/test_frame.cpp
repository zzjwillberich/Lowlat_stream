/**
 * @file    test_frame.cpp
 * @brief   RawFrame / ISource 单元测试 — 覆盖 M1.1 要求的全部用例
 * @author  zzj
 * @date    2026-07-27
 */
#include <gtest/gtest.h>

#include <memory>
#include <type_traits>

#include "common/BoundedQueue.h"
#include "modules/capture/Frame.h"
#include "modules/capture/ISource.h"

// ========== 类型契约 (编译期就该拦下, 跑不到运行期) ==========

// M1.1 验收: RawFrame f2 = f1; 必须编译失败
static_assert(!std::is_copy_constructible<RawFrame>::value,
              "RawFrame 不许拷贝构造: 一帧 1080p 3MB, 隐式拷贝是无声的性能陷阱");
static_assert(!std::is_copy_assignable<RawFrame>::value,
              "RawFrame 不许拷贝赋值");

// M1.1 验收: RawFrame f2 = std::move(f1); 必须通过 —— 队列靠 move 交接所有权
static_assert(std::is_move_constructible<RawFrame>::value, "RawFrame 必须可 move 构造");
static_assert(std::is_move_assignable<RawFrame>::value, "RawFrame 必须可 move 赋值");

// ISource 是纯抽象基类: 误写 ISource s; 应当编译失败
static_assert(std::is_abstract<ISource>::value, "ISource 必须是抽象类");

// 上层持有的是 unique_ptr<ISource>, 析构不虚就会漏掉派生类的设备资源
static_assert(std::has_virtual_destructor<ISource>::value, "ISource 必须有虚析构");

// ========== 尺寸计算 ==========

TEST(Frame, FrameBytesIsThreeHalvesOfPixels) {
    // 640*480 = 307200 亮度 + 2*76800 色度
    EXPECT_EQ(frameBytes(640, 480), 460800u);
    EXPECT_EQ(frameBytes(1920, 1080), 3110400u);
}

TEST(Frame, ResetSizesBuffer) {
    RawFrame f;
    f.reset(640, 480);

    EXPECT_EQ(f.width, 640);
    EXPECT_EQ(f.height, 480);
    EXPECT_EQ(f.fmt, PixelFormat::YUV420P);
    EXPECT_EQ(f.data.size(), frameBytes(640, 480));
    EXPECT_EQ(f.ySize(), 307200u);
    EXPECT_EQ(f.uvSize(), 76800u);
    EXPECT_EQ(f.yStride(), 640);
    EXPECT_EQ(f.uvStride(), 320);
}

TEST(Frame, PlanesAreContiguousAndFillTheBuffer) {
    RawFrame f;
    f.reset(64, 32);

    EXPECT_EQ(f.y(), f.data.data());
    EXPECT_EQ(f.u(), f.y() + f.ySize());
    EXPECT_EQ(f.v(), f.u() + f.uvSize());
    // 三个平面必须刚好铺满整个缓冲区, 多一字节少一字节喂给编码器都是错位画面
    EXPECT_EQ(f.v() + f.uvSize(), f.data.data() + f.data.size());
}

TEST(Frame, ResetAtSameSizeDoesNotReallocate) {
    RawFrame f;
    f.reset(640, 480);
    const uint8_t* first = f.data.data();

    f.reset(640, 480);

    // 采集线程每帧都会 reset 同一个 RawFrame, 这里一旦重新分配就是每秒 30 次 malloc
    EXPECT_EQ(f.data.data(), first);
}

// ========== 移动语义 ==========

TEST(Frame, MoveTransfersBuffer) {
    RawFrame src;
    src.reset(64, 32);
    src.frameId   = 7;
    src.captureMs = 12345;
    src.y()[0]    = 0xAB;

    RawFrame dst = std::move(src);

    EXPECT_EQ(dst.width, 64);
    EXPECT_EQ(dst.height, 32);
    EXPECT_EQ(dst.frameId, 7u);
    EXPECT_EQ(dst.captureMs, 12345u);
    EXPECT_EQ(dst.data.size(), frameBytes(64, 32));
    EXPECT_EQ(dst.y()[0], 0xAB);

    // 陷阱: 默认 move 只搬得动 vector, width/height/frameId 这些标量是照抄过去的,
    // 被 move 走的帧仍然自称 64x32 却已经没有数据了。所以 move 之后不要再读它,
    // 要读就先 reset() —— 流水线里靠 unique_ptr 交接所有权正是为了绕开这件事。
    EXPECT_TRUE(src.data.empty());
}

// ========== 与队列联通 (M1.4 管线的地基) ==========

TEST(Frame, MoveOnlyFrameGoesThroughBoundedQueue) {
    BoundedQueue<std::unique_ptr<RawFrame>> q(4);

    auto f = std::make_unique<RawFrame>();
    f->reset(64, 32);
    f->frameId = 42;

    ASSERT_TRUE(q.push(std::move(f)));
    EXPECT_EQ(f, nullptr);  // push 按值接收, 所有权已经交出去了

    std::unique_ptr<RawFrame> got;
    ASSERT_TRUE(q.pop(got));
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->frameId, 42u);
    EXPECT_EQ(got->data.size(), frameBytes(64, 32));
}
