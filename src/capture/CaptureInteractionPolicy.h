#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREINTERACTIONPOLICY_H
#define TOOLS3000_CAPTURE_CAPTUREINTERACTIONPOLICY_H

#include "capture/CaptureState.h"
#include <algorithm>
#include <cmath>

namespace tools3000::capture {

/// 截图标注会话中的鼠标左键双击意图枚举（单一事实源防误退策略）
enum class DoubleClickIntent {
    ConsumeNoOp = 0,              // 安全吞掉消息（工具栏间隙底板、选区外暗色遮罩、终结按钮防重入等），绝不退出
    ForwardPointerDown,           // 转发为普通左键按下（滑块预设、尺寸菜单、微晶下拉菜单连续点选）
    RepeatToolbarAction,          // 触发工具栏普通属性按钮连点（如快速连点放大倍率、线宽、颜色、撤销/重做）
    ToggleSelectionCornerRadius,  // 双击选区圆角把手切换直角/圆角
    EditOrSelectMarkupElement,    // 命中已有标注图元或其控制手柄（进入文本重编辑或选中图元）
    BeginAnnotationAtPoint,       // 标注工具激活态下快速连点画布空白处（连续打序号/放置放大镜/取消选中后立即下笔）
    ConfirmSelectionAndCopy,      // 纯框选待命态下双击选区内部空白处快速完成截图并复制
};

inline bool isPointInsideRectF(POINT pt, const D2D1_RECT_F& rc) noexcept {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return false;
    const float px = static_cast<float>(pt.x);
    const float py = static_cast<float>(pt.y);
    return px >= rc.left && px <= rc.right && py >= rc.top && py <= rc.bottom;
}

inline bool isTerminalToolbarCommand(ToolbarCommand cmd) noexcept {
    switch (cmd) {
        case ToolbarCommand::Confirm:
        case ToolbarCommand::Copy:
        case ToolbarCommand::Cancel:
        case ToolbarCommand::ExtractText:
        case ToolbarCommand::PinWindow:
        case ToolbarCommand::ScrollCapture:
        case ToolbarCommand::StartRecord:
        case ToolbarCommand::RecordStartConfirm:
            return true;
        default:
            return false;
    }
}

/// 判定双击事件的唯一权威路由策略，彻底阻断标注创作或点击工具栏时误触发自动保存退出
inline DoubleClickIntent resolveDoubleClickIntent(
    const CaptureState& state,
    POINT pt,
    bool inSelection,
    HitArea selBoxHit,
    bool hitAnyMarkupElement,
    const ToolbarButton* hitButton) noexcept {

    // 1. 弹出式滑块、尺寸单位菜单、微晶下拉菜单优先响应连续点击
    if (state.sliderPopup.type != SliderPopupType::None && isPointInsideRectF(pt, state.sliderPopup.popupRect)) {
        return DoubleClickIntent::ForwardPointerDown;
    }
    if (state.isSizeMenuOpen && isPointInsideRectF(pt, state.sizeMenuRect)) {
        return DoubleClickIntent::ForwardPointerDown;
    }
    if (isPointInsideRectF(pt, state.sizeHudRect) ||
        (!state.detectedQrText.empty() && isPointInsideRectF(pt, state.qrChipRect))) {
        return DoubleClickIntent::ConsumeNoOp;
    }
    if (state.dropdownMenu.type != DropdownType::None && isPointInsideRectF(pt, state.dropdownMenu.menuRect)) {
        return DoubleClickIntent::ForwardPointerDown;
    }

    // 2. 工具栏按钮与悬浮微晶玻璃底板绝对豁免，严禁穿透到底层触发自动保存退出
    if (hitButton != nullptr) {
        return isTerminalToolbarCommand(hitButton->command)
            ? DoubleClickIntent::ConsumeNoOp
            : DoubleClickIntent::RepeatToolbarAction;
    }
    if (isPointInsideRectF(pt, state.primaryToolbarRect) ||
        isPointInsideRectF(pt, state.secondaryToolbarRect) ||
        isPointInsideRectF(pt, state.selectionSideRect) ||
        (state.openSubmenu != SubmenuType::None && isPointInsideRectF(pt, state.openSubmenuRect))) {
        return DoubleClickIntent::ConsumeNoOp;
    }

    // 3. 选区圆角把手双击切换；选区外暗色遮罩与选区边缘缩放手柄绝对禁止触发退出
    if (selBoxHit == HitArea::CornerRadius) {
        return DoubleClickIntent::ToggleSelectionCornerRadius;
    }
    if (!inSelection || (selBoxHit != HitArea::None && selBoxHit != HitArea::Body)) {
        return DoubleClickIntent::ConsumeNoOp;
    }

    // 4. 命中当前激活图元手柄或画布已有图元：进入编辑/选中
    if (hitAnyMarkupElement) {
        return DoubleClickIntent::EditOrSelectMarkupElement;
    }

    // 5. 标注创作态防误退守卫：只要激活了任意标注工具，画布快速连点一律作为正常下笔操作
    if (state.isMarkupToolActive) {
        return DoubleClickIntent::BeginAnnotationAtPoint;
    }

    // 6. 仅在未激活任何标注工具的纯框选待命态下，双击选区内部空白处执行快速确认截图
    return DoubleClickIntent::ConfirmSelectionAndCopy;
}

/// 智能计算零遮挡引出式放大镜的最佳空白区透镜圆心（8 向净空评分，斜向黄金角度优先且确保透镜不超出选区边界）
inline cv::Point computeSmartMagnifierCalloutCenter(
    cv::Point srcPt, int lensRadius, float zoomScale, int selW, int selH, float dpiScale) noexcept {
    const float dpi = dpiScale > 0.0f ? dpiScale : 1.0f;
    const int r = (std::max)(20, lensRadius);
    const float z = std::clamp(zoomScale, 1.1f, 8.0f);
    const int srcRingR = std::clamp(static_cast<int>(std::lround(r / z)), 8, (std::max)(10, (r * 2) / 3));
    const double idealDist = static_cast<double>(r + srcRingR) + 24.0 * static_cast<double>(dpi);
    const int margin = static_cast<int>(std::lround(6.0f * dpi));

    auto clampLensX = [&](int x) {
        const int lo = r + margin;
        const int hi = selW - r - margin;
        return (lo <= hi) ? std::clamp(x, lo, hi) : (selW / 2);
    };
    auto clampLensY = [&](int y) {
        const int lo = r + margin;
        const int hi = selH - r - margin;
        return (lo <= hi) ? std::clamp(y, lo, hi) : (selH / 2);
    };

    struct CandidateDir { double ux; double uy; double preferenceBonus; };
    static constexpr CandidateDir kDirs[] = {
        { 0.78, -0.62, 18.0}, // 右上（第一黄金位：同时避开同行文字与上下紧邻行）
        { 0.78,  0.62, 16.0}, // 右下
        {-0.78, -0.62, 14.0}, // 左上
        {-0.78,  0.62, 12.0}, // 左下
        { 1.00,  0.00,  6.0}, // 正右
        {-1.00,  0.00,  4.0}, // 正左
        { 0.00, -1.00,  2.0}, // 正上
        { 0.00,  1.00,  0.0}, // 正下
    };

    cv::Point bestPt(clampLensX(srcPt.x + static_cast<int>(std::lround(idealDist * 0.78))),
                     clampLensY(srcPt.y - static_cast<int>(std::lround(idealDist * 0.62))));
    double bestScore = -1e18;

    for (const auto& dir : kDirs) {
        const int rawX = srcPt.x + static_cast<int>(std::lround(dir.ux * idealDist));
        const int rawY = srcPt.y + static_cast<int>(std::lround(dir.uy * idealDist));
        const int cx = clampLensX(rawX);
        const int cy = clampLensY(rawY);
        const double actualDist = std::hypot(static_cast<double>(cx - srcPt.x),
                                             static_cast<double>(cy - srcPt.y));
        const double clampPenalty = std::hypot(static_cast<double>(cx - rawX),
                                               static_cast<double>(cy - rawY)) * 1.35;
        const double nonOverlapBonus = (actualDist >= static_cast<double>(r + srcRingR + 6)) ? 500.0 : 0.0;
        const double score = nonOverlapBonus + actualDist - clampPenalty + dir.preferenceBonus;
        if (score > bestScore) {
            bestScore = score;
            bestPt = cv::Point(cx, cy);
        }
    }
    return bestPt;
}

/// 解析计算从中心沿单位方向向量 (ux, uy) 到圆形或圆角正方形外轮廓的精确径向距离（用于引线零穿透接驳）
inline float computeLoupeBoundaryDistance(
    float ux, float uy, float halfRadius, float cornerRadius, bool isRoundedSquare) noexcept {
    if (!isRoundedSquare) {
        return halfRadius;
    }
    const float r = std::max(1.0f, halfRadius);
    const float c = std::clamp(cornerRadius, 0.0f, r);
    const float b = r - c;
    const float ax = std::abs(ux);
    const float ay = std::abs(uy);
    if (ax > 1e-5f) {
        const float tx = r / ax;
        if (tx * ay <= b + 1e-4f) return tx;
    }
    if (ay > 1e-5f) {
        const float ty = r / ay;
        if (ty * ax <= b + 1e-4f) return ty;
    }
    const float p = b * (ax + ay);
    const float disc = std::max(0.0f, c * c - b * b * (ax - ay) * (ax - ay));
    return p + std::sqrt(disc);
}

/// 解析放大镜图元的实际透镜圆心（兼容同心原位与引出分离两种形态）
inline cv::Point resolveMagnifierLensCenter(const MarkupElement& elem) noexcept {
    if (!elem.hasLeaderArrow && elem.endPt.x == 0 && elem.endPt.y == 0 &&
        (elem.startPt.x != 0 || elem.startPt.y != 0)) {
        return elem.startPt;
    }
    return elem.endPt;
}

/// 判定选区局部坐标点是否处于指定放大镜图元的交互编辑热区内（含透镜本体、外接框全部 8 向缩放控制点、右上引出靶心与引出源取景环）
inline bool isPointNearMagnifierEditZone(const MarkupElement& elem, cv::Point localPt, float dpiScale) noexcept {
    if (elem.tool != MarkupTool::Magnifier) {
        return false;
    }
    const float dpi = (dpiScale > 0.0f) ? dpiScale : ((elem.dpiScale > 0.0f) ? elem.dpiScale : 1.0f);
    const cv::Point lc = resolveMagnifierLensCenter(elem);
    const int r = (std::max)(20, elem.magnifierRadius);
    const int pad = static_cast<int>(std::lround(16.0f * dpi));

    // 覆盖透镜外接正方形及四角/四边中点全部 8 向缩放手柄与引出靶心手柄
    if (localPt.x >= lc.x - r - pad && localPt.x <= lc.x + r + pad &&
        localPt.y >= lc.y - r - pad && localPt.y <= lc.y + r + pad) {
        return true;
    }

    // 引出分离形态下同步覆盖取景源小环及舒适外扩区
    const bool isOffset = std::hypot(static_cast<double>(lc.x - elem.startPt.x),
                                     static_cast<double>(lc.y - elem.startPt.y)) > 8.0;
    if (isOffset) {
        const float zoom = std::clamp(elem.magnifierScale, 1.1f, 8.0f);
        const int srcRingR = std::clamp(static_cast<int>(std::lround(r / zoom)), 8, (std::max)(10, (r * 2) / 3));
        const double srcPad = static_cast<double>(srcRingR) + 12.0 * static_cast<double>(dpi);
        if (std::hypot(static_cast<double>(localPt.x - elem.startPt.x),
                       static_cast<double>(localPt.y - elem.startPt.y)) <= srcPad) {
            return true;
        }
    }
    return false;
}

/// 查找当前选区局部坐标下悬停命中的已有放大镜图元（激活图元优先，其次按图层倒序自顶向下匹配）
inline MarkupElement* findHoveredMagnifierElement(const CaptureState& state, cv::Point localPt) noexcept {
    const float dpi = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    if (state.activeElement && state.activeElement->isActive &&
        state.activeElement->tool == MarkupTool::Magnifier) {
        if (state.activeElement->hitTestEx(localPt) != HitArea::None ||
            isPointNearMagnifierEditZone(*state.activeElement, localPt, dpi)) {
            return state.activeElement;
        }
    }
    const auto& elems = state.markup.elements();
    for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
        MarkupElement* elem = it->get();
        if (!elem || elem->tool != MarkupTool::Magnifier) continue;
        if (elem->hitTestEx(localPt) != HitArea::None ||
            isPointNearMagnifierEditZone(*elem, localPt, dpi)) {
            return elem;
        }
    }
    return nullptr;
}

