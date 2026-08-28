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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/BoundedQueue.h"
#include "common/Metrics.h"
#include "common/Status.h"
#include "modules/decode/Decoder.h"
#include "modules/render/IRenderer.h"
#include "modules/transport/FrameAssembler.h"
#include "modules/transport/JitterBuffer.h"
#include "modules/transport/LossInjector.h"
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

    /**
     * @brief 渲染器种类: "sdl" 开窗 | "null" 不开窗只校验计数 | "" 完全不渲染
     *
     * @note 空字符串和 "null" 不是一回事: 前者连渲染线程都不起(M2 那种纯落盘模式),
     *          后者照常走完解码和渲染两级, 只是不开窗 —— CI 上要的是后者。
     */
    std::string renderKind = "sdl";

    /** @brief jitter buffer 的整套配置; 命令行的 --jitter-ms 落在 targetDelayMs 上 */
    JitterBufferConfig jitter;

    /** @brief 解码器配置 */
    DecoderConfig decoder;

    /** @brief 渲染器配置; width/height 只是窗口初始大小, 画面尺寸以每帧自带的为准 */
    RendererConfig renderer;

    /**
     * @brief 队列 A(组包 -> 解码)的容量
     *
     * @note **队列容量就是延迟预算**(NOTES D4): 30fps 下 cap=3 就是 100ms。
     *          这跟发送端恰好相反 —— 发送端积压只占内存, 接收端积压是实打实
     *          加到用户看到的延迟上。所以这里给小。
     */
    size_t decodeQueueCapacity = 3;

    /** @brief 队列 B(解码 -> 渲染)的容量; 同上, 给小 */
    size_t renderQueueCapacity = 3;

    /**
     * @brief 统计行的打印间隔(毫秒), 0 表示只在退出时打一次总账
     *
     * @note **绝对不能每帧一行**(CONVENTIONS: 热路径禁 INFO)。30fps 跑一分钟就是
     *          1800 行, 而且格式化本身就会污染你要测的那个延迟(NOTES D1)。
     */
    int statsIntervalMs = 1000;

    /**
     * @brief M4.0 丢包注入; 默认关闭(seed 为哨兵)
     *
     * @note 放在**接收端**而不是发送端: 真实丢包发生在传输途中, 发送端照常发出去了,
     *          是接收端没收到。放在这一侧还有个附带好处 —— 包真的上了网线、真的过了
     *          内核, 网络路径上的时序一点没变, 不像发送端跳过 sendTo 会省掉一次系统调用。
     *
     * @note **它是测试工具, 不是业务功能**: 默认必须完全不生效, 且开启时要在日志里
     *          说清楚 —— 一份没写明"注入了 10% 丢包"的延迟报告是有害的。
     */
    LossConfig loss;
};

/**
 * 端到端延迟的汇总结果
 *
 * @note 存的是**算好的数**而不是 LatencyRecorder 本身: 后者的 percentile() 是非 const
 *          (取数要排序), 塞进一个到处被拷贝的统计结构体里会很别扭。
 * @note 报这几个数字的时候, 光有它们是没有信息量的 —— 必须同时报出边界:
 *          本机回环 / 运行时长 / 丢帧数 / jitter 水位 / 分辨率帧率 / vsync 开没开 /
 *          "不含最后一次 vsync(<=16.7ms @60Hz)"。完整清单见 M3.4 的"报告里必须带的字段"。
 */
struct LatencySummary {
    /**
     * @brief 实际计入统计的样本数
     *
     * @note **不等于**采集帧数, 也不等于 framesWritten: 被丢的、渲染失败的、
     *          captureMs 是哨兵 0 的, 都不在里面。
     */
    uint64_t samples = 0;

    uint32_t p50Ms = 0;
    uint32_t p95Ms = 0;
    uint32_t p99Ms = 0;

    /** @brief 最惨的那一次; 一次卡顿就能把它拉满而 p99 纹丝不动, 所以两个都要报 */
    uint32_t maxMs = 0;
};

