/**
 * @file    IRenderer.cpp
 * @brief   渲染器工厂
 * @author  zzj
 * @date    2026-08-22
 */

#include "modules/render/IRenderer.h"

#include "common/Logger.h"
#include "modules/render/NullRenderer.h"
#include "modules/render/SdlRenderer.h"

std::unique_ptr<IRenderer> createRenderer(const std::string& kind) {
    if (kind == "null") {
        return std::make_unique<NullRenderer>();
    }
    if (kind == "sdl") {
        return std::make_unique<SdlRenderer>();
    }

    LOG_ERROR("render", "unknown renderer kind '%s' (expect \"sdl\" or \"null\")", kind.c_str());
    return nullptr;
}
