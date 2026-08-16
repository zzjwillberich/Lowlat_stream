# 踩坑记录

> 按里程碑追加。每条只记三件事：**现象**（编译器/运行时怎么报的）、**原因**（为什么）、**做法**（现在怎么写）。
> 只记真踩过的，不抄网上的通用列表。

---

## M0 · 工程骨架

### 1. 头文件里在类外定义函数，必须加 `inline`

**现象**
只有一个 `.cpp` 包含时一切正常；第二个 `.cpp` 也包含之后，**编译全过、链接失败**：

```
multiple definition of `parseLogLevel(std::string const&)'
first defined here
```

**原因**
不是"重复编译"，是**重复定义**。`#pragma once` 只防同一个翻译单元（TU）内重复展开，
挡不住两个 TU 各自展开一份。每个 TU 编出一份同名强符号，链接器合并时撞车 —— 违反 ODR。

**做法**
头文件里**在类外（命名空间作用域）定义**的函数一律加 `inline`；`inline` 在这里的语义不是
"建议内联展开"，而是"允许多个 TU 各有一份定义，链接器挑一个"。

以下几种是**隐式 inline，不用加**，别乱加噪音：

| 写法 | 是否需要 `inline` |
|---|---|
| 类**体内**直接写实现的成员函数（如 `Status::isOk()`） | 否，隐式 inline |
| 模板函数 / 模板类的成员函数 | 否 |
| `constexpr` 函数 | 否 |
| 类**体外**定义的成员函数（`inline bool Status::isOk() const {...}`） | **是** |
| 头文件里的普通自由函数 | **是** |
| 头文件里的非 const 全局变量 | **是**（C++17 `inline` 变量） |

> 另一条出路：只在头文件里**声明**，实现挪进 `.cpp`。热路径上的短函数（访问器）才值得放头文件，
> 见 `common/Status.h` 里 `isOk()/code()/message()` 三个访问器的处理。

---

### 2. 宏定义末尾不要写分号

**现象**

```cpp
#define LOG_INFO(mod, ...) Logger::instance().log(...);   // ← 结尾多了个分号

if (ok)
    LOG_INFO("sender", "done");
else                       // error: 'else' without a previous 'if'
    ...
```

**原因**
调用处本来就要写分号，宏里再带一个，展开后变成**两条语句**（第二条是空语句），
把 `if` 的单语句体撑爆，`else` 就找不到对应的 `if` 了。同理会让
`while (x) MACRO(...);` 的循环体提前结束。

**做法**
- 宏定义**永远不以分号结尾**，让调用点自己写分号——这样宏用起来才像函数。
- 单表达式宏：直接写表达式（本项目 `LOG_*` 就是这样，展开成一次函数调用）。
- 多语句宏：包 `do { ... } while (0)`（**注意 `while(0)` 后面也不加分号**），
  这是唯一能同时满足"能带 `;` 调用"和"能安全放进 `if` 单语句体"的写法。
- 宏参数每个都用括号裹住：`#define MAX(a,b) ((a) > (b) ? (a) : (b))`，防运算符优先级被拆。

---

### 3. 模板类不能拆成 `.h` + `.cpp`

**现象**
`BoundedQueue.h` 放声明、`BoundedQueue.cpp` 放实现，编译两个文件都过，**链接报错**：

```
undefined reference to `BoundedQueue<int>::push(int)'
```

**原因**
模板不是代码，是**代码生成器**。编译器只有在看到 `BoundedQueue<int>` 这个具体用法时
才会实例化出真代码，而实例化**必须能看到模板的完整定义**。
`main.cpp` 只 include 了 `.h`（光有声明），编不出实体；
`BoundedQueue.cpp` 里有定义，但它自己没用过 `BoundedQueue<int>`，不会主动实例化。
两边都没生成 → 符号不存在。

**做法**
模板**全部实现写在头文件里**（本项目 `common/BoundedQueue.h` 就是这么做的）。
如果嫌头文件太长，可以把实现放 `.tpp/.ipp`，在头文件**末尾** `#include` 进来——
本质仍是同一个头文件，只是物理分开。
第三条路是显式实例化（`template class BoundedQueue<int>;` 写在 `.cpp` 里），
但那要求**类型集合是封闭的**；本项目后面要塞 `unique_ptr<Frame>`、`unique_ptr<Packet>`
等一堆类型，用它等于每加一种类型改一次库，不划算。

