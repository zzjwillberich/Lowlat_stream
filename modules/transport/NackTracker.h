/**
 * @file    NackTracker.h
 * @brief   按 seq 记录哪些包缺了, 判定"真丢了"并产出重传请求 (M4.1)
 * @author  zzj
 * @date    2026-08-28
 *
 * @note 职责边界, 和另外两个类划清楚:
 *          - FrameAssembler 回答"这一帧拼不拼得起来"(按 frameId/fragIndex);
 *          - JitterBuffer   回答"这一帧什么时候放出去"(按 timestamp);
 *          - **本类回答"网络层面缺了哪几个 seq, 值不值得要回来"**。
 *       三者互不知道对方存在, 各自能单独测。
 *
 * @note 本类**不碰 socket、不认识包格式**: 调用方解好包头, 只喂 (seq, fragCount) 进来。
 *       所以它能像 LossInjector 一样纯用数字做单测, 不必构造真包。
 *
 * @note 这张表同时还了 M2 留下的一笔账(见 FrameAssembler.h:93):
 *       packetsLost() 是 `seq 跨度 - 实收`, 重传包一进"实收"就把丢包率压低;
 *       而本类**逐个 seq 记状态**, 数出来的 lostForReal 不受重复包影响。
 *       M4 的验收判据是"X% 丢包下不花屏", 那个 X 必须先是可信的。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

/**
 * 判定与限流的策略参数。
 *
 * @note 乱序容忍**不在这里**: 它取"一帧的包数", 由喂进来的 fragCount 自适应 ——
 *          设帧率 f、每帧包数 k, 则 k 个包 = k/(f*k) = 1/f 秒, **k 约掉了**。
 *          也就是说这个容忍恒等于一个帧周期, 与码率无关。固定成常数反而会
 *          "码率翻倍 -> 包率翻倍 -> 同样 N 个包的时间减半 -> 乱序容忍悄悄变差"。
 *          下面两个 clamp 只是给这个自适应值兜底, 不是主判据。
 */
struct NackTrackerConfig {
    /** @brief 乱序容忍的下限(包); IDR 之后 fragCount 会突然变大, 但不能小到误判 */
    uint32_t minReorderPackets = 4;

    /** @brief 乱序容忍的上限(包); 挡住畸形的 fragCount 把容忍撑到天上 */
    uint32_t maxReorderPackets = 64;

    /**
     * @brief 同一个 seq 最多请求几次
     *
     * @note 必须有上限。反向通道自己也会丢包(两个方向都是 p 的话, 一次重传成功率
     *          只有 (1-p)^2 —— 10% 丢包下是 81%, 30% 下只有 49%), 所以重发是必要的;
     *          但没有上限的话, 一个永远回不来的包会被无限请求, 而每个重传包都是
     *          重复包, 会把丢包统计越搅越浑。
     */
    int maxRequestsPerSeq = 3;

    /**
     * @brief 跟踪窗口(包数): 比 highestSeq 老这么多的 seq 直接放弃
     *
     * @note 不设上限就是安静地涨内存, 同 FrameAssembler 的 maxPendingFrames。
     *          而且**太老的包重传回来也来不及了** —— 帧早过了 playAt,
     *          JitterBuffer 会以 framesTooLate 计数然后扔掉, 白费一趟带宽。
     */
    size_t windowPackets = 1024;
};

/**
 * 只读计数器。
 *
 * @note recovered / givenUp 必须分开: 前者说明重传**有用**, 后者说明**没救回来**。
 *          合成一个数就没法回答 M4 的核心问题"NACK 到底帮上忙了没有"。
 */
struct NackTrackerStats {
    /** @brief 喂进来的包总数(含重传包与重复包) */
    uint64_t packetsSeen = 0;

    /** @brief 发出过的重传请求**条目**数(同一个 seq 请求 3 次算 3 条) */
    uint64_t nacksRequested = 0;

    /** @brief 请求过、后来又收到了的 seq 数 —— 重传真正救回来的 */
    uint64_t recovered = 0;

    /** @brief 超出重试次数或滑出窗口而放弃的 seq 数 */
    uint64_t givenUp = 0;

    /**
     * @brief 精确丢包数: 至今没收到、且已经放弃的 seq 数
     *
     * @note 和 AssemblerStats::packetsLost() 的区别就是这个类存在的理由之一:
     *          那个是减法(会被重传的重复包压低), 这个是逐 seq 数出来的。
     */
    uint64_t lostForReal = 0;

    /**
     * @brief 当前还在跟踪、尚未补齐也尚未放弃的 seq 数
     *
     * @note **报告里必须和 lostForReal 一起给。** 不变式是:
     *
     *          丢掉的包总数 = recovered + givenUp + pending
     *
     *          流停止时, 最后那几个缺口既没用完重试次数、也没滑出窗口(没有新包推进),
     *          会一直停在 pending 里 —— 于是 lostForReal 单独看是**漏报的**。
     *          实测 10% 丢包 300 帧: injected=202, recovered=200, givenUp=0,
     *          lostForReal=0, 而 pending=2 —— 只看 lostForReal 会得出"一个都没丢"。
     *
     *          本类不知道"流结束了", 所以不能自己把 pending 转成 lostForReal;
     *          这个判断归调用方(它才知道 run() 要返回了)。
     */
    size_t pending = 0;

