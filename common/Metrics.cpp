/**
 * @file    Metrics.cpp
 * @brief   Metrics.h 的实现
 * @author  zzj
 * @date    2026-08-24
 */

#include "common/Metrics.h"

#include <algorithm>

void LatencyRecorder::add(uint32_t sampleMs) {
    samples_.push_back(sampleMs);
    if (sampleMs > max_) max_ = sampleMs;
}

size_t LatencyRecorder::count() const {
    return samples_.size();
}

uint32_t LatencyRecorder::percentile(int p) {
    if (samples_.empty()) return 0;

    p = std::clamp(p, 0, 100);
    std::sort(samples_.begin(), samples_.end());

    const size_t count = samples_.size();
    const size_t percentile = static_cast<size_t>(p);

    // ceil(percentile * count / 100), 但拆开算以避免 percentile * count 溢出。
    const size_t rank = (count / 100) * percentile +
                        ((count % 100) * percentile + 99) / 100;
    const size_t index = rank == 0 ? 0 : rank - 1;
    return samples_[index];
}

uint32_t LatencyRecorder::max() const {
    return max_;
}

void LatencyRecorder::reset() {
    samples_.clear();
    max_ = 0;
}
