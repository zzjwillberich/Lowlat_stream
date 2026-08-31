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
#include "modules/transport/FecEncoder.h"
#include "modules/transport/Packetizer.h"
#include "modules/transport/RetransmitCache.h"
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

    /**
     * @brief 重传缓存的配置 (M4.1); retentionMs 为 0 表示**不启用重传**
     *
     * @note 关掉时不只是缓存不存, 反向通道那条 recvFrom 也不该走 ——
     *          否则 sendLoop 每轮都要多一次 syscall 去等一个永远不会来的包。
     *          "关掉 = 那一级完全不存在"和 target.ip 为空、renderKind 为空是同一条原则。
     */
    RetransmitCacheConfig retransmit;

    /**
     * @brief 反向通道每轮最多处理几个包, 0 表示不限
     *
     * @note 有上限是因为 sendLoop 还得发媒体数据。极端情况下(对端疯狂发 NACK、
     *          或者有人往这个端口灌包)不设上限会让发送线程一直在收包, **媒体流饿死** ——
     *          现象是"一开始丢包就彻底不出画了", 而根因在发送端的循环结构上。
     *          剩下的包留在内核缓冲里, 下一轮再处理。
     */
    int reversePacketsPerIteration = 16;

    /**
     * @brief sendQueue_ 的等待超时(毫秒), 必须大于 0
     *
     * @note 这条线程要同时等两件事: 队列来帧、socket 来 NACK。而队列是条件变量、
     *          socket 是 fd, **poll 只能等 fd, 等不了条件变量** —— 要合并只能给队列
     *          配一个 eventfd, 那会让 BoundedQueue 从纯 C++ 容器变成绑 Linux fd 的东西,
     *          M0 那句"只依赖 llcommon 和 libc"就破了。所以走 D16 的老办法:
     *          带超时地等队列, 每轮顺便非阻塞地收一轮 socket。
     *
     * @note 但和 D16 那次不同, **这里的超时直接吃重传的时间预算**:
     *          targetDelayMs=50、RTT=30ms 时重传预算只有约 20ms, 5ms 就吃掉四分之一。
     *          所以给 1ms 而不是 D16 的 5ms。1000 次/秒的空转循环可以忽略。
     */
    int sendPollMs = 1;

    /**
     * @brief FEC 分组策略 (M4.2); groupSize 为 0 表示**不发 FEC**
     *
     * @note FEC 包在本帧最后一片之后紧跟发出, **不跨帧** —— 跨帧的话 FEC 包比它保护的
     *          数据晚一整帧, 而 FEC 相对 NACK 的全部优势就是"不用等一个往返"。
     */
    FecEncoderConfig fec;
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

    /** @brief 收到的 NACK **包**数 (M4.1) */
    uint64_t nacksReceived = 0;

    /**
     * @brief NACK 里请求的 seq **条目**数
     *
     * @note 和 nacksReceived 分开: 一个 NACK 包可以请求几百个 seq。
     *          "收到 3 个 NACK 包"和"被请求了 300 个包"是两个完全不同的严重程度。
     */
    uint64_t nackedSeqs = 0;

    /** @brief 实际重发出去的包数; 加上 misses 才等于 nackedSeqs */
    uint64_t packetsRetransmitted = 0;

    /**
     * @brief 请求的 seq 已经不在重传缓存里的次数
     *
     * @note 稳态下应当接近 0。持续非 0 说明 retentionMs 太短, 或者接收端的重传窗口
     *          比这边的保留时长还长 —— 两边的时间预算没对齐。这个数是唯一能区分
     *          "NACK 没到"和"NACK 到了但这边没货"的线索, 别和 nackedSeqs 混成一个。
     */
    uint64_t retransmitMisses = 0;

    /** @brief 反向通道上收到的畸形包数; 和 NACK 计数分开, 同 lost / malformed 的理由 */
    uint64_t reverseMalformed = 0;

    /**
     * @brief 收到的 PLI 包数 (M4.4)
     *
     * @note 和 nacksReceived 分开: 一个要的是"重发某几个包", 一个要的是"立刻出一个 IDR",
     *          代价差一个数量级(一个包 vs 25 个包)。混成一个数就看不出对端到底有多绝望。
     */
    uint64_t plisReceived = 0;

    /**
     * @brief FEC 编码器的计数器快照 (M4.2)
     *
     * @note `fec.fecBytes / encodedBytes` 就是**实测的冗余开销**。这个数要和
     *          接收端的 FEC 恢复率一起报 —— 单看恢复率不知道花了多少钱, 单看开销
     *          不知道买到了什么。
     */
    FecEncoderStats fec;
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

    /**
     * @brief M4.1: 收一轮反向通道上的包(NACK), 并把请求的分片重发出去
     *
     * @note **非阻塞**: recvFrom 用 timeoutMs = 0, 没包就立刻返回 Timeout。
     *          这条线程的主业是发媒体, 不能在这里等。
     *
     * @note 每轮最多处理 config_.reversePacketsPerIteration 个包。不设上限的话,
     *          对端疯狂发 NACK(或者有人往这个端口灌包)会让发送线程一直在收包,
     *          **媒体流饿死** —— 现象是"一开始丢包就彻底不出画", 而根因在循环结构上。
     *
     * @note 重传要用 packetizeOneFragment 而不是 packetizer_.packetize():
     *          后者会推进 nextSeq_, 后续原发包就跳号, 接收端把跳号当丢包,
     *          触发一轮**真正的** NACK 风暴 —— 一次重传引发一片重传。
     *          自由函数那个签名连 nextSeq_ 都碰不到, 这条是结构上保证的。
     *
     * @note 重发的目的地用 **NACK 包的来源地址**, 不是 config_.target:
     *          跨 NAT 时只有"回到来源地址"的包能通(同接收端认 peer_ 的理由)。
     *          M4 全在回环上, 两者相同 —— 但写对了就不用等到跨机器再返工。
     */
    void drainReverseChannel();

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

    /**
     * @brief M4.1 重传缓存; 只在 retransmit.retentionMs > 0 时创建
     *
     * @note 存的是**帧**不是打好的包。真正的理由是零拷贝而不是省内存(实测只省 1.9%):
     *          packets_ 是跨帧复用的, 缓存整包必须每帧拷一份出来, 把上面那条
     *          "跨帧复用让堆分配归零"废掉; 而帧本来就要在 sendLoop 末尾析构,
     *          改成 move 进缓存一次拷贝都没有。详见 [[D23]]。
     */
    std::unique_ptr<RetransmitCache> retransmitCache_;

    /** @brief M4.2 冗余包生成器; 只在 fec.groupSize > 0 时创建 */
    std::unique_ptr<FecEncoder> fecEncoder_;

    /** @brief FEC 包的输出缓冲, 同 packets_ 跨帧复用 */
    std::vector<PacketBuffer> fecPackets_;

    /** @brief drainReverseChannel 复用的收包缓冲和 seq 列表, 同 packets_ 的理由 */
    std::vector<uint8_t> reverseBuf_;
    std::vector<uint32_t> nackedSeqs_;
    PacketBuffer retransmitBuf_;

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
