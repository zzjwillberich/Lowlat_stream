# 工程规范

> 目的：让代码看起来像**工程**，不像作业。面试官是会看仓库的。

## 语言与构建

- **C++17**；`-Wall -Wextra`，Release 用 `-O2`
- **CMake**，out-of-source 构建：
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ctest --test-dir build
  ```

## 命名

| 对象 | 风格 | 例 |
|---|---|---|
| 类型 | 大驼峰 | `JitterBuffer` `PacketHeader` |
| 函数/变量 | 小驼峰 | `readFrame()` `frameId` |
| 成员变量 | 小驼峰 + 后缀 `_` | `queue_` `lastSeq_` |
| 常量/宏 | 全大写下划线 | `MAX_PAYLOAD` |
| 文件 | 与主类同名 | `JitterBuffer.h/.cpp` |

## 错误处理

- **不用异常跨模块**：模块接口返回 `bool` 或 `Status`（错误码 + 消息）。
- **每个系统调用/库调用都检查返回值**，失败必须打日志（这是旧 demo 最大的短板）。
- RAII 包装一切资源：socket、`AVCodecContext`、SDL 资源、mmap。

## 日志

- 分级：`TRACE/DEBUG/INFO/WARN/ERROR`
- 格式：`[时间][级别][模块] 消息`
- **热路径（每包/每帧）禁止 INFO 级日志**——会拖垮性能；用计数器 + 定期汇总。

## 配置

- 命令行参数 + 配置文件（简单 key=value 或 JSON 均可）
- 关键可调项必须外置：`--source` `--bitrate` `--gop` `--fec-k` `--jitter-ms` `--loss`（注入）`--cap`（队列容量）
- **不许硬编码 IP/端口/分辨率**

## 测试

- 单测优先覆盖**纯逻辑**（不依赖网络/设备）：
  - 协议：打包 → 解包 往返一致
  - FEC：构造丢 1 包场景 → 能恢复；丢 2 包 → 正确报失败
  - `BoundedQueue`：满/空阻塞、close 后不死锁
  - jitter buffer：乱序输入 → 有序输出
- 集成测试：Null 源 + loopback 跑通端到端

## Git

- 提交信息：`<模块>: <做了什么>`，例 `transport: add XOR FEC encoder`
- **小步提交**，一个提交只做一件事
- 每个里程碑打 tag：`m1-capture-encode`
- `.gitignore`：`build/`、`*.h264`、`*.yuv`、`*.mp4`、大样本

## README 要求（面试官第一眼看的）

1. 一句话说清项目是什么
2. **架构图**
3. **量化指标**（延迟 p50/95/99、X% 丢包下表现、单机 N 路）
4. 一键跑起来的步骤
5. 演示截图/GIF
6. 设计取舍说明（为什么 UDP、为什么 FEC+NACK、为什么不用 MySQL）
7. 踩坑记录

## 每个里程碑收尾动作

- [ ] 更新 README 的运行说明
- [ ] 记录本阶段踩的坑（**面试会问，且这是你相对买课者的优势**）
- [ ] 补该模块的单测
- [ ] 打 tag
