find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG_DEC REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

add_library(lldecode STATIC
    Decoder.cpp
)

# llcapture 走 PUBLIC: Decoder.h include 了 Frame.h, 用到解码器的 target 必然要它。
#
# 输出复用 RawFrame 而不是另起一个 DecodedFrame: 两者字段完全一样, 分成两个类型
# 的唯一后果是渲染器要写两遍、或者中间加一次无谓的转换。代价是这里得链 llcapture,
# 名字上看着有点怪 —— 真觉得别扭了, 把 Frame.h 挪进 common/ 是个十分钟的机械改动
# (它只依赖 Status.h), 但现在不值得为改名字去动一堆 include。
#
# FFmpeg 走 PRIVATE, 同 llencode: Decoder.h 只前向声明了 AVCodecContext 等类型。
target_link_libraries(lldecode
    PUBLIC  llcapture
    PRIVATE PkgConfig::FFMPEG_DEC
)
