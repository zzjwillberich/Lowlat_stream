/**
 * @file    UdpSocket.cpp
 * @brief   UdpSocket.h 的实现
 * @author  zzj
 * @date    2026-08-14
 */

#include "modules/transport/UdpSocket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

std::string Endpoint::toString() const {
    // TODO(M2): return (ip.empty() ? "*" : ip) + ":" + std::to_string(port);
    return "";
}

UdpSocket::~UdpSocket() {
    // TODO(M2): close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept {
    // TODO(M2): 接管 other.fd_, 并把 other.fd_ 置回 -1。
    //  少了置 -1 这一步, 两个对象都会在析构时 close 同一个 fd:
    //  第二次 close 关掉的很可能是别的线程刚打开的 fd, 现象是毫不相干的模块报 EBADF。
    (void)other;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    // TODO(M2): 步骤:
    //  1. 自赋值检查(&other == this)直接 return *this —— 否则先 close 再接管会关掉自己的 fd;
    //  2. close() 释放本对象当前持有的 fd(否则泄漏);
    //  3. 接管 other.fd_ 并把 other.fd_ 置回 -1。
    (void)other;
    return *this;
}

Status UdpSocket::open() {
    // TODO(M2): 步骤:
    //  1. 已经打开(isOpen())直接返回 InvalidArg —— 覆盖 fd_ 会泄漏原来那个;
    //  2. ::socket(AF_INET, SOCK_DGRAM, 0), 失败返回 NetError(带 strerror(errno));
    //  3. fcntl(F_GETFL) / fcntl(F_SETFL, flags | O_NONBLOCK) 设为非阻塞;
    //     设置失败要先 ::close 掉刚创建的 fd 再返回, 不能半开着走人;
    //  4. 成功后才把 fd 写进 fd_ —— 中途失败时 fd_ 必须保持 -1。
    return Status::error(Code::Internal, "UdpSocket::open is not implemented (M2)");
}

Status UdpSocket::bind(const Endpoint& local) {
    // TODO(M2): 步骤:
    //  1. !isOpen() → Closed;
    //  2. sockaddr_in addr{}; toSockAddr(local, &addr) 失败则原样返回(InvalidArg);
    //     local.ip 为空时用 INADDR_ANY;
    //  3. ::bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0 → NetError, message 里带上
    //     local.toString() 和 strerror(errno) —— "bind 127.0.0.1:9000 failed: Address
    //     already in use" 一眼能看出是哪个端口, 光有 errno 还得回去翻配置。
    (void)local;
    return Status::error(Code::Internal, "UdpSocket::bind is not implemented (M2)");
}

Status UdpSocket::sendTo(const Endpoint& remote, const uint8_t* data, size_t len) {
    // TODO(M2): 步骤:
    //  1. !isOpen() → Closed;
    //  2. data == nullptr || len == 0 → InvalidArg;
    //  3. toSockAddr(remote, &addr);
    //  4. ::sendto(fd_, data, len, 0, ...) 循环重试 EINTR;
    //  5. 返回 -1 → NetError; 返回值 != (ssize_t)len → NetError(UDP 不该有短写)。
    //
    //  注意 EAGAIN: 非阻塞 socket 在发送缓冲写满时会返回它。UDP 上这意味着内核队列
    //  满了(本机瞬时发太快), 此时**丢掉这个包并计数**是对的 —— 阻塞等它腾出空间只会
    //  把延迟堆到后面的帧上, 而低延迟场景宁可丢当前这一包。可以先都当 NetError,
    //  M6 加指标时再把 EAGAIN 单列一条曲线。
    (void)remote;
    (void)data;
    (void)len;
    return Status::error(Code::Internal, "UdpSocket::sendTo is not implemented (M2)");
}

