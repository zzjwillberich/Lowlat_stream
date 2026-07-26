/**
 * @file    Status.h
 * @brief   跨模块统一的错误返回类型
 * @author  zzj
 * @date    2026-07-26
 */
#pragma once

#include <string>
#include <utility>

/**
 * 错误码
 *
 * 只区分**调用方会做出不同反应**的类别, 不按底层 errno 细分:
 *  - InvalidArg 参数不合法, 重试无意义, 直接报错退出
 *  - IoError    设备/文件出错, 通常需要重新打开
 *  - NetError   网络出错, 通常可以重连
 *  - Timeout    超时, 通常可以重试
 *  - Closed     对端或队列已关闭, 应当正常收尾退出, 不是异常情况
 *  - Internal   自己的 bug, 不该发生
 */
enum class Code { Ok = 0, InvalidArg, IoError, NetError, Timeout, Closed, Internal };

/**
 * 模块公开接口的统一返回类型
 *
 * 约定: 不跨模块抛异常, 一律返回 Status 或 bool
 *
 * - Ok 路径零堆分配: 空 message 走 SSO 不碰堆, 可以放心用在每帧/每包的热路径上
 * - 标记 [[nodiscard]]: 忽略返回值直接编译告警, 这是相对裸 bool 的主要收益
 *
 * @note 本类**不打日志**。一次错误沿调用栈返回会被拷贝多次, 在这里打会重复;
 *          且底层并不知道调用方打算重试还是退出。日志由**处理**错误的那一层打。
 */
class [[nodiscard]] Status {
public:
    /**
     * @brief 默认构造即成功状态, 让 Status s; 直接可用
     */
    Status() = default;

    /**
     * @brief 构造成功状态
     *
     * @return code() == Code::Ok 的 Status
     */
    static Status ok();

    /**
     * @brief 构造失败状态
     *
     * @param c   错误码, 传 Code::Ok 无意义
     * @param msg 给人看的上下文, 例 "open /dev/video0 failed: EACCES"
     *
     * @return 携带错误码与消息的 Status
     */
    static Status error(Code c, std::string msg = "");

    /**
     * @brief 拼成一行给日志用, 例 "NetError: connect refused"
     *
     * @return 无消息时只返回错误码名
     */
    std::string toString() const;

    // 以下三个访问器位于热路径, 直接内联, 不进 .cpp
    bool isOk() const { return code_ == Code::Ok; }
    Code code() const { return code_; }
    const std::string& message() const { return msg_; }

private:
    Status(Code c, std::string msg) : code_(c), msg_(std::move(msg)) {}

    /**
     * @brief 错误码, 默认成功
     */
    Code code_ = Code::Ok;

    /**
     * @brief 错误上下文, 成功时为空串(不占堆)
     */
    std::string msg_;
};

/**
 * @brief Code 转字符串, 供日志与 Status::toString 使用
 *
 * @param c 需要转换的 Code
 *
 * @return 错误码名, 越界返回 "Unknown"
 *  正常返回 Code字符串
 */
const char* codeStr(Code c);