/// 判定光标是否靠近选区外框的 8 向拉伸手柄或四角圆角调节手柄
inline bool isPointNearSelectionControlHandles(const CaptureState& state, POINT pt) noexcept {
    const float sx1 = static_cast<float>((std::min)(state.dragStart.x, state.dragEnd.x));
    const float sy1 = static_cast<float>((std::min)(state.dragStart.y, state.dragEnd.y));
    const float sx2 = static_cast<float>((std::max)(state.dragStart.x, state.dragEnd.x));
    const float sy2 = static_cast<float>((std::max)(state.dragStart.y, state.dragEnd.y));
    if (sx2 - sx1 < 4.0f || sy2 - sy1 < 4.0f) return false;

    const float dpi = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    const float hitR = 12.0f * dpi;
    const float mx = 0.5f * (sx1 + sx2);
    const float my = 0.5f * (sy1 + sy2);
    const D2D1_POINT_2F handles[8] = {
        {sx1, sy1}, {mx, sy1}, {sx2, sy1}, {sx2, my},
        {sx2, sy2}, {mx, sy2}, {sx1, sy2}, {sx1, my}
    };
    const float px = static_cast<float>(pt.x);
    const float py = static_cast<float>(pt.y);
    for (const auto& h : handles) {
        if (std::abs(px - h.x) <= hitR && std::abs(py - h.y) <= hitR) {
            return true;
        }
    }
    return false;
}

/// 判定是否应自动隐藏跟随光标的动态放大镜预览：
/// 当光标悬停在已有放大镜（含 8 向缩放控制点与源环）、其他已有标注图元或选区控制手柄上时立即退避隐藏，
/// 完整无遮挡露出图元与编辑手柄；光标移回空白画布时返回 false 恢复连续标注预览。
inline bool shouldSuppressDynamicMagnifierPreview(const CaptureState& state, POINT pt) noexcept {
    if (isPointNearSelectionControlHandles(state, pt)) {
        return true;
    }
    const int sx1 = (std::min)(state.dragStart.x, state.dragEnd.x);
    const int sy1 = (std::min)(state.dragStart.y, state.dragEnd.y);
    const cv::Point localPt(pt.x - sx1, pt.y - sy1);

    if (findHoveredMagnifierElement(state, localPt) != nullptr) {
        return true;
    }
    if (state.activeElement && state.activeElement->isActive &&
        state.activeElement->hitTestEx(localPt) != HitArea::None) {
        return true;
    }
    if (state.markup.getElementAtEx(localPt).element != nullptr) {
        return true;
    }
    return false;
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREINTERACTIONPOLICY_H
