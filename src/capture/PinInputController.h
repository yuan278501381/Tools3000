#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinInputController.h — 贴图窗口键盘快捷键策略分发与几何变换中枢 (SSOT)
//
// 职责:
//   1. 表驱动/策略分发解析贴图窗口按键 (WM_KEYDOWN / WM_SYSKEYDOWN)，消灭冗长 if-else；
//   2. 统一承载旋转 (R / Shift+R / Ctrl+R / L / 1 / 2 / Ctrl+Left/Right / Alt+Left/Right)、
//      镜像翻转 (H / V / 3 / 4)、透明度调节 (5~9 / [ / ] / 0)、缩放 (+ / - / 0) 与
//      方向键/WASD 像素级微调移动等全部贴图快捷键规则；
//   3. 统一封装贴图原图与灰度备份矩阵的 90°/180°/270° 无损旋转与水平/垂直翻转算法；
//   4. 统一定义贴图悬停自动获焦与点击激活判定契约，保障截图钉住后或鼠标指向时按键即刻生效。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PININPUTCONTROLLER_H
#define TOOLS3000_CAPTURE_PININPUTCONTROLLER_H

#include <windows.h>
#include <d2d1.h>
#include <opencv2/core.hpp>

namespace tools3000::capture {

/// 贴图窗口交互常量定义 (杜绝魔术字面量)
namespace PinInteractionConstants {
    inline constexpr int kRotateClockwiseDeg = 90;
    inline constexpr int kRotateHalfTurnDeg = 180;
    inline constexpr int kRotateCounterClockwiseDeg = 270;
    inline constexpr int kFullCircleDeg = 360;

    inline constexpr float kMinPinScale = 0.1f;
    inline constexpr float kMaxPinScale = 5.0f;
    inline constexpr float kDefaultPinScale = 1.0f;
    inline constexpr float kZoomInFactor = 1.1f;
    inline constexpr float kZoomOutFactor = 0.9f;

    inline constexpr float kMinPinOpacity = 0.1f;
    inline constexpr float kMaxPinOpacity = 1.0f;
    inline constexpr float kOpacityStep = 0.1f;

    inline constexpr int kNudgeStepNormalPx = 1;
    inline constexpr int kNudgeStepFastPx = 10;

    inline constexpr int kHoverToolbarButtonCount = 8;
} // namespace PinInteractionConstants

/// 贴图快捷键标准动作枚举
enum class PinShortcutAction {
    None = 0,
    RotateClockwise90,
    RotateCounterClockwise90,
    FlipHorizontal,
    FlipVertical,
    SetOpacity,
    AdjustOpacity,
    ResetScaleAndOpacity,
    ZoomIn,
    ZoomOut,
    Nudge,
    EditMarkup,
    InvertColors,
    ToggleGrayscale,
    ToggleFold,
    ToggleClickThrough,
    PasteHistoryOrClipboard,
    CopyAllText,
    CopySelectedOrImage,
    ExtractText,
    SaveImage,
    HistoryPrev,
    HistoryNext,
    EscapeOrClose,
    ClosePin,
};

/// 贴图按键解析决策结果
struct PinShortcutDecision {
    PinShortcutAction action = PinShortcutAction::None;
    int rotateDegrees = 0;
    float opacityValue = PinInteractionConstants::kMaxPinOpacity;
    float opacityDelta = 0.0f;
    float scaleFactor = PinInteractionConstants::kDefaultPinScale;
    int nudgeDx = 0;
    int nudgeDy = 0;
};

/// 贴图悬浮工具栏自适应布局结果 (支持窄窗口自动多行网格与缩放避让)
struct PinHoverToolbarLayout {
    D2D1_RECT_F toolbarRect{};
    D2D1_RECT_F buttons[PinInteractionConstants::kHoverToolbarButtonCount]{};
    int columns = PinInteractionConstants::kHoverToolbarButtonCount;
    int rows = 1;
};

class PinInputController {
public:
    /// 解析贴图窗口 WM_KEYDOWN / WM_SYSKEYDOWN 消息，返回强类型动作决策
    static PinShortcutDecision resolveKeyDown(WPARAM wParam, bool ctrl, bool shift, bool alt) noexcept;

    /// 将任意角度归一化为 [0, 360) 范围内的正交角度 (90 / 180 / 270)，非法角度返回 0
    static int normalizeRightAngleDegrees(int angleDeg) noexcept;

    /// 对贴图源矩阵及色彩备份矩阵执行正交旋转，并同步更新宽高
    static bool applyRotation(cv::Mat& sourceImage,
                              cv::Mat& colorBackup,
                              int& origWidth,
                              int& origHeight,
                              int angleDeg);

    /// 对贴图源矩阵及色彩备份矩阵执行水平或垂直镜像翻转
    static bool applyFlip(cv::Mat& sourceImage,
                          cv::Mat& colorBackup,
                          bool horizontal);

    /// 计算正交旋转后的窗口目标矩形：以当前贴图中心为锚点原地旋转，并钳制在显示器工作区内防止越界
    static RECT calculateRotatedWindowRect(const RECT& currentRect,
                                           int newWidth,
                                           int newHeight,
                                           const RECT& workArea) noexcept;

    /// 计算悬浮工具栏及其 8 个按钮的自适应几何布局（窄贴图自动折行/紧凑缩放，保障按钮永不越界裁切）
    static PinHoverToolbarLayout computeHoverToolbarLayout(float windowWidth,
                                                           float windowHeight,
                                                           float dpiScale) noexcept;

    /// 计算悬浮工具栏下方微晶快捷键提示胶囊矩形（自动根据窗口宽高钳制与避让）
    static D2D1_RECT_F computeTooltipRect(const D2D1_RECT_F& toolbarRect,
                                          float anchorX,
                                          float windowWidth,
                                          float windowHeight,
                                          float dpiScale) noexcept;

    /// 判定鼠标在贴图窗口内移动时是否应自动赋予键盘焦点
    static bool shouldAutoFocusOnHover(bool isWindowFocused,
                                       bool isClickThrough,
                                       bool isOverlayVisible) noexcept;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PININPUTCONTROLLER_H
