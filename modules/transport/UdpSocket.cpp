/**
 * @file    UdpSocket.cpp
 * @brief   UdpSocket.h 的实现
 * @author  zzj
 * @date    2026-08-14
 */

#include "modules/transport/UdpSocket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

#include "common/Clock.h"
#include "common/Status.h"

std::string Endpoint::toString() const {
    return (ip.empty() ? "*" : ip) + ":" + std::to_string(port);
}

Status parseEndpoint(const std::string& text, Endpoint& out) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        return Status::error(Code::InvalidArg,
                             "parseEndpoint: endpoint must have the form ip:port");
    }

    const std::string ip = text.substr(0, colon);
    const std::string portText = text.substr(colon + 1);
    if (portText.empty()) {
        return Status::error(Code::InvalidArg, "parseEndpoint: port must not be empty");
    }

    uint32_t port = 0;
    for (const char ch : portText) {
        if (ch < '0' || ch > '9') {
            return Status::error(Code::InvalidArg,
                                 "parseEndpoint: port must contain only decimal digits");
        }

        port = port * 10 + static_cast<uint32_t>(ch - '0');
        if (port > 65535) {
            return Status::error(Code::InvalidArg,
                                 "parseEndpoint: port must be in the range 1..65535");
        }
    }
    if (port == 0) {
        return Status::error(Code::InvalidArg,
                             "parseEndpoint: port must be in the range 1..65535");
    }

    if (!ip.empty()) {
        in_addr parsedAddress{};
        if (::inet_pton(AF_INET, ip.c_str(), &parsedAddress) != 1) {
            return Status::error(Code::InvalidArg,
                                 "parseEndpoint: invalid IPv4 address: " + ip);
        }
    }

    Endpoint parsed;
    parsed.ip = ip;
    parsed.port = static_cast<uint16_t>(port);
    out = std::move(parsed);
    return Status::ok();
}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (&other != this) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

Status UdpSocket::open() {
    if (isOpen()) {
        return Status::error(Code::InvalidArg, "UdpSocket::open: socket is already open");
    }

    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        return Status::error(Code::NetError,
                             std::string("UdpSocket::open: socket failed: ") +
                                 std::strerror(errno));
    }

    return Status::ok();
}

Status UdpSocket::bind(const Endpoint& local) {
    if (!isOpen()) {
        return Status::error(Code::Closed, "UdpSocket::bind: socket is not open");
    }

    sockaddr_in addr{};
    const Status st = toSockAddr(local, &addr);
    if (!st.isOk()) {
        return st;
    }

    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        return Status::error(Code::NetError,
                             "UdpSocket::bind: bind " + local.toString() + " failed: " +
                                 std::strerror(errno));
    }

    return Status::ok();
}

Status UdpSocket::sendTo(const Endpoint& remote, const uint8_t* data, size_t len) {
    if (!isOpen()) {
        return Status::error(Code::Closed, "UdpSocket::sendTo: socket is not open");
    }

    if (remote.ip.empty() || remote.port == 0) {
        return Status::error(Code::InvalidArg,
                             "UdpSocket::sendTo: remote IP must not be empty and port must be positive");
    }

    if (data == nullptr || len == 0) {
        return Status::error(Code::InvalidArg,
                             "UdpSocket::sendTo: data must not be null and len must be positive");
    }

    sockaddr_in addr{};
    const Status st = toSockAddr(remote, &addr);
    if (!st.isOk()) {
        return st;
    }

    while (true) {
        const ssize_t res = ::sendto(fd_, data, len, 0,
                                     reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Status::error(Code::NetError,
                                 "UdpSocket::sendTo: sendto " + remote.toString() +
                                     " failed: " + std::strerror(errno));
        }
        if (res != static_cast<ssize_t>(len)) {
            return Status::error(Code::NetError,
                                 "UdpSocket::sendTo: sendto returned a short datagram write");
        }

        break;
    }

    return Status::ok();
}

