/**
 * @file    test_logger.cpp
 * @brief   验证 Logger 多线程写入不会产生交错撕裂的行
 * @author  zzj
 * @date    2026-07-23
 */
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <regex>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "common/Logger.h"

TEST(LoggerThreadSafety, NoInterleavedLines) {
    // ---------- 1. 重定向 stderr 到临时文件 ----------
    const char* path = "/tmp/logger_test.log";
    FILE* tmp = std::fopen(path, "w");
    ASSERT_NE(tmp, nullptr);

    int saved = dup(STDERR_FILENO);
    dup2(fileno(tmp), STDERR_FILENO);
    std::fclose(tmp);

    // ---------- 2. 起 4 个线程各打 1000 条 ----------
    Logger::instance().setLevel(LogLevel::TRACE);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < 1000; ++i) {
                LOG_INFO("test", "thread %d msg %d", t, i);
            }
        });
    }
    for (auto& th : threads) th.join();

    // ---------- 3. 恢复 stderr ----------
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    // ---------- 4. 读取并逐行验证 ----------
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open());

    // 合法的日志行格式：
    // [2026-07-23 17:48:01.998][INFO ][test] thread 0 msg 0
    // 级别一律占 5 字符（INFO/WARN 右补一个空格），这里把对齐规则一并钉死，
    // 以后谁改了 levelStr 的补白，这条用例会直接报出来
    std::regex linePattern(
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\]\[(TRACE|DEBUG|INFO |WARN |ERROR)\]\[test\] .+)"
    );

    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        count++;
        // 若 mutex 锁不住，两线程输出互相插入，整行格式必被破坏
        EXPECT_TRUE(std::regex_match(line, linePattern))
            << "Interleaved line #" << count << ": " << line;
    }
    EXPECT_EQ(count, 4000);

    std::remove(path);
}
