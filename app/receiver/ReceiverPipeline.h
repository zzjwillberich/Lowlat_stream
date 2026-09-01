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
#include <deque>
#include <fstream>
#include <map>
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
#include "modules/transport/FecDecoder.h"
#include "modules/transport/LossInjector.h"
#include "modules/transport/NackTracker.h"
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

    /** @brief jitter buffer 的整套配置; 命令行的 --jitter-ms 落在 delay.targetDelayMs 上 */
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

    /**
     * @brief 缺口跟踪与重传请求 (M4.1); windowPackets 为 0 表示**不发 NACK**
     *
     * @note 关掉时连 NackTracker 都不喂 —— 否则每个包多一次 map 操作, 换来一堆
     *          没人看的统计。"关掉 = 那一级完全不存在", 同 renderKind 为空。
     */
    NackTrackerConfig nack;

    /**
     * @brief FEC 解码 (M4.2); recentPackets 为 0 表示**不解 FEC**
     *
     * @note 关掉时连最近包缓冲都不该维护 —— 每个 DATA 包都要往里拷一份,
     *          不用的话是纯浪费。"关掉 = 那一级完全不存在", 同 renderKind 为空。
     */
    FecDecoderConfig fec;

    /**
     * @brief 两次 PLI 之间的最小间隔(毫秒); 0 表示**不发 PLI** (M4.4)
     *
     * @note **限流不是可选项, 是 PLI 唯一真正危险的地方。** 一个 IDR 约 25 片,
     *          10% 丢包下完整到达的概率只有 0.9^25 = 7.2%:
     *
     *          ```text
     *          关键帧丢了 -> PLI -> IDR(25 片) -> 92.8% 又收不齐 -> 再 PLI -> ...
     *          ```
     *
     *          每一轮都往已经拥堵的网络里再灌 25 个包, 而 IDR 比 P 帧大一个数量级 ——
     *          丢包越严重, PLI 越频繁, 网络越差。这是自我强化的死循环。
     *          RTP 那边的惯例是 1 秒, 这里取同一个量级。
     *
     * @note 两个触发源(保险丝烧了 / NACK 放弃了关键帧分片)**共用同一个限流器** ——
     *          否则一次拥塞可能同时触发两边, 一下发出两个 PLI。
     */
    int pliMinIntervalMs = 1000;
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

    /**
     * @brief 水位估计器的瞬时状态 (M4.3)
     *
     * @note 这一组和它上面所有计数器**性质不同**: 上面的是累计量, 做差有意义;
     *          这里的 currentDelayMs / rawDelayMs / floorOffsetMs 是**瞬时量**,
     *          做差没有意义。周期统计里它们要按"当前值"打印, 不是按增量。
     */
    DelayEstimatorStats delay;

    /**
     * @brief 实测重传往返(毫秒); 0 表示这一趟一次都没量到 (M4.6)
     *
     * @note 量法: 记下每个 seq 是什么时候被 NACK 请求的, 它的重传包
     *          (FLAG_RETRANSMIT)回来时做差。**这是整条链路唯一的 RTT 数字** ——
     *          M4.5 第四轮才发现"重传一直在成功、只是回来得太晚",
     *          而当时手上没有任何 RTT 的量, 只能靠 netem 的配置值反推。
     *
     * @note 它同时是水位下限的输入(见 DelayEstimator::setRttMs)。
     */
    int rttMs = 0;

    /** @brief 量到的 RTT 样本数; 为 0 时上面那个数没有意义 */
    uint64_t rttSamples = 0;

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

    /** @brief 缺口跟踪器的计数器快照 (M4.1) */
    NackTrackerStats nack;

    /**
     * @brief FEC 解码器的计数器快照 (M4.2)
     *
     * @note `groupsRecovered / (groupsRecovered + groupsUnrecoverable)` 是 FEC 的
     *          **实际恢复率** —— M4.2 唯一能被验收的数字。但**回环上它没有意义**:
     *          NACK 已经把能救的都救了(实测 30% 丢包下 recovered=656/656), FEC 的
     *          边际价值要等 RTT 大到重传赶不上 playAt 才体现得出来。见 [[D24]]。
     *          能测的是另外两个: NACK 请求数应当**下降**, 发出的包数应当上升 1/K。
     */
    FecDecoderStats fec;

    /** @brief 实际发出去的 NACK **包**数; 一个包可以请求几百个 seq */
    uint64_t nackPacketsSent = 0;

    /**
     * @brief 发 NACK 时 sendTo 失败的次数
     *
     * @note 单独一个字段是因为**它几乎永远是 0** —— UDP 发到没人听的端口都算成功,
     *          所以这个数非 0 意味着 socket 本身出问题了, 和"重传没成功"是两回事。
     *          没有它的话, "NACK 一条都没生效"会有两种原因而你分不清。
     */
    uint64_t nackSendErrors = 0;

    /** @brief 实际发出去的 PLI 包数 (M4.4) */
    uint64_t pliSent = 0;

    /**
     * @brief 触发了但被最小间隔挡下的 PLI 次数
     *
     * @note 单独一个字段, 因为它回答的是"限流有没有在起作用"。它远大于 pliSent
     *          说明触发源太敏感或者网络已经烂到 IDR 根本收不齐 —— 那时再发也没用。
     *          没有这个数的话, "PLI 只发了 2 个"有两种意思: 只触发了 2 次, 还是
     *          触发了 200 次被挡掉 198 次。
     */
    uint64_t pliSuppressed = 0;

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

    /**
     * @brief M4.1: 把这个包喂给缺口跟踪器, 并把该请求重传的 seq 发回对端
     *
     * @param packet 刚收到的整包(已通过注入器)
     * @param len    字节数
     *
     * @note 关掉时(config_.nack.windowPackets == 0)第一行就返回, 零开销。
     *
     * @note 只喂**合法 DATA 包**: 跟踪器要的是 seq 和 fragCount, 后者只有 DATA 包才有。
     *          将来的 FEC 包虽然也占 seq 空间, 但它丢了不该请求重传 ——
     *          FEC 本来就是"丢了也不要紧"的那一份冗余。
     *
     * @note 发 NACK 之前必须确认 `peer_.port != 0`。没见过对端就发, sendTo 会因为
     *          Endpoint{"", 0} 返回 InvalidArg —— 那是"本端调用错误"的语义,
     *          不该在正常启动阶段出现。
     */
    void requestRetransmissions(const uint8_t* packet, size_t len);

    /**
     * @brief M4.4: 请求对端立刻编一个关键帧
     *
     * @param nowMs 当前 steady 时刻, 用于最小间隔限流
     *
     * @return true 真的发出去了; false 被限流挡下或者还没见过对端
     *
     * @note 两个触发源共用这一个函数, 因此也共用同一个限流器:
     *          - **保险丝烧了**: queueA 满 -> dropUntilKeyFrame() -> 画面已经冻结,
     *            正在等下一个 IDR, 主动要一个能把冻结时间从"最长一个 GOP"缩短;
     *          - **NACK 放弃了关键帧分片**: nackTracker_ 的 keyFramesGivenUp 涨了 ——
     *            重传已经尽力还是没救回来, 那一帧永远拼不齐, 后面依赖它的帧全会花。
     *
     * @note PLI 包**没有载荷**, 就是一个 type = Pli 的通用头。seq 用 nackSeq_ 同一个
     *          计数器 —— 它俩都是本端往反向通道上发的包, 各数各的没有意义。
     *
     * @note **PLI 是第三道兜底, 不该是第一反应。** FEC 和 NACK 已经在保护 IDR 了
     *          (实测只开 NACK 时 10% 丢包下 rendered=299/300)。触发源写得太敏感,
     *          换来的是 IDR 风暴而不是更快恢复。
     */
    bool requestKeyFrame(uint64_t nowMs);

    /**
     * @brief 记下这一批 seq 的重传请求时刻, 供 RTT 计算 (M4.6)
     *
     * @param seqs  本次 NACK 里带的 seq
     * @param nowMs 发出时刻
     *
     * @note 只在**真的发出去之后**调用 —— sendTo 失败的请求不该等回包。
     */
    void recordNackSent(const std::vector<uint32_t>& seqs, uint64_t nowMs);

    /**
     * @brief 一个重传包回来了, 试着量一次 RTT (M4.6)
     *
     * @param packet 完整的包
     * @param len    长度
     * @param nowMs  到达时刻
     *
     * @note 只认带 FLAG_RETRANSMIT 的 DATA 包。普通包的 seq 也可能在表里
     *          (请求发出去的同时原包正好到了), 那不是重传, 算进去会把 RTT 低估成 0。
     */
    void observeRetransmit(const uint8_t* packet, size_t len, uint64_t nowMs);

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

    /** @brief M4.1 缺口跟踪器; 收包线程独占, 不需要加锁 */
    NackTracker nackTracker_;

    /** @brief M4.2 冗余解码器; 同样是收包线程独占 */
    FecDecoder fecDecoder_;

    /** @brief 恢复出来的包缓冲, 复用 */
    PacketBuffer recoveredBuf_;

    /** @brief 复用的 NACK 发送缓冲和 seq 列表, 同 recvBuf_ 的理由 */
    std::vector<uint32_t> nackTargets_;
    std::vector<uint8_t> nackBuf_;

    /**
     * @brief 本端发出的 NACK 用的 seq 计数器
     *
     * @note **和 DATA 的 seq 空间无关**, 两个方向各数各的 —— 反向通道的包
     *          不该占用媒体流的序号, 否则发送端的丢包统计会被自己发的 NACK 搅乱。
     */
    uint32_t nackSeq_ = 0;

    /** @brief M4.1 统计, 收包线程独占写 */
    std::atomic<uint64_t> nackPacketsSent_{0};
    std::atomic<uint64_t> nackSendErrors_{0};

    /** @brief M4.4 上一次真正发出 PLI 的时刻; 0 表示还没发过 */
    uint64_t lastPliMs_ = 0;

    /**
     * @brief M4.4 已经因为关键帧分片放弃而触发过 PLI 的次数
     *
     * @note 用来把 NackTrackerStats::keyFramesGivenUp 的**增量**变成边沿触发 ——
     *          那是个累计值, 直接判"非 0 就发"会每个包都发一次。
     */
    uint64_t seenKeyFramesGivenUp_ = 0;

    /**
     * @brief 已请求重传、还没等到回包的 seq -> 请求时刻 (M4.6)
     *
     * @note **必须有上限**: 请求出去的包可能永远回不来(这正是 givenUp 的含义),
     *          不清理就是安静地涨内存 —— 同 NackTrackerConfig::windowPackets
     *          和 FrameAssembler::maxPendingFrames 那两条。
     *          做法是超过 kRttPendingMax 就丢掉最老的。
     */
    std::map<uint32_t, uint64_t> nackSentAtMs_;

    /** @brief 最近若干个 RTT 样本(毫秒), 取中位数 */
    std::deque<int> rttSamples_;

    static constexpr size_t kRttPendingMax = 256;

    /**
     * @brief RTT 取几个样本的中位数
     *
     * @note 用中位数不用最小值: 这个数要去当水位的**下限**, 而下限的作用是
     *          "接得住重传"。取最小值等于按最顺的那一趟定预算, 大半的重传照样赶不上。
     *          也不取最大值 —— 一次超时重传就能把水位顶穿。
     */
    static constexpr size_t kRttWindow = 21;

    std::atomic<int> rttMs_{0};
    std::atomic<uint64_t> rttSampleCount_{0};

    /** @brief M4.4 统计, 收包线程独占写 */
    std::atomic<uint64_t> pliSent_{0};
    std::atomic<uint64_t> pliSuppressed_{0};

    std::ofstream h264File_;
    ReceiverPipelineStats stats_;
    bool opened_ = false;
    bool hasRun_ = false;
};
