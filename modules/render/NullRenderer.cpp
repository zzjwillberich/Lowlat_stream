/**
 * @file    NullRenderer.cpp
 * @brief   NullRenderer.h 的实现
 * @author  zzj
 * @date    2026-08-22
 */

#include "modules/render/NullRenderer.h"

NullRenderer::~NullRenderer() {
    close();
}

Status NullRenderer::open(const RendererConfig& cfg) {
    // TODO(zzj):
    //   1. 校验 cfg.width > 0 && cfg.height > 0, 不合法返回 Status::error(Code::InvalidArg, ...)
    //      —— 校验就在这里做, 不要单独开 validateConfig()
    //   2. config_ = cfg;
    //   3. stats_ = {};  计数器归零, 语义是"自最近一次 open 起累积"
    //   4. 重置跨帧状态: hasPrev_ = false;
    //   5. opened_ = true;
    //
    // @note 这里**不会**因为环境失败 —— 它压根不碰 SDL, 这正是它存在的理由。
    //       所以它永远不返回 IoError。
    (void)cfg;
    return Status::ok();
}

Status NullRenderer::renderFrame(const RawFrame& frame) {
    // TODO(zzj):
    //   1. !opened_ → Status::error(Code::Closed, ...)
    //   2. checkFrame(frame) 不过 → ++stats_.framesRejected; 直接返回那个 Status
    //   3. checkAgainstPrevious(frame) 不过 → 同上
    //   4. 尺寸和上一帧不同(或还没有上一帧) → ++stats_.texturesRebuilt
    //      —— 这里没有真纹理, 计这个数是为了和 SdlRenderer 的行为对齐,
    //         让"稳态下应该恒为 1"这条断言在两个实现上都能验
    //   5. 记住这一帧: hasPrev_/prevWidth_/prevHeight_/prevCaptureMs_/prevFrameId_
    //      注意 prevCaptureMs_ 只在 frame.captureMs != 0 时更新, 别把哨兵值存进去
    //   6. ++stats_.framesRendered; return Status::ok();
    //
    // @note 坏帧只计数不中断: 30fps 下丢一帧是 33ms, 人眼察觉不到; 偶发 1 帧是噪声,
    //       每秒 30 帧全坏才是上游炸了 —— 计数器就是用来区分这两件事的。
    // @note 但计数器自己**不是防线** —— 它不会让测试变红。闭环在单测里:
    //       EXPECT_EQ(renderer->stats().framesRejected, 0)。
    // @note 也不要在这里打日志: 坏帧真的连续起来会刷屏, 而且污染 M3.4 的延迟测量(NOTES.md D1)。
    (void)frame;
    return Status::ok();
}

bool NullRenderer::pumpEvents() {
    // 恒 false: 它**真的**没有事件源, 这不是空实现。
    // 没有窗口就没有 SDL_QUIT, 退出只能由 Ctrl-C 或发送端静默超时来触发。
    return false;
}

const RendererStats& NullRenderer::stats() const {
    return stats_;
}

void NullRenderer::close() {
    // TODO(zzj): opened_ = false; hasPrev_ = false;
    //
    // @note 没有资源要释放, 但仍然要**幂等** —— 析构函数会再调一次。
    // @note stats_ **不要**在这里清零: 调用方 close() 之后还要读统计做断言。
    //       归零的时机是 open()。
}

Status NullRenderer::checkFrame(const RawFrame& frame) const {
    // TODO(zzj): 见头文件里列的四条
    (void)frame;
    return Status::ok();
}

Status NullRenderer::checkAgainstPrevious(const RawFrame& frame) const {
    // TODO(zzj): 见头文件里列的两条; 注意 !hasPrev_ 时直接放行
    (void)frame;
    return Status::ok();
}
