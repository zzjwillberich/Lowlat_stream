/**
 * @file    Packet.h
 * @brief   自研 UDP 协议的包头定义与序列化, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-13
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "common/Status.h"

/**
 * 协议版本号。
 *
 * @note 接收端收到不认识的版本必须**丢弃并计数**, 不能按当前版本硬解 —— 字段一旦挪位,
 *          硬解出来的是随机数, 表现为莫名其妙的巨大 seq 和组不起来的帧。
 */
constexpr uint8_t PROTOCOL_VERSION = 1;

/**
 * 包类型, 取值见 docs/PROTOCOL.md 的类型表。
 *
 * @note 用 uint8_t 底层类型固定线上宽度; 不要依赖编译器给 enum 选的默认宽度。
 */
enum class PacketType : uint8_t {
    Data   = 1,  ///< 媒体分片, 发→收
    Fec    = 2,  ///< 冗余校验包, 发→收 (M4)
    Nack   = 3,  ///< 请求重传, 收→发 (M4)
    Pli    = 4,  ///< 请求关键帧, 收→发 (M4)
    Signal = 5,  ///< 信令, 双向 (M5)
    Stats  = 6,  ///< 指标上报 (M6)
};

/**
 * 所有包共有的通用头, 线上 12 字节。
 *
 * `seq` 与 `frameId`/`fragIndex` **职责分离**, 这是本协议相对旧 demo 的主要改进:
 * - `seq` 每发一个 UDP 包 +1, 只回答"网络层面丢没丢、乱没乱序";
 * - `frameId` + `fragIndex`/`fragCount` 只回答"业务层面这一帧拼不拼得起来"。
 *
 * 混用一个字段时, 重传包和 FEC 包会把帧内序号搅乱, 丢包检测和组包互相干扰。
 *
 * @note 这是**内存表示**, 不是线上布局。序列化由 encode()/decode() 负责,
 *          不允许 `memcpy` 整个结构体上网 —— 结构体有对齐填充, 且本机字节序不一定是网络序。
 */
struct PacketHeader {
    /** @brief 协议版本, 当前恒为 PROTOCOL_VERSION */
    uint8_t version = PROTOCOL_VERSION;

    /** @brief 包类型 */
    PacketType type = PacketType::Data;

    /** @brief 流标识(房间内第几路), M5 之前恒为 0 */
    uint16_t streamId = 0;

    /**
     * @brief 全局包序号, 每发一个 UDP 包 +1
     *
     * @note 会回绕。接收端比较大小必须用**有符号差值**(`int32_t(a - b) > 0`),
     *          直接 `a > b` 在回绕点会把最新包判成最老包。
     */
    uint32_t seq = 0;

    /**
     * @brief 采集时刻(毫秒), 从 RawFrame::captureMs 一路透传
     *
     * @note 端到端延迟的起点。同一帧的所有分片共用一个值, 不在打包时重新取时间。
     */
    uint32_t timestampMs = 0;
};

/**
 * DATA 包的私有头, 紧跟 PacketHeader 之后, 线上 9 字节。
 */
struct DataHeader {
    /** @brief 帧号, 组包的依据 */
    uint32_t frameId = 0;

    /** @brief 该分片在帧内的序号, 从 0 开始 */
    uint16_t fragIndex = 0;

    /** @brief 该帧共分成几片, 恒 ≥ 1 */
    uint16_t fragCount = 1;

    /** @brief 标志位, bit0 = 关键帧(IDR) */
    uint8_t flags = 0;

    /** @brief flags 的 bit0: 该分片属于关键帧 */
    static constexpr uint8_t FLAG_KEYFRAME = 0x01;
};

/** @brief PacketHeader 的线上字节数 */
constexpr size_t PACKET_HEADER_SIZE = 12;

/** @brief DataHeader 的线上字节数 */
constexpr size_t DATA_HEADER_SIZE = 9;

