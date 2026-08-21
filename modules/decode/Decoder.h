/**
 * @file    Decoder.h
 * @brief   H.264 Annex B -> YUV420P 解码器, 低延迟参数
 * @author  zzj
 * @date    2026-08-18
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "common/Status.h"
#include "modules/capture/Frame.h"

// 同 Encoder.h: 前向声明而不是 include <libavcodec/avcodec.h>。
// FFmpeg 的头文件和 include 路径只出现在 Decoder.cpp 里, 不传染给每个用到解码器的 .cpp。
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/**
 * 待解码的一帧, **只借不拥有**。
 *
 * @note 与 Packetizer.h 的 EncodedFrameView 是同一个套路: 传视图而不是传
 *          AssembledFrame, 是为了不让 decode 反向依赖 transport。解码器不需要知道
 *          这段码流是从网上来的还是从文件里读的 —— 它只认字节。
 *          这样它在单测里能直接吃 Encoder 的产物, 一个 socket 都不用碰。
 * @note 是**视图**: data 指向的内存必须在 decode() 返回前一直有效。
 */
struct CodedFrameView {
    /** @brief Annex B 码流首地址, 含起始码 */
    const uint8_t* data = nullptr;

    /** @brief 码流字节数 */
    size_t len = 0;

    /**
     * @brief 采集时刻(毫秒), **原样透传到输出, 不在这里重新取时间**
     *
     * @note 端到端延迟的终点在渲染线程, 起点是采集。中间任何一环重新取一次时间,
     *          测出来的都是个偏小的假数字 —— 而那正是这个项目要拿出去讲的核心指标。
     */
    uint64_t captureMs = 0;

    /** @brief 帧号, 原样透传 */
    uint64_t frameId = 0;
};

/**
 * 解码参数。
 */
struct DecoderConfig {
    /**
     * @brief 解码线程数, 必须 >= 1
     *
     * @note 这**不是性能旋钮, 是延迟旋钮**。见 Decoder 类注释里关于帧级多线程的说明:
     *          线程开多了不会更快出画, 只会更晚出画。
     */
    int threads = 1;
};

/**
 * 解码侧计数器。
 *
 * @note 需要它是因为**大多数码流损坏不会体现为返回值**: H.264 解码器非常宽容,
 *          遇到坏数据通常照样返回成功, 只是少吐一帧或者吐一帧花的。
 *          所以判断"解码到底正不正常"的实际依据是 framesIn 和 framesOut 对不对得上,
 *          而不是有没有收到错误码。
 */
struct DecoderStats {
    /** @brief 喂进去的码流帧数 */
    uint64_t framesIn = 0;

    /** @brief 吐出来的图像帧数(含 flush 的) */
    uint64_t framesOut = 0;

    /** @brief send_packet 明确报错的次数 */
    uint64_t decodeErrors = 0;

    /** @brief 因为不是 YUV420P 而走了 sws_scale 转换的帧数 */
    uint64_t convertedFrames = 0;

    /**
     * @brief pts 取件失败、元信息丢失的帧数
     *
     * @note 这类帧**画面是好的**, 照常交付, 只是不知道它是什么时候采的。`captureMs`
     *          保持 0 —— 那是个**永远不会合法出现**的值(它来自 steadyNowMs(), 要合法
     *          为 0 得在开机后第一毫秒内采到帧), 所以天然就是哨兵, M3.4 的延迟统计
     *          据此跳过这一笔。丢的 frameId 只是调试标签: 排序早在 JitterBuffer 就
     *          做完了, 下游全是 FIFO, 没人再读它。
     * @note **有哨兵还要计数器**: 哨兵让脏数据不进统计, 计数器让你知道脏数据有多少。
     *          分位数对离群值免疫是**有条件的** —— 1000 帧坏 1 帧 p99 没事, 坏 11 帧
     *          (超过 1%) p99 就开始被污染。没有这个数, 你无从判断自己在哪一侧。
     * @note 正常应恒为 0。它一涨就说明"一进一出"的假设破了(见类注释), 该查的是那里,
     *          不是往这儿加逻辑。
     */
    uint64_t framesMissingMeta = 0;
};

