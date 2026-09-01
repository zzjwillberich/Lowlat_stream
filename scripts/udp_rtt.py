#!/usr/bin/env python3
"""
udp_rtt.py — 量一次 127.0.0.1 上的 UDP 往返，毫秒，打印中位数。

用途：给 m45_bench.sh 当 netem 的生效探针。

**为什么不能用 ping**：M4.5 第三轮栽在这里。WSL2 的 mirrored 网络模式下，
`tc qdisc ... dev lo root netem delay 10ms` 挂上之后 **ping 确实变慢了**，
而两个进程之间的 UDP 一点没被延迟——ICMP 和 UDP 走的不是同一条路。
于是探针通过、每一格的检查也通过，g3/g4 十四格全跑完，
数据和不挂 netem 时一字不差。

探针必须和被测流量**用同一种协议、同一个地址族**，否则它证明的是别的事。
"""
import socket
import sys
import time


def main():
    # 21 个而不是 7 个: netem 的 jitter 是**均匀分布**, 7 个样本的中位数抖得厉害。
    # 第四轮就因此误杀了三格 —— delay 20ms 10ms 是 [10,30] 上的均匀分布,
    # 7 个样本的中位数实测 15.3, 而判据要求 >= 20。判据设计错了, 不是 netem 的问题。
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 21

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(2.0)
    addr = sock.getsockname()

    payload = b"x" * 512  # 和真实分片一个量级, 免得撞上按长度分类的规则

    samples = []
    for _ in range(count):
        start = time.perf_counter()
        try:
            sock.sendto(payload, addr)   # 发给自己: 出栈一次、入栈一次
            sock.recv(2048)
        except (socket.timeout, OSError):
            continue
        samples.append((time.perf_counter() - start) * 1000.0)

    sock.close()

    if not samples:
        print("-1")
        return 1

    samples.sort()
    print("%.3f" % samples[len(samples) // 2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
