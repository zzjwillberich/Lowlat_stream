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
        pipeline.renderKind = config.get("render", "sdl");
        pipeline.jitter.targetDelayMs = config.getInt("jitter-ms", 50);
        pipeline.decoder.threads = config.getInt("threads", 1);
        pipeline.renderer.vsync = config.getInt("vsync", 0) != 0;
        pipeline.statsIntervalMs = config.getInt("stats-interval", 1000);

        // M4.0 丢包注入。两个都是整数, 正好用现成的 getInt —— 写成 --loss=5%
        // 或 --loss=0.05 都得先加一个解析函数, 不值得。
        //
        // 默认值让"什么都不给 = 什么都不注入"成立: seed 的默认就是哨兵。
        // --loss 给了但 --seed 没给会被 validateConfig() 拒掉, 不在这里查 ——
        // 校验集中在一处, 这里只负责把命令行搬进配置。
        pipeline.loss.seed = static_cast<uint32_t>(config.getInt("seed", 0));
        pipeline.loss.lossPercent = config.getInt("loss", 0);
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

    // TODO(M4.0): 注入器开着的时候必须在这里打一行 WARN, 写明 loss% 和 seed。
    //   一份没写明"注入了 N% 丢包"的延迟报告是有害的 —— 看报告的人(包括三个月后
    //   的你自己)会把注入的丢包当成真实网络表现。用 WARN 不用 INFO: 它不是常规
    //   运行状态, 应该显眼。

    status = pipeline.run(gStopRequested);

    const ReceiverPipelineStats& stats = pipeline.stats();
    LOG_INFO("receiver",
             "stopped: frames=%llu bytes=%llu key=%llu packets=%llu lost=%llu "
             "malformed=%llu dropped=%llu recv_errors=%llu jitter_dropped=%llu "
             "queue_dropped=%llu resyncs=%llu injected_drops=%llu decoded=%llu "
             "rendered=%llu queue_peak=%zu/%zu elapsed=%llums",
             static_cast<unsigned long long>(stats.framesWritten),
             static_cast<unsigned long long>(stats.bytesWritten),
             static_cast<unsigned long long>(stats.keyFrames),
             static_cast<unsigned long long>(stats.assembler.packetsReceived),
             static_cast<unsigned long long>(stats.assembler.packetsLost()),
             static_cast<unsigned long long>(stats.assembler.packetsMalformed),
             static_cast<unsigned long long>(stats.assembler.framesDropped),
             static_cast<unsigned long long>(stats.recvErrors),
             static_cast<unsigned long long>(stats.jitter.framesTooLate +
                                             stats.jitter.framesDropped +
                                             stats.jitter.framesDroppedForResync),
             static_cast<unsigned long long>(stats.decodeQueueDropped +
                                             stats.renderQueueDropped),
             static_cast<unsigned long long>(stats.decodeResyncs),
             static_cast<unsigned long long>(stats.injectedDrops),
             static_cast<unsigned long long>(stats.decoder.framesOut),
             static_cast<unsigned long long>(stats.renderer.framesRendered),
             stats.decodeQueuePeak, stats.renderQueuePeak,
             static_cast<unsigned long long>(stats.elapsedMs));

    if (!status.isOk()) {
        LOG_ERROR("receiver", "pipeline failed: %s", status.toString().c_str());
        return 1;
    }
    return 0;
}