/**
 * 一次 receiver 运行的统计结果。
 *
 * @note 这个结构体会被**跨线程拷贝**(工作线程发布快照, 渲染线程读快照),
 *          所以它必须保持可拷贝 —— 别往里塞 atomic 或 mutex(NOTES D18)。
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

    /** @brief jitter buffer 的计数器快照 */
    JitterBufferStats jitter;

    /** @brief 解码器的计数器快照; framesMissingMeta 稳态下应当恒为 0 */
    DecoderStats decoder;

    /** @brief 渲染器的计数器快照; framesRejected 应为 0, texturesRebuilt 稳态为 1 */
    RendererStats renderer;

    /** @brief 端到端延迟汇总 */
    LatencySummary latency;

    /**
     * @brief 两个队列见过的最大深度
     *
     * @note 顶到头意味着下游跟不上。队列 A 顶到头尤其要注意 —— 那是 D11 那根
     *          **保险丝**烧了, dropUntilKeyFrame() 被调过, 画面真的冻结过(最长一个 GOP),
     *          而这件事在 p99 里一点痕迹都没有(冻结期间根本没有帧被渲染, 也就没有样本)。
     */
    size_t decodeQueuePeak = 0;
    size_t renderQueuePeak = 0;

    /** @brief 队列 A 里被清空的编码帧，加上发现满时放弃的当前帧 */
    uint64_t decodeQueueDropped = 0;

    /** @brief 队列 B 被 forcePush() 淘汰的已解码裸帧数 */
    uint64_t renderQueueDropped = 0;

    /** @brief 队列 A 满而调用 dropUntilKeyFrame() 的次数；一次意味着最长一个 GOP 的冻结 */
    uint64_t decodeResyncs = 0;

    /**
     * @brief 被**丢包注入器**扔掉的包数 (M4.0)
     *
     * @note 必须和 assembler.packetsLost() 分开报, 理由和 lost / malformed 分开
     *          是同一条: 前者是**我们自己造的**, 后者是网络造的。混成一个数,
     *          "这次丢包率高"就分不清是网络差还是注入器开着 ——
     *          而注入器开着是最容易忘的一件事。
     *
     * @note 这个数和 packetsLost() 在稳态下应当**接近但不相等**:
     *          注入器丢的包 assembler 从来没见过, 所以它会体现在 seq 缺口里,
     *          被 packetsLost() 算进去。两者对不上就说明还有别的地方在丢包。
     */
    uint64_t injectedDrops = 0;

    uint64_t elapsedMs = 0;
};

/**
 * receiver 收包管线。
 *
 * ```text
 * [UdpSocket::recvFrom] --> [FrameAssembler::offer/pop] --> [dump 文件]
 * ```
 *
 * M3.5 起变成三条线程, 拆分点就在 M2 注释里预告的 pop() 之后:
 *
 * ```text
 * [recvLoop]                            [decodeLoop]          [renderLoop]
 *  recvFrom                              pop 队列A             pumpEvents()
 *  FrameAssembler::offer/pop             Decoder::decode       popFor(队列B, 5ms)
 *  JitterBuffer::push/pop  --队列A-->    RawFrame  --队列B-->  IRenderer::renderFrame
 *  (可选)dump 落盘                                             延迟采样
 * ```
 *
 * @note **run() 内部起三条线程, 自己只负责 join。** 渲染没有跑在调用者的线程上,
 *          所以 run() 从哪条线程调都行 —— 现有测试从 std::async 里调它, 一行不用改。
 *          代价是**显式放弃 macOS**: SDL 的硬规则只要求"碰 SDL 的每一行在同一条线程",
 *          "而且是主线程"那条是 extra safety 的建议, 但在 macOS 上是操作系统级强制。
 *          这是一次有记录的取舍, 不是忘了考虑。
 * @note **碰 SDL 的每一行都在渲染线程**: createRenderer() 只是 make_unique 不碰 SDL,
 *          所以放在 open() 里(能早失败); 但 renderer_->open() 和 renderer_->close()
 *          必须是 renderLoop 的头尾两件事 —— Destroy* / SDL_Quit 也在硬规则覆盖范围内。
 *          renderer_ 的析构仍然跑在主线程, 但那时 close() 已经调过, 三个指针都是
 *          nullptr 且 SDL_WasInit(0) 为 0, 是个彻底的空操作。
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
     * @return Ok      正常退出(含空闲超时、收满帧数、用户关窗口、Ctrl-C)
     *  Closed  未调用 open() 或 open() 失败
     *  其它    三条线程里第一个**真正的**故障, 按上游优先(收包 > 解码 > 渲染)挑
     *
     * @note 单个畸形包/收包错误**不终止**循环, 只计数。UDP 上收到垃圾是常态,
     *          为一个坏包退出整个接收端, 等于把对端的 bug 变成自己的可用性问题。
     * @note **收尾导致的失败一律写 Ok**(NOTES D19)。close() 之后 push() 返回 false
     *          是正常收尾的一部分不是故障 —— 写成错误的话, 每次正常关窗口退出码都是 1,
     *          而且这个假错误会按"上游优先"**盖掉**下游真正的故障原因。
     * @note 这个参数是 const 引用, 管线**写不了**它。窗口关闭是从管线内部产生的
     *          第四个触发源, 借不到这个变量往外传, 所以内部另有一个 stopping_ 标志,
     *          三条循环判两者的并集(NOTES D17)。
     */
    Status run(const std::atomic<bool>& stopRequested);

    /**
     * @brief 实际绑定到的端口
     *
     * @return open() 成功后为内核分配的端口; 之前为 0
     */
    uint16_t boundPort() const;

    const ReceiverPipelineStats& stats() const { return stats_; }

    /**
     * @brief M4.1 反向通道认定的对端; port 为 0 表示还没见过任何合法 DATA 包
     *
     * @note 和 stats() 一样, **只在 run() 返回之后读才安全**: 运行期间 peer_ 由
     *          收包线程持续改写, 那时读它是数据竞争(UB, 不是"读到旧值")。
     */
    const Endpoint& peer() const { return peer_; }

