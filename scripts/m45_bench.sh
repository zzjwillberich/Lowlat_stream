#!/usr/bin/env bash
#
# m45_bench.sh — M4.5 弱网对抗的量化实验台
#
# 用法:
#   ./scripts/m45_bench.sh                 跑全部六组 (方案甲, 约 50 分钟)
#   ./scripts/m45_bench.sh g0 g1           只跑指定的组
#   ./scripts/m45_bench.sh --dry-run       只打印将要跑的格子, 不真跑
#   ./scripts/m45_bench.sh --frames 300    缩短每格时长 (调脚本时用)
#
# 输出: bench/<时间戳>/
#   runs.csv     每一次运行一行 (原始数)
#   summary.csv  按格子聚合 (种子平均) + 派生指标
#   meta.txt     git commit / 内核 / 参数, 供复现
#   logs/        每次运行的两端完整日志
#
# ---------------------------------------------------------------------------
# 三条读数纪律, 违反了整张表就是废的:
#
#  1. 丢包只认 lost_exact + nack_pending, **不认 lost**。
#     lost 是 "seq 跨度 - 组包器实收", 而 FEC 包占 seq 空间却被组包器拒收 ——
#     每 K 个包就永久多算 1 个。而且流结束时最后几个缺口卡在 pending 里,
#     只看 lost_exact 会得出"一个都没丢"。
#
#  2. FEC 带宽只能从**发送端**读。接收端注入的丢包不改变发送端发了多少。
#
#  3. 每格至少 3 个种子。注入器是纯哈希, 同种子必然同结果 ——
#     这**不等于**"确定性所以不用重复", 一个种子只是一次抽样。
#     (同 NOTES: p99 of 100 samples is ONE sample)
# ---------------------------------------------------------------------------
#
# @note tc netem 挂在 lo 上, 会影响**本机全部回环流量** —— 跑 g3/g4 期间
#          别在同一台机器上跑数据库、本地服务之类的东西。脚本退出时会清掉,
#          被 kill -9 打断则不会, 手动清: sudo tc qdisc del dev lo root
#
# @note netem 在 lo 上是**逐方向**计的: delay 20ms 意味着去程 20ms、回程 20ms,
#          RTT = 40ms。下面所有 netem 参数写的都是**单向**值, 读结论时记得乘 2。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

RECV="$ROOT/build/app/receiver/lowlat_receiver"
SEND="$ROOT/build/app/sender/lowlat_sender"

FRAMES=1000          # 33s @30fps。低于 ~1000 时 p99 只有个位数样本, 是噪声不是尾延迟
FPS=30
SEEDS=(1 2 3)
WIDTH=640
HEIGHT=480
BITRATE=2000
PORT_BASE=19000

DRY_RUN=0
RUN_GROUPS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --frames)  FRAMES="$2"; shift 2 ;;
        --seeds)   IFS=, read -r -a SEEDS <<< "$2"; shift 2 ;;
        g*)        RUN_GROUPS+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
