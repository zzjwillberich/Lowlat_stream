/**
 * @file    ReceiverPipeline.cpp
 * @brief   ReceiverPipeline.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "app/receiver/ReceiverPipeline.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include "common/Clock.h"
#include "common/Logger.h"
#include "modules/transport/Packet.h"

namespace {
LatencySummary summarizeLatency(LatencyRecorder& recorder) {
    LatencySummary summary;
    summary.samples = recorder.count();
    if (summary.samples != 0) {
        summary.p50Ms = recorder.percentile(50);
        summary.p95Ms = recorder.percentile(95);
        summary.p99Ms = recorder.percentile(99);
        summary.maxMs = recorder.max();
    }
    return summary;
}
}  // namespace

ReceiverPipeline::ReceiverPipeline(ReceiverPipelineConfig config)
    : config_(std::move(config)),
      assembler_(config_.maxPendingFrames),
      jitter_(config_.jitter),
      // BoundedQueue 在构造时 assert 容量。将非法的 0 暂钳成 1，让 open() 能返回
      // 可诊断的 InvalidArg，而不是在构造阶段中止进程。
      queueA_(std::max<size_t>(1, config_.decodeQueueCapacity)),
      queueB_(std::max<size_t>(1, config_.renderQueueCapacity)) {}

Status ReceiverPipeline::open() {
    Status status = validateConfig();
    if (!status.isOk()) return status;

    status = socket_.open();
    if (!status.isOk()) return status;
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

    // 空 renderKind 保留 M2 的纯落盘模式；不启动没有消费者的解码/渲染两级。
    if (!config_.renderKind.empty()) {
        Decoder::installFfmpegLogBridge();
        status = decoder_.open(config_.decoder);
        if (!status.isOk()) {
            closeResources();
            return status;
        }
        renderer_ = createRenderer(config_.renderKind);
        if (!renderer_) {
            closeResources();
            return Status::error(Code::InvalidArg,
                                 "ReceiverPipeline: unknown renderer kind '" +
                                     config_.renderKind + "'");
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
    stopping_.store(false);
    recvStatus_ = Status::ok();
    decodeStatus_ = Status::ok();
    renderStatus_ = Status::ok();

    const uint64_t startMs = steadyNowMs();
    std::thread recvThread(&ReceiverPipeline::recvLoop, this, std::cref(stopRequested));
    std::thread decodeThread;
    std::thread renderThread;
    if (renderer_) {
        decodeThread = std::thread(&ReceiverPipeline::decodeLoop, this, std::cref(stopRequested));
        renderThread = std::thread(&ReceiverPipeline::renderLoop, this, std::cref(stopRequested));
    }

    recvThread.join();
    if (decodeThread.joinable()) decodeThread.join();
    if (renderThread.joinable()) renderThread.join();

    publishRecvStats();
    if (renderer_) {
        publishDecodeStats();
        std::lock_guard<std::mutex> lock(statsMu_);
        shared_.renderer = renderer_->stats();
        shared_.latency = summarizeLatency(latencyTotal_);
    }
    stats_ = snapshotStats();
    stats_.elapsedMs = steadyNowMs() - startMs;
    {
        std::lock_guard<std::mutex> lock(statsMu_);
        shared_.elapsedMs = stats_.elapsedMs;
    }

    if (stats_.latency.samples != 0) {
        LOG_INFO("receiver", "latency total: samples=%llu p50=%ums p95=%ums p99=%ums max=%ums",
                 static_cast<unsigned long long>(stats_.latency.samples), stats_.latency.p50Ms,
                 stats_.latency.p95Ms, stats_.latency.p99Ms, stats_.latency.maxMs);
    }
    closeResources();

    if (!recvStatus_.isOk()) return recvStatus_;
    if (!decodeStatus_.isOk()) return decodeStatus_;
    return renderStatus_;
}

uint16_t ReceiverPipeline::boundPort() const {
    return socket_.localEndpoint().port;
}

Status ReceiverPipeline::validateConfig() const {
    if (config_.recvTimeoutMs <= 0) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: receive timeout must be positive");
    }
    if (config_.maxFrames < 0 || config_.idleTimeoutMs < 0 || config_.maxPendingFrames == 0 ||
        config_.decodeQueueCapacity == 0 || config_.renderQueueCapacity == 0 ||
        config_.statsIntervalMs < 0 || config_.jitter.targetDelayMs < 0 ||
        config_.jitter.maxFrames == 0 || config_.decoder.threads < 1) {
        return Status::error(Code::InvalidArg, "ReceiverPipeline: invalid pipeline configuration");
    }
    if (!config_.renderKind.empty() && config_.renderKind != "null" &&
        config_.renderKind != "sdl") {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: render kind must be \"sdl\", \"null\", or empty");
    }
    if (!config_.renderKind.empty() &&
        (config_.renderer.width <= 0 || config_.renderer.height <= 0)) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: renderer dimensions must be positive");
    }
    if (config_.loss.lossPercent < 0 || config_.loss.lossPercent > 100) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: --loss must be in [0, 100]");
    }
    if (config_.loss.lossPercent > 0 && config_.loss.seed == LOSS_SEED_DISABLED) {
        return Status::error(Code::InvalidArg,
                             "ReceiverPipeline: --seed is required when --loss is positive");
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
    if (frame.isKey) ++stats_.keyFrames;
    return Status::ok();
}

void ReceiverPipeline::closeResources() {
    if (h264File_.is_open()) h264File_.close();
    decoder_.close();
    socket_.close();
    opened_ = false;
}

void ReceiverPipeline::recvLoop(const std::atomic<bool>& stopRequested) {
    recvStatus_ = Status::ok();
    uint64_t lastPacketMs = steadyNowMs();
    bool completedNormally = false;

    while (!shouldStop(stopRequested)) {
        int timeoutMs = config_.recvTimeoutMs;
        const uint64_t nowBeforeReceive = steadyNowMs();
        if (renderer_) {
            const int untilDue = jitter_.msUntilNextDue(nowBeforeReceive);
            if (untilDue >= 0) timeoutMs = std::min(timeoutMs, untilDue);
        }

        size_t receivedBytes = 0;
        Endpoint from;
        const Status receiveStatus = socket_.recvFrom(recvBuf_.data(), recvBuf_.size(),
                                                      receivedBytes, from, timeoutMs);
        const uint64_t now = steadyNowMs();
        if (receiveStatus.code() == Code::Timeout) {
            if (config_.idleTimeoutMs > 0 &&
                now - lastPacketMs >= static_cast<uint64_t>(config_.idleTimeoutMs)) {
                completedNormally = true;
                break;
            }
        } else if (!receiveStatus.isOk()) {
            ++stats_.recvErrors;  // 单个 UDP 收包失败不终止管线
        } else {
            // 包真的到了。lastPacketMs **无论注入器丢不丢它都要更新** —— 网络显然
            // 还活着; 不更新的话高丢包率会被 idleTimeout 误判成对端已停止。
            lastPacketMs = now;
            if (!shouldInjectDrop(recvBuf_.data(), receivedBytes)) {
                // trackPeer 放在注入器**之后**: 注入器的职责是让管线表现得像这个包
                // 从来没到过, 那就不该从它身上学到"对端是谁"。这样"极高丢包率下
                // 反向通道还建不建得起来"才是个能被测出来的问题, 而不是被测试工具
                // 偷偷绕过去的问题。lastPacketMs 是唯一的例外, 理由见上。
                trackPeer(recvBuf_.data(), receivedBytes, from);
                (void)assembler_.offer(recvBuf_.data(), receivedBytes);
            }
        }

        AssembledFrame frame;
        while (assembler_.pop(frame)) {
            const Status writeStatus = writeFrame(frame);
            if (!writeStatus.isOk()) {
                recvStatus_ = writeStatus;
                requestStop();
                break;
            }

            if (renderer_) {
                jitter_.push(std::move(frame), now);
            }

            if (config_.maxFrames > 0 &&
                stats_.framesWritten >= static_cast<uint64_t>(config_.maxFrames)) {
                completedNormally = true;
                break;
            }
        }

        // 即使这次 recvFrom 超时、没有新包可组，也必须检查到期帧。否则流停止后的
        // 最后几帧永远不会离开 jitter buffer，targetDelayMs 反而变成吞帧开关。
        if (renderer_ && !shouldStop(stopRequested)) {
            AssembledFrame due;
            while (jitter_.pop(due, now)) {
                if (queueA_.tryPush(std::make_unique<AssembledFrame>(std::move(due)))) {
                    continue;
                }
                if (shouldStop(stopRequested)) break;

                // 编码帧有依赖关系：队列 A 满时不能只舍弃一帧。清掉整段并要求
                // 下一次交付从 IDR 开始，避免把不可恢复的引用链继续送入解码器。
                ++decodeQueueRejected_;
                ++decodeResyncs_;
                queueA_.clear();
                jitter_.dropUntilKeyFrame();
                break;
            }
        }
        publishRecvStats();
        if (completedNormally) break;
    }

    if (completedNormally && renderer_) {
        // --frames 也是正常结束：把刚刚 push 进 jitter、但还没到 playAt 的尾帧
        // 排空后才关闭 A。否则 --frames=N 的实际渲染数会少 targetDelayMs 内的几帧。
        while (jitter_.size() != 0 && !shouldStop(stopRequested)) {
            const uint64_t now = steadyNowMs();
            AssembledFrame due;
            if (!jitter_.pop(due, now)) {
                const int untilDue = jitter_.msUntilNextDue(now);
                const int sleepMs = untilDue < 0 ? 1 : std::max(1, std::min(untilDue, 5));
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
                continue;
            }
            if (queueA_.tryPush(std::make_unique<AssembledFrame>(std::move(due)))) {
                continue;
            }
            if (shouldStop(stopRequested)) break;
            ++decodeQueueRejected_;
            ++decodeResyncs_;
            queueA_.clear();
            jitter_.dropUntilKeyFrame();
        }
        // 正常 EOF 要把已入队数据和 Decoder 的内部缓存顺序排空；异常/用户停止仍走
        // requestStop()，立即关闭两条队列以唤醒所有阻塞点。
        queueA_.close();
    } else if (!completedNormally) {
        requestStop();
    }
}

void ReceiverPipeline::decodeLoop(const std::atomic<bool>& stopRequested) {
    decodeStatus_ = Status::ok();
    std::vector<RawFrame> decoded;
    bool inputClosedNormally = false;
    while (!shouldStop(stopRequested)) {
        std::unique_ptr<AssembledFrame> frame;
        if (!queueA_.pop(frame)) {
            inputClosedNormally = !shouldStop(stopRequested);
            break;
        }
        if (shouldStop(stopRequested)) break;

        CodedFrameView input;
        input.data = frame->data.data();
        input.len = frame->data.size();
        input.captureMs = frame->timestampMs;
        input.frameId = frame->frameId;

        decoded.clear();
        const Status status = decoder_.decode(input, decoded);
        if (!status.isOk()) {
            // 网络来的坏码流是预期输入；Decoder 自己已把它计进 decodeErrors。
            if (status.code() != Code::NetError) {
                decodeStatus_ = status;
                requestStop();
                break;
            }
            publishDecodeStats();
            continue;
        }

        bool queueClosed = false;
        for (RawFrame& raw : decoded) {
            if (!queueB_.forcePush(std::make_unique<RawFrame>(std::move(raw)))) {
                queueClosed = true;
                break;
            }
        }
        publishDecodeStats();
        if (queueClosed) break;  // close 导致的 false 是正常收尾，不覆盖真正错误
    }

    if (inputClosedNormally && decodeStatus_.isOk()) {
        decoded.clear();
        const Status flushStatus = decoder_.flush(decoded);
        if (!flushStatus.isOk()) {
            decodeStatus_ = flushStatus;
            requestStop();
        } else {
            for (RawFrame& raw : decoded) {
                if (!queueB_.forcePush(std::make_unique<RawFrame>(std::move(raw)))) {
                    break;  // 关闭只可能来自同时发生的外部停止
                }
            }
            // 解码器是队列 B 的唯一生产者；关闭后渲染线程仍会取完残留帧。
            queueB_.close();
        }
    }
    publishDecodeStats();
    if (!inputClosedNormally) requestStop();
}

void ReceiverPipeline::renderLoop(const std::atomic<bool>& stopRequested) {
    renderStatus_ = renderer_->open(config_.renderer);
    if (!renderStatus_.isOk()) {
        renderer_->close();
        requestStop();
        return;
    }

    uint64_t lastStatsMs = steadyNowMs();
    uint64_t lastRendered = 0;
    while (!shouldStop(stopRequested)) {
        if (renderer_->pumpEvents()) {
            requestStop();
            break;
        }

        std::unique_ptr<RawFrame> frame;
        if (queueB_.popFor(frame, std::chrono::milliseconds(5))) {
            const Status status = renderer_->renderFrame(*frame);
            if (status.isOk()) {
                if (frame->captureMs != 0) {
                    const uint32_t now32 = static_cast<uint32_t>(steadyNowMs());
                    const uint32_t latencyMs = now32 - static_cast<uint32_t>(frame->captureMs);
                    latencyTotal_.add(latencyMs);
                    latencyWindow_.add(latencyMs);
                }
            } else if (status.code() != Code::InvalidArg) {
                renderStatus_ = status;
                requestStop();
                break;
            }
        } else if (queueB_.isClosed()) {
            break;
        }

        const uint64_t now = steadyNowMs();
        if (config_.statsIntervalMs > 0 &&
            now - lastStatsMs >= static_cast<uint64_t>(config_.statsIntervalMs)) {
            const uint64_t elapsedMs = now - lastStatsMs;
            const uint64_t renderedNow = renderer_->stats().framesRendered;
            const uint64_t fps = elapsedMs == 0 ? 0 :
                (renderedNow - lastRendered) * 1000 / elapsedMs;
            {
                std::lock_guard<std::mutex> lock(statsMu_);
                shared_.renderer = renderer_->stats();
                shared_.decodeQueueDropped = queueA_.dropped() + decodeQueueRejected_.load();
                shared_.renderQueueDropped = queueB_.dropped();
                shared_.decodeResyncs = decodeResyncs_.load();
            }
            const ReceiverPipelineStats snapshot = snapshotStats();
            const LatencySummary window = summarizeLatency(latencyWindow_);
            LOG_INFO("receiver",
                     "fps=%llu latency samples=%llu p50=%ums p95=%ums | queueA=%zu queueB=%zu "
                     "dropped=%llu resyncs=%llu assembler_lost=%llu",
                     static_cast<unsigned long long>(fps),
                     static_cast<unsigned long long>(window.samples), window.p50Ms, window.p95Ms,
                     queueA_.size(), queueB_.size(),
                     static_cast<unsigned long long>(snapshot.jitter.framesTooLate +
                                                     snapshot.jitter.framesDropped +
                                                     snapshot.jitter.framesDroppedForResync +
                                                     snapshot.decodeQueueDropped +
                                                     snapshot.renderQueueDropped),
                     static_cast<unsigned long long>(snapshot.decodeResyncs),
                     static_cast<unsigned long long>(snapshot.assembler.packetsLost()));
            latencyWindow_.reset();
            lastStatsMs = now;
            lastRendered = renderedNow;
        }
    }

    {
        std::lock_guard<std::mutex> lock(statsMu_);
        shared_.renderer = renderer_->stats();
        shared_.latency = summarizeLatency(latencyTotal_);
        shared_.decodeQueuePeak = queueA_.peak();
        shared_.renderQueuePeak = queueB_.peak();
        shared_.decodeQueueDropped = queueA_.dropped() + decodeQueueRejected_.load();
        shared_.renderQueueDropped = queueB_.dropped();
        shared_.decodeResyncs = decodeResyncs_.load();
    }
    renderer_->close();
    if (shouldStop(stopRequested)) requestStop();
}

void ReceiverPipeline::requestStop() {
    stopping_.store(true);
    queueA_.close();
    queueB_.close();
}

bool ReceiverPipeline::shouldStop(const std::atomic<bool>& stopRequested) const {
    return stopRequested.load() || stopping_.load();
}

void ReceiverPipeline::publishRecvStats() {
    std::lock_guard<std::mutex> lock(statsMu_);
    shared_.framesWritten = stats_.framesWritten;
    shared_.bytesWritten = stats_.bytesWritten;
    shared_.keyFrames = stats_.keyFrames;
    shared_.recvErrors = stats_.recvErrors;
    shared_.assembler = assembler_.stats();
    shared_.jitter = jitter_.stats();
    shared_.decodeQueuePeak = queueA_.peak();
    shared_.renderQueuePeak = queueB_.peak();
    shared_.decodeQueueDropped = queueA_.dropped() + decodeQueueRejected_.load();
    shared_.renderQueueDropped = queueB_.dropped();
    shared_.decodeResyncs = decodeResyncs_.load();
    shared_.injectedDrops = injectedDrops_.load();
}

void ReceiverPipeline::trackPeer(const uint8_t* packet, size_t len, const Endpoint& from) {
    // TODO(M4.1): 闸门定在"合法 DATA 包", 三步:
    //   1. decodePacketHeader 失败 -> 直接返回(野包/旧版本对端, 不认它当对端)
    //   2. header.type != PacketType::Data -> 返回
    //      "对端"的定义是"给我发媒体数据的那个人"; 将来的 FEC/NACK 包不参与认定
    //   3. decodeDataHeader 失败 -> 返回; 成功 -> peer_ = from
    //
    // 就是无条件覆盖, 不要加"只在第一次设置"或者"变了才更新"之类的条件 ——
    // 前者会在 sender 重启后永远够不着(见头文件的 @note), 后者是同一件事写复杂了。
    (void)packet;
    (void)len;
    (void)from;
}

bool ReceiverPipeline::shouldInjectDrop(const uint8_t* packet, size_t len) {
    // 丢包注入必须保持以下顺序:
    //   1. !lossInjectionEnabled(config_.loss) -> return false  (关掉时零开销, 放第一行)
    //   2. decodePacketHeader 失败 -> return false
    //      **不是** return true —— 畸形包要交给 offer() 去计 packetsMalformed,
    //      在这里吞掉的话测试工具会把畸形包统计吃走(见头文件的 @note)
    //   3. 取重传位: 只有 DATA 包才有 DataHeader。
    //      - type == Data 且 decodeDataHeader 成功 -> isRetransmit = flags & FLAG_RETRANSMIT
    //      - 其它情况(FEC 等) -> isRetransmit = false, 但**照样按 seq 判丢**
    //      - DATA 包的 DataHeader 解不出来 -> 同第 2 条, return false
    //   4. shouldDropPacket(config_.loss, header.seq, isRetransmit);
    //      为 true 时 ++injectedDrops_ 再 return true
    if (!lossInjectionEnabled(config_.loss)) return false;

    PacketHeader header;
    if (!decodePacketHeader(packet, len, header).isOk()) return false;

    bool isRetransmit = false;
    if (header.type == PacketType::Data) {
        DataHeader dataHeader;
        if (!decodeDataHeader(packet + PACKET_HEADER_SIZE, len - PACKET_HEADER_SIZE,
                              dataHeader)
                 .isOk()) {
            return false;
        }
        isRetransmit = (dataHeader.flags & DataHeader::FLAG_RETRANSMIT) != 0;
    }

    if (!shouldDropPacket(config_.loss, header.seq, isRetransmit)) return false;
    ++injectedDrops_;
    return true;
}

void ReceiverPipeline::publishDecodeStats() {
    std::lock_guard<std::mutex> lock(statsMu_);
    shared_.decoder = decoder_.stats();
    shared_.renderQueuePeak = queueB_.peak();
    shared_.renderQueueDropped = queueB_.dropped();
}

ReceiverPipelineStats ReceiverPipeline::snapshotStats() const {
    std::lock_guard<std::mutex> lock(statsMu_);
    return shared_;
}
