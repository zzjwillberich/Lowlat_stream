/**
 * @file    ReceiverPipeline.cpp
 * @brief   ReceiverPipeline.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "app/receiver/ReceiverPipeline.h"

#include <utility>

#include "common/Clock.h"
#include "modules/transport/Packet.h"

ReceiverPipeline::ReceiverPipeline(ReceiverPipelineConfig config)
    : config_(std::move(config)), assembler_(config_.maxPendingFrames) {}

Status ReceiverPipeline::open() {
    Status status = validateConfig();
    if (!status.isOk()) {
        return status;
    }

    status = socket_.open();
    if (!status.isOk()) {
        return status;
    }

    status = socket_.bind(config_.listen);
    if (!status.isOk()) {
        closeResources();
        return status;
    }

    if (!config_.h264DumpPath.empty()) {
        h264File_.open(config_.h264DumpPath, std::ios::binary | std::ios::trunc);
        if (!h264File_) {
            closeResources();
            return Status::error(Code::IoError,
                                 "ReceiverPipeline::open: failed to open H.264 dump: " +
                                     config_.h264DumpPath);
        }
    }

    recvBuf_.resize(MAX_DATA_PACKET_SIZE);
    opened_ = true;
    return Status::ok();
}

Status ReceiverPipeline::run(const std::atomic<bool>& stopRequested) {
    if (!opened_) {
        return Status::error(Code::Closed, "ReceiverPipeline::run: pipeline is not open");
    }
    if (hasRun_) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline::run: may only be called once");
    }
    hasRun_ = true;

    const uint64_t start = steadyNowMs();
    uint64_t lastPacketMs = start;
    Status result = Status::ok();
    bool finished = false;

    while (!stopRequested.load() && !finished) {
        size_t receivedBytes = 0;
        Endpoint from;
        const Status receiveStatus =
            socket_.recvFrom(recvBuf_.data(), recvBuf_.size(), receivedBytes, from,
                             config_.recvTimeoutMs);

        if (receiveStatus.code() == Code::Timeout) {
            const uint64_t now = steadyNowMs();
            if (config_.idleTimeoutMs > 0 &&
                now - lastPacketMs >= static_cast<uint64_t>(config_.idleTimeoutMs)) {
                break;
            }
            continue;
        }
        if (!receiveStatus.isOk()) {
            ++stats_.recvErrors;
            continue;
        }

        lastPacketMs = steadyNowMs();
        const Status assembleStatus = assembler_.offer(recvBuf_.data(), receivedBytes);
        (void)assembleStatus;

        AssembledFrame frame;
        while (assembler_.pop(frame)) {
            result = writeFrame(frame);
            if (!result.isOk()) {
                finished = true;
                break;
            }

            if (config_.maxFrames > 0 &&
                stats_.framesWritten >= static_cast<uint64_t>(config_.maxFrames)) {
                finished = true;
                break;
            }
        }
    }

    stats_.assembler = assembler_.stats();
    stats_.elapsedMs = steadyNowMs() - start;
    closeResources();
    return result;
}

uint16_t ReceiverPipeline::boundPort() const {
    return socket_.localEndpoint().port;
}

Status ReceiverPipeline::validateConfig() const {
    if (config_.recvTimeoutMs <= 0) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: receive timeout must be positive");
    }
    if (config_.maxFrames < 0) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: max frames must be non-negative");
    }
    if (config_.idleTimeoutMs < 0) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: idle timeout must be non-negative");
    }
    if (config_.maxPendingFrames == 0) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: max pending frames must be positive");
    }
    return Status::ok();
}

Status ReceiverPipeline::writeFrame(const AssembledFrame& frame) {
    if (h264File_.is_open()) {
        h264File_.write(reinterpret_cast<const char*>(frame.data.data()), frame.data.size());
        if (!h264File_) {
            return Status::error(Code::IoError,
                                 "ReceiverPipeline::writeFrame: failed to write H.264 dump: " +
                                     config_.h264DumpPath);
        }
    }

    ++stats_.framesWritten;
    stats_.bytesWritten += frame.data.size();
    if (frame.isKey) {
        ++stats_.keyFrames;
    }
    return Status::ok();
}

void ReceiverPipeline::closeResources() {
    if (h264File_.is_open()) {
        h264File_.close();
    }
    socket_.close();
    opened_ = false;
}
