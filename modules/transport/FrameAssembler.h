/**
 * @file    FrameAssembler.h
 * @brief   接收端组包: 分片 -> 完整帧, 并按 seq 统计丢包, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-15
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "common/Status.h"

/**
 * 组装好的一帧, 可直接喂解码器。
 *
 * @note 与 EncodedFrame 一样禁止拷贝: 一帧几十上百 KB, 隐式拷贝会悄悄出现在
 *          每帧都走的路径上。需要复制就显式写。
 */
struct AssembledFrame {
    /** @brief 拼好的 Annex B 码流, 含起始码 */
    std::vector<uint8_t> data;

    /** @brief 发送端的帧号(32 位, 会回绕) */
    uint32_t frameId = 0;

    /** @brief 发送端透传过来的采集时刻, 端到端延迟的起点 */
    uint32_t timestampMs = 0;

    /** @brief 是否关键帧 */
    bool isKey = false;

    AssembledFrame() = default;
    AssembledFrame(const AssembledFrame&) = delete;
    AssembledFrame& operator=(const AssembledFrame&) = delete;
    AssembledFrame(AssembledFrame&&) = default;
    AssembledFrame& operator=(AssembledFrame&&) = default;
};

/**
 * 接收侧计数器。
 *
 * @note 丢包**不是**在发现 seq 缺口的当场累加的, 而是由 seq 跨度减去实收数算出来:
 *          乱序的包晚一点会到, 当场记下的那一笔再也减不回去, 丢包率就只会虚高。
 *          RFC 3550 里 RTP 的接收报告也是这么算的。
 * @note 畸形包**不并入丢包**: 丢包说明网络差, 畸形包说明有人乱发或版本不匹配,
 *          两者的处置完全不同, 混成一个数就分不出该修网络还是该修对端。
 */
struct AssemblerStats {
    /** @brief 成功解析的 DATA 包数, **含重复包**(与 RTP 的口径一致) */
    uint64_t packetsReceived = 0;

    /** @brief 解析失败被丢弃的包数(版本/类型/长度/分片字段不合法) */
    uint64_t packetsMalformed = 0;

    /** @brief 同一分片重复到达, 或所属帧已经交付过 */
    uint64_t packetsDuplicate = 0;

    /** @brief 所属帧已被淘汰, 来晚了 */
    uint64_t packetsTooLate = 0;

    /** @brief 成功交付的完整帧数 */
    uint64_t framesCompleted = 0;

    /** @brief 因容量上限被放弃的残帧数 */
    uint64_t framesDropped = 0;

    /** @brief 见过的第一个 seq */
    uint32_t baseSeq = 0;

    /** @brief 见过的最新 seq(按回绕安全的比较) */
    uint32_t highestSeq = 0;

    /** @brief 是否已经收到过至少一个包 */
    bool started = false;

    /**
     * @brief 按 seq 跨度算出的应收包数
     *
     * @return 未收到任何包时为 0, 否则 highestSeq - baseSeq + 1(回绕安全)
     */
    uint64_t expectedPackets() const;

    /**
     * @brief 估算的丢包数
     *
     * @return expectedPackets() - packetsReceived, 下限 0
     *
     * @note 会被重复包压低(实收里含重复), 这是 RTP 同款口径下的已知偏差。
     *          真要精确区分, 得为每个 seq 记一个位图 —— 那是 M4 重传窗口的事。
     */
    uint64_t packetsLost() const;
};

/**
 * 分片组包器 —— 一个个 DATA 包进, 完整帧出。
 *
 * 用法:
 * @code
 * const Status st = assembler.offer(buf, n);
 * // 非 Ok 只需继续收下一个, 计数已经在内部记好
 * AssembledFrame frame;
 * while (assembler.pop(frame)) {
 *     decoder.decode(frame);
 * }
 * @endcode
 *
 * **职责边界**: 它只回答"这一帧的分片齐了没有", **不排序**。帧 5 先齐就先交出去,
 * 哪怕帧 4 还缺一片。把乱序的帧按显示顺序排好、按时间取用是 M3 jitter buffer 的事 ——
 * 两件事混在一个类里, 就再也没法单独测"组包对不对"和"等多久才放弃"。
 *
 * **内存必须有界**: 残帧永远等不齐是常态(那一片就是丢了)。不设上限的话, 每丢一包
 * 就留下一个再也不会完成的 map 条目, 跑一晚上就是几个 GB —— 而且它不像队列积压那样
 * 会在延迟上表现出来, 只会安静地涨。上限用**帧数**而不是字节数: 帧数直接对应
 * "还能容忍多大的乱序", 是个能讲清楚的取舍轴。
 *
 * @note 不是线程安全的, 由收包线程独占。
 */
