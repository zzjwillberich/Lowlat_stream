/**
 * @file    Packet.cpp
 * @brief   Packet.h 的实现
 * @author  zzj
 * @date    2026-08-13
 */

#include "modules/transport/Packet.h"

#include <arpa/inet.h>
#include <cstring>

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
    if (buf[1] < static_cast<uint8_t>(PacketType::Data) ||
        buf[1] > static_cast<uint8_t>(PacketType::Stats)) {
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
