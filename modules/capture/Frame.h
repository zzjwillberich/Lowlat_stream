/**
 * @file    Frame.h
 * @brief   贯穿整条流水线的原始图像帧
 * @author  zzj
 * @date    2026-07-27
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * 像素格式
 *
 * @note 加新格式时要动的地方就三处: 这个 enum、frameBytes() 的 switch、
 *          以及 RawFrame 的平面访问器(见 assertPlanar 的说明)。
 *          前两处漏了 -Wswitch 会报, 第三处漏了 assertPlanar 会在 Debug 下拦下。
 */
enum class PixelFormat { YUV420P };

/**
 * @brief PixelFormat 转字符串, 供日志使用
 *
 * @param fmt 像素格式
 *
 * @return 格式名, 越界返回 "Unknown"
 */
inline const char* pixelFormatStr(PixelFormat fmt) {
    switch(fmt) {
        case PixelFormat::YUV420P: return "YUV420P";
    }
    return "Unknown";
}

/**
 * @brief 计算一帧的字节数
 *
 * @param width  宽, YUV420P 下必须为偶数
 * @param height 高, YUV420P 下必须为偶数
 * @param fmt    像素格式
 *
 * @return 紧凑排布下该帧占用的字节数; 格式不认识时返回 0
 *
 * @note switch **故意不写 default**: 将来加一种 PixelFormat, -Wswitch 会在这里
 *          报"未处理的枚举值", 把所有该改的地方指出来。写了 default 就等于告诉
 *          编译器"都处理过了", 这个提醒也就没了。兜底放在 switch 之后。
 */
inline size_t frameBytes(int width, int height, PixelFormat fmt = PixelFormat::YUV420P) {
    assert(width > 0 && height > 0 && "frameBytes: invalid size");

    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    switch(fmt) {
        case PixelFormat::YUV420P:
            // 偶数要求是 YUV420P 专属的: 色度平面横竖各减半, 奇数除不尽, 算出来的
            // 大小会与解码器的预期对不上, 表现为画面撕裂或错位
            assert(width % 2 == 0 && height % 2 == 0 && "YUV420P requires even width/height");
            return pixels * 3 / 2;
    }

    assert(false && "frameBytes: unhandled PixelFormat");
    return 0;
}

/**
 * 一帧未压缩图像 —— 采集端的产物, 编码器的输入
 *
 * 内存布局(YUV420P, 紧凑无 padding):
 * ```text
 * |<---------- w*h ---------->|<- w*h/4 ->|<- w*h/4 ->|
 * |            Y              |     U     |     V     |
 * ```
 *
 * **禁止拷贝**: 一帧 1080p 是 3MB, 一次隐式拷贝就是一次 3MB 的 memcpy, 30fps
 * 下每秒白烧 90MB 带宽, 而且它不会报错, 只会让延迟悄悄变高 —— 这类问题查起来
 * 极费劲。所以宁可让"想拷贝"变成编译错误: 真要复制就显式写循环。
 * 队列里传的是 std::unique_ptr<RawFrame>, 本来也只需要 move。
 *
 * @note 本结构体是纯数据, 不含任何锁: 同一时刻只应有一个线程持有它。
 *          所有权靠 unique_ptr 在队列上交接, 交出去就不要再碰。
 */
struct RawFrame {
    /**
     * @brief 像素数据, 紧凑排布, 大小由 frameBytes() 决定
     */
    std::vector<uint8_t> data;

    /**
     * @brief 图像宽度(像素)
     */
    int width = 0;

    /**
     * @brief 图像高度(像素)
     */
    int height = 0;

    /**
     * @brief 像素格式
     */
    PixelFormat fmt = PixelFormat::YUV420P;

    /**
     * @brief 采集时刻, steady_clock 毫秒
     *
     * @note 端到端延迟的**起点**, 一路随帧传到接收端渲染时相减。
     *          必须用 steady_clock: system_clock 会被 NTP 校时或用户改表拨动,
     *          拨一次就能算出负延迟。日志时间戳才用 system_clock(那个要给人看)。
     */
    uint64_t captureMs = 0;

    /**
     * @brief 帧序号, 从 0 递增
     *
     * @note 既是调试用的标号(NullSource 会把它画在画面上), 也是 M2 分片组包的依据。
     */
    uint64_t frameId = 0;

