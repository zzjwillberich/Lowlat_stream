#include <fstream>
#include <string>
#include <vector>

#include "common/Clock.h"
#include "common/Config.h"
#include "common/Logger.h"
#include "modules/capture/ISource.h"
#include "modules/encode/Encoder.h"

namespace {
    void writeAll(std::ofstream& out, const std::vector<uint8_t>& data) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
}  // namespace

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

    const std::string rawPath  = config.get("dump-raw");
    const std::string h264Path = config.get("dump");
    const int wanted           = config.getInt("frames", 100);

    std::ofstream rawFile;
    if (!rawPath.empty()) {
        rawFile.open(rawPath, std::ios::binary | std::ios::trunc);
        if (!rawFile) {
            LOG_ERROR("sender", "cannot open dump file %s", rawPath.c_str());
            source->close();
            return 1;
        }
    }

    Encoder encoder;
    std::ofstream h264File;
    if (!h264Path.empty()) {
        EncoderConfig encCfg;
        encCfg.width       = sourceCfg.width;
        encCfg.height      = sourceCfg.height;
        encCfg.fps         = sourceCfg.fps;
        encCfg.bitrateKbps = config.getInt("bitrate", 2000);
        encCfg.gop         = config.getInt("gop", sourceCfg.fps);

        st = encoder.open(encCfg);
        if (!st.isOk()) {
            LOG_ERROR("sender", "open encoder failed: %s", st.toString().c_str());
            source->close();
            return 1;
        }
        h264File.open(h264Path, std::ios::binary | std::ios::trunc);
        if (!h264File) {
            LOG_ERROR("sender", "cannot open dump file %s", h264Path.c_str());
            source->close();
            return 1;
        }
    }

    // 整个循环复用同一个 RawFrame: 分辨率不变时 reset() 不重新分配, 采集路径零堆操作
    RawFrame frame;
    std::vector<EncodedFrame> packets;
    size_t rawBytesPerFrame = 0;
    size_t encodedBytes     = 0;
    int keyFrames           = 0;
    int captured            = 0;
    const uint64_t t0       = steadyNowMs();

    for (int i = 0; i < wanted; ++i) {
        st = source->readFrame(frame);
        if (!st.isOk()) {
            // Closed 是正常收尾, 其余才是故障 —— 这正是 readFrame 返回 Status 而不是 bool 的理由
            if (st.code() != Code::Closed) {
                LOG_ERROR("sender", "readFrame failed: %s", st.toString().c_str());
            }
            break;
        }
        rawBytesPerFrame = frame.data.size();
        ++captured;

        if (rawFile) {
            writeAll(rawFile, frame.data);
        }

        if (h264File) {
            packets.clear();
            st = encoder.encode(frame, packets);
            if (!st.isOk()) {
                LOG_ERROR("sender", "encode failed: %s", st.toString().c_str());
                break;
            }
            for (const auto& pkt : packets) {
                writeAll(h264File, pkt.data);
                encodedBytes += pkt.data.size();
                if (pkt.isKey) ++keyFrames;
            }
        }
    }

    // 先冲刷再关闭: 编码器里攒着的帧不取出来, dump 文件尾部就缺几帧
    if (h264File) {
        packets.clear();
        st = encoder.flush(packets);
        if (!st.isOk()) {
            LOG_ERROR("sender", "flush failed: %s", st.toString().c_str());
        }
        for (const auto& pkt : packets) {
            writeAll(h264File, pkt.data);
            encodedBytes += pkt.data.size();
            if (pkt.isKey) ++keyFrames;
        }
    }

    const uint64_t elapsedMs = steadyNowMs() - t0;
    encoder.close();
    source->close();
    rawFile.close();
    h264File.close();

    const double actualFps =
        elapsedMs > 0 ? captured * 1000.0 / static_cast<double>(elapsedMs) : 0.0;
    LOG_INFO("sender", "captured %d frames in %llu ms (%.1f fps, target %d)", captured,
             static_cast<unsigned long long>(elapsedMs), actualFps, sourceCfg.fps);

    if (!rawPath.empty()) {
        LOG_INFO("sender", "raw dump -> %s (%zu bytes/frame, %zu total)", rawPath.c_str(),
                 rawBytesPerFrame, rawBytesPerFrame * static_cast<size_t>(captured));
        LOG_INFO("sender", "play it: ffplay -f rawvideo -pixel_format yuv420p -video_size %dx%d %s",
                 sourceCfg.width, sourceCfg.height, rawPath.c_str());
    }
    if (!h264Path.empty()) {
        const double kbps =
            elapsedMs > 0 ? encodedBytes * 8.0 / static_cast<double>(elapsedMs) : 0.0;
        LOG_INFO("sender", "h264 dump -> %s (%zu bytes, %d key frames, %.0f kbps)",
                 h264Path.c_str(), encodedBytes, keyFrames, kbps);
        LOG_INFO("sender", "play it: ffplay %s", h264Path.c_str());
    }
    return 0;
}
