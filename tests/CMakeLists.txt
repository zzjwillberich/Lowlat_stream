# ---------- GoogleTest ----------
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
FetchContent_MakeAvailable(googletest)

# ---------- 日志线程安全测试 ----------
add_executable(test_logger
    test_logger.cpp
    ../common/Logger.cpp
)
target_include_directories(test_logger PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_logger PRIVATE gtest_main pthread)

include(GoogleTest)
gtest_discover_tests(test_logger)
