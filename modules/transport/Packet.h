/**
 * @file    Packet.h
 * @brief   自研 UDP 协议的包头定义与序列化, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-13
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

    /** @brief 标志位, bit0 = 关键帧(IDR), bit1 = 重传包; bit2~7 保留(必须为 0) */
    uint8_t flags = 0;

    /** @brief flags 的 bit0: 该分片属于关键帧 */
    static constexpr uint8_t FLAG_KEYFRAME = 0x01;

    /**
     * @brief flags 的 bit1: 这一片是**重传**的, 不是原发的 (M4)
     *
     * @note 由发送端响应 NACK 时置位, 原发包恒为 0。接收端靠它区分两件事:
     *          - 丢包注入器只丢原发包 —— 否则 `hash(seed, seq)` 对同一个 seq
     *            永远给同一个答案, 重传包会被反复丢掉, NACK 的恢复路径
     *            **永远跑不出成功的一次**;
     *          - 统计上"重传救回来几个"要单独算, 不能混进 packetsReceived 的口径。
     *
     * @note M4.2 之前没有任何地方会置位它, 恒为 0 —— 这是预留, 不是未实现的功能。
     */
    static constexpr uint8_t FLAG_RETRANSMIT = 0x02;
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
 *  InvalidArg buf 为空, bufLen 不足, version 不是 PROTOCOL_VERSION,
 *                     或 type 不在已定义取值内
 *
 * @note 逐字段按固定偏移写, 不做 `memcpy(buf, &header, sizeof header)` ——
 *          结构体有对齐填充, sizeof 不等于线上长度, 且本机可能是小端。
 * @note type 在**编码时**就查, 而不是留给对端的 decode 去判 NetError:
 *          否则本端一行赋值写错, 现象是"对端一直丢包", 排查方向指向网络,
 *          bug 却在发送端。编码器的价值就是把错误挡在离 bug 最近的地方。
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

/**
 * NACK 包的私有头, 紧跟 PacketHeader 之后, 线上 2 字节。
 *
 * 载荷是 entryCount 条位图条目, 每条 6 字节 (uint32 pid + uint16 blp), 形状取自
 * RFC 4585 的 Generic NACK —— 只把 PID 从 16 位改成 32 位, 因为本协议的 seq 是 32 位。
 */
struct NackHeader {
    /** @brief 位图条目数, 恒 >= 1; 一条都没有的 NACK 包没有意义, 属于畸形 */
    uint16_t entryCount = 0;
};

/** @brief NackHeader 的线上字节数 */
constexpr size_t NACK_HEADER_SIZE = 2;

/** @brief 一条位图条目的线上字节数: uint32 pid + uint16 blp */
constexpr size_t NACK_ENTRY_SIZE = 6;

/**
 * @brief 一条位图条目覆盖的 seq 个数: pid 自己 + blp 的 16 位
 *
 * @note 真实网络的丢包是**扎堆**的, 所以 6 字节管 17 个通常比列表式 4 字节管 1 个省。
 *          但丢包**均匀分散**时它反而更费 —— 每个孤立缺口都要单独一条 6 字节。
 *          M4 的注入器是均匀随机的, 所以在当前能测到的场景里位图比列表费 50%;
 *          选它是因为真实网络的丢包形态, 别把"省空间"写成已验证的结论(见 [[D20]])。
 */
constexpr size_t NACK_SEQS_PER_ENTRY = 17;

/**
 * @brief 一个 NACK 包最多能装几条位图条目
 *
 * @note (1221 - 12 - 2) / 6 = 201 条, 覆盖最多 3417 个 seq。而缺口数被接收端的
 *          跟踪窗口卡死(NackTrackerConfig::windowPackets, 默认 1024) ——
 *          1024 个 seq 位最多需要 ceil(1024/17) = 61 条, **不管缺口怎么分布**。
 *          所以默认配置下一个包永远装得下整个窗口, 截断路径走不到。
 */
