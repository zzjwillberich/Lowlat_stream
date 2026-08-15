/**
 * @file    Packetizer.h
 * @brief   把一帧 H.264 码流切成可发送的 DATA 包, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-15
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/Status.h"
#include "modules/transport/Packet.h"

/** @brief 一个拼好的完整数据包(通用头 + DATA 头 + 载荷), 可直接交给 UdpSocket::sendTo */
using PacketBuffer = std::vector<uint8_t>;

/**
 * 待打包的一帧, **只借不拥有**。
 *
 * @note 这里不直接收 `EncodedFrame`, 是为了不让 transport 反向依赖 encode:
 *          接收端也要用 transport(组包、jitter buffer), 但它根本不该知道
 *          libavcodec 的存在。发送端多写一行构造这个视图, 换来的是接收端
 *          编译时不用背上编码器的头文件 —— 头文件是给别人看的合同,
 *          不该写明自己上游用了哪个库(同 Encoder.h 里前向声明 AVCodecContext 的理由)。
 * @note 是**视图**: data 指向的内存必须在 packetize() 返回前一直有效。
 */
struct EncodedFrameView {
    /** @brief Annex B 码流首地址, 含起始码 */
    const uint8_t* data = nullptr;

    /** @brief 码流字节数 */
    size_t len = 0;

    /** @brief 帧号, 由 EncodedFrame::frameId 透传 */
    uint64_t frameId = 0;

    /** @brief 采集时刻(毫秒), 由 EncodedFrame::captureMs 透传, **不在这里重新取时间** */
    uint64_t captureMs = 0;

    /** @brief 是否关键帧(IDR) */
    bool isKey = false;
};

/**
 * 分片打包器 —— 一帧进, 一组 DATA 包出。
 *
 * 之所以是个类而不是自由函数: 它要持有**跨帧连续**的全局 seq。这正是 seq 和
 * frameId 分离的意义 —— seq 只回答"网络层面丢没丢", 所以必须一路 +1 数下去,
 * 不因为换了一帧就重来。
 *
 * @note 不是线程安全的: seq 自增没有加锁。发送线程独占一个实例即可,
 *          需要多路就一路一个 Packetizer(streamId 也不同)。
 * @note 只负责切片和填头, **不碰 socket**。发不发得出去、要不要重传是上层的事,
 *          这样它才能在单测里被完整验证。
 */
class Packetizer {
public:
    /**
     * @param streamId   写进每个包头的流标识, M5 之前恒为 0
     * @param initialSeq 起始 seq
     *
     * @note initialSeq 可配不只是为了测回绕: 每次会话用一个随机起点, 上一次会话
     *          残留在网络里的迟到包就不会被当成本次的合法包收下(RTP 也是这么做的)。
     */
    explicit Packetizer(uint16_t streamId = 0, uint32_t initialSeq = 0);

    /**
     * @brief 把一帧切成若干 DATA 包
     *
     * @param frame 待打包的帧
     * @param out   出参, 打好的包按 fragIndex 顺序排列
     *
     * @return Ok         out 里是这一帧的全部分片, seq 已推进 out.size() 个
     *  InvalidArg frame.data 为空, frame.len 为 0, 或分片数超过 65535
     *
     * @note **要么整帧成功, 要么什么都没发生**: 校验全部前置, 失败时 out 不被修改、
     *          seq 也不推进。否则调用方拿到半帧包, 发出去接收端永远组不齐, 还白占了
     *          一段 seq —— 对端会把这段算成丢包, 指标跟着一起错。
     * @note out 由调用方持有并**复用**: 内部只 resize 不 shrink, 跑热之后每帧的
     *          堆分配次数归零。每帧新建一个 vector 会让分配器出现在火焰图上,
     *          而这条路径每秒要走几十次。
     * @note 空帧返回 InvalidArg 而不是"0 个包": fragCount 恒 ≥ 1, 一个空载荷的 DATA
     *          包在接收端和"这一帧全丢了"无法区分。编码器不该产出空帧, 真出现了
     *          说明上游有 bug, 应当报出来而不是悄悄发一个空包。
     */
    Status packetize(const EncodedFrameView& frame, std::vector<PacketBuffer>& out);

    /** @brief 下一个将要使用的 seq, 供测试和指标读取 */
    uint32_t nextSeq() const { return nextSeq_; }

    /** @brief 本打包器写进包头的 streamId */
    uint16_t streamId() const { return streamId_; }

private:
    /**
     * @brief 算这一帧要切几片
     *
     * @param len 码流字节数, 调用前保证 > 0
     *
     * @return 分片数, 恒 ≥ 1
     *
     * @note 向上取整。写成 `len / MAX_PAYLOAD + 1` 是最常见的错法: 帧长正好是
     *          MAX_PAYLOAD 整数倍时会多出一片空载荷, 接收端要么组包多等一片、
     *          要么拼出多余的零字节, 而这种帧长偶尔才出现一次, 现象是"偶发花屏"。
     */
    static size_t fragmentCount(size_t len);

    uint16_t streamId_ = 0;
    uint32_t nextSeq_ = 0;
};
