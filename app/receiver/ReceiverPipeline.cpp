/**
 * @file    ReceiverPipeline.cpp
 * @brief   ReceiverPipeline.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "app/receiver/ReceiverPipeline.h"

#include <utility>

#include "common/Clock.h"
#include "common/Logger.h"
#include "modules/transport/Packet.h"

ReceiverPipeline::ReceiverPipeline(ReceiverPipelineConfig config)
    : config_(std::move(config)), assembler_(config_.maxPendingFrames) {}

Status ReceiverPipeline::open() {
    // TODO(M2): 步骤:
    //  1. validateConfig(), 失败直接返回;
    //  2. socket_.open(), 失败返回;
    //  3. socket_.bind(config_.listen), 失败要 closeResources() 再返回;
    //  4. h264DumpPath 非空时打开文件(binary | trunc), 打不开 → IoError + closeResources();
    //  5. recvBuf_.resize(MAX_DATA_PACKET_SIZE);
    //     **必须按协议上限开**, 不能按"够用就行"开: 缓冲区比数据报小的话,
    //     UdpSocket 会报 NetError, 现象是"每个包都收不进来"却没有任何网络问题;
    //  6. opened_ = true。
    return Status::error(Code::Internal, "ReceiverPipeline::open is not implemented (M2)");
}

Status ReceiverPipeline::run(const std::atomic<bool>& stopRequested) {
    // TODO(M2): 步骤:
    //  1. !opened_ → Closed; hasRun_ → InvalidArg; hasRun_ = true;
    //  2. const uint64_t start = steadyNowMs(); uint64_t lastPacketMs = start;
    //  3. 主循环 while (!stopRequested.load()):
    //     a. size_t n = 0; Endpoint from;
    //        const Status st = socket_.recvFrom(recvBuf_.data(), recvBuf_.size(), n, from,
    //                                           config_.recvTimeoutMs);
    //     b. st.code() == Code::Timeout:
    //          - idleTimeoutMs > 0 且 steadyNowMs() - lastPacketMs >= idleTimeoutMs → break;
    //          - 否则 continue(回到顶部重新检查 stopRequested);
    //        **超时不是错误**, 不要打 ERROR 日志, 每 200ms 一条会把日志淹掉;
    //     c. 其他非 Ok: ++stats_.recvErrors; continue; —— 不 break,
    //        为一个坏包退出整个接收端, 等于把对端的 bug 变成自己的可用性问题;
    //     d. lastPacketMs = steadyNowMs();
    //     e. assembler_.offer(recvBuf_.data(), n) —— 返回值可以忽略,
    //        计数已经在组包器内部记好了(见 FrameAssembler::offer 的注释);
    //     f. AssembledFrame frame; while (assembler_.pop(frame)) {
    //            writeFrame(frame) 失败 → 记 IoError 并整体退出(盘写不进去是真的该停);
    //            maxFrames > 0 且 framesWritten >= maxFrames → 收满退出;
    //        }
    //  4. 退出后: stats_.assembler = assembler_.stats();
    //     stats_.elapsedMs = steadyNowMs() - start; closeResources();
    //
    //  注意 lastPacketMs 只在**收到包**时更新, 不在**组齐帧**时更新:
    //  只收到分片却一直组不齐的时候网络显然还活着, 按帧计时会把"一直丢包"
    //  误报成"对端已经停了", 然后接收端自己退出 —— 排查时看到的是"receiver 莫名其妙退了"。
    (void)stopRequested;
    return Status::error(Code::Internal, "ReceiverPipeline::run is not implemented (M2)");
}

uint16_t ReceiverPipeline::boundPort() const {
    // TODO(M2): return socket_.localEndpoint().port;
    //  bind 传 0 时端口由内核决定, 只能这样回读 —— 同 ISource::actualConfig 的道理。
    return 0;
}

Status ReceiverPipeline::validateConfig() const {
    // TODO(M2): 校验:
    //  - recvTimeoutMs <= 0 → InvalidArg。0 会让循环退化成忙轮询烧满一个核,
    //    负数是无限阻塞, Ctrl-C 之后进程卡在 poll 里出不来;
    //  - maxFrames < 0 → InvalidArg(0 的语义是不限);
    //  - idleTimeoutMs < 0 → InvalidArg(0 的语义是不超时);
    //  - maxPendingFrames == 0 → InvalidArg。
    return Status::error(Code::Internal, "ReceiverPipeline::validateConfig is not implemented (M2)");
}

Status ReceiverPipeline::writeFrame(const AssembledFrame& frame) {
    // TODO(M2): h264File_ 打开时写 frame.data(注意 reinterpret_cast<const char*>),
    //  写失败 → IoError; 然后更新 framesWritten / bytesWritten / keyFrames。
    //  文件没打开时也要更新统计 —— 不写盘不等于没收到, 不然 --dump 一去掉统计就全是 0。
    (void)frame;
    return Status::error(Code::Internal, "ReceiverPipeline::writeFrame is not implemented (M2)");
}

void ReceiverPipeline::closeResources() {
    // TODO(M2): h264File_ 打开则 close(); socket_.close(); opened_ = false;
    //  两个都幂等, 成功路径和失败路径共用这一个函数。
}
