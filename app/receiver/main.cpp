/**
 * @file    main.cpp
 * @brief   receiver 参数解析、信号处理与管线装配
 * @author  zzj
 * @date    2026-08-15
 */

#include <atomic>
#include <csignal>
#include <string>
#include <utility>

#include "app/receiver/ReceiverPipeline.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "modules/transport/UdpSocket.h"

namespace {
    std::atomic<bool> gStopRequested{false};

    void onSignal(int signal) {
        if (signal == SIGINT || signal == SIGTERM) {
            // signal handler 内只改标志，不打日志、不加锁、不做资源释放。
            gStopRequested.store(true);
        }
    }

    ReceiverPipelineConfig makeReceiverConfig(const Config& config, bool& ok) {
        ReceiverPipelineConfig pipeline;
        ok = true;

        const std::string listen = config.get("listen", "0.0.0.0:9000");
        const Status status = parseEndpoint(listen, pipeline.listen);
        if (!status.isOk()) {
            LOG_ERROR("receiver", "invalid --listen '%s': %s", listen.c_str(),
                      status.toString().c_str());
            ok = false;
            return pipeline;
        }

        pipeline.h264DumpPath = config.get("dump");
        pipeline.maxFrames = config.getInt("frames", 0);
        pipeline.idleTimeoutMs = config.getInt("idle-timeout", 0);
        pipeline.recvTimeoutMs = config.getInt("recv-timeout", 200);
        return pipeline;
    }
}  // namespace

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) {
        return 1;
    }

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    bool configOk = false;
    ReceiverPipelineConfig pipelineConfig = makeReceiverConfig(config, configOk);
    if (!configOk) return 1;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ReceiverPipeline pipeline(std::move(pipelineConfig));
    Status status = pipeline.open();
    if (!status.isOk()) {
        LOG_ERROR("receiver", "open failed: %s", status.toString().c_str());
        return 1;
    }

    LOG_INFO("receiver", "listening on port %u",
             static_cast<unsigned>(pipeline.boundPort()));

    status = pipeline.run(gStopRequested);

    const ReceiverPipelineStats& stats = pipeline.stats();
    LOG_INFO("receiver",
             "stopped: frames=%llu bytes=%llu key=%llu packets=%llu lost=%llu "
             "malformed=%llu dropped=%llu recv_errors=%llu elapsed=%llums",
             static_cast<unsigned long long>(stats.framesWritten),
             static_cast<unsigned long long>(stats.bytesWritten),
             static_cast<unsigned long long>(stats.keyFrames),
             static_cast<unsigned long long>(stats.assembler.packetsReceived),
             static_cast<unsigned long long>(stats.assembler.packetsLost()),
             static_cast<unsigned long long>(stats.assembler.packetsMalformed),
             static_cast<unsigned long long>(stats.assembler.framesDropped),
             static_cast<unsigned long long>(stats.recvErrors),
             static_cast<unsigned long long>(stats.elapsedMs));

    if (!status.isOk()) {
        LOG_ERROR("receiver", "pipeline failed: %s", status.toString().c_str());
        return 1;
    }
    return 0;
}
