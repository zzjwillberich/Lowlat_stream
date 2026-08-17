/**
 * @file    main.cpp
 * @brief   sender 参数解析、信号处理与管线装配
 * @author  zzj
 * @date    2026-08-04
 */

#include <atomic>
#include <csignal>
#include <string>
#include <utility>

#include "app/sender/SenderPipeline.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "modules/capture/ISource.h"

namespace {
    std::atomic<bool> gStopRequested{false};

    void onSignal(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            // signal handler 内只改标志，不打日志、不加锁、不做资源释放。
            gStopRequested.store(true);
        }
    }

    SenderPipelineConfig makePipelineConfig(const Config& config, bool& ok) {
        SenderPipelineConfig pipeline;
        ok = true;

        pipeline.source.width  = config.getInt("width", 640);
        pipeline.source.height = config.getInt("height", 480);
        pipeline.source.fps    = config.getInt("fps", 30);
        pipeline.source.device = config.get("device", "/dev/video0");

        pipeline.encoder.width       = pipeline.source.width;
        pipeline.encoder.height      = pipeline.source.height;
        pipeline.encoder.fps         = pipeline.source.fps;
        pipeline.encoder.bitrateKbps = config.getInt("bitrate", 2000);
        pipeline.encoder.gop         = config.getInt("gop", pipeline.source.fps);

        pipeline.queueCapacity = config.getInt("cap", 4);
        pipeline.sendQueueCapacity = config.getInt("send-cap", 4);
        pipeline.maxFrames     = config.getInt("frames", 100);
        pipeline.rawDumpPath   = config.get("dump-raw");
        pipeline.h264DumpPath  = config.get("dump");

        const std::string target = config.get("target", "127.0.0.1:9000");
        if (!target.empty()) {
            const Status status = parseEndpoint(target, pipeline.target);
            if (!status.isOk()) {
                LOG_ERROR("sender", "invalid --target '%s': %s", target.c_str(),
                          status.toString().c_str());
                ok = false;
            }
        }
        return pipeline;
    }
}  // namespace

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) return 1;

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    bool configOk = false;
    SenderPipelineConfig pipelineConfig = makePipelineConfig(config, configOk);
    if (!configOk) return 1;

    auto source = createSource(config.get("source", "null"));
    if (!source) return 1;  // 工厂已经记录具体错误

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SenderPipeline pipeline(std::move(source), std::move(pipelineConfig));
    const Status status = pipeline.run(gStopRequested);
    if (!status.isOk()) {
        LOG_ERROR("sender", "pipeline failed: %s", status.toString().c_str());
        return 1;
    }

    const SenderPipelineStats& stats = pipeline.stats();
    LOG_INFO("sender",
             "stopped: captured=%llu encoded=%llu bytes=%llu key=%llu "
             "queue_peak=%zu send_queue_peak=%zu packets_sent=%llu send_errors=%llu "
             "elapsed=%llums",
             static_cast<unsigned long long>(stats.capturedFrames),
             static_cast<unsigned long long>(stats.encodedFrames),
             static_cast<unsigned long long>(stats.encodedBytes),
             static_cast<unsigned long long>(stats.keyFrames), stats.queuePeak,
             stats.sendQueuePeak, static_cast<unsigned long long>(stats.packetsSent),
             static_cast<unsigned long long>(stats.sendErrors),
             static_cast<unsigned long long>(stats.elapsedMs));
    return 0;
}
