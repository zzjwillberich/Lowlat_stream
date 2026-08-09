/**
 * @file    ISource.cpp
 * @brief   采集源工厂
 * @author  zzj
 * @date    2026-07-28
 */

#include "modules/capture/ISource.h"

#include "common/Logger.h"
#include "modules/capture/NullSource.h"
#include "modules/capture/V4l2Source.h"

std::unique_ptr<ISource> createSource(const std::string& kind) {
    if (kind == "null") {
        return std::make_unique<NullSource>();
    }
    if (kind == "v4l2") {
        return std::make_unique<V4l2Source>();
    }

    LOG_ERROR("capture", "unknown source kind '%s' (expect \"null\" or \"v4l2\")", kind.c_str());
    return nullptr;
}
