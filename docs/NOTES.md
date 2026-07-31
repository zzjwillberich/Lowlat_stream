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
