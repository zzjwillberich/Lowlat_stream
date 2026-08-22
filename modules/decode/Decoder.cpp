/**
 * @file    Decoder.cpp
 * @brief   Decoder.h 的实现
 * @author  zzj
 * @date    2026-08-18
 */

#include "modules/decode/Decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdarg>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

#include "common/Logger.h"

namespace {
    std::string avErr(int rc) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(rc, buf, sizeof(buf));
        return std::to_string(rc) + " (" + buf + ")";
    }

    void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
        char line[1024] = {0};
        int printPrefix = 1;
        av_log_format_line2(ptr, level, fmt, vl, line, sizeof(line), &printPrefix);

        line[std::strcspn(line, "\r\n")] = '\0';
        const LogLevel logLevel = level <= AV_LOG_ERROR ? LogLevel::WARN
                                  : level <= AV_LOG_WARNING ? LogLevel::DEBUG
                                                             : LogLevel::TRACE;
        Logger::instance().log(logLevel, "ffmpeg", "%s", line);
    }
}  // namespace

Decoder::~Decoder() {
    close();
}

Status Decoder::open(const DecoderConfig& cfg) {
    close();
    if (cfg.threads < 1) {
        return Status::error(Code::InvalidArg, "Decoder: threads must be positive");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        return Status::error(Code::Internal, "H.264 decoder not available in this libavcodec build");
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) {
        return Status::error(Code::Internal, "avcodec_alloc_context3 failed");
    }

    ctx_->thread_count = cfg.threads;
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        close();
        return Status::error(Code::Internal, "avcodec_open2 failed: " + avErr(rc));
    }

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (!frame_ || !pkt_) {
        close();
        return Status::error(Code::Internal, "av_frame_alloc/av_packet_alloc failed");
    }

    cfg_ = cfg;
    stats_ = {};
    flushed_ = false;
    LOG_INFO("decode", "H.264 decoder opened (threads=%d, slice threading, low delay)", cfg.threads);
    return Status::ok();
}

Status Decoder::decode(const CodedFrameView& in, std::vector<RawFrame>& out) {
    if (!ctx_) {
        return Status::error(Code::Closed, "Decoder: decode before open");
    }
    if (!in.data || in.len == 0 || in.len > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        in.len > std::numeric_limits<size_t>::max() - AV_INPUT_BUFFER_PADDING_SIZE) {
        return Status::error(Code::InvalidArg, "Decoder: invalid coded frame");
    }
    if (flushed_) {
        return Status::error(Code::Closed, "Decoder: decode after flush");
    }

    input_.resize(in.len + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(input_.data(), in.data, in.len);
    std::memset(input_.data() + in.len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(pkt_);
    pkt_->data = input_.data();
    pkt_->size = static_cast<int>(in.len);
    pkt_->pts = nextPts_;
    pkt_->dts = nextPts_;

    const int64_t pts = nextPts_;
    pending_[pts] = Pending{in.captureMs, in.frameId};
    ++nextPts_;

    const int rc = avcodec_send_packet(ctx_, pkt_);
    if (rc < 0) {
        pending_.erase(pts);
        ++stats_.decodeErrors;
        return Status::error(Code::NetError, "avcodec_send_packet failed: " + avErr(rc));
    }

    ++stats_.framesIn;
    return drainFrames(out);
}

Status Decoder::flush(std::vector<RawFrame>& out) {
    if (!ctx_) {
        return Status::error(Code::Closed, "Decoder: flush before open");
    }

    const int rc = avcodec_send_packet(ctx_, nullptr);
    if (rc < 0 && rc != AVERROR_EOF) {
        return Status::error(Code::Internal, "avcodec_send_packet(nullptr) failed: " + avErr(rc));
    }

    const Status st = drainFrames(out);
    if (st.isOk()) flushed_ = true;
    return st;
}

void Decoder::close() {
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }

    pending_.clear();
    input_.clear();
    nextPts_ = 0;
    cfg_ = {};
    flushed_ = false;
}

void Decoder::installFfmpegLogBridge() {
    av_log_set_callback(ffmpegLogCallback);
}

Status Decoder::drainFrames(std::vector<RawFrame>& out) {
    for (;;) {
        const int rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            return Status::ok();
        }
        if (rc < 0) {
            return Status::error(Code::Internal, "avcodec_receive_frame failed: " + avErr(rc));
        }

        RawFrame decoded;
        Status st = copyToRawFrame(frame_, decoded);
        if (!st.isOk()) {
            av_frame_unref(frame_);
            return st;
        }

        int64_t pts = frame_->pts;
        auto pending = pending_.find(pts);
        if (pending == pending_.end() && frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
            pts = frame_->best_effort_timestamp;
            pending = pending_.find(pts);
        }
        if (pending != pending_.end()) {
            decoded.captureMs = pending->second.captureMs;
            decoded.frameId = pending->second.frameId;
            pending_.erase(pending_.begin(), std::next(pending));
        } else {
            // 查不到元信息也照样把帧交出去 —— 画面比时间戳重要。
            // 但 captureMs 和 frameId 都会留在默认值 0, 下游拿到的是**两个哨兵**:
            // 延迟统计要跳过它(否则 mean/max 被一个 0 直接毁掉),
            // NullRenderer 的跨帧递增检查也要跳过它(否则会误报成坏帧, 而且
            // 错误信息指向渲染器, 把人往反方向带)。
            // 这个计数器是这条路唯一的信号, 稳态下应当恒为 0。
            ++stats_.framesMissingMeta;
        }

        out.push_back(std::move(decoded));
        ++stats_.framesOut;
        av_frame_unref(frame_);
    }
}

Status Decoder::copyToRawFrame(const AVFrame* src, RawFrame& dst) {
    if (!src || src->width <= 0 || src->height <= 0 || src->width % 2 != 0 ||
        src->height % 2 != 0) {
        return Status::error(Code::Internal, "Decoder: invalid decoded frame geometry");
    }

    const int width = src->width;
    const int height = src->height;
    dst.reset(width, height, PixelFormat::YUV420P);

    if (src->format == AV_PIX_FMT_YUV420P) {
        const int planeW[3] = {width, width / 2, width / 2};
        const int planeH[3] = {height, height / 2, height / 2};
        uint8_t* dstData[3] = {dst.y(), dst.u(), dst.v()};
        const int dstLine[3] = {dst.yStride(), dst.uvStride(), dst.uvStride()};

        for (int p = 0; p < 3; ++p) {
            if (!src->data[p] || src->linesize[p] < planeW[p]) {
                return Status::error(Code::Internal, "Decoder: invalid YUV420P plane");
            }
            for (int row = 0; row < planeH[p]; ++row) {
                std::memcpy(dstData[p] + static_cast<size_t>(row) * dstLine[p],
                            src->data[p] + static_cast<size_t>(row) * src->linesize[p],
                            static_cast<size_t>(planeW[p]));
            }
        }
        return Status::ok();
    }

    sws_ = sws_getCachedContext(sws_, width, height, static_cast<AVPixelFormat>(src->format),
                                width, height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                nullptr, nullptr);
    if (!sws_) {
        return Status::error(Code::Internal, "sws_getCachedContext failed");
    }

    uint8_t* dstData[4] = {dst.y(), dst.u(), dst.v(), nullptr};
    const int dstLine[4] = {dst.yStride(), dst.uvStride(), dst.uvStride(), 0};
    const int scaled = sws_scale(sws_, src->data, src->linesize, 0, height, dstData, dstLine);
    if (scaled != height) {
        return Status::error(Code::Internal, "sws_scale failed");
    }

    ++stats_.convertedFrames;
    return Status::ok();
}
