/**
 * @file    NullRenderer.cpp
 * @brief   NullRenderer.h 的实现
 * @author  zzj
 * @date    2026-08-22
 */

#include "modules/render/NullRenderer.h"

#include <limits>

NullRenderer::~NullRenderer() {
    close();
}

Status NullRenderer::open(const RendererConfig& cfg) {
    if (cfg.width <= 0 || cfg.height <= 0) {
        return Status::error(Code::InvalidArg, "NullRenderer: width and height must be positive");
    }

    config_ = cfg;
    stats_ = {};
    hasPrev_ = false;
    prevWidth_ = 0;
    prevHeight_ = 0;
    prevCaptureMs_ = 0;
    prevFrameId_ = 0;
    opened_ = true;
    return Status::ok();
}

Status NullRenderer::renderFrame(const RawFrame& frame) {
    if (!opened_) {
        return Status::error(Code::Closed, "NullRenderer: render before open");
    }

    Status st = checkFrame(frame);
    if (!st.isOk()) {
        ++stats_.framesRejected;
        return st;
    }

    st = checkAgainstPrevious(frame);
    if (!st.isOk()) {
        ++stats_.framesRejected;
        return st;
    }

    if (!hasPrev_ || frame.width != prevWidth_ || frame.height != prevHeight_) {
        ++stats_.texturesRebuilt;
    }

    hasPrev_ = true;
    prevWidth_ = frame.width;
    prevHeight_ = frame.height;
    // 两个哨兵字段的更新都要挡住 0, 否则基准会被哨兵帧冲掉:
    // 一个 frameId == 0 的帧过去之后, prevFrameId_ 变成 0, 紧跟它的那一帧
    // 就会因为 `prevFrameId_ != 0` 不成立而**整条递增检查被跳过** ——
    // 哨兵帧本身要放行, 但不能顺手把下一帧的检查也一起放掉。
    if (frame.captureMs != 0) {
        prevCaptureMs_ = frame.captureMs;
    }
    if (frame.frameId != 0) {
        prevFrameId_ = frame.frameId;
    }
    ++stats_.framesRendered;
    return Status::ok();
}

bool NullRenderer::pumpEvents() {
    // 恒 false: 它**真的**没有事件源, 这不是空实现。
    // 没有窗口就没有 SDL_QUIT, 退出只能由 Ctrl-C 或发送端静默超时来触发。
    return false;
}

const RendererStats& NullRenderer::stats() const {
    return stats_;
}

void NullRenderer::close() {
    opened_ = false;
    hasPrev_ = false;
    prevWidth_ = 0;
    prevHeight_ = 0;
    prevCaptureMs_ = 0;
    prevFrameId_ = 0;
}

Status NullRenderer::checkFrame(const RawFrame& frame) const {
    if (frame.width <= 0 || frame.height <= 0) {
        return Status::error(Code::InvalidArg, "NullRenderer: frame dimensions must be positive");
    }
    if (frame.width % 2 != 0 || frame.height % 2 != 0) {
        return Status::error(Code::InvalidArg, "NullRenderer: YUV420P frame dimensions must be even");
    }
    if (frame.fmt != PixelFormat::YUV420P) {
        return Status::error(Code::InvalidArg, "NullRenderer: only YUV420P frames are supported");
    }

    const size_t width = static_cast<size_t>(frame.width);
    const size_t height = static_cast<size_t>(frame.height);
    if (width > std::numeric_limits<size_t>::max() / height) {
        return Status::error(Code::InvalidArg, "NullRenderer: frame dimensions overflow");
    }
    const size_t yBytes = width * height;
    if (yBytes > std::numeric_limits<size_t>::max() - yBytes / 2) {
        return Status::error(Code::InvalidArg, "NullRenderer: frame size overflow");
    }
    const size_t expectedBytes = yBytes + yBytes / 2;
    if (frame.data.size() != expectedBytes) {
        return Status::error(Code::InvalidArg, "NullRenderer: frame data size does not match geometry");
    }

    return Status::ok();
}

Status NullRenderer::checkAgainstPrevious(const RawFrame& frame) const {
    if (!hasPrev_) return Status::ok();

    if (frame.captureMs != 0 && prevCaptureMs_ != 0 && frame.captureMs <= prevCaptureMs_) {
        return Status::error(Code::InvalidArg, "NullRenderer: capture time must increase");
    }
    // frameId 也豁免 0: 它和 captureMs 是**同一个哨兵** —— Decoder 查不到元信息时
    // 两个字段一起留在默认值。不豁免的话 0 <= 任何已见过的 id 恒真, 元信息一丢失
    // 就被记成坏帧, 而错误信息指向渲染器。
    // 代价: 真实的第 0 帧 id 就是 0, 于是紧跟它的那一帧会少查一次递增。可以忍。
    if (frame.frameId != 0 && prevFrameId_ != 0 && frame.frameId <= prevFrameId_) {
        return Status::error(Code::InvalidArg, "NullRenderer: frame ID must increase");
    }

    return Status::ok();
}
