#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTURESHORTCUTPOLICY_H
#define TOOLS3000_CAPTURE_CAPTURESHORTCUTPOLICY_H

// ─────────────────────────────────────────────────────────────────────────────
// CaptureShortcutPolicy.h — 截图与区域录屏全阶段键盘快捷键单一事实源策略中枢
//
// 职责:
//   1. 彻底消除无选区寻点、拖拽框选、已选区微调、标注创作、富文本编辑 5 大阶段的按键冲突与遮蔽；
//   2. 实现 Shift 纯单按释放 (Tap-Only) 状态机，根治 Shift+组合键 / 长按拖拽误切换放大镜颜色格式；
//   3. 表驱动多态映射标注工具与预设色彩，严格隔离区域录屏模式与无选区模式的截图标注单键。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureState.h"
#include <algorithm>
#include <utility>

namespace tools3000::capture {

namespace CaptureShortcutConstants {
constexpr int kPixelStepFine = 1;
constexpr int kPixelStepFast = 10;
constexpr int kStrokeDeltaDown = -1;
constexpr int kStrokeDeltaUp = 1;
constexpr int kHierarchyDeltaShrink = -1;
constexpr int kHierarchyDeltaExpand = 1;
} // namespace CaptureShortcutConstants

/// 截图与区域录屏键盘动作强类型枚举
enum class CaptureShortcutAction {
    None = 0,
    DelegateToTextEditor,        // 文本原地编辑态下交由 CaptureTextEditor 处理

    // 历史回放穿梭
    ToggleHistoryMode,           // '/' 开关历史截图回放
    StepHistoryPrev,             // ',' 上一张历史截图
    StepHistoryNext,             // '.' 下一张历史截图
    ExitHistoryMode,             // 历史模式下 Esc 退出
    ConsumeInHistoryMode,        // 历史模式下屏蔽无关按键

    // 像素级光标与选区微调
    MoveCursorPixels,            // 无选区 / 拖拽框选中：WASD 或方向键移动鼠标光标 (dx, dy)
    NudgeSelectionOrElement,     // 已选区：WASD 或方向键微调移动选区或选中图元 (dx, dy)
    ResizeSelectionByKey,        // 已选区且无选中图元：Shift+WASD 或 Shift+方向键微调选区尺寸 (dx, dy)

    // 智能窗口探测子/父层级切换 (彻底解耦 W/S，零冲突绑定 Tab / [ / ] / PgUp / PgDn)
    StepWindowHierarchy,         // intDelta: +1 向父窗口扩展，-1 向子控件收缩

    // 像素取色放大镜 (WASD + C/K)
    CycleColorFormat,            // 纯单按并松开 Shift (Tap-Only)：循环切换 8 种颜色格式
    CopyCurrentPixelColor,       // C / K (无选区兼容 I)：复制当前放大镜显示的 colorFormat 色值
    CopyAlternatePixelColor,     // Shift+C / Shift+K (无选区兼容 Shift+I)：快捷复制互补色值 (HEX <-> RGB)

    // 选区几何与工具属性调节
    SelectFullScreen,            // Ctrl+A：一键全屏选区
    LockAspectRatio,             // Shift+1..5：锁定选区宽高比预设 (aspectPreset)
    AdjustCornerRadius,          // '[' / ']'：减小/增大选区或美化外壳圆角 (intDelta: -1 / +1)
    AdjustNumberCounter,         // 序号模式下 '[' / ']'：递减/递增起始序号或当前选中序号 (intDelta: -1 / +1)
    AdjustStrokeWidth,           // '+' / '-'：调节笔触线宽 / 字号 / 马赛克粒度 (intDelta: -1 / +1)

    // 截图标注工具与美化外壳控制 (仅在 Screenshot + hasSelection 下生效)
    ToggleMarkupTool,            // R/O/L/Q(Alt+A)/P/H/T/N/Z/G/M/E/I：切换标注工具 (tool)
    SelectPresetColor,           // 1..7：切换预设标注颜色 (color)
    ToggleBeautyShell,           // B：开关美化外壳
    CycleBeautyShellTheme,       // Shift+B：循环切换 5 种美化外壳质感背景
    CycleBeautyShellPadding,     // Alt+B：循环切换美化外壳留白边距

