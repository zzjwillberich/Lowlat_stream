/**
 * @file    DelayEstimator.cpp
 * @brief   DelayEstimator.h 的实现
 * @author  zzj
 * @date    2026-09-01
 */
#include "modules/transport/DelayEstimator.h"

#include <algorithm>
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
    // TODO(M4.3) 步骤 1: 算 offset 并更新计数
    //   const int64_t offset = int64(nowMs) - int64(timestampMs);
    //   ++stats_.samplesSeen;
    //   **全程 int64**: 跨机器时两端 steady_clock 起点毫不相干, offset 可能是
    //   很大的负数。这条 M3 的 computePlayAt 已经踩过一次了。

    // TODO(M4.3) 步骤 2: 非自适应模式 —— 逐字保持 M3 的行为后直接返回
    //   floor 取历史最小 offset(只减不增), target 恒为 config_.targetDelayMs。
    //   **不要**顺手"改进"它: 它是 M4.5 报告的对照组, 对照组变了整张表就没意义。
    //   窗口在这个模式下不维护(省掉每帧一次 O(n)), 但 stats_ 里的
    //   currentDelayMs / floorOffsetMs 还是要填, 否则日志里这一档是空的。

    // TODO(M4.3) 步骤 3: 进窗口 + 淘汰
    //   window_.push_back({nowMs, offset});
    //   把 atMs 比 nowMs 老过 windowMs 的从**前面**弹掉, 每弹一个 ++samplesEvicted。
    //   注意 nowMs 是调用方给的, **可能比 window_.back().atMs 还小**(单调时钟
    //   理论上不回退, 但"理论上"在这个项目里已经错过一次了 —— 见 RetransmitCache
    //   的 backward-clock guard)。用无符号减法算年龄会绕成天文数字, 于是
    //   **整个窗口被一次清空**, 水位冷启动。先判 `atMs > nowMs` 再算差。
    //   最后 stats_.windowSize = window_.size();

    // TODO(M4.3) 步骤 4: 算 floor 和 raw
    //   floorOffsetMs_ = percentileOffset(config_.floorPercentile);  hasFloor_ = true;
    //   const int64_t high = percentileOffset(config_.delayPercentile);
    //   raw = int(high - floorOffsetMs_);
    //   **floor 不限速**: 它装的是两端时钟差, 要瞬时跟。限速只作用在 target 上,
    //   理由见头文件 downRateMsPerSec 的 @note。
    //   raw 要夹进 int 范围再转 —— high - floor 的上界是 2^33 量级。

    // TODO(M4.3) 步骤 5: 样本不足就不动
    //   window_.size() < config_.minSamples -> target 保持 config_.targetDelayMs,
    //   跳过步骤 6 直接去步骤 7。冷启动时 p95 = 最大的那个样本, 会被前几帧的
    //   偶然抖动锚死一整个窗口。

    // TODO(M4.3) 步骤 6: 非对称跟随
    //   if (raw > targetDelayMs_) { targetDelayMs_ = raw; ++stats_.raises; }
    //   else {
    //       dt = hasLastObserve_ && nowMs > lastObserveMs_ ? nowMs - lastObserveMs_ : 0;
    //       maxDrop = downRateMsPerSec * dt / 1000;    // 整数运算, 注意先乘后除
    //       floorTarget = targetDelayMs_ - maxDrop;
    //       if (raw < floorTarget) { targetDelayMs_ = floorTarget; ++stats_.downRateLimited; }
    //       else                     targetDelayMs_ = raw;
    //   }
    //   **maxDrop 先乘后除**: 30fps 下 dt=33ms, 先除的话 33/1000 = 0 ——
    //   降速永远是 0, 水位只涨不跌, 而且**测试里用大 dt 跑就看不出来**。
    //   dt 用两次 observe 的间隔而不是固定帧周期: 流卡顿和帧率变化时帧周期不成立。

    // TODO(M4.3) 步骤 7: 夹取 + 记 stats
    //   夹取在限速**之后**: 反过来的话 target 会被夹在界上而 raises/downRateLimited
    //   记的是夹取前的动作, 两组数对不上。
    //   越界时 ++clampedHigh / ++clampedLow。
    //   stats_.currentDelayMs = targetDelayMs_;
    //   stats_.rawDelayMs = raw;               // 夹取和限速**之前**的原始估计
    //   stats_.peakDelayMs = max(peakDelayMs, targetDelayMs_);
    //   lastObserveMs_ = nowMs; hasLastObserve_ = true;

    // TODO(M4.3) 步骤 8: 返回 playAt
    //   playAt = int64(timestampMs) + floorOffsetMs_ + targetDelayMs_;
    //   负数返回 0 —— 同 M3 的 computePlayAt。playAt 是无符号的, 让它绕一圈
    //   等于"这一帧要等 49 天", msUntilNextDue 会把 poll 超时顶到 INT_MAX。
    (void)timestampMs;
    (void)nowMs;
    return 0;
}

int64_t DelayEstimator::percentileOffset(int percentile) const {
    // TODO(M4.3):
    //   1. window_ 为空 -> return 0
    //   2. scratch_ 清空后把 window_ 里的 offsetMs 全拷进去(reserve 一次)
    //   3. const size_t index = size_t(percentile) * (scratch_.size() - 1) / 100;
    //      **先乘后除**, 且用 size_t 算 —— percentile*(n-1) 在 int 里
    //      对 n 大到几万时会溢出。取法定死为就近取下、不插值, 见头文件 @note。
    //   4. std::nth_element(begin, begin + index, end); return scratch_[index];
    //      nth_element 是 O(n) 且只做部分排序, 不要用 std::sort ——
    //      虽然这里 n 只有几百, 差别看不见, 但写 sort 会让读的人以为需要全序。
    (void)percentile;
    return 0;
}

void DelayEstimator::reset() {
    window_.clear();
    scratch_.clear();
    floorOffsetMs_ = 0;
    hasFloor_ = false;
    targetDelayMs_ = config_.targetDelayMs;
    lastObserveMs_ = 0;
    hasLastObserve_ = false;
    stats_ = {};
    stats_.currentDelayMs = targetDelayMs_;
    stats_.rawDelayMs = targetDelayMs_;
    stats_.peakDelayMs = targetDelayMs_;
}