constexpr size_t MAX_NACK_ENTRIES = (MAX_DATA_PACKET_SIZE - PACKET_HEADER_SIZE -
                                     NACK_HEADER_SIZE) / NACK_ENTRY_SIZE;

/**
 * @brief 把一组缺失的 seq 编码成一个完整的 NACK 包
 *
 * @param header  通用包头; 调用方负责把 type 设成 PacketType::Nack
 * @param missing 缺失的 seq, **必须已按从老到新排好序**(collectNackTargets 的输出就是)
 * @param buf     出参缓冲, 至少 MAX_DATA_PACKET_SIZE 字节
 * @param bufLen  缓冲长度
 * @param outLen  出参, 实际写入的字节数
 *
 * @return Ok         编码成功
 *  InvalidArg missing 为空 / header.type 不是 Nack / buf 为空 / **bufLen 不足**
 *  Internal   分条数超过 MAX_NACK_ENTRIES
 *
 * @note 两种"装不下"是不同的错误, 别混:
 *          - `bufLen` 不够 -> InvalidArg, 同 encodePacketHeader/encodeDataHeader,
 *            调用方参数给小了, 反应是"改调用处";
 *          - 分条数超过 MAX_NACK_ENTRIES -> Internal, 那个上限是本文件按线上长度
 *            算出来的常量, 超了说明本端算错了或者调用方没有**先截断再调**。
 *          混成一个错误码, 拿到报错的人就分不清该改自己的调用还是该来改这个文件。
 *
 * @note 分条规则: 取第一个未编码的 seq 作 pid, 把它之后 16 个 seq 里也缺的置进 blp,
 *          然后跳到下一个未覆盖的 seq。**贪心即可**, 不需要求最优分条 ——
 *          最优解省不下几个字节, 而分条逻辑越绕越容易出错。
 *
 * @note seq 会回绕, "之后 16 个"要用无符号减法算距离, 不能直接比大小。
 */
Status encodeNackPacket(const PacketHeader& header, const std::vector<uint32_t>& missing,
                        uint8_t* buf, size_t bufLen, size_t& outLen);

/**
 * @brief 解出一个 NACK 包里所有缺失的 seq
 *
 * @param buf     整包(含 PacketHeader)
 * @param bufLen  实际收到的字节数
 * @param out     出参, 调用前会被 clear(); 顺序与编码时一致(从老到新)
 *
 * @return Ok         解析成功
 *  NetError   畸形: 版本/类型不对、长度对不上 entryCount、entryCount 为 0
 *                       或超过 MAX_NACK_ENTRIES
 *  InvalidArg buf 为空
 *
 * @note **长度必须和 entryCount 精确对得上**, 多一个字节少一个字节都算畸形。
 *          UDP 上收到的每个字节都是不可信输入, 拿 entryCount 去循环读数组之前
 *          必须先确认它和实际长度相符 —— 否则就是一次越界读。
 */
Status decodeNackPacket(const uint8_t* buf, size_t bufLen, std::vector<uint32_t>& out);

/**
 * FEC 包的私有头, 紧跟 PacketHeader 之后, 线上 12 字节。
 *
 * 载荷是组内各 DATA 分片**载荷**(不含包头)按字节异或的结果, 短的补零。
 *
 * @note 恢复**只认** groupBaseSeq + groupSize, 这两个字段是自描述的。
 *          groupIndex/groupCount 只用于统计和诊断("这帧的 FEC 收全了吗"), 不参与恢复。
 *          理由: **XOR 恢复没有任何校验**。异或错了一组包, 出来的是一串看着完全合法的
 *          字节, 长度也对, 组包器会把它当真分片收下喂给解码器 —— 没有报错, 只有偶发花屏。
 *          分片有 fragIndex/fragCount 可以验证拼得对不对, XOR 没有对应的东西,
 *          所以头里必须**直接写明覆盖哪些 seq**, 而不是让接收端按切分规则推算。
 */
