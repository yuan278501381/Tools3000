#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngineInternal.h — 截图标注引擎私有跨编译单元协作声明
//
// 职责:
//   仅供 MarkupEngine*.cpp 内部拆分编译单元共享：
//   - 唯一 GDI+ 启动辅助、平滑样条、花样直线、文本高亮/下划线渲染辅助与统一聚光灯渲染声明
//   - 几何图元与特效图元 Handler 工厂注册入口（MarkupEngine.h 公开 ABI 零改动）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_MARKUPENGINEINTERNAL_H
#define TOOLS3000_CAPTURE_MARKUPENGINEINTERNAL_H

#include "capture/MarkupEngine.h"
#include <opencv2/core.hpp>
#include <vector>

namespace tools3000::capture::markup_detail {

// 唯一定义位于 MarkupEngine.cpp
std::vector<cv::Point> generateSmoothSpline(const std::vector<cv::Point>& pts, int stepsPerSegment = 10);
void ensureGdiplusStartup();
bool isGdiplusStarted() noexcept;

// 唯一定义位于 MarkupEngineShapes.cpp
void drawPatternedLine(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style);
void renderTextSpansHighlightImpl(cv::Mat& canvas,
                                  const std::vector<cv::Rect>& spans,
                                  const MarkupColor& color);
void renderTextSpansUnderlineImpl(cv::Mat& canvas,
                                  const std::vector<cv::Rect>& spans,
                                  const MarkupColor& color,
                                  LineStyle style,
                                  float explicitThickness);
void registerShapeMarkupHandlers(MarkupToolRegistry& registry);

// 唯一定义位于 MarkupEngineEffects.cpp
void renderUnifiedSpotlights(cv::Mat& canvas, const std::vector<const MarkupElement*>& spotlights);
void registerEffectMarkupHandlers(MarkupToolRegistry& registry);

} // namespace tools3000::capture::markup_detail

#endif // TOOLS3000_CAPTURE_MARKUPENGINEINTERNAL_H
