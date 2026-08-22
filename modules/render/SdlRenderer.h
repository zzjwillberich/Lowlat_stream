/**
 * @file    SdlRenderer.h
 * @brief   基于 SDL2 的渲染器: 开窗口, 把 YUV420P 直接送上屏幕
 * @author  zzj
 * @date    2026-08-22
 */
#pragma once

#include "modules/render/IRenderer.h"

// 只前向声明, **绝对不要在这里 #include <SDL2/SDL.h>**:
// 那个头文件里有 #define main SDL_main, 一旦 include 就会传染给每一个使用者,
// 在 main.cpp 里表现为莫名其妙的链接错误。CMake 里 SDL2 也走 PRIVATE。
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

/**
 * SDL2 渲染器 —— 真正把画面显示出来的那个
 *
 * 它的验收标准是"人看着画面是对的"。**不做**帧内容校验 —— 那是 NullRenderer 的活。
 *
 * @note 整个对象的生命周期必须待在**主线程**: SDL 要求窗口创建、渲染、事件泵、销毁
 *          都在同一个线程。Linux 上从别的线程渲染经常能跑, 但不保证, 换个驱动就可能挂;
 *          macOS 上直接不行。
 */
class SdlRenderer : public IRenderer {
public:
    SdlRenderer() = default;

    // 析构必须收尾: 提前 return / 抛异常 / 忘了写, 三种都会跳过显式 close(),
    // 析构函数是唯一不会被跳过的东西
    ~SdlRenderer() override;

    Status open(const RendererConfig& cfg) override;
    Status renderFrame(const RawFrame& frame) override;
    bool   pumpEvents() override;
    const RendererStats& stats() const override;
    void   close() override;

private:
    /**
     * @brief 确保纹理尺寸和这一帧一致, 不一致就重建
     *
     * @note 只在尺寸**变了**的时候重建。每帧重建就是每帧一次显存分配,
     *          "连续跑 5 分钟内存不涨"这条验收会直接红。重建一次 ++texturesRebuilt,
     *          稳态下这个数应该恒为 1。
     */
    Status ensureTexture(int width, int height);

    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;

    // 当前纹理的尺寸; 和进来的帧不一致就触发重建
    int texWidth_  = 0;
    int texHeight_ = 0;

    RendererConfig config_;
    RendererStats  stats_;

    // 注意这里**没有** opened_ 字段: window_ != nullptr 本身就是答案。
    // 多一个 bool 就多一处可能跟指针不同步的状态。
    // (NullSource 用 opened_ 是因为它没有指针可查, 情况不同。)
};
