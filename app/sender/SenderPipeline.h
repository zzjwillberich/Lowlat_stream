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

#include "common/BoundedQueue.h"
#include "common/Status.h"
#include "modules/capture/ISource.h"
#include "modules/encode/Encoder.h"

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
};

/**
 * sender 两线程管线。
 *
 * ```text
 * [captureLoop] -- unique_ptr<RawFrame> --> [BoundedQueue] --> [encodeLoop]
 * ```
 *
 * 生命周期由 run() 统一管理：打开资源、启动线程、等待退出、flush、关闭资源并汇总统计。
 * 任一工作线程失败时必须让另一线程可退出，不能把生产者永久堵在满队列上。
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
     */
    void encodeLoop();

    /** @brief 写出一个编码结果并更新 encodedBytes/keyFrames */
    Status writeEncodedFrame(const EncodedFrame& frame);

    /** @brief 关闭所有资源；必须幂等，供成功和失败路径共同调用 */
    void closeResources();

    std::unique_ptr<ISource> source_;
    SenderPipelineConfig config_;
    Encoder encoder_;
    std::unique_ptr<BoundedQueue<std::unique_ptr<RawFrame>>> queue_;

    std::ofstream rawFile_;
    std::ofstream h264File_;

    // encode 失败时通知 capture 尽快停止；具体错误在线程 join 后读取对应 Status。
    std::atomic<bool> abortRequested_{false};
    Status captureStatus_;
    Status encodeStatus_;
    SenderPipelineStats stats_;
    bool hasRun_ = false;
};
