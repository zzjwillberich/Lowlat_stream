/**
 * @file    ISource.cpp
 * @brief   采集源工厂
 * @author  zzj
 * @date    2026-07-28
 */

#include "modules/capture/ISource.h"

#include "common/Logger.h"
#include "modules/capture/NullSource.h"

std::unique_ptr<ISource> createSource(const std::string& kind) {
    if (kind == "null") {
        return std::make_unique<NullSource>();
    }
    if (kind == "v4l2") {
        // 认识这个名字但还没实现, 单独报出来 —— 否则用户会以为自己拼错了
        LOG_ERROR("capture", "v4l2 source is not implemented yet (M1.5), use --source=null");
        return nullptr;
    }

    LOG_ERROR("capture", "unknown source kind '%s' (expect \"null\" or \"v4l2\")", kind.c_str());
    return nullptr;
}
