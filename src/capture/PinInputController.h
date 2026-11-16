#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinInputController.h — 贴图窗口键盘快捷键策略分发与几何变换中枢 (SSOT)
//
// 职责:
//   1. 表驱动/策略分发解析贴图窗口按键 (WM_KEYDOWN / WM_SYSKEYDOWN)，消灭冗长 if-else；
//   2. 统一承载旋转 (R / Shift+R / Ctrl+R / L / 1 / 2 / Ctrl+Left/Right / Alt+Left/Right)、
//      镜像翻转 (H / V / 3 / 4)、透明度调节 (5~9 / [ / ] / 0)、缩放 (+ / - / 0)、
//      OCR 文字全选 (Ctrl+A) 与方向键/WASD 像素级微调移动等全部贴图快捷键规则；
//   3. 在 OCR 文字划选模式或持有文字选区时自动屏蔽单字母/Ctrl+R 误触旋转，保护文字选区；
//   4. 统一封装贴图原图与灰度备份矩阵的 90°/180°/270° 无损旋转与水平/垂直翻转算法；
//   5. 统一定义贴图悬停自动获焦与点击激活判定契约，保障截图钉住后或鼠标指向时按键即刻生效。
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
    inline constexpr float kMinToolbarPinWidthDip = 260.0f;
    inline constexpr float kMinToolbarPinHeightDip = 108.0f;
    inline constexpr float kMinToolbarTextCardHeightDip = 140.0f;
    inline constexpr float kDragStartThresholdDip = 4.0f;
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
    SelectAllText,
    CopyAllText,
    CopySelectedOrImage,
    ExtractText,
    SaveImage,
    HistoryPrev,
    HistoryNext,
    AssignOrToggleGroup,
    RemoveFromGroup,
    AutoGroupUngrouped,
    FilterByGroup,
    ShowAllGroups,
    EscapeOrClose,
    ClosePin,
};

/// 贴图按键解析决策结果
struct PinShortcutDecision {
    PinShortcutAction action = PinShortcutAction::None;
    int rotateDegrees = 0;
    int groupIndex = 0;
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

/// 贴图窗口鼠标指针形态枚举 (映射至 Win32 IDC_SIZEALL / IDC_HAND / IDC_IBEAM)
enum class PinCursorKind {
    MoveWindow = 0, // IDC_SIZEALL: 空白处平移拖拽贴图窗口
    HandButton = 1, // IDC_HAND:    悬浮工具栏、微晶操作胶囊或二维码角标
    TextBeam   = 2, // IDC_IBEAM:   原图实景智能 I 型文本选字指针
};

/// 贴图窗口鼠标左键按下动作决策枚举
enum class PinLeftPressMode {
    PressInteractiveButton = 0, // 命中按钮：进入 Drag-First 按钮候选
    StartLiveTextSelection = 1, // 命中已识别文字热区（或按住 Alt）：原图直接划字选词
    StartWindowDrag        = 2, // 命中无字空白处：清除已有选区并直接拖动贴图窗口
};

/// 贴图原图实景选字 (Live Text) 输入命中快照
struct PinLiveTextState {
    bool isFolded = false;
    bool isDragging = false;
    bool isSelectingText = false;
    bool hoverInteractiveButton = false;
    bool altHeld = false;
    bool textSelectionMode = false;
    bool liveTextEnabled = true;
    bool ocrReady = false;
    bool hitTextBlock = false;
};

class PinInputController {
public:
    /// 解析贴图窗口 WM_KEYDOWN / WM_SYSKEYDOWN 消息，返回强类型动作决策
    /// 当 hasActiveTextContext 为 true（处于 OCR 文字选择态或已有选中文字块）时，
    /// 自动屏蔽 'R' / 'L' 系的快捷旋转以防止误触清空文字选区。
    static PinShortcutDecision resolveKeyDown(WPARAM wParam,
                                              bool ctrl,
                                              bool shift,
                                              bool alt,
                                              bool hasActiveTextContext = false) noexcept;

    /// 原图实景选字 (Live Text) 光标状态机决策：
    /// 无需按住 Alt 或手动切模式，悬停文字框自动返回 TextBeam (IDC_IBEAM)，空白处返回 MoveWindow (IDC_SIZEALL)
    static PinCursorKind resolveCursorKind(const PinLiveTextState& state) noexcept;

    /// 原图实景选字 (Live Text) 鼠标左键按下决策：
    /// 命中文字框时直接启动流式选字，命中空白处时直接拖动贴图窗口，命中工具按钮时走 Drag-First 候选
    static PinLeftPressMode resolveLeftPressMode(const PinLiveTextState& state,
                                                 bool hitInteractiveButton) noexcept;

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

    /// 判定当前贴图尺寸及卡片类型在常规鼠标悬停时是否应浮现 8 按钮工具栏：
    /// 小尺寸贴图 (width < 260*scale 或 height < 108*scale) 及紧凑微晶字签 (isTextCard && height < 140*scale)
    /// 默认不弹出遮挡正文的顶部工具栏（全部功能由右键菜单与快捷键承载），保障视线零遮挡与随手即拖。
    static bool shouldShowHoverToolbar(float windowWidth,
                                       float windowHeight,
                                       float dpiScale,
                                       bool isTextCard = false) noexcept;

    /// 判定鼠标在贴图窗口内移动时是否应自动赋予键盘焦点
    static bool shouldAutoFocusOnHover(bool isWindowFocused,
                                       bool isClickThrough,
                                       bool isOverlayVisible) noexcept;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PININPUTCONTROLLER_H
