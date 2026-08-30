/**
 * @file    FecDecoder.cpp
 * @brief   FecDecoder.h 的实现
 * @author  zzj
 * @date    2026-08-28
 */
#include "modules/transport/FecDecoder.h"

#include <utility>

#include "modules/transport/Packet.h"

FecDecoder::FecDecoder(FecDecoderConfig config) : config_(std::move(config)) {}

void FecDecoder::onDataPacket(const uint8_t* packet, size_t len) {
    // TODO(M4.2):
    //   1. !enabled() 或 packet 为空 或 len <= PACKET_HEADER_SIZE + DATA_HEADER_SIZE -> 返回
    //   2. 整包拷进 recent_(这里**必须**拷贝: 调用方的 recvBuf_ 下一轮就被覆盖了)
    //   3. 超过 config_.recentPackets 就 pop_front
    //   4. stats_.recentPackets = recent_.size()
    (void)packet;
    (void)len;
}

bool FecDecoder::onFecPacket(const uint8_t* packet, size_t len, PacketBuffer& out) {
    // TODO(M4.2):
    //   1. !enabled() -> false
    //   2. decodePacketHeader + 类型必须是 Fec + decodeFecHeader
    //      任一失败 -> ++fecPacketsMalformed, 返回 false
    //   3. ++fecPacketsReceived
    //      异或载荷 = packet + PACKET_HEADER_SIZE + FEC_HEADER_SIZE, 长度 = len - 24
    //   4. 在 recent_ 里找出组内成员: 对每个保留的包解出它的 seq,
    //      `seq - groupBaseSeq < groupSize` 即为组内(**无符号减法**, seq 会回绕)
    //      统计 present 个数, 并记住任意一个活着的成员(用来重建包头)
    //   5. 三种结局:
    //      present == groupSize      -> ++groupsComplete, false
    //      present <= groupSize - 2  -> ++groupsUnrecoverable, false
    //      present == groupSize - 1  -> 恢复:
    //        a. 缺失 seq = groupBaseSeq..+groupSize-1 里没出现的那个
    //        b. 载荷 = FEC 的异或载荷 ^ 每个活着成员的载荷(短的按补零处理, 只异或到它自己的长度)
    //        c. 长度 = payloadLenXor ^ 每个活着成员的载荷长度
    //           **不能**用异或缓冲的长度 —— 那是组内最长的那片, 缺失的可能是短的那片
    //        d. 长度校验: 必须 >= 1 且 <= 异或缓冲长度; 不满足 -> ++groupsUnrecoverable, false
    //           (这是唯一能挡住"异或错组"的护栏, XOR 本身没有校验)
    //        e. 重建包头: PacketHeader{type=Data, streamId/timestampMs 取自活着的成员,
    //           seq = 缺失 seq}; DataHeader{frameId/fragCount/flags 取自活着的成员,
    //           fragIndex = 活着成员的 fragIndex + (缺失 seq − 活着成员的 seq)}
    //           **组不跨帧**是这一步成立的前提
    //        f. fragIndex >= fragCount -> ++groupsUnrecoverable, false (第二道护栏)
    //        g. out = 头 + 载荷; ++groupsRecovered; ++packetsRecovered; true
    //
    //   恢复出的包**不要**再调 onDataPacket 塞回 recent_ —— 它已经被算进这一组了。
    (void)packet;
    (void)len;
    (void)out;
    return false;
}

void FecDecoder::reset() {
    // TODO(M4.2): 清空 recent_, stats_.recentPackets 归零; 累计统计保留(同 NackTracker::reset)
}
