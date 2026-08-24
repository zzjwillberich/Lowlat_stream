add_library(llreceiver STATIC
    ReceiverPipeline.cpp
)
# lldecode / llrender 走 PUBLIC: ReceiverPipeline.h 里有 Decoder 和 IRenderer 成员,
# 用到这个管线的 target(main.cpp、单测)必然要它们的头文件。
# SDL 和 FFmpeg 本身仍然是 PRIVATE 的 —— 那两层已经在各自的 CMakeLists 里挡住了,
# 这里链进来的只是接口, 不会把 #define main SDL_main 传染出去。
target_link_libraries(llreceiver PUBLIC lltransport lldecode llrender)

add_executable(lowlat_receiver main.cpp)
target_link_libraries(lowlat_receiver PRIVATE llreceiver)
