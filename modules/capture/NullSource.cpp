/**
 * @file    NullSource.cpp
 * @brief   NullSource.h 的实现
 * @author  zzj
 * @date    2026-07-28
 */

#include "modules/capture/NullSource.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

#include "common/Clock.h"
#include "common/Logger.h"

namespace {
    /**
     * 亮度取值范围用的是**视频级**(limited range)的 16~235, 不是 0~255。
     *
     * H.264 默认按 limited range 解释 Y 分量, 写 0 或 255 会被播放器裁掉,
     * 表现为黑处发灰、白处过曝 —— 自己造的测试图更该守这个约定, 否则
     * 后面对比"编码前后是否一致"时会被这层裁剪干扰。
     */
    constexpr uint8_t Y_MIN = 16;
    constexpr uint8_t Y_MAX = 235;

    /**
     * 5x7 点阵数字。每行取低 5 位, 最高位(0x10)是最左边那一列。
     *
     * 自己写死这 70 个字节, 就不用为了画几个数字引入字体库 ——
     * 依赖是要还的, 而这里的需求永远只有 0~9。
     */
    constexpr int DIGIT_W = 5;
    constexpr int DIGIT_H = 7;
    const uint8_t DIGITS[10][DIGIT_H] = {
        {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
        {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
        {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    };

    /**
     * @brief 在帧上填一个矩形, 越界部分自动裁掉
     *
     * @param f  目标帧
     * @param x  左上角横坐标(可为负)
     * @param y  左上角纵坐标(可为负)
     * @param w  宽
     * @param h  高
     * @param yv 亮度值
     * @param uv U 值
     * @param vv V 值
     *
     * @note 由本函数统一裁剪边界, 调用方(画点阵的那层)就不用每个点自己判断,
     *          少一处能写错的地方。
     * @note 色度平面是 1/2 分辨率: 起点向下取整、终点向上取整, 否则奇数坐标的
     *          矩形右边和下边会漏掉半个色度像素, 表现为边缘挂一条彩边。
     */
    void fillRect(RawFrame& f, int x, int y, int w, int h, uint8_t yv, uint8_t uv, uint8_t vv) {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(f.width, x + w);
        const int y1 = std::min(f.height, y + h);
        if (x0 >= x1 || y0 >= y1) return;

        uint8_t* yp = f.y();
        for (int r = y0; r < y1; ++r) {
            std::memset(yp + static_cast<size_t>(r) * f.yStride() + x0, yv,
                        static_cast<size_t>(x1 - x0));
        }

        const int cx0 = x0 / 2;
        const int cy0 = y0 / 2;
        const int cx1 = (x1 + 1) / 2;
        const int cy1 = (y1 + 1) / 2;
        uint8_t* up = f.u();
        uint8_t* vp = f.v();
        for (int r = cy0; r < cy1; ++r) {
            const size_t off = static_cast<size_t>(r) * f.uvStride() + cx0;
            const size_t len = static_cast<size_t>(cx1 - cx0);
            std::memset(up + off, uv, len);
            std::memset(vp + off, vv, len);
        }
    }
}  // namespace

Status NullSource::open(const SourceConfig& cfg) {
    if (cfg.width <= 0 || cfg.height <= 0 || cfg.width % 2 != 0 || cfg.height % 2 != 0) {
        return Status::error(Code::InvalidArg, "NullSource: width/height must be positive and even");
    }
    if (cfg.fps <= 0) {
        return Status::error(Code::InvalidArg, "NullSource: fps must be positive");
    }

    cfg_     = cfg;
    frameId_ = 0;
    start_   = std::chrono::steady_clock::now();
    opened_  = true;

    LOG_INFO("capture", "null source opened %dx%d@%dfps", cfg_.width, cfg_.height, cfg_.fps);
    return Status::ok();
}

Status NullSource::readFrame(RawFrame& out) {
    // 未 open 和 close 之后都归为 Closed: 对调用方而言都是"这个源没数据了, 正常收尾"
    if (!opened_) return Status::error(Code::Closed, "NullSource: not opened");

    throttle();

    out.reset(cfg_.width, cfg_.height);
    out.frameId = frameId_;
    // 节流之后才取时间: captureMs 要代表"这一帧诞生的时刻", 不是"上一帧画完的时刻"
    out.captureMs = steadyNowMs();

    // 两个绘制函数都只依赖 out.frameId, 不读成员 —— 同一个帧号画出来的画面永远一样,
    // 这是后面 M2 验"传输前后逐字节一致"的前提
    drawBackground(out);
    drawFrameId(out);

    ++frameId_;
    return Status::ok();
}

void NullSource::close() {
    if (!opened_) return;  // 幂等
    opened_ = false;
    LOG_INFO("capture", "null source closed after %llu frames",
             static_cast<unsigned long long>(frameId_));
}

void NullSource::drawBackground(RawFrame& out) const {
    const int w = out.width;
    const int h = out.height;

    constexpr int SPAN = Y_MAX - Y_MIN;

    // 每帧平移 3 像素。模数**必须**是 SPAN(条纹的空间周期): 回绕那一帧的位移是
    // -216, 对 219 取模仍是 +3, 接缝看不出来。换成别的数(比如 256)每几十帧就会
    // 猛跳一次, 而这条纹正是用来判断有没有丢帧卡顿的 —— 探针自己抽搐最误导人。
    // 先取模再转 int, 也顺带免了跑久了 frameId 撑爆 int
    const int phase = static_cast<int>((out.frameId * 3) % SPAN);

    uint8_t* yp = out.y();
    for (int r = 0; r < h; ++r) {
        uint8_t* row = yp + static_cast<size_t>(r) * out.yStride();
        for (int c = 0; c < w; ++c) {
            // 对角条纹, 随帧号整体平移 —— 画面动没动、卡没卡, 扫一眼就知道
            row[c] = static_cast<uint8_t>(Y_MIN + (c + r + phase) % SPAN);
        }
    }

    // 色度写渐变而不是全填 128: 全灰的画面里, U/V 平面就算整个丢了也看不出来,
    // 而 M2 之后要靠肉眼判断"颜色对不对"来定位分片/组包的错位
    const int cw = w / 2;
    const int ch = h / 2;
    uint8_t* up = out.u();
    uint8_t* vp = out.v();
    for (int r = 0; r < ch; ++r) {
        const size_t off  = static_cast<size_t>(r) * out.uvStride();
        const uint8_t vval = static_cast<uint8_t>(64 + r * 128 / ch);
        for (int c = 0; c < cw; ++c) {
            up[off + static_cast<size_t>(c)] = static_cast<uint8_t>(64 + c * 128 / cw);
            vp[off + static_cast<size_t>(c)] = vval;
        }
    }
}

void NullSource::drawFrameId(RawFrame& out) const {
    const std::string text = std::to_string(out.frameId);

    // 字号跟分辨率走: 480p 下 scale=8, 单个数字约 40x56 像素。
    // 画大是有原因的 —— 帧号要在 H.264 压缩、丢包花屏之后仍然读得出来
    const int scale  = std::max(1, out.height / 60);
    const int glyphW = (DIGIT_W + 1) * scale;  // 5 列字形 + 1 列字距
    const int pad    = scale;
    const int boxW   = glyphW * static_cast<int>(text.size()) + pad;
    const int boxH   = DIGIT_H * scale + 2 * pad;

    // 先铺一块暗底并把色度拉回中性: 数字直接画在滚动渐变上会时亮时暗、还带颜色,
    // 有底才保证任何一帧都读得出来
    fillRect(out, 0, 0, boxW, boxH, Y_MIN, 128, 128);

    for (size_t i = 0; i < text.size(); ++i) {
        const int d  = text[i] - '0';
        const int gx = pad + static_cast<int>(i) * glyphW;
        for (int row = 0; row < DIGIT_H; ++row) {
            const uint8_t bits = DIGITS[d][row];
            for (int col = 0; col < DIGIT_W; ++col) {
                if (bits & (0x10 >> col)) {
                    fillRect(out, gx + col * scale, pad + row * scale, scale, scale,
                             Y_MAX, 128, 128);
                }
            }
        }
    }
}

void NullSource::throttle() const {
    // 第 n 帧的应到时刻 = 基准 + n/fps, 误差不累积。
    // 除法放在乘法之后做, 整个运行期的舍入误差合计不超过 1us
    const auto offset = std::chrono::microseconds(
        static_cast<int64_t>(frameId_ * 1000000ULL / static_cast<uint64_t>(cfg_.fps)));
    std::this_thread::sleep_until(start_ + offset);
}
