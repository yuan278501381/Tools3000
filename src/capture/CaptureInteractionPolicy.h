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
    if (state.dropdownMenu.type != DropdownType::None &&
        (isPointInsideRectF(pt, state.dropdownMenu.menuRect) ||
         isPointInsideRectF(pt, state.dropdownMenu.childMenuRect))) {
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

/// 解析计算从中心沿单位方向向量 (ux, uy) 到椭圆 (rx, ry) 或圆角矩形外轮廓的精确径向距离（用于引线零穿透接驳）
inline float computeLoupeBoundaryDistance(
    float ux, float uy, float rx, float ry, float cornerRadius, bool isRoundedSquare) noexcept {
    const float safeRx = (std::max)(1.0f, rx);
    const float safeRy = (std::max)(1.0f, ry);
    const float ax = std::abs(ux);
    const float ay = std::abs(uy);
    if (!isRoundedSquare) {
        const float nx = ax / safeRx;
        const float ny = ay / safeRy;
        const float denom = std::hypot(nx, ny);
        return (denom > 1e-6f) ? (1.0f / denom) : (std::min)(safeRx, safeRy);
    }
    const float maxC = (std::min)(safeRx, safeRy);
    const float c = std::clamp(cornerRadius, 0.0f, maxC);
    const float bx = safeRx - c;
    const float by = safeRy - c;
    if (ax > 1e-5f) {
        const float tx = safeRx / ax;
        if (tx * ay <= by + 1e-4f) return tx;
    }
    if (ay > 1e-5f) {
        const float ty = safeRy / ay;
        if (ty * ax <= bx + 1e-4f) return ty;
    }
    const float p = bx * ax + by * ay;
    const float cross = bx * ay - by * ax;
    const float disc = (std::max)(0.0f, c * c - cross * cross);
    return p + std::sqrt(disc);
}

inline float computeLoupeBoundaryDistance(
    float ux, float uy, float halfRadius, float cornerRadius, bool isRoundedSquare) noexcept {
    return computeLoupeBoundaryDistance(ux, uy, halfRadius, halfRadius, cornerRadius, isRoundedSquare);
}

/// 解析放大镜图元的实际透镜圆心（兼容同心原位与引出分离两种形态）
inline cv::Point resolveMagnifierLensCenter(const MarkupElement& elem) noexcept {
    if (!elem.hasLeaderArrow && elem.endPt.x == 0 && elem.endPt.y == 0 &&
        (elem.startPt.x != 0 || elem.startPt.y != 0)) {
        return elem.startPt;
    }
    return elem.endPt;
}

/// 判定选区局部坐标点是否处于指定放大镜图元的交互编辑热区内（含椭圆/圆透镜本体、外接框全部 8 向缩放控制点、右上引出靶心与源中心靶点）
inline bool isPointNearMagnifierEditZone(const MarkupElement& elem, cv::Point localPt, float dpiScale) noexcept {
    if (elem.tool != MarkupTool::Magnifier) {
        return false;
    }
    const float dpi = (dpiScale > 0.0f) ? dpiScale : ((elem.dpiScale > 0.0f) ? elem.dpiScale : 1.0f);
    const cv::Point lc = resolveMagnifierLensCenter(elem);
    const int rx = elem.effectiveMagnifierRx();
    const int ry = elem.effectiveMagnifierRy();
    const int pad = static_cast<int>(std::lround(16.0f * dpi));

    // 覆盖透镜外接矩形及四角/四边中点全部 8 向缩放手柄与引出靶心手柄
    if (localPt.x >= lc.x - rx - pad && localPt.x <= lc.x + rx + pad &&
        localPt.y >= lc.y - ry - pad && localPt.y <= lc.y + ry + pad) {
        return true;
    }

    // 引出分离形态下同步覆盖被放大源中心靶点热区
    const bool isOffset = std::hypot(static_cast<double>(lc.x - elem.startPt.x),
                                     static_cast<double>(lc.y - elem.startPt.y)) > 8.0;
    if (isOffset) {
        const double srcPad = 24.0 * static_cast<double>(dpi);
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
    if (state.isDrawingMultiPointLine) {
        return true;
    }
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

/// 直线工具多点连续点击折线 / 美感曲线状态机策略中枢（单一事实源，供 CaptureInput 与离线单测共用）
struct MultiPointLinePolicy {
    static void startFromFirstClick(CaptureState& state, POINT firstDocPt) {
        state.isDrawingMultiPointLine = true;
        state.multiPointDocVertices.clear();
        state.multiPointDocVertices.push_back(firstDocPt);
        state.markupStart = firstDocPt;
        state.markupEnd = firstDocPt;
        state.isMarking = true;
        state.state = OverlayState::Marking;
    }

    /// 追加下一个点击锚点；若点击在起点磁吸圈内 (>=3 点) 自动闭合并返回 true 表示应立即收尾提交
    static bool appendVertexOrCloseLoop(CaptureState& state, POINT rawDocPt, bool shiftSnap45) {
        POINT nextDocPt = rawDocPt;
        if (!state.multiPointDocVertices.empty() && shiftSnap45) {
            const POINT refPt = state.multiPointDocVertices.back();
            const double dx = static_cast<double>(nextDocPt.x - refPt.x);
            const double dy = static_cast<double>(nextDocPt.y - refPt.y);
            const double dist = std::hypot(dx, dy);
            if (dist >= 1.0) {
                const double angle = std::atan2(dy, dx);
                constexpr double step = 3.14159265358979323846 / 4.0;
                const double snappedAngle = std::round(angle / step) * step;
                nextDocPt.x = static_cast<LONG>(std::lround(refPt.x + dist * std::cos(snappedAngle)));
                nextDocPt.y = static_cast<LONG>(std::lround(refPt.y + dist * std::sin(snappedAngle)));
            }
        }
        const double effScale = state.dpiScale > 0.0f ? static_cast<double>(state.dpiScale) : 1.0;
        if (state.multiPointDocVertices.size() >= 3) {
            const POINT firstPt = state.multiPointDocVertices.front();
            if (std::hypot(static_cast<double>(nextDocPt.x - firstPt.x),
                           static_cast<double>(nextDocPt.y - firstPt.y)) <= 10.0 * effScale) {
                state.multiPointDocVertices.push_back(firstPt);
                state.markupEnd = firstPt;
                return true;
            }
        }
        if (state.multiPointDocVertices.empty() ||
            std::hypot(static_cast<double>(nextDocPt.x - state.multiPointDocVertices.back().x),
                       static_cast<double>(nextDocPt.y - state.multiPointDocVertices.back().y)) >= 2.0) {
            state.multiPointDocVertices.push_back(nextDocPt);
        }
        state.markupEnd = nextDocPt;
        return false;
    }

    /// 撤回上一个锚点；若锚点已清空则返回 true 表示退出多点草稿态
    static bool popLastVertex(CaptureState& state) noexcept {
        if (!state.multiPointDocVertices.empty()) {
            state.multiPointDocVertices.pop_back();
        }
        return state.multiPointDocVertices.empty();
    }

    static void applyCurveModeButton(CaptureState& state, int buttonIntParam) noexcept {
        if (buttonIntParam == 1) {
            state.currentSmoothCurve = false;
        } else if (buttonIntParam == 2) {
            state.currentSmoothCurve = true;
        } else {
            state.currentSmoothCurve = !state.currentSmoothCurve;
        }
    }
};

/// 判定某类标注图元是否支持通过滚轮调节其视觉属性（粗细 / 字号 / 块大小 / 暗度 / 倍率）
inline bool supportsWheelAttributeAdjustment(MarkupTool tool) noexcept {
    switch (tool) {
        case MarkupTool::Rectangle:
        case MarkupTool::Ellipse:
        case MarkupTool::Line:
        case MarkupTool::Arrow:
        case MarkupTool::Pen:
        case MarkupTool::Text:
        case MarkupTool::TextReplace:
        case MarkupTool::Number:
        case MarkupTool::Mosaic:
        case MarkupTool::Blur:
        case MarkupTool::Inpaint:
        case MarkupTool::Spotlight:
        case MarkupTool::Watermark:
        case MarkupTool::Magnifier:
            return true;
        case MarkupTool::Highlight:
        default:
            return false;
    }
}

/// 判定某类标注图元是否支持通过 Shift+滚轮 调节圆角半径
inline bool supportsWheelCornerRadiusAdjustment(MarkupTool tool) noexcept {
    return tool == MarkupTool::Rectangle || tool == MarkupTool::Spotlight;
}

/// 计算长图缩放视图下命中选中标注图元的文档像素容差：
/// 保证屏幕上恒定约 11 DIP 舒适瞄准带，并夹紧至 [6, 36] 避免极低倍率误拦截大片画布
inline int computeLongCanvasElementWheelHitPadding(float dpiScale, float longZoom) noexcept {
    const float safeDpi = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const float safeZoom = std::clamp(longZoom, 0.15f, 4.0f);
    const int docPad = static_cast<int>(std::ceil((11.0f * safeDpi) / safeZoom));
    return std::clamp(docPad, 6, 36);
}

/// 将屏幕客户区坐标无钳制映射为标注画布局部坐标，杜绝长图外侧留白区因边界 clamp 误命中靠边图元
inline cv::Point screenToUnclampedMarkupPoint(const CaptureState& state, POINT screenPt) noexcept {
    const auto doc = state.screenToDoc(static_cast<float>(screenPt.x), static_cast<float>(screenPt.y));
    const int selLeft = (std::min)(state.dragStart.x, state.dragEnd.x);
    const int selTop = (std::min)(state.dragStart.y, state.dragEnd.y);
    return cv::Point(
        static_cast<int>(std::lround(doc.x)) - selLeft,
        static_cast<int>(std::lround(doc.y)) - selTop);
}

/// 判定屏幕坐标是否落在长图模式下“当前已选中”的标注图元之上（不含未选中图元，空心框不拦截内部留白）
inline bool isPointOnActiveLongImageElement(const CaptureState& state, POINT screenPt) noexcept {
    if (!state.isLongImageMode()) {
        return false;
    }
    const MarkupElement* elem = state.activeElement;
    if (!elem || !elem->isActive || !supportsWheelAttributeAdjustment(elem->tool)) {
        return false;
    }
    const auto& stg = state.longCanvas.stageRect;
    if (stg.right > stg.left && stg.bottom > stg.top) {
        if (screenPt.x < stg.left || screenPt.x > stg.right ||
            screenPt.y < stg.top || screenPt.y > stg.bottom) {
            return false;
        }
    }

    const cv::Point localPt = screenToUnclampedMarkupPoint(state, screenPt);
    const int docPad = computeLongCanvasElementWheelHitPadding(state.dpiScale, state.longCanvas.zoom);
    if (elem->hitTestEx(localPt, docPad) != HitArea::None) {
        return true;
    }
    if (elem->tool == MarkupTool::Magnifier) {
        const float safeDpi = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
        const float safeZoom = std::clamp(state.longCanvas.zoom, 0.15f, 4.0f);
        const float effEditDpi = std::clamp(safeDpi / safeZoom, 1.0f, 3.0f);
        return isPointNearMagnifierEditZone(*elem, localPt, effEditDpi);
    }
    return false;
}

/// 长图编辑模式滚轮路由决策结果（单一事实源）
enum class LongImageWheelRoute {
    ToolbarOrPopupAttribute, // 悬停工具栏或展开滑块：调工具/弹窗属性
    CanvasZoom,              // Ctrl+滚轮：以光标为锚点无级缩放长图画布
    ActiveMagnifier,         // 悬停在已选中的放大镜上：调倍率 / Shift 调半径
    ActiveElementAttribute,  // 悬停在已选中的普通标注上：调粗细/字号/强度/圆角
    CanvasScrollHorizontal,  // Shift+滚轮（非圆角图元或空白处）：横向滚动长图
    CanvasScrollVertical,    // 普通滚轮（空白处或未选中图元）：丝滑上下翻阅长图
};

inline LongImageWheelRoute resolveLongImageWheelRoute(
    const CaptureState& state,
    POINT screenPt,
    bool hasHoveredToolbar,
    bool ctrlDown,
    bool shiftDown) noexcept {
    if (state.sliderPopup.type != SliderPopupType::None || hasHoveredToolbar) {
        return LongImageWheelRoute::ToolbarOrPopupAttribute;
    }
    if (ctrlDown) {
        return LongImageWheelRoute::CanvasZoom;
    }
    if (isPointOnActiveLongImageElement(state, screenPt)) {
        const MarkupTool activeTool = state.activeElement->tool;
        if (activeTool == MarkupTool::Magnifier) {
            return LongImageWheelRoute::ActiveMagnifier;
        }
        if (shiftDown && !supportsWheelCornerRadiusAdjustment(activeTool)) {
            return LongImageWheelRoute::CanvasScrollHorizontal;
        }
        return LongImageWheelRoute::ActiveElementAttribute;
    }
    return shiftDown
        ? LongImageWheelRoute::CanvasScrollHorizontal
        : LongImageWheelRoute::CanvasScrollVertical;
}

/// 以被选中图元自身当前参数作为滚轮步进基值，防止点选历史细标注时被全局粗细带偏突跳
inline int resolveActiveElementStrokeBaseWidth(
    const MarkupElement& elem, int fallbackStrokeWidth, int maxW) noexcept {
    if (elem.tool == MarkupTool::Mosaic && elem.mosaicBlockSize > 0) {
        return std::clamp(static_cast<int>(std::lround(elem.mosaicBlockSize / 2.0f)), 1, maxW);
    }
    if (elem.tool == MarkupTool::Blur && elem.mosaicBlockSize > 0) {
        return std::clamp(static_cast<int>(std::lround(elem.mosaicBlockSize / 3.0f)), 1, maxW);
    }
    if (elem.tool == MarkupTool::Inpaint && elem.inpaintRadius > 0) {
        return std::clamp(elem.inpaintRadius, 1, maxW);
    }
    if (elem.thickness > 0.0f) {
        return std::clamp(static_cast<int>(std::lround(elem.thickness)), 1, maxW);
    }
    return std::clamp(fallbackStrokeWidth, 1, maxW);
}

enum class WheelAdjustedKind {
    None,
    StrokeWidth,
    StepSize,
    MosaicSize,
    BlurStrength,
    InpaintRadius,
    CornerRadius,
    SpotlightDim,
    TextFontSize,
    TextReplaceFontSize,
    MagnifierScale,
    MagnifierRadius,
    WatermarkSize,
};

struct WheelAdjustmentOutcome {
    WheelAdjustedKind kind = WheelAdjustedKind::None;
    int intValue = 0;
    float floatValue = 0.0f;
};

/// 单一事实源：按“已选中标注元素自身的实际类型”执行单步滚轮属性调节（供 CaptureInput 与单测共用）
inline WheelAdjustmentOutcome applyWheelAdjustmentToActiveElement(
    CaptureState& state,
    MarkupElement& elem,
    int delta,
    bool shiftDown,
    float dpiScale) noexcept {
    WheelAdjustmentOutcome out{};
    if (delta == 0 || !supportsWheelAttributeAdjustment(elem.tool)) {
        return out;
    }
    const float safeDpi = (dpiScale > 0.0f) ? dpiScale : 1.0f;

    if (elem.tool == MarkupTool::Magnifier) {
        if (shiftDown) {
            const int stepPx = (std::max)(4, static_cast<int>(std::lround(6.0f * safeDpi)));
            const int minR = static_cast<int>(std::lround(24.0f * safeDpi));
            const int oldRx = elem.effectiveMagnifierRx();
            const int oldRy = elem.effectiveMagnifierRy();
            const int deltaR = (delta > 0) ? stepPx : -stepPx;
            elem.magnifierRadius = std::clamp(oldRx + deltaR, minR, 420);
            elem.magnifierRadiusY = std::clamp(oldRy + deltaR, minR, 420);
            out.kind = WheelAdjustedKind::MagnifierRadius;
            out.intValue = elem.magnifierRadius;
            return out;
        }
        elem.magnifierScale = std::clamp(
            elem.magnifierScale + ((delta > 0) ? 0.25f : -0.25f), 1.5f, 6.0f);
        state.dynamicMagnifierScale = elem.magnifierScale;
        out.kind = WheelAdjustedKind::MagnifierScale;
        out.floatValue = elem.magnifierScale;
        return out;
    }

    if (shiftDown && supportsWheelCornerRadiusAdjustment(elem.tool)) {
        const float newRad = std::clamp(elem.cornerRadius + static_cast<float>(delta * 2), 0.0f, 40.0f);
        elem.cornerRadius = newRad;
        state.currentElementCornerRadius = newRad;
        out.kind = WheelAdjustedKind::CornerRadius;
        out.intValue = static_cast<int>(std::lround(newRad));
        return out;
    }

    if (elem.tool == MarkupTool::Spotlight) {
        const int curPct = static_cast<int>(std::round(elem.spotlightDimAlpha * 100.0f));
        const int newPct = std::clamp(curPct + delta * 3, 15, 85);
        elem.spotlightDimAlpha = static_cast<float>(newPct) / 100.0f;
        state.currentSpotlightDimAlpha = elem.spotlightDimAlpha;
        out.kind = WheelAdjustedKind::SpotlightDim;
        out.intValue = newPct;
        return out;
    }

    if (elem.tool == MarkupTool::Text) {
        const float newPhysSize = std::clamp(
            elem.fontSize + static_cast<float>(delta * 2), 10.0f, 144.0f * safeDpi);
        elem.fontSize = newPhysSize;
        elem.textRenderSize = cv::Size(0, 0);
        out.kind = WheelAdjustedKind::TextFontSize;
        out.floatValue = std::clamp(newPhysSize / safeDpi, 10.0f, 144.0f);
        out.intValue = static_cast<int>(std::lround(out.floatValue));
        return out;
    }

    if (elem.tool == MarkupTool::TextReplace) {
        const float newPhysSize = std::clamp(
            elem.fontSize + static_cast<float>(delta), 8.0f, 144.0f * safeDpi);
        elem.fontSize = newPhysSize;
        out.kind = WheelAdjustedKind::TextReplaceFontSize;
        out.floatValue = newPhysSize;
        out.intValue = static_cast<int>(std::lround(newPhysSize));
        return out;
    }

    const int maxW = (elem.tool == MarkupTool::Mosaic || elem.tool == MarkupTool::Blur) ? 32 : 28;
    const int baseW = resolveActiveElementStrokeBaseWidth(elem, state.currentStrokeWidth, maxW);
    const int newW = std::clamp(baseW + delta, 1, maxW);
    state.currentStrokeWidth = newW;
    elem.thickness = static_cast<float>(newW);
    if (elem.tool == MarkupTool::Mosaic) {
        elem.mosaicBlockSize = (std::max)(4, newW * 2);
        out.kind = WheelAdjustedKind::MosaicSize;
        out.intValue = elem.mosaicBlockSize;
    } else if (elem.tool == MarkupTool::Blur) {
        elem.mosaicBlockSize = (std::max)(5, newW * 3);
        out.kind = WheelAdjustedKind::BlurStrength;
        out.intValue = elem.mosaicBlockSize;
    } else if (elem.tool == MarkupTool::Inpaint) {
        elem.inpaintRadius = std::clamp(newW, 2, 16);
        out.kind = WheelAdjustedKind::InpaintRadius;
        out.intValue = elem.inpaintRadius;
    } else if (elem.tool == MarkupTool::Watermark) {
        elem.fontSize = std::clamp(12.0f + static_cast<float>(newW) * 1.5f, 12.0f, 64.0f) * safeDpi;
        out.kind = WheelAdjustedKind::WatermarkSize;
        out.intValue = static_cast<int>(std::lround(elem.fontSize / safeDpi));
    } else if (elem.tool == MarkupTool::Number) {
        out.kind = WheelAdjustedKind::StepSize;
        out.intValue = newW;
    } else {
        out.kind = WheelAdjustedKind::StrokeWidth;
        out.intValue = newW;
    }
    return out;
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREINTERACTIONPOLICY_H
