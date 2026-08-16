/**
 * @file    FrameAssembler.cpp
 * @brief   FrameAssembler.h 的实现
 * @author  zzj
 * @date    2026-08-15
 */

#include "modules/transport/FrameAssembler.h"

#include "modules/transport/Packet.h"

uint64_t AssemblerStats::expectedPackets() const {
    // TODO(M2): started 为 false 返回 0;
    //  否则 static_cast<uint32_t>(highestSeq - baseSeq) + 1 —— 无符号相减本身就是
    //  模 2^32, 回绕后差值仍然正确, 和 seqNewerThan 是同一套算术。
    return 0;
}

uint64_t AssemblerStats::packetsLost() const {
    // TODO(M2): expected = expectedPackets();
    //  expected > packetsReceived ? expected - packetsReceived : 0。
    //  必须夹住下限: 实收里含重复包, 重传多了会让实收超过跨度, 无符号相减会绕成天文数字。
    return 0;
}

FrameAssembler::FrameAssembler(size_t maxPendingFrames)
    : maxPendingFrames_(maxPendingFrames == 0 ? 1 : maxPendingFrames) {}

Status FrameAssembler::offer(const uint8_t* packet, size_t len) {
    // TODO(M2): 步骤:
    //  1. packet == nullptr → InvalidArg(本端传错了);
    //  2. len < PACKET_HEADER_SIZE + DATA_HEADER_SIZE → NetError + packetsMalformed++;
    //     注意这里和底层的错误码不一样, 见头文件里"责任方不同"的说明;
    //  3. decodePacketHeader 失败 → NetError + packetsMalformed++;
    //  4. ph.type != PacketType::Data → InvalidArg(**不计 malformed**:
    //     包本身是合法的, 错的是调用方的分派);
    //  5. decodeDataHeader(packet + PACKET_HEADER_SIZE, len - PACKET_HEADER_SIZE, dh)
    //     失败 → NetError + packetsMalformed++;
    //     (fragCount == 0 和 fragIndex >= fragCount 已经在那一层挡掉了)
    //  6. 走到这里包就算收下了: packetsReceived++; trackSeq(ph.seq);
    //  7. 水位检查: hasRetired_ && !seqNewerThan(dh.frameId, retiredFrameId_)
    //     → packetsTooLate++, 返回 Ok(**不是错误**: 这就是丢包/乱序的正常后果,
    //       报成 NetError 会让接收循环以为网络出了问题);
    //  8. 查/建条目:
    //     - 已存在且 completed → packetsDuplicate++, 返回 Ok;
    //     - 已存在但 fragCount 与 dh.fragCount 不符 → NetError + packetsMalformed++,
    //       **不要**照着新值 resize: 伪造一个 fragCount = 60000 的包就能让你分配一大片,
    //       而且那一帧永远不可能齐;
    //     - 不存在 → 先 evictOldest() 腾位置(pending_.size() >= maxPendingFrames_ 时),
    //       再插入并 fragments.resize(dh.fragCount);
    //  9. 该 fragIndex 已经有内容 → packetsDuplicate++, 返回 Ok(**不覆盖**);
    // 10. 存下载荷(packet + PACKET_HEADER_SIZE + DATA_HEADER_SIZE, len - 两个头长),
    //     receivedFragments++, totalBytes += payloadLen;
    //     第一个分片到达时记下 timestampMs 和 isKey;
    // 11. receivedFragments == fragCount 时:
    //     - 按 fragIndex 顺序拼进 AssembledFrame(先 reserve(totalBytes), 别一片一片长);
    //     - ready_.push_back(std::move(frame)); framesCompleted++;
    //     - 转成墓碑: completed = true, 并**释放** fragments 的内存
    //       (clear() 不还内存, 要 swap 一个空 vector 或 shrink_to_fit)。
    (void)packet;
    (void)len;
    return Status::error(Code::Internal, "FrameAssembler::offer is not implemented (M2)");
}

bool FrameAssembler::pop(AssembledFrame& out) {
    // TODO(M2): ready_ 为空返回 false;
    //  否则 out = std::move(ready_.front()); ready_.pop_front(); return true。
    //  AssembledFrame 禁拷贝, 这里只能移动 —— 写成 out = ready_.front() 编译就过不去,
    //  这正是禁拷贝想要的效果。
    (void)out;
    return false;
}

void FrameAssembler::reset() {
    // TODO(M2): pending_.clear(); ready_.clear();
    //  hasRetired_ = false; retiredFrameId_ = 0; stats_ = AssemblerStats{};
    //  漏掉水位不清, 重连后新会话的 frameId 会被上一段的水位挡住, 现象是"重连之后一直黑屏"。
}

void FrameAssembler::trackSeq(uint32_t seq) {
    // TODO(M2): 第一个包(!started): baseSeq = highestSeq = seq; started = true;
    //  之后: seqNewerThan(seq, highestSeq) 时才更新 highestSeq。
    //  乱序来的老包不能把 highestSeq 拉回去 —— 拉回去之后 expectedPackets() 会缩水,
    //  丢包数被算成 0。
    (void)seq;
}

void FrameAssembler::evictOldest() {
    // TODO(M2): 线性扫 pending_, 用 seqNewerThan 找出最老的 frameId:
    //  对每个候选 id, 若 seqNewerThan(oldest, id) 则 oldest = id。
    //  然后:
    //  - 该条目未 completed → framesDropped++(墓碑不算丢帧, 它已经交付过了);
    //  - retiredFrameId_ = oldest; hasRetired_ = true;
    //  - pending_.erase(oldest)。
    //
    //  水位只在这里前移, **不在交付时前移**: 帧 5 齐了就交付, 但帧 4 可能还差一片,
    //  交付时推水位会把还有希望的帧 4 直接判死。
}
