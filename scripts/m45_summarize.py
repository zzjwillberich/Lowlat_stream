#!/usr/bin/env python3
"""
m45_summarize.py — 把 m45_bench.sh 的原始 runs.csv 按格子聚合。

用法: python3 scripts/m45_summarize.py bench/<戳>/runs.csv > summary.csv

聚合规则:
  - 同一 (group, cell) 的多个种子取**平均**, 并给出 min/max 让离散度可见。
  - 派生指标在这里算, 不在 shell 里算 —— 三条读数纪律都在这一层落实。

三条纪律(和 m45_bench.sh 顶上那段一致):
  1. 丢包用 lost_exact + nack_pending, 不用 lost。
     lost 是 "seq 跨度 - 组包器实收", FEC 包占 seq 空间却被组包器拒收,
     每 K 个包永久多算 1 个; 而流结束时最后几个缺口卡在 pending 里,
     只看 lost_exact 会漏报。两个加起来才是这条流真正没拿到的包数。
  2. FEC 带宽从发送端读 (tx_fec_bytes / tx_bytes)。
  3. 每格多种子; 这里把 min/max 一起打出来, 三个种子散得厉害就说明
     样本量还不够, 那一格的均值不能当结论用。
"""
import csv
import sys
from collections import OrderedDict


def num(s):
    if s is None or s == "":
        return None
    try:
        return float(s)
    except ValueError:
        return None


def ratio(a, b):
    """a/b; 分母为 0 或缺失时返回空串 —— 不返回 0。

    「没有这个数」和「这个数是 0」必须能分开: 前者是测量没拿到,
    后者是真的一个都没有。混成 0 会让一次失败的运行看起来像完美的运行。
    """
    if a is None or b is None or b == 0:
        return ""
    return "%.4f" % (a / b)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("用法: m45_summarize.py <runs.csv>\n")
        return 2

    with open(sys.argv[1], newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.stderr.write("runs.csv 是空的\n")
        return 1

    cells = OrderedDict()
    for r in rows:
        cells.setdefault((r["group"], r["cell"]), []).append(r)

    out = csv.writer(sys.stdout)
    out.writerow([
        "group", "cell", "seeds", "netem", "loss%", "adapt", "fec_group", "retx_ms", "pli_ms",
        # 首要判据
        "rendered", "rendered_min", "rendered_max", "render_ratio",
        "resyncs",
        # 真实丢包 (纪律 1)
        "真丢包", "真丢包率", "注入丢包", "注入率",
        # 各层战果
        "nack_recovered", "nack_gaveup", "fec_recovered", "fec_unrecoverable",
        "pli_sent", "pli_suppressed",
        # 带宽 (纪律 2)
        "encoded_bytes", "fec/encoded", "上线包数",
        # 延迟与水位
        "p50", "p95", "p99", "max", "水位", "水位峰值",
    ])

    for (group, cell), rs in cells.items():
        n = len(rs)

        def avg(key):
            vals = [num(r.get(key)) for r in rs]
            vals = [v for v in vals if v is not None]
            return sum(vals) / len(vals) if vals else None

        def lo(key):
            vals = [num(r.get(key)) for r in rs if num(r.get(key)) is not None]
            return min(vals) if vals else None

        def hi(key):
            vals = [num(r.get(key)) for r in rs if num(r.get(key)) is not None]
            return max(vals) if vals else None

        def fmt(v, nd=1):
            return "" if v is None else ("%.*f" % (nd, v)).rstrip("0").rstrip(".")

        rendered = avg("rx_rendered")
        captured = avg("tx_captured")
        arrived = avg("rx_packets")
        injected = avg("rx_injected_drops")

        # 上线的包数 = 数据包 + FEC 包 + 重传包。
        # tx_packets_sent **只数数据包** —— FEC 走 fec_packets, 重传走 retransmitted,
        # 三个计数器互不重叠(见 SenderPipeline::sendLoop)。
        # 用它单独当分母的话, 开了 FEC 的格子注入率会虚高 25%(K=4 时),
        # 于是"同样的 --loss=10, 开 FEC 反而丢得更多"—— 一个纯粹由分母造出来的假象。
        wire_packets = None
        parts = [avg("tx_packets_sent"), avg("tx_fec_packets"), avg("tx_retransmitted")]
        if parts[0] is not None:
            wire_packets = sum(p for p in parts if p is not None)
        packets_sent = wire_packets

        # 纪律 1: 真丢包 = 已放弃 + 还挂着的
        lost_exact = avg("rx_lost_exact")
        pending = avg("rx_nack_pending")
        real_lost = None
        if lost_exact is not None and pending is not None:
            real_lost = lost_exact + pending

        # 分母用发送端真正上线的包数: 接收端的 rx_packets 已经被丢包和 FEC 拒收改过了
        out.writerow([
            group, cell, n, rs[0].get("netem", ""), rs[0].get("loss", ""),
            rs[0].get("adapt", ""), rs[0].get("fec_group", ""), rs[0].get("retx_ms", ""),
            rs[0].get("pli_ms", ""),

            fmt(rendered), fmt(lo("rx_rendered")), fmt(hi("rx_rendered")),
            ratio(rendered, captured),
            fmt(avg("rx_resyncs")),

            fmt(real_lost), ratio(real_lost, packets_sent),
            fmt(injected), ratio(injected, packets_sent),

            fmt(avg("rx_nack_recovered")), fmt(avg("rx_nack_gaveup")),
            fmt(avg("rx_fec_recovered")), fmt(avg("rx_fec_unrecoverable")),
            fmt(avg("rx_pli_sent")), fmt(avg("rx_pli_suppressed")),

            fmt(avg("tx_bytes"), 0), ratio(avg("tx_fec_bytes"), avg("tx_bytes")),
            fmt(wire_packets, 0),

            fmt(avg("lat_p50")), fmt(avg("lat_p95")), fmt(avg("lat_p99")),
            fmt(avg("lat_max")),
            fmt(avg("rx_jitter_delay")), fmt(avg("rx_jitter_peak")),
        ])

        # arrived 目前只用于下面这条自检, 留着是因为它是唯一能交叉验证
        # "注入的 + 收到的 ≈ 发出的" 的量
        if group == "g0" and injected and injected > 0:
            sys.stderr.write(
                "!! G0 是无损伤基线, 但 injected_drops=%s —— 测量工具自己有问题, "
                "后面几组不用看了\n" % fmt(injected))
        if group == "g0" and arrived is not None and packets_sent is not None:
            if arrived < packets_sent * 0.999:
                sys.stderr.write(
                    "!! G0 无损伤却掉了包: 发 %s 收 %s —— 先查这个, "
                    "回环上不该丢\n" % (fmt(packets_sent, 0), fmt(arrived, 0)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
