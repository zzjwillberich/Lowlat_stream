/**
 * @file    Encoder.cpp
 * @brief   Encoder.h 的实现
 * @author  zzj
 * @date    2026-07-28
 */

#include "modules/encode/Encoder.h"

#include <cstring>
#include <iterator>
#include <string>

#include "common/Logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {
    /**
     * @brief 把 libavcodec 的错误码转成可读字符串
     *
     * @param rc 负的 AVERROR
     *
     * @return "-22 (Invalid argument)" 这样的一行
     *
     * @note 光打数字没法查 —— AVERROR 是负的 errno 或四字符码, 肉眼认不出来。
     */
    std::string avErr(int rc) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(rc, buf, sizeof(buf));
        return std::to_string(rc) + " (" + buf + ")";
    }

    /**
     * @brief 设一个 x264 私有选项, 失败即报错
     *
     * @param priv AVCodecContext::priv_data
     * @param key  选项名
     * @param val  选项值
     *
     * @return Ok 或 Internal
     *
     * @note 必须查返回值: 选项名写错时 av_opt_set 只是返回
     *          AVERROR_OPTION_NOT_FOUND, 不会有任何提示。于是"zerolatency 没生效"
     *          就变成一个只能靠测延迟才发现的问题 —— 而那时你已经在怀疑网络了。
     */
    Status setOpt(void* priv, const char* key, const char* val) {
        const int rc = av_opt_set(priv, key, val, 0);
        if (rc < 0) {
            return Status::error(Code::Internal, std::string("av_opt_set ") + key + "=" + val +
                                                     " failed: " + avErr(rc));
        }
        return Status::ok();
    }
}  // namespace

Encoder::~Encoder() {
    close();
}

Status Encoder::open(const EncoderConfig& cfg) {
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.width % 2 != 0 || cfg.height % 2 != 0) {
        return Status::error(Code::InvalidArg, "Encoder: width/height must be positive and even");
    }
    if (cfg.fps <= 0 || cfg.bitrateKbps <= 0 || cfg.gop <= 0) {
        return Status::error(Code::InvalidArg, "Encoder: fps/bitrate/gop must be positive");
    }

    close();  // 允许重复 open, 先把上一次的资源还回去

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        return Status::error(Code::Internal, "libx264 encoder not available in this libavcodec build");
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        return Status::error(Code::Internal, "avcodec_alloc_context3 failed");
    }

    ctx_->width     = cfg.width;
    ctx_->height    = cfg.height;
    ctx_->pix_fmt   = AV_PIX_FMT_YUV420P;
    ctx_->gop_size  = cfg.gop;
    ctx_->bit_rate  = static_cast<int64_t>(cfg.bitrateKbps) * 1000;

    // time_base 是 pts 的单位: 1/fps 表示 pts 直接以帧为单位数
    ctx_->time_base = AVRational{1, cfg.fps};
    ctx_->framerate = AVRational{cfg.fps, 1};

    // B 帧要等它后面的帧编完才能编, 天然引入至少一帧的延迟。
    // tune=zerolatency 也会关掉它, 这里显式再写一次 —— 这是本项目的硬约束, 不该藏在 preset 里
    ctx_->max_b_frames = 0;

    Status st = setOpt(ctx_->priv_data, "preset", "ultrafast");
    if (!st.isOk()) { close(); return st; }
    st = setOpt(ctx_->priv_data, "tune", "zerolatency");
    if (!st.isOk()) { close(); return st; }

    int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        close();
        return Status::error(Code::Internal, "avcodec_open2 failed: " + avErr(rc));
    }

    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
    if (!frame_ || !pkt_) {
        close();
        return Status::error(Code::Internal, "av_frame_alloc/av_packet_alloc failed");
    }

    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width  = cfg.width;
    frame_->height = cfg.height;
    // 第二个参数传 0 = 让 libavutil 自己选对齐; 这也是 linesize 会大于 width 的原因
    rc = av_frame_get_buffer(frame_, 0);
    if (rc < 0) {
        close();
        return Status::error(Code::Internal, "av_frame_get_buffer failed: " + avErr(rc));
    }

    cfg_     = cfg;
    nextPts_ = 0;
    pending_.clear();

    LOG_INFO("encode", "x264 opened %dx%d@%dfps %dkbps gop=%d (ultrafast/zerolatency, no B-frames)",
             cfg.width, cfg.height, cfg.fps, cfg.bitrateKbps, cfg.gop);
    return Status::ok();
}

