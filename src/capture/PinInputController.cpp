// ─────────────────────────────────────────────────────────────────────────────
// PinInputController.cpp — 贴图窗口键盘快捷键策略分发与几何变换中枢实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinInputController.h"
#include <opencv2/core.hpp>
#include <algorithm>
#include <utility>

namespace tools3000::capture {

using namespace PinInteractionConstants;

namespace {

struct SimpleKeyRule {
    WPARAM key;
    bool requireCtrl;
    bool forbidCtrl;
    bool requireShift;
    bool forbidShift;
    PinShortcutAction action;
};

constexpr SimpleKeyRule kActionTable[] = {
    {VK_ESCAPE, false, false, false, false, PinShortcutAction::EscapeOrClose},
    {'W',       true,  false, false, false, PinShortcutAction::ClosePin},
    {VK_DELETE, false, true,  false, true,  PinShortcutAction::ClosePin},
    {'E',       false, true,  false, false, PinShortcutAction::EditMarkup},
    {'I',       false, true,  false, false, PinShortcutAction::InvertColors},
    {'G',       false, true,  false, false, PinShortcutAction::ToggleGrayscale},
    {'M',       false, true,  false, false, PinShortcutAction::ToggleFold},
    {'T',       false, true,  false, true,  PinShortcutAction::ToggleClickThrough},
    {VK_F4,     false, true,  false, true,  PinShortcutAction::ToggleClickThrough},
    {'L',       false, true,  false, false, PinShortcutAction::RotateCounterClockwise90},
    {'1',       false, true,  false, true,  PinShortcutAction::RotateCounterClockwise90},
    {VK_NUMPAD1,false, true,  false, true,  PinShortcutAction::RotateCounterClockwise90},
    {'2',       false, true,  false, true,  PinShortcutAction::RotateClockwise90},
    {VK_NUMPAD2,false, true,  false, true,  PinShortcutAction::RotateClockwise90},
    {'3',       false, true,  false, true,  PinShortcutAction::FlipHorizontal},
    {VK_NUMPAD3,false, true,  false, true,  PinShortcutAction::FlipHorizontal},
    {'H',       false, true,  false, false, PinShortcutAction::FlipHorizontal},
    {'4',       false, true,  false, true,  PinShortcutAction::FlipVertical},
    {VK_NUMPAD4,false, true,  false, true,  PinShortcutAction::FlipVertical},
    {'V',       false, true,  false, false, PinShortcutAction::FlipVertical},
    {'V',       true,  false, false, true,  PinShortcutAction::PasteHistoryOrClipboard},
    {'C',       true,  false, true,  false, PinShortcutAction::CopyAllText},
    {'C',       true,  false, false, true,  PinShortcutAction::CopySelectedOrImage},
    {'T',       true,  false, false, false, PinShortcutAction::ExtractText},
    {'S',       true,  false, false, false, PinShortcutAction::SaveImage},
    {VK_OEM_PLUS,   false, false, false, false, PinShortcutAction::ZoomIn},
    {VK_ADD,        false, false, false, false, PinShortcutAction::ZoomIn},
    {'=',           false, false, false, false, PinShortcutAction::ZoomIn},
    {VK_OEM_MINUS,  false, false, false, false, PinShortcutAction::ZoomOut},
    {VK_SUBTRACT,   false, false, false, false, PinShortcutAction::ZoomOut},
    {'-',           false, false, false, false, PinShortcutAction::ZoomOut},
    {VK_OEM_4,      false, false, false, false, PinShortcutAction::AdjustOpacity},
    {VK_OEM_6,      false, false, false, false, PinShortcutAction::AdjustOpacity},
    {'0',           false, true,  false, false, PinShortcutAction::ResetScaleAndOpacity},
    {VK_NUMPAD0,    false, true,  false, false, PinShortcutAction::ResetScaleAndOpacity},
    {VK_OEM_COMMA,  false, true,  false, false, PinShortcutAction::HistoryPrev},
    {VK_OEM_PERIOD, false, true,  false, false, PinShortcutAction::HistoryNext},
};

} // namespace

PinShortcutDecision PinInputController::resolveKeyDown(WPARAM wParam, bool ctrl, bool shift, bool alt) noexcept {
    PinShortcutDecision decision;

    // 1. Ctrl/Alt + 左右方向键: 逆时针 / 顺时针旋转 90 度
    if ((ctrl || alt) && wParam == VK_LEFT) {
        decision.action = PinShortcutAction::RotateCounterClockwise90;
        decision.rotateDegrees = kRotateCounterClockwiseDeg;
        return decision;
    }
    if ((ctrl || alt) && wParam == VK_RIGHT) {
        decision.action = PinShortcutAction::RotateClockwise90;
        decision.rotateDegrees = kRotateClockwiseDeg;
        return decision;
    }
    if (alt && wParam == VK_F4) {
        decision.action = PinShortcutAction::ClosePin;
        return decision;
    }
    if (alt) {
        return decision;
    }

    // 2. R / Shift+R / Ctrl+R / Ctrl+Shift+R: 顺时针 / 逆时针旋转 90 度
    if (wParam == 'R') {
        decision.action = shift ? PinShortcutAction::RotateCounterClockwise90
                                : PinShortcutAction::RotateClockwise90;
        decision.rotateDegrees = shift ? kRotateCounterClockwiseDeg : kRotateClockwiseDeg;
        return decision;
    }

    // 3. 5~9 / 小键盘 5~9: 快速设置不透明度 50%~90%
    if (!ctrl && !shift &&
        ((wParam >= '5' && wParam <= '9') || (wParam >= VK_NUMPAD5 && wParam <= VK_NUMPAD9))) {
        const int digit = (wParam >= VK_NUMPAD5 && wParam <= VK_NUMPAD9)
            ? static_cast<int>(wParam - VK_NUMPAD0)
            : static_cast<int>(wParam - '0');
        decision.action = PinShortcutAction::SetOpacity;
        decision.opacityValue = static_cast<float>(digit) / 10.0f;
        return decision;
    }

    // 4. 方向键 / WASD: 像素级精准微调位置 (Shift 加速 10px)
    if (!ctrl && (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN ||
                  wParam == 'W' || wParam == 'A' || wParam == 'S' || wParam == 'D')) {
        const int step = shift ? kNudgeStepFastPx : kNudgeStepNormalPx;
        decision.action = PinShortcutAction::Nudge;
        if (wParam == VK_LEFT || wParam == 'A') decision.nudgeDx = -step;
        else if (wParam == VK_RIGHT || wParam == 'D') decision.nudgeDx = step;
        else if (wParam == VK_UP || wParam == 'W') decision.nudgeDy = -step;
        else if (wParam == VK_DOWN || wParam == 'S') decision.nudgeDy = step;
        return decision;
    }

    // 5. 表驱动匹配通用规则
    for (const auto& rule : kActionTable) {
        if (rule.key != wParam) continue;
        if (rule.requireCtrl && !ctrl) continue;
        if (rule.forbidCtrl && ctrl) continue;
        if (rule.requireShift && !shift) continue;
        if (rule.forbidShift && shift) continue;

        decision.action = rule.action;
        if (rule.action == PinShortcutAction::RotateClockwise90) {
            decision.rotateDegrees = kRotateClockwiseDeg;
        } else if (rule.action == PinShortcutAction::RotateCounterClockwise90) {
            decision.rotateDegrees = kRotateCounterClockwiseDeg;
        } else if (rule.action == PinShortcutAction::ZoomIn) {
            decision.scaleFactor = kZoomInFactor;
        } else if (rule.action == PinShortcutAction::ZoomOut) {
            decision.scaleFactor = kZoomOutFactor;
        } else if (rule.action == PinShortcutAction::AdjustOpacity) {
            decision.opacityDelta = (wParam == VK_OEM_4) ? -kOpacityStep : kOpacityStep;
        }
        return decision;
    }

    return decision;
}

int PinInputController::normalizeRightAngleDegrees(int angleDeg) noexcept {
    const int norm = ((angleDeg % kFullCircleDeg) + kFullCircleDeg) % kFullCircleDeg;
    if (norm == kRotateClockwiseDeg || norm == kRotateHalfTurnDeg || norm == kRotateCounterClockwiseDeg) {
        return norm;
    }
    return 0;
}

bool PinInputController::applyRotation(cv::Mat& sourceImage,
                                       cv::Mat& colorBackup,
                                       int& origWidth,
                                       int& origHeight,
                                       int angleDeg) {
    if (sourceImage.empty()) return false;
    const int norm = normalizeRightAngleDegrees(angleDeg);
    if (norm == 0) return false;

    if (norm == kRotateClockwiseDeg) {
        cv::rotate(sourceImage, sourceImage, cv::ROTATE_90_CLOCKWISE);
        if (!colorBackup.empty()) {
            cv::rotate(colorBackup, colorBackup, cv::ROTATE_90_CLOCKWISE);
        }
        std::swap(origWidth, origHeight);
    } else if (norm == kRotateHalfTurnDeg) {
        cv::rotate(sourceImage, sourceImage, cv::ROTATE_180);
        if (!colorBackup.empty()) {
            cv::rotate(colorBackup, colorBackup, cv::ROTATE_180);
        }
    } else if (norm == kRotateCounterClockwiseDeg) {
        cv::rotate(sourceImage, sourceImage, cv::ROTATE_90_COUNTERCLOCKWISE);
        if (!colorBackup.empty()) {
            cv::rotate(colorBackup, colorBackup, cv::ROTATE_90_COUNTERCLOCKWISE);
        }
        std::swap(origWidth, origHeight);
    }
    return true;
}

bool PinInputController::applyFlip(cv::Mat& sourceImage,
                                   cv::Mat& colorBackup,
                                   bool horizontal) {
    if (sourceImage.empty()) return false;
    const int flipCode = horizontal ? 1 : 0;
    cv::flip(sourceImage, sourceImage, flipCode);
    if (!colorBackup.empty()) {
        cv::flip(colorBackup, colorBackup, flipCode);
    }
    return true;
}

RECT PinInputController::calculateRotatedWindowRect(const RECT& currentRect,
                                                    int newWidth,
                                                    int newHeight,
                                                    const RECT& workArea) noexcept {
    const int safeW = (std::max)(1, newWidth);
    const int safeH = (std::max)(1, newHeight);
    const int oldW = currentRect.right - currentRect.left;
    const int oldH = currentRect.bottom - currentRect.top;

    // 以当前贴图几何中心为锚点原地正交旋转
    int left = currentRect.left + (oldW - safeW) / 2;
    int top = currentRect.top + (oldH - safeH) / 2;

    if (workArea.right > workArea.left && workArea.bottom > workArea.top) {
        const int maxLeft = (std::max)(static_cast<int>(workArea.left), static_cast<int>(workArea.right) - safeW);
        const int maxTop = (std::max)(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom) - safeH);
        left = std::clamp(left, static_cast<int>(workArea.left), maxLeft);
        top = std::clamp(top, static_cast<int>(workArea.top), maxTop);
    }

