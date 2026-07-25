/**
 * @file LogLevel.h
 * @brief string类型转 LogLevel
 * @author zzj
 * @date 2026.07.25
 */

#include "common/Logger.h"
#include <string>

/**
 * @brief string类型转 LogLevel
 *
 * @param s LogLevel 字符串
 *
 * @return LogLevel enum值
 */
inline LogLevel parseLogLevel(const std::string& s) {
    if (s == "trace") return LogLevel::TRACE;
    if (s == "debug") return LogLevel::DEBUG;
    if (s == "info")  return LogLevel::INFO;
    if (s == "warn")  return LogLevel::WARN;
    if (s == "error") return LogLevel::ERROR;
    return LogLevel::INFO;  // 不认识的值默认 INFO
}