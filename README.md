# LowLat Stream

> 面向**弱网环境**的低延迟视频传输系统：自研 UDP 传输层（分片 / FEC / NACK 重传 / 自适应抖动缓冲），
> 配 C++ 分发服务端（一推多拉），端到端可观测。

C++17 · CMake · GoogleTest · Linux(WSL2)

---

## 架构

```text
┌────────────┐        ┌──────────────────┐        ┌────────────┐
│  推流端     │        │    分发服务端      │        │  接收端     │
│ 采集→编码   │──UDP──▶│ 房间/配对/转发     │──UDP──▶│ 组包→解码   │
│ 分片→FEC    │◀─NACK──│ 指标采集(Redis)   │◀─NACK──│ 抖动缓冲→渲染│
└────────────┘        └──────────────────┘        └────────────┘
                              │
                         HTTP 指标查询
```

## 当前进度

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **M0** | 工程骨架：CMake / 日志 / 配置 / 状态码 / 有界队列 / 单测 | ✅ 完成（tag `m0-skeleton`） |
| M1 | 采集 + 编码（Null 模拟源 → H.264） | ⬜ |
| M2 | UDP 传输（分片 / 组包 / 丢包乱序统计） | ⬜ |
| M3 | 接收端解码渲染，端到端出画面 | ⬜ |
| M4 | 弱网对抗：NACK / FEC / jitter buffer / PLI ⭐ | ⬜ |
| M5 | 分发服务端：房间 + 一推多拉 + 背压 ⭐ | ⬜ |
| M6 | Redis 指标 + HTTP API + 压测报告 | ⬜ |

> M0 只搭地基，**不含任何业务逻辑**。三个可执行目前是空壳：解析参数、打一条日志、退出。
> 量化指标（延迟 p50/p95/p99、丢包下表现、单机路数）和演示截图会在 M3/M4/M6 补上。

---

## 构建与运行

依赖：CMake ≥ 3.16、支持 C++17 的编译器、pthread。GoogleTest 由 CMake `FetchContent` 自动拉取（首次构建需要联网）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

三个可执行：

```bash
./build/app/sender/lowlat_sender     --help
./build/app/receiver/lowlat_receiver --log-level=debug
./build/app/server/lowlat_server     --config conf/server.conf
```

### 通用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--help` | — | 打印用法并退出 |
| `--log-level <lv>` | `info` | `trace` / `debug` / `info` / `warn` / `error` |
| `--config <file>` | — | 配置文件，`key=value` 每行一条，`#` 注释 |
| `--listen <addr:port>` | `0.0.0.0:9000` | 服务端 / 接收端监听地址 |
| `--target <addr:port>` | `127.0.0.1:9000` | 发送目标地址 |

`--key=value` 和 `--key value` 两种写法都支持；优先级 **命令行 > 配置文件 > 默认值**；
未知参数**报错退出**，不静默忽略。

日志输出到 stderr，格式固定（M6 要靠脚本解析它算指标）：

```text
[2026-07-27 08:15:02.123][INFO ][sender] sender starting
```

---

## 目录结构

```text
app/        三个可执行的 main：sender / receiver / server
modules/    业务模块：capture / encode / transport / decode / render / server
common/     基础设施：Logger / Config / Status / BoundedQueue
tests/      GoogleTest 单测
tools/      压测与辅助脚本
docs/       设计文档
```

`common/` 编成静态库 `llcommon`，include 路径与 `pthread` 以 `PUBLIC` 方式传递，
上层 target 只需 `target_link_libraries(xxx PRIVATE llcommon)`。

---

## 设计取舍

**为什么 UDP 不用 TCP** —— 实时流里"迟到的正确数据"等于没用。TCP 的队头阻塞会让一个丢包
把后面所有已到达的数据一起卡住，延迟尖峰不可控；UDP 上自己做选择性重传，才能决定
"哪些包值得等、哪些直接放弃"。

**为什么 FEC 和 NACK 都要** —— 单靠 NACK，恢复一个包至少要一个 RTT，弱网下 RTT 本身就大；
单靠 FEC，抗突发丢包要付出固定带宽冗余。做法是 FEC 兜住零星丢包（不加延迟），
NACK 兜住 FEC 恢复不了的（省带宽），实在救不回来再 PLI 请求新 IDR。

