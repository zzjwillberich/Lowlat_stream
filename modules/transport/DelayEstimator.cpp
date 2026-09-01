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
        targetDelayMs_ = config_.targetDelayMs;
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
    stats_ = {};
    stats_.currentDelayMs = targetDelayMs_;
    stats_.rawDelayMs = targetDelayMs_;
    stats_.peakDelayMs = targetDelayMs_;
}
