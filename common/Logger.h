/**
 * @file    Logger.h
 * @brief   轻量化线程安全日志系统
 * @author  zzj
 * @date    2026-07-22
 */
#pragma once

#include <mutex>
#include <atomic>
#include <string>

#include <cstdio>

/**
 * 日志级别  输出时低于该级别的消息直接静默丢弃
 *
 * TRACE/DEBUG  仅供开发调试, Release 默认 INFO
 * 即 TRACE/DEBUG 在 Release 模式下零开销 (会在加锁前过滤)
 */
enum class LogLevel{ TRACE = 0, DEBUG, INFO, WARN, ERROR };

/**
 * @brief string 转 LogLevel, 供 --log-level 等配置项使用
 *
 * @param s LogLevel 字符串, 小写: trace/debug/info/warn/error
 *
 * @return 对应的 LogLevel 枚举值
 *
 * @note 不认识的值退回 INFO 并打一条 WARN, 而不是拒绝启动:
 *          否则 --log-level=debgu 这类拼写错误会表现为"日志系统坏了"
 */
LogLevel parseLogLevel(const std::string& s);

/**
 * 线程安全的单例模式日志器
 *
 * - 所有 log() 调用在写 stderr 的时候持有互斥锁,保证多线程输出不会穿台
 * - 级别过滤在加锁之前, 被过滤掉的信息不会加锁, 减少部分开销
 * - 输出格式:
 *      [2026-07-22 16:28:02.123][INFO ][sender] startup complete
 *
 * @note 设计上不引入第三方库依赖
 */
class Logger {
public:
    /**
     * @brief 全局唯一实例 (c++11 保证静态局部变量初始化的线程安全)
     *
     * @return Logger对象
     */
    static Logger& instance();

    /**
     * @brief 设置最低的输出级别, 低于该级别的消息被丢弃
     *
     * @param lv 想要设置的LogLevel
     */
    void setLevel(LogLevel lv);

    /** 格式化输出一条日志
     *
     * @param lv        本条日志级别
     * @param moudle    模块名, 输出时右对齐 5 字符
     * @param fmt       printf 风格格式串
     * @param ...       可变参数
     *
     * @note 若 lv < 当前级别, 立即返回
     *          时间戳在锁内获取, 保证线程输出时间单调不交叉
    */
    void log(LogLevel lv, const char* module, const char* fmt, ...);

private:
    Logger() = default;

    /**
     * @brief 将 logLevel 转换为 5 字符右对齐的大写字符串
     *
     * @param lv 需要转化的LogLevel
     *
     * @return 大写 LogLevel字符串
     */
    static const char* levelStr(LogLevel lv);

    /**
     * @brief 格式化当前时间戳到 buf, 含毫秒: "YYYY-MM-DD HH:MM:SS.mmm"
     *
     * @param buf 时间戳的缓冲区
     * @param cap 缓冲区大小
     *
     * @return 装填时间戳后的缓冲区
     */
    static void timestamp(char* buf ,size_t cap);

    std::mutex mu_;
    std::atomic<LogLevel> level_{LogLevel::INFO};
};

/**
 * 便捷宏
 *
 * 使用方法: LOG_INFO("sender", "listen on %s", addr)
 */
#define LOG_TRACE(mod, ...) Logger::instance().log(LogLevel::TRACE, mod, __VA_ARGS__)
#define LOG_DEBUG(mod, ...) Logger::instance().log(LogLevel::DEBUG, mod, __VA_ARGS__)
#define LOG_INFO(mod, ...) Logger::instance().log(LogLevel::INFO, mod, __VA_ARGS__)
#define LOG_WARN(mod, ...) Logger::instance().log(LogLevel::WARN, mod, __VA_ARGS__)
#define LOG_ERROR(mod, ...) Logger::instance().log(LogLevel::ERROR, mod, __VA_ARGS__)
