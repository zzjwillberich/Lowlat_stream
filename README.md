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
| **M1** | 采集 + 编码：Null 源 / V4L2 源 → 两线程管线 → H.264 | ✅ 完成（tag `m1-capture-encode`） |
| **M2** | UDP 传输：自研协议 / 分片打包 / 组包 / 丢包乱序统计 | ✅ 完成（tag `m2-transport`） |
| **M3** | 接收端：抖动缓冲 / H.264 解码 / SDL 渲染 → 三线程管线出画面 | ✅ 完成 |
| **M4** | 弱网对抗：丢包注入 / NACK 重传 / FEC / 自适应抖动缓冲 / PLI ⭐ | ✅ 完成 |
| M5 | 分发服务端：房间 + 一推多拉 + 背压 ⭐ | ⬜ |
| M6 | Redis 指标 + HTTP API + 压测报告 | ⬜ |

> M3 打通了全链路「采集 → 编码 → 分片 → UDP → 组包 → 抖动缓冲 → 解码 → 渲染」，
> 本机回环端到端 **p50 5ms / p99 7ms**（不含抖动缓冲的目标水位），900 帧零丢弃。
> 数据和复现命令见 [跑一遍 M3](#跑一遍-m3)。
>
> M4 在这条链路上加了四层弱网对抗，并用 89 格 × 3 种子的实验台量了每一层各值多少 ——
> 10% 丢包下从**渲染 47/1000** 变成 **995/1000**，见 [弱网实测](#弱网实测m4)。
> 过程中有**三条设计假设被自己的实验推翻**，都留在 [M4 文档](docs/M4_%E5%BC%B1%E7%BD%91%E5%AF%B9%E6%8A%97.md) 里。
> `lowlat_server` 仍是空壳，一推多拉留给 M5。

![端到端出画面](screenshot/M3.5测试实图.png)

---

## 构建与运行

依赖：CMake ≥ 3.16、支持 C++17 的编译器、pthread、FFmpeg 开发包（编解码与色彩转换），
以及 SDL2（接收端渲染；CMake 里是 `REQUIRED`，不是可选的）。
GoogleTest 由 CMake `FetchContent` 自动拉取（首次构建需要联网）。

```bash
sudo apt install -y build-essential cmake pkg-config \
     libavcodec-dev libavutil-dev libswscale-dev libsdl2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`pkg_check_modules` 查的是 pkg-config 的 `.pc` 文件，只有 `-dev` 包才带；装完若仍报
`Package 'libswscale' not found`，删掉 `build/` 重新 configure —— 查找失败的结果会进
`CMakeCache.txt`，不删缓存会一直沿用旧结论。

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

### sender 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--source <kind>` | `null` | `null` 合成画面源 / `v4l2` 摄像头源 |
| `--device <path>` | `/dev/video0` | V4L2 设备节点，仅 `--source=v4l2` 使用 |
| `--width` `--height` `--fps` | `640` `480` `30` | 期望采集参数，**驱动可能给出不同的值** |
| `--bitrate <kbps>` | `2000` | 编码码率 |
| `--gop <n>` | 同 `--fps` | 关键帧间隔（**单位是帧**，不是秒） |
| `--cap <n>` | `4` | 采集→编码队列容量 |
| `--send-cap <n>` | `4` | 编码→发送队列容量 |
| `--target <addr:port>` | `127.0.0.1:9000` | 发送目标；**传空串 `--target=` 表示不发送**，退回 M1 的纯本地 dump |
| `--frames <n>` | `100` | 采集帧数上限；`0` 表示一直跑到 Ctrl-C |
| `--dump <file>` | — | 落 H.264 Annex B 裸流 |
| `--dump-raw <file>` | — | 落编码前的 YUV420P 原始帧 |

分辨率和帧率是**期望值不是承诺值**：摄像头驱动可以只给相近的值，实际协商结果会打一条
INFO，编码器按**实际值**打开。

注意 `--target` **默认是开着的**：不带这个参数跑 sender，它也会往 `127.0.0.1:9000` 发。
没人监听时不会报错（UDP 无连接），只是白发一遍。想完全关掉网络这一级用 `--target=`——
排查问题时这一档很有用，它能一句话把范围劈成两半：关掉发送还坏就是采集/编码的事。

### receiver 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--listen <addr:port>` | `0.0.0.0:9000` | 监听地址；`:9000` 表示所有网卡 |
| `--dump <file>` | — | 落还原出的 H.264 Annex B 裸流 |
| `--frames <n>` | `0`（不限） | 收够 N 个**完整帧**后退出 |
| `--idle-timeout <ms>` | `0`（不超时） | 连续这么久没收到**任何包**就退出，脚本化验收要靠它 |
| `--recv-timeout <ms>` | `200` | 单次收包等待上限，决定 Ctrl-C 的最坏响应延迟 |
| `--render <kind>` | `sdl` | `sdl` 开窗显示 / `null` 只校验不开窗 / **空串 `--render=` 表示不解码不渲染**，退回 M2 的纯落盘 |
| `--jitter-ms <ms>` | `50` | 抖动缓冲的目标水位，直接加在端到端延迟上 |
| `--threads <n>` | `1` | 解码器线程数 |
| `--vsync <0\|1>` | `0` | 渲染是否等垂直同步；开了帧率就被显示器接管，测延迟时应保持 `0` |
| `--stats-interval <ms>` | `1000` | 每隔多久打一行运行时统计；`0` 表示不打 |

`--render` 的三档对应三种用途：`sdl` 是给人看的，`null` 是给脚本看的（逐帧校验尺寸、
格式、时间戳单调性，不需要 X server），空串则把解码渲染整段摘掉——排查问题时这一档
能一句话把范围劈成两半，和 sender 的 `--target=` 是同一个用法。

`--idle-timeout` 计的是"多久没收到包"而不是"多久没组齐帧"：只收到分片却一直组不齐时
网络显然还活着，按帧计时会把"一直丢包"误判成"对端已停止"，接收端自己退出。

### 跑一遍 M1

```bash
# 合成画面源：无需任何硬件
./build/app/sender/lowlat_sender --source=null --frames=300 --dump=out_null.h264
ffplay out_null.h264          # 能看到色块和递增的帧号
```

V4L2 这条路用 **v4l2loopback 虚拟设备**验证，不依赖物理摄像头（原因见
[NOTES 第 17 条](docs/NOTES.md)）：

```bash
sudo apt install -y v4l2loopback-dkms v4l-utils ffmpeg
sudo modprobe v4l2loopback video_nr=0 exclusive_caps=1

# 另开一个终端挂着，持续往虚拟设备推固定测试图案
ffmpeg -re -f lavfi -i testsrc=size=640x480:rate=30 -pix_fmt yuyv422 -f v4l2 /dev/video0

./build/app/sender/lowlat_sender --source=v4l2 --frames=300 --dump=out_v4l2.h264
ffplay out_v4l2.h264
```

真设备冒烟测试默认跳过，指定设备后才会运行：

```bash
LOWLAT_V4L2_DEVICE=/dev/video0 ctest --test-dir build --output-on-failure
```

### 跑一遍 M2

两个进程，本机回环。先起接收端（它会打印实际监听的端口），再起发送端：

```bash
./build/app/receiver/lowlat_receiver \
    --listen=127.0.0.1:9100 --dump=recv.h264 --idle-timeout=1500 &

./build/app/sender/lowlat_sender \
    --source=null --frames=60 --dump=send.h264 --target=127.0.0.1:9100
wait

cmp send.h264 recv.h264 && echo "逐字节一致"
```

两端的日志要能对上——这比"能播放"更能说明问题：

```text
[sender]   stopped: captured=60 encoded=60 bytes=400995 ... packets_sent=356 send_errors=0
[receiver] stopped: frames=60 bytes=400995 packets=356 lost=0 malformed=0 dropped=0
```

**验收判据是 `cmp` 而不是 ffplay**：播放器对残缺码流很宽容，少了一个分片照样往下播，
花那么几帧屏靠肉眼根本发现不了。字节比对是唯一能把"看起来对"和"真的对"分开的东西。

丢包和畸形包在统计里是**两条线**：`lost` 说明网络差，`malformed` 说明对端在乱发或者
版本对不上，处置方式完全不同，混成一个数就没法定位了。

日志输出到 stderr，格式固定（M6 要靠脚本解析它算指标）：

```text
[2026-07-27 08:15:02.123][INFO ][sender] sender starting
```

### 跑一遍 M3

WSL2 下窗口靠 WSLg 直接出来，不需要额外配 X server。

```bash
# 终端 A：接收端先起，它要先占住端口
./build/app/receiver/lowlat_receiver --listen=0.0.0.0:9000 --render=sdl --jitter-ms=50

# 终端 B：合成画面源，30 秒
./build/app/sender/lowlat_sender --source=null --target=127.0.0.1:9000 --frames=900 --fps=30
```

窗口里是递增的帧号叠在色带上——**帧号是验收的一部分**：色带花没花肉眼分得出，但帧
掉没掉、顺序乱没乱，只有帧号说得清。接收端每秒打一行运行时统计，退出时再打两行总结
（下面的 `stopped:` 实际是一行，这里为了可读折了行）：

```text
[receiver] fps=30 latency samples=30 p50=54ms p95=55ms | queueA=0 queueB=0 dropped=0 resyncs=0 assembler_lost=0
[receiver] latency total: samples=900 p50=55ms p95=56ms p99=57ms max=126ms
[receiver] stopped: frames=900 bytes=7829139 key=30 packets=6978 lost=0 malformed=0 dropped=0
           recv_errors=0 jitter_dropped=0 queue_dropped=0 resyncs=0 decoded=900 rendered=900
           queue_peak=1/2 elapsed=52106ms
```

无头验收（不开窗，可进 CI）：

```bash
./build/app/receiver/lowlat_receiver --listen=0.0.0.0:9000 --render=null --frames=100 &
./build/app/sender/lowlat_sender --source=null --target=127.0.0.1:9000 --frames=100
wait
```

`--render=null` 不开窗，但逐帧校验尺寸、像素格式、`captureMs` 与 `frameId` 的单调性，
不合法的帧计入 `framesRejected`。判据是 `rendered=100`，**一帧不少**。

#### 实测延迟

本机回环（WSL2，单机双进程），640×480@30fps，2000kbps，gop=30，900 帧：

| 统计量 | `--jitter-ms=0` | `--jitter-ms=50` |
|---|---|---|
| p50 | **5ms** | 55ms |
| p95 | 6ms | 56ms |
| p99 | **7ms** | 57ms |
| max | 19ms | 126ms |
| 丢帧 | 0 | 0 |

**三个分位数同时平移了整整 50ms，`max` 没有** —— 这正是选分位数而不是 mean/max 的理由：
两个 `max` 都是各自的第一帧（SDL 建窗 + 首张 texture + 解码器预热），跟稳态无关，
用它描述"这套系统有多快"会得出错误结论。

`--jitter-ms=0` 那一列是**管线自身的开销**：组包 → 解码 → 队列 B → 渲染，全程 5ms，
占 33ms 每帧预算的 15%。从中位数到 p99 只差 2ms（5 → 7），也就是说 900 帧里最慢的
那 1% 也只比典型帧慢 2ms——这是回环该有的样子，跨机器时会立刻难看，M4 的基线就是它。

#### 这些数字的边界

一个说清楚了边界的数字才是可信的，所以把测不掉的部分写在这里：

- **不含最后一次 vsync**。采样点在 `renderFrame()` 返回之后，而 `SDL_RenderPresent`
  返回不等于像素已经打到屏幕上。测的是"管线交付时刻"，不是"人眼看到时刻"。
- **`captureMs == 0` 的帧不进统计**。组包元信息查不到时时间戳会退化成哨兵值 0，
  这种帧照样渲染（画面比时间戳重要），但不计入延迟——否则会算出一个开机至今的天文数字。
- **只在同一台机器上成立**。`captureMs` 用 `steady_clock`，起点是本机开机时刻，
  跨机器相减是垃圾（见下方「已知限制」）。
- **统计日志的开销未计入，实测也量不到**。日志在渲染线程里，`Logger` 是全局锁 + 无缓冲
  stderr。把 `--stats-interval` 从 1000ms 压到 1ms（受 `popFor(5ms)` 底噪限制，实际约
  200 行/秒，频率涨 200 倍）后 p50/p95 一动不动。原因是渲染线程每帧有约 28ms 余量，
  这点开销落在它本来就在睡的时间里。**有余量时观测者效应不是"小"，是 0**；
  要显现得先把渲染线程榨干。

---

## 弱网实测（M4）

丢包用**可复现的注入器**（纯哈希，同种子同结果），延迟与抖动用 `tc netem`。
每格 3 个种子 × 1000 帧（33 秒 @30fps）。完整推导与八轮实验的踩坑见
[M4 文档](docs/M4_%E5%BC%B1%E7%BD%91%E5%AF%B9%E6%8A%97.md)。

### 每一层各值多少（10% 丢包）

| 配置 | 渲染 /1000 | 真丢包率 | NACK 救回 | FEC 救回 |
|---|---|---|---|---|
| 全关 | **47**（三个种子里两个是 **0**） | 10.5% | 0 | 0 |
| 只 FEC | 590 | 2.6% | 0 | 558 |
| 只 NACK | 998 | 0.02% | 817 | 0 |
| FEC + NACK | **995** | **0** | 261 | 558 |

有一处独立的交叉验证：「只 FEC」剩下 **261** 个包没救回来，
而「FEC+NACK」里 NACK 恰好救回 **261** 个 —— 两个数出自不同计数器。

「全关」那格三个种子里**两个一帧都没渲染出来**：IDR 约 25~30 个分片，
`0.9^30 ≈ 4%`，起播门等不到一个完整的关键帧。不是画质下降，是根本起不来。

### 自适应抖动缓冲 vs 固定水位（`tc netem` 注入抖动）

八格全部渲染 1000 帧，一帧不丢：

| 链路抖动 | 固定 p50 | **自适应 p50** | 固定 p95 | 自适应 p95 |
|---|---|---|---|---|
| 无 | 53ms | **8ms** | 53ms | 46ms |
| 10±5ms | 60ms | **19ms** | 60ms | 57ms |
| 20±10ms | 67ms | **34ms** | 70ms | 64ms |
| 30±15ms | 75ms | **49ms** | 79ms | 75ms |

**p50 省 25~45ms，种子间散布 ≤1ms；p95 只差 3~7ms，而单格内散布最大到 9ms。**
所以结论只能写成：**自适应把中位延迟砍到六分之一至一半，
而尾延迟与固定水位没有可测的差异** —— p95 上的那点差距落在噪声里。

### 跑一遍

```bash
./scripts/netem_check.sh     # 20 秒: 这台机器能不能给回环 UDP 加延迟
./scripts/m45_bench.sh       # 全量 89 格, 约 55 分钟
./scripts/m45_bench.sh g1    # 或只跑消融那一组
```

上面两张表背后的原始 CSV（逐次运行、未聚合）存在 [`docs/bench/`](docs/bench/)。

### 这些数字的边界

- **回环 RTT ≈ 0 时 NACK 的成绩是虚高的** —— 重传有 4 次机会，30% 丢包下残留只有
  `0.3^4 = 0.81%`。所以「高丢包率下仍然不花屏」这句话**还没有价格**，
  要跑「高丢包 × 高 RTT」的组合才算数。
- **FEC 实测过销 30.8%，不是理论的 25%** —— 冗余包要按组内最长分片补齐。
- **PLI 八轮里只真正触发过 2 次** —— 两个触发源都对不上实测到的故障，
  缺的那个是「帧被抖动缓冲判为过期」。留给 M5。

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

## 技术设计

### 1. UDP 上分层恢复，把可靠性放进延迟预算

自研 UDP 传输层让重传与放弃策略由应用控制：XOR FEC 恢复组内单片丢失，无需等待 RTT；
NACK 补救剩余缺片，PLI 请求新 IDR 恢复参考链。三者分别承担冗余恢复、按需重传和重新起播，
不能只看“包救回多少”，还要看是否赶上播放期限。回环下的消融结果见[弱网实测](#弱网实测m4)；
重传预算与当前 PLI 触发盲区见 [D25](docs/NOTES.md#d25-按典型值定的预算接不住偶尔的最坏值而把它加宽到最坏值往往买不起)。

### 2. FEC 按帧切组，编号与恢复信息独立

DATA 与 FEC 分别编号，避免冗余包污染 DATA 的丢包统计；每帧内按 K 个分片切组，
避免跨帧等组满，也避免“大 IDR 只配一个校验包、反而保护最弱”。
FEC 头携带组起始序号、组大小和载荷长度异或值，让接收端能确定恢复范围及短分片的真实长度，
降低两端推算规则不一致导致的静默损坏风险。推导见 [D24](docs/NOTES.md#d24-fec-的三个决定独立编号按帧切组自描述的头)。

### 3. 自适应抖动缓冲，同时考虑网络波动与重传预算

用滑动窗口的到达偏移分位数分离时钟映射与帧级抖动，只对目标水位限速调整；
获得有效 RTT 和帧周期估计后，再以“帧周期 + RTT”约束水位下限，避免水位缩得过低，
让补回的帧持续迟到。已有八格对照实验均渲染 1000 帧，中位延迟降低 25–45ms，
尾延迟改善未超出测量噪声。预算取舍与包级/帧级抖动的区别见 [D25](docs/NOTES.md#d25-按典型值定的预算接不住偶尔的最坏值而把它加宽到最坏值往往买不起)、[D26](docs/NOTES.md#d26-分片本身就是个低通滤波器一帧等它最慢的那一片)。

### 4. 三线程隔离抖动，有界队列按解码依赖丢帧

收包、解码、渲染各用一条线程，避免解码或显示卡顿阻塞收包。UDP 接收端不能靠阻塞
向发送端施加背压，因此队列 A（编码帧）满时清空并等待 IDR 重新起播，
队列 B（解码帧）满时丢最老帧，优先保留新画面；分别记录重同步和丢帧计数。
这让积压有界，也明确了两种丢弃的不同代价。完整取舍见 [D3](docs/NOTES.md#d3-接收端不存在反压阻塞式-push-压不住任何人只压死自己)、[D11](docs/NOTES.md#d11-接收端两个队列的丢弃策略一个泄压阀一个保险丝)。

### 5. 端到端可观测，明确统计口径和测量边界

采集时间戳随帧穿过传输与解码链路，渲染交付后统计 p50/p95/p99；
元信息缺失的帧仍可渲染，但不计入延迟，并单独计数。跨线程统计用互斥量发布整份快照，
避免多个原子字段拼出不一致的状态。当前延迟仅验证同机回环，且不包含最后一次屏幕显示等待。
端点定义、元信息追踪与快照设计见 [D10](docs/NOTES.md#d10-端到端延迟端点定义测不到的那一段以及为什么只做本地回环)、[D14](docs/NOTES.md#d14-静默的元信息丢失会以p99--3-天的形式出现在报告里)、[D18](docs/NOTES.md#d18-跨线程读统计先问要多强的一致性再选机制无锁不等于更轻)。

其余设计取舍见 [Note：补充设计取舍](docs/NOTES.md#补充设计取舍)
（采集源抽象、参数协商、包/帧编号、丢包统计、组包缓存、错误传播与指标存储）；
完整复盘见 [Note：设计债](docs/NOTES.md#设计债--回头看才发现的)。

---

## 已知限制 / 待办

已经想清楚但**当前阶段还不需要**的取舍，记在这里，等触发条件出现再动。

**弱网结论的三条边界**（M4 实测，详见 [M4 文档](docs/M4_%E5%BC%B1%E7%BD%91%E5%AF%B9%E6%8A%97.md)）：
丢包率的那个 X 还没定价（回环 RTT≈0 让 NACK 虚高，缺「高丢包 × 高 RTT」的组合）；
低丢包下偶尔丢的那一帧会连累约半个 GOP，而现有的两个 PLI 触发源都对不上它；
消融表是 M4.6 修复之前测的，`rendered` 那一列要在最终 build 上重跑。

**跨机器测延迟需要换时钟** —— `RawFrame::captureMs` 用 `steady_clock`，起点是**本机开机时刻**。
同一台机器上 `CLOCK_MONOTONIC` 全系统共享，两个进程相减有效，所以 loopback 场景（M1–M4）没问题；
但两台机器的起点毫不相干，直接相减得到的是垃圾。
真要跨机器测端到端延迟时，改用 NTP 对齐后的 `system_clock`，或像 RTP 那样用 **RTT/2** 估算单向延迟。
触发条件：M3 之后把 sender/receiver 部署到两台机器上。

**GOP 单位是帧，帧率被驱动改了它就不等价** —— `--gop` 默认取 `--fps`，意思是"每秒一个 IDR"。
但驱动只给 15fps 时 gop 仍是 30，就变成"每两秒一个 IDR"——起播时间和花屏恢复时间直接翻倍。
是否按实际帧率缩放 gop 是个有取舍的决定（IDR 越密越抗丢包，也越费码率），
要拿起播时间和花屏恢复时间说话才定得下来。M3 测了稳态延迟但**没埋起播延迟这个点**，
所以还是没数据。触发条件：起播延迟能测出来之后（见下方同名待办）。

**V4L2 只支持 YUYV** —— 驱动最终协商出别的格式（常见的是 MJPEG）时直接返回 `IoError`，
不按 YUYV 强行解释后输出花屏。要支持 MJPEG 得在采集侧接一次解码，
触发条件：碰到只出 MJPEG 的设备且确实需要用它。

**分片打包每包一次 memcpy，没做 `sendmsg`/`iovec` 零拷贝** —— 现在的做法是把包头和载荷拼进
一块连续缓冲再 `sendto`。`sendmsg` + `iovec` 可以让内核直接从两段内存收集，省掉这次拷贝。
没做是因为**还没有证据说它值得**：1200 字节的 memcpy 在 30fps 下每秒才几十次，
真正的开销大概率在 syscall 本身。触发条件：M6 压测时 memcpy 出现在火焰图上。

**发送队列满时是阻塞而不是丢帧** —— M2 的编码线程会被慢的发送端顶住。真正该做的是
丢非关键帧、保 IDR，但那需要"丢什么"的判断依据（`flags` 里的关键帧位）和一套背压策略，
属于 M4 的内容。现在阻塞是诚实的：本机回环发不出去只可能是自己写错了，悄悄丢包只会把
bug 藏起来。触发条件：M4 做背压策略时。

**渲染只吃 YUV420P** —— `SdlRenderer` 固定用 `SDL_PIXELFORMAT_IYUV` 的 streaming texture，
拿到别的像素格式直接返回 `InvalidArg`（那一帧被丢掉并计入 `framesRejected`，不中断管线）。
上游解码器现在只输出 YUV420P，所以这个限制不花钱。窗口尺寸和帧尺寸不一致时由
`SDL_RenderCopy` 拉伸，不需要额外处理。触发条件：接入输出 NV12 的硬件解码器时。

**丢帧策略是"泄压阀 + 保险丝"，还不区分帧类型** —— 队列 B 满了丢最老的一帧（33ms，
人眼无感），队列 A 满了整段清空并要求从下一个 IDR 重新起播（最长冻结一个 GOP，非常明显）。
两者代价差一个数量级，所以统计里是**两个独立字段**（`queue_dropped` 和 `resyncs`），
混成一个数就没法定位。真正该做的是按帧类型丢——丢 P 帧保 IDR，那需要一套背压策略，
属于 M4。触发条件：M4 做背压策略时。

**没有起播延迟这个数** —— M3 测的是稳态的端到端延迟，"从启动到第一帧出画"没有单独埋点。
上面那个 `max=126ms` 里混了 SDL 建窗、首张 texture 分配和解码器预热，不能当起播延迟用。
这直接卡住了另一条待办（`--gop` 是否该按实际帧率缩放），因为那个取舍要拿起播时间和
花屏恢复时间说话。触发条件：M4 做 PLI / 关键帧请求时顺便埋这个点。

---

## 踩坑记录

保留 4 条与管线正确性直接相关的记录；完整内容见 [Note](docs/NOTES.md)
（19 条踩坑记录，另有 D1–D26 设计复盘）。

### 1. 关闭队列后，消费者仍要取完残留数据

`pop()` 一看到 `closed_` 就退出，会让收尾时的最后几帧消失。
关闭后 push 拒收新数据，pop 则要等队列为空才返回 false；正常结束与立即停止也应分别处理。
详见 [Note 5](docs/NOTES.md#5-close-的语义在-push-和-pop-上是不对称的)、[D21](docs/NOTES.md#d21-立刻停止和正常结束的收尾动作不一样写成同一条路径就会吃掉尾巴)。

### 2. 编码结束必须显式排空，不能用重置代替

送完输入帧直接销毁编码器，可能丢掉内部暂存的尾帧。
用 `avcodec_send_frame(ctx, nullptr)` 发送 EOF，再持续 `receive_packet` 到 `AVERROR_EOF`；
`avcodec_flush_buffers()` 是重置状态，不能代替排空。详见 [Note 16](docs/NOTES.md#16-编码结束前必须发送空帧排空编码器)。

### 3. 协议头不能整块复制结构体上网

结构体的填充、对齐和本机字节序都不属于线上协议，整块 `memcpy` 会造成解析错误。
按固定偏移逐字段读写并转换字节序，头长度使用协议常量；单字段转换后再复制是安全的。
详见 [Note 18](docs/NOTES.md#18-协议头不能把整个结构体-memcpy-上网)。

### 4. 淘汰组包缓存前，先判断迟到帧是否值得接收

缓存满时无条件淘汰最老帧，会让更老的迟到分片挤掉有效残帧，而它自己又被交付水位挡住。
先比较新到帧与缓存中最老帧，只有更新的帧才可触发淘汰，并维护水位防止旧帧重建。
原有 22 条用例漏掉了“新到帧比整个窗口都老”的输入，详见 [Note 19](docs/NOTES.md#19-组包缓存满了就淘汰最老的一条会被更老的迟到帧反过来利用)。

其余记录按阶段查阅：[M0 工程骨架](docs/NOTES.md#m0--工程骨架)
（ODR、模板、宏、条件变量、阻塞测试等）、
[M1 采集编码](docs/NOTES.md#m1--采集--编码)
（移动语义、采集节拍、虚拟摄像头等）、
[M2 自研传输](docs/NOTES.md#m2--自研传输)。

---

## 文档

- [PROJECT.md](PROJECT.md) — 项目总纲、技术选型、非目标
- [MILESTONES.md](MILESTONES.md) — 里程碑与验收标准
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 模块划分与线程模型
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — 自研 UDP 协议设计
- [docs/CONVENTIONS.md](docs/CONVENTIONS.md) — 工程规范
- [docs/NOTES.md](docs/NOTES.md) — 踩坑记录、设计复盘与补充取舍
- [docs/测试记录.md](docs/%E6%B5%8B%E8%AF%95%E8%AE%B0%E5%BD%95.md) — 测试记录（测量时踩的，按轮次追加）

各里程碑的详细拆解与设计理由：
[M0 工程骨架](docs/M0_工程骨架.md) ·
[M1 采集编码](docs/M1_采集编码.md) ·
[M2 传输](docs/M2_传输.md) ·
[M3 解码渲染](docs/M3_解码渲染.md) ·
[M4 弱网对抗](docs/M4_%E5%BC%B1%E7%BD%91%E5%AF%B9%E6%8A%97.md)