[[ ${#RUN_GROUPS[@]} -eq 0 ]] && RUN_GROUPS=(g0 g1 g2 g3 g4 g5 g6)

wants() { for g in "${RUN_GROUPS[@]}"; do [[ "$g" == "$1" ]] && return 0; done; return 1; }

# ---------------------------------------------------------------- 前置检查

for bin in "$RECV" "$SEND"; do
    [[ -x "$bin" ]] || { echo "缺少可执行文件: $bin  (先 cmake --build build)" >&2; exit 1; }
done

HAVE_NETEM=0
SUDO_KEEPALIVE=""
if { wants g3 || wants g4; } && [[ $DRY_RUN -eq 0 ]]; then
    if ! command -v tc >/dev/null 2>&1; then
        echo "!! 找不到 tc, g3/g4 跳过 (apt install iproute2)" >&2
    else
        echo "g3/g4 要给 lo 挂 netem, 现在申请一次 sudo (只在这里问一次):"
        if sudo -v; then
            # 探针用**真正要用的那条语法**(带抖动的 delay), 不是 `delay 1ms` ——
            # 探针和实际命令不一样的话, 语法问题会在 20 分钟后才暴露。
            if sudo -n tc qdisc replace dev lo root netem delay 10ms 5ms 2>/dev/null; then
                sudo -n tc qdisc del dev lo root 2>/dev/null
                HAVE_NETEM=1

                # sudo 凭据默认 15 分钟过期, 而全量要跑一个钟头。
                # 第一次跑就是这么废掉的: 探针在第 0 分钟通过, g3 在第 20 分钟开始,
                # 那时 `sudo -n` 已经要密码了 —— 每一格都打印"设置失败"然后跳过,
                # 脚本正常退出、返回 0。**最需要的两组就这样没了, 而且不报错。**
                ( while sudo -n true 2>/dev/null; do sleep 60; done ) &
                SUDO_KEEPALIVE=$!
            else
                echo "!! netem 挂不上 lo, g3/g4 跳过 (查 sch_netem 模块)" >&2
            fi
        else
            echo "!! 没拿到 sudo, g3/g4 跳过" >&2
        fi
    fi
fi
if [[ $HAVE_NETEM -eq 0 ]] && { wants g3 || wants g4; }; then
    cat >&2 <<'MSG'
!! g3/g4 会被跳过。这两组是 M4.3 自适应水位和"RTT 预算"预测的唯一证据 ——
!! 跳过它们, 那两条结论就一条都没有。
MSG
fi

# ---------------------------------------------------------------- 输出目录

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="$ROOT/bench/$STAMP"
LOGDIR="$OUT/logs"
mkdir -p "$LOGDIR"
CSV="$OUT/runs.csv"

{
    echo "commit    $(git rev-parse --short HEAD 2>/dev/null) $(git status --porcelain | wc -l) 个未提交改动"
    echo "kernel    $(uname -r)"
    echo "date      $(date -Is)"
    echo "frames    $FRAMES @ ${FPS}fps  ${WIDTH}x${HEIGHT} ${BITRATE}kbps"
    echo "seeds     ${SEEDS[*]}"
    echo "groups    ${RUN_GROUPS[*]}"
    echo "netem     $([[ $HAVE_NETEM -eq 1 ]] && echo available || echo UNAVAILABLE)"
    echo
    echo "netem 的 delay 值是单向的; lo 上去程回程各计一次, RTT = 2x。"
    echo "render=null: 走真解码, 不开窗口。SDL 的显示延迟不在这些数里。"
} > "$OUT/meta.txt"

# dropped(组包器丢的整帧)第一次跑漏掉了 —— 而"组好了几帧"和"解出来几帧"
# 之间的缺口正是这次最重要的发现, 少一个计数器就得靠推断。
RX_KEYS="frames key packets injected_drops lost_exact nack_pending nack_sent nack_seqs nack_recovered nack_gaveup fec_recv fec_recovered fec_unrecoverable pli_sent pli_suppressed dropped jitter_dropped queue_dropped resyncs decoded rendered jitter_delay jitter_peak malformed recv_errors elapsed"
TX_KEYS="captured encoded bytes key packets_sent send_errors nacks nacked_seqs retransmitted retx_misses reverse_malformed plis fec_packets fec_bytes"
LAT_KEYS="samples p50 p95 p99 max"

csv_header() {
    local h="group,cell,seed,netem,loss,adapt,fec_group,retx_ms,pli_ms,gop"
    for k in $RX_KEYS; do h="$h,rx_$k"; done
    for k in $TX_KEYS; do h="$h,tx_$k"; done
    for k in $LAT_KEYS; do h="$h,lat_$k"; done
    echo "$h"
}
csv_header > "$CSV"

# 从一行日志里把所有 key=value 抠出来, 按给定顺序输出成 CSV 片段。
# 值尾部的 ms 会被剥掉; 找不到的键留空 —— **留空而不是填 0**,
# "没有这个数"和"这个数是 0"必须能分开。
extract() {
    local file="$1" marker="$2"; shift 2
    awk -v keys="$*" -v marker="$marker" '
        index($0, marker) {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^[A-Za-z_][A-Za-z_0-9]*=/) {
                    p = index($i, "=")
                    k = substr($i, 1, p - 1)
                    v = substr($i, p + 1)
                    sub(/ms$/, "", v)
                    m[k] = v
                }
            }
        }
        END {
            n = split(keys, a, " ")
            s = ""
            for (i = 1; i <= n; i++) s = s (i > 1 ? "," : "") (a[i] in m ? m[a[i]] : "")
            print s
        }
    ' "$file"
}

# ---------------------------------------------------------------- netem

NETEM_ON=0
netem_set() {
    [[ $HAVE_NETEM -eq 1 ]] || return 1
    sudo -n tc qdisc replace dev lo root netem $1 >/dev/null 2>&1 || return 1
    NETEM_ON=1
}
netem_clear() {
    [[ $NETEM_ON -eq 1 ]] || return 0
    sudo -n tc qdisc del dev lo root >/dev/null 2>&1
    NETEM_ON=0
}
cleanup() {
    netem_clear
    [[ -n "$SUDO_KEEPALIVE" ]] && kill "$SUDO_KEEPALIVE" 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- 单次运行

RUN_INDEX=0
run_one() {
    local group="$1" cell="$2" seed="$3" netem="$4" recv_extra="$5" send_extra="$6"
    RUN_INDEX=$((RUN_INDEX + 1))
    local port=$((PORT_BASE + RUN_INDEX % 1000))
    local tag="${group}_${cell}_s${seed}"

    if [[ $DRY_RUN -eq 1 ]]; then
        printf '%-28s netem=[%s] recv=[%s] send=[%s]\n' "$tag" "$netem" "$recv_extra" "$send_extra"
        return 0
    fi

    local rlog="$LOGDIR/${tag}.recv.log" slog="$LOGDIR/${tag}.send.log"

    # shellcheck disable=SC2086
    "$RECV" --listen=127.0.0.1:$port --render=null --threads=1 --frames=0 \
            --idle-timeout=3000 --recv-timeout=100 --stats-interval=1000 \
            --seed="$seed" $recv_extra >"$rlog" 2>&1 &
    local rpid=$!

    # 等它真的 bind 上再发, 否则前几十个包打在空气上
    local ok=0
    for _ in $(seq 1 200); do
        grep -q "listening on port" "$rlog" 2>/dev/null && { ok=1; break; }
        kill -0 $rpid 2>/dev/null || break
        sleep 0.05
    done
    if [[ $ok -eq 0 ]]; then
        echo "  !! 接收端没起来: $tag (见 $rlog)" >&2
        kill $rpid 2>/dev/null; wait $rpid 2>/dev/null
        return 1
    fi

    # shellcheck disable=SC2086
    "$SEND" --source=null --target=127.0.0.1:$port \
            --width=$WIDTH --height=$HEIGHT --fps=$FPS --frames=$FRAMES \
            --bitrate=$BITRATE --gop=$FPS $send_extra >"$slog" 2>&1
    wait $rpid 2>/dev/null

    local rx tx lat
    rx="$(extract "$rlog" "stopped:" $RX_KEYS)"
    tx="$(extract "$slog" "stopped:" $TX_KEYS)"
    lat="$(extract "$rlog" "latency total:" $LAT_KEYS)"

    local loss adapt fecg retx plims gop
    loss="$(sed -n 's/.*--loss=\([0-9]*\).*/\1/p'          <<< "$recv_extra")"; loss="${loss:-0}"
    adapt="$(sed -n 's/.*--jitter-adapt=\([0-9]*\).*/\1/p'  <<< "$recv_extra")"; adapt="${adapt:-1}"
    plims="$(sed -n 's/.*--pli-ms=\([0-9]*\).*/\1/p'        <<< "$recv_extra")"; plims="${plims:-1000}"
    fecg="$(sed -n 's/.*--fec-group=\([0-9]*\).*/\1/p'      <<< "$send_extra")"; fecg="${fecg:-4}"
    retx="$(sed -n 's/.*--retx-ms=\([0-9]*\).*/\1/p'        <<< "$send_extra")"; retx="${retx:-200}"
    gop="$(sed -n 's/.*--gop=\([0-9]*\).*/\1/p'             <<< "$send_extra")"; gop="${gop:-$FPS}"

    echo "$group,$cell,$seed,\"$netem\",$loss,$adapt,$fecg,$retx,$plims,$gop,$rx,$tx,$lat" >> "$CSV"

    local rendered; rendered="$(cut -d, -f1 <<< "$(extract "$rlog" "stopped:" rendered)")"
    printf '  %-26s rendered=%-6s p95=%-5s 水位=%sms\n' "$tag" \
           "${rendered:-?}" "$(cut -d, -f3 <<< "$lat")" "$(cut -d, -f22 <<< "$rx")"
}

# 跑一格: 同样的配置, 每个种子跑一遍
run_cell() {
    local group="$1" cell="$2" netem="$3" recv_extra="$4" send_extra="$5"
    echo "[$group/$cell] netem=[${netem:-none}]"
    if [[ -n "$netem" ]]; then
        netem_set "$netem" || { echo "  !! netem 设置失败, 跳过这一格" >&2; return 1; }
    else
        netem_clear
    fi
    for s in "${SEEDS[@]}"; do
        run_one "$group" "$cell" "$s" "$netem" "$recv_extra" "$send_extra"
    done
}

FEC_OFF="--fec-group=0"
PLI_OFF="--pli-ms=0"

# 关重传要关在**发送端**(--retx-ms=0), 不能关在接收端(--nack-window=0)。
# --nack-window=0 关掉的是 NackTracker 本身 —— 而 lost_exact / nack_pending
# 都出自它, 关掉之后那几格的"真丢包"会全是 0, 看起来像一个包都没丢。
# **那是把温度计拔了当退烧**。
#
# 关在发送端的话, 接收端照样逐 seq 记账, 只是请求过去没人理 ——
# 前向路径与"没有重传"完全一致, 而丢包数依然可信。
# 代价是这几格的 nack_sent 不为 0(反向通道上确实发了), 读表时别误认为重传开着。
RETX_OFF="--retx-ms=0"

# 组的顺序: **脆的先跑**。g3/g4 依赖 sudo 和 netem, 是唯一会中途失效的东西;
# 排在两个钟头的实验末尾, 一旦失效就是白等。g0 仍然排最前 —— 它是自检,
# 它要是不干净, 后面全部不用跑。

# ================================================================= G0 基线
# 无损伤。建立地板, 并顺便确认: 无损伤时每一个丢包/恢复计数器都必须是 0。
# 任何一个非 0 都说明测量工具自己有问题, 后面五组全部不用看了。
if wants g0; then
    echo "=== G0 基线(无损伤) ==="
    run_cell g0 clean "" "" ""
fi

# ================================================================= G3 抖动
# M4.3 自适应水位的**唯一存在证明**。回环上 d≈0, 正确实现和"直接 return 下限"
# 的假实现输出一模一样 —— 只有真抖动能把两者分开。
#
# 要看的是**交换比**: 自适应应当用更少的延迟换到同样的不丢帧, 或者同样的延迟
# 换到更少的丢帧。两个都没改善就是白做, 那结论也要照实写。
if wants g3 && { [[ $HAVE_NETEM -eq 1 ]] || [[ $DRY_RUN -eq 1 ]]; }; then
    echo "=== G3 抖动 x 自适应水位 ==="
    for j in "delay 10ms 5ms" "delay 20ms 10ms" "delay 30ms 15ms"; do
        label="j$(tr -dc '0-9 ' <<< "$j" | awk '{print $1"_"$2}')"
        run_cell g3 "${label}_fixed" "$j" "--jitter-adapt=0" ""
        run_cell g3 "${label}_adapt" "$j" "--jitter-adapt=1" ""
    done
    # 无抖动时的对照: 自适应不该在干净链路上白付延迟
    run_cell g3 "clean_fixed" "" "--jitter-adapt=0" ""
    run_cell g3 "clean_adapt" "" "--jitter-adapt=1" ""
fi

# ================================================================= G4 RTT
# 验证设计阶段的一个预测: 抖动 50ms - 检测延迟 33ms(一个帧周期) = 17ms 重传预算,
# 所以 RTT 过 ~17ms 之后 NACK 应当迅速失效, FEC 接管。
#
# **推翻它比证实它值钱** —— 那说明我们对这条链路的理解有洞。
# 看 nack_recovered 和 fec_recovered 的此消彼长。
if wants g4 && { [[ $HAVE_NETEM -eq 1 ]] || [[ $DRY_RUN -eq 1 ]]; }; then
    echo "=== G4 RTT x NACK 预算 ==="
    for d in 5 15 25; do   # 单向; RTT = 10/30/50ms
        run_cell g4 "rtt$((d * 2))" "delay ${d}ms" "--loss=10" ""
    done
fi

# ================================================================= G1 消融
# 固定 10% 丢包, 逐个关。**这张表是整个 M4 的交付物** ——
# 没有它,"实现了 FEC/NACK/PLI"只是功能列表; 有了它才知道每层各值多少。
if wants g1; then
    echo "=== G1 消融 @10% 丢包 ==="
    run_cell g1 none     "" "--loss=10 $PLI_OFF" "$FEC_OFF $RETX_OFF"
    run_cell g1 fec      "" "--loss=10 $PLI_OFF" "$RETX_OFF"
    run_cell g1 nack     "" "--loss=10 $PLI_OFF" "$FEC_OFF"
    run_cell g1 fec_nack "" "--loss=10 $PLI_OFF" ""
    run_cell g1 all      "" "--loss=10"          ""
fi

# ================================================================= G2 丢包扫描
# 全开, 找到"画面还能看"的那个 X —— README 第一行里的那个数。
#
# 判据**必须现在定死, 不能看完数据再定**:
#     rendered/frames >= 0.98  且  resyncs == 0
if wants g2; then
    echo "=== G2 丢包率扫描(全开) ==="
    for p in 1 3 5 10 20 30; do
        run_cell g2 "loss${p}" "" "--loss=$p" ""
    done
fi

# ================================================================= G5 PLI 风暴
# IDR 约 25 个分片, 0.9^25 = 7.2% —— 10% 丢包下一个 IDR 整帧到达的概率只有 7%。
# 于是 PLI -> IDR -> 大概率又丢 -> 再 PLI, 正反馈。
#
# **限流失效的表现不是崩溃, 是码率翻几倍而画面没有变好** ——
# 只看丢帧数看不出来, 必须同时看 tx_bytes。
if wants g5; then
    echo "=== G5 PLI 正反馈 @30% 丢包 ==="
    run_cell g5 pli_off "" "--loss=30 $PLI_OFF" "--gop=30"
    run_cell g5 pli_1s  "" "--loss=30 --pli-ms=1000" "--gop=30"
    run_cell g5 pli_100 "" "--loss=30 --pli-ms=100"  "--gop=30"
fi

# ================================================================= G6 参考链放大
# 第一轮跑出来的现象: loss5 那格组好了 1000 帧、jitter 只丢了 1 帧,
# 解码器却只吐出 987 帧。缺口 13, 而 gop=30 —— **一帧丢掉连累了约半个 GOP**。
#
# 那是"约等于"不是"测出来"。这一组把 gop 拉成 10/30/60 去验:
# 缺口跟着 gop 走就说明机制是参考链断裂; 不跟着走就说明另有原因,
# 那条结论也就不能写进报告。
#
# 选 loss=5 是因为它是第一轮里缺口最大的那一格(水位停在 10ms 下限,
# 接不住重传往返, 于是偶尔掉一帧)。
if wants g6; then
    echo "=== G6 一帧丢失连累多少帧 (gop 扫描 @5% 丢包) ==="
    for g in 10 30 60; do
        run_cell g6 "gop$g" "" "--loss=5" "--gop=$g"
    done
fi

netem_clear

# ---------------------------------------------------------------- 汇总

[[ $DRY_RUN -eq 1 ]] && { echo; echo "(dry run, 没有真跑)"; exit 0; }

python3 "$ROOT/scripts/m45_summarize.py" "$CSV" > "$OUT/summary.csv" 2>/dev/null \
    && echo && echo "汇总: $OUT/summary.csv" \
    || echo "!! 汇总脚本没跑成, 原始数还在 $CSV"

echo "原始: $CSV"
echo "日志: $LOGDIR"
