/**
 * @file    RetransmitCache.cpp
 * @brief   RetransmitCache.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/RetransmitCache.h"

#include <utility>

RetransmitCache::RetransmitCache(RetransmitCacheConfig config) : config_(std::move(config)) {}

void RetransmitCache::store(const EncodedFrameView& view, std::vector<uint8_t> data,
                            uint32_t baseSeq, uint16_t fragCount, uint64_t nowMs) {
    // TODO(M4.1):
    //   1. fragCount == 0 或 data.empty() -> 直接返回, 什么都不做。
    //      存进去一条 fragCount=0 的条目, 查的时候 `seq - baseSeq < 0` 永远不成立,
    //      不会崩但会白占内存; 更糟的是它让 bytes 统计和实际能重传的东西对不上。
    //   2. 构造 Entry: data 用 std::move 移进去(**这是零拷贝的关键, 别写成拷贝**),
    //      元信息从 view 复制, storedAtMs = nowMs
    //   3. push_back, ++framesStored, 累加 bytes
    //   4. evictExpired(nowMs)
    //   5. 再按 maxFrames 兜底淘汰(0 表示不限)
    (void)view;
    (void)data;
    (void)baseSeq;
    (void)fragCount;
    (void)nowMs;
}

bool RetransmitCache::find(uint32_t seq, EncodedFrameView& outView, uint16_t& outFragIndex,
                           uint16_t& outFragCount) {
    // TODO(M4.1):
    //   线性扫 entries_, 命中条件是 `seq - e.baseSeq < e.fragCount`
    //   —— **无符号减法**, seq 会回绕, 写成 `seq >= baseSeq && seq < baseSeq + fragCount`
    //   会在回绕点查不到本来在缓存里的帧, 现象是"重传偶发失效, 跑久了才出现"。
    //
    //   命中: 填 outView(data 指向 e.data, len = e.data.size(), frameId/captureMs/isKey
    //         从 e 取), outFragIndex = seq - e.baseSeq, outFragCount = e.fragCount,
    //         ++hits, return true
    //   未命中: ++misses, return false  (outView 等出参**不要动**)
    (void)seq;
    (void)outView;
    (void)outFragIndex;
    (void)outFragCount;
    return false;
}

void RetransmitCache::evictExpired(uint64_t nowMs) {
    // TODO(M4.1): 从头部弹掉 `nowMs - storedAtMs >= retentionMs` 的条目,
    //             ++framesEvicted, 扣减 bytes。
    //             entries_ 天然按 storedAtMs 递增, 所以从头弹到第一个不过期的就能停。
    //             注意 nowMs 比 storedAtMs 小的情况(不该发生, 但别让无符号减法回绕成
    //             一个巨大的年龄, 那会把整个缓存清空)。
    (void)nowMs;
}

void RetransmitCache::reset() {
    // TODO(M4.1): 清空 entries_、frames/bytes 归零; 累计统计保留(同 NackTracker::reset)
}