    // 图元编辑、撤销重做与会话完结动作
    ReEditSelectedText,          // 选中文本图元时按 Enter 或 F2 重新进入编辑
    DeleteSelectedElement,       // Delete / Backspace：删除当前选中标注图元
    UndoMarkup,                  // Ctrl+Z：撤销
    RedoMarkup,                  // Ctrl+Y / Ctrl+Shift+Z：重做
    ClearAllMarkup,              // Ctrl+Shift+Delete / Ctrl+Shift+Back：清空全部标注
    PinSelection,                // Ctrl+T：贴图置顶
    SaveSelectionAs,             // Ctrl+S：另存为图片文件
    ConfirmOrCommitUi,           // Enter：提交打开的下拉菜单 / 收起滑块，或确认完成并复制
    CopySelectionAndFinish,      // Ctrl+C：完成当前选区/拖拽/探测窗口截图并复制到剪贴板
    HierarchicalEscape,          // Esc：分级退出
};

/// 键盘路由输入快照（纯值结构，零 Win32 句柄耦合，支持 100% 离线确定性单元测试）
struct CaptureKeyContext {
    OverlayMode mode = OverlayMode::Screenshot;
    OverlayState overlayState = OverlayState::Selecting;
    bool dragging = false;
    bool isMarking = false;
    bool isManipulating = false;
    bool historyMode = false;
    bool editingText = false;
    bool hasDetectedHierarchy = false;
    bool isMarkupToolActive = false;
    MarkupTool currentTool = MarkupTool::Rectangle;
    bool hasActiveElement = false;
    MarkupTool activeElementTool = MarkupTool::Rectangle;
};

/// 键盘路由输出决策
struct CaptureShortcutDecision {
    CaptureShortcutAction action = CaptureShortcutAction::None;
    int dx = 0;
    int dy = 0;
    int intDelta = 0;
    HitArea resizeHandle = HitArea::RB;
    MarkupTool tool = MarkupTool::Rectangle;
    MarkupColor color = MarkupColor::Red();
    AspectRatioPreset aspectPreset = AspectRatioPreset::Free;
    bool selectAllOnTextEdit = false;
};

class CaptureShortcutPolicy {
public:
    static constexpr bool isShiftVirtualKey(WPARAM vk) noexcept {
        return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    }

    static constexpr bool isArrowKey(WPARAM vk) noexcept {
        return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
    }

    static constexpr bool isWasdKey(WPARAM vk) noexcept {
        return vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D';
    }

    static CaptureKeyContext snapshotFromState(const CaptureState& state) noexcept {
        CaptureKeyContext ctx;
        ctx.mode = state.mode;
        ctx.overlayState = state.state.load();
        ctx.dragging = state.dragging;
        ctx.isMarking = state.isMarking;
        ctx.isManipulating = state.isManipulating;
        ctx.historyMode = state.historyMode;
        ctx.editingText = (state.activeElement != nullptr &&
                           state.activeElement->tool == MarkupTool::Text &&
                           state.activeElement->isEditing);
        ctx.hasDetectedHierarchy = !state.detectedWindowHierarchy.empty();
        ctx.isMarkupToolActive = state.isMarkupToolActive;
        ctx.currentTool = state.currentTool;
        ctx.hasActiveElement = (state.activeElement != nullptr);
        if (state.activeElement != nullptr) {
            ctx.activeElementTool = state.activeElement->tool;
        }
        return ctx;
    }

    /// 更新 Shift 单按轻触候选状态（WM_KEYDOWN / WM_SYSKEYDOWN 时调用）
    /// - 首次纯按下 Shift：标记候选为 true；
    /// - 按住不放产生硬件连发 (isAutoRepeat) 或叠加任何其他键：立即熔断为 false，保障 Shift+组合键与长按约束零误触。
    static void updateShiftTapOnKeyDown(
        bool& shiftTapCandidate, WPARAM vk, bool isAutoRepeat, bool ctrl, bool alt) noexcept {
        if (isShiftVirtualKey(vk)) {
            if (ctrl || alt || isAutoRepeat) {
                shiftTapCandidate = false;
            } else {
                shiftTapCandidate = true;
            }
        } else {
            shiftTapCandidate = false;
        }
    }

    /// 鼠标点击、拖拽或滚轮活动时熔断 Shift 轻触候选（防止 Shift+拖拽正方形选区松开 Shift 时误切颜色格式）
    static void cancelShiftTapCandidate(bool& shiftTapCandidate) noexcept {
        shiftTapCandidate = false;
    }

    /// 解析 WM_KEYUP / WM_SYSKEYUP 事件（专用于纯单按释放 Shift 循环切换颜色格式）
    static CaptureShortcutDecision resolveKeyUp(
        bool& shiftTapCandidate,
        const CaptureKeyContext& ctx,
        WPARAM vk,
        bool ctrl,
        bool alt) noexcept {
        CaptureShortcutDecision out;
        if (!isShiftVirtualKey(vk)) {
            return out;
        }
        const bool wasCandidate = std::exchange(shiftTapCandidate, false);
        if (!wasCandidate || ctrl || alt) {
            return out;
        }
        if (ctx.overlayState == OverlayState::Idle || ctx.historyMode ||
            ctx.editingText || ctx.isMarking || ctx.isManipulating) {
            return out;
        }
        out.action = CaptureShortcutAction::CycleColorFormat;
        return out;
    }

    /// 获取指定颜色格式的人类可读短名称（用于复制提示与放大镜面板展示）
    static const wchar_t* colorFormatShortName(ColorFormatType fmt) noexcept {
        switch (fmt) {
            case ColorFormatType::HEX:    return L"HEX";
            case ColorFormatType::RGB:    return L"RGB";
            case ColorFormatType::RGBA:   return L"RGBA";
            case ColorFormatType::HEX_0x: return L"0xHEX";
            case ColorFormatType::HSL:    return L"HSL";
            case ColorFormatType::HSV:    return L"HSV";
            case ColorFormatType::CMYK:   return L"CMYK";
            case ColorFormatType::DEC:    return L"DEC";
            default:                      return L"HEX";
        }
    }

    /// 计算 Shift+C / Shift+K 快捷互补颜色格式（当前为 HEX 则取 RGB，其余格式一键取 HEX）
    static constexpr ColorFormatType alternateColorFormat(ColorFormatType current) noexcept {
        return (current == ColorFormatType::HEX) ? ColorFormatType::RGB : ColorFormatType::HEX;
    }

