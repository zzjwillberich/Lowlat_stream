/**
 * @file    Packetizer.cpp
 * @brief   Packetizer.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "modules/transport/Packetizer.h"
#include "modules/transport/Packet.h"

#include <algorithm>
#include <cstring>
#include <limits>

Packetizer::Packetizer(uint16_t streamId, uint32_t initialSeq)
    : streamId_(streamId), nextSeq_(initialSeq) {}

Status Packetizer::packetize(const EncodedFrameView& frame, std::vector<PacketBuffer>& out) {
    if (frame.data == nullptr || frame.len == 0) {
        return Status::error(
            Code::InvalidArg,
            "Packetizer::packetize: frame data must not be null and len must be positive");
    }

    const size_t fragCount = fragmentCount(frame.len);
    if (fragCount > std::numeric_limits<uint16_t>::max()) {
        return Status::error(Code::InvalidArg,
                             "Packetizer::packetize: fragment count exceeds uint16_t wire limit");
    }

    out.resize(fragCount);
    uint32_t seq = nextSeq_;

    for (size_t i = 0; i < fragCount; ++i) {
        const Status fragmentStatus =
            packetizeOneFragment(streamId_, seq, frame, static_cast<uint16_t>(i),
                                 static_cast<uint16_t>(fragCount), false, out[i]);
        if (!fragmentStatus.isOk()) {
            return Status::error(Code::Internal,
                                 "Packetizer::packetize: fragment encoding failed: " +
                                     fragmentStatus.toString());
        }
        ++seq;
    }

    nextSeq_ = seq;
    return Status::ok();
}

size_t Packetizer::fragmentCount(size_t len) {
    return len / MAX_PAYLOAD + (len % MAX_PAYLOAD != 0 ? 1 : 0);
}

Status packetizeOneFragment(uint16_t streamId, uint32_t seq, const EncodedFrameView& frame,
                            uint16_t fragIndex, uint16_t fragCount, bool isRetransmit,
                            PacketBuffer& out) {
    // 校验必须全部放在写 out 之前:
    //   1. 校验全部前置(失败时 out 不动): data 非空、len > 0、fragCount > 0、
    //      fragIndex < fragCount、且 fragIndex * MAX_PAYLOAD < len
    //   2. 算这一片的字节范围: [fragIndex*MAX_PAYLOAD, min((fragIndex+1)*MAX_PAYLOAD, len))
    //      **必须和 packetize() 用同一份算法** —— 两处各写一遍迟早分叉, 而分叉的现象是
    //      重传包和原发包内容不一致, 接收端拼出来的帧是坏的, 表现为偶发花屏。
    //      正确做法: 让 packetize() 也调这个函数, 只留一份切片逻辑。
    //   3. PacketHeader: type=Data, streamId, seq(**传进来的那个**),
    //      timestampMs = static_cast<uint32_t>(frame.captureMs)
    //   4. DataHeader: frameId = static_cast<uint32_t>(frame.frameId), fragIndex, fragCount,
    //      flags = (frame.isKey ? FLAG_KEYFRAME : 0) | (isRetransmit ? FLAG_RETRANSMIT : 0)
    //   5. out.resize(21 + 这一片长度), 写头, 再 memcpy 载荷
    if (frame.data == nullptr || frame.len == 0) {
        return Status::error(
            Code::InvalidArg,
            "packetizeOneFragment: frame data must not be null and len must be positive");
    }
    if (fragCount == 0) {
        return Status::error(Code::InvalidArg,
                             "packetizeOneFragment: fragCount must be positive");
    }
    if (fragIndex >= fragCount) {
        return Status::error(Code::InvalidArg,
                             "packetizeOneFragment: fragIndex must be smaller than fragCount");
    }

    const size_t payloadOffset = static_cast<size_t>(fragIndex) * MAX_PAYLOAD;
    if (payloadOffset >= frame.len) {
        return Status::error(Code::InvalidArg,
                             "packetizeOneFragment: fragment starts beyond frame data");
    }
    const size_t payloadLen = std::min(MAX_PAYLOAD, frame.len - payloadOffset);

    PacketHeader packetHeader;
    packetHeader.type = PacketType::Data;
    packetHeader.streamId = streamId;
    packetHeader.seq = seq;
    packetHeader.timestampMs = static_cast<uint32_t>(frame.captureMs);

    DataHeader dataHeader;
    dataHeader.frameId = static_cast<uint32_t>(frame.frameId);
    dataHeader.fragIndex = fragIndex;
    dataHeader.fragCount = fragCount;
    dataHeader.flags = frame.isKey ? DataHeader::FLAG_KEYFRAME : uint8_t{0};
    if (isRetransmit) dataHeader.flags |= DataHeader::FLAG_RETRANSMIT;

    out.resize(PACKET_HEADER_SIZE + DATA_HEADER_SIZE + payloadLen);
    const Status packetStatus = encodePacketHeader(packetHeader, out.data(), out.size());
    if (!packetStatus.isOk()) {
        return Status::error(Code::Internal,
                             "packetizeOneFragment: encodePacketHeader failed: " +
                                 packetStatus.toString());
    }
    const Status dataStatus =
        encodeDataHeader(dataHeader, out.data() + PACKET_HEADER_SIZE,
                         out.size() - PACKET_HEADER_SIZE);
    if (!dataStatus.isOk()) {
        return Status::error(Code::Internal,
                             "packetizeOneFragment: encodeDataHeader failed: " +
                                 dataStatus.toString());
    }

    std::memcpy(out.data() + PACKET_HEADER_SIZE + DATA_HEADER_SIZE,
                frame.data + payloadOffset, payloadLen);
    return Status::ok();
}
