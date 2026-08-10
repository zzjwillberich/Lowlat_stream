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
#include <limits>
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
    actualCfg_ = cfg;

    st = openAndQueryDevice();
    if(!st.isOk()) {
        close();
        return st;
    }

    st = negotiateFormatAndRate();
    if(!st.isOk()) {
        close();
        return st;
    }

    st = createConverter();
    if(!st.isOk()) {
        close();
        return st;
    }

    st = mapBuffers();
    if(!st.isOk()) {
        close();
        return st;
    }

    st = startStreaming();
    if(!st.isOk()) {
        close();
        return st;
    }

    frameId_ = 0;
    opened_ = true;
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
    if(sws_){
        return Status::error(
            Code::Internal,
            "V4l2Source: converter context already exists before initialization"
        );
    }

    if(actualCfg_.width <= 0 || actualCfg_.height <= 0 ||
       actualCfg_.width % 2 != 0 || actualCfg_.height % 2 != 0) {
        return Status::error(
            Code::Internal,
            "V4l2Source: invalid negotiated geometry for converter: " +
                std::to_string(actualCfg_.width) + "x" +
                std::to_string(actualCfg_.height)
        );
    }

    if(devicePixelFormat_ != V4L2_PIX_FMT_YUYV){
        return Status::error(
            Code::Internal,
            "V4l2Source: cannot create converter for device pixel format " +
                std::to_string(devicePixelFormat_) + "; expected YUYV"
        );
    }

    sws_ = sws_getContext(
        actualCfg_.width, 
        actualCfg_.height, 
        AVPixelFormat::AV_PIX_FMT_YUYV422,
        actualCfg_.width, 
        actualCfg_.height, 
        AVPixelFormat::AV_PIX_FMT_YUV420P, 
        SWS_FAST_BILINEAR,
        nullptr, 
        nullptr, 
        nullptr
    );

    if(sws_ == nullptr){
        return Status::error(
            Code::Internal,
            "V4l2Source: sws_getContext failed for YUYV422 to YUV420P conversion at " +
                std::to_string(actualCfg_.width) + "x" +
                std::to_string(actualCfg_.height)
        );
    }

    return Status::ok();
}

Status V4l2Source::mapBuffers() {
    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(fd_, VIDIOC_REQBUFS, &req) < 0){
        return Status::error(
            Code::IoError,
            "V4l2Source: VIDIOC_REQBUFS failed for device '" +
                requestedCfg_.device + "': " +
                std::string(std::strerror(errno))
        );
    }
    if(req.count == 0){
        return Status::error(
            Code::IoError,
            "V4l2Source: VIDIOC_REQBUFS returned no MMAP buffers for device '" +
                requestedCfg_.device + "'"
        );
    }

    buffers_.reserve(req.count);
    for(uint32_t i = 0; i < req.count; ++i){
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if(xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0){
            return Status::error(
                Code::IoError,
                "V4l2Source: VIDIOC_QUERYBUF failed for buffer " +
                    std::to_string(i) + ": " +
                    std::string(std::strerror(errno))
            );
        }
        if(buf.length == 0){
            return Status::error(
                Code::IoError,
                "V4l2Source: VIDIOC_QUERYBUF returned zero length for buffer " +
                    std::to_string(i)
            );
        }

        void* start = mmap(
            nullptr,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd_,
            buf.m.offset
        );

        if(start == MAP_FAILED){
            return Status::error(
                Code::IoError,
                "V4l2Source: mmap failed for buffer " + std::to_string(i) +
                    " (length=" + std::to_string(buf.length) + "): " +
                    std::string(std::strerror(errno))
            );
        }

        MappedBuffer mbuf{start,buf.length};
        buffers_.push_back(mbuf);
    }
    return Status::ok();
}

