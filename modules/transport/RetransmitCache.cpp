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
    // 存入后先按时间、再按帧数上限淘汰:
    //   1. fragCount == 0 或 data.empty() -> 直接返回, 什么都不做。
    //      存进去一条 fragCount=0 的条目, 查的时候 `seq - baseSeq < 0` 永远不成立,
    //      不会崩但会白占内存; 更糟的是它让 bytes 统计和实际能重传的东西对不上。
    //   2. 构造 Entry: data 用 std::move 移进去(**这是零拷贝的关键, 别写成拷贝**),
    //      元信息从 view 复制, storedAtMs = nowMs
    //   3. push_back, ++framesStored, 累加 bytes
    //   4. evictExpired(nowMs)
    //   5. 再按 maxFrames 兜底淘汰(0 表示不限)
    if (fragCount == 0 || data.empty()) return;

    Entry entry;
    entry.data = std::move(data);
    entry.captureMs = view.captureMs;
    entry.frameId = view.frameId;
    entry.baseSeq = baseSeq;
    entry.fragCount = fragCount;
    entry.isKey = view.isKey;
    entry.storedAtMs = nowMs;

    stats_.bytes += entry.data.size();
    entries_.push_back(std::move(entry));
    ++stats_.framesStored;
    stats_.frames = entries_.size();

    evictExpired(nowMs);
    while (config_.maxFrames != 0 && entries_.size() > config_.maxFrames) {
        stats_.bytes -= entries_.front().data.size();
        entries_.pop_front();
        ++stats_.framesEvicted;
    }
    stats_.frames = entries_.size();
}

bool RetransmitCache::find(uint32_t seq, EncodedFrameView& outView, uint16_t& outFragIndex,
                           uint16_t& outFragCount) {
    // 区间命中用无符号差值判断, 自然支持 seq 回绕:
    //   线性扫 entries_, 命中条件是 `seq - e.baseSeq < e.fragCount`
    //   —— **无符号减法**, seq 会回绕, 写成 `seq >= baseSeq && seq < baseSeq + fragCount`
    //   会在回绕点查不到本来在缓存里的帧, 现象是"重传偶发失效, 跑久了才出现"。
    //
    //   命中: 填 outView(data 指向 e.data, len = e.data.size(), frameId/captureMs/isKey
    //         从 e 取), outFragIndex = seq - e.baseSeq, outFragCount = e.fragCount,
    //         ++hits, return true
    //   未命中: ++misses, return false  (outView 等出参**不要动**)
    for (const Entry& entry : entries_) {
        const uint32_t fragmentIndex = seq - entry.baseSeq;
        if (fragmentIndex >= entry.fragCount) continue;

        EncodedFrameView view;
        view.data = entry.data.data();
        view.len = entry.data.size();
        view.frameId = entry.frameId;
        view.captureMs = entry.captureMs;
        view.isKey = entry.isKey;

        outView = view;
        outFragIndex = static_cast<uint16_t>(fragmentIndex);
        outFragCount = entry.fragCount;
        ++stats_.hits;
        return true;
    }

    ++stats_.misses;
    return false;
}

void RetransmitCache::evictExpired(uint64_t nowMs) {
    // 从头部弹掉 `nowMs - storedAtMs >= retentionMs` 的条目,
    //             ++framesEvicted, 扣减 bytes。
    //             entries_ 天然按 storedAtMs 递增, 所以从头弹到第一个不过期的就能停。
    //             注意 nowMs 比 storedAtMs 小的情况(不该发生, 但别让无符号减法回绕成
    //             一个巨大的年龄, 那会把整个缓存清空)。
    while (!entries_.empty()) {
        const Entry& oldest = entries_.front();
        if (nowMs < oldest.storedAtMs) break;

        const uint64_t age = nowMs - oldest.storedAtMs;
        if (config_.retentionMs > 0 &&
            age < static_cast<uint64_t>(config_.retentionMs)) {
            break;
        }

        stats_.bytes -= oldest.data.size();
        entries_.pop_front();
        ++stats_.framesEvicted;
    }
    stats_.frames = entries_.size();
}

void RetransmitCache::reset() {
    entries_.clear();
    stats_.frames = 0;
    stats_.bytes = 0;
}
