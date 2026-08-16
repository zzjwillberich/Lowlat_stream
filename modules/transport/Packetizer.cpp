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
        const size_t payloadOffset = i * MAX_PAYLOAD;
        const size_t payloadLen = std::min(MAX_PAYLOAD, frame.len - payloadOffset);

        PacketBuffer& packet = out[i];
        packet.resize(PACKET_HEADER_SIZE + DATA_HEADER_SIZE + payloadLen);

        PacketHeader packetHeader;
        packetHeader.version = PROTOCOL_VERSION;
        packetHeader.type = PacketType::Data;
        packetHeader.streamId = streamId_;
        packetHeader.seq = seq;
        packetHeader.timestampMs = static_cast<uint32_t>(frame.captureMs);

        DataHeader dataHeader;
        dataHeader.frameId = static_cast<uint32_t>(frame.frameId);
        dataHeader.fragIndex = static_cast<uint16_t>(i);
        dataHeader.fragCount = static_cast<uint16_t>(fragCount);
        dataHeader.flags = frame.isKey ? DataHeader::FLAG_KEYFRAME : uint8_t{0};

        const Status packetStatus =
            encodePacketHeader(packetHeader, packet.data(), packet.size());
        if (!packetStatus.isOk()) {
            return Status::error(Code::Internal,
                                 "Packetizer::packetize: encodePacketHeader failed: " +
                                     packetStatus.toString());
        }

        const Status dataStatus =
            encodeDataHeader(dataHeader, packet.data() + PACKET_HEADER_SIZE,
                             packet.size() - PACKET_HEADER_SIZE);
        if (!dataStatus.isOk()) {
            return Status::error(Code::Internal,
                                 "Packetizer::packetize: encodeDataHeader failed: " +
                                     dataStatus.toString());
        }

        std::memcpy(packet.data() + PACKET_HEADER_SIZE + DATA_HEADER_SIZE,
                    frame.data + payloadOffset, payloadLen);
        ++seq;
    }

    nextSeq_ = seq;
    return Status::ok();
}

size_t Packetizer::fragmentCount(size_t len) {
    return len / MAX_PAYLOAD + (len % MAX_PAYLOAD != 0 ? 1 : 0);
}
