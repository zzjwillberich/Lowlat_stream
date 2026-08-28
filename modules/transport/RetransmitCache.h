/**
 * @file    RetransmitCache.h
 * @brief   发送端的重传缓存: 按 seq 反查最近发过的帧 (M4.1)
 * @author  zzj
 * @date    2026-08-28
 *
 * @note **按时间淘汰, 不按帧数也不按字节数。** 它存在的理由就是一段时间 ——
 *       覆盖"一个 NACK 最晚什么时候可能到":
 *
 *       ```text
 *       检测延迟(一帧) + RTT + 最多重发 N 次 × 一帧间隔
 *       = 33ms + RTT + 3 × 33ms  ≈  130ms + RTT
 *       回环 RTT≈0    -> 4 帧 ≈ 35 KB
 *       跨机 RTT=30ms -> 5 帧 ≈ 43 KB
 *       ```
 *
 *       按帧数定的话, IDR 和 P 帧差一个数量级, 同样 8 帧可能是 30KB 也可能是 300KB,
 *       内存上限不可控; 按字节数定的话, "够不够用"要换算成时间才说得清, 而那取决于
 *       码率。按时间定, 码率变了它自己会跟着变。详见 [[D23]]。
 *
 * @note 存帧而不是存打好的整包, 真正的理由是**零拷贝**而不是省内存(实测只省 1.9%,
 *       因为包里装的本来就是帧数据, 多出来的只有 21 字节头)。sendLoop 里的
 *       `packets_` 是跨帧复用的成员, 缓存整包必须每帧拷一份出来, 把 M2 那条
 *       "跨帧复用让堆分配归零"的设计废掉; 而帧本来就要在循环末尾析构,
 *       改成 move 进来一次拷贝都没有。
 *
 * @note 本类**不认识 EncodedFrame**, 只收裸字节 + 元信息 —— 同 Packetizer 收
 *       EncodedFrameView 的理由: lltransport 不能依赖 libavcodec, 否则 M5 的
 *       服务端也要背上编解码器的头文件。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "modules/transport/Packetizer.h"

/**
 * 淘汰策略。
 */
struct RetransmitCacheConfig {
    /**
     * @brief 保留时长(毫秒); 比这更老的帧会被淘汰
     *
     * @note 取值要盖住 `检测延迟 + RTT + 重发次数 × 间隔`。设小了 NACK 来了查不到
     *          (miss 上升, 重传形同虚设); 设大了只是多占内存, 不影响正确性 ——
     *          所以宁可往大里给。
     */
    int retentionMs = 200;

    /**
     * @brief 帧数硬上限, 0 表示不限
     *
     * @note 时间是主判据, 这个只是**兜底**: 码率异常高或者时钟出问题时,
     *          防止内存无界。同 FrameAssembler 的 maxPendingFrames ——
     *          任何按外部输入增长的容器都要有一个不依赖那个输入的上限。
     */
    size_t maxFrames = 256;
};

/** 只读计数器 */
struct RetransmitCacheStats {
    /** @brief 存进来过的帧数 */
    uint64_t framesStored = 0;

    /** @brief 因超时或超上限被淘汰的帧数 */
    uint64_t framesEvicted = 0;

    /** @brief find() 命中的次数 —— 重传真正发得出去的那些 */
    uint64_t hits = 0;

    /**
     * @brief find() 未命中的次数
     *
     * @note 稳态下应当很小。持续非 0 说明 retentionMs 太短、或者对端的
     *          重传窗口比这边的保留时长还长 —— 两边的时间预算没对齐,
     *          现象是"NACK 一直发但重传一次都没成功"。这个数是唯一能区分
     *          "NACK 没发出去"和"发出去了但这边没货"的线索。
     */
    uint64_t misses = 0;

    /** @brief 当前帧数 */
    size_t frames = 0;

    /** @brief 当前占用的载荷字节数(不含每条的元信息) */
    size_t bytes = 0;
};

/**
 * 发送端重传缓存。
 *
 * 用法(发送线程内, 单线程, 不需要加锁):
 * ```cpp
 * // 打完包之后
 * cache.store(view, std::move(frame->data), baseSeq, fragCount, nowMs);
 *
 * // 收到 NACK 之后
 * EncodedFrameView view;
 * uint16_t fragIndex = 0, fragCount = 0;
 * if (cache.find(seq, view, fragIndex, fragCount)) {
 *     packetizeOneFragment(streamId, seq, view, fragIndex, fragCount, true, out);
 * }
 * ```
 */
class RetransmitCache {
public:
    explicit RetransmitCache(RetransmitCacheConfig config);

    RetransmitCache(const RetransmitCache&) = delete;
    RetransmitCache& operator=(const RetransmitCache&) = delete;

    /**
     * @brief 把刚发完的一帧存进来
     *
     * @param view      打包时用的视图; 除 data 指针外的元信息会被复制下来
     * @param data      帧字节, **移入** —— 调用方的 vector 之后为空
     * @param baseSeq   这一帧第一个分片的 seq
     * @param fragCount 这一帧的分片数, 必须 >= 1
     * @param nowMs     当前 steady 时刻, 用于淘汰
     *
     * @note 顺手按 nowMs 淘汰过期条目, 调用方不必单独调 evictExpired()。
     * @note fragCount 为 0 或 data 为空时**什么都不做** —— 缓存不该因为上游给了
     *          坏参数而崩, 但也不能存进去一条查出来会越界的条目。
     */
    void store(const EncodedFrameView& view, std::vector<uint8_t> data, uint32_t baseSeq,
               uint16_t fragCount, uint64_t nowMs);

    /**
     * @brief 按 seq 反查它属于哪一帧的第几片
     *
     * @param seq          NACK 请求的 seq
     * @param outView      出参; **data 指向缓存内部的存储**
     * @param outFragIndex 出参, seq 在帧内的分片下标
     * @param outFragCount 出参, 该帧的分片数
     *
     * @return true 命中(++hits); false 不在缓存里(++misses)
     *
     * @note **outView.data 的有效期只到下一次 store() 或 evictExpired()。**
     *          调用方必须立刻 packetizeOneFragment 并发出去, 不能存着待会儿用。
     *          写成视图而不是拷贝, 正是为了不在重传路径上多一次 memcpy。
     *
     * @note 反查靠 `baseSeq <= seq < baseSeq + fragCount`, 距离必须用**无符号减法**
     *          (`seq - baseSeq < fragCount`) —— seq 会回绕, 直接比大小会在回绕点
     *          查不到本来在缓存里的帧。
     */
    bool find(uint32_t seq, EncodedFrameView& outView, uint16_t& outFragIndex,
              uint16_t& outFragCount);

    /**
     * @brief 淘汰比 retentionMs 更老的条目
     *
     * @note store() 里已经会调, 单独暴露是给"发送端空转、很久没有新帧"的情况用 ——
     *          那时内存该释放, 而不是等下一帧才释放。
     */
    void evictExpired(uint64_t nowMs);

    const RetransmitCacheStats& stats() const { return stats_; }

    /** @brief 清空; 换对端或重新起流时调 */
    void reset();

private:
    struct Entry {
        std::vector<uint8_t> data;
        uint64_t captureMs = 0;
        uint64_t frameId = 0;
        uint32_t baseSeq = 0;
        uint16_t fragCount = 0;
        bool isKey = false;
        uint64_t storedAtMs = 0;
    };

    RetransmitCacheConfig config_;
    RetransmitCacheStats stats_;

    /** @brief 按存入时间排列的帧; 淘汰只从头部弹出 */
    std::deque<Entry> entries_;
};