Status Encoder::encode(const RawFrame& in, std::vector<EncodedFrame>& out) {
    if (!ctx_) {
        return Status::error(Code::Internal, "Encoder: encode before open");
    }
    if (in.width != cfg_.width || in.height != cfg_.height) {
        return Status::error(Code::InvalidArg, "Encoder: frame geometry differs from open()");
    }
    if (in.fmt != PixelFormat::YUV420P) {
        return Status::error(Code::InvalidArg, "Encoder: only YUV420P is supported");
    }

    // 编码器可能仍持有上一帧缓冲的引用(参考帧), 直接写会踩到它。
    // 这个调用在需要时会复制出一份独占的缓冲, 不需要时是零成本
    int rc = av_frame_make_writable(frame_);
    if (rc < 0) {
        return Status::error(Code::Internal, "av_frame_make_writable failed: " + avErr(rc));
    }

    Status st = fillAvFrame(in);
    if (!st.isOk()) return st;

    frame_->pts       = nextPts_;
    pending_[nextPts_] = Pending{in.captureMs, in.frameId};
    ++nextPts_;

    rc = avcodec_send_frame(ctx_, frame_);
    if (rc < 0) {
        return Status::error(Code::Internal, "avcodec_send_frame failed: " + avErr(rc));
    }
    return drainPackets(out);
}

Status Encoder::flush(std::vector<EncodedFrame>& out) {
    if (!ctx_) {
        return Status::error(Code::Internal, "Encoder: flush before open");
    }

    // 送一个空帧 = 告诉编码器"没有更多输入了", 之后才能把攒着的帧全部取出来
    const int rc = avcodec_send_frame(ctx_, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) {
        return Status::error(Code::Internal, "avcodec_send_frame(nullptr) failed: " + avErr(rc));
    }
    return drainPackets(out);
}

void Encoder::close() {
    if (pkt_)   av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (ctx_)   avcodec_free_context(&ctx_);
    pending_.clear();
    nextPts_ = 0;
}

Status Encoder::fillAvFrame(const RawFrame& in) {
    const uint8_t* src[3]    = {in.y(), in.u(), in.v()};
    const int      srcLine[3] = {in.yStride(), in.uvStride(), in.uvStride()};
    const int      planeW[3]  = {in.width, in.width / 2, in.width / 2};
    const int      planeH[3]  = {in.height, in.height / 2, in.height / 2};

    for (int p = 0; p < 3; ++p) {
        if (!frame_->data[p]) {
            return Status::error(Code::Internal, "AVFrame plane not allocated");
        }
        for (int r = 0; r < planeH[p]; ++r) {
            // **逐行拷, 不能整块 memcpy**: AVFrame 的 linesize 为了 SIMD 对齐通常
            // 大于 width(比如 640 宽的 Y 平面 linesize 可能是 640 也可能是 704),
            // 整块拷会让每一行都往前串位, 画面斜着裂开
            std::memcpy(frame_->data[p] + static_cast<size_t>(r) * frame_->linesize[p],
                        src[p] + static_cast<size_t>(r) * srcLine[p],
                        static_cast<size_t>(planeW[p]));
        }
    }
    return Status::ok();
}

Status Encoder::drainPackets(std::vector<EncodedFrame>& out) {
    for (;;) {
        const int rc = avcodec_receive_packet(ctx_, pkt_);

        // EAGAIN = 攒着还没到吐的时候; EOF = flush 之后已经取完。两个都是正常结束
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return Status::ok();
        }
        if (rc < 0) {
            return Status::error(Code::Internal, "avcodec_receive_packet failed: " + avErr(rc));
        }

        EncodedFrame ef;
        ef.data.assign(pkt_->data, pkt_->data + pkt_->size);
        ef.isKey = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;

        // 按 pts 找回这一包对应的采集时刻, 而不是用"最近一次 encode 的入参"
        auto it = pending_.find(pkt_->pts);
        if (it != pending_.end()) {
            ef.captureMs = it->second.captureMs;
            ef.frameId   = it->second.frameId;
            // 连同更早的一起删: 编码器若丢弃了某帧, 它的记录不会有人来取, 留着就是慢性泄漏
            pending_.erase(pending_.begin(), std::next(it));
        }

        out.push_back(std::move(ef));
        av_packet_unref(pkt_);  // 必须还回去, 否则下一次 receive_packet 会失败
    }
}
