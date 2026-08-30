/**
 * @file    FecEncoder.cpp
 * @brief   FecEncoder.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/FecEncoder.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "modules/transport/Packet.h"

FecEncoder::FecEncoder(FecEncoderConfig config) : config_(std::move(config)) {}

Status FecEncoder::buildForFrame(const std::vector<PacketBuffer>& packets, uint32_t baseSeq,
                                 uint16_t streamId, uint32_t timestampMs,
                                 std::vector<PacketBuffer>& out) {
    out.clear();
    if (!enabled() || packets.empty()) return Status::ok();

    constexpr size_t dataHeaderBytes = PACKET_HEADER_SIZE + DATA_HEADER_SIZE;
    for (const PacketBuffer& packet : packets) {
        if (packet.size() < dataHeaderBytes) {
            return Status::error(
                Code::InvalidArg,
                "FecEncoder::buildForFrame: DATA packet is smaller than its headers");
        }
    }

    ++stats_.framesSeen;
    const size_t groupSize = config_.groupSize;
    const size_t groupCount =
        packets.size() / groupSize + (packets.size() % groupSize != 0 ? 1 : 0);
    std::vector<uint8_t> xorBuf;

    for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        const size_t first = groupIndex * groupSize;
        const size_t last = std::min(first + groupSize, packets.size());
        const size_t packetsInGroup = last - first;
        if (packetsInGroup < config_.minGroupSize) {
            ++stats_.groupsSkipped;
            continue;
        }

        size_t maxPayload = 0;
        for (size_t i = first; i < last; ++i) {
            maxPayload = std::max(maxPayload, packets[i].size() - dataHeaderBytes);
        }
        xorBuf.assign(maxPayload, 0);

        uint16_t payloadLenXor = 0;
        for (size_t i = first; i < last; ++i) {
            const PacketBuffer& packet = packets[i];
            const size_t payloadLen = packet.size() - dataHeaderBytes;
            payloadLenXor ^= static_cast<uint16_t>(payloadLen);
            for (size_t byte = 0; byte < payloadLen; ++byte) {
                xorBuf[byte] ^= packet[dataHeaderBytes + byte];
            }
        }

        PacketHeader packetHeader;
        packetHeader.type = PacketType::Fec;
        packetHeader.streamId = streamId;
        packetHeader.seq = nextFecSeq_++;
        packetHeader.timestampMs = timestampMs;

        FecHeader fecHeader;
        fecHeader.groupBaseSeq = baseSeq + static_cast<uint32_t>(first);
        fecHeader.groupSize = static_cast<uint16_t>(packetsInGroup);
        fecHeader.payloadLenXor = payloadLenXor;
        fecHeader.groupIndex = static_cast<uint8_t>(groupIndex);
        fecHeader.groupCount = static_cast<uint8_t>(groupCount);
        fecHeader.reserved = 0;

        out.emplace_back(PACKET_HEADER_SIZE + FEC_HEADER_SIZE + maxPayload);
        PacketBuffer& fecPacket = out.back();
        const Status packetStatus =
            encodePacketHeader(packetHeader, fecPacket.data(), fecPacket.size());
        if (!packetStatus.isOk()) {
            return Status::error(Code::Internal,
                                 "FecEncoder::buildForFrame: packet header encoding failed: " +
                                     packetStatus.toString());
        }
        const Status fecStatus =
            encodeFecHeader(fecHeader, fecPacket.data() + PACKET_HEADER_SIZE,
                            fecPacket.size() - PACKET_HEADER_SIZE);
        if (!fecStatus.isOk()) {
            return Status::error(Code::Internal,
                                 "FecEncoder::buildForFrame: FEC header encoding failed: " +
                                     fecStatus.toString());
        }
        if (!xorBuf.empty()) {
            std::memcpy(fecPacket.data() + PACKET_HEADER_SIZE + FEC_HEADER_SIZE,
                        xorBuf.data(), xorBuf.size());
        }

        ++stats_.fecPacketsBuilt;
        stats_.fecBytes += fecPacket.size();
    }

    return Status::ok();
}
