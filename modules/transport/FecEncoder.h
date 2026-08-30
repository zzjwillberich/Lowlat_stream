/**
 * @file    FecEncoder.h
 * @brief   发送端: 给一帧的分片按组生成 XOR 冗余包 (M4.2)
 * @author  zzj
 * @date    2026-08-28
 *
 * @note **按帧切组**, 组不跨帧。理由是 FEC 包必须等组满才发得出去 —— 组跨帧的话
 *       第 1 帧最后几个包所在的组要等第 2 帧的包补满, FEC 包比它保护的数据**晚一整帧**,
 *       而 FEC 相对 NACK 的全部优势就是"不用等一个往返"。详见 [[D24]]。
 *
 * @note FEC 包用**自己的** seq 计数器, 不占 DATA 的 seq 空间。接收端的
 *       `packetsLost() = seq 跨度 − 组包器实收`, 而组包器只收 DATA 包 ——
 *       FEC 一旦占号, 丢包率被永久虚高 1/K, 而那正是 M4 验收判据里的那个数。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/Status.h"
#include "modules/transport/Packetizer.h"

/**
 * 分组策略。
 */
struct FecEncoderConfig {
    /**
     * @brief 每组的 DATA 包数; 0 表示**不启用 FEC**
     *
     * @note K 小抗丢强但费带宽(开销 1/K), K 大反之。一个包丢了能被救回来的条件是
     *          组内其它 K 个包一个都没丢, 即 (1-p)^K:
     *
     *          | K  | 冗余  | p=5% | p=10% | p=30% |
     *          |  4 | 25%   |  81% |   66% |   24% |
     *          |  8 | 12.5% |  66% |   43% |  5.7% |
     *          | 16 | 6.25% |  44% | 18.5% |  0.3% |
     *
     *          **XOR 在高丢包率下基本没用** —— 这是"只能纠 1 个"的天花板, 不是实现问题。
     */
    uint16_t groupSize = 4;

    /**
     * @brief 组内包数少于这个值就不发 FEC; 恒 >= 2
     *
     * @note 1 个包的"FEC"就是原包的完整副本 —— 那不叫纠错, 叫重复发送。
     *          名字对不上实质的东西迟早被人误解成"这里有 FEC 保护"。
     *          (关键帧确实值得复制一份, 但那是**另一条策略**(IDR 双发), 不该塞进 FEC 当特例。)
     */
    uint16_t minGroupSize = 2;
};

/** 只读计数器 */
struct FecEncoderStats {
    /** @brief 处理过的帧数 */
    uint64_t framesSeen = 0;

    /** @brief 生成的 FEC 包数 */
    uint64_t fecPacketsBuilt = 0;

    /** @brief 因为不足 minGroupSize 而跳过的组数 */
    uint64_t groupsSkipped = 0;

    /** @brief 生成的 FEC 包的总字节数; 和 encodedBytes 一比就是实际冗余开销 */
    uint64_t fecBytes = 0;
};

/**
 * XOR 冗余包生成器。
 *
 * 用法(发送线程内, 单线程):
 * ```cpp
 * packetizer_.packetize(view, packets_);
 * fecEncoder_.buildForFrame(packets_, baseSeq, streamId, captureMs, fecPackets_);
 * for (auto& p : packets_)    sendTo(...);
 * for (auto& p : fecPackets_) sendTo(...);   // FEC 紧跟本帧发出, 不跨帧
 * ```
 */
class FecEncoder {
public:
    explicit FecEncoder(FecEncoderConfig config);

    FecEncoder(const FecEncoder&) = delete;
    FecEncoder& operator=(const FecEncoder&) = delete;

    /**
     * @brief 给一帧的全部分片生成 FEC 包
     *
     * @param packets     这一帧的全部分片(**完整 UDP 包**, 含 21 字节头), 顺序即 fragIndex 序
     * @param baseSeq     第一片的 seq; 组内 seq 连续递增
     * @param streamId    与 DATA 包一致
     * @param timestampMs 与 DATA 包一致(帧的 captureMs 低 32 位)
     * @param out         出参, 调用前会被 clear(); 可能是**空的**(帧太小、或 groupSize 为 0)
     *
     * @return Ok         生成成功(包括"一个都没生成")
     *  InvalidArg packets 里有短于 21 字节的包 —— 那不是本函数该处理的输入
     *  Internal   头编码失败; 缓冲是本函数按线上长度算好的, 失败只可能是本文件算错了
     *
     * @note **只异或载荷, 不异或包头。** 恢复时接收端从组内**活着的**成员那里取
     *          frameId / fragCount / flags / timestampMs, 再用 seq 差推出 fragIndex ——
     *          因为组不跨帧, 这些字段组内必然相同。异或包头反而会把这些字段搅烂。
     *
     * @note 异或缓冲的长度取组内**最长**的那片; 短的补零参与异或。
     *          同时把各片长度也异或进 payloadLenXor —— XOR 出来的是字节不是长度,
     *          一帧的最后一片是短的, 恢复时长度猜错就是静默损坏。
     *
     * @note out 由调用方复用, 内部只 resize 不 shrink。
     */
    Status buildForFrame(const std::vector<PacketBuffer>& packets, uint32_t baseSeq,
                         uint16_t streamId, uint32_t timestampMs,
                         std::vector<PacketBuffer>& out);

    /** @brief 下一个 FEC 包要用的 seq; 和 DATA 的 seq 空间无关 */
    uint32_t nextFecSeq() const { return nextFecSeq_; }

    /** @brief 配置里 groupSize 为 0 时恒为 false; 调用方靠它决定要不要走这一级 */
    bool enabled() const { return config_.groupSize > 0; }

    const FecEncoderStats& stats() const { return stats_; }

private:
    FecEncoderConfig config_;
    FecEncoderStats stats_;

    /** @brief FEC 自己的包序号; 每发一个 FEC 包 +1, 会回绕 */
    uint32_t nextFecSeq_ = 0;
};