/**
 * libavcodec 封装 —— 低延迟解码。
 *
 * 用法: open() -> decode() * N -> flush() -> close()。与 Encoder 完全对称。
 *
 * **低延迟解码只有一件要紧事: 关掉帧级多线程。**
 * @code
 * ctx->thread_count = cfg.threads;      // 1
 * ctx->thread_type  = FF_THREAD_SLICE;  // 关键: 不是 FF_THREAD_FRAME
 * ctx->flags       |= AV_CODEC_FLAG_LOW_DELAY;
 * @endcode
 * `FF_THREAD_FRAME`(帧级多线程)靠"同时解 N 帧"提高吞吐, 代价是解码器要**先攒够 N 帧
 * 才吐第一帧**。它默认开着, 且 FFmpeg 不会告诉你。现象是"明明编码端 zerolatency 了,
 * 端到端还是稳定多出 3~4 帧延迟", 换算到 30fps 就是 100ms 以上 —— 比整条网络链路
 * 的延迟还大。`FF_THREAD_SLICE`(片级)只在一帧内部并行, 不引入跨帧排队。
 *
 * 这和编码端关 B 帧是**同一件事的两端**: 任何"要等后面的数据才能出结果"的优化,
 * 在实时场景里都是负收益。
 *
 * @note 本类**不是线程安全**的, 由解码线程独占。
 * @note 解码器对坏数据非常宽容, 别指望用返回值来判断码流质量, 见 DecoderStats。
 */
class Decoder {
public:
    Decoder() = default;

    /**
     * @brief 析构时释放 AVCodecContext/AVFrame/AVPacket/SwsContext
     *
     * @note 这些是 C 风格的裸资源, 忘了释放就是每次 open 泄漏一份。
     */
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /**
     * @brief 打开解码器
     *
     * @param cfg 解码参数
     *
     * @return Ok         成功
     *  InvalidArg threads < 1
     *  Internal   找不到 h264 解码器, 或 libavcodec 调用失败
     *
     * @note 可重复调用, 内部先 close() 掉上一次的资源。
     * @note **不需要传分辨率**: H.264 码流自带 SPS, 解码器从码流里读。
     *          这也意味着分辨率可能**中途变化**(对端重开编码器), 输出的 RawFrame
     *          尺寸要以每一帧自己带的为准, 渲染器据此重建纹理。
     */
    Status open(const DecoderConfig& cfg);

    /**
     * @brief 解码一帧码流
     *
     * @param in  待解码的码流视图
     * @param out 出参, **追加**解出的图像帧; 可能追加 0 个或多个
     *
     * @return Ok         成功(哪怕这次一帧都没吐出来)
     *  Closed     没有 open()
     *  InvalidArg in.data 为空或 in.len 为 0
     *  NetError   解码器明确拒绝了这段码流
     *  Internal   libavcodec 内部错误(如 ENOMEM)
     *
     * @note 码流被拒返回 **NetError 而不是 Internal**: 这段字节是从网上来的,
     *          调用方的反应是"记一笔继续收下一帧", 不是"停机报警"。同一个判据
     *          在 FrameAssembler::offer 里已经用过一次 —— 责任方决定错误码。
     * @note 一次 send 可能吐 0 帧(解码器还在等后续数据)也可能吐多帧, 所以 out 是数组。
     *          zerolatency + LOW_DELAY 下事实上是一进一出, 但别把这个假设写进调用方。
     */
    Status decode(const CodedFrameView& in, std::vector<RawFrame>& out);

    /**
     * @brief 冲刷解码器内部缓存
     *
     * @param out 出参, 追加剩余的图像帧
     *
     * @return Ok 成功; Closed 没有 open()
     *
     * @note flush 之后解码器进入排空状态, 不能再 decode(), 只能 close()。
     */
    Status flush(std::vector<RawFrame>& out);

    /** @brief 释放资源; 幂等 */
    void close();

    const DecoderStats& stats() const { return stats_; }

    /**
     * @brief 把 FFmpeg 自己的日志接进本项目的 Logger
     *
     * @note 进程内调一次即可(main 或管线 open 里), 与具体实例无关, 所以是 static。
     * @note **必须接**: 一旦开始丢包, 解码器会疯狂往 stderr 刷
     *          `error while decoding MB xx` 之类的东西, 把自己的日志全淹掉。
     *          M4 一开丢包注入, 不接管的终端根本没法看。
     * @note 接进来之后这些消息要降到 DEBUG/TRACE: 它们在丢包场景下是**正常现象**,
     *          按 ERROR 打会让人以为程序坏了。
     */
    static void installFfmpegLogBridge();

private:
    /** @brief 循环 receive_frame 直到 EAGAIN —— 一次 send 可能对应多帧 */
    Status drainFrames(std::vector<RawFrame>& out);

    /**
     * @brief 把解码器吐出的 AVFrame 拷成紧凑排布的 RawFrame
     *
     * @note 必须**当场拷走**: receive_frame 返回的 AVFrame 是解码器内部复用的,
     *          下一次调用它的内容就变了。存指针是这一层最容易犯的错。
     * @note 按 **linesize 逐行拷**, 不能整块 memcpy —— 解码器输出的 linesize[0]
     *          通常大于 width(对齐到 32/64 字节)。整块拷得到的是斜着撕裂的画面,
     *          这是 M1 踩坑 3 在解码端的镜像版, 编码端踩过一次这里还会再踩一次。
     * @note pix_fmt 不是 YUV420P 时用 sws_scale 转一次。本项目的编码端固定产出 420P,
     *          这段代码平时不执行 —— 它的价值是让"别人的流"进来时是花屏还是正常有区别。
     *          SwsContext 建一次存成员复用, 别每帧建(M1 踩坑 14)。
     */
    Status copyToRawFrame(const AVFrame* src, RawFrame& dst);

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;

    /**
     * @brief 复用的输入缓冲, 尾部留 AV_INPUT_BUFFER_PADDING_SIZE 字节且**清零**
     *
     * @note 这不是优化, 是**正确性要求**: H.264 的比特流读取器会越界预读,
     *          缓冲区尾部不留填充就是实打实的堆溢出。valgrind/ASan 下必报,
     *          平时可能"看着没事", 压力大时随机崩 —— 最难查的那一类。
     * @note 顺带也确实省掉了每帧一次堆分配, 而这条路径每秒要走几十次。
     */
    std::vector<uint8_t> input_;

    /**
     * @brief 等解码器吐出来的帧的元信息, 按 pts 索引
     *
     * @note 与 Encoder::pending_ 同一个理由: 解码器可能缓存和重排, 拿到一帧时
     *          "当前帧"未必就是刚喂进去的那一帧, 所以时间戳和帧号要靠 pts 查回来。
     *          LOW_DELAY 下这张表里通常只有一项。
     */
    struct Pending {
        uint64_t captureMs = 0;
        uint64_t frameId = 0;
    };
    std::map<int64_t, Pending> pending_;

    /**
     * @brief 下一个要用的 pts
     *
     * @note 不直接拿 CodedFrameView::frameId 当 pts: 帧号是 32 位循环计数器,
     *          回绕之后 pts 会**回退**, map 里就会撞上早先那一项, 元信息张冠李戴。
     *          这里自己数一个单调值, 帧号通过 pending_ 关联回去(同 Encoder::nextPts_)。
     */
    int64_t nextPts_ = 0;

    DecoderConfig cfg_;
    DecoderStats stats_;
    bool flushed_ = false;
};
