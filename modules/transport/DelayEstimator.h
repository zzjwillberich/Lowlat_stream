/**
 * @file    DelayEstimator.h
 * @brief   从到达时刻估计抖动水位: 滑动窗口分位数 + 非对称跟随 (M4.3)
 * @author  zzj
 * @date    2026-09-01
 *
 * @note 职责边界: 本类**只回答"这一帧该在本地时间轴的哪一刻播"**。
 *          - FrameAssembler 回答"这一帧拼不拼得起来";
 *          - JitterBuffer   回答"排序、容量、起播门";
 *          - NackTracker    回答"缺了哪几个 seq";
 *          - **本类回答"等多久"**。
 *       它是 JitterBuffer 里 M3 那个 minOffset_ + 固定 targetDelayMs 的替代品,
 *       抽出来是因为水位估计是纯数字运算, 能脱离帧、脱离时钟单独测。
 *
 * @note 本类**不取时间**: nowMs 由调用方传进来。理由同 JitterBuffer ——
 *          一次收包循环里所有时间判断必须用同一个时刻, 而且单测不能靠 sleep。
 *
 * @note 这一层还的是 M2 留下的第二笔账(见 docs/M2_传输.md「留给 M4 的账」②):
 *          minOffset_ 只减不增, 网络永久变好之后水位不会自动收窄; 时钟频差
 *          也会让映射慢慢漂。滑动窗口把两个问题一起解决了 —— 见下面的 @note。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

/**
 * 水位估计的策略参数。
 *
 * @note 默认值都带推导, 改之前先看推导。凭感觉调这几个数会得到一个
 *          "看起来在自适应、其实在跟着噪声抖"的水位。
 */
struct DelayEstimatorConfig {
    /**
     * @brief 是否自适应; false 时行为**完全等同 M3**(固定水位 + 历史最小偏移)
     *
     * @note 默认 false 是刻意的, 和产品默认相反(receiver 的 --jitter-adapt 默认开)。
     *          理由: JitterBuffer 的绝大多数单测测的是排序 / 容量 / 起播门,
     *          它们不该因为水位会自己动而变得难以预期。**默认给最可预测的行为**,
     *          要自适应的显式打开。
     *
     * @note 这个开关同时是 M4.5 的 A/B 旋钮: 同一条链路上跑 adapt=0 和 adapt=1,
     *          比 framesTooLate 和端到端延迟, 才能说明这一层到底值不值。
     *          **没有这个对照组, "自适应"就只是一个形容词。**
     */
    bool adaptive = false;

    /**
     * @brief 初始水位(毫秒); 自适应模式下是冷启动值与样本不足时的兜底
     *
     * @note 非自适应模式下它就是全部 —— 恒定水位, 和 M3 一模一样。
     */
    int targetDelayMs = 50;

    /**
     * @brief 滑动窗口长度(毫秒)
     *
     * @note 这个数同时决定三件事, 调它之前把三条都算一遍:
     *          1. **上调响应时间** ≈ (100 - delayPercentile)% × windowMs。
     *             默认 5% × 10s = 500ms: 网络变差之后半秒内水位跟上。
     *          2. **样本量** = windowMs × fps / 1000。默认 30fps 下 300 个。
     *             低于 ~100 个时高分位数就是噪声放大器(见 NOTES: p99 of 100
     *             samples is ONE sample), 水位会跟着单帧抖动跳。
     *          3. **时钟频差的残留误差** = windowMs × 频差。消费级晶振 ±50ppm,
     *             10 秒窗口内只累积 0.5ms —— 这就是滑动窗口顺手把 M2 账②
     *             (时钟频差让映射慢慢漂)一起还掉的原因: 窗口每滑一次就重新对基准,
     *             漂移根本没有累积的机会。
     *
     * @note 想让上调更快只能缩窗口, 而缩窗口直接减样本量 —— 1 和 2 是**对冲的**,
     *          没有两头都要的设法。
     */
    uint32_t windowMs = 10000;