Status V4l2Source::startStreaming() {
    if(fd_ < 0){
        return Status::error(
          Code::Internal,
          "V4l2Source: cannot start streaming with an invalid device fd"
        );
    }
    if(buffers_.empty()){
        return Status::error(
            Code::Internal,
            "V4l2Source: cannot start streaming without mapped buffers"
        );
    }

    for(uint32_t i = 0; i < buffers_.size(); ++i){
        v4l2_buffer buf{};
        buf.memory = V4L2_MEMORY_MMAP;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.index = i;
        if(xioctl(fd_, VIDIOC_QBUF, &buf) < 0){
            return Status::error(
                Code::IoError,
                "V4l2Source: VIDIOC_QBUF failed for buffer " +
                    std::to_string(i) + ": " +
                    std::string(std::strerror(errno))
            );
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(xioctl(fd_, VIDIOC_STREAMON, &type) < 0){
        return Status::error(
            Code::IoError,
            "V4l2Source: VIDIOC_STREAMON failed for device '" +
                requestedCfg_.device + "': " +
                std::string(std::strerror(errno))
        );
    }

    streaming_ = true;
    return Status::ok();
}

Status V4l2Source::waitForFrame() const {
    if(fd_ < 0) {
        return Status::error(Code::Internal,
                             "V4l2Source: cannot wait for a frame with an invalid device fd");
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN | POLLPRI;

    for(;;) {
        pfd.revents = 0;
        const int rc = ::poll(&pfd, 1, POLL_TIMEOUT_MS);

        if(rc < 0 && errno == EINTR) {
            continue;
        }
        if(rc < 0) {
            return Status::error(Code::IoError,
                                 "V4l2Source: poll failed for device '" +
                                     requestedCfg_.device + "': " +
                                     std::string(std::strerror(errno)));
        }
        if(rc == 0) {
            return Status::error(Code::Timeout,
                                 "V4l2Source: timed out after " +
                                     std::to_string(POLL_TIMEOUT_MS) +
                                     " ms waiting for a frame from device '" +
                                     requestedCfg_.device + "'");
        }

        if(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return Status::error(Code::IoError,
                                 "V4l2Source: poll reported device error events=" +
                                     std::to_string(static_cast<int>(pfd.revents)) +
                                     " for device '" + requestedCfg_.device + "'");
        }
        if(pfd.revents & (POLLIN | POLLPRI)) {
            return Status::ok();
        }

        return Status::error(Code::IoError,
                             "V4l2Source: poll returned unexpected events=" +
                                 std::to_string(static_cast<int>(pfd.revents)) +
                                 " for device '" + requestedCfg_.device + "'");
    }
}

Status V4l2Source::convertFrame(const void* data, size_t bytesUsed, RawFrame& out) {
    if(data == nullptr) {
        return Status::error(Code::Internal,
                             "V4l2Source: cannot convert a null capture buffer");
    }
    if(sws_ == nullptr) {
        return Status::error(Code::Internal,
                             "V4l2Source: cannot convert a frame without a SwsContext");
    }
    if(actualCfg_.width <= 0 || actualCfg_.height <= 0 ||
       actualCfg_.width % 2 != 0 || actualCfg_.height % 2 != 0) {
        return Status::error(Code::Internal,
                             "V4l2Source: invalid negotiated geometry during conversion: " +
                                 std::to_string(actualCfg_.width) + "x" +
                                 std::to_string(actualCfg_.height));
    }
    if(out.width != actualCfg_.width || out.height != actualCfg_.height ||
       out.fmt != PixelFormat::YUV420P ||
       out.data.size() != frameBytes(actualCfg_.width, actualCfg_.height)) {
        return Status::error(Code::Internal,
                             "V4l2Source: output frame was not initialized for negotiated "
                             "YUV420P geometry " +
                                 std::to_string(actualCfg_.width) + "x" +
                                 std::to_string(actualCfg_.height));
    }

    const size_t rowBytes = static_cast<size_t>(actualCfg_.width) * 2;
    if(bytesperline_ < rowBytes ||
       bytesperline_ > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return Status::error(Code::Internal,
                             "V4l2Source: invalid YUYV input stride " +
                                 std::to_string(bytesperline_) +
                                 " for width " + std::to_string(actualCfg_.width));
    }

    const size_t rowsBeforeLast = static_cast<size_t>(actualCfg_.height - 1);
    if(rowsBeforeLast > 0 &&
       bytesperline_ > (std::numeric_limits<size_t>::max() - rowBytes) /
                           rowsBeforeLast) {
        return Status::error(Code::Internal,
                             "V4l2Source: YUYV input size calculation overflow");
    }
    const size_t requiredBytes = rowsBeforeLast * bytesperline_ + rowBytes;
    if(bytesUsed < requiredBytes) {
        return Status::error(Code::IoError,
                             "V4l2Source: captured YUYV frame is too short: bytesused=" +
                                 std::to_string(bytesUsed) + ", required=" +
                                 std::to_string(requiredBytes));
    }

    const uint8_t* srcData[4] = {
        static_cast<const uint8_t*>(data), nullptr, nullptr, nullptr
    };
    const int srcStride[4] = {
        static_cast<int>(bytesperline_), 0, 0, 0
    };
    uint8_t* dstData[4] = {
        out.y(), out.u(), out.v(), nullptr
    };
    const int dstStride[4] = {
        out.yStride(), out.uvStride(), out.uvStride(), 0
    };

    const int outputRows = sws_scale(sws_,
                                     srcData,
                                     srcStride,
                                     0,
                                     actualCfg_.height,
                                     dstData,
                                     dstStride);
    if(outputRows != actualCfg_.height) {
        return Status::error(Code::Internal,
                             "V4l2Source: sws_scale produced " +
                                 std::to_string(outputRows) + " rows; expected " +
                                 std::to_string(actualCfg_.height));
    }

    return Status::ok();
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
