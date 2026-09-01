/**
 * @file    main.cpp
 * @brief   receiver 参数解析、信号处理与管线装配
 * @author  zzj
 * @date    2026-08-15
 */

#include <algorithm>
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
        // M4.3 自适应水位。--jitter-ms 的含义变了: 自适应关掉时它是**固定水位**
        // (和 M3 逐字相同, 是 M4.5 的对照组); 打开时它是冷启动值与样本不足时的兜底。
        pipeline.jitter.delay.targetDelayMs = config.getInt("jitter-ms", 50);
        pipeline.jitter.delay.adaptive = config.getInt("jitter-adapt", 1) != 0;
        pipeline.jitter.delay.windowMs =
            static_cast<uint32_t>(std::max(0, config.getInt("jitter-window-ms", 10000)));
        pipeline.jitter.delay.delayPercentile = config.getInt("jitter-pct", 95);
        pipeline.jitter.delay.minDelayMs = config.getInt("jitter-min-ms", 10);
        pipeline.jitter.delay.maxDelayMs = config.getInt("jitter-max-ms", 500);
        pipeline.jitter.delay.downRateMsPerSec = config.getInt("jitter-down-rate", 10);
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

        // M4.1 NACK。--nack-window=0 关掉整条重传请求路径。
        pipeline.nack.windowPackets =
            static_cast<size_t>(std::max(0, config.getInt("nack-window", 1024)));
        pipeline.nack.maxRequestsPerSeq = config.getInt("nack-retries", 3);

        // M4.4 PLI。--pli-ms=0 关掉；否则是两次关键帧请求的最小间隔。
        pipeline.pliMinIntervalMs = config.getInt("pli-ms", 1000);

        // M4.2 FEC 解码。--fec-recent=0 关掉。
        pipeline.fec.recentPackets =
            static_cast<size_t>(std::max(0, config.getInt("fec-recent", 64)));
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
    const LossConfig lossConfig = pipelineConfig.loss;

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

    if (lossInjectionEnabled(lossConfig)) {
        LOG_WARN("receiver", "loss injection enabled: loss=%d%% seed=%u",
                 lossConfig.lossPercent, static_cast<unsigned>(lossConfig.seed));
    }

    status = pipeline.run(gStopRequested);

    const ReceiverPipelineStats& stats = pipeline.stats();
    LOG_INFO("receiver",
             "stopped: frames=%llu bytes=%llu key=%llu packets=%llu lost=%llu "
             "malformed=%llu dropped=%llu recv_errors=%llu jitter_dropped=%llu "
             "queue_dropped=%llu resyncs=%llu injected_drops=%llu "
             "nack_sent=%llu nack_seqs=%llu nack_recovered=%llu nack_gaveup=%llu "
             "lost_exact=%llu nack_pending=%zu "
             "fec_recv=%llu fec_recovered=%llu fec_unrecoverable=%llu "
             "pli_sent=%llu pli_suppressed=%llu "
             "jitter_delay=%dms jitter_peak=%dms jitter_raw=%dms "
             "jitter_floor=%dms floor_src=%s frame_int=%dms rtt=%dms rtt_n=%llu "
             "rtt_pending=%zu "
             "sampled_dropped=%llu decoded=%llu "
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
             static_cast<unsigned long long>(stats.nackPacketsSent),
             static_cast<unsigned long long>(stats.nack.nacksRequested),
             static_cast<unsigned long long>(stats.nack.recovered),
             static_cast<unsigned long long>(stats.nack.givenUp),
             static_cast<unsigned long long>(stats.nack.lostForReal),
             stats.nack.pending,
             static_cast<unsigned long long>(stats.fec.fecPacketsReceived),
             static_cast<unsigned long long>(stats.fec.groupsRecovered),
             static_cast<unsigned long long>(stats.fec.groupsUnrecoverable),
             static_cast<unsigned long long>(stats.pliSent),
             static_cast<unsigned long long>(stats.pliSuppressed),
             stats.delay.currentDelayMs, stats.delay.peakDelayMs,
             stats.delay.rawDelayMs,
             stats.delay.effectiveMinDelayMs,
             stats.delay.floorFromBudget ? "budget" : "min",
             stats.delay.frameIntervalMs, stats.rttMs,
             static_cast<unsigned long long>(stats.rttSamples),
             stats.rttPending,
             static_cast<unsigned long long>(stats.jitter.framesSampledButDropped),
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