    /**
     * @brief 水位取窗口里第几分位(0-100)
     *
     * @note 它的含义就是**你愿意让百分之几的帧过期**: p95 @30fps = 每秒 1.5 帧
     *          judged too late。p99 = 每秒 0.3 帧, 但 300 个样本的 p99 是第 3 大,
     *          三个样本说了算 —— 稳定性差一个量级, 换来的只是每 3 秒少丢一帧。
     *
     * @note 100 不是"绝不丢帧": 它是"水位 = 窗口内见过的最大抖动", 一个尖峰
     *          能把水位钉高整整一个窗口的时间。
     */
    int delayPercentile = 95;

    /**
     * @brief 时钟基准取窗口里第几分位(0-100)
     *
     * @note **不用 min**。offset = nowMs - timestampMs, 而 timestampMs 是 32 位
     *          毫秒: 49.7 天回绕一次, 且和 seq 一样**每个取值都合法**(NOTES D22)。
     *          一次回绕、一个位翻转, 就能把 min 拉低 2^32 毫秒 ——
     *          于是 playAt 落在 49 天前, 每帧一到手就过期, **水位静默变成 0**。
     *          取 p5 要 5% 的样本一起坏才动得了, 于是不需要任何额外的剔除逻辑。
     *
     * @note 代价: 有 5% 的帧会算出比 floor 还早的到达, 它们的 d 是负数,
     *          等效于多等一点点。这个偏差恒等于"最快的 5% 之间的散布", 稳态下是几毫秒。
     */
    int floorPercentile = 5;

    /**
     * @brief 水位的**绝对**下限(毫秒); 实际下限还会被重传预算抬高
     *
     * @note 不设下限的话, 一条太顺的链路(本地回环: 抖动几乎为 0)会把水位收到 0,
     *          而水位 0 的含义是"任何比前一帧晚到的帧都丢" —— 一个抖动就掉帧。
     *
     * @note **这个数不再承担"接得住重传"的职责** —— 那由重传预算负责,
     *          见 setRttMs。M4.5 第一轮 10 这个值背了两条罪:
     *          低丢包时接不住重传往返(3%/5% 的成绩反而比 10% 差),
     *          以及在第四轮里成了坏平衡点的落脚处。两条都不是"绝对下限"该管的事。
     *
     * @note 实际生效的下限是 `max(minDelayMs, 帧周期 + RTT)`。
     *          两者哪个在起作用, 看 stats 的 floorFromBudget。
     */
    int minDelayMs = 5;

    /**
     * @brief 水位上限(毫秒)
     *
     * @note **必须有**。分位数抗得住少量脏样本, 但抗不住"脏样本占了前 5%" ——
     *          而 2^32 毫秒那种量级的坏值**保证**落在最大的 5% 里。
     *          没有上限, 一次 timestampMs 回绕就是几分钟的延迟。
     *
     * @note 500 的出处: NOTES「水位到底该设多少」—— 500ms 是点播 / HLS 的量级,
     *          WebRTC 这类实时系统跑 20~100ms。水位一上 500ms,
     *          README 第一行"低延迟"就不成立了, 所以这里是**产品定义的上限**,
     *          不是一个技术参数。
     */
    int maxDelayMs = 500;

    /**
     * @brief 水位下调的最大速率(毫秒/秒); 上调不限速
     *
     * @note 非对称是刻意的, 两个方向的代价完全不同:
     *          - **上调慢 = 掉帧**。水位没跟上, 帧到手就已经过期, 画面卡。
     *          - **下调快 = 画面加速**。水位收窄 X 毫秒, 等价于把接下来的帧
     *            整体提前 X 毫秒放出去 —— 观感是画面"抽"了一下。
     *          掉帧不可逆, 多等几秒才收回延迟只是慢一点, 所以只限下调。
     *
     * @note 10ms/s 的含义是播放速度快 1%。音频超过 ~1% 就能听出来, 视频宽容得多,
     *          但 1% 是个有出处的保守值。
     *
     * @note **这个限速只作用在水位上, 绝不能作用在 floorOffsetMs 上。**
     *          floor 装的是两端时钟差 —— 对端重启、NTP 跳变都会让它整体平移,
     *          限成 10ms/s 的话一次 300ms 的时钟跳变要 30 秒才对回来,
     *          期间每一帧的 playAt 都是错的。时钟要瞬时跟, 抖动才限速。
     */
    int downRateMsPerSec = 10;

