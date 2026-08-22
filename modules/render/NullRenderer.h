/**
 * @file    NullRenderer.h
 * @brief   不开窗的渲染器: 校验帧的不变量并计数, 让整条流水线在无显示器环境跑完
 * @author  zzj
 * @date    2026-08-22
 */
#pragma once

#include <cstdint>

#include "modules/render/IRenderer.h"

/**
 * 空渲染器 —— ctest 和无显示器环境下的主力
 *
 * 它**不是**"什么都不做"。参照 NullSource: 那个也不是"什么都不产出", 它画了一张
 * 内容只取决于 frameId 的合成画面, 好让下游出问题时看得出来。这边对应的是:
 * 每一帧都过一遍不变量检查, 坏帧计数, 让测试有东西可断言。
 *
 * **能查的是"结构", 不是"内容"** —— 它没有参考图, 而且 H.264 是有损压缩,
 * 解出来的像素值本来就跟原图不完全相等。所以只查各字段之间自不自洽。
 *
 * @note 检查逻辑只属于这里, SdlRenderer 一行都不该有: 那边的验收是人眼。
 */
class NullRenderer : public IRenderer {
public:
    NullRenderer() = default;
    ~NullRenderer() override;

    Status open(const RendererConfig& cfg) override;
    Status renderFrame(const RawFrame& frame) override;
    bool   pumpEvents() override;
    const RendererStats& stats() const override;
    void   close() override;

private:
    /**
     * @brief 单帧内部的不变量 —— 各字段之间必须自洽
     *
     * 查这几条:
     *   - width > 0 && height > 0
     *   - width % 2 == 0 && height % 2 == 0   YUV420P 的色度平面是 1/2 下采样, 奇数除不尽
     *   - fmt == PixelFormat::YUV420P
     *   - data.size() == width * height * 3 / 2   Y(w*h) + U(w*h/4) + V(w*h/4)
     *
     * @note 最后一条才是真正的检查。"data 非空"是它的一个特例, 而且太弱 ——
     *          一个 640x480 的帧带着 100 字节数据同样是坏帧, "非空"会放它过去。
     * @note RawFrame 是三平面紧凑排布(Frame.h: yStride() == width), 没有行填充,
     *          所以这个等式是硬的。哪天改成直接吃解码器输出(带 linesize), 这里要跟着改。
     */
    Status checkFrame(const RawFrame& frame) const;

    /**
     * @brief 跨帧的不变量 —— 相邻两帧之间必须成立
     *
     * 查这几条:
     *   - captureMs 严格递增, **但 captureMs == 0 时跳过比较**
     *   - frameId   严格递增, 允许跳号(丢帧就跳了), 不要求 +1
     *
     * @note captureMs == 0 是哨兵不是时间: Decoder::drainFrames() 查不到 pending_
     *          元信息时, 帧会带着 0 出去, 那正是 DecoderStats::framesMissingMeta
     *          存在的原因(NOTES.md D14)。不跳过就会被自己的哨兵值误报。
     * @note **不查"宽高跨帧一致"** —— 那不是不变量。H.264 码流自带 SPS, 对端重开
     *          编码器时分辨率会变, 而那恰恰是渲染器该正确处理的场景, 不是错误。
     * @note frameId 不查回绕: RawFrame::frameId 是 uint64, 30fps 跑满 2^64 要约
     *          195 亿年。会回绕的是 PacketHeader 里那个 uint32, 而它已经在
     *          JitterBuffer::extendFrameId() 里用扩展序列号解决掉了 ——
     *          回绕是传输层的问题, 在传输层就该终结, 不该一路渗到渲染器。
     */
    Status checkAgainstPrevious(const RawFrame& frame) const;

    RendererConfig config_;
    RendererStats  stats_;

    bool opened_ = false;

    // 上一帧的信息, 供跨帧检查和 texturesRebuilt 计数使用
    bool     hasPrev_       = false;
    int      prevWidth_     = 0;
    int      prevHeight_    = 0;
    uint64_t prevCaptureMs_ = 0;
    uint64_t prevFrameId_   = 0;
};
