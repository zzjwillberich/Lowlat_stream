/**
 * @file    Encoder.h
 * @brief   YUV420P -> H.264 Annex B 编码器, 低延迟参数
 * @author  zzj
 * @date    2026-07-28
 */
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "common/Status.h"
#include "modules/capture/Frame.h"

// 前向声明而不是 #include <libavcodec/avcodec.h>: 指针成员不需要完整类型。
// 这样 FFmpeg 的头文件和 include 路径就只出现在 Encoder.cpp 里, 不会传染给
// 每一个用到编码器的 .cpp —— 头文件是给别人看的合同, 不该写明自己用了哪个库。
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

/**
 * 编码参数
 *
 * @note gop 是取舍轴: 短 GOP 起播快、丢包后恢复快(下一个 IDR 来得早),
 *          代价是 IDR 帧大、码率高。M4 做丢包对抗时会回来调这个值。
 */
struct EncoderConfig {
    int width       = 640;
    int height      = 480;
    int fps         = 30;
    int bitrateKbps = 2000;
    int gop         = 30;  // 关键帧间隔(帧)
};

/**
 * 一帧压缩后的码流
 *
 * @note 与 RawFrame 一样禁止拷贝: M2 的重传缓存要存最近 N 个包, 隐式拷贝会在
 *          那里变成看不见的内存和延迟。需要复制就显式写。
 */
struct EncodedFrame {
    /**
     * @brief Annex B 字节流, **含起始码**, 可直接 dump 成 .h264 喂 ffplay
     */
    std::vector<uint8_t> data;

    /**
     * @brief 从 RawFrame 透传过来的采集时刻, 不在这里重新取时间
     *
     * @note 重新取就变成"编码完成时刻", 端到端延迟里会少算编码耗时 —— 那正是要测的东西之一。
     */
    uint64_t captureMs = 0;

    /**
     * @brief 从 RawFrame 透传的帧号
     */
    uint64_t frameId = 0;

    /**
     * @brief 是否关键帧(IDR)
     *
     * @note M4 的背压策略靠它决定"保谁丢谁": 队列满时丢非关键帧, IDR 一定要送到,
     *          丢了就要花屏到下一个 IDR。
     */
    bool isKey = false;

    EncodedFrame() = default;
    EncodedFrame(const EncodedFrame&)            = delete;
    EncodedFrame& operator=(const EncodedFrame&) = delete;
    EncodedFrame(EncodedFrame&&)                 = default;
    EncodedFrame& operator=(EncodedFrame&&)      = default;
};

/**
 * libavcodec/x264 封装 —— 低延迟编码
 *
 * 用法: open() -> encode() * N -> flush() -> close()
 *
 * 低延迟的三件事(都在 open 里):
 * - `preset=ultrafast` 编码耗时最短, 画质换速度
 * - `tune=zerolatency` 关 lookahead、关 B 帧、关帧级多线程
 * - `max_b_frames=0`   B 帧要等后面的帧才能编, 天然引入延迟
 *
 * @note **不设 AV_CODEC_FLAG_GLOBAL_HEADER**。那个 flag 的含义是"把 SPS/PPS 抽到
 *          extradata、不要放进码流", 是给 MP4/FLV 这类容器用的。我们要的恰恰相反:
 *          每个 IDR 前都重复一次 SPS/PPS, 这样 M5 中途加入的观众才能立刻解码。
 *          x264 默认就是重复的, 不设这个 flag 即可 —— 实测设了以后码流里 SPS 出现 0 次。
 *
 * @note 本类**不是线程安全**的, 由编码线程独占。
 */
class Encoder {
public:
    Encoder() = default;

    /**
     * @brief 析构时释放 AVCodecContext/AVFrame/AVPacket
     *
     * @note 这些是 C 风格的裸资源, 忘了释放就是每次 open 泄漏一份。
     *          RAII 的意义就是让"忘记"不再可能。
     */
    ~Encoder();

    // 持有独占的编码器上下文, 拷贝没有合理语义
    Encoder(const Encoder&)            = delete;
    Encoder& operator=(const Encoder&) = delete;

    /**
     * @brief 打开编码器
     *
     * @param cfg 编码参数
     *
     * @return Ok         成功
     *  InvalidArg 参数不合法(分辨率非偶数、fps/码率/gop 非正)
     *  Internal   libx264 不可用或 libavcodec 调用失败
     *
     * @note 可重复调用, 内部会先 close() 掉上一次的资源。
     */
    Status open(const EncoderConfig& cfg);

    /**
     * @brief 编码一帧
     *
     * @param in  待编码的原始帧, 分辨率必须与 open 时一致
     * @param out 出参, **追加**产出的码流帧; 可能追加 0 个或多个
     *
     * @return Ok         成功(哪怕这次一个包都没吐出来)
     *  InvalidArg 输入帧的分辨率/格式对不上
     *  Internal   编码器出错
     *
     * @note 一次 send 可能吐 0 个包(编码器还在攒)也可能吐多个, 所以 out 是数组而不是单值。
     *          调用方不要假设"一进一出" —— 虽然 zerolatency 下事实上就是一进一出。
     */
    Status encode(const RawFrame& in, std::vector<EncodedFrame>& out);

    /**
     * @brief 冲刷编码器内部缓存
     *
     * @param out 出参, 追加剩余的码流帧
     *
     * @return Ok 成功
     *
     * @note **close() 之前必须先 flush()**, 否则编码器里攒着的帧就没了,
     *          表现为 dump 文件尾部缺几帧。
     * @note flush 之后编码器进入排空状态, 不能再 encode(), 只能 close()。
     */
    Status flush(std::vector<EncodedFrame>& out);

    /**
     * @brief 释放编码器资源
     *
     * @note 幂等, 可重复调用。
     */
    void close();

private:
    // 把 RawFrame 的三个平面按 AVFrame::linesize **逐行**拷进去
    Status fillAvFrame(const RawFrame& in);

    // 循环取包直到 EAGAIN —— 一次 send 可能对应多个包
    Status drainPackets(std::vector<EncodedFrame>& out);

    /**
     * @brief 等待编码器吐出的帧的元信息, 按 pts 索引
     *
     * @note 编码器可能缓存和重排, 拿到一个包时"当前帧"未必就是它对应的那一帧,
     *          所以时间戳和帧号要靠 pts 查回来, 不能直接用最近一次 encode 的入参。
     *          zerolatency 下这张表里通常只有一项。
     */
    struct Pending {
        uint64_t captureMs = 0;
        uint64_t frameId   = 0;
    };
    std::map<int64_t, Pending> pending_;

    AVCodecContext* ctx_   = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_   = nullptr;

    EncoderConfig cfg_;

    /**
     * @brief 下一帧的 pts, 编码器要求单调递增
     *
     * @note 不直接用 RawFrame::frameId: 采集源重新 open 后帧号会归零, pts 一旦回退
     *          编码器就会报错或丢帧。这里自己数, 帧号通过 pending_ 关联回去。
     */
    int64_t nextPts_ = 0;
};
