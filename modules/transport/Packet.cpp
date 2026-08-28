/**
 * @file    Packet.cpp
 * @brief   Packet.h 的实现
 * @author  zzj
 * @date    2026-08-13
 */

#include "modules/transport/Packet.h"

#include <arpa/inet.h>
#include <cstring>

namespace {
    /**
     * 类型合法性只在这一处判定: 编解码两边各写一份, 加了新类型迟早漏改一边。
     *
     * @note 依赖 PacketType 取值连续 —— 新增类型必须接在 Stats 之后, 否则中间的空洞会被放行。
     */
    bool isKnownPacketType(uint8_t type) {
        return type >= static_cast<uint8_t>(PacketType::Data) &&
               type <= static_cast<uint8_t>(PacketType::Stats);
    }
}  // namespace

Status encodePacketHeader(const PacketHeader& header, uint8_t* buf, size_t bufLen) {
    if (buf == nullptr) {
        return Status::error(Code::InvalidArg, "encodePacketHeader: buf must not be null");
    }
    if (bufLen < PACKET_HEADER_SIZE) {
        return Status::error(Code::InvalidArg,
                             "encodePacketHeader: buffer is smaller than PACKET_HEADER_SIZE");
    }
    if (header.version != PROTOCOL_VERSION) {
        return Status::error(Code::InvalidArg,
                             "encodePacketHeader: version does not match PROTOCOL_VERSION");
    }
    if (!isKnownPacketType(static_cast<uint8_t>(header.type))) {
        return Status::error(Code::InvalidArg,
                             "encodePacketHeader: type is outside the defined range");
    }

    buf[0] = header.version;
    buf[1] = static_cast<uint8_t>(header.type);

    const uint16_t streamIdNet = htons(header.streamId);
    const uint32_t seqNet = htonl(header.seq);
    const uint32_t timestampNet = htonl(header.timestampMs);

    std::memcpy(buf + 2, &streamIdNet, sizeof(streamIdNet));
    std::memcpy(buf + 4, &seqNet, sizeof(seqNet));
    std::memcpy(buf + 8, &timestampNet, sizeof(timestampNet));
    return Status::ok();
}

Status decodePacketHeader(const uint8_t* buf, size_t bufLen, PacketHeader& out) {
    if (buf == nullptr) {
        return Status::error(Code::InvalidArg, "decodePacketHeader: buf must not be null");
    }
    if (bufLen < PACKET_HEADER_SIZE) {
        return Status::error(Code::InvalidArg,
                             "decodePacketHeader: buffer is smaller than PACKET_HEADER_SIZE");
    }
    if (buf[0] != PROTOCOL_VERSION) {
        return Status::error(Code::NetError, "decodePacketHeader: unsupported protocol version");
    }
    if (!isKnownPacketType(buf[1])) {
        return Status::error(Code::NetError, "decodePacketHeader: unknown packet type");
    }

    uint16_t streamIdNet;
    uint32_t seqNet;
    uint32_t timestampNet;
    std::memcpy(&streamIdNet, buf + 2, sizeof(streamIdNet));
    std::memcpy(&seqNet, buf + 4, sizeof(seqNet));
    std::memcpy(&timestampNet, buf + 8, sizeof(timestampNet));

    PacketHeader decoded;
    decoded.version = buf[0];
    decoded.type = static_cast<PacketType>(buf[1]);
    decoded.streamId = ntohs(streamIdNet);
    decoded.seq = ntohl(seqNet);
    decoded.timestampMs = ntohl(timestampNet);
    out = decoded;

    return Status::ok();
}

Status encodeDataHeader(const DataHeader& header, uint8_t* buf, size_t bufLen) {
    if (buf == nullptr) {
        return Status::error(Code::InvalidArg, "encodeDataHeader: buf must not be null");
    }
    if (bufLen < DATA_HEADER_SIZE) {
        return Status::error(Code::InvalidArg,
                             "encodeDataHeader: buffer is smaller than DATA_HEADER_SIZE");
    }
    if (header.fragCount == 0) {
        return Status::error(Code::InvalidArg, "encodeDataHeader: fragCount must be at least 1");
    }
    if (header.fragIndex >= header.fragCount) {
        return Status::error(Code::InvalidArg,
                             "encodeDataHeader: fragIndex must be smaller than fragCount");
    }

    const uint32_t frameIdNet = htonl(header.frameId);
    const uint16_t fragIndexNet = htons(header.fragIndex);
    const uint16_t fragCountNet = htons(header.fragCount);

    std::memcpy(buf, &frameIdNet, sizeof(frameIdNet));
    std::memcpy(buf + 4, &fragIndexNet, sizeof(fragIndexNet));
    std::memcpy(buf + 6, &fragCountNet, sizeof(fragCountNet));
    buf[8] = header.flags;

    return Status::ok();
}

