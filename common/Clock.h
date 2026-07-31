/**
 * @file    Clock.h
 * @brief   单调时钟工具, 延迟测量的统一入口
 * @author  zzj
 * @date    2026-07-28
 */
#pragma once

#include <chrono>
#include <cstdint>

/**
 * @brief 取当前单调时刻, 毫秒
 *
 * @return steady_clock 自其起点以来的毫秒数
 *
 * @note **返回值的绝对大小没有意义**, 只能用来相减。steady_clock 的起点由实现决定
 *          (Linux 上是开机时刻), 不是 1970 年。想打印给人看的时间戳请用日志。
 * @note 用 steady_clock 而不是 system_clock: 后者会被 NTP 校时或用户改表往回拨,
 *          拨一次就能算出负延迟。
 * @note **跨机器不可比**: 两台机器的起点毫不相干。真要跨机器测端到端延迟,
 *          见 README"已知限制/待办"里的方案。
 */
inline uint64_t steadyNowMs() {
    const auto d = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}
