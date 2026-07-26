/**
 * @file    Config.h
 * @brief   命令行参数与配置文件解析
 * @author  zzj
 * @date    2026-07-23
 */
#pragma once

#include <map>
#include <string>
#include <set>

/**
 * M0.4 命令行与配置文件的键值配置存储
 *
 * 支持三种来源,优先级从高到低
 *  1. 命令行参数   --key=value 或者 --key value
 *  2. 配置文件     key=value 每行一条, # 注释
 *  3. 硬编码默认值 调用 get(key, default) 时的第二个参数
 *
 * @note
 *  - 命令行仅接受 --help、--log-level、--config、--listen、--target
 *  - 未知参数报错并打印用法，不静默忽略
 */
class Config{
public:
    /**
     * @brief 解析命令行参数, 填充内部 kv 表
     *
     * 自动处理 --help (打印用法后exit(0))
     * 若指定 --config file 会先加载配置文件, 再用命令行覆盖同名项
     *
     * @param argc main 的 argc
     * @param argv main 的 argv
     *
     * @return true 解析成功
     *  false 解析失败
     */
    bool parse(int argc, char** argv);

    /**
     * @brief 读取字符串配置
     *
     * @param key 参数名
     * @param def 默认返回值
     *
     * @return 解析成功返回正确value
     *  解析失败返回def
     */
    std::string get(const std::string& key, const std::string& def = "") const;

    /**
     * @brief 读取整数配置
     *
     * @param key 参数名
     * @param def 默认返回值
     *
     * @return 解析成功返回正确value
     *  解析失败返回def
     */
    int getInt(const std::string& key, int def = 0) const;

    /**
     * @brief 打印所用已注册参数与默认值到 stderr
     *
     * 调用方应在此列出自己关心的参数, 供用户 --help 查阅
     */
    void printUsage() const;

    /**
     * @brief 返回已注册的参数 key
     *
     * @return 包含所有 key的set
     */
    std::set<std::string> keys() const;

private:
    /**
     * @brief 从 key=value 格式的文本文件加载配置
     *
     * @param path 文件路径
     *
     * @return true 解析成功
     *  false解析失败
     */
    bool loadFile(const std::string& path);

    /**
     * @brief 解析单个 kv 参数对
     *
     * @param arg
     * @param nextArg
     * @param key
     * @param value
     *
     * @return 解析成功的参数个数
     */
    int parseOne(const std::string& arg, const char* nextArg,std::string& key,std::string& value);

    /**
     * @brief kv存储容器
     */
    std::map<std::string, std::string> kv_;
};
