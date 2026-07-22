# 自研传输协议设计

> 跑在 UDP 之上的应用层协议。设计目标：**分片可组包、丢包可检测、可重传、可纠错、可请求关键帧**。
> 参考 RTP 的思想但简化自研（RTP 是同一问题的标准答案，见 [[RTP 说明]]）。

## 设计要点

- **全局 `seq`（每发一个 UDP 包 +1）** 用于**丢包/乱序检测**；
- **`frame_id` + `frag_index/frag_count`** 用于**组包还原**；
- 二者分离是关键：seq 管"网络层面丢没丢"，frame/frag 管"业务层面拼不拼得起来"。
  （旧 demo 只用 `nalu_index`+`frag_index`，检丢包不方便——这是本次改进点。）

## 通用包头（所有包共有，12 字节，网络字节序）

```cpp
#pragma pack(push, 1)
struct PacketHeader {
    uint8_t  version;    // 协议版本，当前 1
    uint8_t  type;       // 包类型，见下
    uint16_t stream_id;  // 流标识（房间内第几路）
    uint32_t seq;        // 全局包序号，每发一包 +1（丢包/乱序检测）
    uint32_t timestamp;  // 采集时刻（毫秒，用于端到端延迟测量）
};
#pragma pack(pop)
```

### 包类型

| type | 名称 | 方向 | 用途 |
|---|---|---|---|
| 1 | `DATA` | 发→收 | 媒体分片 |
| 2 | `FEC` | 发→收 | 冗余校验包 |
| 3 | `NACK` | 收→发 | 请求重传 |
| 4 | `PLI` | 收→发 | 请求关键帧 |
| 5 | `SIGNAL` | 双向 | 信令（加入房间/开始/停止/心跳） |
| 6 | `STATS` | 收→发/服务端 | 上报指标 |

## DATA 包

```cpp
struct DataHeader {          // 紧跟 PacketHeader
    uint32_t frame_id;       // 第几帧（组包用）
    uint16_t frag_index;     // 该帧第几片
    uint16_t frag_count;     // 该帧共几片
    uint8_t  flags;          // bit0: 关键帧(IDR)
};
// 之后是载荷（≤ MAX_PAYLOAD）
```

- `MAX_PAYLOAD = 1200`（保守值，留足 IP/UDP + 自定义头空间，避免 IP 分片）
- 载荷**包含起始码**，接收端还原出的即为合法 Annex B（可直接喂解码器/ffplay 验证）

## FEC 包（XOR 方案）

**分组**：每 `K` 个 DATA 包为一组，生成 1 个 FEC 包 = 组内所有载荷按字节异或（不足处补零）。

```cpp
struct FecHeader {
    uint32_t group_base_seq; // 该组第一个 DATA 包的 seq
    uint16_t group_size;     // K
    uint16_t payload_len;    // 组内最大载荷长度
};
```

- **恢复条件**：组内**恰好丢 1 个**包 → 用其余包 + FEC 包异或还原。
- 丢 ≥2 个 → FEC 无能为力，退回 NACK 或 PLI。
- `K` 可配：K 小冗余多、抗丢强、费带宽；K 大省带宽、抗丢弱。（又是取舍轴）

## NACK 包（重传请求）

```cpp
struct NackHeader {
    uint16_t count;          // 缺失的 seq 数量
    // 之后是 count 个 uint32_t seq
};
```

- 接收端发现 `seq` 缺口且**超过等待窗口仍未到**（排除乱序）→ 发 NACK。
- 发送端维护**重传缓存**（最近 N 个包，环形缓冲），收到 NACK 后重发。
- **限流**：同一 seq 最多重传 M 次；超过重传窗口的请求直接忽略（太老的包重传没意义）。

## PLI 包（关键帧请求）

无负载，仅靠 `PacketHeader.type=PLI`。

- 触发：关键帧分片无法恢复 / 缺口过大 / 新观众加入需要立即起画。
- 发送端收到后：**立即编码一个 IDR**（呼应"花屏撑到下一个 IDR"，主动缩短这个等待）。

## SIGNAL 包（信令）

用简单文本（JSON）承载，便于调试：

```json
{"cmd":"join","room":"1001","role":"publisher|player"}
{"cmd":"leave","room":"1001"}
{"cmd":"heartbeat","room":"1001"}
{"cmd":"ack","result":"ok","stream_id":1}
```

## 字节序

**所有多字节字段一律 `htonl/htons` 转网络字节序**（不图省事，跨机器必须）。

## 交互时序（简化）

```text
推流端                     服务端                    接收端
  │──SIGNAL(join,publisher)─▶│                          │
  │                          │◀─SIGNAL(join,player)─────│
  │──DATA(seq,frame,frag)───▶│──转发 DATA──────────────▶│
  │──FEC───────────────────▶│──转发 FEC───────────────▶│
  │◀─────NACK(seq列表)───────│◀──NACK（缺口）────────────│
  │──重传 DATA──────────────▶│─────────────────────────▶│
  │◀─────PLI─────────────────│◀──PLI（无法恢复）─────────│
  │──立即 IDR───────────────▶│─────────────────────────▶│
```

## 待定 / 可演进

- 拥塞控制：接收端定期上报丢包率/RTT，发送端据此调码率（M7）
- 升级到标准 RTP + FU-A（M7 可选）
- FEC 从 XOR 升级到 Reed-Solomon（可纠多个丢包）
