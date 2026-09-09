#pragma once

#include "capture/CaptureState.h"

namespace tools3000::capture {

// The painted toolbar uses compact glyphs. Keep the spoken/UIA contract in a
// small header-only mapping so it cannot drift from the command dispatcher or
// require UI/window construction in unit tests.
inline std::wstring toolbarButtonAccessibleName(const ToolbarButton& button) {
    switch (button.command) {
        case ToolbarCommand::SelectTool:
            switch (button.tool) {
                case MarkupTool::Rectangle: return L"矩形标注";
                case MarkupTool::Line: return L"直线标注";
                case MarkupTool::Arrow: return L"箭头标注";
                case MarkupTool::Ellipse: return L"椭圆标注";
                case MarkupTool::Pen: return L"画笔标注";
                case MarkupTool::Highlight: return L"高亮标注";
                case MarkupTool::Mosaic: return L"马赛克标注";
                case MarkupTool::Text: return L"文字标注";
                case MarkupTool::Number: return L"编号标注";
                case MarkupTool::Magnifier: return L"放大镜标注";
                case MarkupTool::Spotlight: return L"聚光灯标注";
                case MarkupTool::Watermark: return L"水印标注";
                case MarkupTool::Inpaint: return L"修复标注";
                case MarkupTool::Blur: return L"模糊标注";
            }
            return L"标注工具";
        case ToolbarCommand::SelectColor: return L"标注颜色";
        case ToolbarCommand::ChooseCustomColor: return L"选择自定义颜色";
        case ToolbarCommand::Undo: return L"撤销";
        case ToolbarCommand::Redo: return L"重做";
        case ToolbarCommand::Clear: return L"清空标注";
        case ToolbarCommand::ToggleCornerRadius: return L"调节选区圆角";
        case ToolbarCommand::SideToggleCornerRadius: return L"调节选区圆角半径";
        case ToolbarCommand::SideCycleAspectRatio: return L"选区比例锁定";
        case ToolbarCommand::SideInvertSelection: return L"反向选择";
        case ToolbarCommand::SideResetSelection: return L"重置直角";
        case ToolbarCommand::ExtractText: return L"提取文本";
        case ToolbarCommand::PinWindow: return L"置顶到屏幕";
        case ToolbarCommand::ScrollCapture: return L"开始长截图";
        case ToolbarCommand::StartRecord: return L"区域录屏";
        case ToolbarCommand::ToggleBeautyShell: return L"美化外壳分享模式";
        case ToolbarCommand::CycleBeautyBg: return L"切换美化外壳背景";
        case ToolbarCommand::CycleBeautyPadding: return L"调节美化外壳边距";
        case ToolbarCommand::CycleBeautyRadius: return L"调节美化外壳圆角";
        case ToolbarCommand::ToggleNumberShape:
            return button.tool == MarkupTool::Magnifier ? L"切换放大透镜形状" : L"切换序号徽章形状";
        case ToolbarCommand::ToggleNumberFormatDropdown: return L"选择序号编号制式";
        case ToolbarCommand::ToggleSpotlightShape: return L"切换聚光灯形状";
        case ToolbarCommand::CycleSpotlightDim: return L"调节聚光灯背景弱化度";
        case ToolbarCommand::CycleMagnifierScale: return L"调节局部放大倍率";
        case ToolbarCommand::ToggleMagnifierCallout: return L"切换引出式详图放大模式";
        case ToolbarCommand::ResetNumberCounter: return L"重置序号计数器";
        case ToolbarCommand::ToggleTextOutline: return L"文字高反差描边";
        case ToolbarCommand::SelectTextOutlineColor: return L"文字描边颜色";
        case ToolbarCommand::RecordToggleFormat: return L"录屏格式设置";
        case ToolbarCommand::RecordCycleFps: return L"录屏帧率设置";
        case ToolbarCommand::RecordCycleQuality: return L"录屏画质设置";
        case ToolbarCommand::RecordCycleClickEffect: return L"录屏鼠标点击特效设置";
        case ToolbarCommand::RecordToggleKeycast: return L"录屏按键回显开关";
        case ToolbarCommand::RecordToggleSystemAudio: return L"录屏系统声音开关";
        case ToolbarCommand::RecordToggleMicrophone: return L"录屏麦克风开关";
        case ToolbarCommand::RecordStartConfirm: return L"正式开始录屏";
        case ToolbarCommand::Copy: return L"复制到剪贴板";
        case ToolbarCommand::Confirm: return L"确认截图";
        case ToolbarCommand::Cancel: return L"取消截图";
        default: break;
    }
    return L"截图工具栏操作";
}

inline std::wstring toolbarButtonKeyboardShortcut(const ToolbarButton& button) {
    switch (button.command) {
        case ToolbarCommand::ToggleCornerRadius: return L"[ / ]";
        case ToolbarCommand::SideCycleAspectRatio: return L"Shift+1..5";
        case ToolbarCommand::ToggleBeautyShell: return L"B";
        case ToolbarCommand::Undo: return L"Ctrl+Z";
        case ToolbarCommand::Redo: return L"Ctrl+Y";
        case ToolbarCommand::Copy: return L"Ctrl+C";
        case ToolbarCommand::Confirm: return L"Enter";
        case ToolbarCommand::Cancel: return L"Escape";
        default: return L"";
    }
}

inline bool isToolbarButtonSelected(const ToolbarButton& button, const CaptureState& state) noexcept {
    if (button.command == ToolbarCommand::SelectTool) return button.tool == state.currentTool;
    if (button.command == ToolbarCommand::ToggleBeautyShell) return state.beautyShell.enabled;
    if (button.command == ToolbarCommand::SideCycleAspectRatio) return state.aspectRatio != AspectRatioPreset::Free;
    if (button.command == ToolbarCommand::ToggleFill) {
        if (button.tool == MarkupTool::Magnifier) return state.magnifierShowBadge;
        if (button.tool == MarkupTool::Spotlight) return state.currentSpotlightBorder;
        return state.currentFillMode;
    }
    if (button.command == ToolbarCommand::ToggleSpotlightShape) {
        return state.currentSpotlightEllipse;
    }
    if (button.command == ToolbarCommand::ToggleMagnifierCallout) {
        if (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier) {
            const bool unsetEnd = !state.activeElement->hasLeaderArrow &&
                                  state.activeElement->endPt.x == 0 && state.activeElement->endPt.y == 0 &&
                                  (state.activeElement->startPt.x != 0 || state.activeElement->startPt.y != 0);
            const cv::Point lc = unsetEnd ? state.activeElement->startPt : state.activeElement->endPt;
            const double dx = static_cast<double>(lc.x - state.activeElement->startPt.x);
            const double dy = static_cast<double>(lc.y - state.activeElement->startPt.y);
            return (dx * dx + dy * dy) > 64.0;
        }
        return state.magnifierCalloutMode;
    }
    if (button.command == ToolbarCommand::ToggleTextOutline) return state.currentTextOutline;
    if (button.command == ToolbarCommand::SelectTextOutlineColor) {
        return button.color.isAuto()
            ? state.currentTextOutlineColor.isAuto()
            : (!state.currentTextOutlineColor.isAuto() && button.color == state.currentTextOutlineColor);
    }
    if (button.command == ToolbarCommand::RecordToggleSystemAudio) return state.recordSetup.captureSystemAudio && state.recordSetup.format != RecordFormat::GIF;
    if (button.command == ToolbarCommand::RecordToggleMicrophone) return state.recordSetup.captureMicrophone && state.recordSetup.format != RecordFormat::GIF;
    if (button.command == ToolbarCommand::RecordToggleKeycast) return state.recordSetup.includeKeycast;
    if (button.command == ToolbarCommand::RecordCycleClickEffect) return state.recordSetup.showClickEffects || state.recordSetup.showClickZoom;
    if (button.command == ToolbarCommand::SelectColor) {
        const MarkupColor& activeColor = (state.currentTool == MarkupTool::Magnifier)
            ? state.currentMagnifierColor
            : state.currentColor;
        return button.color.r == activeColor.r && button.color.g == activeColor.g &&
               button.color.b == activeColor.b && button.color.a == activeColor.a;
    }
    if (button.command == ToolbarCommand::ChooseCustomColor) {
        const MarkupColor& activeColor = (state.currentTool == MarkupTool::Magnifier)
            ? state.currentMagnifierColor
            : state.currentColor;
        return state.hasCustomColor && button.color.r == activeColor.r &&
               button.color.g == activeColor.g && button.color.b == activeColor.b;
    }
    return false;
}

inline bool isToolbarButtonEnabled(const ToolbarButton& button, const CaptureState& state) noexcept {
    const auto overlayState = state.state.load(std::memory_order_acquire);
    if (overlayState != OverlayState::Selected && overlayState != OverlayState::Marking) return false;
    switch (button.command) {
        case ToolbarCommand::Undo: return state.markup.canUndo();
        case ToolbarCommand::Redo: return state.markup.canRedo();
        case ToolbarCommand::Clear: return state.markup.elementCount() > 0;
        case ToolbarCommand::ExtractText: return static_cast<bool>(state.ocrCallback);
        default: return true;
    }
}

}  // namespace tools3000::capture
