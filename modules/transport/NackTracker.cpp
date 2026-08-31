/**
 * @file    NackTracker.cpp
 * @brief   NackTracker.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/NackTracker.h"

#include <algorithm>
#include <utility>

#include "modules/transport/Packet.h"

NackTracker::NackTracker(NackTrackerConfig config) : config_(std::move(config)) {}

void NackTracker::onPacket(uint32_t seq, uint16_t fragCount, bool isKey) {
    // TODO(M4.4): 登记缺口时把 isKey 记进 GapState::likelyKeyFrame ——
    //   "揭示这个缺口的那个包"就是当前这个包(它的 seq 比缺口新)。
    //   放弃缺口时(givenUp 那两处: 滑出窗口 和 请求次数用完),
    //   likelyKeyFrame 为真就 ++stats_.keyFramesGivenUp。
    //   两处都要加, 漏一处的现象是"丢包一多 PLI 就不发了"——
    //   因为丢得越多越容易走"滑出窗口"那条路。
    (void)isKey;
    // 按以下顺序更新:
    //   1. ++stats_.packetsSeen
    //   2. 更新乱序容忍 = clamp(fragCount, minReorderPackets, maxReorderPackets)
    //      (可以只记最近一次, 也可以做滑动平均 —— IDR 的 fragCount 会比 P 帧大几倍,
    //       先用最近值跑通, 等完整版注入器能测乱序了再说)
    //   3. 第一个包: 只把 highestSeq 设成它就返回。**不要**把它之前的 seq 记成缺口
    //   4. seqNewerThan(seq, highestSeq_):
    //      - 是: 中间跳过的 seq 全部登记为缺口, highestSeq_ = seq
    //      - 否: 这是个迟到/重传/重复包 —— 如果它销掉了一个已请求的缺口, ++recovered;
    //            销掉一个未请求的缺口就只是普通乱序, 不计 recovered;
    //            两个都不是就是重复包, 什么都不做
    //   5. 滑出 windowPackets 的缺口: ++givenUp, ++lostForReal, 从表里删掉
    ++stats_.packetsSeen;

    const uint32_t reorderMin =
        std::min(config_.minReorderPackets, config_.maxReorderPackets);
    const uint32_t reorderMax =
        std::max(config_.minReorderPackets, config_.maxReorderPackets);
    reorderPackets_ = std::clamp(static_cast<uint32_t>(fragCount), reorderMin, reorderMax);

    if (!hasBaseline_) {
        highestSeq_ = seq;
        hasBaseline_ = true;
        return;
    }

    if (!seqNewerThan(seq, highestSeq_)) {
        const auto gap = gaps_.find(seq);
        if (gap != gaps_.end()) {
            if (gap->second.requestCount > 0) ++stats_.recovered;
            gaps_.erase(gap);
            stats_.pending = gaps_.size();
        }
        return;
    }

    const uint32_t forwardDistance = seq - highestSeq_;

    // 跳号超过一整个窗口 -> 按"换了一条流"处理:
    //   gaps_.clear(); highestSeq_ = seq; ++stats_.discontinuities; stats_.pending = 0;
    //   然后直接 return —— 不登记任何缺口, 也不计 givenUp / lostForReal。
    //   理由见头文件 onPacket 的 @note: seq 是包头里唯一没有校验的字段,
    //   forwardDistance 的上界是 2^31, 一个坏包就能把 lostForReal 加十亿并制造
    //   一整窗口的幻影缺口。
    if (static_cast<uint64_t>(forwardDistance) >= config_.windowPackets) {
        gaps_.clear();
        highestSeq_ = seq;
        ++stats_.discontinuities;
        stats_.pending = 0;
        return;
    }

    highestSeq_ = seq;

    for (auto gap = gaps_.begin(); gap != gaps_.end();) {
        const uint32_t age = highestSeq_ - gap->first;
        if (config_.windowPackets == 0 ||
            static_cast<uint64_t>(age) >= config_.windowPackets) {
            ++stats_.givenUp;
            ++stats_.lostForReal;
            gap = gaps_.erase(gap);
        } else {
            ++gap;
        }
    }

    const uint64_t missingCount = static_cast<uint64_t>(forwardDistance) - 1;
    const uint64_t maxTrackedAge =
        config_.windowPackets == 0 ? 0 : static_cast<uint64_t>(config_.windowPackets - 1);
    const uint64_t trackedCount = std::min(missingCount, maxTrackedAge);
    const uint64_t immediatelyGivenUp = missingCount - trackedCount;
    stats_.givenUp += immediatelyGivenUp;
    stats_.lostForReal += immediatelyGivenUp;

    for (uint64_t age = trackedCount; age != 0; --age) {
        gaps_.emplace(highestSeq_ - static_cast<uint32_t>(age), GapState{});
    }
    stats_.pending = gaps_.size();
}

void NackTracker::collectNackTargets(std::vector<uint32_t>& out) {
    // 请求判定和重发间隔都只由包序号推进:
    //   out.clear() 先做 —— 调用方复用这个 vector, 残留会变成重复请求
    //
    //   遍历缺口表, 从老到新, 挑出同时满足两条的:
    //     a. highestSeq_ - seq >= 当前乱序容忍   (等够了一帧的包数, 不是乱序)
    //     b. requestCount < maxRequestsPerSeq
    //     c. 且距上次请求又过了一帧的包数(第一次请求时这条自动满足)
    //   挑出来的: ++requestCount, 记下 lastRequestAtHighestSeq, ++stats_.nacksRequested
    //
    //   requestCount 到达上限的: ++givenUp, ++lostForReal, 删掉 —— 别留在表里空占位置
    out.clear();
    if (!hasBaseline_ || gaps_.empty()) return;

    std::vector<uint32_t> oldestFirst;
    oldestFirst.reserve(gaps_.size());
    for (const auto& gap : gaps_) oldestFirst.push_back(gap.first);
    std::sort(oldestFirst.begin(), oldestFirst.end(), [this](uint32_t a, uint32_t b) {
        return highestSeq_ - a > highestSeq_ - b;
    });

    for (uint32_t seq : oldestFirst) {
        const auto found = gaps_.find(seq);
        if (found == gaps_.end()) continue;
        GapState& gap = found->second;

        if (gap.requestCount >= config_.maxRequestsPerSeq) {
            ++stats_.givenUp;
            ++stats_.lostForReal;
            gaps_.erase(found);
            continue;
        }

        const uint32_t age = highestSeq_ - seq;
        if (age < reorderPackets_) continue;
        if (gap.requestCount > 0 &&
            highestSeq_ - gap.lastRequestAtHighestSeq < reorderPackets_) {
            continue;
        }

        out.push_back(seq);
        ++gap.requestCount;
        gap.lastRequestAtHighestSeq = highestSeq_;
        ++stats_.nacksRequested;

        if (gap.requestCount >= config_.maxRequestsPerSeq) {
            ++stats_.givenUp;
            ++stats_.lostForReal;
            gaps_.erase(found);
        }
    }
    stats_.pending = gaps_.size();
}

void NackTracker::reset() {
    gaps_.clear();
    highestSeq_ = 0;
    reorderPackets_ = 0;
    hasBaseline_ = false;
    stats_.pending = 0;
}
