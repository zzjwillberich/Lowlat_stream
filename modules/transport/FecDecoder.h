/**
 * @file    FecDecoder.h
 * @brief   接收端: 用 XOR 冗余包补回组内丢掉的那一个 DATA 包 (M4.2)
 * @author  zzj
 * @date    2026-08-28
 *
 * @note **不等待**: FEC 包到达的那一刻就判定能不能恢复, 不为"也许还有成员会晚到"
 *       留着组。理由是发送端把 FEC 包排在本帧最后一片之后发出, 正常情况下它到达时
 *       组内成员该到的都到了。成员乱序到 FEC 之后属于少数情况, 那部分交给 NACK ——
 *       为它维护一堆待定的组, 换来的恢复率提升很小, 而状态机复杂一倍。
 *
 * @note 恢复**只认** FecHeader 的 groupBaseSeq + groupSize, 不按任何切分规则推算。
 *       XOR 没有校验: 异或错了一组包, 出来的是一串看着完全合法的字节, 长度也对,
 *       组包器会把它当真分片收下喂给解码器 —— 没有报错, 只有偶发花屏。
 *
 * @note 恢复出来的是一个**完整的 DATA 包**(含 21 字节头), 可以直接喂给
 *       FrameAssembler::offer() 和 NackTracker::onPacket(), 调用方不必区别对待。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "common/Status.h"
#include "modules/transport/Packetizer.h"

/**
 * 容量策略。
 */
struct FecDecoderConfig {
    /**
     * @brief 保留最近多少个 DATA 包用于恢复; 0 表示**不启用 FEC 解码**
     *
     * @note 要盖住"一个组的跨度 + FEC 包排在本帧末尾的延迟"。按帧切组时,
     *          FEC 包最晚在本帧最后一片之后到达, 所以保留一帧多一点就够 ——
     *          默认 64 对 7.75 包/帧的实测值是 8 帧的余量, 足够宽。
     *
     * @note 存的是**整包**而不只是载荷: 恢复时要从组内活着的成员那里取
     *          frameId / fragCount / flags / timestampMs 来重建包头。
     *          64 × 1221 字节 ≈ 78KB, 有界且可忽略。
     */
    size_t recentPackets = 64;
};

/** 只读计数器 */
struct FecDecoderStats {
    /** @brief 收到的 FEC 包数 */
    uint64_t fecPacketsReceived = 0;

    /** @brief 解不出 FecHeader 的 FEC 包数; 和丢包分开计, 同 lost / malformed 的理由 */
    uint64_t fecPacketsMalformed = 0;

    /** @brief 组内一个都没丢, FEC 包白来一趟 —— 稳态下这应当是绝大多数 */
    uint64_t groupsComplete = 0;

    /** @brief 成功恢复出一个 DATA 包的组数 */
    uint64_t groupsRecovered = 0;

    /**
     * @brief 组内丢了 >= 2 个, XOR 救不回来的组数
     *
     * @note 和 groupsRecovered 一比就是 FEC 的**实际恢复率**, 这是 M4.2 唯一
     *          能被验收的数字。它偏高说明 groupSize 该调小(代价是更多冗余带宽),
     *          或者丢包已经严重到 XOR 方案的天花板之外(见 [[D24]] 的取舍表)。
     */
    uint64_t groupsUnrecoverable = 0;

    /** @brief 恢复出来的 DATA 包总数; 恒等于 groupsRecovered(一组最多救一个) */
    uint64_t packetsRecovered = 0;

    /** @brief 当前保留的 DATA 包数 */
    size_t recentPackets = 0;
};

/**
 * XOR 冗余解码器。
 *
 * 用法(收包线程内, 单线程):
 * ```cpp
 * if (header.type == PacketType::Data) {
 *     fecDecoder_.onDataPacket(packet, len);
 *     assembler_.offer(packet, len);
 * } else if (header.type == PacketType::Fec) {
 *     PacketBuffer recovered;
 *     if (fecDecoder_.onFecPacket(packet, len, recovered)) {
 *         nackTracker_.onPacket(...);              // 恢复出的包也要销掉缺口
 *         assembler_.offer(recovered.data(), recovered.size());
 *     }
 * }
 * ```
 */
class FecDecoder {
public:
    explicit FecDecoder(FecDecoderConfig config);

    FecDecoder(const FecDecoder&) = delete;
    FecDecoder& operator=(const FecDecoder&) = delete;

    /**
     * @brief 喂一个刚收到的合法 DATA 包(整包), 存进最近包缓冲供之后的 FEC 恢复用
     *
     * @note **恢复出来的包不要再喂回来。** 它已经被算进这一组了, 喂回来只会占位置;
     *          更要紧的是, 如果将来允许一个包属于多个组, 那会变成重复计数。
     * @note 短于 21 字节的包直接忽略 —— 那种包组包器也会判畸形, 这里不必重复报错。
     */
    void onDataPacket(const uint8_t* packet, size_t len);

    /**
     * @brief 喂一个 FEC 包, 尝试恢复组内缺失的那一个 DATA 包
     *
     * @param packet 整包(含 PacketHeader)
     * @param len    字节数
     * @param out    出参, 恢复出的**完整 DATA 包**; 只在返回 true 时有效
     *
     * @return true 恢复出了一个包
     *
     * @note 三种结局各自计数, 别合并:
     *          - 组内一个不缺 -> groupsComplete, 返回 false (FEC 白来一趟, 正常)
     *          - 恰好缺一个   -> groupsRecovered, 返回 true
     *          - 缺 >= 2 个   -> groupsUnrecoverable, 返回 false (XOR 的天花板)
     *          合成一个数就没法回答"FEC 到底帮上忙没有"和"该不该调小 groupSize"。
     *
     * @note 缺失包的 fragIndex 由 `活着成员的 fragIndex + (缺失 seq − 活着成员的 seq)` 推出,
     *          frameId / fragCount / flags / timestampMs 直接取活着成员的 ——
     *          **组不跨帧**, 这些字段组内必然相同。这条前提一旦被破坏(比如将来改成跨帧分组),
     *          恢复出来的包会带着错误的 frameId, 而且**不会报错**。
     *
     * @note 缺失包的载荷长度来自 `payloadLenXor 异或掉已收到的那些长度`。
     *          用异或缓冲的长度当它的长度是错的 —— 那是组内**最长**的那片。
     */
    bool onFecPacket(const uint8_t* packet, size_t len, PacketBuffer& out);

    /** @brief recentPackets 为 0 时恒为 false; 调用方靠它决定要不要走这一级 */
    bool enabled() const { return config_.recentPackets > 0; }

    const FecDecoderStats& stats() const { return stats_; }

    /** @brief 清空最近包缓冲; 换对端或重新起流时调, 同 NackTracker::reset */
    void reset();

private:
    FecDecoderConfig config_;
    FecDecoderStats stats_;

    // TODO(M4.2): deque<PacketBuffer> 就够 —— 按到达顺序 push_back, 超过 recentPackets
    //   就 pop_front, 查找时线性扫比对 seq。几十条的规模, 别上 map:
    //   本来就要为每一组扫一遍组内成员, map 省不下什么。
    std::deque<PacketBuffer> recent_;
};