private:
    /** @brief 校验配置; 不碰任何资源 */
    Status validateConfig() const;

    /** @brief 把一帧写进 dump 并更新统计 */
    Status writeFrame(const AssembledFrame& frame);

    /** @brief 关闭 socket 和文件; 必须幂等 */
    void closeResources();

    /**
     * @brief 收包 -> 组包 -> jitter buffer -> 队列 A
     *
     * @note M2 那个单线程 run() 的循环体基本原样搬过来, 唯一的结构改动是
     *          原来 pop() 出来直接 writeFrame(), 现在还要 push 进队列 A。
     * @note **recvFrom 的超时要被 jitter 的到期时间夹一下**:
     *          这条线程现在还兼职"到点了把帧放出去", 傻等满 recvTimeoutMs 的话,
     *          一帧最坏被多压 200ms —— --jitter-ms=50 设了等于没设, 而且现象是
     *          延迟数字比设定值大得离谱且忽大忽小, 很容易怀疑到解码器头上去。
     *          JitterBuffer::msUntilNextDue() 就是为这件事存在的。
     */
    void recvLoop(const std::atomic<bool>& stopRequested);

    /**
     * @brief M4.0: 这个刚收到的包该不该被注入器"丢掉"
     *
     * @param packet 刚从 recvFrom 拿到的整包(含包头)
     * @param len    实际收到的字节数
     *
     * @return true 表示调用方应当**当作没收到**: 直接 continue, 不要 offer 给组包器
     *
     * @note 注入器要 seq, 所以这里得先解一次包头 —— offer() 里还会再解一次。
     *          多解一次是纳秒级的, 而且 lossInjectionEnabled() 为 false 时这个函数
     *          第一行就返回, 关掉注入时开销是零。别为了省这一次解析把注入逻辑
     *          塞进 FrameAssembler —— 那会把测试工具焊进业务模块。
     *
     * @note **解包头失败的包必须返回 false**(照常交给 offer)。畸形包要由组包器
     *          计进 packetsMalformed; 在这一层就丢掉的话, 测试工具会把畸形包统计
     *          吃掉, 而那正是排查"对端在乱发还是版本不匹配"的唯一线索。
     *
     * @note 非 DATA 包(将来的 FEC)也要按 seq 判丢 —— FEC 包不能丢的话,
     *          "FEC 恢复率"这个数就没有意义了。seq 在通用头里, 拿得到。
     */
    bool shouldInjectDrop(const uint8_t* packet, size_t len);

    /**
     * @brief M4.1: 记住"对端是谁", 供反向通道(NACK / PLI)回发
     *
     * @param packet 刚从 recvFrom 拿到的整包
     * @param len    实际收到的字节数
     * @param from   这个数据报的来源地址
     *
     * @note **每收到一个合法 DATA 包就无条件更新**。UDP 无连接, 绑在同一个端口上
     *          谁都能发, 所以"对端"不是一次确定的常量, 而是一个持续跟随的量。
     *
     * @note 为什么不"锁定第一个不再改": 发送端每次启动的源端口都由内核随机分配
     *          (UdpSocket::open() 不 bind), 而"接收端挂着、反复跑 sender"是调参的
     *          标准工作流。锁死的话第二次开始 NACK 全发到一个死端口 ——
     *          而 sendTo 到没人听的端口**不报错**(UDP 无连接), 统计里一点痕迹都没有。
     *
     * @note 为什么野包引不走: 640x480@2000kbps@30fps 实测约 233 包/秒(7.75 包/帧),
     *          真流每约 4ms 就把这个值冲刷一遍,
     *          劫持窗口只有一个包的间隔。M4 的威胁模型里没有攻击者 —— 存心的人
     *          直接伪造 DATA 包让画面花掉更省事, 那要靠 M5/M6 的鉴权解决, 不是这里。
     *
     * @note 闸门定在"合法 DATA 包": 反正取重传位就要解 DataHeader, 几乎白送;
     *          而且它天然排除 FEC/NACK 包 —— **"对端"的定义是"给我发媒体数据的那个人"**。
     */
    void trackPeer(const uint8_t* packet, size_t len, const Endpoint& from);

    /** @brief 队列 A -> Decoder -> 队列 B */
    void decodeLoop(const std::atomic<bool>& stopRequested);

    /**
     * @brief 队列 B -> 渲染; 兼管事件泵、延迟采样、统计行打印
     *
     * @note 头尾两件事是 renderer_->open() 和 renderer_->close() —— 碰 SDL 的
     *          每一行都必须在这条线程上, 见类注释。
     * @note open() 失败时(比如没有 DISPLAY)不能只是自己退出: 那时 ReceiverPipeline::open()
     *          早就返回 Ok 了, 另外两条线程还在跑。要写 renderStatus_ 再调 requestStop()。
     */
    void renderLoop(const std::atomic<bool>& stopRequested);

    /**
     * @brief 发起收尾: 置标志 + 唤醒所有阻塞点
     *
     * Ctrl-C、窗口关闭和真正故障共用这一个函数。maxFrames 收满、idleTimeout 空闲
     * 则是正常 EOF：收包线程先排空 jitter，再关闭队列 A，让解码/渲染顺序排空。
     *
     * @note **三件事必须一起做**, 少一条就有线程醒不过来, run() 的 join() 永远返回不了:
     *          stopping_ 让还在循环里的线程下一轮退出;
     *          close(队列A) 唤醒堵在 pop(A) 的解码线程和堵在 push(A) 的收包线程;
     *          close(队列B) 唤醒堵在 push(B) 的解码线程。
     *          标志说"该走了", close() 让它**有机会听见**(NOTES D17)。
     * @note 任何线程都能调, 调几次都行: atomic 的写天然安全, BoundedQueue::close()
     *          内部加锁且幂等。不需要额外互斥。
     * @note **不能由 run() 那条线程统一协调** —— 它已经堵在 join() 上了。
     *          收尾只能由三条工作线程自己发起。
     */
    void requestStop();

    /** @brief stopRequested(外部) 和 stopping_(内部) 的并集 */
    bool shouldStop(const std::atomic<bool>& stopRequested) const;

    /**
     * @brief 把本线程负责的那几个计数器拷进 shared_
     *
     * @note 各个 stats 结构体仍然被各自的线程独占, 本来就没有竞争 ——
     *          需要同步的只有这次快照拷贝。所以三个模块一行都不用改(NOTES D18)。
     * @note 调用频率自己定: 每包一次约 750 次/秒(约 15 微秒/秒, 可忽略),
     *          嫌多就每 100ms 一次 —— 统计行一秒才打, 100ms 的陈旧度肉眼看不出来。
     */
    void publishRecvStats();
    void publishDecodeStats();

    /** @brief 读一份一致快照; 打统计行和 run() 收尾都用它 */
    ReceiverPipelineStats snapshotStats() const;

    ReceiverPipelineConfig config_;
    UdpSocket socket_;
    FrameAssembler assembler_;
    JitterBuffer jitter_;
    Decoder decoder_;

    /**
     * @brief 渲染器; 在 open() 里构造, 但 open/close 由 renderLoop 调
     *
     * @note renderKind 为空时是 nullptr —— 那种模式下连渲染线程都不起。
     */
    std::unique_ptr<IRenderer> renderer_;

    /** @brief 组包+定时放行 -> 解码; 元素是 move-only 的 unique_ptr */
    BoundedQueue<std::unique_ptr<AssembledFrame>> queueA_;

    /** @brief 解码 -> 渲染 */
    BoundedQueue<std::unique_ptr<RawFrame>> queueB_;

    /** @brief 整场累积, 退出时报总账; 只被渲染线程碰 */
    LatencyRecorder latencyTotal_;

    /**
     * @brief 最近一个统计间隔的样本, 每打完一行就 reset()
     *
     * @note 和 latencyTotal_ 回答的是**两个不同的问题**: 整场汇总说"这次跑下来
     *          多少延迟", 滑动窗口说"**现在**是不是正在变坏"。整场数字跑得越久对
     *          新变化越迟钝(后面的坏样本被前面海量的好样本稀释), 而 M4 要注入丢包,
     *          那就是有意制造"前后两段不一样"(NOTES D15 的同族问题)。
     */
    LatencyRecorder latencyWindow_;

    /**
     * @brief 每条线程各自的结果; join() 之后由 run() 按上游优先挑一个返回
     *
     * @note 不需要加锁也不用 atomic: **join() 本身提供 happens-before** ——
     *          被 join 的线程结束前写的东西, join 返回之后读得到。
     */
    Status recvStatus_;
    Status decodeStatus_;
    Status renderStatus_;

    /** @brief 内部停止标志; 见 run() 的 @note 和 requestStop() */
    std::atomic<bool> stopping_{false};

    /** @brief 只保护 shared_; 各模块自己的 stats 仍由各自线程独占 */
    mutable std::mutex statsMu_;

    /** @brief 跨线程可见的统计快照 */
    ReceiverPipelineStats shared_;

    /** @brief queueA_.tryPush 满时当前这帧已被按值传入、无法由队列自身计数 */
    std::atomic<uint64_t> decodeQueueRejected_{0};

    /** @brief 队列 A 保险丝触发次数；收包线程写、渲染线程读取统计快照 */
    std::atomic<uint64_t> decodeResyncs_{0};

    /**
     * @brief M4.0 注入器扔掉的包数；收包线程独占写
     *
     * @note 用 atomic 是为了跟着上面两个走同一条发布路径(publishRecvStats 拷进 shared_),
     *          不是因为有竞争 —— 只有收包线程写它。
     */
    std::atomic<uint64_t> injectedDrops_{0};

    /** @brief 复用的收包缓冲, 每次 recvFrom 都新建一个 vector 是纯浪费 */
    std::vector<uint8_t> recvBuf_;

    /**
     * @brief M4.1 反向通道的目的地: 最后一个发来合法 DATA 包的源地址
     *
     * @note port 为 0 表示**还没见过任何对端**, 此时不能发 NACK/PLI ——
     *          `Endpoint{"", 0}` 传给 sendTo 会被它的参数校验挡下来返回 InvalidArg,
     *          但那是"本端调用错误"的语义, 不该在正常启动阶段出现。调用前先判。
     *
     * @note **不需要加锁**: socket 归 recvLoop 独占, 缺口也由它发现, NACK 也由它发出 ——
     *          写和读都在同一条线程上。这是"收发共用一个 socket"直接带来的简化,
     *          用两个 socket 反而要考虑谁来写这个变量。
     */
    Endpoint peer_;

    std::ofstream h264File_;
    ReceiverPipelineStats stats_;
    bool opened_ = false;
    bool hasRun_ = false;
};