    /**
     * @brief 跳号超过一整个窗口而重新建立基线的次数
     *
     * @note **必须报出来, 不能静默重建**。静默的话"包被打乱了"和"对端换了一条流"
     *          两件事在外面看起来完全一样, 而它们的处置方式不同。
     *          稳态下这个数应当恒为 0; 非 0 就说明有东西在往这个端口上发不该发的包,
     *          或者对端重启了而 reset() 没被调用。
     */
    uint64_t discontinuities = 0;
};

/**
 * 缺口跟踪器 —— seq 进, 该重传的 seq 出。
 *
 * 用法(收包线程内, 单线程, 不需要加锁):
 * ```cpp
 * // 每收到一个合法 DATA 包
 * tracker.onPacket(header.seq, dataHeader.fragCount);
 *
 * // 每轮循环取一次, 有东西就发一个 NACK 包
 * tracker.collectNackTargets(nackTargets);
 * if (!nackTargets.empty() && peer_.port != 0) { ...发 NACK... }
 * ```
 */
class NackTracker {
public:
    explicit NackTracker(NackTrackerConfig config);

    NackTracker(const NackTracker&) = delete;
    NackTracker& operator=(const NackTracker&) = delete;

    /**
     * @brief 喂一个刚收到的合法 DATA 包
     *
     * @param seq       PacketHeader::seq
     * @param fragCount DataHeader::fragCount, 用来自适应乱序容忍
     *
     * @note **第一个包只建立基线, 不产生任何缺口。** 接收端从流的中间接进来是常态
     *          (对端已经跑了一会儿), 第一个 seq 是 5000 的话, 绝不能把 0..4999
     *          全判成丢包然后发五千条 NACK。这是这类代码最容易写出的第一个 bug。
     *
     * @note 重传包和重复包也要喂进来: 前者要销掉对应的缺口(recovered++),
     *          后者要被识别成"已经收到过", 不能重复计数。
     *
     * @note seq 会回绕。比较大小一律用 seqNewerThan(Packet.h), 直接 `a > b`
     *          在 0xFFFFFFFF -> 0 处会把最新的包判成最老的。
     *
     * @note **跳号超过 windowPackets 时按"换了一条流"处理**: 像首包一样重新建立基线,
     *          既不登记缺口也不计任何丢包, 只 ++discontinuities。
     *
     *          理由: seqNewerThan 只保证差值为正, 上界是 2^31 —— 而 seq 是包头里
     *          **唯一没有任何校验的字段**(版本、类型、分片自洽性 M2 都查了,
     *          但 seq 的每一个 32 位取值都合法)。一个位翻转、一个上一轮残留的包、
     *          同端口上另一个进程, 都能让 forwardDistance 变成十亿级。
     *
     *          不堵的话两层后果: lostForReal 被永久污染(而它正是这个类存在的理由 ——
     *          M4 的"X% 丢包下不花屏"里那个 X 靠它), 以及为 windowPackets 个
     *          **根本不存在的包**登记幻影缺口, 接下来是一场 NACK 风暴。
     *
     *          阈值取 windowPackets 而不是另立一个: 窗口外的包本来就救不回来
     *          (重传赶不上 playAt), 跳过一整个窗口意味着连续性已经没有意义了。
     */
    void onPacket(uint32_t seq, uint16_t fragCount);

    /**
     * @brief 取出这一轮应当请求重传的 seq
     *
     * @param out 出参, 调用前会被 clear(); 由调用方复用以避免每轮分配
     *
     * @note 判定标准: 某个缺失的 seq 之后, 已经收到了**至少一帧的包数**那么多个
     *          更新的包 —— 到这个程度还没来, 就不是乱序而是真丢了。
     *
     * @note 同一个 seq 被取出后, 要再等一帧的包数才允许第二次取出(重发),
     *          最多 maxRequestsPerSeq 次。**不要用定时器**: 一切以包数推进,
     *          流停了自然就不再产生请求 —— 对端已经退出时不会对着一个死地址狂发。
     *
     * @note 取出的顺序应当是**从老到新**: 老的那个离 playAt 更近, 更急。
     */
    void collectNackTargets(std::vector<uint32_t>& out);

    const NackTrackerStats& stats() const { return stats_; }

    /**
     * @brief 清空全部状态, 回到"还没见过任何包"
     *
     * @note 什么时候该调: 对端换了(peer_ 变了)意味着换了一条流, seq 空间完全不同,
     *          旧缺口再请求也没有意义。**别拿它当"队列满了"的处理** —— 那是 D12。
     */
    void reset();

private:
    struct GapState {
        int requestCount = 0;
        uint32_t lastRequestAtHighestSeq = 0;
    };

    NackTrackerConfig config_;
    NackTrackerStats stats_;

    /** @brief 当前未收到的 seq 及其请求限流状态 */
    std::map<uint32_t, GapState> gaps_;

    /** @brief 最新的已收 seq; 只在 hasBaseline_ 为 true 时有效 */
    uint32_t highestSeq_ = 0;

    /** @brief 最近一个合法 DATA 包给出的自适应乱序容忍 */
    uint32_t reorderPackets_ = 0;

    bool hasBaseline_ = false;
};