class FrameAssembler {
public:
    /**
     * @param maxPendingFrames 同时保留的帧数上限(含已交付的墓碑), 必须 >= 1
     *
     * @note 这个值就是能容忍的乱序/延迟窗口: 太小则轻微乱序就丢帧, 太大则丢一片
     *          之后要多占好一会儿内存, 且残帧要拖到很晚才被判死。M3 会用时间维度
     *          替这个数做更准的判断, M2 先用帧数把内存框住。
     */
    explicit FrameAssembler(size_t maxPendingFrames = 8);

    /**
     * @brief 喂进一个刚收到的 UDP 数据报
     *
     * @param packet 数据报首地址(通用头 + DATA 头 + 载荷)
     * @param len    数据报字节数
     *
     * @return Ok         包已被接受(可能补齐了一帧, 也可能只是存下)
     *  InvalidArg packet 为空, 或包类型不是 DATA
     *  NetError   包畸形: 长度不足、版本/类型不认识、分片字段自相矛盾,
     *                     或与本帧先前声明的 fragCount 不一致
     *
     * @note **长度不足返回 NetError 而不是 InvalidArg**, 尽管底层的
     *          decodePacketHeader 对短缓冲区报的是 InvalidArg: 那里的缓冲区是本端给的,
     *          这里的长度是**网线上来的**。同一个现象, 责任方不同, 调用方的反应
     *          也不同(改代码 vs 丢包计数), 所以必须在这一层翻译过来。
     * @note 非 DATA 包返回 InvalidArg: FEC/NACK/PLI 都是合法的包, 只是不归它管。
     *          分派是**调用方**的责任, 喂错了是本端的 bug。
     * @note 返回非 Ok 时内部计数已经加好, 调用方直接 continue 即可, 不必自己再记一笔。
     */
    Status offer(const uint8_t* packet, size_t len);

    /**
     * @brief 取出一帧已经齐了的帧
     *
     * @param out 出参, 移动赋值进来
     *
     * @return true 取到一帧; false 当前没有完整帧
     *
     * @note 用 while 循环取干净。虽然一个包最多补齐一帧, 但别把这个假设写进调用方 ——
     *          M4 的 FEC 恢复会一次放出好几帧。
     */
    bool pop(AssembledFrame& out);

    /** @brief 只读计数器 */
    const AssemblerStats& stats() const { return stats_; }

    /** @brief 当前保留的帧条目数(含墓碑), 用于验证内存有界 */
    size_t pendingFrames() const { return pending_.size(); }

    /** @brief 清空全部状态(含计数器), 重连时用 */
    void reset();

private:
    /**
     * 一帧的收集状态。
     *
     * @note completed 是**墓碑**: 帧交付之后条目不删, 留着挡住重传/乱序带来的重复分片。
     *          删掉的话, 迟到的那一片会重新建出一个残帧条目, 占着内存等一个永远不会
     *          到齐的帧; 整帧重传时更会被再交付一次, 解码器收到重复帧。
     *          进入墓碑状态时要**释放 fragments 的内存**, 只留几个字节的标记。
     */
    struct PendingFrame {
        /** @brief 按 fragIndex 下标存放的分片载荷, 空表示还没到 */
        std::vector<std::vector<uint8_t>> fragments;

        uint16_t fragCount = 0;
        uint16_t receivedFragments = 0;
        size_t totalBytes = 0;
        uint32_t timestampMs = 0;
        bool isKey = false;
        bool completed = false;
    };

    /** @brief 更新 baseSeq/highestSeq; highestSeq 的比较必须用 seqNewerThan */
    void trackSeq(uint32_t seq);

    /** @brief 返回当前最老的 frameId; 调用前保证 pending_ 非空 */
    uint32_t oldestPendingFrameId() const;

    /**
     * @brief 淘汰最老的一条, 并把水位推到它那里
     *
     * @note "最老"要用 seqNewerThan 比, 不能直接比大小 —— frameId 和 seq 一样是
     *          32 位循环计数器, 回绕之后 0x00000001 比 0xFFFFFFFF 新。直接比大小
     *          会在回绕点把刚到的新帧当成最老的一条淘汰掉, 表现为"每隔很久画面卡一下"。
     * @note 条目数少(默认 8), 线性扫一遍就行, 不值得为它维护有序结构。
     */
    void evictOldest();

    std::unordered_map<uint32_t, PendingFrame> pending_;
    std::deque<AssembledFrame> ready_;

    /**
     * @brief 淘汰水位: 不比它新的 frameId 一律拒收(计入 packetsTooLate)
     *
     * @note 没有这个水位, 被淘汰的帧会被迟到的分片**复活**成一个新残帧,
     *          占着容量把真正在收的帧挤掉 —— 越丢包挤得越狠。
     */
    uint32_t retiredFrameId_ = 0;
    bool hasRetired_ = false;

    size_t maxPendingFrames_ = 8;
    AssemblerStats stats_;
};
