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
    if (config_.hardLimitFrames == 0) {
        const size_t minLimit = 1;
        if (config_.maxFrames > std::numeric_limits<size_t>::max() / 2) {
            config_.hardLimitFrames = std::numeric_limits<size_t>::max();
        } else {
            config_.hardLimitFrames = std::max(minLimit, config_.maxFrames * 2);
        }
    }

    started_ = !config_.startOnKeyFrame;
}

void JitterBuffer::push(AssembledFrame frame, uint64_t nowMs) {
    ++stats_.framesIn;

    // 扩展锚点要跟上每一帧, 哪怕这帧接下来就被丢掉 ——
    // 只给收下的帧扩展, 连丢一串之后锚点就跟丢了
    const uint64_t extended = extendFrameId(frame.frameId);

    if (!started_) {
        if (!frame.isKey) {
            ++stats_.framesBeforeKey;
            return;
        }
        started_ = true;
    }

    // <= 而不是 <: 相等就是已经放出去过的那一帧本身, 整帧重传会造出这种输入
    if (hasOutput_ && extended <= lastOutExtended_) {
        ++stats_.framesTooLate;
        return;
    }

    if (pending_.find(extended) != pending_.end()) {
        ++stats_.framesDuplicate;
        return;
    }

    Entry entry;
    entry.frame = std::move(frame);
    entry.playAtMs = computePlayAt(entry.frame.timestampMs, nowMs);
    pending_.emplace(extended, std::move(entry));
    enforceCapacity();
}

bool JitterBuffer::pop(AssembledFrame& out, uint64_t nowMs) {
    if (pending_.empty()) return false;

    auto oldest = pending_.begin();
    if (oldest->second.playAtMs > nowMs) return false;

    out = std::move(oldest->second.frame);
    lastOutExtended_ = oldest->first;
    hasOutput_ = true;
    pending_.erase(oldest);
    ++stats_.framesOut;
    return true;
}

int JitterBuffer::msUntilNextDue(uint64_t nowMs) const {
    if (pending_.empty()) return -1;

    const uint64_t due = pending_.begin()->second.playAtMs;
    if (due <= nowMs) return 0;

    // 夹到 int 范围: 时钟映射一旦出岔子 due 可能是个天文数字, 强转成负数
    // 再传给 poll 就是永久阻塞 —— 一个算错的时间戳能把整个接收端挂死
    const uint64_t remaining = due - nowMs;
    const uint64_t maxInt = static_cast<uint64_t>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(remaining, maxInt));
}

void JitterBuffer::reset() {
    pending_.clear();

    refFrameId_ = 0;
    refExtended_ = 0;
    hasRef_ = false;

    minOffsetMs_ = 0;
    hasOffset_ = false;

    lastOutExtended_ = 0;
    hasOutput_ = false;

    started_ = !config_.startOnKeyFrame;
    stats_ = {};
}

uint64_t JitterBuffer::extendFrameId(uint32_t frameId) {
    if (!hasRef_) {
        refFrameId_ = frameId;
        // 初值取 frameId 而不是 0: 不回绕的常见情况下扩展号与帧号一致,
        // 调试打印时不用在脑子里做换算
        refExtended_ = frameId;
        hasRef_ = true;
        return refExtended_;
    }

    // 锚点每帧都跟着走, 两帧之间的距离才永远远小于 2^31, 有符号差值才靠得住
    const int32_t delta = static_cast<int32_t>(frameId - refFrameId_);
    refExtended_ = static_cast<uint64_t>(static_cast<int64_t>(refExtended_) + delta);
    refFrameId_ = frameId;
    return refExtended_;
}

uint64_t JitterBuffer::computePlayAt(uint32_t timestampMs, uint64_t nowMs) {
    // 全程 int64: 跨机器时两台机器的 steady_clock 起点毫不相干, offset 可能是
    // 很大的负数, 用无符号算会直接绕成天文数字
    const int64_t offset = static_cast<int64_t>(nowMs) - static_cast<int64_t>(timestampMs);
    if (!hasOffset_ || offset < minOffsetMs_) {
        minOffsetMs_ = offset;
        hasOffset_ = true;
    }

    const int64_t playAt = static_cast<int64_t>(timestampMs) + minOffsetMs_ +
                           static_cast<int64_t>(config_.targetDelayMs);
    return playAt < 0 ? 0 : static_cast<uint64_t>(playAt);
}

void JitterBuffer::enforceCapacity() {
    // 硬上限: 真丢最老的。它们早过了该显示的时刻, 留着只是给延迟做加法。
    // 丢掉的帧**不推 lastOutExtended_** —— 那个水位的含义是"已经交付到哪儿"
    while (pending_.size() > config_.hardLimitFrames) {
        pending_.erase(pending_.begin());
        ++stats_.framesDropped;
    }

    // 软上限: 只改"什么时候该放", 不减少条目数。最老的几条立刻到期,
    // 播早了而不是丢掉 —— 它们已经在手里了, 丢必然花屏
    const size_t forcedCount = pending_.size() > config_.maxFrames
                                   ? pending_.size() - config_.maxFrames
                                   : 0;
    auto entry = pending_.begin();
    for (size_t i = 0; i < forcedCount; ++i, ++entry) {
        if (entry->second.playAtMs != 0) {
            entry->second.playAtMs = 0;
            ++stats_.framesForcedEarly;
        }
    }
}
