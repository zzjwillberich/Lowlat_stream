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

    if (!config_.target.ip.empty()) {
        sendQueue_ =
            std::make_unique<BoundedQueue<std::unique_ptr<EncodedFrame>>>(
                config_.sendQueueCapacity);
    }

    status =  openResources();
    if(!status.isOk()) return status;
    uint64_t start = steadyNowMs();

    std::thread capture ([this,&stopRequested](){
        captureLoop(stopRequested);
    });
    std::thread encode([this](){
        encodeLoop();
    });

    std::thread send;
    if (sendQueue_) {
        send = std::thread([this]() { sendLoop(); });
    }

    capture.join();
    encode.join();
    if (send.joinable()) {
        send.join();
    }

    stats_.queuePeak = queue_->peak();
    if (sendQueue_) {
        stats_.sendQueuePeak = sendQueue_->peak();
    }
    uint64_t end = steadyNowMs();
    stats_.elapsedMs = end - start;
    closeResources();

    if(!captureStatus_.isOk()) return captureStatus_;
    if(!encodeStatus_.isOk()) return encodeStatus_;
    if(!sendStatus_.isOk()) return sendStatus_;
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

    if (!config_.target.ip.empty()) {
        if (config_.sendQueueCapacity <= 0) {
            return Status::error(Code::InvalidArg,
                                 "SenderPipeline: send queue capacity must be positive");
        }
        if (config_.target.port == 0) {
            return Status::error(Code::InvalidArg,
                                 "SenderPipeline: target port must be positive");
        }
        // TODO(M4.1): 重传相关的三条校验
        //   1. retransmit.retentionMs < 0 -> InvalidArg (0 是"关掉重传", 合法)
        //   2. sendPollMs <= 0 -> InvalidArg
        //      0 会让 popFor 变成忙轮询, 烧满一个核; 负数语义未定义。
        //      同 recvTimeoutMs 那条 —— 这条线程要靠这个超时定期回来收 NACK。
        //   3. reversePacketsPerIteration < 0 -> InvalidArg (0 表示不限, 合法)
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

    if (!config_.target.ip.empty()) {
        st = socket_.open();
        if (!st.isOk()) {
            closeResources();
            return st;
        }
        // TODO(M4.1): retransmit.retentionMs > 0 时创建 retransmitCache_,
        //   并 reverseBuf_.resize(MAX_DATA_PACKET_SIZE)。
        //   为 0 时保持 nullptr —— drainReverseChannel 靠它判断"这一级不存在",
        //   同 target.ip 为空时不建 sendQueue_ 的写法。
        //
        //   socket_ 不需要 bind: sendTo 时内核会自动分配一个源端口, 而接收端认的
        //   正是那个端口(它把 recvFrom 的 from 存下来当 peer_)。所以反向通道
        //   **不需要任何额外配置**, 这也是当初选"收发共用一个 socket"的主要收益。
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
    bool sendQueueClosed = false;

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
            if (sendQueue_ &&
                !sendQueue_->push(std::make_unique<EncodedFrame>(std::move(it)))) {
                sendQueueClosed = true;
                break;
            }
        }
        if(!encodeStatus_.isOk() || sendQueueClosed) break;

        out.clear();
    }

    if(encodeStatus_.isOk() && !sendQueueClosed){
        encodeStatus_ = encoder_.flush(out);
        if(encodeStatus_.isOk()) {       
            for(auto& it : out){
                encodeStatus_ = writeEncodedFrame(it);
                if(!encodeStatus_.isOk()) break;
                stats_.encodedFrames++;
                if (sendQueue_ &&
                    !sendQueue_->push(std::make_unique<EncodedFrame>(std::move(it)))) {
                    sendQueueClosed = true;
                    break;
                }
            }
        }
    }

    if(!encodeStatus_.isOk() || sendQueueClosed) abortRequested_.store(true);
    queue_->close();
    if (sendQueue_) {
        sendQueue_->close();
    }
}

