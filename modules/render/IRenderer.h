/**
 * @file    IRenderer.h
 * @brief   渲染器统一接口, Null 渲染器与 SDL 渲染器可互换
 * @author  zzj
 * @date    2026-08-22
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/Status.h"
#include "modules/capture/Frame.h"

/**
 * 渲染器配置
 *
 * @note 和 SourceConfig 一样, 这些值一律来自命令行/配置文件, 不在源码里写死。
 */
struct RendererConfig {
    /**
     * @brief 窗口标题
     */
    std::string title = "lowlat_stream";

    /**
     * @brief 窗口初始宽度(像素)
     *
     * @note **只是窗口的初始大小, 不是画面尺寸**。画面尺寸以每一帧自己带的
     *          RawFrame::width 为准 —— H.264 码流自带 SPS, 对端重开编码器时
     *          分辨率可能中途变化, 渲染器要据此重建纹理。
     */
    int width = 640;

    /**
     * @brief 窗口初始高度(像素)
     */
    int height = 480;

    /**
     * @brief 是否等垂直同步
     *
     * @note 默认关。开了 SDL_RenderPresent 会阻塞到下一次屏幕刷新, 60Hz 下平均
     *          多 8ms、最坏 16ms —— 对一个把低延迟写在标题上的项目, 这个默认值必须是关。
     *          留 --vsync=1 给"我就是要看画面不撕裂"的场合。
     */
    bool vsync = false;
};

/**
 * 渲染器运行统计
 *
 * @note 这些计数器不是给人看着玩的, 是**单测唯一的判据**。NullRenderer 跑在
 *          ctest 里不开窗, 帧对不对没人能用眼睛验, 只能靠测试读这里的数字断言。
 *          光有计数器不做断言 = 没查(同 DecoderStats::framesMissingMeta)。
 */
struct RendererStats {
    /**
     * @brief 成功提交显示的帧数
     */
    uint64_t framesRendered = 0;

    /**
     * @brief 不变量检查没过、被拒绝的帧数
     *
     * @note 单测里应当 EXPECT_EQ(framesRejected, 0)。偶发 1 帧是噪声, 每秒 30 帧
     *          全坏是上游炸了 —— 只有计数能区分这两件事。
     */
    uint64_t framesRejected = 0;

    /**
     * @brief 因分辨率变化而重建纹理的次数
     *
     * @note 稳态下应当恒为 1(首帧建一次)。这个数持续增长就说明纹理在每帧重建,
     *          那是显存分配打满热路径 —— "连续跑 5 分钟内存不涨"这条验收
     *          就是靠它来定位的。
     */
    uint64_t texturesRebuilt = 0;
};

/**
 * 渲染器抽象 —— 流水线的最后一级
 *
 * 有这层抽象的**第一**理由和 ISource 完全一样: **没有显示器也能把整条流水线跑完**。
 * 收包 → 组包 → JitterBuffer → 解码 → 队列 → 渲染, 这一整条在 CI 上必须能端到端
 * 跑通, 而 CI 机器没有 DISPLAY。NullRenderer 是让**别的东西能被测**的那块垫脚石,
 * 它自己不是被测对象。
 *
 * 第二理由才是校验与计数; 第三个是白送的: 同一条流水线换成 NullRenderer 跑一遍,
 * 测出来的延迟就是"不含显示开销"的下限, 和 SdlRenderer 一减就知道渲染花了多少。
 *
 * 两个实现只有**接口**相同, 验收标准完全不同:
 *   - SdlRenderer  的验收是"人看着画面是对的"; 检查画面正不正常是人眼的活, 不是它的活。
 *   - NullRenderer 的验收是"进程没崩、计数对得上、断言没炸"。
 *
 * 生命周期: open() → { renderFrame() | pumpEvents() } * N → close(), close() 后可再 open()。
 *
 * @note **它不拥有主循环**。"什么时候取帧、从哪取帧、要不要退出"全是调用方的事,
 *          渲染器只提供循环体里的一步 —— 同 ISource::readFrame() 不跑循环。
 * @note 实现类**不保证线程安全**, 而且比 ISource 更严: SDL 要求窗口创建、渲染、
 *          事件泵在**同一个线程**且是主线程(macOS 上是操作系统级强制, Linux 上
 *          换个驱动就可能挂)。所以整个对象的生命周期都待在主线程里, 解码线程
 *          只往队列 push, 一行 SDL 都不碰。
 */
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // 渲染器持有窗口/纹理这类独占资源, 拷贝没有合理语义(同 ISource)
    IRenderer(const IRenderer&) = delete;
    IRenderer& operator=(const IRenderer&) = delete;

    /**
     * @brief 打开渲染器(初始化 SDL、建窗口、建渲染器)
     *
     * @param cfg 窗口标题/初始尺寸/是否等垂直同步
     *
     * @return Ok         打开成功
     *  InvalidArg 参数不合法(宽高 <= 0)
     *  IoError    环境不给(没有 DISPLAY、没有可用渲染后端)
     *
     * @note 这两种失败调用方的动作**不一样**, 所以必须能区分:
     *          InvalidArg 是命令行写错了, 报错退出让用户改参数;
     *          IoError    是这台机器就是没显示器(SSH 上去跑就是这个),
     *                     调用方可以选择退回 NullRenderer 继续跑。
     *          一个 bool 会把这两件事混成一件。
     * @note 参数校验就在这里做, **不要**单独开一个 validateConfig():
     *          独立出来调用方就可能忘了调, 于是 open() 还得自己再校一遍。
     *          能被忘记的检查等于没有检查。
     */
    virtual Status open(const RendererConfig& cfg) = 0;

    /**
     * @brief 提交一帧去显示
     *
     * @param frame 待显示的一帧; 尺寸/格式以它自己带的字段为准
     *
     * @return Ok         已提交
     *  InvalidArg 这一帧不合法(尺寸、格式、data 大小对不上),
     *                     **下一帧可能是好的**, 调用方记一笔继续跑
     *  Internal   渲染器自己坏了(纹理建不出来、渲染器已失效), 继续没有意义
     *  Closed     还没 open() 或已经 close()
     *
     * @note 传 const RawFrame& 不是风格选择: RawFrame 的拷贝构造在 Frame.h 里被
     *          = delete 了 —— 隐式拷贝一帧 YUV420P 是 w*h*3/2 字节的整块搬运,
     *          直接从类型层面堵死。渲染器只读不留, **零次拷贝**(SDL 那边会拷进显存,
     *          那是必要的一次)。
     * @note 每秒被调 30 次, 在热路径上。Ok 不带错误消息所以成功路径不分配内存
     *          (见 NOTES.md D7); 但**不要在这里打日志** —— Logger 是全局锁 +
     *          无缓冲 stderr, 一行日志就把测量出来的延迟污染了(NOTES.md D1)。
     * @note frame 尺寸和上一帧不同是**合法**的, 不是错误: H.264 码流自带 SPS,
     *          对端重开编码器时分辨率会变。实现必须重建纹理并 ++texturesRebuilt。
     */
    virtual Status renderFrame(const RawFrame& frame) = 0;

    /**
     * @brief 泵一次窗口事件, 返回用户是否要求退出
     *
     * @return true 用户点了窗口的 × 或按了 Esc, 调用方应当发起整条流水线停止
     *
     * @note **这是"用户要退出"这件事进入程序的唯一入口。** SDL_QUIT 只有
     *          SDL_PollEvent 看得见, 而只有渲染器这一侧可以碰 SDL —— 让主循环
     *          直接调 SDL_PollEvent, 就得让 main.cpp include SDL.h, 那个头文件里
     *          有 #define main SDL_main, 会传染给每个使用者。所以这条通路必须
     *          经过接口。
     * @note **不泵事件的后果不只是点不掉窗口**: 合成器过一会儿会判定这个程序卡死,
     *          给窗口打上灰色遮罩。所以哪怕一个事件都不处理, 也得把队列抽干。
     * @note 必须和 open() 在同一个线程调用。
     * @note NullRenderer 恒返回 false —— 它**真的**没有事件源, 这不是空实现。
     */
    virtual bool pumpEvents() = 0;

    /**
     * @brief 读取运行统计
     *
     * @return 自最近一次 open() 起累积的计数
     */
    virtual const RendererStats& stats() const = 0;

    /**
     * @brief 关闭渲染器, 释放窗口/纹理/SDL 子系统
     *
     * @note 不返回 Status, 而且理由比 ISource::close() 更硬: SDL_DestroyTexture /
     *          SDL_DestroyRenderer / SDL_DestroyWindow / SDL_Quit **全都返回 void**。
     *          就算声明成返回 Status, 也没有任何信息可以填进去。
     *          ISource 那边是"知道了也没用", 这边是"压根不知道"。
     * @note 必须幂等, 且析构函数里要能安全调用 —— 提前 return、抛异常、单纯忘了写,
     *          三种都会跳过显式的 close(), 析构函数是唯一不会被跳过的东西。
     * @note 销毁**有顺序**: Texture → Renderer → Window → SDL_Quit()。
     *          Texture 是从 Renderer 里创建的, Renderer 又绑在 Window 上,
     *          反过来就是拿已释放的对象干活。
     * @note 里面不要写任何提前返回的分支: 要么全销毁, 要么一个都别动。
     */
    virtual void close() = 0;

protected:
    // 只作为基类使用, 不允许直接构造; 但派生类需要能构造自己
    IRenderer() = default;
};

/**
 * @brief 按名字创建渲染器
 *
 * @param kind "sdl" 开真窗口 | "null" 不开窗只校验计数
 *
 * @return 对应的渲染器; kind 不认识时返回 nullptr 并打一条 ERROR
 *
 * @note 是**自由函数不是成员函数**: 成员函数得先有对象才能调, 而工厂的全部意义
 *          就是"我还没有对象, 帮我造一个"。
 * @note 返回 unique_ptr 而不是按值: IRenderer 是抽象类, 不能实例化, 也就不能按值存在。
 * @note kind 不认识时返回 nullptr 而不是退回 NullRenderer —— 用户明确写了
 *          --render=sdI 却安静地跑起 Null 渲染器, 等他发现没画面已经浪费半天了(同 createSource)。
 */
std::unique_ptr<IRenderer> createRenderer(const std::string& kind);
