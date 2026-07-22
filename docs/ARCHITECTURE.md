# 架构与模块设计

## 三层结构（三个可执行共用）

```text
app/        入口与启动编排（解析参数、装配模块、启动/停止）
modules/    业务模块（capture / encode / transport / decode / render / server）
common/     基础设施（日志、配置、有界队列、时间、指标、网络工具）
```

**依赖方向严格单向**：`app → modules → common`。**common 不许反向依赖 modules。**

## 目录结构

```text
lowlat_stream/
├── PROJECT.md  MILESTONES.md  README.md
├── docs/       PROTOCOL.md  ARCHITECTURE.md  CONVENTIONS.md
├── CMakeLists.txt
├── app/
│   ├── sender/     main.cpp      # 推流端
│   ├── receiver/   main.cpp      # 接收端
│   └── server/     main.cpp      # 分发服务端
├── modules/
│   ├── capture/    ISource.h  NullSource.*  V4l2Source.*
│   ├── encode/     Encoder.*                      # libavcodec/x264 封装
│   ├── transport/  Packetizer.*  Fec.*  Nack.*  JitterBuffer.*  UdpSocket.*
│   ├── decode/     Decoder.*
│   ├── render/     SdlRenderer.*
│   └── server/     Room.*  Forwarder.*  Signaling.*
├── common/         Logger.*  Config.*  BoundedQueue.h  Clock.*  Metrics.*  Net.*
├── tests/
└── tools/          loss_injector 等测试工具
```

## 关键抽象

```cpp
// 采集源统一接口 —— Null 与 V4L2 可互换（便于无摄像头开发/压测）
class ISource {
public:
    virtual ~ISource() = default;
    virtual bool open(const SourceConfig&) = 0;
    virtual bool readFrame(RawFrame& out) = 0;   // 阻塞取一帧
    virtual void close() = 0;
};
```

```cpp
// 有界队列 —— 各线程之间唯一的解耦手段（阶段4支柱的工程化版本）
template <typename T>
class BoundedQueue {
public:
    bool push(T item);          // 队满阻塞；已关闭返回 false
    bool pop(T& out);           // 队空阻塞；空且关闭返回 false
    void close();               // 唤醒全部，干净收尾
    size_t size() const; size_t peak() const;
};
```

## 线程模型

### 推流端 sender

```text
[采集线程] --RawFrame队列--> [编码线程] --EncodedFrame队列--> [发送线程]
                                                              ├ 分片打包
                                                              ├ FEC 生成
                                                              ├ 重传缓存
                                                              └ pacing 控速
                                        [接收控制线程] ← NACK / PLI
```

### 接收端 receiver

```text
[接收线程] --> jitter buffer(排序/等待/FEC恢复/发NACK) --完整帧队列-->
[解码线程] --Frame队列--> [渲染线程 + 指标展示]
```

### 服务端 server

```text
[IO 线程(epoll/Reactor)] 收包
      ├ SIGNAL → 房间管理（加入/离开/心跳超时）
      ├ DATA/FEC → Forwarder：查房间订阅者 → per-client 发送队列
      └ NACK/PLI → 回传给对应推流端
[指标线程] 定期聚合 → Redis（TTL）
[HTTP 线程] 指标查询 API
```

**背压策略**：per-client 发送队列满时 → 丢**非关键帧**包（保 IDR），并计数；持续满则标记该 client 拥塞（后续可降码率或踢出）。

## 数据流全景

```text
V4L2/Null → RawFrame → Encoder(H.264) → Packetizer(分片+FEC)
   → UDP → Server(房间/转发) → UDP
   → JitterBuffer(排序/FEC恢复/NACK) → Decoder → SDL 渲染
                                                    ↓
                                          指标(延迟p50/95/99, 丢包, 恢复率)
```

## 错误与生命周期

- 所有模块提供 `start()` / `stop()`；`stop()` 必须**唤醒并 join 所有线程**（队列 `close()` 是关键）。
- 资源用 RAII 包装（socket、AVCodecContext、SDL 资源、mmap 缓冲）。
- 线程内异常不外抛；转为日志 + 状态标记。

## 与旧 demo 的区别（提醒自己）

| 旧 demo | 本项目 |
|---|---|
| 单文件、main 里全写完 | 三层分离、接口抽象 |
| 无错误处理 | 统一错误码 + 日志 |
| 硬编码参数 | 配置文件/命令行 |
| 无测试 | 单测覆盖协议/队列/FEC |
| 只跑通就行 | 可配置、可观测、可压测 |
