find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

add_library(llencode STATIC
    Encoder.cpp
)

# FFMPEG 用 PRIVATE: Encoder.h 只前向声明了 AVCodecContext 等类型, 没有 include
# 任何 libavcodec 头文件 —— 所以用到 llencode 的 target 不需要 FFmpeg 的 include 路径。
# 这正是前向声明换来的好处, 改成 PUBLIC 就白做了。
target_link_libraries(llencode
    PUBLIC  llcapture
    PRIVATE PkgConfig::FFMPEG
)
