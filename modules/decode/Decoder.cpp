/**
 * @file    Decoder.cpp
 * @brief   Decoder.h 的实现
 * @author  zzj
 * @date    2026-08-18
 */

#include "modules/decode/Decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstring>

#include "common/Logger.h"

Decoder::~Decoder() {
    // TODO(M3.2): close()
}

Status Decoder::open(const DecoderConfig& cfg) {
    // TODO(M3.2): 先 close(), 让重复 open 不泄漏(同 Encoder::open)。
    // TODO(M3.2): 校验 cfg.threads >= 1 -> InvalidArg。
    // TODO(M3.2): avcodec_find_decoder(AV_CODEC_ID_H264) -> 找不到报 Internal;
    //             avcodec_alloc_context3 / av_frame_alloc / av_packet_alloc 逐个查空。
    // TODO(M3.2): 低延迟三件事(顺序无所谓, 但必须在 avcodec_open2 **之前**设):
    //               ctx_->thread_count = cfg.threads;
    //               ctx_->thread_type  = FF_THREAD_SLICE;   // 不是 FF_THREAD_FRAME
    //               ctx_->flags       |= AV_CODEC_FLAG_LOW_DELAY;
    //             设在 open2 之后不报错也不生效 —— 这类"静默失效"最坑, 因为你会以为
    //             自己关过帧级多线程了, 而延迟依然多出三四帧。
    // TODO(M3.2): 任何一步失败都要回滚已经分配的资源, 别留半开状态。
    (void)cfg;
    return Status::error(Code::Internal, "Decoder::open: not implemented");
}

Status Decoder::decode(const CodedFrameView& in, std::vector<RawFrame>& out) {
    // TODO(M3.2): 没 open -> Closed; in.data 为空或 in.len == 0 -> InvalidArg;
    //             已经 flush 过 -> Closed(排空状态不能再喂, 同 Encoder)。
    // TODO(M3.2): 拷进 input_ 并补填充:
    //               input_.resize(in.len + AV_INPUT_BUFFER_PADDING_SIZE);
    //               memcpy(input_.data(), in.data, in.len);
    //               memset(input_.data() + in.len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    //             **填充必须清零**, 只 resize 不清是复用缓冲时的经典残留 bug:
    //             上一帧的尾巴会被当成这一帧的比特流读进去。
    // TODO(M3.2): pkt_->data = input_.data(); pkt_->size = (int)in.len;
    //             pkt_->pts = nextPts_; 元信息存进 pending_[nextPts_], 然后 ++nextPts_。
    // TODO(M3.2): avcodec_send_packet 失败 -> ++decodeErrors, 返回 NetError
    //             (码流是网上来的, 责任方不是本端)。
    // TODO(M3.2): ++stats_.framesIn, 然后 drainFrames(out)。
    (void)in;
    (void)out;
    return Status::error(Code::Internal, "Decoder::decode: not implemented");
}

Status Decoder::flush(std::vector<RawFrame>& out) {
    // TODO(M3.2): 没 open -> Closed。
    // TODO(M3.2): avcodec_send_packet(ctx_, nullptr) 通知排空, 然后 drainFrames(out)。
    // TODO(M3.2): flushed_ = true。
    (void)out;
    return Status::error(Code::Internal, "Decoder::flush: not implemented");
}

void Decoder::close() {
    // TODO(M3.2): avcodec_free_context / av_frame_free / av_packet_free / sws_freeContext,
    //             每个都置回 nullptr —— 幂等的关键就在这里, 不置空的话第二次调用是
    //             double free。
    // TODO(M3.2): pending_ 清空, nextPts_ 归零, flushed_ 归 false。
    //             **stats_ 不清**: 调用方是在 close 之后读统计的。
}

void Decoder::installFfmpegLogBridge() {
    // TODO(M3.2): av_log_set_callback(...) 装一个自由函数, 把 FFmpeg 的等级映射到
    //             本项目的 Logger:
    //               AV_LOG_PANIC/FATAL/ERROR -> WARN     (见下面为什么不是 ERROR)
    //               AV_LOG_WARNING           -> DEBUG
    //               其余                      -> TRACE
    // TODO(M3.2): 丢包时 "error while decoding MB" 是**正常现象**不是故障,
    //             按 ERROR 打会让人以为程序坏了, 而且刷屏。降一级是有意的。
    // TODO(M3.2): 回调里要用 av_log_format_line2 把 va_list 格式化成字符串;
    //             回调可能被**任意线程**调用, 所以只能碰 Logger(它自己是线程安全的),
    //             不要在里面访问任何 Decoder 实例状态 —— 这也是它 static 的原因。
}

Status Decoder::drainFrames(std::vector<RawFrame>& out) {
    // TODO(M3.2): 循环 avcodec_receive_frame:
    //               AVERROR(EAGAIN) 或 AVERROR_EOF -> 正常结束, 返回 Ok;
    //               其他负值 -> Internal;
    //               成功 -> copyToRawFrame 追加进 out, ++stats_.framesOut。
    //             **必须是 while 不是 if**: 一次 send 可能对应多帧, 写成 if 会漏帧
    //             (M1 踩坑 5 的镜像)。
    // TODO(M3.2): 用 frame_->pts 去 pending_ 里查元信息(查不到就退回
    //             best_effort_timestamp); 取完那一项要 erase, 否则这张表只涨不消。
    // TODO(M3.2): 每次 receive 之后记得 av_frame_unref(frame_), 否则引用一直挂着。
    (void)out;
    return Status::error(Code::Internal, "Decoder::drainFrames: not implemented");
}

Status Decoder::copyToRawFrame(const AVFrame* src, RawFrame& dst) {
    // TODO(M3.2): dst.width/height 取 src->width/height —— **不是 ctx_ 里的**,
    //             中途换分辨率时以这一帧自己带的为准。
    // TODO(M3.2): dst.data.resize(w * h * 3 / 2), fmt = YUV420P。
    // TODO(M3.2): src->format == AV_PIX_FMT_YUV420P 时逐平面**按行**拷:
    //               Y: h 行, 每行 w 字节, 源行距 src->linesize[0]
    //               U: h/2 行, 每行 w/2 字节, 源行距 src->linesize[1]
    //               V: 同 U, 源行距 src->linesize[2]
    //             linesize 通常大于行宽, 整块 memcpy 得到斜着撕裂的画面。
    // TODO(M3.2): 否则用 sws_scale 转成 YUV420P, ++stats_.convertedFrames。
    //             sws_getCachedContext 建一次存 sws_ 复用, 别每帧建。
    // TODO(M3.2): U 和 V 别写反。本项目的 NullSource 里 U 按列渐变、V 按行渐变,
    //             写反了画面颜色会明显不对 —— 单测就是靠这个性质抓它的。
    (void)src;
    (void)dst;
    return Status::error(Code::Internal, "Decoder::copyToRawFrame: not implemented");
}
