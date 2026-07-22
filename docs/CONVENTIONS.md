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

## 注释

采用 C++ 标准库风格的 Doxygen 注释，只注释**意图**（为什么这样做），不注释**语法**（做了什么）。

- 多行文档用 `/** ... */`
- 单行或简短说明用 `//`

### 文件头

```cpp
/**
 * @file    JitterBuffer.h
 * @brief   乱序包排序与抖动缓冲，支持 FEC 恢复和 NACK 重传
 * @author  zzj
 * @date    2026-07-22
 */
```

### 类

```cpp
/**
 * 有界阻塞队列 —— 线程间唯一的数据通道。
 *
 * 生产者-消费者模型：
 * - push() 队满阻塞，直到有空位或被 close() 唤醒。
 * - pop()  队空阻塞，直到有数据或被 close() 唤醒。
 * - close() 唤醒所有等待者，消费者可将残留数据取完后安全退出。
 *
 * @tparam T  队列元素类型，支持 move-only 类型（如 std::unique_ptr）
 */
template <typename T>
class BoundedQueue { ... };
```

### 公开函数

```cpp
/**
 * 向队列尾部插入一个元素。
 *
 * 若队列已满，调用线程阻塞等待；若队列已 close，立即返回 false。
 *
 * @param item  要插入的元素（接受右值，内部 move）
 * @return  true  插入成功
 *          false 队列已 close
 * @note   多线程安全，可在任意线程调用
 */
bool push(T item);
```

### 私有实现

```cpp
// 唤醒所有阻塞在 notEmpty_ 上的消费者线程（在 close() / pop 成功时调用）
void notifyConsumers();
```

### 行内注释

```cpp
// 先让消费者取完残留数据，再返回 false——直接丢弃会丢帧
if (closed_ && q_.empty()) return false;
```

### 总结

| 位置 | 风格 | 说明 |
|---|---|---|
| 文件头 | `/** @file @brief */` | 一句话概括文件职责 |
| 类声明 | `/** ... @tparam */` | 说清设计意图和使用场景 |
| 公开 API | `/** @param @return @note */` | 每个参数、返回值、副作用都要写 |
| 私有函数 | `//` | 只写为什么、或非显而易见的细节 |
| 行内 | `//` | 解释 tricky 逻辑，不写 `i++ // 自增` 这种废话 |

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
