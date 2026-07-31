/**
 * @file    NullSource.h
 * @brief   不依赖硬件的合成画面采集源
 * @author  zzj
 * @date    2026-07-28
 */
#pragma once

#include <chrono>
#include <cstdint>

#include "modules/capture/ISource.h"

/**
 * 合成画面源 —— 没有摄像头也能跑完整条流水线
 *
 * 产出的画面有两个刻意设计:
 * - **滚动的对角渐变**: 一眼能看出画面在不在动、卡没卡
 * - **画在画面上的帧号**: 后面验"逐字节一致"(M2)、看延迟(M3)、看丢帧花屏(M4)
 *   全靠肉眼对帧号。控制台打印的帧号只能证明**代码**跑到了, 证明不了**画面**对不对。
 *
 * 它同时是压测和复现问题的手段: 合成画面每次都一样, 摄像头拍到的永远不一样。
 *
 * @note 不持有任何系统资源, 所以没有自定义析构函数 —— close() 只是把状态标回未打开。
 *          V4l2Source 就不一样了(fd/mmap/STREAMOFF), 那边必须写析构。
 */
class NullSource : public ISource {
public:
    NullSource() = default;

    Status open(const SourceConfig& cfg) override;
    Status readFrame(RawFrame& out) override;
    void   close() override;

private:
    // 画滚动的对角亮度渐变 + 色度渐变, 内容只取决于 out.frameId
    void drawBackground(RawFrame& out) const;

    // 用 5x7 点阵把 out.frameId 画在左上角
    void drawFrameId(RawFrame& out) const;

    /**
     * @brief 按帧率节流, 睡到第 frameId_ 帧的应到时刻
     *
     * @note 以 start_ 为**绝对基准**算目标时刻, 而不是每帧 sleep(1/fps):
     *          后者会把每帧的调度误差累加起来, 跑一分钟就明显偏慢。
     */
    void throttle() const;

    /**
     * @brief open() 时协商好的参数, 之后不再变
     */
    SourceConfig cfg_;

    /**
     * @brief 是否已 open, close() 后 readFrame 返回 Closed
     */
    bool opened_ = false;

    /**
     * @brief 下一帧的序号, open() 时归零
     */
    uint64_t frameId_ = 0;

    /**
     * @brief 节流的时间基准, open() 时取一次
     */
    std::chrono::steady_clock::time_point start_;
};
