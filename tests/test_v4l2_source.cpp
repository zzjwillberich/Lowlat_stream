/**
 * @file    test_v4l2_source.cpp
 * @brief   V4l2Source 无硬件契约测试与可选真机冒烟测试
 * @author  zzj
 * @date    2026-08-07
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <unistd.h>

#include "modules/capture/ISource.h"
#include "modules/capture/V4l2Source.h"

namespace {
    SourceConfig validConfig() {
        SourceConfig cfg;
        cfg.width = 640;
        cfg.height = 480;
        cfg.fps = 30;
        cfg.device = "/dev/video0";
        return cfg;
    }
}  // namespace

TEST(CreateSource, V4l2KindReturnsInstanceWithoutOpeningDevice) {
    auto source = createSource("v4l2");
    ASSERT_NE(source, nullptr);
    EXPECT_NE(dynamic_cast<V4l2Source*>(source.get()), nullptr);
}

TEST(V4l2Source, RejectsInvalidConfigBeforeTouchingDevice) {
    V4l2Source source;

    SourceConfig cfg = validConfig();
    cfg.width = 641;
    EXPECT_EQ(source.open(cfg).code(), Code::InvalidArg);

    cfg = validConfig();
    cfg.height = 0;
    EXPECT_EQ(source.open(cfg).code(), Code::InvalidArg);

    cfg = validConfig();
    cfg.fps = 0;
    EXPECT_EQ(source.open(cfg).code(), Code::InvalidArg);

    cfg = validConfig();
    cfg.device.clear();
    EXPECT_EQ(source.open(cfg).code(), Code::InvalidArg);
}

TEST(V4l2Source, MissingDeviceReturnsIoErrorAndCanStillCloseTwice) {
    V4l2Source source;
    SourceConfig cfg = validConfig();
    cfg.device = "/tmp/lowlat_missing_video_device_" + std::to_string(::getpid());
    std::filesystem::remove(cfg.device);

    EXPECT_EQ(source.open(cfg).code(), Code::IoError);
    source.close();
    source.close();
}

TEST(V4l2Source, ReadBeforeOpenReturnsClosed) {
    V4l2Source source;
    RawFrame frame;

    EXPECT_EQ(source.readFrame(frame).code(), Code::Closed);
}

TEST(V4l2SourceHardware, ReadsOneFrameWhenDeviceWasExplicitlyProvided) {
    const char* device = std::getenv("LOWLAT_V4L2_DEVICE");
    if (!device || device[0] == '\0') {
        GTEST_SKIP() << "set LOWLAT_V4L2_DEVICE=/dev/video0 to enable the hardware smoke test";
    }

    V4l2Source source;
    SourceConfig cfg = validConfig();
    cfg.device = device;

    ASSERT_TRUE(source.open(cfg).isOk());
    const SourceConfig& actual = source.actualConfig();
    EXPECT_GT(actual.width, 0);
    EXPECT_GT(actual.height, 0);
    EXPECT_EQ(actual.width % 2, 0);
    EXPECT_EQ(actual.height % 2, 0);
    EXPECT_GT(actual.fps, 0);

    RawFrame frame;
    ASSERT_TRUE(source.readFrame(frame).isOk());
    EXPECT_EQ(frame.width, actual.width);
    EXPECT_EQ(frame.height, actual.height);
    EXPECT_EQ(frame.fmt, PixelFormat::YUV420P);
    EXPECT_EQ(frame.data.size(), frameBytes(actual.width, actual.height));
    EXPECT_GT(frame.captureMs, 0u);
    EXPECT_EQ(frame.frameId, 0u);

    source.close();
}