Status UdpSocket::recvFrom(uint8_t* buf, size_t bufLen, size_t& outLen, Endpoint& outFrom, int timeoutMs) {
    if (!isOpen()) {
        return Status::error(Code::Closed, "UdpSocket::recvFrom: socket is not open");
    }

    if (buf == nullptr || bufLen == 0) {
        return Status::error(Code::InvalidArg,
                             "UdpSocket::recvFrom: buf must not be null and bufLen must be positive");
    }

    const Status st = waitReadable(timeoutMs);
    if (!st.isOk()) {
        return st;
    }

    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);

    while (true) {
        const ssize_t res = ::recvfrom(fd_, buf, bufLen, MSG_TRUNC,
                                       reinterpret_cast<sockaddr*>(&from), &fromLen);

        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return Status::error(Code::Timeout,
                                     "UdpSocket::recvFrom: no datagram available after readiness");
            }

            return Status::error(Code::NetError,
                                 std::string("UdpSocket::recvFrom: recvfrom failed: ") +
                                     std::strerror(errno));
        }

        if (res > static_cast<ssize_t>(bufLen)) {
            return Status::error(Code::NetError,
                                 "UdpSocket::recvFrom: datagram exceeds receive buffer");
        }

        outLen = static_cast<size_t>(res);
        break;
    }

    outFrom = fromSockAddr(&from);
    return Status::ok();
}

Endpoint UdpSocket::localEndpoint() const {
    if (!isOpen()) {
        return Endpoint{};
    }

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const int res = ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (res < 0) {
        return Endpoint{};
    }

    return fromSockAddr(&addr);
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Status UdpSocket::toSockAddr(const Endpoint& ep, void* out) {
    auto* addr = static_cast<sockaddr_in*>(out);
    *addr = {};

    addr->sin_family = AF_INET;
    addr->sin_port = htons(ep.port);
    if (ep.ip.empty()) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        const int res = ::inet_pton(AF_INET, ep.ip.c_str(), &addr->sin_addr);
        if (res == 0) {
            return Status::error(Code::InvalidArg,
                                 "UdpSocket::toSockAddr: invalid IPv4 address: " + ep.ip);
        }
        if (res < 0) {
            return Status::error(Code::NetError,
                                 std::string("UdpSocket::toSockAddr: inet_pton failed: ") +
                                     std::strerror(errno));
        }
    }

    return Status::ok();
}

Endpoint UdpSocket::fromSockAddr(const void* addr) {
    if (addr == nullptr) {
        return Endpoint{};
    }

    Endpoint ep;
    const auto* in = static_cast<const sockaddr_in*>(addr);
    char ip[INET_ADDRSTRLEN]{};

    const char* res = ::inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    if (res == nullptr) {
        return ep;
    }

    ep.ip = res;
    ep.port = ntohs(in->sin_port);

    return ep;
}

Status UdpSocket::waitReadable(int timeoutMs) const {
    pollfd pfd{fd_, POLLIN, 0};
    const uint64_t deadline =
        timeoutMs >= 0 ? steadyNowMs() + static_cast<uint64_t>(timeoutMs) : 0;
    int remainingMs = timeoutMs;

    while (true) {
        const int res = ::poll(&pfd, 1, remainingMs);
        if (res == 0) {
            return Status::error(Code::Timeout, "UdpSocket::waitReadable: receive timed out");
        }
        if (res < 0) {
            if (errno == EINTR) {
                if (timeoutMs < 0) {
                    continue;
                }

                const uint64_t now = steadyNowMs();
                if (now >= deadline) {
                    return Status::error(Code::Timeout,
                                         "UdpSocket::waitReadable: receive timed out");
                }

                remainingMs = static_cast<int>(deadline - now);
                continue;
            }

            return Status::error(Code::NetError,
                                 std::string("UdpSocket::waitReadable: poll failed: ") +
                                     std::strerror(errno));
        }

        break;
    }

    return Status::ok();
}
