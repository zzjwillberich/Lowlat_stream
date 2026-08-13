/**
 * @file    Packet.cpp
 * @brief   Packet.h 的实现骨架
 * @author  zzj
 * @date    2026-08-13
 */

#include "modules/transport/Packet.h"

Status encodePacketHeader(const PacketHeader& header, uint8_t* buf, size_t bufLen) {
    (void)header;
    (void)buf;
    (void)bufLen;

    // TODO(M2):
    // 1. 校验 buf 非空、bufLen >= PACKET_HEADER_SIZE、version == PROTOCOL_VERSION。
    // 2. 按固定偏移逐字段写: [0]version [1]type [2..3]streamId
    //    [4..7]seq [8..11]timestampMs, 多字节字段一律网络字节序。
    // 3. 不要 memcpy 整个结构体 —— 对齐填充和本机字节序都会让线上布局出错。
    return Status::error(Code::Internal, "encodePacketHeader is not implemented (M2)");
}

Status decodePacketHeader(const uint8_t* buf, size_t bufLen, PacketHeader& out) {
    (void)buf;
    (void)bufLen;
    (void)out;

    // TODO(M2):
    // 1. 校验 buf 非空、bufLen >= PACKET_HEADER_SIZE, 否则 InvalidArg。
    // 2. 按与 encode 相同的偏移读回, 网络序转本机序。
    // 3. version 不等于 PROTOCOL_VERSION、或 type 不在 1..6 内, 返回 NetError:
    //    这是对端/网络的问题, 调用方应当丢包计数而不是修参数重试。
    // 4. 校验全部通过后才写 out, 不要边解析边填 —— 失败时留下半个结构体, 调用方
    //    如果忽略了返回值就会拿它去组包。
    return Status::error(Code::Internal, "decodePacketHeader is not implemented (M2)");
}

Status encodeDataHeader(const DataHeader& header, uint8_t* buf, size_t bufLen) {
    (void)header;
    (void)buf;
    (void)bufLen;

    // TODO(M2):
    // 1. 校验 buf 非空、bufLen >= DATA_HEADER_SIZE。
    // 2. 校验 fragCount >= 1 且 fragIndex < fragCount, 否则 InvalidArg ——
    //    这是本端 packetizer 的 bug, 不该让畸形包上网。
    // 3. 偏移: [0..3]frameId [4..5]fragIndex [6..7]fragCount [8]flags。
    return Status::error(Code::Internal, "encodeDataHeader is not implemented (M2)");
}

Status decodeDataHeader(const uint8_t* buf, size_t bufLen, DataHeader& out) {
    (void)buf;
    (void)bufLen;
    (void)out;

    // TODO(M2):
    // 1. 校验 buf 非空、bufLen >= DATA_HEADER_SIZE, 否则 InvalidArg。
    // 2. 读回四个字段。
    // 3. fragCount == 0 或 fragIndex >= fragCount 返回 NetError ——
    //    组包器信任这两个字段去索引数组, 必须在入口挡住。
    return Status::error(Code::Internal, "decodeDataHeader is not implemented (M2)");
}

bool seqNewerThan(uint32_t a, uint32_t b) {
    (void)a;
    (void)b;

    // TODO(M2): return static_cast<int32_t>(a - b) > 0;
    //           无符号相减本身就是模 2^32 运算, 回绕后差值仍然正确。
    return false;
}
