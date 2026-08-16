/**
 * @file    SenderPipeline.cpp
 * @brief   SenderPipeline.h 的实现
 * @author  zzj
 * @date    2026-08-04
 */

#include "app/sender/SenderPipeline.h"
#include "common/BoundedQueue.h"
#include "common/Clock.h"
#include "common/Logger.h"
#include "common/Status.h"
#include "modules/capture/Frame.h"
#include "modules/encode/Encoder.h"

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

    // TODO(M2): target.ip 非空时创建发送队列:
    //  sendQueue_ = std::make_unique<BoundedQueue<std::unique_ptr<EncodedFrame>>>(
    //      config_.sendQueueCapacity);
    //  为空时保持 nullptr —— encodeLoop 靠它判断这一级存不存在, 行为与 M1 完全一致。

    status =  openResources();
    if(!status.isOk()) return status;
    uint64_t start = steadyNowMs();

    std::thread capture ([this,&stopRequested](){
        captureLoop(stopRequested);
    });
    std::thread encode([this](){
        encodeLoop();
    });

    // TODO(M2): sendQueue_ 非空时再起一个 std::thread 跑 sendLoop(), 并在 encode.join()
    //  之后 join 它。顺序不能反: encodeLoop 退出时才会 close(sendQueue_),
    //  先 join 发送线程会等一个还没被关的队列, 直接死锁。

    capture.join();
    encode.join();

    stats_.queuePeak = queue_->peak();
    // TODO(M2): sendQueue_ 非空时 stats_.sendQueuePeak = sendQueue_->peak();
    uint64_t end = steadyNowMs();
    stats_.elapsedMs = end - start;
    closeResources();

    if(!captureStatus_.isOk()) return captureStatus_;
    if(!encodeStatus_.isOk()) return encodeStatus_;
    // TODO(M2): if(!sendStatus_.isOk()) return sendStatus_;
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

    // TODO(M2): target.ip 非空时校验:
    //  - config_.sendQueueCapacity <= 0 → InvalidArg(同 queueCapacity, 负数会变成巨大容量);
    //  - config_.target.port == 0 → InvalidArg(0 号端口发不出去, 而且是个典型的
    //    "参数没填"信号 —— 让它在启动时炸掉, 而不是跑起来之后每个包都失败)。

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

    // 摄像头驱动可以不理会请求值, 只给相近的分辨率/帧率。编码器必须按**实际**参数打开:
    // 按 640x480 开的编码器喂进 320x240 的帧, 出来的不是报错而是花屏 —— 更难查。
    const SourceConfig& actual = source_->actualConfig();
    if(actual.width != config_.encoder.width || actual.height != config_.encoder.height ||
       actual.fps != config_.encoder.fps) {
        LOG_INFO("sender",
                 "source negotiated %dx%d@%dfps (requested %dx%d@%dfps), "
                 "opening encoder with the negotiated values",
                 actual.width, actual.height, actual.fps,
                 config_.encoder.width, config_.encoder.height, config_.encoder.fps);
    }

    // 覆盖 config_ 本身而不是用局部变量: 之后的日志和统计读到的都应该是真正在跑的参数,
    // 留着一份对不上的"请求值"迟早会有人拿它去算东西。
    config_.encoder.width = actual.width;
    config_.encoder.height = actual.height;
    config_.encoder.fps = actual.fps;

    st = encoder_.open(config_.encoder);
    if(!st.isOk()) {
        closeResources();
        return st;
    }

    // TODO(M2): target.ip 非空时 socket_.open(), 失败照样要 closeResources() 再返回。
    //  不需要 bind: 发送端的源端口由内核在首次 sendTo 时分配。
    //  M4 要收 NACK/PLI 时才需要一个固定端口 —— 那时再加, 现在加是猜需求。

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
            // TODO(M2): sendQueue_ 非空时把这一帧移进发送队列:
            //  sendQueue_->push(std::make_unique<EncodedFrame>(std::move(it)));
            //  **必须在 writeEncodedFrame 之后**: 移动过后 it.data 就是空的了,
            //  反过来写 dump 会写出 0 字节, 而且只在开了发送时才复现。
            //  push 返回 false 表示队列已关闭, 按 break 处理。
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
    // TODO(M2): sendQueue_ 非空时 sendQueue_->close();
    //  漏了这行, 发送线程会永远阻塞在 pop() 上, run() 的 join 再也回不来 ——
    //  现象是"跑完最后一帧程序不退出", 而且 Ctrl-C 也没用(停止标志只管采集线程)。
    //  flush 出来的帧也要先入队再关, 顺序反了会丢掉编码器缓冲里的最后几帧。
}

void SenderPipeline::sendLoop() {
    // TODO(M2): 步骤:
    //  1. sendStatus_ = Status::ok();
    //  2. while (sendQueue_->pop(frame)) {
    //       - 构造 EncodedFrameView: data/len 指向 frame->data, frameId/captureMs/isKey 透传;
    //       - packetizer_.packetize(view, packets_) —— packets_ 是成员, 跨帧复用;
    //         失败(空帧等)记 sendStatus_ 并 break, 那是本端 bug 不是网络问题;
    //       - 逐包 socket_.sendTo(config_.target, pkt.data(), pkt.size()):
    //           成功 → ++stats_.packetsSent;
    //           失败 → ++stats_.sendErrors, **继续发下一个**, 不中断管线。
    //     }
    //
    //  为什么单包失败不算错误: UDP 上丢包是常态, 一个 sendto 返回 -1 (缓冲区满、
    //  ICMP 不可达) 不代表管线该停。socket 真坏了的话每个包都会失败, sendErrors
    //  的数量会明白地告诉你 —— 让计数说话, 不要让第一个错误就把整条流掐掉。
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
    socket_.close();  // 幂等, 没 open 过也能调

    if(source_) source_->close();
}
