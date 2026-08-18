/**
 * @file    JitterBuffer.cpp
 * @brief   JitterBuffer.h 的实现
 * @author  zzj
 * @date    2026-08-18
 */

#include "modules/transport/JitterBuffer.h"

#include <algorithm>
#include <limits>
#include <utility>

JitterBuffer::JitterBuffer(JitterBufferConfig config) : config_(std::move(config)) {
    // TODO(M3.1): hardLimitFrames 为 0 时取 maxFrames * 2; maxFrames 为 0 时至少给 1,
    //             否则每一帧刚进来就被判超限, 表现为"一帧都放不出去"。
    // TODO(M3.1): started_ 的初值是 !config_.startOnKeyFrame ——
    //             不要求从关键帧起播时, 一开始就算已起播。
    (void)config_;
}

void JitterBuffer::push(AssembledFrame frame, uint64_t nowMs) {
    // TODO(M3.1): 按下面的顺序做, 顺序换了会算错计数:
    //   1. ++stats_.framesIn
    //   2. 先扩展帧号(extendFrameId), 再做各种判断 —— 扩展的锚点要跟上每一帧,
    //      哪怕这帧最后被丢弃; 只对收下的帧扩展, 连丢一串之后锚点就跟丢了。
    //   3. 起播门: 还没 started_ 且这帧不是关键帧 -> ++framesBeforeKey, 直接返回;
    //      是关键帧则 started_ = true, 继续往下。
    //   4. 太晚: hasOutput_ 且 extended <= lastOutExtended_ -> ++framesTooLate, 返回。
    //      注意是 <= 不是 <: 等于就是已经放出去过的那一帧本身(整帧重传会这样)。
    //   5. 重复: pending_ 里已有这个 extended -> ++framesDuplicate, 返回。
    //   6. 算应播时刻(computePlayAt), 插入 pending_。
    //   7. enforceCapacity()。
    (void)frame;
    (void)nowMs;
}

bool JitterBuffer::pop(AssembledFrame& out, uint64_t nowMs) {
    // TODO(M3.1): pending_ 为空返回 false;
    //             否则看**最小**的那一条(begin()): playAtMs > nowMs 就返回 false,
    //             到点了就 move 给 out、更新 lastOutExtended_/hasOutput_、erase、
    //             ++framesOut、返回 true。
    // TODO(M3.1): 一次只交付一帧。调用方用 while 循环取干净 —— 让"取几帧"由调用方
    //             的循环决定, 而不是藏在这里。
    (void)out;
    (void)nowMs;
    return false;
}

int JitterBuffer::msUntilNextDue(uint64_t nowMs) const {
    // TODO(M3.1): 空返回 -1; 已到期返回 0; 否则返回 due - nowMs。
    // TODO(M3.1): 返回前**夹到 int 范围**。时钟映射一旦出岔子, due 可能是个天文数字,
    //             直接强转成 int 会变成负数, 再传给 poll 就是"永久阻塞" ——
    //             一个算错的时间戳把整个接收端挂死, 而且看不出是谁干的。
    (void)nowMs;
    return -1;
}

void JitterBuffer::reset() {
    // TODO(M3.1): 清空 pending_ 和全部状态位, 计数器也归零(语义同 FrameAssembler::reset)。
}

uint64_t JitterBuffer::extendFrameId(uint32_t frameId) {
    // TODO(M3.1): 第一次调用时锚定 refFrameId_ = frameId、refExtended_ = frameId,
    //             hasRef_ = true, 直接返回。
    // TODO(M3.1): 之后:
    //               const int32_t delta = static_cast<int32_t>(frameId - refFrameId_);
    //               refExtended_ = static_cast<uint64_t>(
    //                   static_cast<int64_t>(refExtended_) + delta);
    //               refFrameId_ = frameId;
    //             锚点**每帧都跟着走**, 这样两帧之间的距离永远远小于 2^31,
    //             有符号差值才靠得住。
    // TODO(M3.1): refExtended_ 初值给 frameId 而不是 0, 是为了让扩展号在不回绕的
    //             常见情况下与帧号一致 —— 调试打印时不用在脑子里做换算。
    (void)frameId;
    return 0;
}

uint64_t JitterBuffer::computePlayAt(uint32_t timestampMs, uint64_t nowMs) {
    // TODO(M3.1):
    //   const int64_t offset = static_cast<int64_t>(nowMs) - static_cast<int64_t>(timestampMs);
    //   if (!hasOffset_ || offset < minOffsetMs_) { minOffsetMs_ = offset; hasOffset_ = true; }
    //   const int64_t playAt = static_cast<int64_t>(timestampMs) + minOffsetMs_
    //                          + config_.targetDelayMs;
    //   return playAt < 0 ? 0 : static_cast<uint64_t>(playAt);
    // TODO(M3.1): 全程走 int64_t。offset 跨机器时可能是很大的负数(两台机器的
    //             steady_clock 起点毫不相干), 用无符号算会直接绕成天文数字。
    (void)timestampMs;
    (void)nowMs;
    return 0;
}

void JitterBuffer::enforceCapacity() {
    // TODO(M3.1): 两级处置, 顺序是先硬后软:
    //   1. while (pending_.size() > hardLimit) { erase(begin()); ++framesDropped; }
    //      —— 真丢, 且丢最老的。它们早过了该显示的时刻, 留着只是给延迟做加法。
    //      注意被丢的帧**不更新 lastOutExtended_**: 那个水位的含义是"已经交付到哪儿",
    //      丢掉的帧从来没交付过。
    //   2. if (pending_.size() > maxFrames) 把最老的一条 playAtMs 改成 0,
    //      ++framesForcedEarly —— 提前放行而不是丢弃。
    //      已经是 0 的不要重复计数, 否则每收一帧就多记一笔。
}
