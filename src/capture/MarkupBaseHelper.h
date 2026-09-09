#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MarkupBaseHelper.h — 截图标注底图提取与 1:1 像素无损对齐管线
//
// 核心职责:
//   1. 保证选区底图提取的绝对 1:1 像素保真度，即使选区因窗口阴影包含负坐标或超出屏幕边界，
//      也创建严格等于 targetRect 宽高的画布，并将屏幕像素 1:1 拷贝至正确偏移处，彻底根除拉伸变形。
//   2. 提供光标屏幕物理坐标向标注画布坐标的 1:1 严格对齐映射 (calculateMarkupPoint)。
//   3. 提供 Direct2D 渲染预览目标矩形与源矩形的 1:1 整数对齐计算 (calculateMarkupDestRect / calculateMarkupSrcRect)。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_MARKUPBASEHELPER_H
#define TOOLS3000_CAPTURE_MARKUPBASEHELPER_H

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <d2d1.h>
#include <windows.h>
#include <algorithm>
#include <cmath>

#include "MarkupEngine.h"

namespace tools3000::capture {

/// 从冻结屏幕中提取目标选区底图，生成尺寸严格等于 targetRect.width x targetRect.height 的无损画布。
/// 若 targetRect 存在负坐标或部分越界（如窗口阴影或边缘贴附），屏幕交叉区域无损放置于 (srcRoi.x - targetRect.x, srcRoi.y - targetRect.y)，
/// 确保无论选区几何如何变化，画面物理像素 100% 1:1 真实还原，彻底消除拉伸变形。
inline cv::Mat cropMarkupBase(const cv::Mat& frozenScreen, const cv::Rect& targetRect) {
    if (frozenScreen.empty() || targetRect.width <= 0 || targetRect.height <= 0) {
        return {};
    }

    cv::Mat canvas = cv::Mat::zeros(targetRect.height, targetRect.width, CV_8UC3);
    const cv::Rect screenBounds(0, 0, frozenScreen.cols, frozenScreen.rows);
    const cv::Rect srcRoi = targetRect & screenBounds;

    if (srcRoi.area() > 0) {
        const cv::Rect dstRoi(srcRoi.x - targetRect.x, srcRoi.y - targetRect.y, srcRoi.width, srcRoi.height);
        cv::Mat srcPart = frozenScreen(srcRoi);
        if (srcPart.channels() == 4) {
            cv::cvtColor(srcPart, canvas(dstRoi), cv::COLOR_BGRA2BGR);
        } else if (srcPart.channels() == 3) {
            srcPart.copyTo(canvas(dstRoi));
        } else if (srcPart.channels() == 1) {
            cv::cvtColor(srcPart, canvas(dstRoi), cv::COLOR_GRAY2BGR);
        }
    }

    return canvas;
}

/// 底图状态与选区几何绝对同步器 (单一事实源 SSOT)
/// 保证选区底图提取的绝对 1:1 像素保真度，并在几何变动时统筹平移已有标注图元
inline void syncMarkupBase(
    const cv::Mat& frozenScreen,
    const cv::Rect& targetRect,
    bool& markupBaseReady,
    cv::Rect& markupBaseRect,
    MarkupEngine& markup) {
    if (frozenScreen.empty() || targetRect.width <= 0 || targetRect.height <= 0) {
        return;
    }

    // 如果底图已就绪、几何尺寸与选区完全吻合，且已有非空底图，直接复用
    if (markupBaseReady && markupBaseRect == targetRect && markup.hasBaseImage() &&
        markup.baseWidth() == targetRect.width && markup.baseHeight() == targetRect.height) {
        return;
    }

    cv::Mat canvas = cropMarkupBase(frozenScreen, targetRect);
    if (canvas.empty()) {
        return;
    }

    if (markupBaseReady && markup.hasAnyMarkup()) {
        int dx = markupBaseRect.x - targetRect.x;
        int dy = markupBaseRect.y - targetRect.y;
        if (dx != 0 || dy != 0) {
            markup.translateAll(dx, dy);
        }
        markup.updateBaseImage(canvas);
    } else {
        markup.setBaseImage(canvas);
    }

    markupBaseRect = targetRect;
    markupBaseReady = true;
}

/// 计算屏幕物理坐标在标注画布上的局部相对坐标，与 cropMarkupBase 画布绝对 1:1 对应
inline cv::Point calculateMarkupPoint(POINT screenPoint, const cv::Rect& targetRect) noexcept {
    const int width = (std::max)(1, targetRect.width);
    const int height = (std::max)(1, targetRect.height);
    const int x = std::clamp(static_cast<int>(screenPoint.x - targetRect.x), 0, width - 1);
    const int y = std::clamp(static_cast<int>(screenPoint.y - targetRect.y), 0, height - 1);
    return {x, y};
}

/// 计算 Direct2D 标注位图在屏幕宿主上的绘制目标矩形，强制对齐整数物理像素，尺寸与位图 1:1 绝对对齐
inline D2D1_RECT_F calculateMarkupDestRect(const D2D1_RECT_F& selectionRect, UINT32 bitmapWidth, UINT32 bitmapHeight) noexcept {
    const float destLeft = std::round(selectionRect.left);
    const float destTop  = std::round(selectionRect.top);
    return D2D1::RectF(
        destLeft,
        destTop,
        destLeft + static_cast<float>(bitmapWidth),
        destTop  + static_cast<float>(bitmapHeight)
    );
}

/// 计算 Direct2D 标注位图绘制时的源矩形，确保点对点无损贴图
inline D2D1_RECT_F calculateMarkupSrcRect(UINT32 bitmapWidth, UINT32 bitmapHeight) noexcept {
    return D2D1::RectF(
        0.0f,
        0.0f,
        static_cast<float>(bitmapWidth),
        static_cast<float>(bitmapHeight)
    );
}

/// 窗口检测边界安全夹取：排除 DWM 隐形阴影产生的负坐标与屏幕越界，确保选区完全落在有效屏幕内
inline cv::Rect clampToScreenBounds(const RECT& winRect, int screenWidth, int screenHeight) noexcept {
    if (screenWidth <= 0 || screenHeight <= 0) return {0, 0, 0, 0};
    const int l = (std::clamp)(static_cast<int>(winRect.left), 0, screenWidth);
    const int t = (std::clamp)(static_cast<int>(winRect.top), 0, screenHeight);
    const int r = (std::clamp)(static_cast<int>(winRect.right), 0, screenWidth);
    const int b = (std::clamp)(static_cast<int>(winRect.bottom), 0, screenHeight);
    const int w = (std::max)(0, r - l);
    const int h = (std::max)(0, b - t);
    return {l, t, w, h};
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_MARKUPBASEHELPER_H
