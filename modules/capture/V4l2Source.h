/**
 * @file    V4l2Source.h
 * @brief   Linux V4L2 摄像头采集源
 * @author  zzj
 * @date    2026-08-07
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modules/capture/ISource.h"

struct SwsContext;

/**
 * V4L2 mmap 流式采集源。
 *
 * 打开流程：open device → QUERYCAP → S_FMT/S_PARM → REQBUFS/mmap →
 * QBUF all → STREAMON。readFrame() 用 poll 等待、DQBUF 取帧、libswscale
 * 将 YUYV422 转为 YUV420P，最后必须把同一缓冲 QBUF 归还驱动。
 *
 * @note 本类不是线程安全的，一个实例只能由一个采集线程使用。
 * @note close() 必须幂等；析构函数会调用 close() 回收 STREAMON、mmap、fd 和 SwsContext。
 * @note M1.5 只支持驱动最终协商为 V4L2_PIX_FMT_YUYV。其他格式应明确返回 IoError，
 *          不允许按 YUYV 强行解释后输出花屏。
 */
class V4l2Source : public ISource {
public:
    V4l2Source() = default;
    ~V4l2Source() override;

    Status open(const SourceConfig& cfg) override;
    const SourceConfig& actualConfig() const override { return actualCfg_; }
    Status readFrame(RawFrame& out) override;
    void close() override;

private:
    /** @brief 一块由驱动分配、用户态 mmap 的采集缓冲 */
    struct MappedBuffer {
        void* start = nullptr;
        size_t length = 0;
    };

    /** @brief 校验宽高、帧率和设备路径；不得触碰设备 */
    Status validateConfig(const SourceConfig& cfg) const;

    /** @brief 打开设备节点，并确认 VIDEO_CAPTURE 与 STREAMING 能力 */
    Status openAndQueryDevice();

    /**
     * @brief 请求 YUYV、宽高和帧率，并把驱动返回的实际值写入 actualCfg_
     *
     * @note 请求值与实际值不同时必须打 WARN；最终格式不是 YUYV 时返回 IoError。
     */
    Status negotiateFormatAndRate();

    /** @brief 创建并缓存 YUYV422 -> YUV420P 的 SwsContext；不可每帧重建 */
    Status createConverter();

    /** @brief REQBUFS 后逐个 QUERYBUF/mmap，并保存到 buffers_ */
    Status mapBuffers();

    /** @brief 将全部缓冲 QBUF 后 STREAMON */
    Status startStreaming();

    /** @brief poll 等待设备可读；EINTR 重试，超时返回 Code::Timeout */
    Status waitForFrame() const;

    /**
     * @brief 将一块 YUYV mmap 数据转换进紧凑布局的 RawFrame
     *
     * @param data      DQBUF 对应的 mmap 首地址
     * @param bytesUsed 驱动报告的有效字节数
     * @param out       YUV420P 输出帧
     */
    Status convertFrame(const void* data, size_t bytesUsed, RawFrame& out);

    /** @brief 对 ioctl 的 EINTR 重试封装；其他错误原样返回 -1 并保留 errno */
    static int xioctl(int fd, unsigned long request, void* arg);

    /** @brief STREAMOFF；失败只能记录日志，不能阻止后续资源释放 */
    void stopStreaming();

    /** @brief munmap 所有成功映射的缓冲并清空 buffers_ */
    void unmapBuffers();

    SourceConfig requestedCfg_;
    SourceConfig actualCfg_;

    int fd_ = -1;
    bool opened_ = false;
    bool streaming_ = false;
    uint32_t devicePixelFormat_ = 0;
    uint64_t frameId_ = 0;
    size_t bytesperline_ = 0;

    std::vector<MappedBuffer> buffers_;
    SwsContext* sws_ = nullptr;

    // poll 不能永久阻塞，否则 Ctrl-C 和拔设备时管线无法及时退出。
    static constexpr int POLL_TIMEOUT_MS = 500;
};
