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

JitterBuffer::JitterBuffer(JitterBufferConfig config)
    : config_(std::move(config)), estimator_(config_.delay) {
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

    // 查重必须在采样之前: 整帧重传会让同一帧到达两次, 而第二次的 offset
    // 不是"一帧走了多久", 是重传的产物。一帧只该贡献一个样本。
    if (pending_.find(extended) != pending_.end()) {
        ++stats_.framesDuplicate;
        return;
    }

    // TODO(M4.6) 采样点前移 —— **这一句必须在下面两个 return 之前**。
    //
    //   const uint64_t playAtMs = computePlayAt(frame.timestampMs, nowMs);
    //
    //   原来它在函数末尾, 于是"太晚"和"起播前"的帧在走到它之前就 return 了,
    //   **永远不会成为水位估计器的样本**。后果实测(M4.5 第四轮 g4/rtt50):
    //   估计器只看得到准时到达的帧 —— 那些按定义就是快的 —— p95 偏小,
    //   水位收窄, 于是更多帧迟到, 采样更加只剩快的。正反馈。
    //   三个种子里两个掉进坏平衡点: 水位从初值 50 一路衰减到下限 10 从未回升,
    //   同期 150 帧因太晚被丢, 只渲染出 632/1000; 第三个种子锁在 87ms, 渲染 980。
    //   同样的配置同样的链路, 结果差 1.6 倍。
    //
    //   **幸存者偏差长在控制器自己的输入上**: 估计器看不见证明它错了的那些帧。
    //
    //   采样之后照常判"起播前"和"太晚"并 return —— 这一帧确实不播,
    //   但它到得多晚这件事必须被记下来。计到 framesSampledButDropped。
    (void)nowMs;

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

    estimator_.reset();

    lastOutExtended_ = 0;
    hasOutput_ = false;

    started_ = !config_.startOnKeyFrame;
    stats_ = {};
}

void JitterBuffer::setRttMs(int rttMs) { estimator_.setRttMs(rttMs); }

void JitterBuffer::dropUntilKeyFrame() {
    // 这是下游跟不上时的局部重启，不是重连：帧号扩展锚、时钟映射和已交付水位都
    // 必须保留。清掉它们会让回绕后的新帧永久被判作过期，也会把抖动水位重新锚在
    // 拥塞期间的慢样本上。**estimator_ 同理，这里不碰它**——流没变，
    // 而这一刻恰好是最需要准确水位的时候。
    stats_.framesDroppedForResync += pending_.size();
    pending_.clear();
    started_ = !config_.startOnKeyFrame;
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
    return estimator_.observe(timestampMs, nowMs);
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
