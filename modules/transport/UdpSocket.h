/**
 * @file    UdpSocket.h
 * @brief   UDP 收发的 RAII 封装, 见 docs/PROTOCOL.md
 * @author  zzj
 * @date    2026-08-14
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/Status.h"

/**
 * 一个 UDP 端点(IP + 端口)。
 *
 * @note 用字符串存 IP 而不是 `sockaddr_in`: 这一层是给配置和日志看的, 别把 BSD socket
 *          的类型漏到模块接口上。转换成 `sockaddr_in` 是 UdpSocket 内部的事。
 */
struct Endpoint {
    /** @brief 点分十进制 IPv4, 例 "127.0.0.1"; 空串在 bind 时表示 INADDR_ANY */
    std::string ip;

    /** @brief 端口, bind 时传 0 表示"让内核分配", 之后用 localEndpoint() 取回实际值 */
    uint16_t port = 0;

    /** @brief 拼成 "ip:port" 供日志使用 */
    std::string toString() const;
};

/**
 * UDP 套接字的 RAII 封装。
 *
 * 发送端: `open()` 后直接 `sendTo()`, 内核自动分配源端口。
 * 接收端: `open()` → `bind()` → 循环 `recvFrom()`。
 *
 * **错误码分工**(和 Packet 的约定一致, 依据是"调用方会做出不同反应"):
 * - `Closed`     还没 open 就调用 —— 生命周期用错了, 改代码;
 * - `InvalidArg` 参数不合法(空指针、长度为 0、IP 解析失败) —— 本端的问题, 改代码;
 * - `NetError`   syscall 返回 -1 —— 对端/网络/系统的问题, 丢包计数后继续收;
 * - `Timeout`    等待期内没有数据 —— **正常情况**, 收包循环据此检查退出标志。
 *
 * 规则从简: 凡 syscall 失败一律 `NetError`, 具体 errno 进 message。不按 errno 细分,
 * 因为调用方对 ECONNREFUSED 和 ENETUNREACH 的反应是同一个 —— 记一笔然后继续。
 *
 * @note 本类不是线程安全的。一个实例只由一个线程使用; 需要同时收发就开两个 socket,
 *          不要给 fd 加锁 —— 加锁会让发送阻塞住收包线程, 正好毁掉低延迟。
 * @note close() 必须幂等, 析构函数会调用它。可移动不可拷贝: 拷贝一个 fd 会导致
 *          double close, 而 double close 在多线程下会**关掉别人刚打开的 fd**,
 *          表现为毫不相干的模块随机报 EBADF。
 * @note 不要顺手打开 `SO_REUSEADDR`: UDP 上它可能让两个套接字绑同一端口, 结果是
 *          数据报被内核在两者之间**静默瓜分**, 现象是"丢了一半包"却查不出丢在哪。
 */
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /** @note 移动后源对象必须回到"未打开"状态(fd 置 -1), 否则析构时双重关闭 */
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /**
     * @brief 创建 UDP 套接字并设为非阻塞
     *
     * @return Ok       套接字可用
     *  InvalidArg 已经打开过(不允许覆盖 fd, 那会泄漏)
     *  NetError   socket() 失败
     *
     * @note 设为非阻塞不是为了轮询, 而是为了兜住一个陷阱: poll 报告可读之后,
     *          recvfrom **仍可能阻塞**(例如该数据报校验和错误被内核丢弃)。
     *          阻塞式 socket 在这种情况下会把收包线程永久卡住。
     * @note 用 socket() 的 SOCK_NONBLOCK|SOCK_CLOEXEC 一次建好, 不走事后 fcntl:
     *          分两步中间有窗口, 而且 CLOEXEC 一旦漏设, fork/exec 出去的子进程会
     *          继承这个 fd —— 端口在本进程 close 之后仍被占着, 现象是重启服务报
     *          "Address already in use", 查半天查不到是谁占的。
     */
    Status open();

    /**
     * @brief 绑定本地端点(接收端/服务端需要, 发送端不需要)
     *
     * @param local 本地地址; ip 为空表示监听所有网卡, port 为 0 表示由内核分配
     *
     * @return Ok       绑定成功
     *  Closed     尚未 open()
     *  InvalidArg ip 非法, 无法解析成 IPv4
     *  NetError   bind() 失败(端口被占用等)
     */
    Status bind(const Endpoint& local);

    /**
     * @brief 发送一个数据报
     *
     * @param remote 目的端点
     * @param data   载荷首地址
     * @param len    载荷长度
     *
     * @return Ok       整个数据报已交给内核
     *  Closed     尚未 open()
     *  InvalidArg data 为空, len 为 0, 或 remote 的 ip/port 非法
     *  NetError   sendto() 失败, 或返回的字节数不等于 len
     *
     * @note UDP 的数据报是原子的: 要么整个进内核缓冲, 要么失败, **不存在部分发送**。
     *          所以这里没有出参 sentLen —— 真出现短写就是异常, 直接报 NetError。
     * @note 上层必须保证 len 不超过 MAX_DATA_PACKET_SIZE。这里不引用那个常量,
     *          是为了不让传输层的 socket 封装反过来依赖协议定义 —— 超了会由内核
     *          返回 EMSGSIZE, 一样能发现。
     */
    Status sendTo(const Endpoint& remote, const uint8_t* data, size_t len);

    /**
     * @brief 收一个数据报, 最多等待 timeoutMs
     *
     * @param buf       输出缓冲区
     * @param bufLen    缓冲区容量, 应 >= MAX_DATA_PACKET_SIZE
     * @param outLen    出参, 实际收到的字节数
     * @param outFrom   出参, 发送方端点(NACK/PLI 要靠它回包)
     * @param timeoutMs > 0 最多等这么久; 0 立即返回; < 0 无限等待
     *
     * @return Ok       收到一个完整数据报
     *  Closed     尚未 open()
     *  InvalidArg buf 为空或 bufLen 为 0
     *  Timeout    等待期内无数据 —— 不是错误, 不要打 ERROR 日志
     *  NetError   poll/recvfrom 失败, 或数据报长度超过 bufLen
     *
     * @note 数据报大于 bufLen 时必须报 NetError 而不是悄悄截断: 截断后的包会通过
     *          包头校验, 却少了尾部载荷, 最终表现为"某些帧解码花屏"——从解码器一路
     *          倒查回来极费时间。用 recvfrom 的 MSG_TRUNC 拿到真实长度来判断。
     * @note timeoutMs < 0 只适合一次性工具。收包线程必须给有限超时, 否则停止时
     *          线程卡在 poll 里, join 永远回不来。
     */
    Status recvFrom(uint8_t* buf, size_t bufLen, size_t& outLen, Endpoint& outFrom,
                    int timeoutMs);

    /**
     * @brief 取本地实际绑定的端点
     *
     * @return 已绑定则为实际地址; 未打开返回默认值
     *
     * @note bind 时传 port = 0, 实际端口由内核决定 —— 和摄像头协商分辨率是同一件事:
     *          **请求值不是承诺值**, 拿到手之后必须回读实际值(参考 ISource::actualConfig)。
     *          单元测试也靠它避免端口写死后在 CI 上撞车。
     */
    Endpoint localEndpoint() const;

    /** @brief 关闭并置为未打开状态; 必须幂等, 关闭后可以重新 open() */
    void close();

    bool isOpen() const { return fd_ >= 0; }

    /**
     * @brief 裸 fd, 供 M5 服务端塞进 epoll
     *
     * @note 只读用途。谁拿到都不许 close 它 —— 所有权始终在本对象手里。
     */
    int fd() const { return fd_; }

private:
    /** @brief 把 Endpoint 转成 sockaddr_in; ip 非法返回 InvalidArg */
    static Status toSockAddr(const Endpoint& ep, void* out);

    /** @brief 把 sockaddr_in 转回 Endpoint */
    static Endpoint fromSockAddr(const void* addr);

    /** @brief poll 等待可读; EINTR 重试并扣除已等待的时间, 超时返回 Code::Timeout */
    Status waitReadable(int timeoutMs) const;

    int fd_ = -1;
};
