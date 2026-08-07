/**
 * @file    SenderPipeline.cpp
 * @brief   SenderPipeline.h 的实现
 * @author  zzj
 * @date    2026-08-04
 */

#include "app/sender/SenderPipeline.h"
#include "common/BoundedQueue.h"
#include "common/Clock.h"
#include "common/Status.h"
#include "modules/capture/Frame.h"
#include "modules/encode/Encoder.h"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

SenderPipeline::SenderPipeline(std::unique_ptr<ISource> source, SenderPipelineConfig config)
    : source_(std::move(source)), config_(std::move(config)) {}

Status SenderPipeline::run(const std::atomic<bool>& stopRequested) {
    if(hasRun_) return Status::error(Code::InvalidArg,"run() may only be called once");
    hasRun_ = true;

    Status status = validateConfig();
    if(!status.isOk()) return status;

    queue_ = std::make_unique<BoundedQueue<std::unique_ptr<RawFrame>>>(config_.queueCapacity);


    status =  openResources();
    if(!status.isOk()) return status;
    uint64_t start = steadyNowMs();

    std::thread capture ([this,&stopRequested](){
        captureLoop(stopRequested);
    });
    std::thread encode([this](){
        encodeLoop();
    });

    capture.join();
    encode.join();

    stats_.queuePeak = queue_->peak();
    uint64_t end = steadyNowMs();
    stats_.elapsedMs = end - start;
    closeResources();

    if(!captureStatus_.isOk()) return captureStatus_;
    if(!encodeStatus_.isOk()) return encodeStatus_;
    return Status::ok();
}

Status SenderPipeline::validateConfig() const {
    if (!source_) {
        return Status::error(Code::InvalidArg, "SenderPipeline: source must not be null");
    }

    // queue_ 的构造参数是 size_t；必须在转换前拦住 0 和负数，否则负数会变成巨大容量。
    if (config_.queueCapacity <= 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: queue capacity must be positive");
    }

    // 0 的语义是持续运行，只有负数不合法。
    if (config_.maxFrames < 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: max frames must be non-negative");
    }

    const SourceConfig& sourceConfig = config_.source;
    if (sourceConfig.width <= 0 || sourceConfig.height <= 0 ||
        sourceConfig.width % 2 != 0 || sourceConfig.height % 2 != 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: source width/height must be positive and even");
    }
    if (sourceConfig.fps <= 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: source fps must be positive");
    }

    const EncoderConfig& encoderConfig = config_.encoder;
    if (encoderConfig.width <= 0 || encoderConfig.height <= 0 ||
        encoderConfig.width % 2 != 0 || encoderConfig.height % 2 != 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: encoder width/height must be positive and even");
    }
    if (encoderConfig.fps <= 0 || encoderConfig.bitrateKbps <= 0 || encoderConfig.gop <= 0) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: encoder fps/bitrate/gop must be positive");
    }

    if (sourceConfig.width != encoderConfig.width ||
        sourceConfig.height != encoderConfig.height) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: source and encoder geometry must match");
    }
    if (sourceConfig.fps != encoderConfig.fps) {
        return Status::error(Code::InvalidArg,
                             "SenderPipeline: source and encoder fps must match");
    }

    return Status::ok();
}

Status SenderPipeline::openResources() {
    Status st;

    st = source_->open(config_.source);
    if(!st.isOk()) {
        closeResources();
        return st;
    }

    st = encoder_.open(config_.encoder);
    if(!st.isOk()) {
        closeResources();
        return st;
    }

    if(!config_.rawDumpPath.empty()){
        rawFile_.open(config_.rawDumpPath,std::ios::binary | std::ios::trunc);
        if(!rawFile_){
            closeResources();
            return Status::error(Code::IoError,
                                 "SenderPipeline: failed to open raw YUV dump: " +
                                     config_.rawDumpPath);
        }
    }

    if(!config_.h264DumpPath.empty()){
        h264File_.open(config_.h264DumpPath, std::ios::binary | std::ios::trunc);
        if(!h264File_){
            closeResources();
            return Status::error(Code::IoError,
                                 "SenderPipeline: failed to open H.264 dump: " +
                                     config_.h264DumpPath);
        }
    }

    return Status::ok();
}

void SenderPipeline::captureLoop(const std::atomic<bool>& stopRequested) {
    captureStatus_ = Status::ok();

    while(!stopRequested.load() && !abortRequested_.load() && (config_.maxFrames == 0 || stats_.capturedFrames < static_cast<uint64_t>(config_.maxFrames))) {
        std::unique_ptr<RawFrame> frame = std::make_unique<RawFrame>();

        captureStatus_ = source_->readFrame(*frame);
        if(!captureStatus_.isOk()) break;

        bool pushed = queue_->push(std::move(frame));
        if(!pushed) break;
        stats_.capturedFrames++;
    }
    if(captureStatus_.code() == Code::Closed) {
        captureStatus_ = Status::ok();
    }
    queue_->close();
}

void SenderPipeline::encodeLoop() {
    encodeStatus_ = Status::ok();

    std::vector<EncodedFrame> out;
    while(true){
        std::unique_ptr<RawFrame> frame;

        bool flag = queue_->pop(frame);
        if(!flag){
            break;
        }

        if(rawFile_.is_open()){
            rawFile_.write( reinterpret_cast<const char*>(frame->data.data()), frame->data.size());
            if(!rawFile_){
                encodeStatus_ = Status::error(Code::IoError,
                                              "SenderPipeline: failed to write raw YUV dump: " +
                                                  config_.rawDumpPath);
                break;
            }
        }

        encodeStatus_ =  encoder_.encode(*frame, out);
        if(!encodeStatus_.isOk()) break;

        for(auto& it : out){
            encodeStatus_ = writeEncodedFrame(it);
            if(!encodeStatus_.isOk()) break;
            stats_.encodedFrames++;
        }
        if(!encodeStatus_.isOk()) break;

        out.clear();
    }

    if(encodeStatus_.isOk()){
        encodeStatus_ = encoder_.flush(out);
        if(encodeStatus_.isOk()) {       
            for(auto& it : out){
                encodeStatus_ = writeEncodedFrame(it);
                if(!encodeStatus_.isOk()) break;
                stats_.encodedFrames++;
            }
        }
    }

    if(!encodeStatus_.isOk()) abortRequested_.store(true);
    queue_->close();
}

Status SenderPipeline::writeEncodedFrame(const EncodedFrame& frame) {
    if(h264File_.is_open()){
        const char* data = reinterpret_cast<const char*>(frame.data.data());
        h264File_.write(data, frame.data.size());
        if(!h264File_){
            return Status::error(Code::IoError,
                                 "SenderPipeline: failed to write H.264 dump: " +
                                     config_.h264DumpPath);
        }
    }

    stats_.encodedBytes += frame.data.size();
    if(frame.isKey) stats_.keyFrames++;

    return Status::ok();
}

void SenderPipeline::closeResources() {
    if(h264File_.is_open()) h264File_.close();
    if(rawFile_.is_open()) rawFile_.close();

    encoder_.close();

    if(source_) source_->close();
}