**为什么队列必须有界** —— 无界队列 = 内存无界 = **延迟无界**。上游比下游快时，堆积起来的
每一帧最后都要变成端到端延迟。必须让上游感到疼（`push` 阻塞）或主动丢帧（`tryPush` 返回 false）。

**为什么不用 MySQL** —— 实时流不落库。指标是高频写、短时效、要 TTL 自动过期的数据，
没有持久化和关联查询场景，硬塞关系库属于设计冗余，用 Redis。

**为什么不跨模块抛异常** —— 公开接口一律返回 `Status`（错误码 + 消息）。`Status` 标了
`[[nodiscard]]`，忽略返回值直接编译告警——这是相对裸 `bool` 的主要收益。
`Status` 本身**不打日志**：一次错误沿调用栈返回会被拷贝多次，且底层并不知道调用方
打算重试还是退出，日志应由**处理**错误的那一层打。

---

## M0 踩坑记录

完整版见 [docs/NOTES.md](docs/NOTES.md)，这里挑几个典型的。

### 1. 头文件里在类外定义函数，必须加 `inline`

编译全过，**链接**报 `multiple definition`。`#pragma once` 只防同一个翻译单元内重复展开，
挡不住两个 `.cpp` 各展开一份——每个 TU 编出一个同名强符号，链接器合并时撞车（违反 ODR）。
`inline` 在这里的语义不是"建议内联展开"，而是"允许多个 TU 各有一份定义，链接器挑一个"。

类**体内**直接写实现的成员函数、模板、`constexpr` 是隐式 inline，不用加；
头文件里的自由函数、类体外定义的成员函数才需要。

### 2. 模板类不能拆成 `.h` + `.cpp`

两个文件单独编都过，**链接**报 `undefined reference to BoundedQueue<int>::push(int)`。
模板是代码生成器，实例化必须能看到完整定义：`main.cpp` 只 include 了声明，编不出实体；
`.cpp` 里有定义但它自己没用过 `BoundedQueue<int>`，不会主动实例化。
所以 `BoundedQueue` 的实现全部写在头文件里。显式实例化那条路要求类型集合封闭，
本项目后面要塞 `unique_ptr<Frame>` 等一堆类型，用不了。

### 3. 宏定义末尾不要写分号

调用处本来就要写分号，宏里再带一个，展开后多出一条空语句，把 `if` 的单语句体撑爆，
`else` 就报 `'else' without a previous 'if'`。多语句宏用 `do { ... } while (0)` 包住。

### 4. `close()` 在 push 和 pop 上是不对称的

`pop()` 第一版写成 `if (closed_) return false;`，结果 `close()` 一调，队列里的残留数据全丢了——
表现为"退出时最后几帧莫名其妙没了"。关闭的语义是"不再收新数据"，不是"丢掉已有数据"：
push 立即拒收，pop 要**先把残留取完**再返回 false，判断条件是 `q_.empty()` 而不是 `closed_`。

### 5. 阻塞类单测会挂死，不是失败

`close()` 唤不醒等待者时，`ctest` 不报错、直接卡死（`std::future` 的析构函数也会阻塞）。
两层保险：用例内 `std::async` + `future.wait_for(timeout)`；CMake 侧
`gtest_discover_tests(... PROPERTIES TIMEOUT 30)`，让挂死表现为"这条用例失败"而不是"流水线卡住"。

其余若干条（条件变量 `wait` 必须带谓词、先解锁再 notify、`std::mutex` 不可重入、
`size()` 返回即过期、按值传参的 `push` 失败时实参已被掏空、clangd 编译数据库过期……）
都在 [docs/NOTES.md](docs/NOTES.md) 里。

---

## 文档

- [PROJECT.md](PROJECT.md) — 项目总纲、技术选型、非目标
- [MILESTONES.md](MILESTONES.md) — 里程碑与验收标准
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 模块划分与线程模型
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — 自研 UDP 协议设计
- [docs/CONVENTIONS.md](docs/CONVENTIONS.md) — 工程规范
- [docs/NOTES.md](docs/NOTES.md) — 踩坑记录
