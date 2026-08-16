/**
 * @file    ReceiverPipeline.h
 * @brief   receiver 收包 -> 组包 -> 落盘的生命周期编排
 * @author  zzj
 * @date    2026-08-15
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "common/Status.h"
#include "modules/transport/FrameAssembler.h"
#include "modules/transport/UdpSocket.h"

/**
 * receiver 的完整配置。
 */
struct ReceiverPipelineConfig {
    /** @brief 监听地址; ip 为空表示所有网卡, port 为 0 表示由内核分配(测试用) */
    Endpoint listen{"0.0.0.0", 9000};

    /** @brief 可选的 H.264 Annex B dump 路径; 空字符串表示不写 */
    std::string h264DumpPath;

    /**
     * @brief 最多写出的完整帧数
     *
     * @note 大于 0 时收满即正常退出; 等于 0 表示一直收, 直到 stopRequested 置位。
     */
    int maxFrames = 0;

    /**
     * @brief 连续多久没收到任何包就退出(毫秒), 0 表示不因空闲退出
     *
     * @note 脚本化验收要靠它: sender 发完就退出了, receiver 不能傻等。
     *          注意计时基准是**收到包**而不是**收到完整帧** —— 只收到分片但组不齐时,
     *          网络显然还活着, 这时退出会把"一直丢包"误报成"对端已停止"。
     */
    int idleTimeoutMs = 0;

    /**
     * @brief 单次 recvFrom 的等待上限(毫秒)
     *
     * @note 不能设成无限: 收包线程要定期回到循环顶部检查 stopRequested,
     *          否则 Ctrl-C 之后进程卡在 poll 里不动。这个值决定了停止响应的最坏延迟。
     */
    int recvTimeoutMs = 200;

    /** @brief 组包器同时保留的帧数上限 */
    size_t maxPendingFrames = 8;
};

/**
 * 一次 receiver 运行的统计结果。
 */
struct ReceiverPipelineStats {
    /** @brief 写出的完整帧数 */
    uint64_t framesWritten = 0;

    /** @brief 写出的字节数 */
    uint64_t bytesWritten = 0;

    /** @brief 其中的关键帧数 */
    uint64_t keyFrames = 0;

    /** @brief recvFrom 返回 NetError 的次数(不含超时) */
    uint64_t recvErrors = 0;

    /** @brief 组包器的计数器快照 */
    AssemblerStats assembler;

    uint64_t elapsedMs = 0;
};

/**
 * receiver 收包管线。
 *
 * ```text
 * [UdpSocket::recvFrom] --> [FrameAssembler::offer/pop] --> [dump 文件]
 * ```
 *
 * M2 只有一个线程: 收包、组包、落盘都在 run() 里跑完。**这是有意的** ——
 * 现在没有解码器也没有渲染, 多切一个线程只会多一个 join 要收尾, 换不来任何并行。
 * M3 加 jitter buffer 和解码线程时, 拆分点就在 pop() 之后。
 *
 * @note open() 和 run() 分开: 测试要先拿到内核分配的端口(bind 传 0), 才能让 sender
 *          知道往哪发。端口写死在测试里迟早会在 CI 上和别的进程撞车。
 * @note 本对象只能 run() 一次。
 */
class ReceiverPipeline {
public:
    explicit ReceiverPipeline(ReceiverPipelineConfig config);

    ReceiverPipeline(const ReceiverPipeline&) = delete;
    ReceiverPipeline& operator=(const ReceiverPipeline&) = delete;

    /**
     * @brief 校验配置、绑定端口、打开 dump 文件
     *
     * @return Ok       套接字已绑定, 可以从 boundPort() 读实际端口
     *  InvalidArg 配置不合法
     *  IoError    dump 文件打不开
     *  NetError   bind 失败(端口被占用等)
     *
     * @note 单独暴露是为了让"端口已就绪"成为一个**可等待的时刻**。run() 里再 bind 的话,
     *          调用方只能靠 sleep 猜它绑好了没有 —— 那种测试在慢机器上必然偶发失败。
     */
    Status open();

    /**
     * @brief 收包直到达到帧数上限、空闲超时或收到停止请求
     *
     * @param stopRequested 外部停止标志
     *
     * @return Ok      正常退出(含空闲超时和收满帧数)
     *  Closed  未调用 open() 或 open() 失败
     *  IoError dump 写入失败
     *
     * @note 单个畸形包/收包错误**不终止**循环, 只计数。UDP 上收到垃圾是常态,
     *          为一个坏包退出整个接收端, 等于把对端的 bug 变成自己的可用性问题。
     */
    Status run(const std::atomic<bool>& stopRequested);

    /**
     * @brief 实际绑定到的端口
     *
     * @return open() 成功后为内核分配的端口; 之前为 0
     */
    uint16_t boundPort() const;

    const ReceiverPipelineStats& stats() const { return stats_; }

private:
    /** @brief 校验配置; 不碰任何资源 */
    Status validateConfig() const;

    /** @brief 把一帧写进 dump 并更新统计 */
    Status writeFrame(const AssembledFrame& frame);

    /** @brief 关闭 socket 和文件; 必须幂等 */
    void closeResources();

    ReceiverPipelineConfig config_;
    UdpSocket socket_;
    FrameAssembler assembler_;

    /** @brief 复用的收包缓冲, 每次 recvFrom 都新建一个 vector 是纯浪费 */
    std::vector<uint8_t> recvBuf_;

    std::ofstream h264File_;
    ReceiverPipelineStats stats_;
    bool opened_ = false;
    bool hasRun_ = false;
};
