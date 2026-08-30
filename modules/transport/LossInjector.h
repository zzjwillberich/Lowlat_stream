/**
 * @file    LossInjector.h
 * @brief   可复现的丢包注入 —— M4 的测试手段, 先于所有弱网对抗逻辑
 * @author  zzj
 * @date    2026-08-28
 *
 * @note **这是测试工具, 不是业务功能。** 默认必须是不生效的(见 LossConfig::seed)。
 *
 * @note 为什么是自由函数而不是类: 丢不丢完全由 (seed, seq) 决定, 没有任何需要
 *          记住的东西。无状态因此也无需考虑线程安全 —— 收包线程直接调。
 */
#pragma once

#include <cstdint>

#include "modules/transport/Packet.h"

/**
 * @brief seed 的哨兵值: 表示**不注入**
 *
 * @note 用哨兵而不是单独一个 bool 开关, 是为了让"什么都不给 = 什么都不发生"
 *          成为默认行为。同 `--target=` 空串表示不发送。
 */
constexpr uint32_t LOSS_SEED_DISABLED = 0;

/**
 * 丢包注入的配置。
 *
 * @note 两个字段**都**能关掉注入(seed 为哨兵、或者丢包率为 0)。但
 *          "给了 lossPercent 却没给 seed" 是个危险组合 —— 它会静默地什么都不做,
 *          你以为测了 10% 丢包, 其实测了 0%。调用方必须把这个组合当成配置错误
 *          挡下来, 不能默默放过。
 */
struct LossConfig {
    /** @brief 伪随机种子; LOSS_SEED_DISABLED 表示不注入 */
    uint32_t seed = LOSS_SEED_DISABLED;

    /** @brief 丢包率, 整数百分比 0~100; 0 表示不丢 */
    int lossPercent = 0;
};

/**
 * @brief 这套配置会不会真的丢包
 *
 * @return seed 不是哨兵、且 lossPercent > 0 时为 true
 *
 * @note 调用方用它决定要不要在收包路径上多解一次包头 —— 关掉时开销必须是零。
 */
bool lossInjectionEnabled(const LossConfig& config);

/**
 * @brief 判断序号为 seq 的这一个包该不该被"丢掉"
 *
 * @param config       注入配置
 * @param seq          PacketHeader::seq, **该类型**的包序号
 * @param type         PacketHeader::type; 必须混进哈希, 理由见下
 * @param isRetransmit 这一片是不是重传的(DataHeader::FLAG_RETRANSMIT)
 *
 * @return true 表示调用方应当**当作没收到**这个包 —— 直接返回, 不要交给
 *         FrameAssembler, 也不要计入任何真实统计
 *
 * @note **纯函数**: 同样的 (seed, seq) 永远给同样的答案, 与到达顺序、与之前
 *          调过多少次、与是否有重传包插队全都无关。
 *
 *          这一条是整个注入器存在的意义。反面写法是"持有一个 mt19937, 每来一个包
 *          掷一次骰" —— 那样在没有重传时也能复现, 但你把 NACK 写出来的那一刻,
 *          重传包会多消耗一次掷骰, 之后每个包拿到的骰子**全部错位**。
 *          "用同一个种子对比优化前后"这件事, 恰好在开始优化的那一刻坏掉,
 *          而且丢包率看上去还是对的, 坏得非常安静。
 *
 * @note **重传包永不丢弃**(isRetransmit 为 true 时恒返回 false)。因为丢不丢是
 *          seq 的纯函数, 同一个 seq 重传多少次都会得到同一个"丢", NACK 的恢复路径
 *          就永远跑不出成功的一次。接收端自己看不出这是第几次传输, 所以由发送端
 *          在 flags 里说明。
 *
 * @note seq 回绕**不需要特殊处理**: 哈希对 uint32 全域都有定义, 回绕只是换了个输入。
 *
 * @note **type 必须参与混合。** M4.2 起 DATA 和 FEC 各有自己的 seq 计数器, 两条流的
 *          序号空间重叠 —— 只用 (seed, seq) 的话, FEC#3 和 DATA#3 拿到同一个哈希,
 *          **逐位同丢同留**。直接危害不大(FEC#g 覆盖的是 DATA seq [g*K, g*K+K-1],
 *          只有 g=0 那组才自己撞自己), 但那是结构性的假相关, 不是网络会有的行为,
 *          它会以预测不到的方式影响测出来的 FEC 恢复率。
 *
 * @note 加上 type 之后, **同一个种子丢掉的 DATA 包集合会和 M4.1 时不一样**。
 *          这不影响可复现性(重要的性质是"同一个种子可复现", 不是"某一批特定的包"),
 *          但拿 M4.1 的数字和 M4.2 的对比时, 基线要重跑一遍。
 */
bool shouldDropPacket(const LossConfig& config, uint32_t seq, PacketType type,
                      bool isRetransmit);
