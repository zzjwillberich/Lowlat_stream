/**
 * @file    NackTracker.cpp
 * @brief   NackTracker.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/NackTracker.h"

#include "modules/transport/Packet.h"

NackTracker::NackTracker(NackTrackerConfig config) : config_(std::move(config)) {}

void NackTracker::onPacket(uint32_t seq, uint16_t fragCount) {
    // TODO(M4.1):
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
    (void)seq;
    (void)fragCount;
}

void NackTracker::collectNackTargets(std::vector<uint32_t>& out) {
    // TODO(M4.1):
    //   out.clear() 先做 —— 调用方复用这个 vector, 残留会变成重复请求
    //
    //   遍历缺口表, 从老到新, 挑出同时满足两条的:
    //     a. highestSeq_ - seq >= 当前乱序容忍   (等够了一帧的包数, 不是乱序)
    //     b. requestCount < maxRequestsPerSeq
    //     c. 且距上次请求又过了一帧的包数(第一次请求时这条自动满足)
    //   挑出来的: ++requestCount, 记下 lastRequestAtHighestSeq, ++stats_.nacksRequested
    //
    //   requestCount 到达上限的: ++givenUp, ++lostForReal, 删掉 —— 别留在表里空占位置
    (void)out;
}

void NackTracker::reset() {
    // TODO(M4.1): 清空缺口表和 highestSeq_/hasBaseline_; stats_ 是否一起清由你定
    //             (倾向于**不清** —— 统计是整轮运行的账, 换对端不该把前面的抹掉)
}