    /**
     * @brief 样本少于这么多就还用 targetDelayMs, 不动
     *
     * @note 冷启动保护。窗口里只有 3 个样本时 p95 = 最大的那个,
     *          水位会被前几帧的偶然抖动锚死一整个窗口。
     */
    size_t minSamples = 30;
};

/**
 * 只读计数器。
 *
 * @note current / raw / peak 三个都要给, 因为它们回答三个不同的问题:
 *          - **current**: 现在的水位, 也就是现在每帧要付的延迟
 *          - **raw**: 限速和夹取**之前**的原始估计。current != raw 就说明
 *            当前的水位不是网络要求的, 而是被某条规则按住的 —— 是哪条看下面两个
 *          - **peak**: 跑完之后回答"这条链路最坏需要多少水位"。稳态的 current
 *            看不出跑的过程里有没有过一次逼近上限。
 */
struct DelayEstimatorStats {
    /** @brief 喂进来的样本总数 */
    uint64_t samplesSeen = 0;

    /** @brief 因滑出窗口而丢弃的样本数 */
    uint64_t samplesEvicted = 0;

    /** @brief 当前窗口内的样本数 */
    size_t windowSize = 0;

    /** @brief 当前生效的水位(毫秒) */
    int currentDelayMs = 0;

    /** @brief 限速与夹取之前的原始分位数估计(毫秒) */
    int rawDelayMs = 0;

    /** @brief 当前的时钟基准偏移(毫秒); 含两端时钟差, 绝对值没有意义 */
    int64_t floorOffsetMs = 0;

    /** @brief 整个生命周期里 currentDelayMs 到过的最大值 */
    int peakDelayMs = 0;

    /** @brief 水位上调的次数(立刻跟随) */
    uint64_t raises = 0;

    /**
     * @brief 下调被限速按住的次数
     *
     * @note 它和 raises 一起画出水位的"锯齿": 大量 raises 配大量 downRateLimited,
     *          说明抖动在反复冲高, 窗口或分位数选小了。
     */
    uint64_t downRateLimited = 0;

    /** @brief 被 maxDelayMs 夹住的次数; **非 0 就要查**, 见 maxDelayMs 的 @note */
    uint64_t clampedHigh = 0;

    /**
     * @brief 被下限夹住的次数
     *
     * @note 非 0 本身不是问题(链路比下限还稳)。但要配着 floorFromBudget 一起看:
     *          下限来自重传预算而且频繁夹住, 说明**水位是被预算撑着的**,
     *          不是网络要求的 —— 那时该问的是重传值不值这个延迟。
     */
    uint64_t clampedLow = 0;

    /** @brief 当前生效的下限(毫秒) = max(minDelayMs, 帧周期 + RTT) */
    int effectiveMinDelayMs = 0;

    /** @brief 当前生效的下限是不是由重传预算给出的(而非 minDelayMs) */
    bool floorFromBudget = false;

    /** @brief 实测帧周期(毫秒); 由相邻 timestampMs 的差值中位数得到 */
    int frameIntervalMs = 0;

    /** @brief 外部喂进来的实测 RTT(毫秒); 0 表示还不知道 */
    int rttMs = 0;
};

/**
 * 抖动水位估计器 —— (发送时间戳, 到达时刻) 进, 应播时刻出。
 *
 * 用法(收包线程内, 单线程, 不需要加锁):
 * @code
 * // JitterBuffer::computePlayAt 里就这一句
 * return estimator_.observe(frame.timestampMs, nowMs);
 * @endcode
 *
 * @note **observe() 既是输入也是输出**, 刻意合成一个: 分成 observe() + playAtMs()
 *          两步的话, 忘了调 observe 的表现是"水位永远不动", 而这个 bug
 *          在回环上(抖动本来就接近 0)完全看不出来。合成一步就不可能忘。
 *
 * @note 不是线程安全的, 由收包线程独占 —— 同 JitterBuffer。
 */
