/**
 * @file    Packetizer.cpp
 * @brief   Packetizer.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "modules/transport/Packetizer.h"

#include <cstring>

Packetizer::Packetizer(uint16_t streamId, uint32_t initialSeq)
    : streamId_(streamId), nextSeq_(initialSeq) {}

Status Packetizer::packetize(const EncodedFrameView& frame, std::vector<PacketBuffer>& out) {
    // TODO(M2): 步骤:
    //  1. 校验(全部前置, 之后不再有失败路径):
    //     - frame.data == nullptr || frame.len == 0 → InvalidArg;
    //     - fragmentCount(frame.len) > 65535 → InvalidArg;
    //       fragCount 是 uint16_t, 溢出会让分片数悄悄绕回去, 组包器永远等不齐。
    //  2. out.resize(fragCount) —— 用 resize 不用 clear + push_back,
    //     旧元素的 capacity 留着复用, 跑热之后不再分配;
    //  3. 逐片填:
    //     - payloadLen = min(MAX_PAYLOAD, frame.len - i * MAX_PAYLOAD);
    //     - out[i].resize(PACKET_HEADER_SIZE + DATA_HEADER_SIZE + payloadLen);
    //     - PacketHeader{version, Data, streamId_, nextSeq_++, (uint32_t)frame.captureMs};
    //     - DataHeader{(uint32_t)frame.frameId, (uint16_t)i, (uint16_t)fragCount,
    //                  frame.isKey ? FLAG_KEYFRAME : 0};
    //     - encodePacketHeader / encodeDataHeader 写头, memcpy 写载荷;
    //     - 两个 encode 的返回值要检查 —— 这里返回错误就是本文件自己算错了偏移,
    //       属于 Internal, 不该发生, 但别把 [[nodiscard]] 直接 (void) 掉。
    //
    //  三个必须想清楚的点:
    //
    //  a) **captureMs 截成 uint32_t 是有意的**: 线上字段就 4 字节。它是 steady_clock
    //     的毫秒数, 约 49.7 天回绕一次; 接收端算延迟时用有符号差值(同 seqNewerThan),
    //     回绕点也不会算出负数。别为了"不丢精度"把它扩成 8 字节 —— 每包多 4 字节,
    //     1080p 30fps 下一天多出几十 MB, 换来的精度一点用没有。
    //
    //  b) **frameId 同样截成 uint32_t**: 组包只需要"最近这些帧里各不相同",
    //     32 位每秒 30 帧能撑 4.5 年。
    //
    //  c) **isKey 要打在每一片上, 不是只打第一片**: M4 的背压是按**包**丢的,
    //     只有第一片带标记时, 中间那些包该保该丢无从判断, 结果是 IDR 被丢掉一半 ——
    //     花屏一直撑到下一个 IDR。一个 bit 的成本换掉一整类问题。
    (void)frame;
    (void)out;
    return Status::error(Code::Internal, "Packetizer::packetize is not implemented (M2)");
}

size_t Packetizer::fragmentCount(size_t len) {
    // TODO(M2): 向上取整, 一行: return (len + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    //  别写成 len / MAX_PAYLOAD + 1 —— 见头文件里的说明。
    (void)len;
    return 0;
}
