/**
 * @file    FecDecoder.cpp
 * @brief   FecDecoder.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/FecDecoder.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "modules/transport/Packet.h"

FecDecoder::FecDecoder(FecDecoderConfig config) : config_(std::move(config)) {}

void FecDecoder::onDataPacket(const uint8_t* packet, size_t len) {
    if (!enabled() || packet == nullptr ||
        len <= PACKET_HEADER_SIZE + DATA_HEADER_SIZE) {
        return;
    }

    recent_.emplace_back(packet, packet + len);
    while (recent_.size() > config_.recentPackets) recent_.pop_front();
    stats_.recentPackets = recent_.size();
}

bool FecDecoder::onFecPacket(const uint8_t* packet, size_t len, PacketBuffer& out) {
    if (!enabled()) return false;

    PacketHeader fecPacketHeader;
    if (!decodePacketHeader(packet, len, fecPacketHeader).isOk() ||
        fecPacketHeader.type != PacketType::Fec) {
        ++stats_.fecPacketsMalformed;
        return false;
    }

    FecHeader fecHeader;
    if (!decodeFecHeader(packet + PACKET_HEADER_SIZE, len - PACKET_HEADER_SIZE,
                         fecHeader)
             .isOk()) {
        ++stats_.fecPacketsMalformed;
        return false;
    }
    ++stats_.fecPacketsReceived;

    struct PresentMember {
        const PacketBuffer* packet = nullptr;
        PacketHeader packetHeader;
        DataHeader dataHeader;
        uint16_t groupOffset = 0;
    };
    std::vector<PresentMember> members;
    members.reserve(std::min(recent_.size(), static_cast<size_t>(fecHeader.groupSize)));

    for (const PacketBuffer& recentPacket : recent_) {
        PacketHeader packetHeader;
        if (!decodePacketHeader(recentPacket.data(), recentPacket.size(), packetHeader)
                 .isOk() ||
            packetHeader.type != PacketType::Data) {
            continue;
        }

        const uint32_t groupOffset = packetHeader.seq - fecHeader.groupBaseSeq;
        if (groupOffset >= fecHeader.groupSize) continue;
        const auto duplicate =
            std::find_if(members.begin(), members.end(), [groupOffset](const PresentMember& m) {
                return m.groupOffset == groupOffset;
            });
        if (duplicate != members.end()) continue;

        DataHeader dataHeader;
        if (!decodeDataHeader(recentPacket.data() + PACKET_HEADER_SIZE,
                              recentPacket.size() - PACKET_HEADER_SIZE, dataHeader)
                 .isOk()) {
            continue;
        }

        PresentMember member;
        member.packet = &recentPacket;
        member.packetHeader = packetHeader;
        member.dataHeader = dataHeader;
        member.groupOffset = static_cast<uint16_t>(groupOffset);
        members.push_back(member);
    }

    if (members.size() == fecHeader.groupSize) {
        ++stats_.groupsComplete;
        return false;
    }
    if (members.size() <= static_cast<size_t>(fecHeader.groupSize - 2)) {
        ++stats_.groupsUnrecoverable;
        return false;
    }

    uint16_t missingOffset = 0;
    for (; missingOffset < fecHeader.groupSize; ++missingOffset) {
        const auto found =
            std::find_if(members.begin(), members.end(), [missingOffset](const PresentMember& m) {
                return m.groupOffset == missingOffset;
            });
        if (found == members.end()) break;
    }
    const uint32_t missingSeq = fecHeader.groupBaseSeq + missingOffset;

    constexpr size_t fecPayloadOffset = PACKET_HEADER_SIZE + FEC_HEADER_SIZE;
    constexpr size_t dataPayloadOffset = PACKET_HEADER_SIZE + DATA_HEADER_SIZE;
    std::vector<uint8_t> recoveredPayload(packet + fecPayloadOffset, packet + len);
    uint16_t recoveredPayloadLen = fecHeader.payloadLenXor;
    for (const PresentMember& member : members) {
        const size_t payloadLen = member.packet->size() - dataPayloadOffset;
        if (payloadLen > recoveredPayload.size()) {
            ++stats_.groupsUnrecoverable;
            return false;
        }
        recoveredPayloadLen ^= static_cast<uint16_t>(payloadLen);
        for (size_t byte = 0; byte < payloadLen; ++byte) {
            recoveredPayload[byte] ^= (*member.packet)[dataPayloadOffset + byte];
        }
    }
    if (recoveredPayloadLen == 0 || recoveredPayloadLen > recoveredPayload.size()) {
        ++stats_.groupsUnrecoverable;
        return false;
    }

    const PresentMember& reference = members.front();
    const uint32_t recoveredFragIndex =
        static_cast<uint32_t>(reference.dataHeader.fragIndex) +
        (missingSeq - reference.packetHeader.seq);
    if (recoveredFragIndex >= reference.dataHeader.fragCount) {
        ++stats_.groupsUnrecoverable;
        return false;
    }

    PacketHeader recoveredPacketHeader;
    recoveredPacketHeader.type = PacketType::Data;
    recoveredPacketHeader.streamId = reference.packetHeader.streamId;
    recoveredPacketHeader.seq = missingSeq;
    recoveredPacketHeader.timestampMs = reference.packetHeader.timestampMs;

    DataHeader recoveredDataHeader;
    recoveredDataHeader.frameId = reference.dataHeader.frameId;
    recoveredDataHeader.fragIndex = static_cast<uint16_t>(recoveredFragIndex);
    recoveredDataHeader.fragCount = reference.dataHeader.fragCount;
    recoveredDataHeader.flags = reference.dataHeader.flags;

    PacketBuffer recovered(dataPayloadOffset + recoveredPayloadLen);
    if (!encodePacketHeader(recoveredPacketHeader, recovered.data(), recovered.size()).isOk() ||
        !encodeDataHeader(recoveredDataHeader, recovered.data() + PACKET_HEADER_SIZE,
                          recovered.size() - PACKET_HEADER_SIZE)
             .isOk()) {
        ++stats_.groupsUnrecoverable;
        return false;
    }
    std::memcpy(recovered.data() + dataPayloadOffset, recoveredPayload.data(),
                recoveredPayloadLen);
    out = std::move(recovered);
    ++stats_.groupsRecovered;
    ++stats_.packetsRecovered;
    return true;
}

void FecDecoder::reset() {
    recent_.clear();
    stats_.recentPackets = 0;
}