struct FecHeader {
    /** @brief 本组覆盖 [groupBaseSeq, groupBaseSeq + groupSize) 这一段 DATA 的 seq */
    uint32_t groupBaseSeq = 0;

    /** @brief 组内 DATA 包数, 恒 >= 2; 1 个包的"FEC"就是原包副本, 那不叫纠错 */
    uint16_t groupSize = 0;

    /**
     * @brief 组内各分片**载荷长度**的异或
     *
     * @note 必须有这个字段: XOR 出来的是**字节**, 不是**长度**。一帧的最后一片是短的,
     *          恢复时长度猜错就是静默损坏(多出或少掉几百字节拼进帧里)。
     *          用它异或掉已收到的那些长度, 就得到缺失那片的真实长度。RFC 5109 同款做法。
     */
    uint16_t payloadLenXor = 0;

    /** @brief 本帧的第几组, 从 0 开始; **仅供诊断** */
    uint8_t groupIndex = 0;

    /** @brief 本帧共几组; **仅供诊断** */
    uint8_t groupCount = 0;

    /** @brief 保留, 必须为 0; 接收端应当忽略而不是判成畸形 */
    uint16_t reserved = 0;
};

/** @brief FecHeader 的线上字节数 */
constexpr size_t FEC_HEADER_SIZE = 12;

/**
 * @brief 一个 FEC 包的最大线上长度
 *
 * @note 12 + 12 + 1200 = **1224**, 比 MAX_DATA_PACKET_SIZE(1221) **大 3 字节** ——
 *          FEC 头比 DATA 头宽 3 字节。这 3 字节很容易咬人: 接收缓冲区如果还按
 *          MAX_DATA_PACKET_SIZE 开, 满载的 FEC 包会被 recvfrom 的 MSG_TRUNC 判成
 *          "数据报大于缓冲区"返回 NetError —— 现象是"FEC 全部收不到, 而 DATA 一切正常",
 *          排查方向会指向 FEC 的编码逻辑, 而根因在一个 resize 上。
 *          **所有收包缓冲一律用 MAX_PACKET_SIZE。**
 */
constexpr size_t MAX_FEC_PACKET_SIZE = PACKET_HEADER_SIZE + FEC_HEADER_SIZE + MAX_PAYLOAD;

/**
 * @brief 任意类型的包的最大线上长度; 所有收包缓冲都该按它开
 *
 * @note 目前最大的是 FEC(1224); NACK 满载是 12 + 2 + 201*6 = 1220, DATA 是 1221。
 *          加新包类型时记得把它算进来 —— 这是**一处**要改的地方, 而不是每个收包点各改一遍。
 */
constexpr size_t MAX_PACKET_SIZE =
    MAX_FEC_PACKET_SIZE > MAX_DATA_PACKET_SIZE ? MAX_FEC_PACKET_SIZE : MAX_DATA_PACKET_SIZE;

/**
 * @brief 把 FEC 私有头写入缓冲区(网络字节序)
 *
 * @return Ok         写入 FEC_HEADER_SIZE 字节
 *  InvalidArg buf 为空, bufLen 不足, 或 groupSize < 2
 */
Status encodeFecHeader(const FecHeader& header, uint8_t* buf, size_t bufLen);

/**
 * @brief 从缓冲区解析 FEC 私有头
 *
 * @return Ok         解析成功
 *  InvalidArg buf 为空或 bufLen < FEC_HEADER_SIZE
 *  NetError   groupSize < 2 (对端发错了或包被损坏)
 *
 * @note reserved 非 0 **不算畸形** —— 那是给以后版本留的扩展位, 老接收端应当忽略,
 *          否则协议再也没法往前兼容地加东西(同 DataHeader 的 bit2~7)。
 */
Status decodeFecHeader(const uint8_t* buf, size_t bufLen, FecHeader& out);