class DelayEstimator {
public:
    explicit DelayEstimator(DelayEstimatorConfig config = {});

    DelayEstimator(const DelayEstimator&) = delete;
    DelayEstimator& operator=(const DelayEstimator&) = delete;

    /**
     * @brief 喂一帧的到达情况, 返回它的应播时刻
     *
     * @param timestampMs 帧携带的发送端时间戳
     * @param nowMs       该帧的到达时刻(本地单调时钟)
     *
     * @return 本地时间轴上的应播时刻(毫秒); 恒 >= 0
     *
     * @note 算法(自适应模式):
     *          @code
     *          offset = int64(nowMs) - int64(timestampMs)   // = C + T
     *          窗口.push(nowMs, offset); 淘汰比 nowMs 老过 windowMs 的
     *
     *          floor  = 窗口 offset 的 floorPercentile 分位     // ≈ C + T_min
     *          raw    = 窗口 offset 的 delayPercentile 分位 - floor
     *          target = raw > target ? raw                       // 上调: 立刻
     *                                : max(raw, target - 降速×已过秒数)
     *          target = clamp(target, minDelayMs, maxDelayMs)
     *
     *          playAt = timestampMs + floor + target
     *          @endcode
     *
     * @note **全程 int64**。offset 是 `nowMs - timestampMs`, 两台机器的
     *          steady_clock 起点毫不相干, 它可能是很大的负数; 用无符号算
     *          直接绕成天文数字。这条 M3 的 computePlayAt 已经踩过了。
     *
     * @note **降速的 dt 用两次 observe 之间的间隔**, 不是固定的帧周期:
     *          流卡顿、帧率变化时帧周期不成立。而 dt 必须防负 —— 单调时钟
     *          理论上不回退, 但 nowMs 是调用方传进来的, 传什么都合法。
     *
     * @note 非自适应模式下退化成 M3: floor 取历史最小 offset(只减不增),
     *          target 恒为 targetDelayMs。**必须逐字保持这个行为** ——
     *          它是 M4.5 报告的对照组, 对照组变了整张表就没有意义了。
     */
    uint64_t observe(uint32_t timestampMs, uint64_t nowMs);

    /**
     * @brief 告诉估计器当前实测的重传往返(毫秒)
     *
     * @param rttMs 实测 RTT; <= 0 表示"还不知道", 忽略
     *
     * @note 它**不直接决定水位**, 只抬高水位的下限:
     *          @code
     *          实际下限 = max(config_.minDelayMs, 帧周期 + rttMs)
     *          @endcode
     *
     * @note 为什么下限要含重传预算: 一帧缺了分片时, 补齐它需要
     *          **检测延迟 + RTT**。检测延迟是"再收到一帧的包数那么多个更新的包"
     *          (见 NackTrackerConfig 的推导, 恒等于一个帧周期, 与码率无关)。
     *          水位低于这个和, 重传回来的帧必然已经过了 playAt —— 白跑一趟带宽,
     *          而且丢的那一帧会连累到下一个 IDR 为止的所有帧。
     *
     * @note 为什么是**下限**而不是水位本身: 高丢包时 p95 自己就会涨到这个量级
     *          (一帧 8 个分片、10% 丢包下 1-0.9^8 = 57% 的帧要等重传,
     *          远超 5% 的分位阈值), 那时预算是多余的。
     *          预算真正管用的是**低丢包**那一段: 0.5% 丢包下只有 3.9% 的帧要等重传,
     *          落不进 p95, 于是这些帧被静默丢掉 —— 正是 M4.5 第一轮
     *          "3%/5% 丢包的成绩反而比 10% 差"的成因。
     *
     * @note 帧周期不用配置: 相邻 timestampMs 的差值就是它, 本类自己量。
     *          RTT 本类量不了(它不认识包, 也不知道 NACK 什么时候发的), 所以由外部喂。
     */
    void setRttMs(int rttMs);

