add_library(llcapture STATIC
    ISource.cpp
    NullSource.cpp
)

# PUBLIC: 用到 llcapture 的 target 必然会 include Frame.h/ISource.h,
# 而它们又 include common/Status.h —— include 路径和 pthread 由 llcommon 一路传上来
target_link_libraries(llcapture PUBLIC llcommon)
