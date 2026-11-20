#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositorInternal.h — 演播室视频合成器私有跨编译单元协作声明
//
// 职责:
//   仅供 StudioVideoCompositor*.cpp 内部拆分编译单元共享：
//   - 微信 GIF 安全体积/帧数常量与通用圆角/Alpha 混合/色值/质量系数辅助声明
//   - 时间轴标注画布渲染入口声明（StudioVideoCompositor.h 公开 ABI 零改动）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITORINTERNAL_H
#define TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITORINTERNAL_H

#include "capture/StudioVideoCompositor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::capture::compositor_detail {

inline constexpr uint64_t kWeChatGifSafeMaxBytes = 4'850'000ULL;
inline constexpr int kWeChatGifMaxDimension = 800;
inline constexpr int kWeChatGifMaxFrames = 260;

// 唯一定义位于 StudioVideoCompositor.cpp
void fillRoundedRectMask(cv::Mat& maskU8, cv::Rect rect, float radius);
void alphaBlendPremultipliedBgraOntoBgr(const cv::Mat& srcBgra, cv::Mat& dstBgr, int dstX, int dstY);
double qualityScaleFactor(StudioExportQuality q) noexcept;
cv::Scalar parseHexBgrScalar(const std::string& hex, const cv::Scalar& fallbackBgr) noexcept;

// 唯一定义位于 StudioVideoCompositorAnnotations.cpp
void drawRoundedBoxStrokeBgr(cv::Mat& img, cv::Rect rc, int radius, const cv::Scalar& col, int thick);
void renderTimelineAnnotationsOnInner(
    cv::Mat& innerBgr,
    const cv::Mat& fullSrcBgr,
    const std::vector<StudioAnnotationItem>& annotations,
    int64_t ptsMs,
    float srcCols, float srcRows,
    float effCropX, float effCropY,
    float effCropW, float effCropH,
    bool animateAnnotations = true,
    int64_t annotationPreviewElapsedMs = -1,
    int previewTargetAnnIndex = -1);

} // namespace tools3000::capture::compositor_detail

#endif // TOOLS3000_CAPTURE_STUDIOVIDEOCOMPOSITORINTERNAL_H
