/**
 * @file    JitterBuffer.h
 * @brief   接收端抖动缓冲: 按帧号排序、按时间放行、从关键帧起播
 * @author  zzj
 * @date    2026-08-18
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "modules/transport/FrameAssembler.h"

/**
 * 抖动缓冲的配置。
 */
struct JitterBufferConfig {
    /**
     * @brief 目标缓冲水位(毫秒)
     *
     * @note **0 不等于"关掉这一层"**, 而是"水位为零"。水位就是能容忍多大的乱序和抖动:
     *          设成 0 之后, 任何比前一帧晚到的帧都会被判为过期丢弃, 一乱序就掉帧。
     *          它的用途是**测基线** —— 量出"不缓冲能做到多低", 好知道水位到底花了多少钱。
     */
    int targetDelayMs = 50;

    /**
     * @brief 缓冲里攒到几帧就开始提前放行
     *
     * @note 攒这么多说明消费端跟不上或者时钟映射漂了。此时最老的那帧**立刻放出去**
     *          而不是丢掉: 它已经在手里了, 丢必然花屏, 播早了至少画面是对的。
     */
    size_t maxFrames = 16;

    /**
     * @brief 硬上限, 超过就真的丢最老的帧
     *
     * @note 为 0 表示取 maxFrames * 2。有软上限还要硬上限, 是因为软上限只改"什么时候该放",
     *          并不减少条目数 —— 消费端要是彻底卡住了, 光标记到期不会让内存停止增长。
     *          到了这一步, 那些帧早就过了该显示的时刻, 留着只是在给延迟做加法。
     */
    size_t hardLimitFrames = 0;

    /**
     * @brief 第一帧必须是关键帧
     *
     * @note 解码器拿到的第一个包不是 IDR, 会一路吐 `no frame!` / `non-existing PPS`,
     *          画面全绿或全花直到下一个 IDR。这条策略放在这里, M4 的 PLI 才有地方接。
     */
    bool startOnKeyFrame = true;
};

/**
 * 抖动缓冲的计数器。
 *
 * @note 丢帧的三种原因分开计: **太晚**说明网络抖动超过了水位(该调大 targetDelayMs),
 *          **溢出**说明消费端跟不上(该查解码耗时), **起播前丢弃**是正常的一次性代价。
 *          混成一个"丢帧数", 就分不清该调参数还是该查性能。
 */
struct JitterBufferStats {
    /** @brief push() 进来的帧数 */
    uint64_t framesIn = 0;

    /** @brief pop() 出去的帧数 */
    uint64_t framesOut = 0;

    /** @brief 比已经放出去的帧还老, 直接丢 */
    uint64_t framesTooLate = 0;

    /** @brief 同一个 frameId 重复到达 */
    uint64_t framesDuplicate = 0;

    /** @brief 因超过 maxFrames 被改成"立刻到期"的帧数(**没丢**, 只是播早了) */
    uint64_t framesForcedEarly = 0;

    /** @brief 因超过 hardLimitFrames 被真正丢弃的帧数 */
    uint64_t framesDropped = 0;

    /** @brief 起播前丢弃的非关键帧数 */
    uint64_t framesBeforeKey = 0;
};

/**
 * 抖动缓冲 —— 乱序进, 有序且按时出。
 *
 * `FrameAssembler` 只回答"这一帧的分片齐了没有", **明确不排序**(见它的职责边界注释)。
 * 剩下两件事归这里:
 * - **排序**: 按帧号交付。喂进解码器的顺序错了, 它不会报错, 只会吐出看着像画面的垃圾 ——
 *   这类 bug 没有任何错误信息, 只能靠肉眼, 是最难查的一种。
 * - **按时放行**: 早到的帧多等一会儿, 晚到的帧少等, 把网络抖动吸收在水位里。
 *
 * 用法(收包线程独占):
 * @code
 * jitter.push(std::move(frame), steadyNowMs());
 *
 * AssembledFrame due;
 * while (jitter.pop(due, steadyNowMs())) {
 *     frameQueue.push(std::make_unique<AssembledFrame>(std::move(due)));
 * }
 *
 * // 下一次 recvFrom 的超时要被下一帧的到期时间夹住, 否则水位形同虚设, 见 msUntilNextDue()
 * @endcode
 *
 * @note 不是线程安全的, 由收包线程独占。**这是刻意的**: 项目定死了 `BoundedQueue`
 *          是线程间唯一的数据通道。挂在收包线程上它就被单线程独占, 一把锁都不用加;
 *          挂在解码线程则要么再插一个队列(jitter 的时序就被前一级的排队污染了),
 *          要么给它上锁 —— 两条路都更差。
 */
class JitterBuffer {
public:
    explicit JitterBuffer(JitterBufferConfig config = {});

    /**
     * @brief 放入一帧刚组好的帧
     *
     * @param frame 组好的帧, 移动进来
     * @param nowMs 当前单调时刻(steadyNowMs())
     *
     * @note **不返回 Status**: 丢弃在这里是**策略**不是错误。太晚了、重复了、还没等到
     *          第一个关键帧 —— 每一种都是设计好的行为, 计数器会说明发生了什么。
     *          返回错误码只会诱使调用方写出"丢一帧就中止接收"的代码。
     * @note nowMs 由调用方传入而不是内部取: 一次收包循环里 push/pop/msUntilNextDue
     *          必须用**同一个时刻**, 否则三者互相矛盾(pop 说没到点, msUntilNextDue 说
     *          已经过期)。顺带让时间成为可注入的量, 单测才能不靠 sleep 验时序。
     */
    void push(AssembledFrame frame, uint64_t nowMs);

    /**
     * @brief 取出一帧到点该播的帧
     *
     * @param out   出参, 移动赋值进来
     * @param nowMs 当前单调时刻
     *
     * @return true 取到一帧; false 现在没有该播的帧
     *
     * @note 用 while 循环取干净: 消费端卡顿之后可能一次到期好几帧。
     * @note 交付顺序**严格递增**, 绝不回头。已经放出去帧 5 之后再来的帧 4 会被丢弃 ——
     *          解码器收到倒退的帧比收不到更糟。
     */
    bool pop(AssembledFrame& out, uint64_t nowMs);

    /**
     * @brief 距离下一帧到期还有多久(毫秒)
     *
     * @param nowMs 当前单调时刻
     *
     * @return 缓冲为空返回 -1; 已经到期返回 0; 否则返回剩余毫秒数
     *
     * @note **收包线程的 recvFrom 超时必须拿它夹一下**:
     *          @code
     *          int timeoutMs = config_.recvTimeoutMs;               // 200
     *          const int untilDue = jitter_.msUntilNextDue(nowMs);
     *          if (untilDue >= 0) timeoutMs = std::min(timeoutMs, untilDue);
     *          @endcode
     *          不夹的话, 一帧最坏要多压满一个 recvTimeoutMs 才被放行, `--jitter-ms=50`
     *          设了等于没设。现象是延迟比设定值大得离谱且忽大忽小, 很容易怀疑到解码器头上。
     */
    int msUntilNextDue(uint64_t nowMs) const;

    /** @brief 当前缓冲的帧数 */
    size_t size() const { return pending_.size(); }

    const JitterBufferStats& stats() const { return stats_; }

    /**
     * @brief 清空全部状态(含计数器和时钟映射), 回到刚构造的样子
     *
     * @note **仅用于重连** —— 对端换了、流从头开始, 所有历史都无效。
     * @note 下游丢帧 / 帧队列满**不要用这个**, 用 dropUntilKeyFrame()。那个场景下流没变,
     *          清掉时钟映射会让每帧多等(基准锚在拥塞时的样本上), 清掉交付水位会让还在飞的
     *          老关键帧把播放往回倒带, 而只清帧号锚点不清交付水位会在回绕时永久卡死。
     *          三者的推演见 NOTES.md D12。
     */
    void reset();

    /**
     * @brief 丢弃当前缓冲, 并要求从下一个关键帧重新起播
     *
     * 调用之后保证: **下一个交付出去的帧一定是关键帧**。除此之外一切不变 —— 还是同一条流,
     * 帧号编号连续, 时钟映射保留, 计数器继续累加。
     *
     * @note 用在**下游丢了帧**的时候(接收端帧队列满、解码器跟不上)。此时流没变, 只是我们
     *          暂时消化不动了 —— 所以**只清 pending_ 和起播门**, 帧号锚点 / 时钟映射 /
     *          交付水位 / 计数器都必须留着。逐条推演见 NOTES.md D12。
     * @note 起播门必须清: 被丢掉的那些帧**已经离开本类了**, 是在下游丢的。解码器解到帧 496,
     *          497-500 在下游队列里被清掉, 接下来帧 501 引用 500 —— 解码器没见过它, 照样花屏。
     * @note **这个操作很贵**: 下一个关键帧平均要等半个 GOP(默认 gop=fps, 即 ~0.5 秒),
     *          期间画面冻结。它是**保险丝不是调节阀** —— 频繁触发说明解码线程被下游堵住了,
     *          或者解码器真跟不上, 那时该查的是那里, 不是往这儿加逻辑。
     * @note 由**收包线程**调用: 帧队列满是它发现的(它是生产者), 而本类也归它独占,
     *          所以是同线程调用, 不需要任何同步。
     */
    void dropUntilKeyFrame();

private:
    /**
     * @brief 把会回绕的 32 位帧号扩展成单调递增的 64 位序号
     *
     * @param frameId 线上来的帧号
     *
     * @return 扩展后的序号; 同一路流内单调可比
     *
     * @note 这是本类能用 `std::map` 排序的前提。**不能**拿 `seqNewerThan` 当比较器:
     *          它不是全序(回绕点上 a<b、b<c 推不出 a<c), 而有序容器要求严格弱序,
     *          违反了就是未定义行为 —— 表现为偶发的元素找不到、迭代顺序错乱。
     * @note 做法是 RTP 的 extended sequence number: 只在入口用一次有符号差值
     *          `int32_t(frameId - refFrameId_)` 累加到 64 位计数上, 之后所有比较
     *          都是普通的大小比较。前提同样是相邻两帧相距不超过 2^31。
     */
    uint64_t extendFrameId(uint32_t frameId);

    /**
     * @brief 算一帧的应播时刻
     *
     * @param timestampMs 帧携带的发送端时间戳
     * @param nowMs       该帧的到达时刻
     *
     * @return 本地时间轴上的应播时刻(毫秒)
     *
     * @note 做法是在**发送端时间轴**和**本地时间轴**之间维护一个偏移量:
     *          @code
     *          offset      = nowMs - timestampMs        // 本帧观测到的偏移
     *          minOffset_  = min(minOffset_, offset)    // 取历史最小 = 走得最快的那一趟
     *          playAtMs    = timestampMs + minOffset_ + targetDelayMs
     *          @endcode
     *          早到的帧算出来的 playAt 更靠后, 于是多等; 晚到的帧少等甚至立刻到期 ——
     *          抖动被吸收在水位里, 这才是 "jitter buffer" 这个名字的意思。
     *
     * @note **错误做法是 `playAt = 到达时刻 + targetDelayMs`**: 那只是给每一帧统一
     *          加了个延迟, 抖动一点没被吸收, 早到的照样早播、晚到的照样晚播。
     *
     * @note 取**最小**偏移: 要估的是这条链路的**下限** C + T_min —— C 是两端时钟起点之差
     *          (常数, 未知, 但对每一帧都相同), T 是本帧的传输耗时(每帧不同, 这就是抖动)。
     *          offset = now - ts = C + T, 取历史最小即得到 C + T_min, 对应"网络最顺的那一趟"。
     *
     *          锚错基准有**两种相反**的失效模式, 别搞混:
     *          - 基准**偏晚**(锚在慢样本上, 例如只用第一帧而它恰好走得慢): playAt 整体后移,
     *            之后每帧多等 (T_slow - T_min) 毫秒, **实际水位比 targetDelayMs 大**,
     *            现象是端到端延迟稳定地高于设定值。
     *          - 基准**偏早**(锚在异常快的样本上): playAt 整体前移, 帧一到手就已经过期,
     *            **水位形同虚设**, 现象是抖动完全没被吸收, 输出节奏跟着网络一起抖。
     *
     *          取 min 天然免疫第一种, 但对第二种毫无抵抗 —— 见下面那条已知限制。
     *
     * @note 偏移量只参与**差值**运算, 所以跨机器时钟起点不同也不影响排期(只是
     *          算出来的绝对延迟数字没有意义, 见 Clock.h)。
     *
     * @note 已知限制: minOffset_ 只减不增, 网络永久变好之后水位不会自动收窄;
     *          时钟频差也会让映射慢慢漂。两者都归 M4 的自适应水位处理。
     */
    uint64_t computePlayAt(uint32_t timestampMs, uint64_t nowMs);

    /** @brief 超过软/硬上限时的处置; 在 push 插入之后调用 */
    void enforceCapacity();

    struct Entry {
        AssembledFrame frame;

        /** @brief 本地时间轴上的应播时刻; 被提前放行时会被改成 0 */
        uint64_t playAtMs = 0;
    };

    JitterBufferConfig config_;

    /**
     * @brief 待播帧, 按扩展帧号排序
     *
     * @note 用 map 而不是优先队列: 条目最多十几个, 红黑树的常数完全无所谓,
     *          换来的是"取最小"和"查重"都现成。而且 pop 要的是**最小**元素,
     *          恰好是 begin()。
     */
    std::map<uint64_t, Entry> pending_;

    // ---- 帧号扩展的锚点 ----
    uint32_t refFrameId_ = 0;
    uint64_t refExtended_ = 0;
    bool hasRef_ = false;

    // ---- 时钟映射 ----
    int64_t minOffsetMs_ = 0;
    bool hasOffset_ = false;

    // ---- 交付水位 ----
    uint64_t lastOutExtended_ = 0;
    bool hasOutput_ = false;

    /** @brief 是否已经等到第一个关键帧(startOnKeyFrame 为 false 时构造即为 true) */
    bool started_ = false;

    JitterBufferStats stats_;
};
