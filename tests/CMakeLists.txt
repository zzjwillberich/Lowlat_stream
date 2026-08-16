# ---------- GoogleTest ----------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# 测试目标统一链接 llcommon, 不再逐个编译 ../common/*.cpp:
# 否则 common 每加一个源文件都得同步改这里, 迟早漏。
# include 路径和 pthread 由 llcommon 以 PUBLIC 方式传递过来, 无需重复声明。
# 额外的库写在名字后面: add_unit_test(test_xxx llcapture)
function(add_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE gtest_main llcommon ${ARGN})
    # 阻塞类用例万一没被唤醒, future 析构会永久阻塞;
    # 这里兜一层硬超时, 让挂死表现为"测试失败"而不是"整条流水线卡住"
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 30)
endfunction()

add_unit_test(test_logger)          # 日志线程安全
add_unit_test(test_config)          # 配置解析
add_unit_test(test_bounded_queue)   # 有界队列
add_unit_test(test_frame)                      # 帧类型与采集接口契约
add_unit_test(test_null_source llcapture)      # 合成画面源
add_unit_test(test_v4l2_source llcapture)      # V4L2 无硬件契约 + 可选真机冒烟
add_unit_test(test_encoder llencode)           # H.264 编码器
add_unit_test(test_packet lltransport)         # 协议包头编解码
add_unit_test(test_udp_socket lltransport)     # UDP 收发(回环)
add_unit_test(test_packetizer lltransport)     # 帧分片打包
add_unit_test(test_frame_assembler lltransport)  # 接收端组包与丢包统计
add_unit_test(test_sender_pipeline llsender)   # M1.4 采集 -> 队列 -> 编码管线
