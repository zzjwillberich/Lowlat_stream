/**
 * @file    FrameAssembler.cpp
 * @brief   FrameAssembler.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "modules/transport/FrameAssembler.h"

#include <utility>

#include "modules/transport/Packet.h"

uint64_t AssemblerStats::expectedPackets() const {
    return started ? static_cast<uint64_t>(static_cast<uint32_t>(highestSeq - baseSeq)) + 1 : 0;
}

uint64_t AssemblerStats::packetsLost() const {
    const uint64_t expected = expectedPackets();
    return expected > packetsReceived ? expected - packetsReceived : 0;
}

FrameAssembler::FrameAssembler(size_t maxPendingFrames)
    : maxPendingFrames_(maxPendingFrames == 0 ? 1 : maxPendingFrames) {}

Status FrameAssembler::offer(const uint8_t* packet, size_t len) {
    if (packet == nullptr) {
        return Status::error(Code::InvalidArg, "FrameAssembler::offer: packet must not be null");
    }

    constexpr size_t headerSize = PACKET_HEADER_SIZE + DATA_HEADER_SIZE;
    if (len <= headerSize) {
        ++stats_.packetsMalformed;
        return Status::error(Code::NetError,
                             "FrameAssembler::offer: DATA packet has no payload or is too short");
    }
    if (len > MAX_DATA_PACKET_SIZE) {
        ++stats_.packetsMalformed;
        return Status::error(Code::NetError,
                             "FrameAssembler::offer: DATA packet exceeds MAX_DATA_PACKET_SIZE");
    }

    PacketHeader packetHeader;
    const Status packetStatus = decodePacketHeader(packet, len, packetHeader);
    if (!packetStatus.isOk()) {
        ++stats_.packetsMalformed;
        return Status::error(Code::NetError,
                             "FrameAssembler::offer: invalid packet header: " +
                                 packetStatus.message());
    }
    if (packetHeader.type != PacketType::Data) {
        return Status::error(Code::InvalidArg,
                             "FrameAssembler::offer: packet type must be DATA");
    }

    DataHeader dataHeader;
    const Status dataStatus =
        decodeDataHeader(packet + PACKET_HEADER_SIZE, len - PACKET_HEADER_SIZE, dataHeader);
    if (!dataStatus.isOk()) {
        ++stats_.packetsMalformed;
        return Status::error(Code::NetError,
                             "FrameAssembler::offer: invalid DATA header: " +
                                 dataStatus.message());
    }

    ++stats_.packetsReceived;
    trackSeq(packetHeader.seq);

    if (hasRetired_ && !seqNewerThan(dataHeader.frameId, retiredFrameId_)) {
        ++stats_.packetsTooLate;
        return Status::ok();
    }

    auto it = pending_.find(dataHeader.frameId);
    if (it != pending_.end()) {
        if (it->second.completed) {
            ++stats_.packetsDuplicate;
            return Status::ok();
        }
        if (it->second.fragCount != dataHeader.fragCount) {
            ++stats_.packetsMalformed;
            return Status::error(
                Code::NetError,
                "FrameAssembler::offer: fragCount contradicts earlier fragments of the frame");
        }
    } else {
        if (pending_.size() >= maxPendingFrames_) {
            const uint32_t oldestFrameId = oldestPendingFrameId();
            if (!seqNewerThan(dataHeader.frameId, oldestFrameId)) {
                ++stats_.packetsTooLate;
                return Status::ok();
            }
            evictOldest();
        }

        PendingFrame pendingFrame;
        pendingFrame.fragCount = dataHeader.fragCount;
        pendingFrame.fragments.resize(dataHeader.fragCount);
        it = pending_.emplace(dataHeader.frameId, std::move(pendingFrame)).first;
    }

    PendingFrame& pendingFrame = it->second;
    std::vector<uint8_t>& fragment = pendingFrame.fragments[dataHeader.fragIndex];
    if (!fragment.empty()) {
        ++stats_.packetsDuplicate;
        return Status::ok();
    }

    const size_t payloadLen = len - headerSize;
    const uint8_t* payload = packet + headerSize;
    fragment.assign(payload, payload + payloadLen);

    if (pendingFrame.receivedFragments == 0) {
        pendingFrame.timestampMs = packetHeader.timestampMs;
        pendingFrame.isKey = (dataHeader.flags & DataHeader::FLAG_KEYFRAME) != 0;
    }
    ++pendingFrame.receivedFragments;
    pendingFrame.totalBytes += payloadLen;

    if (pendingFrame.receivedFragments != pendingFrame.fragCount) {
        return Status::ok();
    }

    AssembledFrame frame;
    frame.data.reserve(pendingFrame.totalBytes);
    for (const auto& storedFragment : pendingFrame.fragments) {
        frame.data.insert(frame.data.end(), storedFragment.begin(), storedFragment.end());
    }
    frame.frameId = dataHeader.frameId;
    frame.timestampMs = pendingFrame.timestampMs;
    frame.isKey = pendingFrame.isKey;
    ready_.push_back(std::move(frame));
    ++stats_.framesCompleted;

    pendingFrame.completed = true;
    std::vector<std::vector<uint8_t>>().swap(pendingFrame.fragments);
    return Status::ok();
}

bool FrameAssembler::pop(AssembledFrame& out) {
    if (ready_.empty()) {
        return false;
    }

    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
}

void FrameAssembler::reset() {
    pending_.clear();
    ready_.clear();
    retiredFrameId_ = 0;
    hasRetired_ = false;
    stats_ = AssemblerStats{};
}

void FrameAssembler::trackSeq(uint32_t seq) {
    if (!stats_.started) {
        stats_.baseSeq = seq;
        stats_.highestSeq = seq;
        stats_.started = true;
        return;
    }

    if (seqNewerThan(seq, stats_.highestSeq)) {
        stats_.highestSeq = seq;
    }
}

uint32_t FrameAssembler::oldestPendingFrameId() const {
    uint32_t oldestFrameId = pending_.begin()->first;
    for (const auto& entry : pending_) {
        if (seqNewerThan(oldestFrameId, entry.first)) {
            oldestFrameId = entry.first;
        }
    }
    return oldestFrameId;
}

void FrameAssembler::evictOldest() {
    if (pending_.empty()) {
        return;
    }

    const uint32_t oldestFrameId = oldestPendingFrameId();
    const auto oldest = pending_.find(oldestFrameId);

    if (!oldest->second.completed) {
        ++stats_.framesDropped;
    }
    retiredFrameId_ = oldestFrameId;
    hasRetired_ = true;
    pending_.erase(oldest);
}
