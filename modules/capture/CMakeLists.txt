find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale libavutil)

add_library(llcapture STATIC
    ISource.cpp
    NullSource.cpp
    V4l2Source.cpp
)

# PUBLIC: 用到 llcapture 的 target 必然会 include Frame.h/ISource.h,
# 而它们又 include common/Status.h —— include 路径和 pthread 由 llcommon 一路传上来
target_link_libraries(llcapture
    PUBLIC  llcommon
    PRIVATE PkgConfig::SWSCALE
)
