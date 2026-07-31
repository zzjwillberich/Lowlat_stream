#include <fstream>
#include <string>

#include "common/Clock.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "modules/capture/ISource.h"

int main(int argc, char** argv) {
    Config config;
    if (!config.parse(argc, argv)) {
        return 1;
    }

    Logger::instance().setLevel(parseLogLevel(config.get("log-level", "info")));

    LOG_INFO("sender", "sender starting");

    SourceConfig sourceCfg;
    sourceCfg.width  = config.getInt("width", 640);
    sourceCfg.height = config.getInt("height", 480);
    sourceCfg.fps    = config.getInt("fps", 30);
    sourceCfg.device = config.get("device", "/dev/video0");

    auto source = createSource(config.get("source", "null"));
    if (!source) {
        return 1;  // createSource 已经打过 ERROR, 这里不重复
    }

    Status st = source->open(sourceCfg);
    if (!st.isOk()) {
        LOG_ERROR("sender", "open source failed: %s", st.toString().c_str());
        return 1;
    }

    const std::string dumpPath = config.get("dump-raw");
    const int wanted = config.getInt("frames", 100);

    std::ofstream dump;
    if (!dumpPath.empty()) {
        dump.open(dumpPath, std::ios::binary | std::ios::trunc);
        if (!dump) {
            LOG_ERROR("sender", "cannot open dump file %s", dumpPath.c_str());
            source->close();
            return 1;
        }
    }

    // 整个循环复用同一个 RawFrame: 分辨率不变时 reset() 不重新分配, 采集路径零堆操作
    RawFrame frame;
    size_t frameBytes = 0;
    int captured      = 0;
    const uint64_t t0 = steadyNowMs();

    for (int i = 0; i < wanted; ++i) {
        st = source->readFrame(frame);
        if (!st.isOk()) {
            // Closed 是正常收尾, 其余才是故障 —— 这正是 readFrame 返回 Status 而不是 bool 的理由
            if (st.code() != Code::Closed) {
                LOG_ERROR("sender", "readFrame failed: %s", st.toString().c_str());
            }
            break;
        }
        if (dump) {
            dump.write(reinterpret_cast<const char*>(frame.data.data()),
                       static_cast<std::streamsize>(frame.data.size()));
        }
        frameBytes = frame.data.size();
        ++captured;
    }

    const uint64_t elapsedMs = steadyNowMs() - t0;
    source->close();
    dump.close();

    const double actualFps = elapsedMs > 0 ? captured * 1000.0 / static_cast<double>(elapsedMs) : 0.0;
    LOG_INFO("sender", "captured %d frames in %llu ms (%.1f fps, target %d)", captured,
             static_cast<unsigned long long>(elapsedMs), actualFps, sourceCfg.fps);

    if (!dumpPath.empty()) {
        LOG_INFO("sender", "raw dump -> %s (%zu bytes/frame, %zu total)", dumpPath.c_str(),
                 frameBytes, frameBytes * static_cast<size_t>(captured));
        LOG_INFO("sender", "play it: ffplay -f rawvideo -pixel_format yuv420p -video_size %dx%d %s",
                 sourceCfg.width, sourceCfg.height, dumpPath.c_str());
    }
    return 0;
}
