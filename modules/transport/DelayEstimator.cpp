/**
 * @file    DelayEstimator.cpp
 * @brief   DelayEstimator.h 的实现
 * @author  zzj
 * @date    2026-09-01
 */
#include "modules/transport/DelayEstimator.h"

#include <algorithm>
#include <limits>
#include <utility>

DelayEstimator::DelayEstimator(DelayEstimatorConfig config) : config_(std::move(config)) {
    // 上下界互换的话 clamp 是 UB(标准库要求 lo <= hi), 在构造时归一化一次,
    // 之后的每一帧都不用再判。同 NackTracker 对 min/maxReorderPackets 的处理。
    if (config_.minDelayMs > config_.maxDelayMs) {
        std::swap(config_.minDelayMs, config_.maxDelayMs);
    }
    config_.delayPercentile = std::clamp(config_.delayPercentile, 0, 100);
    config_.floorPercentile = std::clamp(config_.floorPercentile, 0, 100);

    targetDelayMs_ = config_.targetDelayMs;
    stats_.currentDelayMs = targetDelayMs_;
    stats_.rawDelayMs = targetDelayMs_;
    stats_.peakDelayMs = targetDelayMs_;
}

uint64_t DelayEstimator::observe(uint32_t timestampMs, uint64_t nowMs) {
    const int64_t offset =
        static_cast<int64_t>(nowMs) - static_cast<int64_t>(timestampMs);
    ++stats_.samplesSeen;
    updateFrameInterval(timestampMs);

    if (!config_.adaptive) {
        if (!hasFloor_ || offset < floorOffsetMs_) {
            floorOffsetMs_ = offset;
            hasFloor_ = true;
        }

        targetDelayMs_ = config_.targetDelayMs;
        stats_.currentDelayMs = targetDelayMs_;
        stats_.rawDelayMs = targetDelayMs_;
        stats_.floorOffsetMs = floorOffsetMs_;
        stats_.peakDelayMs = std::max(stats_.peakDelayMs, targetDelayMs_);

        const int64_t playAt = static_cast<int64_t>(timestampMs) + floorOffsetMs_ +
                               static_cast<int64_t>(targetDelayMs_);
        return playAt < 0 ? 0 : static_cast<uint64_t>(playAt);
    }

    window_.push_back(Sample{nowMs, offset});
    while (!window_.empty()) {
        const uint64_t atMs = window_.front().atMs;
        if (atMs > nowMs || nowMs - atMs <= config_.windowMs) break;
        window_.pop_front();
        ++stats_.samplesEvicted;
    }
    stats_.windowSize = window_.size();

    floorOffsetMs_ = percentileOffset(config_.floorPercentile);
    hasFloor_ = true;
    const int64_t high = percentileOffset(config_.delayPercentile);
    const int64_t raw64 = high - floorOffsetMs_;
    const int64_t minInt = std::numeric_limits<int>::min();
    const int64_t maxInt = std::numeric_limits<int>::max();
    const int raw = static_cast<int>(std::clamp(raw64, minInt, maxInt));

    if (window_.size() < config_.minSamples) {
        // 样本不足时**保持不动**, 而不是赋回 config_.targetDelayMs。
        // 冷启动时两种写法一样(水位本来就是初值); 差别在**窗口缩回边界以下**的时候 ——
        // 赋回初值等于一次瞬时收窄, 绕过了 downRateMsPerSec, 而那正是限速要挡的事。
        // 触发路径: 停顿超过一个窗口后恢复, 或者帧率低到 windowMs*fps/1000 恰好
        // 卡在 minSamples 上(10s 窗口 @3fps 正好 30 个), 样本数在边界两侧来回穿,
        // 水位就在自适应值和初值之间反复横跳。
    } else if (raw > targetDelayMs_) {
        targetDelayMs_ = raw;
        downRateRemainder_ = 0;
        ++stats_.raises;
    } else if (raw == targetDelayMs_ || config_.downRateMsPerSec <= 0) {
        targetDelayMs_ = raw;
        downRateRemainder_ = 0;
    } else {
        const uint64_t dt =
            hasLastObserve_ && nowMs > lastObserveMs_ ? nowMs - lastObserveMs_ : 0;
        const uint64_t rate = static_cast<uint64_t>(config_.downRateMsPerSec);
        uint64_t scaled =
            dt > std::numeric_limits<uint64_t>::max() / rate
                ? std::numeric_limits<uint64_t>::max()
                : rate * dt;
        if (scaled > std::numeric_limits<uint64_t>::max() - downRateRemainder_) {
            scaled = std::numeric_limits<uint64_t>::max();
        } else {
            scaled += downRateRemainder_;
        }
        const uint64_t maxDrop = scaled / 1000;
        downRateRemainder_ = scaled % 1000;
        const uint64_t maxUsefulDrop = static_cast<uint64_t>(
            static_cast<int64_t>(targetDelayMs_) - minInt);
        const int floorTarget = static_cast<int>(
            static_cast<int64_t>(targetDelayMs_) -
            static_cast<int64_t>(std::min(maxDrop, maxUsefulDrop)));
        if (raw < floorTarget) {
            targetDelayMs_ = floorTarget;
            ++stats_.downRateLimited;
        } else {
            targetDelayMs_ = raw;
        }
    }

    // TODO(M4.6) 下限改成 max(minDelayMs, 帧周期 + RTT):
    //   const int budget = (rttMs_ > 0 && stats_.frameIntervalMs > 0)
    //                          ? stats_.frameIntervalMs + rttMs_ : 0;
    //   const int floor = std::max(config_.minDelayMs, budget);
    //   stats_.effectiveMinDelayMs = floor;
    //   stats_.floorFromBudget = budget > config_.minDelayMs;
    //   然后拿 floor 代替下面的 config_.minDelayMs。
    //
    //   **上限仍然优先**: RTT 测歪了的话 floor 可能大过 maxDelayMs,
    //   而 maxDelayMs 是产品定义的上限(过 500ms README 第一行就不成立),
    //   不能被一个测量值突破。所以顺序是先判上限、再判下限,
    //   且 floor 自己也要先夹进 [minDelayMs, maxDelayMs]。
    if (targetDelayMs_ > config_.maxDelayMs) {
        targetDelayMs_ = config_.maxDelayMs;
        ++stats_.clampedHigh;
    } else if (targetDelayMs_ < config_.minDelayMs) {
        targetDelayMs_ = config_.minDelayMs;
        ++stats_.clampedLow;
    }

    stats_.currentDelayMs = targetDelayMs_;
    stats_.rawDelayMs = raw;
    stats_.floorOffsetMs = floorOffsetMs_;
    stats_.peakDelayMs = std::max(stats_.peakDelayMs, targetDelayMs_);
    lastObserveMs_ = nowMs;
    hasLastObserve_ = true;

    const int64_t playAt = static_cast<int64_t>(timestampMs) + floorOffsetMs_ +
                           static_cast<int64_t>(targetDelayMs_);
    return playAt < 0 ? 0 : static_cast<uint64_t>(playAt);
}