/**
 * @brief 单个 UDP 包的载荷上限(不含任何包头)
 *
 * @note 1200 是保守值: 以太网 MTU 1500 减去 IP(20/40) + UDP(8) 还有富余,
 *          留出的空间用于容忍隧道/VPN 的额外封装。**宁可小也不要触发 IP 分片** ——
 *          IP 分片后任意一个分片丢失, 整个 IP 包都废掉, 相当于把丢包率放大数倍,
 *          而且这一层的丢失我们自己的 FEC/NACK 完全看不见。
 */
constexpr size_t MAX_PAYLOAD = 1200;

/** @brief 一个 DATA 包的最大线上长度 */
constexpr size_t MAX_DATA_PACKET_SIZE = PACKET_HEADER_SIZE + DATA_HEADER_SIZE + MAX_PAYLOAD;

/**
 * @brief 把通用头写入缓冲区(网络字节序)
 *
 * @param header 待写入的头
 * @param buf    输出缓冲区首地址
 * @param bufLen 缓冲区可写字节数
 *
 * @return Ok         写入 PACKET_HEADER_SIZE 字节
 *  InvalidArg buf 为空, 或 bufLen 不足, 或 version 不是 PROTOCOL_VERSION
 *
 * @note 逐字段按固定偏移写, 不做 `memcpy(buf, &header, sizeof header)` ——
 *          结构体有对齐填充, sizeof 不等于线上长度, 且本机可能是小端。
 */
Status encodePacketHeader(const PacketHeader& header, uint8_t* buf, size_t bufLen);

/**
 * @brief 从缓冲区解析通用头
 *
 * @param buf    输入缓冲区首地址
 * @param bufLen 可读字节数
 * @param out    出参, 解析成功时被填充
 *
 * @return Ok         解析成功
 *  InvalidArg buf 为空或 bufLen < PACKET_HEADER_SIZE
 *  NetError   版本号不认识, 或 type 不在已定义取值内
 *
 * @note 版本和类型不合法返回 NetError 而不是 InvalidArg: 这是**对端或网络的问题**,
 *          调用方的反应是"丢弃该包并计数", 不是"修正参数重试"。这两种反应完全不同,
 *          所以必须用不同的错误码区分。
 */
Status decodePacketHeader(const uint8_t* buf, size_t bufLen, PacketHeader& out);

/**
 * @brief 把 DATA 私有头写入缓冲区(网络字节序)
 *
 * @param header 待写入的头
 * @param buf    输出缓冲区首地址, 通常是通用头之后的位置
 * @param bufLen 缓冲区可写字节数
 *
 * @return Ok         写入 DATA_HEADER_SIZE 字节
 *  InvalidArg buf 为空, bufLen 不足, fragCount 为 0, 或 fragIndex >= fragCount
 */
Status encodeDataHeader(const DataHeader& header, uint8_t* buf, size_t bufLen);

/**
 * @brief 从缓冲区解析 DATA 私有头
 *
 * @param buf    输入缓冲区首地址
 * @param bufLen 可读字节数
 * @param out    出参, 解析成功时被填充
 *
 * @return Ok         解析成功
 *  InvalidArg buf 为空或 bufLen < DATA_HEADER_SIZE
 *  NetError   fragCount 为 0 或 fragIndex >= fragCount
 *
 * @note 这两个字段的自洽性必须在**解析时**就查: 组包器拿到 fragIndex >= fragCount
 *          的包会越界写数组。畸形包可能来自网络损坏, 也可能来自恶意构造, 一律当作
 *          NetError 丢弃。
 */
Status decodeDataHeader(const uint8_t* buf, size_t bufLen, DataHeader& out);

/**
 * @brief 比较两个可回绕的 seq 的先后
 *
 * @param a 左侧 seq
 * @param b 右侧 seq
 *
 * @return true 表示 a 比 b 新
 *
 * @note 判据是 `int32_t(a - b) > 0` —— 无符号回绕后差值仍然正确, 转成有符号即可得到
 *          带方向的距离。直接写 `a > b` 在 seq 从 0xFFFFFFFF 回到 0 时会把最新的包
 *          判成最老的包, 表现为"跑了几十分钟后突然全是乱序"。
 * @note 只在两个 seq 相距不超过 2^31 时有意义 —— 这是所有滑动窗口协议的共同前提。
 */
bool seqNewerThan(uint32_t a, uint32_t b);
