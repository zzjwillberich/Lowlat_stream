add_library(lltransport STATIC
    Packet.cpp
)

# 只依赖 llcommon(Status): 协议编解码是纯逻辑, 不碰 socket 也不碰编解码器 ——
# 这正是它能被单测 100% 覆盖的原因, 别往这里加重依赖。
target_link_libraries(lltransport PUBLIC llcommon)