Status decodeDataHeader(const uint8_t* buf, size_t bufLen, DataHeader& out) {
    if (buf == nullptr) {
        return Status::error(Code::InvalidArg, "decodeDataHeader: buf must not be null");
    }
    if (bufLen < DATA_HEADER_SIZE) {
        return Status::error(Code::InvalidArg,
                             "decodeDataHeader: buffer is smaller than DATA_HEADER_SIZE");
    }

    uint32_t frameIdNet;
    uint16_t fragIndexNet;
    uint16_t fragCountNet;
    std::memcpy(&frameIdNet, buf, sizeof(frameIdNet));
    std::memcpy(&fragIndexNet, buf + 4, sizeof(fragIndexNet));
    std::memcpy(&fragCountNet, buf + 6, sizeof(fragCountNet));

    const uint16_t fragIndex = ntohs(fragIndexNet);
    const uint16_t fragCount = ntohs(fragCountNet);
    if (fragCount == 0) {
        return Status::error(Code::NetError, "decodeDataHeader: received fragCount is zero");
    }
    if (fragIndex >= fragCount) {
        return Status::error(Code::NetError,
                             "decodeDataHeader: received fragIndex is outside fragCount");
    }

    DataHeader decoded;
    decoded.frameId = ntohl(frameIdNet);
    decoded.fragIndex = fragIndex;
    decoded.fragCount = fragCount;
    decoded.flags = buf[8];
    out = decoded;

    return Status::ok();
}

bool seqNewerThan(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

Status encodeNackPacket(const PacketHeader& header, const std::vector<uint32_t>& missing,
                        uint8_t* buf, size_t bufLen, size_t& outLen) {
    // TODO(M4.1):
    //   1. 参数校验(全部前置, 失败时 buf 和 outLen 都不动):
    //      buf == nullptr -> InvalidArg
    //      missing.empty() -> InvalidArg     (空 NACK 没有意义)
    //      header.type != PacketType::Nack -> InvalidArg
    //        ^ 和 encodePacketHeader 查 type 是同一条理由: 本端写错时,
    //          现象会是"对端收到 NACK 不理睬", 排查方向指向对端, bug 却在这里
    //   2. 先按贪心分条算出需要几条, 超过 MAX_NACK_ENTRIES -> Internal
    //      (缓冲是按线上长度算好的, 装不下只可能是本端算错了; 调用方超了应当先截断)
    //   3. encodePacketHeader 写前 12 字节
    //   4. 写 entryCount(2 字节, 网络序)
    //   5. 逐条写 pid(4) + blp(2), 全部网络序
    //   6. outLen = 12 + 2 + 6 * entryCount
    //
    // 贪心分条: 取第一个未覆盖的 seq 作 pid, 遍历后面的 seq, 距离 d = s - pid 落在
    //   [1, 16] 的置进 blp 的第 (d-1) 位, 然后跳到第一个 d > 16 的 seq 作下一条的 pid。
    //   **距离要用无符号减法算**, seq 会回绕, 直接比大小会在回绕点分错条。
    //   不要求最优分条 —— 最优解省不下几个字节, 分条逻辑越绕越容易出错。
    (void)header;
    (void)missing;
    (void)buf;
    (void)bufLen;
    (void)outLen;
    return Status::error(Code::Internal, "encodeNackPacket: not implemented");
}

Status decodeNackPacket(const uint8_t* buf, size_t bufLen, std::vector<uint32_t>& out) {
    // TODO(M4.1):
    //   out.clear() 先做 —— 调用方复用这个 vector
    //   1. buf == nullptr -> InvalidArg
    //   2. decodePacketHeader 拿到头(它自己会查版本和类型合法性);
    //      header.type != PacketType::Nack -> NetError(对端发错了, 不是本端参数错)
    //   3. bufLen < PACKET_HEADER_SIZE + NACK_HEADER_SIZE -> NetError
    //   4. 读 entryCount; 为 0 或 > MAX_NACK_ENTRIES -> NetError
    //   5. **长度必须精确相符**:
    //        bufLen != PACKET_HEADER_SIZE + NACK_HEADER_SIZE + 6 * entryCount -> NetError
    //      多一个字节少一个字节都算畸形。拿 entryCount 去循环读数组之前必须先确认它和
    //      实际长度相符, 否则就是一次越界读 —— UDP 上收到的每个字节都是不可信输入。
    //   6. 逐条读 pid + blp, 展开成 seq 列表: 先 push pid, 再对 blp 的第 i 位
    //      (i = 0..15) 为 1 的 push (pid + 1 + i)。顺序与编码时一致(从老到新)。
    (void)buf;
    (void)bufLen;
    (void)out;
    return Status::error(Code::Internal, "decodeNackPacket: not implemented");
}
