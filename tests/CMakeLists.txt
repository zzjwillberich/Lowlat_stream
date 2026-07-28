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
function(add_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE gtest_main llcommon)
    # 阻塞类用例万一没被唤醒, future 析构会永久阻塞;
    # 这里兜一层硬超时, 让挂死表现为"测试失败"而不是"整条流水线卡住"
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 30)
endfunction()

add_unit_test(test_logger)          # 日志线程安全
add_unit_test(test_config)          # 配置解析
add_unit_test(test_bounded_queue)   # 有界队列
add_unit_test(test_frame)           # 帧类型与采集接口契约
