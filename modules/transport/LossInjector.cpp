/**
 * @file    LossInjector.cpp
 * @brief   LossInjector.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/LossInjector.h"

bool lossInjectionEnabled(const LossConfig& config) {
    // TODO(M4.0): seed 不是哨兵, 且 lossPercent 落在 (0, 100] 内才算开启。
    (void)config;
    return false;
}

bool shouldDropPacket(const LossConfig& config, uint32_t seq, bool isRetransmit) {
    // TODO(M4.0): 按以下顺序判断, 顺序本身是契约的一部分 ——
    //   1. 没开启 -> false
    //   2. isRetransmit -> false   (在算哈希之前就短路, 见头文件的理由)
    //   3. lossPercent >= 100 -> true
    //   4. 把 (seed, seq) 混成一个 32 位值, 取模 100 与 lossPercent 比较
    //
    // 第 4 步的混合函数要满足两条, 都能被测试证伪:
    //   - 相邻 seq 的结果不能相关。seq 是 +1 递增的, 用 (seed + seq) % 100 这种
    //     线性写法会丢出**连续的一段** —— 那不是随机丢包, 是周期性突发丢包,
    //     FEC 恰好最怕这个(组内丢 >= 2 就救不回来), 测出来的恢复率会莫名其妙地低,
    //     而你会以为是 FEC 写错了。
    //   - 换一个 seed, 丢的那批包要基本换一批。
    //
    // 现成的选择: 32 位版 splitmix / murmur3 的 finalizer(几行位运算, 无依赖),
    // 把 seed 和 seq 先混进一个 uint64 再 finalize。别自己发明。
    (void)config;
    (void)seq;
    (void)isRetransmit;
    return false;
}
