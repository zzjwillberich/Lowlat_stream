add_library(lltransport STATIC
    Packet.cpp
    UdpSocket.cpp
)

# 只依赖 llcommon(Status) 和 libc: 协议编解码是纯逻辑, UdpSocket 也只用 BSD socket,
# 都不碰编解码器和 SDL —— 这正是本模块能被单测 100% 覆盖(回环收发即可)的原因,
# 别往这里加重依赖。
target_link_libraries(lltransport PUBLIC llcommon)
