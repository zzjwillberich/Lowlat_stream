/**
 * @file    V4l2Source.cpp
 * @brief   V4l2Source.h 的实现骨架
 * @author  zzj
 * @date    2026-08-07
 */

#include "modules/capture/V4l2Source.h"
#include "common/Clock.h"
#include "common/Status.h"
#include "modules/capture/Frame.h"
#include "common/Logger.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

V4l2Source::~V4l2Source() {
    close();
}

Status V4l2Source::open(const SourceConfig& cfg) {
    V4l2Source::close();
    Status st = validateConfig(cfg);
    if(!st.isOk()) return st;

    requestedCfg_ = cfg;
    actualCfg_ = actualConfig();
    return Status::ok();
}

Status V4l2Source::readFrame(RawFrame& out) {
    if(!opened_) return Status::error(Code::Closed ,"V4l2Source: not opened");
    if(fd_ < 0 || !streaming_ || !sws_) return Status::error(Code::Internal, "V4l2Source: inconsistent opened state");

    Status st;
    v4l2_buffer buf{};
    
    for(;;){
        st = waitForFrame();
        if(!st.isOk()) return st;

        buf = {};
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if(xioctl(fd_, VIDIOC_DQBUF, &buf) == 0) break;

        if(errno == EAGAIN) continue;

        return Status::error(Code::IoError,"V4l2Source: VIDIOC_DQBUF failed: " +
                std::string(std::strerror(errno)));
    }

    if(buf.index >= buffers_.size()) {
        st = Status::error(
            Code::IoError,
            "V4l2Source: VIDIOC_DQBUF returned buffer index " +
                std::to_string(buf.index) + " but only " +
                std::to_string(buffers_.size()) + " buffers are mapped");
    }else if(buf.bytesused > buffers_[buf.index].length) {
        st = Status::error(
            Code::IoError,
            "V4l2Source: VIDIOC_DQBUF reported bytesused=" +
        std::to_string(buf.bytesused) + " for mapped buffer length=" +
        std::to_string(buffers_[buf.index].length));
    }else{   
        void* data =  buffers_[buf.index].start;
        out.reset(actualCfg_.width, actualCfg_.height);
        out.frameId = frameId_;
        out.captureMs = steadyNowMs();
        
        st = convertFrame(data, buf.bytesused, out);
    }

    if(xioctl(fd_, VIDIOC_QBUF, &buf) < 0){
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_QBUF failed: " +
                                 std::string(std::strerror(errno)));
    }

    if(!st.isOk()) return st;

    ++frameId_;
    return Status::ok();
}

void V4l2Source::close() {
    opened_ = false;

    stopStreaming();
    unmapBuffers();
    if(sws_){
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if(fd_ >= 0){
        if(::close(fd_) < 0){
            const int saveErr = errno;

            LOG_WARN(
                "captrue",
                "close V4l2 device %s failed: %s",
                requestedCfg_.device.c_str(),
                std::strerror(saveErr)
            );
        }

        fd_ = -1;
    }
    devicePixelFormat_ = 0;
    frameId_ = 0;
    bytesperline_ = 0;
}

Status V4l2Source::validateConfig(const SourceConfig& cfg) const {
    if(cfg.device.empty()) return Status::error(Code::InvalidArg, "V4l2Source: device path must not be empty");

    if(cfg.height <= 0 || cfg.width <= 0) return Status::error(Code::InvalidArg, "V4l2Source: width/height must be positive");

    if(cfg.height % 2 == 1 || cfg.width % 2 == 1) return Status::error(Code::InvalidArg, "V4l2Source: YUV420P requires even width/height");

    if(cfg.fps <= 0) return Status::error(Code::InvalidArg, "V4l2Source: fps must be positive");
    
    return Status::ok();
}

Status V4l2Source::openAndQueryDevice() {
    if(requestedCfg_.device.empty()) return Status::error(Code::InvalidArg, "V4l2Source: device path must not be empty");

    fd_ = ::open(requestedCfg_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd_ < 0){
        return Status::error(Code::IoError,
                             "V4l2Source: failed to open device '" + requestedCfg_.device +
                                 "': " + std::string(std::strerror(errno)));
    }

    v4l2_capability cap{};
    if(xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_QUERYCAP failed for device '" +
                                 requestedCfg_.device + "': " +
                                 std::string(std::strerror(errno)));
    }

    if(cap.capabilities & V4L2_CAP_DEVICE_CAPS){
        if(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE && cap.device_caps & V4L2_CAP_STREAMING) return Status::ok();
    }else{
        if(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE && cap.capabilities & V4L2_CAP_STREAMING) return Status::ok();
    }
    return Status::error(Code::IoError,
                         "V4l2Source: device '" + requestedCfg_.device +
                             "' does not provide the required VIDEO_CAPTURE and STREAMING capabilities");
}

