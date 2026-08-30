/**
 * @file    FecEncoder.cpp
 * @brief   FecEncoder.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/FecEncoder.h"

#include <utility>

#include "modules/transport/Packet.h"

FecEncoder::FecEncoder(FecEncoderConfig config) : config_(std::move(config)) {}

Status FecEncoder::buildForFrame(const std::vector<PacketBuffer>& packets, uint32_t baseSeq,
                                 uint16_t streamId, uint32_t timestampMs,
                                 std::vector<PacketBuffer>& out) {
    // TODO(M4.2):
    //   0. out.clear(); groupSize == 0 或 packets 为空 -> 直接返回 Ok(不是错误, 是"没启用")
    //   1. 校验: 每个包都得 >= PACKET_HEADER_SIZE + DATA_HEADER_SIZE, 否则 InvalidArg
    //      (载荷长度 = packet.size() - 21)
    //   2. ++stats_.framesSeen
    //   3. 分组: 第 g 组覆盖 packets[g*K .. min((g+1)*K, N) - 1]
    //      groupCount = ceil(N / K), 用 N/K + (N%K != 0), **别用 (N+K-1)/K**
    //      —— 后者在 N 接近上限时溢出(同 Packetizer::fragmentCount 的理由)
    //   4. 每组:
    //      a. 组内包数 < minGroupSize -> ++stats_.groupsSkipped, 跳过
    //      b. maxPayload = 组内最长载荷; xorBuf 清零到 maxPayload 长
    //      c. 逐个包: xorBuf[i] ^= payload[i] (超出该片长度的部分不动, 等价于补零);
    //         lenXor ^= 该片载荷长度
    //      d. 组包: PacketHeader{type=Fec, streamId, seq=nextFecSeq_++, timestampMs}
    //               FecHeader{groupBaseSeq = baseSeq + g*K, groupSize = 组内包数,
    //                         payloadLenXor = lenXor, groupIndex = g, groupCount, reserved = 0}
    //               载荷 = xorBuf
    //      e. ++stats_.fecPacketsBuilt; stats_.fecBytes += 整包长度
    //
    // **只异或载荷, 不异或包头**: 恢复时接收端从组内活着的成员取 frameId/fragCount/flags,
    // 用 seq 差推 fragIndex —— 组不跨帧, 这些字段组内必然相同。异或包头会把它们搅烂。
    //
    // nextFecSeq_ 只在整帧成功之后才写回? **不需要** —— 这个函数在生成第一个包之前
    // 就把所有可能失败的校验做完了, 后面只有 Internal 级别的失败(本文件算错偏移)。
    // 但如果你的实现里还有可能中途失败, 就照 Packetizer 那样先在局部变量上推进。
    (void)packets;
    (void)baseSeq;
    (void)streamId;
    (void)timestampMs;
    (void)out;
    return Status::error(Code::Internal, "FecEncoder::buildForFrame: not implemented");
}
