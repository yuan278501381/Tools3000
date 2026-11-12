#pragma once

#include "capture/CaptureState.h"
#include "core/config/ConfigManager.h"

namespace tools3000::capture {

// The painted toolbar uses compact glyphs. Keep the spoken/UIA contract in a
// small header-only mapping so it cannot drift from the command dispatcher or
// require UI/window construction in unit tests.
inline std::wstring toolbarButtonAccessibleName(const ToolbarButton& button, bool zh) {
    switch (button.command) {
        case ToolbarCommand::SelectTool:
            switch (button.tool) {
                case MarkupTool::Rectangle: return zh ? L"矩形标注" : L"Rectangle Annotation";
                case MarkupTool::Line: return zh ? L"直线标注" : L"Line Annotation";
                case MarkupTool::Arrow: return zh ? L"箭头标注" : L"Arrow Annotation";
                case MarkupTool::Ellipse: return zh ? L"椭圆标注" : L"Ellipse Annotation";
                case MarkupTool::Pen: return zh ? L"画笔标注" : L"Pen Annotation";
                case MarkupTool::Highlight: return zh ? L"高亮标注" : L"Highlighter Annotation";
                case MarkupTool::Mosaic: return zh ? L"马赛克标注" : L"Mosaic Annotation";
                case MarkupTool::Text: return zh ? L"文字标注" : L"Text Annotation";
                case MarkupTool::Number: return zh ? L"编号标注" : L"Step Badge Annotation";
                case MarkupTool::Magnifier: return zh ? L"放大镜标注" : L"Magnifier Annotation";
                case MarkupTool::Spotlight: return zh ? L"聚光灯标注" : L"Spotlight Annotation";
                case MarkupTool::Watermark: return zh ? L"水印标注" : L"Watermark Annotation";
                case MarkupTool::Inpaint: return zh ? L"修复标注" : L"Smart Eraser Annotation";
                case MarkupTool::Blur: return zh ? L"模糊标注" : L"Blur Annotation";
            }
            return zh ? L"标注工具" : L"Annotation Tool";
        case ToolbarCommand::SelectColor: return zh ? L"标注颜色" : L"Annotation Color";
        case ToolbarCommand::ChooseCustomColor: return zh ? L"选择自定义颜色" : L"Choose Custom Color";
        case ToolbarCommand::Undo: return zh ? L"撤销" : L"Undo";
        case ToolbarCommand::Redo: return zh ? L"重做" : L"Redo";
        case ToolbarCommand::Clear: return zh ? L"清空标注" : L"Clear Annotations";
        case ToolbarCommand::ToggleCornerRadius: return zh ? L"调节选区圆角" : L"Adjust Selection Corner Radius";
        case ToolbarCommand::SideToggleCornerRadius: return zh ? L"调节选区圆角半径" : L"Adjust Selection Corner Radius";
        case ToolbarCommand::SideCycleAspectRatio: return zh ? L"选区比例锁定" : L"Lock Aspect Ratio";
        case ToolbarCommand::SideInvertSelection: return zh ? L"扩展至全屏或还原选区" : L"Expand to Full Screen or Restore";
        case ToolbarCommand::SideResetSelection: return zh ? L"重置直角与自由比例" : L"Reset Corner Radius and Aspect Ratio";
        case ToolbarCommand::ExtractText: return zh ? L"提取文本" : L"Extract Text (OCR)";
        case ToolbarCommand::PinWindow: return zh ? L"置顶到屏幕" : L"Pin to Screen";
        case ToolbarCommand::ScrollCapture: return zh ? L"开始长截图" : L"Scrolling Capture";
        case ToolbarCommand::StartRecord: return zh ? L"区域录屏" : L"Record Region";
        case ToolbarCommand::ToggleBeautyShell: return zh ? L"美化外壳分享模式" : L"Beauty Shell Mode";
        case ToolbarCommand::CycleBeautyBg: return zh ? L"切换美化外壳背景" : L"Switch Beauty Shell Background";
        case ToolbarCommand::CycleBeautyPadding: return zh ? L"调节美化外壳边距" : L"Adjust Beauty Shell Padding";
        case ToolbarCommand::CycleBeautyRadius: return zh ? L"调节美化外壳圆角" : L"Adjust Beauty Shell Radius";
        case ToolbarCommand::CycleStrokeWidth:
            if (button.tool == MarkupTool::Text) return zh ? L"调节文本字号" : L"Adjust Font Size";
            if (button.tool == MarkupTool::Mosaic) return zh ? L"调节马赛克粒度" : L"Adjust Mosaic Size";
            if (button.tool == MarkupTool::Blur) return zh ? L"调节模糊强度" : L"Adjust Blur Strength";
            if (button.tool == MarkupTool::Inpaint) return zh ? L"调节涂抹笔刷大小" : L"Adjust Eraser Brush Size";
            if (button.tool == MarkupTool::Number) return zh ? L"调节序号尺寸" : L"Adjust Badge Size";
            if (button.tool == MarkupTool::Magnifier) return zh ? L"调节镜框粗细" : L"Adjust Lens Border Width";
            return zh ? L"调节线条粗细" : L"Adjust Stroke Width";
        case ToolbarCommand::ToggleFill:
            if (button.tool == MarkupTool::Text) return zh ? L"文字背景底衬开关" : L"Toggle Text Background";
            if (button.tool == MarkupTool::Magnifier) return zh ? L"倍率角标显隐开关" : L"Toggle Zoom Badge";
            if (button.tool == MarkupTool::Spotlight) return zh ? L"聚光区域描边开关" : L"Toggle Spotlight Border";
            return zh ? L"切换填充模式" : L"Toggle Fill Mode";
        case ToolbarCommand::CycleElementCornerRadius: return zh ? L"调节标注圆角" : L"Adjust Element Corner Radius";
        case ToolbarCommand::ToggleLineStyleDropdown: return zh ? L"选择线条样式" : L"Select Line Style";
        case ToolbarCommand::SelectLineStyle: return zh ? L"切换线条样式" : L"Switch Line Style";
        case ToolbarCommand::ToggleArrowStyleDropdown: return zh ? L"选择箭头样式" : L"Select Arrow Style";
        case ToolbarCommand::SelectArrowStyle: return zh ? L"切换箭头样式" : L"Switch Arrow Style";
        case ToolbarCommand::SelectMosaicType:
            return button.intParam == 0
                ? (zh ? L"像素马赛克模式" : L"Pixel Mosaic Mode")
                : (zh ? L"高斯模糊模式" : L"Gaussian Blur Mode");
        case ToolbarCommand::ToggleNumberShape:
            return button.tool == MarkupTool::Magnifier
                ? (zh ? L"切换放大透镜形状" : L"Switch Lens Shape")
                : (zh ? L"切换序号徽章形状" : L"Switch Badge Shape");
        case ToolbarCommand::ToggleNumberFormatDropdown: return zh ? L"选择序号编号制式" : L"Select Numbering Format";
        case ToolbarCommand::ToggleSpotlightShape: return zh ? L"切换聚光灯形状" : L"Switch Spotlight Shape";
        case ToolbarCommand::CycleSpotlightDim: return zh ? L"调节聚光灯背景弱化度" : L"Adjust Spotlight Dimming";
        case ToolbarCommand::CycleMagnifierScale: return zh ? L"调节局部放大倍率" : L"Adjust Magnifier Zoom";
        case ToolbarCommand::ToggleMagnifierCallout: return zh ? L"切换引出式详图放大模式" : L"Toggle Callout Magnifier Mode";
        case ToolbarCommand::ToggleMagnifierCalloutStyleDropdown: return zh ? L"选择放大镜引出线样式" : L"Select Magnifier Leader Style";
        case ToolbarCommand::ResetNumberCounter: return zh ? L"重置序号计数器" : L"Reset Step Counter";
        case ToolbarCommand::ToggleTextOutline:
            if (button.tool != MarkupTool::Text) return zh ? L"调节边框粗细与边框颜色" : L"Adjust Border Width and Color";
            return zh ? L"文字高反差描边" : L"Toggle Text Outline";
        case ToolbarCommand::SelectTextOutlineColor: return zh ? L"文字描边颜色" : L"Select Text Outline Color";
        case ToolbarCommand::ToggleCurveMode: return zh ? L"切换平滑曲线" : L"Toggle Smooth Curve";
        case ToolbarCommand::ToggleLineArrow: return zh ? L"折线端点箭头开关" : L"Toggle Line Endpoint Arrow";
        case ToolbarCommand::ToggleStartCapDropdown: return zh ? L"选择起点端点样式" : L"Select Start Cap Style";
        case ToolbarCommand::SelectStartCap: return zh ? L"切换起点端点样式" : L"Switch Start Cap Style";
        case ToolbarCommand::ToggleEndCapDropdown: return zh ? L"选择终点端点样式" : L"Select End Cap Style";
        case ToolbarCommand::SelectEndCap: return zh ? L"切换终点端点样式" : L"Switch End Cap Style";
        case ToolbarCommand::RecordToggleFormat: return zh ? L"录屏格式设置" : L"Recording Format";
        case ToolbarCommand::RecordCycleFps: return zh ? L"录屏帧率设置" : L"Recording Frame Rate";
        case ToolbarCommand::RecordCycleQuality: return zh ? L"录屏画质设置" : L"Recording Quality";
        case ToolbarCommand::RecordCycleClickEffect: return zh ? L"录屏鼠标点击特效设置" : L"Mouse Click Effects";
        case ToolbarCommand::RecordToggleKeycast: return zh ? L"录屏按键回显开关" : L"Toggle Keycast";
        case ToolbarCommand::RecordToggleSystemAudio: return zh ? L"录屏系统声音开关" : L"Toggle System Audio";
        case ToolbarCommand::RecordToggleMicrophone: return zh ? L"录屏麦克风开关" : L"Toggle Microphone";
        case ToolbarCommand::RecordStartConfirm: return zh ? L"正式开始录屏" : L"Start Recording";
        case ToolbarCommand::Copy: return zh ? L"复制到剪贴板" : L"Copy to Clipboard";
        case ToolbarCommand::Confirm: return zh ? L"确认截图" : L"Confirm Capture";
        case ToolbarCommand::Cancel: return zh ? L"取消截图" : L"Cancel Capture";
        default: break;
    }
    return zh ? L"截图工具栏操作" : L"Capture Toolbar Action";
}

inline std::wstring toolbarButtonAccessibleName(const ToolbarButton& button) {
    const std::string lang = tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    const bool zh = !(lang.rfind("en", 0) == 0);
    return toolbarButtonAccessibleName(button, zh);
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
    if (button.command == ToolbarCommand::SelectTool) {
        if (button.isSecondary) {
            return button.tool == state.currentTool;
        }
        if (button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse ||
            button.tool == MarkupTool::Line || button.tool == MarkupTool::Spotlight) {
            return state.currentTool == MarkupTool::Rectangle || state.currentTool == MarkupTool::Ellipse ||
                   state.currentTool == MarkupTool::Line || state.currentTool == MarkupTool::Spotlight;
        }
        if (button.tool == MarkupTool::Pen || button.tool == MarkupTool::Highlight) {
            return state.currentTool == MarkupTool::Pen || state.currentTool == MarkupTool::Highlight;
        }
        if (button.tool == MarkupTool::Mosaic || button.tool == MarkupTool::Blur) {
            return state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Blur;
        }
        return button.tool == state.currentTool;
    }
    if (button.command == ToolbarCommand::SelectMosaicType) {
        const int effectiveMosaicType = (state.currentTool == MarkupTool::Blur) ? 1 : state.currentMosaicType;
        return button.intParam == effectiveMosaicType;
    }
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
    if (button.command == ToolbarCommand::ToggleTextOutline) {
        if (button.tool != MarkupTool::Text) {
            return state.isEditingOutlineColor || state.sliderPopup.type == SliderPopupType::OutlineWidth;
        }
        return state.currentTextOutline || state.isEditingOutlineColor;
    }
    if (button.command == ToolbarCommand::ToggleCurveMode) return state.currentSmoothCurve;
    if (button.command == ToolbarCommand::ToggleLineArrow) return state.currentLineArrowEnabled;
    if (button.command == ToolbarCommand::SelectStartCap) {
        return button.intParam == static_cast<int>(state.currentStartCap);
    }
    if (button.command == ToolbarCommand::SelectEndCap) {
        return button.intParam == static_cast<int>(state.currentEndCap);
    }
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
            : (state.isEditingOutlineColor ? state.currentTextOutlineColor : state.currentColor);
        return !activeColor.isAuto() &&
               button.color.r == activeColor.r && button.color.g == activeColor.g &&
               button.color.b == activeColor.b && button.color.a == activeColor.a;
    }
    if (button.command == ToolbarCommand::ChooseCustomColor) {
        const MarkupColor& activeColor = (state.currentTool == MarkupTool::Magnifier)
            ? state.currentMagnifierColor
            : (state.isEditingOutlineColor ? state.currentTextOutlineColor : state.currentColor);
        return state.hasCustomColor && !activeColor.isAuto() && button.color.r == activeColor.r &&
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