void SenderPipeline::sendLoop() {
    sendStatus_ = Status::ok();

    // TODO(M4.1): 这个 while 要改成"同时等两件事"的形状。队列是条件变量、socket 是 fd,
    //   poll 等不了条件变量, 所以走 D16 的老办法: 带超时地等队列, 每轮顺便收一轮 socket。
    //
    //   while (!(sendQueue_->popFor(frame, ms(config_.sendPollMs)))) 这样写不对 ——
    //   popFor 返回 false 时"超时"和"关闭后已取空"是同一个值, 要靠 isClosed() 区分,
    //   同 renderLoop 里那段:
    //
    //     for (;;) {
    //         std::unique_ptr<EncodedFrame> frame;
    //         if (sendQueue_->popFor(frame, milliseconds(config_.sendPollMs))) {
    //             ...原来的打包发送...
    //             // 打完包之后把帧移进重传缓存(**这里是 move, 不是拷贝**):
    //             // retransmitCache_->store(view, std::move(frame->data), baseSeq,
    //             //                         fragCount, steadyNowMs());
    //             // baseSeq 要在 packetize **之前**读 packetizer_.nextSeq() —— 之后读到的
    //             // 已经是下一帧的起点了。这是个很容易写反、而且只在有重传时才暴露的错。
    //         } else if (sendQueue_->isClosed()) {
    //             break;
    //         }
    //         drainReverseChannel();   // 不管有没有帧, 都收一轮
    //     }
    //
    //   收尾还要注意: 队列关闭并取空之后就 break, 不要为了"把剩下的 NACK 处理完"
    //   多等一轮 —— 那是 D21 说的"正常结束"和"立刻停止"混在一起。流都停了,
    //   重传出去的包接收端也早过了 playAt。

    std::unique_ptr<EncodedFrame> frame;
    while (sendQueue_->pop(frame)) {
        EncodedFrameView view;
        view.data = frame->data.data();
        view.len = frame->data.size();
        view.frameId = frame->frameId;
        view.captureMs = frame->captureMs;
        view.isKey = frame->isKey;

        sendStatus_ = packetizer_.packetize(view, packets_);
        if (!sendStatus_.isOk()) {
            break;
        }

        for (const PacketBuffer& packet : packets_) {
            const Status status =
                socket_.sendTo(config_.target, packet.data(), packet.size());
            if (status.isOk()) {
                ++stats_.packetsSent;
            } else {
                ++stats_.sendErrors;
            }
        }
    }

    if (!sendStatus_.isOk()) {
        abortRequested_.store(true);
        sendQueue_->close();
        queue_->close();
    }
}

void SenderPipeline::drainReverseChannel() {
    // TODO(M4.1):
    //   1. retransmitCache_ 为空(retentionMs == 0) -> 直接返回。
    //      关掉重传时连 recvFrom 都不该走 —— 每轮多一次 syscall 去等一个永远不来的包。
    //   2. 循环最多 config_.reversePacketsPerIteration 次(0 表示不限):
    //      a. socket_.recvFrom(reverseBuf_, ..., from, /*timeoutMs=*/0)
    //         Timeout -> 没包了, break(**这是正常情况, 不是错误, 别计 sendErrors**)
    //         其它非 Ok -> ++sendErrors, break
    //      b. decodeNackPacket(reverseBuf_, len, nackedSeqs_)
    //         失败 -> ++reverseMalformed, continue
    //         (端口上收到垃圾是常态; 和 NACK 计数分开, 同 lost / malformed 的理由)
    //      c. ++nacksReceived; stats_.nackedSeqs += nackedSeqs_.size()
    //      d. 对每个 seq: retransmitCache_->find(...)
    //           命中 -> packetizeOneFragment(streamId, seq, view, fragIndex, fragCount,
    //                                        /*isRetransmit=*/true, retransmitBuf_)
    //                   -> socket_.sendTo(**from**, ...) -> ++packetsRetransmitted
    //           未命中 -> ++retransmitMisses
    //
    //   目的地用 from 而不是 config_.target: 跨 NAT 时只有"回到来源地址"的包能通。
    //   M4 全在回环上两者相同, 但写对了就不用等到跨机器再返工。
    //
    //   重传的包**不计入 packetsSent** —— 那个数是"原发了多少", 混进重传之后
    //   "接收端收到的包数 vs 发送端发出的包数"这个对账就再也对不上了。
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