Status V4l2Source::negotiateFormatAndRate() {
    //写预设的宽高和像素格式
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = requestedCfg_.width;
    fmt.fmt.pix.height = requestedCfg_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if(xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0){
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_FMT failed for device '" +
                                 requestedCfg_.device + "': " +
                                 std::string(std::strerror(errno)));
    }

    auto pixel = fmt.fmt.pix;
    //检查返回的宽高和像素格式是否合法，是否与请求一致
    if(pixel.height <= 0 || pixel.width <= 0) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_FMT returned invalid geometry " +
                                 std::to_string(pixel.width) + "x" +
                                 std::to_string(pixel.height));
    }
    if(pixel.height % 2 == 1 || pixel.width % 2 == 1) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_FMT returned odd geometry " +
                                 std::to_string(pixel.width) + "x" +
                                 std::to_string(pixel.height) +
                                 ", which cannot be converted to YUV420P");
    }
    if(pixel.pixelformat != V4L2_PIX_FMT_YUYV) {
        return Status::error(Code::IoError,
                                 "V4l2Source: VIDIOC_S_FMT returned unsupported pixel format " +
                                 std::to_string(pixel.pixelformat) +
                                 "; expected YUYV");
    }

    if(static_cast<uint32_t>(requestedCfg_.width) != pixel.width || static_cast<uint32_t>(requestedCfg_.height) != pixel.height){
        LOG_WARN(
            "capture",
            "V4L2 device %s adjusted resolution from %dx%d to %ux%u",
            requestedCfg_.device.c_str(),
            requestedCfg_.width,
            requestedCfg_.height,
            pixel.width,
            pixel.height
        );
    }
    actualCfg_.width = static_cast<int>(pixel.width);
    actualCfg_.height = static_cast<int>(pixel.height);
    devicePixelFormat_ = pixel.pixelformat;

    const size_t minimumBytesPerLine = static_cast<size_t>(pixel.width) * 2;
    bytesperline_ = pixel.bytesperline < minimumBytesPerLine
                        ? minimumBytesPerLine
                        : pixel.bytesperline;

    //设置FPS
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = requestedCfg_.fps;
    if(xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_PARM failed for device '" +
                                 requestedCfg_.device + "': " +
                                 std::string(std::strerror(errno)));
    }

    //检查返回的实际 FPS 是否合法，以及是否与请求一致
    auto timer = parm.parm.capture.timeperframe;
    if(timer.numerator <= 0 || timer.denominator <= 0) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_PARM returned invalid timeperframe " +
                                 std::to_string(timer.numerator) + "/" +
                                 std::to_string(timer.denominator));
    }
    
    actualCfg_.fps = static_cast<int>(std::lround(
        static_cast<double>(timer.denominator) /
        static_cast<double>(timer.numerator)));

    if(actualCfg_.fps <= 0) {
        return Status::error(Code::IoError,
                             "V4l2Source: VIDIOC_S_PARM produced a non-positive frame rate");
    }

    if(actualCfg_.fps != requestedCfg_.fps) {
        LOG_WARN(
            "capture",
            "V4L2 device %s adjusted frame rate from %d fps to %d fps",
            requestedCfg_.device.c_str(),
            requestedCfg_.fps,
            actualCfg_.fps
        );
    }
    
    return Status::ok();
}

Status V4l2Source::createConverter() {
    // TODO(M1.5): 只创建一次 SwsContext，输入 YUYV422，输出 YUV420P。
    return Status::error(Code::Internal,
                         "V4l2Source::createConverter is not implemented (M1.5)");
}

Status V4l2Source::mapBuffers() {
    // TODO(M1.5): REQBUFS(建议 4) -> QUERYBUF -> mmap；部分失败也必须能由 close 回收。
    return Status::error(Code::Internal,
                         "V4l2Source::mapBuffers is not implemented (M1.5)");
}

Status V4l2Source::startStreaming() {
    // TODO(M1.5): 先 QBUF 所有 buffer，再 VIDIOC_STREAMON；成功后设置 streaming_。
    return Status::error(Code::Internal,
                         "V4l2Source::startStreaming is not implemented (M1.5)");
}

Status V4l2Source::waitForFrame() const {
    // TODO(M1.5): poll(POLLIN|POLLPRI)，EINTR 重试，POLLERR/HUP/NVAL 返回 IoError。
    return Status::error(Code::Internal,
                         "V4l2Source::waitForFrame is not implemented (M1.5)");
}

Status V4l2Source::convertFrame(const void* data, size_t bytesUsed, RawFrame& out) {
    (void)data;
    (void)bytesUsed;
    (void)out;

    // TODO(M1.5): 校验 bytesUsed >= width*height*2，并按 out 的三个平面/stride 调 sws_scale。
    return Status::error(Code::Internal,
                         "V4l2Source::convertFrame is not implemented (M1.5)");
}

int V4l2Source::xioctl(int fd, unsigned long request, void* arg) {
    int n;
    do{
        n = ioctl(fd, request, arg);
    }while(n == -1 && errno == EINTR);
    
    return n;
}

void V4l2Source::stopStreaming() {
    if(!streaming_) return;

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(xioctl(fd_, VIDIOC_STREAMOFF, &t) == -1){
        const int saveErr = errno;

        LOG_WARN("captrue",
            "VIDIOC_STREAMOFF failed for %s: %s",
            requestedCfg_.device.c_str(),
            std::strerror(saveErr));
    }
    streaming_ = false;
}

void V4l2Source::unmapBuffers() {
    auto it = buffers_.begin(); 
    while(it != buffers_.end()){
        if(it->start == nullptr
        || it->start == MAP_FAILED
        || it->length == 0){
            it = buffers_.erase(it);
            continue;
        }
        
        if(munmap(it->start, it->length) == 0){
            it = buffers_.erase(it);
            continue;
        }

        LOG_WARN(
            "captrue", 
            "munmap V4l2 failed for %s: %s",
            requestedCfg_.device.c_str(),
            std::strerror(errno));

            ++it;
    }
}