void DelayEstimator::setRttMs(int rttMs) {
    // TODO(M4.6): rttMs > 0 时记进 rttMs_ 并同步 stats_.rttMs; <= 0 忽略(保持旧值)。
    //   **不要**在这里改 targetDelayMs_ —— 预算只抬下限, 下限在 observe 里参与夹取。
    (void)rttMs;
}

void DelayEstimator::updateFrameInterval(uint32_t timestampMs) {
    // TODO(M4.6):
    //   1. 第一帧只记 lastTimestampMs_ / hasLastTimestamp_ 就返回
    //   2. delta = abs(static_cast<int32_t>(timestampMs - lastTimestampMs_))
    //      **必须用 int32_t 做差**: timestampMs 会回绕, 无符号减法在回绕点
    //      给出十亿级的差, 一个样本就能把帧周期顶到天上。
    //   3. delta 压进 frameDeltas_, 超过 kFrameDeltaWindow 就从前面弹掉
    //   4. 取中位数(拷一份用 nth_element, 同 percentileOffset)写进 stats_.frameIntervalMs
    //   **中位数不是均值**: 帧乱序时相邻差值会是 0 或两倍, 均值被拖偏而中位数不会。
    (void)timestampMs;
}

int64_t DelayEstimator::percentileOffset(int percentile) const {
    if (window_.empty()) return 0;

    scratch_.clear();
    scratch_.reserve(window_.size());
    for (const Sample& sample : window_) {
        scratch_.push_back(sample.offsetMs);
    }

    const size_t index =
        static_cast<size_t>(percentile) * (scratch_.size() - 1) / 100;
    std::nth_element(scratch_.begin(), scratch_.begin() + index, scratch_.end());
    return scratch_[index];
}

void DelayEstimator::reset() {
    window_.clear();
    scratch_.clear();
    floorOffsetMs_ = 0;
    hasFloor_ = false;
    targetDelayMs_ = config_.targetDelayMs;
    lastObserveMs_ = 0;
    hasLastObserve_ = false;
    downRateRemainder_ = 0;
    frameDeltas_.clear();
    lastTimestampMs_ = 0;
    hasLastTimestamp_ = false;
    // rttMs_ **不清**: 换对端不改变这条链路的往返时间, 而重新测一次要好几秒,
    // 那几秒里水位没有下限保护 —— 正是最容易掉进坏平衡点的时候。
    stats_ = {};
    stats_.currentDelayMs = targetDelayMs_;
    stats_.rawDelayMs = targetDelayMs_;
    stats_.peakDelayMs = targetDelayMs_;
}
