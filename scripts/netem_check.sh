#!/usr/bin/env bash
#
# netem_check.sh — 20 秒内回答"这台机器上能不能给回环 UDP 加延迟"
#
# 用法: ./scripts/netem_check.sh
#
# 为什么单独一个脚本: m45_bench.sh 跑一趟 g3/g4 要 7 分钟, 而"netem 到底生不生效"
# 这个问题值得先花 20 秒问清楚。第二、三轮各浪费了一次完整运行,
# 拿回来的是和不挂 netem 一模一样的数据。
#
# 它同时量 **UDP** 和 **ICMP**, 因为这两个可能不一样:
# WSL2 的 mirrored 模式下挂 netem 到 lo, ping 会变慢而进程间的 UDP 不会。
# **判据是 UDP** —— 那才是被测流量用的协议。

set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DELAY_MS=50

udp_ms()  { python3 "$ROOT/scripts/udp_rtt.py" 2>/dev/null || echo -1; }
icmp_ms() {
    ping -c 3 -i 0.2 -W 1 127.0.0.1 2>/dev/null | tail -1 |
        awk -F'[/ ]' '/=/ {for (i = 1; i <= NF; i++)
                              if ($i ~ /^[0-9]+\.[0-9]+$/) { print $(i + 1); exit }}'
}

command -v tc >/dev/null 2>&1 || { echo "找不到 tc (apt install iproute2)" >&2; exit 1; }
sudo -v || exit 1

printf '基线(没挂 netem):  UDP %sms   ICMP %sms\n\n' "$(udp_ms)" "$(icmp_ms)"
printf '%-12s %-12s %-12s %s\n' 设备 UDP ICMP 结论

WINNER=""
for dev in $(ip -br link 2>/dev/null | awk '{print $1}' | grep -E '^(lo|loopback)'); do
    if ! sudo tc qdisc replace dev "$dev" root netem delay ${DELAY_MS}ms 2>/dev/null; then
        printf '%-12s %-12s %-12s %s\n' "$dev" - - "挂不上"
        continue
    fi
    u="$(udp_ms)"; i="$(icmp_ms)"
    sudo tc qdisc del dev "$dev" root 2>/dev/null

    if awk -v r="${u:-0}" -v w="$DELAY_MS" 'BEGIN{exit !(r+0 >= w+0)}'; then
        verdict="✅ UDP 被延迟了 —— 用这个"
        [[ -z "$WINNER" ]] && WINNER="$dev"
    elif awk -v r="${i:-0}" -v w="$DELAY_MS" 'BEGIN{exit !(r+0 >= w+0)}'; then
        verdict="❌ 只有 ICMP 慢了, UDP 没慢 —— 假阳性"
    else
        verdict="❌ 都没慢"
    fi
    printf '%-12s %-12s %-12s %s\n' "$dev" "${u:-?}" "${i:-?}" "$verdict"
done

echo
if [[ -n "$WINNER" ]]; then
    echo "结论: 在 $WINNER 上挂 netem 有效, g3/g4 可以跑。"
else
    cat <<'MSG'
结论: 没有一个回环设备能让 UDP 变慢。

这台机器上要拿到真延迟, 见 docs/测试记录.md「待决 · 抖动链路怎么造」:
  甲 veth + netns   乙 .wslconfig 切回 NAT   丙 不测, 报告写"未验证"
MSG
fi