    return RECT{left, top, left + safeW, top + safeH};
}

PinHoverToolbarLayout PinInputController::computeHoverToolbarLayout(float windowWidth,
                                                                    float windowHeight,
                                                                    float dpiScale) noexcept {
    PinHoverToolbarLayout out{};
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const float safeWinW = (std::max)(16.0f, windowWidth);
    const float safeWinH = (std::max)(16.0f, windowHeight);

    float btnW = 26.0f * safeScale;
    float btnH = 24.0f * safeScale;
    float btnPad = 4.0f * safeScale;
    float tbPadding = 8.0f * safeScale;
    if (safeWinW < 64.0f * safeScale || safeWinH < 48.0f * safeScale) {
        tbPadding = 2.0f * safeScale;
    }

    const float availW = (std::max)(12.0f, safeWinW - 2.0f * tbPadding);
    const float availH = (std::max)(12.0f, safeWinH - 2.0f * tbPadding);

    // 候选网格布局 (cols x rows): 优先 8x1 -> 4x2 -> 2x4 -> 1x8
    constexpr int kCandidateCols[] = {8, 4, 2, 1};
    int chosenCols = 8;
    int chosenRows = 1;

    for (int cols : kCandidateCols) {
        const int rows = (kHoverToolbarButtonCount + cols - 1) / cols;
        const float reqW = cols * btnW + (cols + 1) * btnPad;
        const float reqH = rows * btnH + (rows + 1) * btnPad;
        if (reqW <= availW && reqH <= availH) {
            chosenCols = cols;
            chosenRows = rows;
            break;
        }
        // 若宽度不够但高度充足（例如窄竖条贴图 40x400），继续尝试更少列数
        if (reqH <= availH) {
            chosenCols = cols;
            chosenRows = rows;
        }
    }

    const float rawW = chosenCols * btnW + (chosenCols + 1) * btnPad;
    const float rawH = chosenRows * btnH + (chosenRows + 1) * btnPad;

    // 若所选网格仍超出窄小窗口尺寸，则等比紧凑缩放按钮与间距，确保 8 个按钮 100% 落在贴图内部
    const float fitScaleW = (rawW > availW && rawW > 0.0f) ? (availW / rawW) : 1.0f;
    const float fitScaleH = (rawH > availH && rawH > 0.0f) ? (availH / rawH) : 1.0f;
    const float fitScale = (std::min)(fitScaleW, fitScaleH);
    if (fitScale < 1.0f) {
        btnW *= fitScale;
        btnH *= fitScale;
        btnPad *= fitScale;
    }

    const float tbWidth = chosenCols * btnW + (chosenCols + 1) * btnPad;
    const float tbHeight = (chosenRows == 1 && fitScale >= 1.0f)
        ? (32.0f * safeScale)
        : (chosenRows * btnH + (chosenRows + 1) * btnPad);

    float tx = safeWinW - tbWidth - tbPadding;
    if (tx < tbPadding) {
        tx = (std::max)(0.0f, (safeWinW - tbWidth) * 0.5f);
    }
    float ty = (std::min)(tbPadding, (std::max)(0.0f, (safeWinH - tbHeight) * 0.5f));

    out.toolbarRect = D2D1::RectF(tx, ty, tx + tbWidth, ty + tbHeight);
    out.columns = chosenCols;
    out.rows = chosenRows;

    const float rowPitch = (chosenRows == 1)
        ? 0.0f
        : (btnH + btnPad);
    const float firstRowY = (chosenRows == 1)
        ? (ty + (tbHeight - btnH) * 0.5f)
        : (ty + btnPad);

    for (int i = 0; i < kHoverToolbarButtonCount; ++i) {
        const int r = i / chosenCols;
        const int c = i % chosenCols;
        const float bx = tx + btnPad + c * (btnW + btnPad);
        const float by = firstRowY + r * rowPitch;
        out.buttons[i] = D2D1::RectF(bx, by, bx + btnW, by + btnH);
    }

    return out;
}

D2D1_RECT_F PinInputController::computeTooltipRect(const D2D1_RECT_F& toolbarRect,
                                                   float anchorX,
                                                   float windowWidth,
                                                   float windowHeight,
                                                   float dpiScale) noexcept {
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const float margin = 4.0f * safeScale;
    const float tipH = 22.0f * safeScale;
    const float tipY = toolbarRect.bottom + margin;

    if (windowWidth < 48.0f * safeScale || tipY + tipH + margin > windowHeight) {
        return D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const float tipW = (std::min)(118.0f * safeScale, (std::max)(40.0f * safeScale, windowWidth - 2.0f * margin));
    const float maxTipX = (std::max)(margin, windowWidth - tipW - margin);
    const float tipX = std::clamp(anchorX - tipW * 0.5f, margin, maxTipX);
    return D2D1::RectF(tipX, tipY, tipX + tipW, tipY + tipH);
}

bool PinInputController::shouldAutoFocusOnHover(bool isWindowFocused,
                                                bool isClickThrough,
                                                bool isOverlayVisible) noexcept {
    return !isWindowFocused && !isClickThrough && !isOverlayVisible;
}

} // namespace tools3000::capture