Status UdpSocket::recvFrom(uint8_t* buf, size_t bufLen, size_t& outLen, Endpoint& outFrom,
                           int timeoutMs) {
    // TODO(M2): 步骤:
    //  1. !isOpen() → Closed; buf == nullptr || bufLen == 0 → InvalidArg;
    //  2. waitReadable(timeoutMs), 非 Ok 直接返回(超时就是 Timeout);
    //  3. sockaddr_in from{}; socklen_t fromLen = sizeof(from);
    //     ::recvfrom(fd_, buf, bufLen, MSG_TRUNC, (sockaddr*)&from, &fromLen), EINTR 重试;
    //  4. 返回 -1: EAGAIN/EWOULDBLOCK → Timeout(poll 说可读但数据没了, 见 open() 的注释),
    //     其余 → NetError;
    //  5. **MSG_TRUNC 的返回值是数据报的真实长度, 不是拷进 buf 的长度**:
    //     n > bufLen 说明包被截断了, 返回 NetError, 不要把 outLen 设成 bufLen 蒙混过去;
    //  6. 一切正常再写出参: outLen = n; outFrom = fromSockAddr(&from)。
    //     失败路径上不碰出参 —— 让调用方拿到半个结果比直接失败更难查。
    (void)buf;
    (void)bufLen;
    (void)outLen;
    (void)outFrom;
    (void)timeoutMs;
    return Status::error(Code::Internal, "UdpSocket::recvFrom is not implemented (M2)");
}

Endpoint UdpSocket::localEndpoint() const {
    // TODO(M2): !isOpen() 返回 Endpoint{};
    //  否则 ::getsockname(fd_, ...) 后交给 fromSockAddr。
    //  bind(port = 0) 时端口由内核分配, 只能这样回读。
    return Endpoint{};
}

void UdpSocket::close() {
    // TODO(M2): fd_ >= 0 时 ::close(fd_), 然后 fd_ = -1。
    //  顺序很重要: 先置 -1 再 close 也可以, 但**必须保证再调一次是空操作**,
    //  析构和显式 close 会各来一次。
    //  close 的返回值这里不检查也无法处理, 但不要重试 —— Linux 上无论成败 fd 都已释放,
    //  重试等于关掉别人的 fd。
}

Status UdpSocket::toSockAddr(const Endpoint& ep, void* out) {
    // TODO(M2): 步骤:
    //  1. auto* addr = static_cast<sockaddr_in*>(out); *addr = {};
    //  2. addr->sin_family = AF_INET; addr->sin_port = htons(ep.port);
    //  3. ep.ip 为空 → addr->sin_addr.s_addr = htonl(INADDR_ANY);
    //     否则 inet_pton(AF_INET, ep.ip.c_str(), &addr->sin_addr) != 1 → InvalidArg。
    //
    //  用 inet_pton 而不是 inet_addr: 后者把 "255.255.255.255" 和解析失败都返回
    //  INADDR_NONE, 分不开; 而且 inet_addr 还会接受 "1.2.3" 这种残缺写法。
    (void)ep;
    (void)out;
    return Status::error(Code::Internal, "UdpSocket::toSockAddr is not implemented (M2)");
}

Endpoint UdpSocket::fromSockAddr(const void* addr) {
    // TODO(M2): inet_ntop(AF_INET, &in->sin_addr, ...) 填 ip, ntohs(in->sin_port) 填 port。
    //  不要用 inet_ntoa —— 它返回静态缓冲区, 两个线程同时调会互相覆盖。
    (void)addr;
    return Endpoint{};
}

Status UdpSocket::waitReadable(int timeoutMs) const {
    // TODO(M2): 步骤:
    //  1. pollfd pfd{fd_, POLLIN, 0};
    //  2. ::poll(&pfd, 1, timeoutMs);
    //  3. 返回 0 → Timeout; 返回 -1 且 errno == EINTR → 重试; 其余 -1 → NetError;
    //  4. > 0 → Ok。
    //
    //  EINTR 重试时要**扣掉已经等过的时间**(用 Clock 记开始时刻, 重算剩余 timeout),
    //  否则每来一个信号超时就重新计时, 上层设的 100ms 可能变成永远回不来。
    //  timeoutMs < 0 时无需扣减, 本来就是无限等。
    (void)timeoutMs;
    return Status::error(Code::Internal, "UdpSocket::waitReadable is not implemented (M2)");
}
