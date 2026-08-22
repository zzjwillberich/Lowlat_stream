/**
 * @file    SdlRenderer.cpp
 * @brief   SdlRenderer.h 的实现
 * @author  zzj
 * @date    2026-08-22
 */

#include "modules/render/SdlRenderer.h"

// SDL 只在这个 .cpp 里出现。#define main SDL_main 的传染到此为止。
#include <SDL2/SDL.h>

SdlRenderer::~SdlRenderer() {
    close();
}

Status SdlRenderer::open(const RendererConfig& cfg) {
    // TODO(zzj):
    //   1. 校验 cfg.width > 0 && cfg.height > 0 → InvalidArg
    //   2. SDL_Init(SDL_INIT_VIDEO)
    //      **不要** SDL_INIT_EVERYTHING: 白初始化音频和手柄子系统, 而 SDL_Init 是
    //      "任一子系统失败就整体返回失败", 在没有声卡的容器里会因此起不来。
    //      失败 → Status::error(Code::IoError, SDL_GetError())
    //   3. SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, ..., cfg.width, cfg.height, 0)
    //      失败 → IoError, 并且**要把已经 Init 的 SDL 收回去**(调 close())
    //   4. SDL_CreateRenderer(window_, -1, cfg.vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
    //      失败 → 同上
    //   5. config_ = cfg; stats_ = {}; texWidth_ = texHeight_ = 0;
    //
    // @note 中途失败必须调 close() 再返回, 否则前几步建好的东西就泄漏了。
    //       close() 是幂等的, 放心调。
    (void)cfg;
    return Status::ok();
}

Status SdlRenderer::renderFrame(const RawFrame& frame) {
    // TODO(zzj):
    //   1. !renderer_ → Status::error(Code::Closed, ...)
    //   2. 最低限度的自保检查(不是内容校验, 是防止越界读):
    //      frame.width/height > 0 且 frame.data.size() 够大 → 否则 InvalidArg
    //   3. ensureTexture(frame.width, frame.height) 失败 → 把它的 Status 传出去
    //   4. SDL_UpdateYUVTexture(texture_, nullptr,
    //                           frame.y(), frame.yStride(),
    //                           frame.u(), frame.uvStride(),
    //                           frame.v(), frame.uvStride());
    //      —— stride 传的是**行跨距**不是 width。RawFrame 是紧凑排布所以这里恰好
    //         等于 w / w/2 / w/2; 哪天改成直接吃解码器输出, 这里要换成 linesize。
    //   5. SDL_RenderClear → SDL_RenderCopy(renderer_, texture_, nullptr, nullptr)
    //      → SDL_RenderPresent
    //   6. ++stats_.framesRendered; return Status::ok();
    //
    // @note 帧内容对不对不归这里管, 那是人眼的活。这里只做防越界的自保。
    // @note 不要打日志: 每秒 30 次, Logger 是全局锁 + 无缓冲 stderr(NOTES.md D1)。
    (void)frame;
    return Status::ok();
}

Status SdlRenderer::ensureTexture(int width, int height) {
    // TODO(zzj):
    //   1. texture_ && texWidth_ == width && texHeight_ == height → 直接 return Ok
    //      **这个提前返回是整个函数的重点**: 每帧重建纹理就是每帧一次显存分配,
    //      "连续跑 5 分钟内存不涨"那条验收会直接红。
    //   2. texture_ 非空 → SDL_DestroyTexture(texture_); texture_ = nullptr;
    //   3. SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
    //                        SDL_TEXTUREACCESS_STREAMING, width, height)
    //      失败 → Status::error(Code::Internal, SDL_GetError())
    //   4. texWidth_ = width; texHeight_ = height; ++stats_.texturesRebuilt;
    //
    // @note IYUV 就是 YUV420P 的平面序 Y-U-V, 和解码器输出一致 —— 省掉一次 swscale
    //       全画面转换, 那在延迟账上不小。
    (void)width;
    (void)height;
    return Status::ok();
}

bool SdlRenderer::pumpEvents() {
    // TODO(zzj):
    //   SDL_Event e;
    //   bool quit = false;
    //   while (SDL_PollEvent(&e)) {
    //       if (e.type == SDL_QUIT) quit = true;
    //       if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
    //   }
    //   return quit;
    //
    // @note SDL_Event 是**union** 不是继承体系: 先看 e.type, 再决定读哪个成员。
    // @note 循环必须把队列**抽干**, 不能拿到一个就 return —— 剩下的事件会一直堆着,
    //       合成器会判定程序卡死给窗口打灰色遮罩。
    // @note 发现 quit 之后也要继续把这一轮泵完再返回, 别提前 break。
    return false;
}

const RendererStats& SdlRenderer::stats() const {
    return stats_;
}

void SdlRenderer::close() {
    // TODO(zzj): 严格按这个顺序, 每一步都先判空后置空
    //   if (texture_)  { SDL_DestroyTexture(texture_);   texture_  = nullptr; }
    //   if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    //   if (window_)   { SDL_DestroyWindow(window_);     window_   = nullptr; }
    //   SDL_Quit();
    //   texWidth_ = texHeight_ = 0;
    //
    // @note 顺序不能反: Texture 是从 Renderer 里创建的, Renderer 又绑在 Window 上。
    //       反过来就是拿已释放的对象干活。
    // @note 置空是幂等的实现方式 —— 析构函数还会再调一次。
    // @note 里面不要写任何提前返回: 要么全销毁, 要么一个都别动。
    // @note SDL_Quit() 是**进程级全局**的, 不是对象级的。一个进程里同时活着两个
    //       SdlRenderer 的话, 第一个 close 就把另一个的 SDL 也关了。
    //       现在只有一个渲染器所以不是问题, 但单测里反复 open/close/open 会踩到。
}
