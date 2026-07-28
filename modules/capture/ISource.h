/**
 * @file    ISource.h
 * @brief   采集源统一接口, Null 源与 V4L2 源可互换
 * @author  zzj
 * @date    2026-07-27
 */
#pragma once

#include <memory>
#include <string>

#include "common/Status.h"
#include "modules/capture/Frame.h"

/**
 * 采集源配置
 *
 * @note 这些值一律来自命令行/配置文件, 不许在源码里写死 —— 换分辨率不该需要重新编译。
 */
struct SourceConfig {
    /**
     * @brief 期望宽度(像素)
     */
    int width = 640;

    /**
     * @brief 期望高度(像素)
     */
    int height = 480;

    /**
     * @brief 期望帧率
     */
    int fps = 30;

    /**
     * @brief 摄像头设备节点, 仅 V4L2 源使用
     */
    std::string device = "/dev/video0";
};

/**
 * 采集源抽象 —— 流水线的第一级
 *
 * 有这层抽象的唯一理由: **没有摄像头也能开发整条流水线**。
 * NullSource 生成合成画面, V4l2Source 读真设备, 上层代码一行不用改。
 * 它同时也是压测和复现问题的手段: 合成画面可重复, 摄像头不可重复。
 *
 * 生命周期: open() → readFrame() * N → close(), close() 后可再次 open()。
 *
 * @note 实现类**不保证线程安全**: 一个源只由采集线程独占, 加锁没有意义。
 */
class ISource {
public:
    virtual ~ISource() = default;

    // 采集源持有设备句柄/mmap 缓冲这类独占资源, 拷贝没有合理语义
    ISource(const ISource&) = delete;
    ISource& operator=(const ISource&) = delete;

    /**
     * @brief 打开采集源
     *
     * @param cfg 期望的分辨率/帧率/设备
     *
     * @return Ok         打开成功
     *  InvalidArg 参数不合法(如分辨率为奇数)
     *  IoError    设备打不开或不支持请求的格式
     *
     * @note cfg 是**期望值不是承诺值**: 摄像头可能只支持相近的分辨率。
     *          实现必须把实际协商到的值写回自己的状态, 并由 readFrame() 产出的
     *          RawFrame 如实反映 —— 上层按 RawFrame 里的宽高走, 不要按 cfg 走。
     */
    virtual Status open(const SourceConfig& cfg) = 0;

    /**
     * @brief 阻塞取一帧
     *
     * @param out 出参, 被填充为最新一帧; 内部会 reset() 到实际分辨率
     *
     * @return Ok      取到一帧
     *  Closed  源已正常结束(如 close() 被调用), 调用方应正常收尾退出
     *  IoError 设备出错, 需要重新 open
     *
     * @note 用 Status 而不是 bool, 就是为了让调用方能区分**正常结束**和**设备炸了**:
     *          前者安静退出, 后者必须报警。一个 bool 会把这两件事混成一件。
     * @note out 会被复用: 传同一个 RawFrame 进来可以避免每帧一次堆分配。
     */
    virtual Status readFrame(RawFrame& out) = 0;

    /**
     * @brief 关闭采集源, 释放设备资源
     *
     * @note 不返回 Status: 关闭失败调用方也没有第二种选择, 只能记日志。
     * @note 必须幂等, 且析构函数里要能安全调用。
     */
    virtual void close() = 0;

protected:
    // 只作为基类使用, 不允许直接构造; 但派生类需要能构造自己
    ISource() = default;
};

/**
 * @brief 按名字创建采集源
 *
 * @param kind "null" 合成画面源 | "v4l2" 摄像头源
 *
 * @return 对应的源; kind 不认识时返回 nullptr 并打一条 ERROR
 *
 * @note 返回 nullptr 而不是退回默认源: 用户明确写了 --source=v4l3 却安静地跑起 Null 源,
 *          等他发现画面不对已经浪费半天了。配置错误就该在启动时炸掉。
 */
std::unique_ptr<ISource> createSource(const std::string& kind);