    RawFrame() = default;

    // 见类注释: 隐式拷贝是性能陷阱, 直接从类型层面堵死
    RawFrame(const RawFrame&) = delete;
    RawFrame& operator=(const RawFrame&) = delete;

    //让移动构造和移动赋值默认创建
    RawFrame(RawFrame&&) = default;
    RawFrame& operator=(RawFrame&&) = default;

    /**
     * @brief 按分辨率与像素格式重置, 并调整缓冲区大小
     *
     * @param w 宽, 约束随格式而定(YUV420P 要求偶数)
     * @param h 高, 约束随格式而定(YUV420P 要求偶数)
     * @param f 像素格式, 默认 YUV420P
     *
     * @note 本函数**与格式无关**: 尺寸计算全部委托给 frameBytes(), 参数合法性
     *          也由它按格式各自 assert。所以加一种新格式时**不用动这里**,
     *          只需在 frameBytes() 的 switch 里加一个 case —— 那是全项目
     *          唯一知道"某格式一帧多少字节"的地方。
     * @note 采集线程会**反复复用同一个 RawFrame**, 靠 vector::resize 在容量够时
     *          不重新分配这一点, 把每帧一次 malloc/free 省掉。分辨率不变时
     *          这个函数没有任何堆操作; 格式或分辨率变了才可能重新分配。
     * @note 不清零内容: 调用方紧接着就要把整帧写满, 先清零纯属多跑一遍 memset。
     */
    void reset(int w, int h, PixelFormat f = PixelFormat::YUV420P) {
        width  = w;
        height = h;
        fmt    = f;
        data.resize(frameBytes(w, h, f));
    }

    /**
     * @brief 断言当前帧是**平面格式**, 平面访问器的共同前提
     *
     * @note 下面这一组 y() u() v() ySize() uvSize() yStride() uvStride() 描述的是
     *          "三平面依次紧凑排布"这一种布局, 只对 YUV420P 成立。
     *          将来加入 BGRA 这类**打包格式**(像素交错, 根本没有 U/V 平面)后,
     *          调它们就是语义错误 —— 靠这个 assert 在 Debug 期当场拦下,
     *          Release 下(NDEBUG)整组调用被编译掉, 零开销。
     *          真要支持打包格式, 入口在这里: 按 fmt 分派出各自的一组访问器。
     */
    void assertPlanar() const {
        assert(fmt == PixelFormat::YUV420P && "plane accessors are planar-format only");
    }

    /**
     * @brief Y(亮度)平面起始地址
     *
     * @return 指向 data 首字节
     */
    uint8_t* y() { assertPlanar(); return data.data(); }
    const uint8_t* y() const { assertPlanar(); return data.data(); }

    /**
     * @brief U(蓝色差)平面起始地址
     *
     * @return 指向 Y 平面之后
     */
    uint8_t* u() { assertPlanar(); return data.data() + ySize(); }
    const uint8_t* u() const { assertPlanar(); return data.data() + ySize(); }

    /**
     * @brief V(红色差)平面起始地址
     *
     * @return 指向 U 平面之后
     */
    uint8_t* v() { assertPlanar(); return data.data() + ySize() + uvSize(); }
    const uint8_t* v() const { assertPlanar(); return data.data() + ySize() + uvSize(); }

    /**
     * @brief Y 平面字节数, 也是 U 平面的偏移量
     *
     * @return width * height
     */
    size_t ySize() const {
        assertPlanar();
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    /**
     * @brief 单个色度平面的字节数
     *
     * @return width/2 * height/2
     */
    size_t uvSize() const { assertPlanar(); return ySize() / 4; }

    /**
     * @brief Y 平面行跨度
     *
     * @return width
     *
     * @note 这里紧凑排布所以 stride == width。喂给 libavcodec 时**不能**照搬这个假设:
     *          AVFrame 的 linesize 会为了 SIMD 对齐比 width 大, 必须逐行按 linesize 拷贝。
     */
    int yStride() const { assertPlanar(); return width; }

    /**
     * @brief 色度平面行跨度
     *
     * @return width / 2
     */
    int uvStride() const { assertPlanar(); return width / 2; }
};
