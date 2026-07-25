/**
 * @file    Logger.cpp
 * @brief   Logger.h 的实现
 * @author  zzj
 * @date    2026-07-22
 */
#include "common/Logger.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <mutex>

Logger&  Logger::instance(){
    static Logger inst;
    return inst;
}

void Logger::setLevel(LogLevel lv){
    level_ = lv;
}

void Logger::log(LogLevel lv, const char* module, const char* fmt, ...){
    //加锁前先判断, 低于设定级别直接过滤掉, 过滤掉的消息零开销
    if(lv < level_) return;

    std::lock_guard<std::mutex> lock(mu_);

    //时间戳
    char ts[32];
    timestamp(ts, sizeof(ts));

    //日志前缀
    fprintf(stderr, "[%s][%s][%s] ", ts, levelStr(lv), module);

    //用户消息
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

const char* Logger::levelStr(LogLevel lv){
    static const char* names[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    int idx = static_cast<int>(lv);
    if(idx < 0 || idx > 4) return "???? ";
    return names[idx];
}

void Logger::timestamp(char* buf, size_t cap){
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    time_t tt = system_clock::to_time_t(now);

    struct tm tm_buf;
    localtime_r(&tt, &tm_buf);

    size_t n = strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_buf);
    snprintf(buf + n, cap - n, ".%03d", static_cast<int>(ms));
}
