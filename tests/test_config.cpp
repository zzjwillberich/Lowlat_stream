/**
 * @file    test_config.cpp
 * @brief   Config 单元测试 — 覆盖 M0.4 §6.2 全部用例
 * @author  zzj
 * @date    2026-07-25
 */
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include "common/Config.h"

// ========== 基本格式解析 ==========

TEST(Config, BasicKeyEqualsValue) {
    const char* argv[] = {"prog", "--target=1.2.3.4:9000"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(2, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("target"), "1.2.3.4:9000");
}

TEST(Config, BasicKeySpaceValue) {
    const char* argv[] = {"prog", "--target", "1.2.3.4:9000"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("target"), "1.2.3.4:9000");
}

TEST(Config, MixedFormat) {
    // --listen 用 = 格式, --target 用空格格式
    const char* argv[] = {"prog", "--listen=0.0.0.0:9001", "--target", "1.2.3.4:9000"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(4, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen"), "0.0.0.0:9001");
    EXPECT_EQ(cfg.get("target"), "1.2.3.4:9000");
}

// ========== 默认值 ==========

TEST(Config, DefaultValue) {
    const char* argv[] = {"prog"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(1, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen", "0.0.0.0:9000"), "0.0.0.0:9000");
}

TEST(Config, CommandLineOverridesDefault) {
    const char* argv[] = {"prog", "--listen=1.2.3.4:8000"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(2, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen", "0.0.0.0:9000"), "1.2.3.4:8000");
}

// ========== 多个相同 key ==========

TEST(Config, DuplicateKeyLastWins) {
    const char* argv[] = {"prog", "--listen", "0.0.0.0:9000", "--listen", "0.0.0.0:9001"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(5, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen"), "0.0.0.0:9001");  // 后者覆盖前者
}

// ========== -- 终止选项解析 ==========

TEST(Config, DashDashTerminatesOptions) {
    const char* argv[] = {"prog", "--listen", "8080", "--", "--target=1.2.3.4:9000"};
    Config cfg;
    // -- 后面的 --target 不应该被解析
    ASSERT_TRUE(cfg.parse(5, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen"), "8080");
    // target 没有被解析（-- 后面的参数被忽略）
    EXPECT_EQ(cfg.get("target", "default"), "default");
}

// ========== 未知参数检测 ==========

TEST(Config, UnknownArgFails) {
    const char* argv[] = {"prog", "--unknown-flag=123"};
    Config cfg;
    EXPECT_FALSE(cfg.parse(2, const_cast<char**>(argv)));
}

TEST(Config, KeysMethodReturnsAllKeys) {
    const char* argv[] = {"prog", "--log-level=debug", "--listen=0.0.0.0:9000"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));

    auto keys = cfg.keys();
    EXPECT_EQ(keys.size(), 2u);
    EXPECT_TRUE(keys.count("log-level"));
    EXPECT_TRUE(keys.count("listen"));
    EXPECT_FALSE(keys.count("target"));
}

// ========== getInt ==========

TEST(Config, GetIntValid) {
    const char* path = "/tmp/test_lowlat_port.conf";
    {
        std::ofstream f(path);
        f << "port=8080\n";
    }
    const char* argv[] = {"prog", "--config", path};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.getInt("port", 80), 8080);
    std::remove(path);
}

TEST(Config, GetIntInvalidFallsBack) {
    const char* path = "/tmp/test_lowlat_invalid_port.conf";
    {
        std::ofstream f(path);
        f << "port=abc\n";
    }
    const char* argv[] = {"prog", "--config", path};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.getInt("port", 80), 80);  // 无法转 int, 用默认值
    std::remove(path);
}

// ========== 配置文件 ==========

TEST(Config, LoadConfigFile) {
    const char* path = "/tmp/test_lowlat.conf";
    {
        std::ofstream f(path);
        f << "# comment\n";
        f << "log-level = debug\n";
        f << "target = 10.0.0.1:9000\n";
    }

    const char* argv[] = {"prog", "--config", path};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("log-level"), "debug");
    EXPECT_EQ(cfg.get("target"), "10.0.0.1:9000");

    std::remove(path);
}

TEST(Config, CommandLineOverridesConfigFile) {
    const char* path = "/tmp/test_lowlat2.conf";
    {
        std::ofstream f(path);
        f << "log-level = info\n";
        f << "listen = 0.0.0.0:8000\n";
    }

    // --config 在前, --log-level 在后 → 命令行覆盖配置文件
    const char* argv[] = {"prog", "--config", path, "--log-level=trace"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(4, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("log-level"), "trace");          // 被命令行覆盖
    EXPECT_EQ(cfg.get("listen"), "0.0.0.0:8000");      // 配置文件的值留下来了

    std::remove(path);
}

TEST(Config, ConfigFileNotFound) {
    const char* argv[] = {"prog", "--config", "/tmp/does_not_exist.conf"};
    Config cfg;
    EXPECT_FALSE(cfg.parse(3, const_cast<char**>(argv)));
}

TEST(Config, ConfigFileCommentsAndEmptyLines) {
    const char* path = "/tmp/test_lowlat3.conf";
    {
        std::ofstream f(path);
        f << "# 这是注释\n";
        f << "\n";                          // 空行
        f << "   # 带缩进的注释\n";
        f << "  listen = 1.2.3.4:9999  \n"; // 前后有空格
        f << "target=5.6.7.8:8888\n";       // = 前后没有空格
    }

    const char* argv[] = {"prog", "--config", path};
    Config cfg;
    ASSERT_TRUE(cfg.parse(3, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("listen"), "1.2.3.4:9999");
    EXPECT_EQ(cfg.get("target"), "5.6.7.8:8888");

    std::remove(path);
}

// ========== --log-level 被正确存储（后续和 Logger 串联用） ==========

TEST(Config, LogLevelTrace) {
    const char* argv[] = {"prog", "--log-level=trace"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(2, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("log-level"), "trace");
}

TEST(Config, LogLevelDefaultInfo) {
    const char* argv[] = {"prog"};
    Config cfg;
    ASSERT_TRUE(cfg.parse(1, const_cast<char**>(argv)));
    EXPECT_EQ(cfg.get("log-level", "info"), "info");
}