    /// 解析 WM_KEYDOWN / WM_SYSKEYDOWN 全阶段无冲突路由决策
    static CaptureShortcutDecision resolveKeyDown(
        const CaptureKeyContext& ctx,
        WPARAM vk,
        bool ctrl,
        bool shift,
        bool alt) noexcept {
        using namespace CaptureShortcutConstants;
        CaptureShortcutDecision out;

        if (ctx.overlayState == OverlayState::Idle) {
            return out;
        }

        const bool hasSelection = (ctx.overlayState == OverlayState::Selected ||
                                   ctx.overlayState == OverlayState::Marking);
        const bool isScreenshotMode = (ctx.mode == OverlayMode::Screenshot);

        // 1. 文本原地编辑态最高优先级隔离：仅 Esc 走分级退出，其余按键 100% 交由富文本编辑器处理
        if (ctx.editingText) {
            if (vk == VK_ESCAPE) {
                out.action = CaptureShortcutAction::HierarchicalEscape;
            } else {
                out.action = CaptureShortcutAction::DelegateToTextEditor;
            }
            return out;
        }

        // 2. 历史截图回放模式（'/' 切换，',' / '.' 翻页，Esc 退出）
        if (!alt && !ctrl && vk == VK_OEM_2) {
            out.action = CaptureShortcutAction::ToggleHistoryMode;
            return out;
        }
        if (!alt && !ctrl && (vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD)) {
            out.action = (vk == VK_OEM_COMMA)
                ? CaptureShortcutAction::StepHistoryPrev
                : CaptureShortcutAction::StepHistoryNext;
            return out;
        }
        if (ctx.historyMode) {
            out.action = (vk == VK_ESCAPE)
                ? CaptureShortcutAction::ExitHistoryMode
                : CaptureShortcutAction::ConsumeInHistoryMode;
            return out;
        }

        // 3. Alt 组合键专属路由（避免落入无修饰单键误触）
        if (alt) {
            if (!ctrl && hasSelection) {
                if (!ctx.hasActiveElement && isArrowKey(vk)) {
                    const int step = shift ? kPixelStepFast : kPixelStepFine;
                    fillDirectionDelta(vk, step, out.dx, out.dy);
                    out.action = CaptureShortcutAction::ResizeSelectionByKey;
                    out.resizeHandle = (vk == VK_LEFT || vk == VK_RIGHT) ? HitArea::L : HitArea::T;
                    return out;
                }
                if (isScreenshotMode) {
                    if (vk == 'B' && !shift) {
                        out.action = CaptureShortcutAction::CycleBeautyShellPadding;
                        return out;
                    }
                    if (vk == 'A' && !shift) {
                        out.action = CaptureShortcutAction::ToggleMarkupTool;
                        out.tool = MarkupTool::Arrow;
                        return out;
                    }
                }
            }
            return out;
        }

        // 4. Esc 分级退出（全阶段通用）
        if (vk == VK_ESCAPE) {
            out.action = CaptureShortcutAction::HierarchicalEscape;
            return out;
        }

        // 5. Ctrl 组合快捷键优先分发（严防与单键 WASD / C / T / S / Z 等发生遮蔽）
        if (ctrl) {
            if (vk == 'A' && !shift) {
                out.action = CaptureShortcutAction::SelectFullScreen;
                return out;
            }
            if (vk == 'C' && !shift) {
                out.action = CaptureShortcutAction::CopySelectionAndFinish;
                return out;
            }
            if (hasSelection && isScreenshotMode) {
                if (vk == 'T' && !shift) {
                    out.action = CaptureShortcutAction::PinSelection;
                    return out;
                }
                if (vk == 'S' && !shift) {
                    out.action = CaptureShortcutAction::SaveSelectionAs;
                    return out;
                }
                if (vk == 'Z') {
                    out.action = shift
                        ? CaptureShortcutAction::RedoMarkup
                        : CaptureShortcutAction::UndoMarkup;
                    return out;
                }
                if (vk == 'Y' && !shift) {
                    out.action = CaptureShortcutAction::RedoMarkup;
                    return out;
                }
                if (shift && (vk == VK_DELETE || vk == VK_BACK)) {
                    out.action = CaptureShortcutAction::ClearAllMarkup;
                    return out;
                }
            }

            // Ctrl + 方向键（以及无选区下 Ctrl + W/S/D）：10px 快速微调
            if (hasSelection && isArrowKey(vk)) {
                fillDirectionDelta(vk, kPixelStepFast, out.dx, out.dy);
                if (shift && !ctx.hasActiveElement) {
                    out.action = CaptureShortcutAction::ResizeSelectionByKey;
                    out.resizeHandle = (vk == VK_LEFT || vk == VK_RIGHT) ? HitArea::R : HitArea::B;
                } else {
                    out.action = CaptureShortcutAction::NudgeSelectionOrElement;
                }
                return out;
            }
            if (!hasSelection && (isArrowKey(vk) || vk == 'W' || vk == 'S' || vk == 'D')) {
                fillDirectionDelta(vk, kPixelStepFast, out.dx, out.dy);
                out.action = CaptureShortcutAction::MoveCursorPixels;
                return out;
            }
            return out;
        }

        // ── 以下均为 !ctrl && !alt 分支 ─────────────────────────────────────

        // 6. 无选区 / 拖拽框选阶段：
        //    - WASD 与 方向键 100% 绑定像素级光标移动（Shift 加速 10px），绝不触发窗口层级切换！
        //    - Tab / Shift+Tab / [ / ] / PgUp / PgDn 专司切换智能窗口子/父层级！
        if (!hasSelection) {
            if (isWasdKey(vk) || isArrowKey(vk)) {
                const int step = shift ? kPixelStepFast : kPixelStepFine;
                fillDirectionDelta(vk, step, out.dx, out.dy);
                out.action = CaptureShortcutAction::MoveCursorPixels;
                return out;
            }

            if (!ctx.dragging && ctx.hasDetectedHierarchy) {
                if (vk == VK_TAB) {
                    out.action = CaptureShortcutAction::StepWindowHierarchy;
                    out.intDelta = shift ? kHierarchyDeltaShrink : kHierarchyDeltaExpand;
                    return out;
                }
                if (!shift && (vk == VK_OEM_6 || vk == VK_PRIOR)) { // ']' 或 PageUp：向父窗口扩展
                    out.action = CaptureShortcutAction::StepWindowHierarchy;
                    out.intDelta = kHierarchyDeltaExpand;
                    return out;
                }
                if (!shift && (vk == VK_OEM_4 || vk == VK_NEXT)) {  // '[' 或 PageDown：向子控件收缩
                    out.action = CaptureShortcutAction::StepWindowHierarchy;
                    out.intDelta = kHierarchyDeltaShrink;
                    return out;
                }
            }

            // 像素取色：C / K / I 复制当前放大镜颜色格式，Shift+C / Shift+K / Shift+I 复制互补格式
            if (vk == 'C' || vk == 'K' || vk == 'I') {
                out.action = shift
                    ? CaptureShortcutAction::CopyAlternatePixelColor
                    : CaptureShortcutAction::CopyCurrentPixelColor;
                return out;
            }

            // Shift + 1..5 预设宽高比（拖拽前/拖拽中均可切换）
            if (shift && vk >= '1' && vk <= '5') {
                out.action = CaptureShortcutAction::LockAspectRatio;
                out.aspectPreset = aspectPresetFromDigit(vk);
                return out;
            }

            // Enter 确认智能探测窗口截图
            if (!shift && vk == VK_RETURN) {
                out.action = CaptureShortcutAction::ConfirmOrCommitUi;
                return out;
            }

            return out;
        }

        // ── 以下均为已锁定选区阶段 (hasSelection == true, !ctrl, !alt) ────────

        // 7. 已选区阶段 WASD 与 方向键：
        //    - 无 Shift：1px 微调移动选区（或当前选中图元）；
        //    - 按住 Shift：无选中图元时 1px 微调选区右下尺寸；有选中图元时 10px 快速移动图元。
        if (isWasdKey(vk) || isArrowKey(vk)) {
            if (ctx.hasActiveElement) {
                const int step = shift ? kPixelStepFast : kPixelStepFine;
                fillDirectionDelta(vk, step, out.dx, out.dy);
                out.action = CaptureShortcutAction::NudgeSelectionOrElement;
            } else if (shift) {
                fillDirectionDelta(vk, kPixelStepFine, out.dx, out.dy);
                out.action = CaptureShortcutAction::ResizeSelectionByKey;
                out.resizeHandle = (vk == VK_LEFT || vk == VK_RIGHT || vk == 'A' || vk == 'D')
                    ? HitArea::R
                    : HitArea::B;
            } else {
                fillDirectionDelta(vk, kPixelStepFine, out.dx, out.dy);
                out.action = CaptureShortcutAction::NudgeSelectionOrElement;
            }
            return out;
        }

        // 8. 聚光灯 / 几何图形快捷切换（Shift+0 进入/退出聚光灯；聚光灯激活时 Shift+1 切回几何图形）
        if (shift && isScreenshotMode && !ctx.isMarking) {
            if (vk == '0') {
                out.action = CaptureShortcutAction::ToggleMarkupTool;
                out.tool = MarkupTool::Spotlight;
                return out;
            }
            if (vk == '1' && ctx.isMarkupToolActive && ctx.currentTool == MarkupTool::Spotlight) {
                out.action = CaptureShortcutAction::ToggleMarkupTool;
                out.tool = MarkupTool::Rectangle;
                return out;
            }
        }

        // 8b. Shift + 1..5：锁定并约束当前选区宽高比（非绘制笔触中生效）
        if (shift && vk >= '1' && vk <= '5' && !ctx.isMarking) {
            out.action = CaptureShortcutAction::LockAspectRatio;
            out.aspectPreset = aspectPresetFromDigit(vk);
            return out;
        }

        // 9. Enter / F2：选中文本图元时优先重编辑，否则 Enter 执行菜单提交或完成确认
        if (vk == VK_RETURN || vk == VK_F2) {
            if (isScreenshotMode && ctx.hasActiveElement && ctx.activeElementTool == MarkupTool::Text) {
                out.action = CaptureShortcutAction::ReEditSelectedText;
                out.selectAllOnTextEdit = (vk == VK_F2);
                return out;
            }
            if (vk == VK_RETURN) {
                out.action = CaptureShortcutAction::ConfirmOrCommitUi;
                return out;
            }
            return out;
        }

        // 10. 区域录屏框选模式隔离守卫：屏蔽后续所有截图专属标注工具、画笔色与圆角快捷键
        if (!isScreenshotMode) {
            return out;
        }

        // 11. 删除当前选中图元
        if ((vk == VK_DELETE || vk == VK_BACK) && ctx.hasActiveElement) {
            out.action = CaptureShortcutAction::DeleteSelectedElement;
            return out;
        }

        // 12. 像素取色（已选区态仍支持按 C / K 取色并同步设为当前画笔颜色）
        if (vk == 'C' || vk == 'K') {
            out.action = shift
                ? CaptureShortcutAction::CopyAlternatePixelColor
                : CaptureShortcutAction::CopyCurrentPixelColor;
            return out;
        }

        // 13. 美化外壳导出：B 开关，Shift+B 循环切换 5 种质感主题
        if (vk == 'B') {
            out.action = shift
                ? CaptureShortcutAction::CycleBeautyShellTheme
                : CaptureShortcutAction::ToggleBeautyShell;
            return out;
        }

        // 14. '[' / ']' 上下文智能分发：
        //     - 序号标注激活或选中序号徽章时：递减/递增序号编号（与 HUD 提示 100% 一致）；
        //     - 其余状态：减小/增大选区或美化外壳圆角半径。
        if (!shift && (vk == VK_OEM_4 || vk == VK_OEM_6)) {
            const int delta = (vk == VK_OEM_4) ? -1 : 1;
            const bool inNumberContext =
                (ctx.isMarkupToolActive && ctx.currentTool == MarkupTool::Number) ||
                (ctx.hasActiveElement && ctx.activeElementTool == MarkupTool::Number);
            out.action = inNumberContext
                ? CaptureShortcutAction::AdjustNumberCounter
                : CaptureShortcutAction::AdjustCornerRadius;
            out.intDelta = delta;
            return out;
        }

        // 15. '+' / '-' 无级调节笔触粗细 / 字号 / 马赛克粒度
        if (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT) {
            out.action = CaptureShortcutAction::AdjustStrokeWidth;
            out.intDelta = (vk == VK_OEM_PLUS || vk == VK_ADD) ? kStrokeDeltaUp : kStrokeDeltaDown;
            return out;
        }

        // 16. 单键数字 1..7（无 Shift）：切换二级属性栏 7 种标准预设画笔色
        if (!shift && vk >= '1' && vk <= '7') {
            out.action = CaptureShortcutAction::SelectPresetColor;
            out.color = presetColorFromDigit(vk);
            return out;
        }

        // 17. 表驱动标注工具单键映射（彻底消除 W/A/S/D 冲突与 E 键橡皮擦/椭圆错位）
        if (!shift) {
            struct ToolKeyBinding {
                WPARAM key;
                MarkupTool tool;
            };
            static constexpr ToolKeyBinding kToolBindings[] = {
                {'R', MarkupTool::Rectangle}, // 矩形 (R)
                {'O', MarkupTool::Ellipse},   // 椭圆 (O)
                {'L', MarkupTool::Line},      // 直线 (L)
                {'Q', MarkupTool::Arrow},     // 箭头 (Q / Alt+A，释放 A 给 WASD 左移微调)
                {'P', MarkupTool::Pen},       // 画笔 (P)
                {'H', MarkupTool::Highlight}, // 荧光笔 (H)
                {'T', MarkupTool::Text},      // 文本 (T)
                {'N', MarkupTool::Number},    // 序号 (N)
                {'Z', MarkupTool::Magnifier}, // 局部放大镜 (Z)
                {'G', MarkupTool::Magnifier}, // 局部放大镜兼容键 (G)
                {'M', MarkupTool::Mosaic},    // 马赛克 (M)
                {'E', MarkupTool::Inpaint},   // 智能消除 / 橡皮擦 (E，与工具栏 Tooltip 对齐)
                {'I', MarkupTool::Inpaint},   // 智能消除兼容键 (I)
            };
            for (const auto& binding : kToolBindings) {
                if (binding.key == vk) {
                    out.action = CaptureShortcutAction::ToggleMarkupTool;
                    out.tool = binding.tool;
                    return out;
                }
            }
        }

        return out;
    }

private:
    static constexpr void fillDirectionDelta(WPARAM vk, int step, int& dx, int& dy) noexcept {
        dx = 0;
        dy = 0;
        if (vk == VK_LEFT || vk == 'A')       dx = -step;
        else if (vk == VK_RIGHT || vk == 'D') dx = step;
        else if (vk == VK_UP || vk == 'W')    dy = -step;
        else if (vk == VK_DOWN || vk == 'S')  dy = step;
    }

    static constexpr AspectRatioPreset aspectPresetFromDigit(WPARAM vk) noexcept {
        switch (vk) {
            case '1': return AspectRatioPreset::Free;
            case '2': return AspectRatioPreset::Ratio_16_9;
            case '3': return AspectRatioPreset::Ratio_4_3;
            case '4': return AspectRatioPreset::Ratio_1_1;
            case '5': return AspectRatioPreset::Ratio_Golden;
            default:  return AspectRatioPreset::Free;
        }
    }

    static MarkupColor presetColorFromDigit(WPARAM vk) noexcept {
        switch (vk) {
            case '1': return MarkupColor::Red();
            case '2': return MarkupColor::Orange();
            case '3': return MarkupColor::Yellow();
            case '4': return MarkupColor::Green();
            case '5': return MarkupColor::Blue();
            case '6': return MarkupColor::Black();
            case '7': return MarkupColor::White();
            default:  return MarkupColor::Red();
        }
    }
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTURESHORTCUTPOLICY_H
