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
