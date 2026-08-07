/**
 * @file    test_sender_pipeline.cpp
 * @brief   M1.4 sender 两线程管线验收测试
 * @author  zzj
 * @date    2026-08-04
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "app/sender/SenderPipeline.h"
#include "modules/capture/NullSource.h"

using namespace std::chrono_literals;

namespace {
    class TempOutputs {
    public:
        explicit TempOutputs(const std::string& caseName) {
            const std::string prefix = "lowlat_pipeline_" + std::to_string(::getpid()) + "_" + caseName;
            raw = std::filesystem::temp_directory_path() / (prefix + ".yuv");
            h264 = std::filesystem::temp_directory_path() / (prefix + ".h264");
            std::filesystem::remove(raw);
            std::filesystem::remove(h264);
        }

        ~TempOutputs() {
            std::error_code ignored;
            std::filesystem::remove(raw, ignored);
            std::filesystem::remove(h264, ignored);
        }

        std::filesystem::path raw;
        std::filesystem::path h264;
    };

    SenderPipelineConfig pipelineConfig(int frames = 30, int fps = 1000, int cap = 4) {
        SenderPipelineConfig cfg;
        cfg.source.width = 320;
        cfg.source.height = 240;
        cfg.source.fps = fps;

        cfg.encoder.width = cfg.source.width;
        cfg.encoder.height = cfg.source.height;
        cfg.encoder.fps = cfg.source.fps;
        cfg.encoder.bitrateKbps = 1000;
        cfg.encoder.gop = 10;

        cfg.queueCapacity = cap;
        cfg.maxFrames = frames;
        return cfg;
    }
}  // namespace

TEST(SenderPipeline, RejectsInvalidQueueCapacity) {
    SenderPipelineConfig cfg = pipelineConfig();
    cfg.queueCapacity = 0;

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
}

TEST(SenderPipeline, CapturesEncodesAndDrainsEveryFrame) {
    constexpr int FRAMES = 30;
    TempOutputs output("drain");
    SenderPipelineConfig cfg = pipelineConfig(FRAMES);
    cfg.rawDumpPath = output.raw.string();
    cfg.h264DumpPath = output.h264.string();

    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    ASSERT_TRUE(pipeline.run(stop).isOk());
    const SenderPipelineStats& stats = pipeline.stats();

    EXPECT_EQ(stats.capturedFrames, static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(stats.encodedFrames, stats.capturedFrames)
        << "队列关闭后必须先排空残留帧，再结束编码线程";
    EXPECT_GT(stats.encodedBytes, 0u);
    EXPECT_GT(stats.keyFrames, 0u);
    EXPECT_GT(stats.queuePeak, 0u);
    EXPECT_LE(stats.queuePeak, static_cast<size_t>(cfg.queueCapacity));

    ASSERT_TRUE(std::filesystem::exists(output.raw));
    ASSERT_TRUE(std::filesystem::exists(output.h264));
    EXPECT_EQ(std::filesystem::file_size(output.raw),
              frameBytes(cfg.source.width, cfg.source.height) * static_cast<uint64_t>(FRAMES));
    EXPECT_EQ(std::filesystem::file_size(output.h264), stats.encodedBytes);
}

TEST(SenderPipeline, StopRequestExitsWithinOneSecondAndDrainsQueue) {
    SenderPipelineConfig cfg = pipelineConfig(0, 30);  // 0 表示持续运行，等待停止标志
    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    std::atomic<bool> stop{false};

    auto result = std::async(std::launch::async, [&] { return pipeline.run(stop); });
    std::this_thread::sleep_for(100ms);
    stop.store(true);

    ASSERT_EQ(result.wait_for(1s), std::future_status::ready)
        << "停止请求后 1 秒内必须完成 close、drain、flush 和 join";
    EXPECT_TRUE(result.get().isOk());
    EXPECT_EQ(pipeline.stats().capturedFrames, pipeline.stats().encodedFrames);
}

TEST(SenderPipeline, CannotRunTwiceAfterQueueHasClosed) {
    SenderPipelineConfig cfg = pipelineConfig(3);
    SenderPipeline pipeline(std::make_unique<NullSource>(), cfg);
    const std::atomic<bool> stop{false};

    ASSERT_TRUE(pipeline.run(stop).isOk());
    EXPECT_EQ(pipeline.run(stop).code(), Code::InvalidArg);
}