    /** @brief 当前生效的水位(毫秒) */
    int currentDelayMs() const { return stats_.currentDelayMs; }

    const DelayEstimatorStats& stats() const { return stats_; }

    /**
     * @brief 清空全部状态(含计数器), 回到刚构造的样子
     *
     * @note 什么时候该调: **只在重连时** —— 对端换了, 时钟差整个变了,
     *          旧样本全部无效。跟着 JitterBuffer::reset() 走。
     * @note 队列满 / dropUntilKeyFrame() **不要**调这个: 流没变, 时钟差没变,
     *          清掉窗口只会让水位重新冷启动, 而冷启动期间恰好最需要准确的水位
     *          (下游刚出过问题)。同 NOTES D12 的推演。
     */
    void reset();

private:
    /**
     * @brief 用最近的 timestampMs 差值更新帧周期估计
     *
     * @param timestampMs 本帧的发送端时间戳
     *
     * @note 取**中位数**而不是均值: 帧会乱序到达, 乱序时相邻两帧的差值是负的或很大,
     *          均值会被拖偏而中位数不会。差值取绝对值, 且用 int32 做差
     *          (timestampMs 会回绕, 无符号减法在回绕点给出十亿级的差)。
     */
    void updateFrameInterval(uint32_t timestampMs);

    /**
     * @brief 取窗口内 offset 的第 p 分位
     *
     * @param percentile 0-100
     *
     * @return 分位值; 窗口为空时返回 0
     *
     * @note 取法定死为 **index = p × (n-1) / 100**(整数除法, 就近取下),
     *          不做插值。定死是为了让单测能写出确定的期望值 ——
     *          "大约是 p95" 这种断言测不住任何东西。
     * @note 实现上先把 offset 拷进一个复用的 buffer 再 nth_element:
     *          窗口本身按时间有序, 不能就地排序。n 是几百, 每帧一次 O(n)
     *          的开销在 233 包/秒 的量级下完全看不见(NOTES D18)。
     */
    int64_t percentileOffset(int percentile) const;

    struct Sample {
        uint64_t atMs = 0;
        int64_t offsetMs = 0;
    };

    DelayEstimatorConfig config_;
    DelayEstimatorStats stats_;

    /** @brief 最近 windowMs 内的样本, 按 atMs 递增(push_back / pop_front) */
    std::deque<Sample> window_;

    /** @brief percentileOffset() 的复用缓冲, 避免每帧分配 */
    mutable std::vector<int64_t> scratch_;

    /** @brief 当前时钟基准; 自适应模式是窗口分位数, 否则是历史最小 */
    int64_t floorOffsetMs_ = 0;
    bool hasFloor_ = false;

    /** @brief 当前水位; 构造时 = config_.targetDelayMs */
    int targetDelayMs_ = 0;

    /** @brief 上一次 observe 的时刻, 用来算降速的 dt */
    uint64_t lastObserveMs_ = 0;
    bool hasLastObserve_ = false;

    /** @brief 下调限速除以 1000 后留下的余数, 避免逐帧小 dt 永远截成 0 */
    uint64_t downRateRemainder_ = 0;

    /** @brief 外部喂进来的实测 RTT; 0 = 还不知道 */
    int rttMs_ = 0;

    /** @brief 最近若干个相邻 timestampMs 的差值(毫秒), 取中位数当帧周期 */
    std::deque<int> frameDeltas_;
    uint32_t lastTimestampMs_ = 0;
    bool hasLastTimestamp_ = false;

    /**
     * @brief 帧周期估计要攒几个差值
     *
     * @note 别取太大: 帧率变化时它要跟得上。也别取太小: 一次乱序就能歪掉。
     *          15 个 @30fps 是半秒。
     */
    static constexpr size_t kFrameDeltaWindow = 15;
};
