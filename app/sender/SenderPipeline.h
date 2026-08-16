/**
 * @file    SenderPipeline.h
 * @brief   sender 采集线程与编码线程的生命周期编排
 * @author  zzj
 * @date    2026-08-04
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/BoundedQueue.h"
#include "common/Status.h"
#include "modules/capture/ISource.h"
#include "modules/encode/Encoder.h"
#include "modules/transport/Packetizer.h"
#include "modules/transport/UdpSocket.h"

/**
 * sender 两线程管线的完整配置。
 *
 * @note main 只负责把命令行转换成该结构；参数校验、资源打开和线程收尾属于管线职责。
 */
struct SenderPipelineConfig {
    SourceConfig source;
    EncoderConfig encoder;

    /** @brief RawFrame 队列容量，必须大于 0 */
    int queueCapacity = 4;

    /**
     * @brief 最多采集的帧数
     *
     * @note 大于 0 时采满后正常退出；等于 0 时持续运行，直到 stopRequested 置位。
     */
    int maxFrames = 100;

    /** @brief 可选的原始 YUV420P dump 路径；空字符串表示不写 */
    std::string rawDumpPath;

    /** @brief 可选的 H.264 Annex B dump 路径；空字符串表示不写 */
    std::string h264DumpPath;

    /**
     * @brief 发送目标
     *
     * @note ip 为空表示**不发送**，只做本地 dump —— 这正是 M1 的行为。留着这条路径
     *          不只是为了兼容：没有网络参与的那一档能把"采集编码对不对"和"传输对不对"
     *          分开定位，出问题时第一件事就是关掉发送再跑一遍。
     */
    Endpoint target;

    /**
     * @brief EncodedFrame 队列容量，必须大于 0
     *
     * @note 队满时编码线程**阻塞**等待，M2 就这样。M4 会改成丢非关键帧保 IDR ——
     *          那时才有"丢什么"的判断依据（flags 里的关键帧位）。现在阻塞是诚实的：
     *          本机回环发不出去只可能是自己写错了，悄悄丢包只会把 bug 藏起来。
     */
    int sendQueueCapacity = 4;
};

/**
 * 一次 sender 管线运行的统计结果。
 *
 * @note 这些字段只在线程退出并 join 后由外部读取，不需要为每个计数器付出原子操作开销。
 */
struct SenderPipelineStats {
    uint64_t capturedFrames = 0;
    uint64_t encodedFrames = 0;
    uint64_t encodedBytes = 0;
    uint64_t keyFrames = 0;
    size_t queuePeak = 0;
    uint64_t elapsedMs = 0;

    /** @brief 成功发出的 UDP 包数 */
    uint64_t packetsSent = 0;

    /** @brief sendTo 失败的次数 */
    uint64_t sendErrors = 0;

    /** @brief EncodedFrame 队列的峰值长度 */
    size_t sendQueuePeak = 0;
};

/**
 * sender 两线程管线。
 *
 * ```text
 * [captureLoop] --RawFrame队列--> [encodeLoop] --EncodedFrame队列--> [sendLoop]
 * ```
 *
 * 生命周期由 run() 统一管理：打开资源、启动线程、等待退出、flush、关闭资源并汇总统计。
 * 任一工作线程失败时必须让另一线程可退出，不能把生产者永久堵在满队列上。
 *
 * 第三级只在 `config.target.ip` 非空时启动；为空时行为与 M1 完全一致（只 dump 不发送）。
 *
 * @note 打包和发送放在**独立线程**而不是搭在编码线程尾巴上：sendTo 会被内核缓冲区
 *          顶住，M4 的 pacing 控速和重传缓存也都要按自己的节奏走。挂在编码线程上，
 *          网络一卡就会把编码器一起拖慢，最后表现成"网络差的时候帧率也掉了"——
 *          明明这两件事该是解耦的。
 *
 * @note 本对象只能 run() 一次。队列一旦 close 不可重新打开，重复运行属于调用方错误。
 * @note 本类不是线程安全的；只有传给 run() 的停止标志会被信号处理函数异步修改。
 */