---

### 4. `condition_variable::wait` 必须带谓词

**现象**
`BoundedQueue` 压测偶发：队列明明是空的，`pop()` 却往下走了；或者 `close()` 之后
个别线程死活不退出。

**原因**
两件事叠在一起：**虚假唤醒**（wait 可能无缘无故返回），以及 `close()` 用的是
`notify_all()`——多个线程被同时唤醒，但只有第一个抢到数据，其余的醒来时条件已经不成立。

**做法**
一律用带谓词的重载，等价于"`while (!pred()) wait();`"：

```cpp
notEmpty_.wait(lk, [this]{ return !q_.empty() || closed_; });
```

谓词里**必须把 `closed_` 一起判**，否则 `close()` 唤不醒等待者，退出时直接挂死。

---

### 5. `close()` 的语义在 push 和 pop 上是**不对称**的

**现象**
第一版 `pop()` 写成 `if (closed_) return false;`，结果 `close()` 一调，队列里
残留的数据全丢了——表现为"退出时最后几帧莫名其妙没了"。

**原因**
关闭的语义是"不再接收新数据"，不是"立刻丢掉已有数据"。

**做法**

| | close 后的行为 |
|---|---|
| `push` / `tryPush` | 立即返回 false，拒收新数据 |
| `pop` | **先把残留取完**，取空之后才返回 false |

所以 `pop()` 的判断是 `if (q_.empty()) return false;` 而不是判 `closed_`
（见 `common/BoundedQueue.h`）。这样消费者线程能自然跑完 `while (q.pop(x))` 循环退出，
不需要额外的退出标志位。

---

### 6. 按值传参的 `push` 失败时，实参已经被掏空

**现象**
`if (!q.push(std::move(frame))) { /* 想把 frame 重新入队 */ }` —— 拿到手的是个空壳。

**原因**
`bool push(T item)` 按值接收，`std::move` 在**进函数那一刻**就完成了转移，
后面无论成功失败，调用方的 `frame` 都已经是 moved-from 状态。

**做法**
接受这个语义并在文档里写死：push 返回 false 意味着整条流水线正在退出，丢弃可接受。
真需要"失败可回滚"的地方，改用 `tryPush` **先探测再转移**，或者传指针自己管所有权。

---

### 7. 先解锁再 notify

**现象**
不算 bug，是性能问题：持锁状态下 `notify_one()`，被唤醒的线程立刻又堵在同一把
mutex 上，白白多一次上下文切换（惊群的小号版本）。

**做法**
```cpp
lk.unlock();
notEmpty_.notify_one();
```
反过来 `close()` 里是先在小作用域里改完 `closed_` 再出作用域解锁、然后 `notify_all()`，
同一个道理。

---

### 8. `std::mutex` 不可重入，私有辅助函数不要再锁一次

**现象**
把 `push`/`tryPush` 的公共部分抽成 `enqueue()`，里面又 `lock_guard` 一次 —— 自己把自己锁死。

**做法**
抽出来的函数**不加锁**，用 `Locked` 后缀 + 签名上收一个用不到的
`const std::unique_lock<std::mutex>&` 参数，把"必须在锁内调用"这件事钉在接口上
（编译器查不了这个约定，锁外误调是**不崩不报错的数据竞争**，只能靠命名和签名防）。
参数配 `[[maybe_unused]]`，否则 `-Wextra` 会报未使用参数。

---

### 9. `size()` 返回即过期

多线程下 `size()` 的返回值在 return 的下一纳秒就可能变了。
只能用于**观测**（打点、日志、`peak()` 水位分析），
绝不能写 `if (q.size() < cap) q.push(...)` 这种 check-then-act —— 中间有窗口。
真要非阻塞入队，用 `tryPush`（判断和入队在同一个临界区内）。

---

### 10. 阻塞类单测必须有超时保护

**现象**
测试一旦写错（比如 `close()` 没唤醒等待者），`ctest` 不是失败，是**永远卡住**，
CI 挂到超时才被杀。更阴的是 `std::future` 的**析构函数也会阻塞**到任务结束。

**做法** 两层保险：
1. 用例内部 `std::async` + `future.wait_for(timeout)` 判断是否按预期阻塞/返回；
2. CMake 侧兜底：`gtest_discover_tests(${name} PROPERTIES TIMEOUT 30)`，
   让挂死表现为"这条用例失败"而不是"整条流水线卡住"。

---

### 11. 测试目标别逐个列 `../common/*.cpp`

**现象**
最初 `tests/CMakeLists.txt` 里把 common 的源文件一个个列进 `add_executable`。
`common/` 每加一个 `.cpp` 就得同步改测试这边，漏一个就是 undefined reference；
同一份源码还被编译两遍。

**做法**
统一 `target_link_libraries(... PRIVATE llcommon)`。
`llcommon` 用 `PUBLIC` 声明的 include 路径和 `pthread` 会自动传递过来，测试侧一行不用重复写。
顺带：根 `CMakeLists.txt` 里 `add_subdirectory(common)` **必须排在 `app/*` 之前**，否则找不到 target。

---

### 12. clangd 报一堆"找不到头文件"的假错

**现象**
代码能正常编译，编辑器里却满屏红波浪线。

**原因**
根目录放了一份手工拷贝的 `compile_commands.json`。新增源文件、新增 FetchContent 依赖之后，
这份副本就过期了，clangd 拿着旧的 include 路径找不到东西。

**做法**
`.clangd` 里直接指向 build 目录，让它读 `CMAKE_EXPORT_COMPILE_COMMANDS` 生成的**活的**数据库：

```yaml
CompileFlags:
  CompilationDatabase: build
```

---

### 13. 单测把日志格式钉死了，改格式就会红

`test_logger` 用正则匹配 `[时间][级别][模块]`，其中级别是**5 字符右对齐**（`INFO ` 带一个尾空格）。
这不是坑，是**有意的**：格式是对外契约（后面 M6 要靠脚本解析日志算指标），
改格式就该让测试拦下来，而不是悄悄改完等下游解析崩。

---

## M1 · 采集 + 编码

### 14. 只 `= delete` 拷贝，会把移动一起弄没

**现象**
`RawFrame` 想做成"禁止拷贝、允许移动"，于是先写了两行：

```cpp
RawFrame(const RawFrame&) = delete;
RawFrame& operator=(const RawFrame&) = delete;
```

结果 `RawFrame b = std::move(a);` 编译失败，而且报的是**拷贝**构造函数：

```
error: use of deleted function 'RawFrame::RawFrame(const RawFrame&)'
note: declared here
    RawFrame(const RawFrame&) = delete;
```

明明写的是 `std::move`，报错却指向拷贝——第一眼完全看不懂。

**原因**
两步：

1. **只要声明了拷贝构造/拷贝赋值（哪怕是 `= delete`），编译器就不再隐式生成移动构造/移动赋值。**
   于是 `RawFrame` 压根没有移动构造函数。
2. `std::move(a)` 产生一个右值，重载决议里唯一能接住它的是 `const RawFrame&`
   （const 左值引用可以绑右值），而它恰好是 deleted → 报错，且报的是拷贝版本。

反过来也成立：声明了移动构造，拷贝构造会被自动 delete。这就是**五法则**——
拷贝构造、拷贝赋值、移动构造、移动赋值、析构，一旦手写其中任何一个，
其余几个的默认生成规则就会变，最稳妥的做法是**要么一个都别写（零法则），要么写全**。

**做法**
四行成套写，缺一不可：

```cpp
RawFrame(const RawFrame&)            = delete;   // 3MB 一帧, 隐式拷贝是无声的性能陷阱
RawFrame& operator=(const RawFrame&) = delete;
RawFrame(RawFrame&&)            = default;       // 队列靠 move 交接所有权
RawFrame& operator=(RawFrame&&) = default;
```

并在单测里用 `static_assert` 钉住这四条性质（见 `tests/test_frame.cpp`），
哪天有人手滑删掉 move 那两行，编译期立刻指出问题，而不是等到调用点报一句
看不懂的 "use of deleted function"。

这就是 **move-only 类型**的标准写法，`unique_ptr` / `thread` / `fstream` / `mutex`
全是这个路子：资源天然独占，拷贝没有合理语义，但所有权可以转交。

**附带一个坑**：`= default` 生成的是**逐成员移动**。`data`（vector）被偷走指针变空，
但 `width` / `height` / `frameId` 这些标量是**照抄**过去的——被 move 走的帧会
"自称 64×32 却一个字节数据都没有"。所以 move 之后不要再读那个对象，要用先 `reset()`。
流水线里全程传 `unique_ptr<RawFrame>`，正是为了根本不出现这种半空对象。

---

### 15. 周期性任务不要用相对睡眠

**现象**
`NullSource` 的帧率节流，第一反应是每帧睡一个固定时长：

```cpp
std::this_thread::sleep_for(std::chrono::milliseconds(1000 / fps));
```

实测 150 帧 @30fps（应该 5000ms）：

```
sleep_for   : 5245 ms   实际 28.60 fps
sleep_until : 4968 ms   实际 30.19 fps
```

5 秒漂 277ms，**折算到 60 秒是 3.3 秒**。而且是稳定偏慢，不是抖动。

**原因**
`sleep_for(T)` 的标准语义是"**至少**睡 T"（at least），保证不会少睡，从不保证不会多睡。
于是每帧的实际周期是：

```
一帧 = 干活时间 + sleep 时间 = 1.5ms + (33ms + 调度误差)
```

误差**恒为正**，且没有任何机制把它拿回来——只能一帧帧累加。写成递推式一眼看穿：

```
sleep_for   :  t(n+1) = t(n) + T + ε(n)      ← 误差进入下一轮的输入, 滚雪球
sleep_until :  t(n)   = start + n·T + ε(n)   ← 误差不进状态, 每帧独立
```

**做法**
以 `open()` 时取的 `start_` 为**绝对基准**，算第 n 帧的应到时刻：

```cpp
const auto offset = std::chrono::microseconds(frameId_ * 1000000ULL / fps);
std::this_thread::sleep_until(start_ + offset);
```

绝对基准**自带纠错**：某帧被系统卡住迟到 100ms，下一帧的目标时刻没变，它会睡得更短甚至
立即返回，把欠的追回来。实测 1800 帧 @30fps 用时 59967ms，理论值 59966.7ms，**偏差 <1ms**。

两个细节：

- **先乘后除**：`frameId_ * 1000000 / fps` 而不是 `frameId_ * (1000000 / fps)`。
  后者 `1000000/30 = 33333`（真值 33333.33），每帧丢 0.33µs——又一个只增不减的累加项。
  先乘后除，整个运行期的截断误差合计不超过 1µs。
- **治不了"真的算不过来"**：如果画一帧要 40ms 而目标 33ms，`sleep_until` 每次都发现已超时、
  立即返回，帧率自然掉下来。这是正确行为，节流器的职责是"别跑太快"，不是变出算力，
  更不该靠补帧去追（那会产生一串时间戳挤在一起的帧，下游更难受）。

**为什么这事对"低延迟"是硬伤**
漂移本身是"帧率不准"，它变成延迟要经过一条链：**速率失配 → 队列积压 → 积压就是延迟**。
下游全都按 30fps 的节奏工作，采集实际 30.3fps 的话每秒多产 0.3 帧，10 秒后队列里躺着
3 帧 = 100ms 额外延迟，且只增不减，直到队列满开始丢帧（第 5 条那个有界队列就是兜这个的）。
反方向偏慢则是接收端 jitter buffer 饿死、卡顿重复帧。

还有更阴的一条：`captureMs` 是端到端延迟的**起点**，采集节奏漂了这个时间戳就偏，
M3 算出的 p50/p95 本身就不准——**测量工具自己先漂了**。

> 通用结论：任何周期性的东西（定时器、心跳、pacing 发包、码率控制）都不要拿
> "上一次的时间"当基准，要拿一个固定原点。

---

### 16. 编码结束前必须发送空帧排空编码器

**现象**
所有输入帧都已经通过 `avcodec_send_frame()` 送入编码器，但直接关闭编码器后，输出视频的
最后几帧消失，时长比预期短；使用 B 帧或带 lookahead 的编码器时更容易出现。

**原因**
编码器不保证“输入一帧，立刻输出一个包”。为了 B 帧重排、帧间预测和码率分析，编码器可能
在内部暂存若干输入帧。送完最后一张正常帧只表示“目前没有继续送”，不表示“以后不会再送”，
所以编码器不能主动把依赖后续输入的状态收尾。此时直接 `avcodec_free_context()`，内部尚未输出的
编码包会被直接丢弃。

`avcodec_send_frame(ctx, nullptr)` 里的 `nullptr` **不是一张黑帧**，而是 EOF/排空信号：
告诉编码器不会再有输入，让它进入 draining 状态并输出所有剩余包。

**做法**
正常结束时先发送一次 `nullptr`，然后持续接收，直到 `avcodec_receive_packet()` 返回
`AVERROR_EOF`：

```cpp
int rc = avcodec_send_frame(ctx_, nullptr);
if (rc < 0 && rc != AVERROR_EOF) {
    return Status::error(Code::Internal,
                         "avcodec_send_frame(nullptr) failed: " + avErr(rc));
}

// flush 之后必须一直 drain 到 AVERROR_EOF，不能只收到 EAGAIN 就当作完成。
return drainPackets(out);
```

注意三个边界：

- 发送 `nullptr` 后已经进入 draining 状态，不能再发送普通帧；想重新开始要重置或重建上下文。
- `drainPackets()` 在普通编码阶段可以遇到 `AVERROR(EAGAIN)` 就返回；进入 draining 状态后则应
  一直接收到 `AVERROR_EOF`，此时正常流程不应再返回 `EAGAIN`。
- 不要用 `avcodec_flush_buffers()` 替代。前者的目标是**取完剩余输出**，后者是**重置内部状态**，
  可能直接丢掉尚未输出的数据。

---

### 17. 摄像头挂不进开发环境，最后改用 v4l2loopback 造虚拟设备

**现象**
V4L2 采集代码写完、契约测试全绿，但真机验证跑不起来 —— 开发环境里根本没有 `/dev/video0`。
为了给它变出一个摄像头，连试三条路，全废：

| 尝试 | 结果 | 原因 |
|---|---|---|
| WSL2 直通笔记本摄像头 | `/dev/video*` 不存在 | WSL2 默认内核没编 `CONFIG_VIDEO_DEV`，**没有 V4L2 子系统**。就算用 `usbipd-win` 把 USB 设备转进去，也没有驱动去认领它 |
| VMware 虚拟机 + USB 直通 | `lsusb` 里始终看不到设备 | 笔记本内置摄像头虽然走 USB 协议，但被主机固件/系统握得很紧，VMware 抢不过来 |
| `apt install v4l2loopback-dkms` | dkms 编译失败 | 发行版打包的是 0.12.7，比内核老 |

第三条的报错值得单独记，因为它和前两条不是一类问题：

```
v4l2loopback.c:2089:9: error: too few arguments to function 'v4l2_fh_add'
  2089 |   v4l2_fh_add(&opener->fh);
/usr/src/linux-headers-.../include/media/v4l2-fh.h:97:6: note: declared here
    97 | void v4l2_fh_add(struct v4l2_fh *fh, struct file *filp);
```

**原因**
内核给 `v4l2_fh_add` / `v4l2_fh_del` 加了 `struct file *filp` 参数，而发行版仓库里的
v4l2loopback 版本早于这次改动。**内核模块没有稳定 ABI，也不承诺 API 兼容** —— 树外模块
（out-of-tree module）必须跟着内核版本走，发行版打包滞后半个版本就编不过。这类报错
和自己的代码无关，看 `note: declared here` 指向的内核头文件就能确认是签名对不上。

**做法**
拿上游源码编，两行改动补上新参数：

```bash
git clone https://github.com/umlaeute/v4l2loopback.git
cd v4l2loopback && make && sudo make install && sudo depmod -a
sudo modprobe v4l2loopback video_nr=0 exclusive_caps=1
```

然后用 ffmpeg 往这个虚拟设备里持续推固定图案，`/dev/video0` 就是一个标准 V4L2 采集设备，
`V4l2Source` 一行不用改：

```bash
ffmpeg -re -f lavfi -i testsrc=size=640x480:rate=30 -pix_fmt yuyv422 -f v4l2 /dev/video0
```

**为什么这不是"退而求其次"**
一开始把 v4l2loopback 当成没有摄像头时的替代品，跑通之后发现它在两件事上**强于真摄像头**：

- **可重复**。`testsrc` 每次输出的图案完全一致。调 `convertFrame()` 时颜色偏了、U/V 平面接反了、
  stride 算错了，一眼就能看出来；真摄像头对着房间拍，画面每次都不同，**没有参照物**，
  只能靠"看着好像不太对"来判断。
- **可回归**。虚拟设备可以在任意机器上按需创建，V4L2 这条路径因此能进 CI、能参与压测。
  真摄像头做不到 —— 它不能被脚本创建，也没法保证两次运行输入相同。

这和 `NullSource` 的存在理由是同一条：**可重复的输入是调试和压测的前提**。区别只在于
`NullSource` 绕过了整个 V4L2 子系统，而 v4l2loopback 保留了完整的
`ioctl` / `mmap` / `QBUF` / `DQBUF` 调用链，验证的是真实的内核交互路径。

> 更一般的教训：**先问"我到底要验证什么"，再决定环境怎么搭**。这里要验证的是
> "V4L2 协议交互写对了没有"，不是"这个摄像头能不能用"。一开始盯着"把物理摄像头挂进来"，
> 是把手段当成了目的，白花了一下午在 USB 直通上。

---

## M2 · 自研传输

### 18. 协议头不能把整个结构体 `memcpy` 上网

**现象**
发送端和接收端使用同一份 `PacketHeader` 定义，本机自测可能正常；一旦更换编译器、调整字段顺序，
或与不同字节序的机器通信，接收端就会读出错误的 `streamId`、巨大 `seq` 等异常值。
线上协议明明规定包头是 12 字节，`sizeof(PacketHeader)` 也可能不是 12。

**原因**
`PacketHeader` 是 C++ 对象的**内存表示**，不是协议的**线上表示**，两者受不同规则约束：

- 编译器可能为满足字段对齐，在结构体成员之间或末尾插入填充字节；因此 `sizeof` 不保证等于各字段
  线上长度之和，填充字节也不属于协议。
- 多字节整数在内存中使用本机字节序；协议规定统一使用网络字节序（大端）。在常见的小端机器上，
  `0x12345678` 的内存顺序是 `78 56 34 12`，直接复制会把顺序发反。
- `#pragma pack(1)` 最多只能去掉填充，解决不了字节序问题，还会让内存布局依赖编译器扩展。

因此下面这种写法不可靠：

```cpp
std::memcpy(buf, &header, sizeof(header));  // 错：复制的是本机对象表示
```

**做法**
把线上布局当成协议契约，按规定偏移逐字段写入：`version` 写 `buf[0]`，`type` 写 `buf[1]`，
`streamId` 转为网络序后写 `buf[2..3]`，`seq` 写 `buf[4..7]`，`timestampMs` 写
`buf[8..11]`。解码时按相同偏移反向读取并转回本机字节序。

```cpp
buf[0] = header.version;
buf[1] = static_cast<uint8_t>(header.type);

const uint16_t streamId = htons(header.streamId);
const uint32_t seq = htonl(header.seq);
const uint32_t timestampMs = htonl(header.timestampMs);
std::memcpy(buf + 2, &streamId, sizeof(streamId));
std::memcpy(buf + 4, &seq, sizeof(seq));
std::memcpy(buf + 8, &timestampMs, sizeof(timestampMs));
```

这里禁止的是**一次复制整个结构体**；单个字段先完成字节序转换，再用 `memcpy` 写入固定偏移是安全的，
并且不会产生未对齐指针解引用的问题。`PACKET_HEADER_SIZE` 必须是协议明确规定的 12，不能写成
`sizeof(PacketHeader)`。

---

### 19. 组包缓存满了就淘汰最老的一条，会被"更老的迟到帧"反过来利用

**现象**
`FrameAssembler` 的残帧表按帧数封顶，满了就淘汰 `frameId` 最老的那一条。单元测试全绿，
局域网上也跑得好好的。但只要出现**超过整个窗口的乱序**——一个比表里所有帧都老的分片到达——
就会连着坏两件事：本来有希望收齐的帧被丢掉，而挤进来的那一帧又永远补不齐。

复现（容量 2，表里是还差一片的帧 5 和帧 6，此时帧 3 的第一片到达）：

```
after insert of old frame 3: pending=2 dropped=1
frame 3 completed? NO   tooLate=1  pending=2
```

**原因**
淘汰这个动作有个副作用：它要把**淘汰水位**推到被淘汰的那个 `frameId`，否则迟到的分片会把
已判死的帧重新建出来，占着容量等一个永远不会到齐的帧。于是形成了一个自食其果的顺序：

1. 帧 3 不在表里，表满了 → 淘汰最老的帧 5，水位推到 5；
2. 帧 3 插入成功；
3. 帧 3 的第二片到达 → 水位检查 `!seqNewerThan(3, 5)` → 判为迟到丢弃。

**帧 3 被自己刚才制造的水位挡在了外面。** 代价是帧 5 白丢了，帧 3 也没收上来，
而且帧 3 的条目还要占着容量直到它自己被淘汰——这期间 `retiredFrameId_` 还可能从 5 **倒退**回 3。

根子上是少问了一句：容量满的时候，**新来的这一帧配不配占这个位置**。无条件淘汰等于默认
"后到的一定比先到的新"，而这个前提在 UDP 上本来就不成立。

**做法**
淘汰之前先比一次，新来的必须比现有最老的还新，才有资格挤掉它：

```cpp
if (pending_.size() >= maxPendingFrames_) {
    const uint32_t oldestFrameId = oldestPendingFrameId();
    if (!seqNewerThan(dataHeader.frameId, oldestFrameId)) {
        ++stats_.packetsTooLate;   // 落后整个窗口, 收齐无望, 别挤掉还有希望的
        return Status::ok();
    }
    evictOldest();
}
```

改完之后同样的场景：`dropped=0`，帧 5 保住了，帧 3 被计为迟到。这条规则还顺带保证了
**水位单调**——只会淘汰比新来者更老的条目，`retiredFrameId_` 就再也不会倒退。

> 值得记下来的不是这个 bug 本身，而是**它为什么没被测出来**。22 条用例覆盖了乱序、重复、
> 重传、回绕、容量上限，唯独没有"新到的帧比表里所有帧都老"——因为写用例时脑子里的模型是
> "帧号大致递增，偶尔乱一点"。**测试只能覆盖你想得到的情况**，而这一条要等到 M4 上了 NACK
> 重传才会大量出现：重传的分片天然比当前在收的帧老得多，那时的现象是"一开重传帧率反而掉了"。
>
> 一般的教训：凡是**淘汰/驱逐**逻辑，都要单独问一句"被淘汰的一定该淘汰吗"。
> LRU、缓存、连接池都是同一类问题——按单一维度排序后取极值，很容易在极值那一端撞上
> 一个本不该参与排序的元素。
