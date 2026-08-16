/**
 * @file    main.cpp
 * @brief   receiver 参数解析、信号处理与管线装配
 * @author  zzj
 * @date    2026-08-15
 */

#include <atomic>
#include <csignal>

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

    // TODO(M2): 照着 sender 的 makePipelineConfig 写:
    //  ReceiverPipelineConfig makeReceiverConfig(const Config& config, bool& ok);
    //  - listen        ← parseEndpoint(config.get("listen", "0.0.0.0:9000"))
    //                    解析失败要让 main 返回 1, 别退回默认端口后安静跑起来 ——
    //                    用户明明写了 --listen 却在别的端口上监听, 等他发现已经浪费半天
    //                    (同 createSource 对未知 source 的处理);
    //  - h264DumpPath  ← config.get("dump")
    //  - maxFrames     ← config.getInt("frames", 0)
    //  - idleTimeoutMs ← config.getInt("idle-timeout", 0)
    //  - recvTimeoutMs ← config.getInt("recv-timeout", 200)
}  // namespace

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) {
        return 1;
    }

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // TODO(M2): 装配并运行:
    //  1. 构造 ReceiverPipelineConfig(解析失败 return 1);
    //  2. ReceiverPipeline pipeline(cfg);
    //  3. pipeline.open() 失败 → LOG_ERROR + return 1;
    //  4. open() 成功后打一条 "listening on port %u" —— 用 boundPort() 而不是配置里的值,
    //     bind(0) 时那才是真正在听的端口;
    //  5. pipeline.run(gStopRequested);
    //  6. 收尾打统计: framesWritten / bytesWritten / keyFrames /
    //     assembler.packetsReceived / packetsLost() / packetsMalformed / framesDropped。
    //     丢包和畸形包要**分开打**, 混在一起就分不出该修网络还是该修对端。
    LOG_INFO("receiver", "receiver starting");
    LOG_DEBUG("receiver", "log-level=%s listen=%s",
              config.get("log-level", "info").c_str(),
              config.get("listen", "0.0.0.0:9000").c_str());
    return 0;
}
