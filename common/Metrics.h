/**
 * @file    Metrics.h
 * @brief   延迟采样与百分位统计
 * @author  zzj
 * @date    2026-08-24
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * 端到端延迟记录器
 *
 * 攒样本, 退出时给出 p50/p95/p99/max。**只攒不算** —— 排序推迟到真正要取数的
 * 那一刻, 因为每加一个样本就重排一次纯属浪费, 而且中途没人看那个结果。
 *
 * ## 一个样本是什么
 *
 * 起点是 `RawFrame::captureMs`(采集那一刻盖的章, 一路透传), 终点是渲染提交
 * 返回那一刻。两个端点的定义见 NOTES.md **D10**, 报数字时必须一起写出来。
 *
 * ## 采样点在渲染器**外面**
 *
 * ```cpp
 * const Status st = renderer->renderFrame(f);
 * if (st.isOk() && f.captureMs != 0) {
 *     const uint32_t now32 = static_cast<uint32_t>(steadyNowMs());
 *     recorder.add(now32 - static_cast<uint32_t>(f.captureMs));
 * }
 * ```
 *
 * @note **减法必须在 uint32 里做。** captureMs 在 Packetizer.cpp:47 被
 *          `static_cast<uint32_t>` 截断过, 而 steadyNowMs() 没有 —— 拿被切过的数
 *          去减没切过的数, 开机超过 49.7 天之后会安静地算出约 49.7 天的"延迟",
 *          不报错、不崩, 从此每一帧都是。两边都切到 32 位, 无符号减法自带模 2^32
 *          回绕, 跨越回绕点也正确(同 JitterBuffer::extendFrameId 处理帧号回绕)。
 * @note **只在 renderFrame 返回 Ok 时采样。** 没提交成功的帧没有终点,
 *          "它的延迟"这个说法不成立。用白名单而不是枚举失败码 —— 枚举法每加一种
 *          失败就得回来补一次, 漏一次就多一个假样本。
 * @note **captureMs == 0 要跳过。** 那是 Decoder 元信息丢失的哨兵
 *          (DecoderStats::framesMissingMeta), 不是时间。算进去就是
 *          `now32 - 0` = 开机至今的毫秒数, max 当场废掉。
 * @note **被丢掉的帧根本到不了这里。** 队列 B 满了走 forcePush 丢最老的(D11),
 *          那些帧不是"延迟很大", 是"不存在"。所以这个数字描述的是**活下来的帧**,
 *          越是扛不住越只有跑得快的帧被统计 —— 它会自我美化。报延迟时**必须**
 *          同时报丢帧数, 否则同一个 p99 可以既表示"完美"又表示"崩了"。
 *
 * @note 不线程安全, 也不需要: 采样和取数都在主线程(= 渲染线程, 见 M3.3 的线程模型),
 *          没有第二个线程碰它。
 */
class LatencyRecorder {
public:
    /**
     * @brief 记一个样本
     *
     * @param sampleMs 这一帧的端到端延迟(毫秒)
     *
     * @note O(1), 只是 push_back。真正的开销在 percentile() 那一次排序上。
     */
    void add(uint32_t sampleMs);

    /**
     * @brief 样本个数
     *
     * @return 自最近一次 reset()(或构造)以来 add() 的次数
     *
     * @note **调用 percentile() 之前必须先查这个。** 见 percentile 的说明。
     * @note 它同时也是报告里"实际统计了多少帧"那个字段 —— 注意它**不等于**
     *          采集了多少帧: 丢掉的、坏的、元信息缺失的都不在里面。
     */
    size_t count() const;

    /**
     * @brief 取第 p 百分位的样本值
     *
     * @param p 百分位, 取值 0~100 的**整数**; 超出范围会被夹到边界
     *
     * @return 排序后第 p% 位置那个样本(毫秒)
     *
     * @note 参数是整数 99 而不是浮点 0.99: `0.29 * 100` 在 IEEE double 下等于
     *          28.999999999999996, floor 之后掉成 28。最坏的是它**只在某些值上错** ——
     *          0.5/0.95/0.99 恰好都对, 也就是你会写的那几个测试用例恰好都过。
     *          整数版 `p * count / 100` 全程整数, 没有这个问题。
     * @note 中间量用 size_t: 写成 uint32_t 的话 `100 * count` 在 count 超过
     *          约 4295 万(30fps 跑 16.6 天)时溢出。
     * @note 代价: 问不了 p99.9。尾延迟有时候要看它, 到那天把刻度换成 /1000。
     * @note **不是 const**: 取数要排序, 排序就是改动容器。代价是这个对象不能被当
     *          const 传 —— 对一个存在意义就是"累积可变状态"的类来说不算损失。
     * @note **count() == 0 时返回值无意义**(实现返回 0)。调用方必须先查 count():
     *          0 是一个**合法的延迟值**, 直接打印出去就是
     *          "p50=0 p99=0 max=0" —— 一个把"什么都没发生"报成"完美"的谎。
     *          这个分支要写在**打印那一侧**, 不能指望这里拦。
     */
    uint32_t percentile(int p);

    /**
     * @brief 最大样本值
     *
     * @return 见过的最大延迟(毫秒); 没有样本时为 0
     *
     * @note 在 add() 里增量维护, 所以是 const 且 O(1), 不用排序。
     * @note 它和 p99 是两个不同的问题: p99 说"除了最倒霉的 1% 之外有多快",
     *          max 说"最惨的那一次有多惨"。一次卡顿就能把 max 拉满而 p99 纹丝不动 ——
     *          两个都要报。
     */
    uint32_t max() const;

    /**
     * @brief 清空所有样本
     *
     * @note 整场汇总用不上它。它是给"最近一段"那个记录器准备的 —— 每秒打一行
     *          "现在是不是正在变坏"需要一个滑动的窗口, 而整场累积的数字跑得越久
     *          对新变化越迟钝(后面的坏样本被前面海量的好样本稀释)。
     *          两个记录器回答两个不同的问题, 别指望一个顶两个。
     */
    void reset();

private:
    std::vector<uint32_t> samples_;
    uint32_t max_ = 0;
};
