/**
 * @file    SdlRenderer.cpp
 * @brief   SdlRenderer.h 的实现
 * @author  zzj
 * @date    2026-08-22
 */

#include "modules/render/SdlRenderer.h"

// SDL 只在这个 .cpp 里出现。#define main SDL_main 的传染到此为止。
#include <SDL2/SDL.h>

#include <limits>
#include <string>

SdlRenderer::~SdlRenderer() {
    close();
}

Status SdlRenderer::open(const RendererConfig& cfg) {
    if (cfg.width <= 0 || cfg.height <= 0) {
        return Status::error(Code::InvalidArg, "SdlRenderer: width and height must be positive");
    }

    close();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return Status::error(Code::IoError, std::string("SDL_Init failed: ") + SDL_GetError());
    }

    window_ = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               cfg.width, cfg.height, 0);
    if (!window_) {
        const std::string error = SDL_GetError();
        close();
        return Status::error(Code::IoError, "SDL_CreateWindow failed: " + error);
    }

    // flags 不写 SDL_RENDERER_ACCELERATED, 让 SDL 自己按驱动列表顺序挑 ——
    // 实践中软件驱动排在最后, 有加速就会先选上。
    //
    // 之所以不显式要加速: 那样在无头环境(SDL_VIDEODRIVER=dummy)下 CreateRenderer
    // 会直接失败 "Couldn't find matching render driver", 而无头是单测能跑到
    // SdlRenderer 的唯一途径。ensureTexture 的重建逻辑、close 的销毁顺序、
    // renderFrame 的整条路径 —— 这些才是这个类里真正有逻辑的部分, 显式要加速
    // 就等于把它们全部划到测试覆盖之外。用"可能选到软件渲染器"换"这些代码
    // 每次 ctest 都被跑一遍", 这笔交易划算。
    Uint32 flags = 0;
    if (cfg.vsync) {
        flags |= SDL_RENDERER_PRESENTVSYNC;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, flags);
    if (!renderer_) {
        const std::string error = SDL_GetError();
        close();
        return Status::error(Code::IoError, "SDL_CreateRenderer failed: " + error);
    }

    config_ = cfg;
    stats_ = {};
    texWidth_ = 0;
    texHeight_ = 0;
    return Status::ok();
}

Status SdlRenderer::renderFrame(const RawFrame& frame) {
    if (!renderer_) {
        return Status::error(Code::Closed, "SdlRenderer: render before open");
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.width % 2 != 0 || frame.height % 2 != 0 ||
        frame.fmt != PixelFormat::YUV420P) {
        ++stats_.framesRejected;
        return Status::error(Code::InvalidArg, "SdlRenderer: expected a positive, even YUV420P frame");
    }

    const size_t width = static_cast<size_t>(frame.width);
    const size_t height = static_cast<size_t>(frame.height);
    if (width > std::numeric_limits<size_t>::max() / height) {
        ++stats_.framesRejected;
        return Status::error(Code::InvalidArg, "SdlRenderer: frame dimensions overflow");
    }
    const size_t yBytes = width * height;
    if (yBytes > std::numeric_limits<size_t>::max() - yBytes / 2 ||
        frame.data.size() < yBytes + yBytes / 2) {
        ++stats_.framesRejected;
        return Status::error(Code::InvalidArg, "SdlRenderer: frame data is too small");
    }

    Status st = ensureTexture(frame.width, frame.height);
    if (!st.isOk()) return st;

    if (SDL_UpdateYUVTexture(texture_, nullptr, frame.y(), frame.yStride(), frame.u(),
                             frame.uvStride(), frame.v(), frame.uvStride()) != 0) {
        return Status::error(Code::Internal, std::string("SDL_UpdateYUVTexture failed: ") + SDL_GetError());
    }
    if (SDL_RenderClear(renderer_) != 0) {
        return Status::error(Code::Internal, std::string("SDL_RenderClear failed: ") + SDL_GetError());
    }
    if (SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) != 0) {
        return Status::error(Code::Internal, std::string("SDL_RenderCopy failed: ") + SDL_GetError());
    }

    SDL_RenderPresent(renderer_);
    ++stats_.framesRendered;
    return Status::ok();
}

Status SdlRenderer::ensureTexture(int width, int height) {
    if (texture_ && texWidth_ == width && texHeight_ == height) {
        return Status::ok();
    }

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    texWidth_ = 0;
    texHeight_ = 0;

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                 width, height);
    if (!texture_) {
        return Status::error(Code::Internal, std::string("SDL_CreateTexture failed: ") + SDL_GetError());
    }

    texWidth_ = width;
    texHeight_ = height;
    ++stats_.texturesRebuilt;
    return Status::ok();
}

bool SdlRenderer::pumpEvents() {
    bool quit = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
            quit = true;
        }
    }
    return quit;
}

const RendererStats& SdlRenderer::stats() const {
    return stats_;
}

void SdlRenderer::close() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    // 只在真的初始化过之后才 Quit: open() 开头会先调一次 close(), 那时 SDL 还没
    // Init。SDL 对此本来就是安全的, 但"先关一个还没开的东西"读起来是错的,
    // 而且 SDL_WasInit 不需要我们自己再养一个 bool 去记这件事。
    if (SDL_WasInit(0) != 0) {
        SDL_Quit();
    }
    texWidth_ = 0;
    texHeight_ = 0;
}