class SenderPipeline {
public:
    /**
     * @brief 构造 sender 管线并接管采集源所有权
     *
     * @param source 已由工厂创建的采集源，不可为空
     * @param config 本次运行配置
     */
    SenderPipeline(std::unique_ptr<ISource> source, SenderPipelineConfig config);

    SenderPipeline(const SenderPipeline&) = delete;
    SenderPipeline& operator=(const SenderPipeline&) = delete;

    /**
     * @brief 运行采集与编码管线，直到达到帧数上限、收到停止请求或发生错误
     *
     * @param stopRequested 外部停止标志；SIGINT handler 只负责将它置为 true
     *
     * @return Ok         正常采满或响应停止请求后干净退出
     *  InvalidArg 配置或 source 不合法
     *  IoError    source 或 dump 文件读写失败
     *  Internal   编码器或线程内部错误
     *
     * @note 返回前必须完成两个线程的 join、编码器 flush 和所有资源关闭。
     */
    Status run(const std::atomic<bool>& stopRequested);

    /**
     * @brief 获取最近一次运行的统计
     *
     * @return 只读统计引用；仅允许在 run() 返回后读取
     */
    const SenderPipelineStats& stats() const { return stats_; }

private:
    /** @brief 校验构造参数以及 source/encoder 配置的一致性 */
    Status validateConfig() const;

    /** @brief 打开 source、encoder 和可选 dump 文件；任一步失败都要回滚已打开资源 */
    Status openResources();

    /**
     * @brief 采集线程入口
     *
     * 循环读取 RawFrame 并转移进 queue_。退出前无论成功失败都必须 close queue_，
     * 让编码线程把残留帧取完后自然退出。
     */
    void captureLoop(const std::atomic<bool>& stopRequested);

    /**
     * @brief 编码线程入口
     *
     * 从 queue_ 取帧、编码并写 dump。队列关闭且取空后 flush 编码器。
     * 编码失败时必须 close queue_，唤醒可能阻塞在 push() 的采集线程。
     *
     * @note 顺序是**先写 dump 再把帧移进发送队列**：移动之后那一帧就不属于本线程了。
     *          反过来写会读到已被移走的 data，是个只在多线程下偶发的空指针/空 vector。
     * @note 退出前必须 close sendQueue_，否则发送线程永远阻塞在 pop() 上，join 回不来。
     */
    void encodeLoop();

    /**
     * @brief 发送线程入口
     *
     * 从 sendQueue_ 取帧，用 packetizer_ 切片后逐包 sendTo。
     *
     * @note 单包发送失败**不中断整条管线**：UDP 上丢一个包是常态，记一笔
     *          sendErrors 继续发下一个。真正该中止的是 socket 本身坏了，
     *          那种情况下每个包都会失败，计数会明白地告诉你。
     */
    void sendLoop();

    /** @brief 写出一个编码结果并更新 encodedBytes/keyFrames */
    Status writeEncodedFrame(const EncodedFrame& frame);

    /** @brief 关闭所有资源；必须幂等，供成功和失败路径共同调用 */
    void closeResources();

    std::unique_ptr<ISource> source_;
    SenderPipelineConfig config_;
    Encoder encoder_;
    std::unique_ptr<BoundedQueue<std::unique_ptr<RawFrame>>> queue_;

    /** @brief 只在 target.ip 非空时创建；为空表示这一级不存在 */
    std::unique_ptr<BoundedQueue<std::unique_ptr<EncodedFrame>>> sendQueue_;
    UdpSocket socket_;
    Packetizer packetizer_;

    /** @brief sendLoop 复用的包缓冲，跨帧复用才能让堆分配次数归零 */
    std::vector<PacketBuffer> packets_;

    std::ofstream rawFile_;
    std::ofstream h264File_;

    // encode 失败时通知 capture 尽快停止；具体错误在线程 join 后读取对应 Status。
    std::atomic<bool> abortRequested_{false};
    Status captureStatus_;
    Status encodeStatus_;
    Status sendStatus_;
    SenderPipelineStats stats_;
    bool hasRun_ = false;
};
