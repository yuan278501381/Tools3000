#include "capture/CaptureToolbarLayout.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/ShortcutHintOverlay.h"

#include "core/config/ConfigManager.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>
#include <format>

namespace tools3000::capture {
namespace {

struct ButtonSpec {
    ToolbarCommand command = ToolbarCommand::SelectTool;
    MarkupTool tool = MarkupTool::Rectangle;
    MarkupColor color = MarkupColor::Red();
    std::wstring label;
    float baseWidth = 30.0f;
    int intParam = 0;
    LineStyle lineStyleParam = LineStyle::Solid;
    ArrowStyle arrowStyleParam = ArrowStyle::Standard;
    bool boolParam = false;
    bool hasDropdown = false;
    bool isSecondary = false;
    bool isSeparatorBefore = false;
};

bool useChineseLabels() {
    return tools3000::core::isUiLanguageChinese();
}

// 1. 主工具栏规格
std::vector<ButtonSpec> primaryButtonSpecs(const CaptureState& state, bool zh) {
    if (state.mode == OverlayMode::RecordRegion) {
        std::vector<ButtonSpec> specs;
        // 1. 录制格式 (MP4 / HEVC / WebM / GIF) — 留足充裕水平呼吸内边距
        const std::wstring fmtLabel = (state.recordSetup.format == RecordFormat::GIF) ? L"GIF"
            : (state.recordSetup.format == RecordFormat::MP4_H265 ? L"HEVC"
            : (state.recordSetup.format == RecordFormat::WebM_VP9 ? L"WebM" : L"MP4"));
        specs.push_back({
            ToolbarCommand::RecordToggleFormat, MarkupTool::Rectangle, {},
            fmtLabel,
            54.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
        });

        // 2. 录制帧率 (15 / 30 / 60 fps)
        specs.push_back({
            ToolbarCommand::RecordCycleFps, MarkupTool::Rectangle, {},
            std::format(L"{} fps", state.recordSetup.fps),
            62.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
        });

        // 3. 码率/画质选择 (4 / 8 / 16 / 32 Mbps)
        const int dispBitrate = state.recordSetup.bitrateMbps > 0
            ? state.recordSetup.bitrateMbps
            : recordQualityLevelToBitrateMbps(state.recordSetup.qualityLevel);
        std::wstring qText = std::format(L"{}M", dispBitrate);
        specs.push_back({
            ToolbarCommand::RecordCycleQuality, MarkupTool::Rectangle, {},
            qText,
            50.0f, state.recordSetup.qualityLevel, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
        });

        // 4. 鼠标点击特效切换 (无 / 柔光水波 / macOS缩放 / 水波+缩放)
        std::wstring clickText;
        if (zh) {
            clickText = (state.recordSetup.showClickZoom && state.recordSetup.showClickEffects) ? L"水波+缩放"
                      : state.recordSetup.showClickZoom ? L"macOS缩放"
                      : state.recordSetup.showClickEffects ? L"柔光水波"
                      : L"无点击特效";
        } else {
            clickText = (state.recordSetup.showClickZoom && state.recordSetup.showClickEffects) ? L"Ripple+Zoom"
                      : state.recordSetup.showClickZoom ? L"Mac Zoom"
                      : state.recordSetup.showClickEffects ? L"Ripple"
                      : L"No FX";
        }
        specs.push_back({
            ToolbarCommand::RecordCycleClickEffect, MarkupTool::Rectangle, {},
            clickText,
            zh ? 94.0f : 100.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.showClickEffects || state.recordSetup.showClickZoom,
            false, false, true // separator
        });

        // 5. 按键回显 (Keycast) — 标准 32x32 正圆角方形热区
        specs.push_back({
            ToolbarCommand::RecordToggleKeycast, MarkupTool::Rectangle, {},
            L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.includeKeycast, false, false, false
        });

        // 6. 系统声音 (GIF 格式下置灰/不可选)
        specs.push_back({
            ToolbarCommand::RecordToggleSystemAudio, MarkupTool::Rectangle, {},
            L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.captureSystemAudio && state.recordSetup.format != RecordFormat::GIF,
            false, false, true // separator
        });

        // 7. 麦克风 (GIF 格式下置灰/不可选)
        specs.push_back({
            ToolbarCommand::RecordToggleMicrophone, MarkupTool::Rectangle, {},
            L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.captureMicrophone && state.recordSetup.format != RecordFormat::GIF,
            false, false, false
        });

        // 8. 取消按钮
        specs.push_back({
            ToolbarCommand::Cancel, MarkupTool::Rectangle, {},
            L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            false, false, false, true // separator
        });

        // 9. 正式开始录制 (纯矢量双层呼吸圆点 + 居中标签，摒弃高低错位的字体符号 ●)
        specs.push_back({
            ToolbarCommand::RecordStartConfirm, MarkupTool::Rectangle, {},
            zh ? L"开始录制" : L"Start",
            zh ? 98.0f : 84.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            true, false, false, false
        });

        return specs;
    }

    std::vector<ButtonSpec> specs;
    // 形状工具组（几何图形：矩形 / 椭圆 / 直线；或聚光灯模式）
    MarkupTool shapeTool = MarkupTool::Rectangle;
    if (state.currentTool == MarkupTool::Ellipse) shapeTool = MarkupTool::Ellipse;
    else if (state.currentTool == MarkupTool::Line) shapeTool = MarkupTool::Line;
    else if (state.currentTool == MarkupTool::Spotlight) shapeTool = MarkupTool::Spotlight;
    specs.push_back({ToolbarCommand::SelectTool, 
                     shapeTool,
                     {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, true, false, false});

    // 画笔工具组（铅笔 / 荧光笔）
    specs.push_back({ToolbarCommand::SelectTool,
                     (state.currentTool == MarkupTool::Highlight ? MarkupTool::Highlight : MarkupTool::Pen),
                     {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 箭头工具组
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Arrow, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 文本工具
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Text, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 序号工具
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Number, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 局部放大工具（一等公民：同心光学透镜 + Alt 引出式旁路放大）
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Magnifier, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 马赛克 / 高斯模糊工具组
    const bool blurSelected = (state.currentTool == MarkupTool::Blur) ||
                              (state.currentTool == MarkupTool::Mosaic && state.currentMosaicType == 1);
    specs.push_back({ToolbarCommand::SelectTool,
                     blurSelected ? MarkupTool::Blur : MarkupTool::Mosaic,
                     {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 智能消除 / 橡皮擦
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Inpaint, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 操作组（带前置分隔线）
    specs.push_back({ToolbarCommand::Undo, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::Redo, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 系统能力（带前置分隔线）
    specs.push_back({ToolbarCommand::ExtractText, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::PinWindow, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    if (!state.isLongImageMode()) {
        specs.push_back({ToolbarCommand::ScrollCapture, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
        specs.push_back({ToolbarCommand::StartRecord, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    }
    specs.push_back({ToolbarCommand::ToggleBeautyShell, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.beautyShell.enabled, false, false, false});

    // 动作组（带前置分隔线）
    specs.push_back({ToolbarCommand::Copy, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::Cancel, MarkupTool::Rectangle, {}, L"", 32.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    specs.push_back({ToolbarCommand::Confirm, MarkupTool::Rectangle, {}, L"", 42.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    return specs;
}

// 2. 二级属性栏规格（根据 currentTool 动态变幻，或单独展示美化外壳导出菜单栏）
std::vector<ButtonSpec> secondaryButtonSpecs(const CaptureState& state, bool zh) {
    if (state.mode == OverlayMode::RecordRegion ||
        (!state.isMarkupToolActive && !state.beautyShell.enabled)) {
        return {};
    }

    static const std::array colors{
        MarkupColor::Red(), MarkupColor::Orange(), MarkupColor::Yellow(),
        MarkupColor::Green(), MarkupColor::Blue(), MarkupColor::Black(), MarkupColor::White(),
    };

    std::vector<ButtonSpec> specs;

    auto appendColorPalette = [&](MarkupTool tool) {
        MarkupColor refColor = state.currentColor;
        if (tool == MarkupTool::Magnifier) {
            refColor = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                           ? state.activeElement->color
                           : state.currentMagnifierColor;
        } else if (state.isEditingOutlineColor) {
            if (state.activeElement && state.activeElement->tool == tool && !state.activeElement->textOutlineColor.isAuto()) {
                refColor = state.activeElement->textOutlineColor;
            } else if (!state.currentTextOutlineColor.isAuto()) {
                refColor = state.currentTextOutlineColor;
            } else {
                refColor = MarkupColor::White();
            }
        } else {
            refColor = (state.activeElement && state.activeElement->tool == tool && !state.activeElement->color.isAuto())
                           ? state.activeElement->color
                           : state.currentColor;
        }

        // 1. 调色板第 1 个按钮：当前颜色指示器 & 自定义调色盘入口 (PixPin 标杆：外圈连续彩虹圆角方环 + 白环间隙 + 内芯当前生效色实心圆角方块)
        specs.push_back({ToolbarCommand::ChooseCustomColor, tool, refColor, L"", 24.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.hasCustomColor, false, true, false});

        // 2. 紧接着右侧是预设颜色色块（第 1 个预设色块带 isSeparatorBefore = true，在彩虹当前色框与预设色块之间画浅灰竖向分隔线）
        bool firstColor = true;
        for (const auto& color : colors) {
            specs.push_back({ToolbarCommand::SelectColor, tool, color, L"", 20.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, firstColor});
            firstColor = false;
        }
        if (state.hasCustomColor) {
            bool isPreset = false;
            for (const auto& c : colors) {
                if (c.r == state.customColor.r && c.g == state.customColor.g && c.b == state.customColor.b) {
                    isPreset = true;
                    break;
                }
            }
            if (!isPreset) {
                specs.push_back({ToolbarCommand::SelectColor, tool, state.customColor, L"", 20.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            }
        }
    };

    if (state.isMarkupToolActive) {
        switch (state.currentTool) {
            case MarkupTool::Rectangle:
            case MarkupTool::Ellipse: {
                // 子形状切换（矩形 / 椭圆 / 直线）
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Rectangle, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Ellipse, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Line, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                // 填充 Toggle（PixPin 风格：左侧圆角复选框 + 右侧「填充 / Fill」文字，与左侧子形状切换器通过微刻痕分隔）
                specs.push_back({ToolbarCommand::ToggleFill, state.currentTool, {}, zh ? L"填充" : L"Fill", zh ? 58.0f : 56.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, true});

                // 线条样式下拉
                specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});

                // 线宽
                specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                // 圆角（仅矩形支持，同步回显选中矩形标注的真实圆角半径）
                if (state.currentTool == MarkupTool::Rectangle) {
                    const float rectRadius = (state.activeElement && state.activeElement->tool == MarkupTool::Rectangle)
                        ? state.activeElement->cornerRadius
                        : state.currentElementCornerRadius;
                    const int dispRectRad = static_cast<int>(std::round(rectRadius));
                    specs.push_back({ToolbarCommand::CycleElementCornerRadius, state.currentTool, {}, std::format(L"{}", dispRectRad), 48.0f, dispRectRad, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                }

                // 实心填充模式下提供独立「边框粗细 + 边框选色」双层复合控制胶囊
                if (state.currentFillMode) {
                    const int dispOutlineW = (state.activeElement && state.activeElement->tool == state.currentTool && state.activeElement->hasCustomOutline)
                        ? std::clamp(static_cast<int>(std::round(state.activeElement->outlineThickness)), 0, 12)
                        : std::clamp(state.currentOutlineWidth, 0, 12);
                    specs.push_back({ToolbarCommand::ToggleTextOutline, state.currentTool, {}, std::format(L"{}", dispOutlineW), 44.0f, dispOutlineW, LineStyle::Solid, ArrowStyle::Standard, state.isEditingOutlineColor, false, true, true});
                }

                // 调色板与任意选色
                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Pen:
            case MarkupTool::Highlight: {
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Pen, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Highlight, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                if (state.currentTool == MarkupTool::Pen) {
                    specs.push_back({ToolbarCommand::ToggleCurveMode, MarkupTool::Pen, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentSmoothCurve, false, true, true});
                }

                specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, state.currentTool != MarkupTool::Pen});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Arrow: {
                specs.push_back({ToolbarCommand::ToggleArrowStyleDropdown, MarkupTool::Arrow, {}, L"", 38.0f, 0, LineStyle::Solid, state.currentArrowStyle, false, true, true, false});
                specs.push_back({ToolbarCommand::ToggleCurveMode, MarkupTool::Arrow, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentSmoothCurve, false, true, false});
                specs.push_back({ToolbarCommand::ToggleStartCapDropdown, MarkupTool::Arrow, {}, L"", 38.0f, static_cast<int>(state.currentStartCap), LineStyle::Solid, ArrowStyle::Standard, false, true, true, true});
                specs.push_back({ToolbarCommand::ToggleEndCapDropdown, MarkupTool::Arrow, {}, L"", 38.0f, static_cast<int>(state.currentEndCap), LineStyle::Solid, ArrowStyle::Standard, false, true, true, false});
                specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, MarkupTool::Arrow, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Arrow, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                {
                    const int dispArrowOutlineW = (state.activeElement && state.activeElement->tool == MarkupTool::Arrow && state.activeElement->hasCustomOutline)
                        ? std::clamp(static_cast<int>(std::round(state.activeElement->outlineThickness)), 0, 12)
                        : std::clamp(state.currentOutlineWidth, 0, 12);
                    specs.push_back({ToolbarCommand::ToggleTextOutline, MarkupTool::Arrow, {}, std::format(L"{}", dispArrowOutlineW), 44.0f, dispArrowOutlineW, LineStyle::Solid, ArrowStyle::Standard, state.isEditingOutlineColor, false, true, true});
                }

                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Text: {
                const int cfgFontSize = std::clamp(
                    static_cast<int>(std::round(tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0))),
                    10, 144);
                const int displayFontSize = (state.activeElement && state.activeElement->tool == MarkupTool::Text && state.activeElement->fontSize > 0.0f)
                    ? std::clamp(static_cast<int>(std::round(state.activeElement->fontSize)), 10, 144)
                    : (state.currentStrokeWidth > 10 ? state.currentStrokeWidth : cfgFontSize);
                const float fontBtnWidth = (displayFontSize >= 100) ? 52.0f : 44.0f;
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Text, {}, std::format(L"{}", displayFontSize), fontBtnWidth, displayFontSize, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Text, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, false});

                // 文字描边体系：开启时默认采用智能高反差描边 (MarkupColor::Auto())，彻底告别平铺太极圆圈
                specs.push_back({ToolbarCommand::ToggleTextOutline, MarkupTool::Text, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentTextOutline, false, true, true});

                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Mosaic:
            case MarkupTool::Blur: {
                const MarkupTool effectivePrivacyTool =
                    (state.currentTool == MarkupTool::Blur || state.currentMosaicType == 1)
                        ? MarkupTool::Blur
                        : MarkupTool::Mosaic;
                specs.push_back({ToolbarCommand::SelectMosaicType, MarkupTool::Mosaic, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectMosaicType, MarkupTool::Blur, {}, L"", 28.0f, 1, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, effectivePrivacyTool, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, true});
                break;
            }

            case MarkupTool::Inpaint: {
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Inpaint, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                break;
            }

            case MarkupTool::Spotlight: {
                const bool isEllipse = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                    ? state.activeElement->fill
                    : state.currentSpotlightEllipse;
                const float spotRadius = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                    ? state.activeElement->cornerRadius
                    : state.currentElementCornerRadius;
                const float spotDim = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                    ? state.activeElement->spotlightDimAlpha
                    : state.currentSpotlightDimAlpha;
                const bool spotBorder = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                    ? state.activeElement->spotlightShowBorder
                    : state.currentSpotlightBorder;
                const int dimPct = std::clamp(static_cast<int>(std::round(spotDim * 100.0f)), 15, 85);

                specs.push_back({ToolbarCommand::ToggleSpotlightShape, MarkupTool::Spotlight, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, isEllipse, false, true, false});
                if (!isEllipse) {
                    const int dispSpotRad = static_cast<int>(std::round(spotRadius));
                    specs.push_back({ToolbarCommand::CycleElementCornerRadius, MarkupTool::Spotlight, {}, std::format(L"{}", dispSpotRad), 48.0f, dispSpotRad, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                }
                specs.push_back({ToolbarCommand::CycleSpotlightDim, MarkupTool::Spotlight, {}, std::format(L"{}%", dimPct), 54.0f, dimPct, LineStyle::Solid, ArrowStyle::Standard, false, false, true, true});
                specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Spotlight, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, spotBorder, false, true, false});
                appendColorPalette(MarkupTool::Spotlight);
                break;
            }

            case MarkupTool::Watermark: {
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Watermark, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Line: {
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Rectangle, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Ellipse, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Line, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                specs.push_back({ToolbarCommand::ToggleCurveMode, state.currentTool, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentSmoothCurve, false, true, true});
                specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::ToggleLineArrow, state.currentTool, {}, zh ? L"箭头" : L"Arrow", zh ? 58.0f : 64.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentLineArrowEnabled, false, true, true});
                if (state.currentLineArrowEnabled) {
                    specs.push_back({ToolbarCommand::ToggleStartCapDropdown, state.currentTool, {}, L"", 38.0f, static_cast<int>(state.currentStartCap), LineStyle::Solid, ArrowStyle::Standard, false, true, true, false});
                    specs.push_back({ToolbarCommand::ToggleEndCapDropdown, state.currentTool, {}, L"", 38.0f, static_cast<int>(state.currentEndCap), LineStyle::Solid, ArrowStyle::Standard, false, true, true, false});
                }

                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Number: {
                const NumberFormat dispFmt = (state.activeElement && state.activeElement->tool == MarkupTool::Number)
                    ? state.activeElement->numberFormat
                    : state.currentNumberFormat;
                specs.push_back({ToolbarCommand::ResetNumberCounter, MarkupTool::Number, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Number, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, false});
                specs.push_back({ToolbarCommand::ToggleNumberShape, MarkupTool::Number, {}, L"", 28.0f, static_cast<int>(state.currentNumberShape), LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::ToggleNumberFormatDropdown, MarkupTool::Number, {}, numberFormatToolbarTag(dispFmt), 48.0f, static_cast<int>(dispFmt), LineStyle::Solid, ArrowStyle::Standard, false, true, true, false});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Number, {}, std::format(L"{}", state.currentStrokeWidth), 46.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

                appendColorPalette(state.currentTool);
                break;
            }

            case MarkupTool::Magnifier: {
                const float dispScale = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier && state.activeElement->magnifierScale >= 1.1f)
                    ? state.activeElement->magnifierScale
                    : std::clamp(state.dynamicMagnifierScale, 1.5f, 6.0f);
                const int strokeVal = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                    ? std::clamp(static_cast<int>(std::round(state.activeElement->thickness)), 1, 12)
                    : std::clamp(state.currentStrokeWidth, 1, 12);
                const NumberBadgeShape shapeVal = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                    ? state.activeElement->numberShape
                    : state.currentMagnifierShape;
                const bool badgeVal = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                    ? state.activeElement->fill
                    : state.magnifierShowBadge;
                bool calloutVal = state.magnifierCalloutMode;
                if (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier) {
                    const bool unsetEnd = !state.activeElement->hasLeaderArrow &&
                                          state.activeElement->endPt.x == 0 && state.activeElement->endPt.y == 0 &&
                                          (state.activeElement->startPt.x != 0 || state.activeElement->startPt.y != 0);
                    const cv::Point lc = unsetEnd ? state.activeElement->startPt : state.activeElement->endPt;
                    calloutVal = std::hypot(static_cast<double>(lc.x - state.activeElement->startPt.x),
                                            static_cast<double>(lc.y - state.activeElement->startPt.y)) > 8.0;
                }

                const MagnifierCalloutStyle calloutStyleVal = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                    ? state.activeElement->magnifierCalloutStyle
                    : state.currentMagnifierCalloutStyle;
                auto calloutStyleShortLabel = [&](MagnifierCalloutStyle st) -> std::wstring {
                    switch (st) {
                        case MagnifierCalloutStyle::TangentFrustum: return zh ? L"双切线" : L"Tangent";
                        case MagnifierCalloutStyle::ElbowLeader:    return zh ? L"折线"   : L"Elbow";
                        case MagnifierCalloutStyle::DashedDirect:   return zh ? L"虚线"   : L"Dashed";
                        case MagnifierCalloutStyle::CenterPointer:  return zh ? L"单线"   : L"Single";
                        default:                                    return zh ? L"双切线" : L"Tangent";
                    }
                };

                specs.push_back({ToolbarCommand::ToggleMagnifierCallout, MarkupTool::Magnifier, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, calloutVal, false, true, false});
                specs.push_back({ToolbarCommand::ToggleMagnifierCalloutStyleDropdown, MarkupTool::Magnifier, {}, calloutStyleShortLabel(calloutStyleVal), 56.0f, static_cast<int>(calloutStyleVal), LineStyle::Solid, ArrowStyle::Standard, false, true, true, false});
                specs.push_back({ToolbarCommand::ToggleNumberShape, MarkupTool::Magnifier, {}, L"", 28.0f, static_cast<int>(shapeVal), LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::CycleMagnifierScale, MarkupTool::Magnifier, {}, std::format(L"{:.1f}x", dispScale), 48.0f, static_cast<int>(std::round(dispScale * 10.0f)), LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
                specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Magnifier, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, badgeVal, false, true, false});
                specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Magnifier, {}, std::format(L"{}", strokeVal), 46.0f, strokeVal, LineStyle::Solid, ArrowStyle::Standard, false, false, true, true});

                appendColorPalette(MarkupTool::Magnifier);
                break;
            }

            default:
                break;
        }
    }

    if (state.beautyShell.enabled) {
        const bool standaloneBeauty = !state.isMarkupToolActive;
        std::wstring bgName;
        if (zh) {
            switch (state.beautyShell.bgType) {
                case BeautyBackgroundType::StudioSlate: bgName = standaloneBeauty ? L"极光紫" : L"极光"; break;
                case BeautyBackgroundType::PaperChalk: bgName = standaloneBeauty ? L"日落珊瑚" : L"珊瑚"; break;
                case BeautyBackgroundType::SilkMist: bgName = standaloneBeauty ? L"翡翠海" : L"翡翠"; break;
                case BeautyBackgroundType::MidnightGraphite: bgName = standaloneBeauty ? L"曜石黑" : L"曜石"; break;
                case BeautyBackgroundType::PureMinimal: bgName = standaloneBeauty ? L"极简浅灰" : L"浅灰"; break;
                default: bgName = standaloneBeauty ? L"美化背景" : L"背景"; break;
            }
        } else {
            switch (state.beautyShell.bgType) {
                case BeautyBackgroundType::StudioSlate: bgName = standaloneBeauty ? L"Aurora Purple" : L"Aurora"; break;
                case BeautyBackgroundType::PaperChalk: bgName = standaloneBeauty ? L"Sunrise Coral" : L"Coral"; break;
                case BeautyBackgroundType::SilkMist: bgName = standaloneBeauty ? L"Emerald Sea" : L"Emerald"; break;
                case BeautyBackgroundType::MidnightGraphite: bgName = standaloneBeauty ? L"Obsidian Dark" : L"Obsidian"; break;
                case BeautyBackgroundType::PureMinimal: bgName = standaloneBeauty ? L"Minimal Light" : L"Minimal"; break;
                default: bgName = L"Theme"; break;
            }
        }
        const float bgWidth = standaloneBeauty ? (zh ? 88.0f : 132.0f) : (zh ? 66.0f : 90.0f);
        specs.push_back({
            ToolbarCommand::CycleBeautyBg, MarkupTool::Rectangle, {},
            bgName, bgWidth, static_cast<int>(state.beautyShell.bgType),
            LineStyle::Solid, ArrowStyle::Standard, false, true, true, !specs.empty()
        });
        specs.push_back({
            ToolbarCommand::CycleBeautyPadding, MarkupTool::Rectangle, {},
            std::format(L"{}", state.beautyShell.padding), 54.0f, state.beautyShell.padding,
            LineStyle::Solid, ArrowStyle::Standard, false, true, true, false
        });
        specs.push_back({
            ToolbarCommand::CycleBeautyRadius, MarkupTool::Rectangle, {},
            std::format(L"{}", static_cast<int>(state.beautyShell.cornerRadius)), 54.0f,
            static_cast<int>(state.beautyShell.cornerRadius),
            LineStyle::Solid, ArrowStyle::Standard, false, true, true, false
        });
    }

    return specs;
}

void rebuildSubmenuLayout(CaptureState& state, D2D1_SIZE_F surfaceSize, float scale) {
    state.submenuButtons.clear();
    state.openSubmenuRect = {};
    if (state.openSubmenu == SubmenuType::None || surfaceSize.width <= 0.0f || surfaceSize.height <= 0.0f) {
        return;
    }

    auto resolvePopupRect = [&](ToolbarCommand triggerCmd, float popupW, float popupH) -> D2D1_RECT_F {
        const float margin = 8.0f * scale;
        const float gapY = 6.0f * scale;
        D2D1_RECT_F anchorRect{};
        bool foundAnchor = false;
        for (const auto& btn : state.secondaryToolbarButtons) {
            if (btn.command == triggerCmd && btn.rect.right > btn.rect.left) {
                anchorRect = btn.rect;
                foundAnchor = true;
                break;
            }
        }
        if (!foundAnchor) {
            for (const auto& btn : state.toolbarButtons) {
                if (btn.command == triggerCmd && btn.rect.right > btn.rect.left) {
                    anchorRect = btn.rect;
                    foundAnchor = true;
                    break;
                }
            }
        }
        const bool hasSecondary = (state.secondaryToolbarRect.right > state.secondaryToolbarRect.left &&
                                   state.secondaryToolbarRect.bottom > state.secondaryToolbarRect.top);
        const bool hasPrimary = (state.primaryToolbarRect.right > state.primaryToolbarRect.left &&
                                 state.primaryToolbarRect.bottom > state.primaryToolbarRect.top);
        const D2D1_RECT_F baseBarRect = hasSecondary ? state.secondaryToolbarRect : state.primaryToolbarRect;
        const float rawX = foundAnchor ? anchorRect.left : (baseBarRect.left + margin);
        const float maxPopupX = (std::max)(margin, surfaceSize.width - popupW - margin);
        const float popupX = std::clamp(rawX, margin, maxPopupX);

        float bottomAnchor = baseBarRect.bottom;
        float topAnchor = baseBarRect.top;
        if (hasPrimary && hasSecondary) {
            const bool stacksAdjacent =
                std::abs(state.primaryToolbarRect.bottom - state.secondaryToolbarRect.top) <= 22.0f * scale ||
                std::abs(state.secondaryToolbarRect.bottom - state.primaryToolbarRect.top) <= 22.0f * scale;
            if (stacksAdjacent) {
                bottomAnchor = (std::max)(state.primaryToolbarRect.bottom, state.secondaryToolbarRect.bottom);
                topAnchor = (std::min)(state.primaryToolbarRect.top, state.secondaryToolbarRect.top);
            }
        }
        float popupY = bottomAnchor + gapY;
        if (popupY + popupH > surfaceSize.height - margin) {
            popupY = topAnchor - popupH - gapY;
        }
        popupY = std::clamp(popupY, margin, (std::max)(margin, surfaceSize.height - popupH - margin));
        return D2D1::RectF(popupX, popupY, popupX + popupW, popupY + popupH);
    };

    if (state.openSubmenu == SubmenuType::LineStyle) {
        const float menuW = 152.0f * scale;
        const float itemH = 32.0f * scale;
        const float itemGap = 4.0f * scale;
        const float padX = 8.0f * scale;
        const float padY = 8.0f * scale;
        const LineStyle styles[] = { LineStyle::Solid, LineStyle::Dashed, LineStyle::Dotted, LineStyle::DashDot };
        const float menuH = 4.0f * itemH + 3.0f * itemGap + 2.0f * padY;
        state.openSubmenuRect = resolvePopupRect(ToolbarCommand::ToggleLineStyleDropdown, menuW, menuH);
        for (size_t i = 0; i < 4; ++i) {
            const float iy = state.openSubmenuRect.top + padY + i * (itemH + itemGap);
            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectLineStyle;
            sbtn.lineStyleParam = styles[i];
            sbtn.rect = D2D1::RectF(state.openSubmenuRect.left + padX, iy, state.openSubmenuRect.right - padX, iy + itemH);
            state.submenuButtons.push_back(sbtn);
        }
    } else if (state.openSubmenu == SubmenuType::ArrowStyle) {
        const float menuW = 142.0f * scale;
        const float itemH = 31.0f * scale;
        const float itemGap = 3.5f * scale;
        const float padX = 8.0f * scale;
        const float padY = 8.0f * scale;
        const ArrowStyle styles[] = {
            ArrowStyle::Standard, ArrowStyle::Thin, ArrowStyle::Tech,
            ArrowStyle::DoubleEnded, ArrowStyle::CampaignMarch, ArrowStyle::MarchDoubleTrack
        };
        const float menuH = 6.0f * itemH + 5.0f * itemGap + 2.0f * padY;
        state.openSubmenuRect = resolvePopupRect(ToolbarCommand::ToggleArrowStyleDropdown, menuW, menuH);
        for (size_t i = 0; i < 6; ++i) {
            const float iy = state.openSubmenuRect.top + padY + i * (itemH + itemGap);
            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectArrowStyle;
            sbtn.arrowStyleParam = styles[i];
            sbtn.rect = D2D1::RectF(state.openSubmenuRect.left + padX, iy, state.openSubmenuRect.right - padX, iy + itemH);
            state.submenuButtons.push_back(sbtn);
        }
    } else if (state.openSubmenu == SubmenuType::StartCapStyle ||
               state.openSubmenu == SubmenuType::EndCapStyle) {
        const bool isStartMenu = (state.openSubmenu == SubmenuType::StartCapStyle);
        const float menuW = 158.0f * scale;
        const float itemH = 29.0f * scale;
        const float itemGap = 3.0f * scale;
        const float padX = 8.0f * scale;
        const float padY = 7.0f * scale;
        const float menuH = 10.0f * itemH + 9.0f * itemGap + 2.0f * padY;
        state.openSubmenuRect = resolvePopupRect(
            isStartMenu ? ToolbarCommand::ToggleStartCapDropdown : ToolbarCommand::ToggleEndCapDropdown,
            menuW, menuH);
        for (int i = 0; i < 10; ++i) {
            const float iy = state.openSubmenuRect.top + padY + i * (itemH + itemGap);
            ToolbarButton sbtn;
            sbtn.command = isStartMenu ? ToolbarCommand::SelectStartCap : ToolbarCommand::SelectEndCap;
            sbtn.intParam = i;
            sbtn.rect = D2D1::RectF(state.openSubmenuRect.left + padX, iy, state.openSubmenuRect.right - padX, iy + itemH);
            state.submenuButtons.push_back(sbtn);
        }
    }
}

}  // namespace

void rebuildCaptureToolbar(CaptureState& state, const D2D1_RECT_F& selectionRect,
                           D2D1_SIZE_F surfaceSize) {
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto close = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) < 0.01f;
    };
    const bool isLongImg = state.isLongImageMode();
    if (state.toolbarLayoutValid && !state.toolbarButtons.empty() &&
        state.toolbarLayoutMode == state.mode &&
        state.toolbarLayoutLongImage == isLongImg &&
        close(state.toolbarLayoutScale, scale) &&
        close(state.toolbarLayoutSurface.width, surfaceSize.width) &&
        close(state.toolbarLayoutSurface.height, surfaceSize.height) &&
        close(state.toolbarLayoutSelection.left, selectionRect.left) &&
        close(state.toolbarLayoutSelection.top, selectionRect.top) &&
        close(state.toolbarLayoutSelection.right, selectionRect.right) &&
        close(state.toolbarLayoutSelection.bottom, selectionRect.bottom) &&
        state.toolbarLayoutCustomPos == state.hasCustomToolbarPos &&
        (!state.hasCustomToolbarPos ||
         (close(state.toolbarLayoutCustomOrigin.x, state.customToolbarOrigin.x) &&
          close(state.toolbarLayoutCustomOrigin.y, state.customToolbarOrigin.y)))) {
        rebuildSubmenuLayout(state, surfaceSize, scale);
        return;
    }

    state.toolbarButtons.clear();
    state.secondaryToolbarButtons.clear();
    state.toolbarLayoutValid = false;
    if (surfaceSize.width <= 0.0f || surfaceSize.height <= 0.0f) return;

    const float buttonHeight = 32.0f * scale;
    const float gapX = (state.mode == OverlayMode::RecordRegion ? 5.0f : 4.0f) * scale;
    const float sepWidth = 8.5f * scale;
    const float paddingX = 7.5f * scale;
    const float paddingY = 6.0f * scale;
    const float tierGap = 7.0f * scale;
    const float screenMargin = 8.0f * scale;
    const float gripW = 14.0f * scale;
    const float gripGap = 4.0f * scale;
    const bool chinese = useChineseLabels();

    // 1. 主工具栏布局（含最左侧 2x3 六点灰色拖拽手柄区）
    const auto priSpecs = primaryButtonSpecs(state, chinese);
    float priContentW = gripW + gripGap;
    for (const auto& sp : priSpecs) {
        float extra = sp.isSeparatorBefore ? sepWidth : 0.0f;
        priContentW += extra + sp.baseWidth * scale + gapX;
    }
    if (!priSpecs.empty()) priContentW -= gapX;

    float priPanelW = priContentW + 2.0f * paddingX;
    float priPanelH = buttonHeight + 2.0f * paddingY;

    const float availRight =
        (isLongImg && state.longCanvas.minimapPanelRect.right > state.longCanvas.minimapPanelRect.left)
            ? std::max(40.0f * scale, state.longCanvas.minimapPanelRect.left - screenMargin)
            : (surfaceSize.width - screenMargin);

    // 针对极限小屏幕 / 超高 DPI 的响应式宽度自适应压缩 (Responsive Scaling Gate)
    const float maxAvailPriW = std::max(40.0f * scale, availRight - screenMargin - 2.0f * paddingX);
    float priShrink = (priContentW > maxAvailPriW && priContentW > 0.0f) ? (maxAvailPriW / priContentW) : 1.0f;
    if (priShrink < 1.0f) {
        priContentW *= priShrink;
        priPanelW = priContentW + 2.0f * paddingX;
    }

    // 2. 二级属性栏布局 (拉开间距与边距，呈现舒展呼吸感与充裕留白)
    const float secGapX = 7.0f * scale;
    const float secPaddingX = 10.0f * scale;
    const auto secSpecs = secondaryButtonSpecs(state, chinese);
    float secContentW = 0.0f;
    for (const auto& sp : secSpecs) {
        float extra = sp.isSeparatorBefore ? sepWidth : 0.0f;
        secContentW += extra + sp.baseWidth * scale + secGapX;
    }
    if (!secSpecs.empty()) secContentW -= secGapX;

    float secPanelW = secSpecs.empty() ? 0.0f : (secContentW + 2.0f * secPaddingX);
    float secPanelH = secSpecs.empty() ? 0.0f : (buttonHeight + 2.0f * paddingY);

    const float maxAvailSecW = std::max(40.0f * scale, availRight - screenMargin - 2.0f * secPaddingX);
    float secShrink = (secContentW > maxAvailSecW && secContentW > 0.0f) ? (maxAvailSecW / secContentW) : 1.0f;
    if (secShrink < 1.0f) {
        secContentW *= secShrink;
        secPanelW = secContentW + 2.0f * secPaddingX;
    }

    float totalHeight = priPanelH + (secSpecs.empty() ? 0.0f : (tierGap + secPanelH));

    // 智能几何避让计算：依次尝试下方外部、上方外部、下方内嵌、屏幕边缘安全退化
    const float selH = selectionRect.bottom - selectionRect.top;
    const float gap = 8.0f * scale;
    const float insideMargin = 12.0f * scale;
    const float spaceBelow = (surfaceSize.height - screenMargin) - (selectionRect.bottom + gap);
    const float spaceAbove = (selectionRect.top - gap) - screenMargin;

    const float maxPriX = std::max(screenMargin, availRight - priPanelW);
    float priPanelX = std::clamp(selectionRect.left, screenMargin, maxPriX);
    float priPanelY = 0.0f;

    if (state.hasCustomToolbarPos) {
        priPanelX = std::clamp(state.customToolbarOrigin.x, screenMargin,
                               std::max(screenMargin, surfaceSize.width - priPanelW - screenMargin));
        priPanelY = std::clamp(state.customToolbarOrigin.y, screenMargin,
                               std::max(screenMargin, surfaceSize.height - priPanelH - screenMargin));
    } else if (isLongImg && state.longCanvas.stageRect.bottom > state.longCanvas.stageRect.top) {
        const float stageCenterX = (state.longCanvas.stageRect.left + state.longCanvas.stageRect.right) * 0.5f;
        priPanelX = std::clamp(stageCenterX - priPanelW * 0.5f, screenMargin, maxPriX);
        priPanelY = std::clamp(state.longCanvas.stageRect.bottom + gap, screenMargin,
                               std::max(screenMargin, surfaceSize.height - totalHeight - screenMargin));

        float hintSafeRight = 520.0f * scale;
        float hintSafeTop = surfaceSize.height - 290.0f * scale;
        if (ShortcutHintOverlay::instance().isVisible()) {
            const RECT hintBounds = ShortcutHintOverlay::instance().getBounds();
            if (hintBounds.right > hintBounds.left && hintBounds.bottom > hintBounds.top) {
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                hintSafeRight = static_cast<float>(hintBounds.right - vx) + 14.0f * scale;
                hintSafeTop = static_cast<float>(hintBounds.top - vy) - 14.0f * scale;
            }
        }
        if (state.options.showShortcutHints &&
            (priPanelY + totalHeight > hintSafeTop) &&
            (priPanelX < hintSafeRight) &&
            (maxPriX > priPanelX)) {
            priPanelX = std::clamp(std::min(hintSafeRight, maxPriX), screenMargin, maxPriX);
        }
    } else {
        if (spaceBelow >= totalHeight) {
            // 1. 选区下方外部空间充裕（默认黄金布局）
            priPanelY = selectionRect.bottom + gap;
        } else if (spaceAbove >= totalHeight) {
            // 2. 选区上方外部空间充裕（自动翻转至上方外部，不遮挡选区内部）
            priPanelY = selectionRect.top - totalHeight - gap;
        } else if (selH >= totalHeight + 2.0f * insideMargin) {
            // 3. 上下外部空间均不足（大选区/近全屏），优雅内嵌至选区内部下方，避开上下手柄
            priPanelY = selectionRect.bottom - totalHeight - insideMargin;
        } else {
            // 4. 极窄/极小贴边选区，退化至空间较大的一侧并做屏幕安全边距夹取
            if (spaceAbove > spaceBelow) {
                priPanelY = std::clamp(selectionRect.top - totalHeight - gap, screenMargin,
                                       std::max(screenMargin, surfaceSize.height - totalHeight - screenMargin));
            } else {
                priPanelY = std::clamp(selectionRect.bottom + gap, screenMargin,
                                       std::max(screenMargin, surfaceSize.height - totalHeight - screenMargin));
            }
        }

        // 当主工具栏位于屏幕底部区域且左对齐会侵入左下角快捷键提示 HUD 驻留区时，
        // 优先向选区右侧对齐或向右平移避让，确保左下角双栏快捷键提示窗稳固驻留左下角不跳变。
        float hintSafeRight = 520.0f * scale;
        float hintSafeTop = surfaceSize.height - 290.0f * scale;
        if (ShortcutHintOverlay::instance().isVisible()) {
            const RECT hintBounds = ShortcutHintOverlay::instance().getBounds();
            if (hintBounds.right > hintBounds.left && hintBounds.bottom > hintBounds.top) {
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                hintSafeRight = static_cast<float>(hintBounds.right - vx) + 14.0f * scale;
                hintSafeTop = static_cast<float>(hintBounds.top - vy) - 14.0f * scale;
            }
        }
        const bool alignedRightToAvoidHint =
            state.options.showShortcutHints &&
            (priPanelY + totalHeight > hintSafeTop) &&
            (priPanelX < hintSafeRight) &&
            (maxPriX > priPanelX);
        if (alignedRightToAvoidHint) {
            float preferredRightAlignedX = selectionRect.right - priPanelW;
            float targetX = std::max(preferredRightAlignedX, std::min(hintSafeRight, maxPriX));
            priPanelX = std::clamp(targetX, screenMargin, maxPriX);
        }
    }

    state.primaryToolbarRect = D2D1::RectF(priPanelX, priPanelY, priPanelX + priPanelW, priPanelY + priPanelH);

    // 填充左侧 6 点灰色自由拖拽手柄区与主工具栏按钮
    const float gripLeft = priPanelX + paddingX * 0.65f;
    const float gripRight = gripLeft + gripW * priShrink;
    state.toolbarGripRect = D2D1::RectF(gripLeft, priPanelY + paddingY, gripRight, priPanelY + paddingY + buttonHeight);

    float curX = priPanelX + paddingX + (gripW + gripGap) * priShrink;
    float curY = priPanelY + paddingY;
    state.toolbarButtons.reserve(priSpecs.size());
    for (const auto& sp : priSpecs) {
        if (sp.isSeparatorBefore) {
            curX += sepWidth * priShrink;
        }
        float w = sp.baseWidth * scale * priShrink;
        ToolbarButton btn;
        btn.command = sp.command;
        btn.tool = sp.tool;
        btn.color = sp.color;
        btn.label = sp.label;
        btn.intParam = sp.intParam;
        btn.lineStyleParam = sp.lineStyleParam;
        btn.arrowStyleParam = sp.arrowStyleParam;
        btn.boolParam = sp.boolParam;
        btn.hasDropdown = sp.hasDropdown;
        btn.isSecondary = false;
        btn.isSeparatorBefore = sp.isSeparatorBefore;
        btn.rect = D2D1::RectF(curX, curY, curX + w, curY + buttonHeight);
        state.toolbarButtons.push_back(btn);
        curX += w + gapX * priShrink;
    }

    // 填充二级属性栏按钮：与一级主工具栏左边缘对齐；当主工具栏贴近屏幕底边时自动向上翻转
    if (!secSpecs.empty()) {
        const float secRightLimit = state.hasCustomToolbarPos ? (surfaceSize.width - screenMargin) : availRight;
        const float maxSecX = std::max(screenMargin, secRightLimit - secPanelW);
        float secPanelX = std::clamp(priPanelX, screenMargin, maxSecX);
        float secPanelY = priPanelY + priPanelH + tierGap;
        if (secPanelY + secPanelH > surfaceSize.height - screenMargin &&
            priPanelY - tierGap - secPanelH >= screenMargin) {
            secPanelY = priPanelY - tierGap - secPanelH;
        }
        state.secondaryToolbarRect = D2D1::RectF(secPanelX, secPanelY, secPanelX + secPanelW, secPanelY + secPanelH);

        float sCurX = secPanelX + secPaddingX;
        float sCurY = secPanelY + paddingY;
        state.secondaryToolbarButtons.reserve(secSpecs.size());
        for (const auto& sp : secSpecs) {
            if (sp.isSeparatorBefore) {
                sCurX += sepWidth * secShrink;
            }
            float w = sp.baseWidth * scale * secShrink;
            ToolbarButton btn;
            btn.command = sp.command;
            btn.tool = sp.tool;
            btn.color = sp.color;
            btn.label = sp.label;
            btn.intParam = sp.intParam;
            btn.lineStyleParam = sp.lineStyleParam;
            btn.arrowStyleParam = sp.arrowStyleParam;
            btn.boolParam = sp.boolParam;
            btn.hasDropdown = sp.hasDropdown;
            btn.isSecondary = true;
            btn.isSeparatorBefore = sp.isSeparatorBefore;
            btn.rect = D2D1::RectF(sCurX, sCurY, sCurX + w, sCurY + buttonHeight);
            state.secondaryToolbarButtons.push_back(btn);
            sCurX += w + secGapX * secShrink;
        }
    } else {
        state.secondaryToolbarRect = {};
    }

    // 3. 选区侧边自适应悬浮圆钮布局 (PixPin 标杆：紧贴选区右上角外侧顶部对齐，4 枚独立悬浮正圆微晶圆钮)
    state.selectionSideButtons.clear();
    float selW = selectionRect.right - selectionRect.left;
    if (state.mode == OverlayMode::Screenshot && selW >= 20.0f * scale && selH >= 20.0f * scale) {
        const float sideBtnSz = 30.0f * scale;
        const float sideGapY = 8.0f * scale;
        const int sideCount = 4; // 1: 圆角, 2: 比例锁定, 3: 反选/全屏扩展, 4: 重置选区圆角与比例
        const float sideH = sideCount * sideBtnSz + (sideCount - 1) * sideGapY;
        const float sideW = sideBtnSz;
        const float outerGapX = 10.0f * scale;

        // 默认紧贴选区右侧外沿（长图模式下避让右侧 Minimap）
        bool placedInside = false;
        float sideX = selectionRect.right + outerGapX;
        // 若右侧空间不足，平滑翻转至选区左侧外沿
        if (sideX + sideW > availRight) {
            sideX = selectionRect.left - sideW - outerGapX;
        }
        // 若两侧均触达屏幕边缘（如全屏选区），优雅内嵌至选区右上角内侧
        if (sideX < screenMargin) {
            placedInside = true;
            sideX = std::clamp(selectionRect.right - sideW - outerGapX, screenMargin,
                               (std::max)(screenMargin, availRight - sideW));
        }

        // Y 轴紧贴选区顶部起排（对标 PixPin 右上角对齐；全屏内嵌时自动避让顶部尺寸胶囊）
        const float minSideY = (isLongImg && state.longCanvas.stageRect.top > screenMargin)
                                   ? state.longCanvas.stageRect.top + 8.0f * scale
                                   : screenMargin;
        float sideY = selectionRect.top;
        if (placedInside && state.sizeHudRect.right > state.sizeHudRect.left &&
            sideX < state.sizeHudRect.right + 6.0f * scale &&
            sideX + sideW > state.sizeHudRect.left - 6.0f * scale &&
            sideY < state.sizeHudRect.bottom + 8.0f * scale) {
            sideY = state.sizeHudRect.bottom + 8.0f * scale;
        }
        const float maxSideY = (std::max)(minSideY, surfaceSize.height - sideH - screenMargin);
        sideY = std::clamp(sideY, minSideY, maxSideY);

        state.selectionSideRect = D2D1::RectF(sideX, sideY, sideX + sideW, sideY + sideH);

        // 按钮 1: 调节选区圆角
        ToolbarButton btnCorner;
        btnCorner.command = ToolbarCommand::SideToggleCornerRadius;
        btnCorner.tool = MarkupTool::Rectangle;
        btnCorner.rect = D2D1::RectF(sideX, sideY, sideX + sideBtnSz, sideY + sideBtnSz);
        state.selectionSideButtons.push_back(btnCorner);

        // 按钮 2: 选区固定比例 (自由/16:9/4:3/1:1/黄金比例)
        ToolbarButton btnAspect;
        btnAspect.command = ToolbarCommand::SideCycleAspectRatio;
        btnAspect.tool = MarkupTool::Rectangle;
        btnAspect.intParam = static_cast<int>(state.aspectRatio);
        float y2 = sideY + sideBtnSz + sideGapY;
        btnAspect.rect = D2D1::RectF(sideX, y2, sideX + sideBtnSz, y2 + sideBtnSz);
        state.selectionSideButtons.push_back(btnAspect);

        // 按钮 3: 反向选择 / 选区扩展
        ToolbarButton btnInvert;
        btnInvert.command = ToolbarCommand::SideInvertSelection;
        btnInvert.tool = MarkupTool::Rectangle;
        float y3 = y2 + sideBtnSz + sideGapY;
        btnInvert.rect = D2D1::RectF(sideX, y3, sideX + sideBtnSz, y3 + sideBtnSz);
        state.selectionSideButtons.push_back(btnInvert);

        // 按钮 4: 重置选区圆角与比例状态
        ToolbarButton btnReset;
        btnReset.command = ToolbarCommand::SideResetSelection;
        btnReset.tool = MarkupTool::Rectangle;
        float y4 = y3 + sideBtnSz + sideGapY;
        btnReset.rect = D2D1::RectF(sideX, y4, sideX + sideBtnSz, y4 + sideBtnSz);
        state.selectionSideButtons.push_back(btnReset);
    } else {
        state.selectionSideRect = {};
    }

    // 4. 展开态下拉子菜单几何与按钮预计算（确保在下一渲染帧前及无头测试中即可精确命中）
    rebuildSubmenuLayout(state, surfaceSize, scale);

    state.toolbarLayoutSelection = selectionRect;
    state.toolbarLayoutSurface = surfaceSize;
    state.toolbarLayoutScale = scale;
    state.toolbarLayoutMode = state.mode;
    state.toolbarLayoutChinese = chinese;
    state.toolbarLayoutLongImage = isLongImg;
    state.toolbarLayoutCustomPos = state.hasCustomToolbarPos;
    state.toolbarLayoutCustomOrigin = state.customToolbarOrigin;
    state.toolbarLayoutValid = true;
}

std::wstring tooltipForButton(const ToolbarButton& button, bool chinese) {
    if (button.command == ToolbarCommand::SelectTool) {
        switch (button.tool) {
            case MarkupTool::Rectangle:
                if (!button.isSecondary && button.hasDropdown) {
                    return chinese ? L"几何图形 (R) · 右键或点小箭头切换聚光灯 (Shift+0)" : L"Shapes (R) · Right-click for Spotlight (Shift+0)";
                }
                return chinese ? L"矩形 (R)" : L"Rectangle (R)";
            case MarkupTool::Line: return chinese ? L"直线 (L)" : L"Line (L)";
            case MarkupTool::Ellipse: return chinese ? L"椭圆 (O)" : L"Ellipse (O)";
            case MarkupTool::Arrow: return chinese ? L"箭头 (Q / Alt+A)" : L"Arrow (Q / Alt+A)";
            case MarkupTool::Pen: return chinese ? L"涂鸦画笔 (P)" : L"Pen (P)";
            case MarkupTool::Highlight: return chinese ? L"荧光笔 (H)" : L"Highlighter (H)";
            case MarkupTool::Mosaic: return chinese ? L"马赛克 (M)" : L"Mosaic (M)";
            case MarkupTool::Blur: return chinese ? L"高斯模糊 (M)" : L"Gaussian Blur (M)";
            case MarkupTool::Text: return chinese ? L"添加文本 (T)" : L"Text (T)";
            case MarkupTool::Number: return chinese ? L"序号标记 (N)" : L"Numbered Step (N)";
            case MarkupTool::Magnifier: return chinese ? L"放大镜 (Z)" : L"Magnifier (Z)";
            case MarkupTool::Spotlight:
                return chinese ? L"聚光灯 (Shift+0) · 右键切换几何图形 (Shift+1)" : L"Spotlight (Shift+0) · Right-click for Shapes (Shift+1)";
            case MarkupTool::Watermark: return chinese ? L"水印图章" : L"Watermark";
            case MarkupTool::Inpaint: return chinese ? L"智能消除 / 橡皮擦 (E / I)" : L"Inpaint / Eraser (E / I)";
            default: return chinese ? L"标注工具" : L"Tool";
        }
    }
    if (button.command == ToolbarCommand::SelectColor) {
        if (button.color.r == 244 && button.color.g == 63 && button.color.b == 94) return chinese ? L"珊瑚红" : L"Coral Red";
        if (button.color.r == 245 && button.color.g == 158 && button.color.b == 11) return chinese ? L"曜石橙" : L"Amber Orange";
        if (button.color.r == 234 && button.color.g == 179 && button.color.b == 8) return chinese ? L"明快黄" : L"Yellow";
        if (button.color.r == 16 && button.color.g == 185 && button.color.b == 129) return chinese ? L"薄荷绿" : L"Mint Green";
        if (button.color.r == 59 && button.color.g == 130 && button.color.b == 246) return chinese ? L"科技蓝" : L"Tech Blue";
        if (button.color.r == 30 && button.color.g == 41 && button.color.b == 59) return chinese ? L"极客黑" : L"Black";
        if (button.color.r == 255 && button.color.g == 255 && button.color.b == 255) return chinese ? L"纯洁白" : L"White";
        wchar_t hexBuf[32];
        swprintf_s(hexBuf, L"#%02X%02X%02X", button.color.r, button.color.g, button.color.b);
        return chinese ? (std::wstring(L"自定义色 (") + hexBuf + L")") : (std::wstring(L"Custom Color (") + hexBuf + L")");
    }
    switch (button.command) {
        case ToolbarCommand::ChooseCustomColor:
            return chinese ? L"点击设置颜色，滚轮调整不透明度，Ctrl+滚轮调整亮度"
                           : L"Click to pick color, Scroll for opacity, Ctrl+Scroll for brightness";
        case ToolbarCommand::Undo: return chinese ? L"撤销 (Ctrl+Z)" : L"Undo (Ctrl+Z)";
        case ToolbarCommand::Redo: return chinese ? L"重做 (Ctrl+Y / Ctrl+Shift+Z)" : L"Redo (Ctrl+Y / Ctrl+Shift+Z)";
        case ToolbarCommand::Clear: return chinese ? L"清空所有标注" : L"Clear all annotations";
        case ToolbarCommand::ToggleCornerRadius: return chinese ? L"调节选区圆角 ([ / ])" : L"Corner radius ([ / ])";
        case ToolbarCommand::SideToggleCornerRadius: return chinese ? L"调节选区圆角半径" : L"Selection Corner Radius";
        case ToolbarCommand::SideInvertSelection: return chinese ? L"全屏选区 / 还原原选区" : L"Full Screen / Restore Selection";
        case ToolbarCommand::SideResetSelection: return chinese ? L"重置选区直角与自由比例" : L"Reset Sharp Corners & Free Ratio";
        case ToolbarCommand::ExtractText: return chinese ? L"提取文字 (OCR)" : L"Extract text (OCR)";
        case ToolbarCommand::PinWindow: return chinese ? L"贴图置顶到屏幕 (Ctrl+T)" : L"Pin to screen (Ctrl+T)";
        case ToolbarCommand::ScrollCapture: return chinese ? L"长截图" : L"Scrolling capture";
        case ToolbarCommand::StartRecord: return chinese ? L"区域录屏" : L"Record Video";
        case ToolbarCommand::ToggleBeautyShell: return chinese ? L"美化外壳导出 (B)" : L"Beauty Shell (B)";
        case ToolbarCommand::SideCycleAspectRatio: return chinese ? L"选区固定比例 (Shift+1..5)" : L"Aspect Ratio (Shift+1..5)";
        case ToolbarCommand::ToggleNumberShape:
            if (button.tool == MarkupTool::Magnifier) {
                return chinese ? L"切换透镜形状 (圆形/圆角方镜)" : L"Toggle Lens Shape (Circle/Rounded Square)";
            }
            return chinese ? L"切换序号背景形状 (圆形/圆角矩形)" : L"Toggle Badge Shape (Circle/Rounded Rect)";
        case ToolbarCommand::ToggleNumberFormatDropdown:
            return chinese ? L"序号样式 (1,2,3 / ABC / I,II,III / 一,二,三)" : L"Number Style (1,2,3 / ABC / I,II,III / CJK)";
        case ToolbarCommand::ToggleSpotlightShape:
            return chinese ? L"切换聚光灯形状 (矩形/椭圆)" : L"Toggle Spotlight Shape (Rect/Ellipse)";
        case ToolbarCommand::CycleSpotlightDim:
            return chinese ? L"背景弱化浓度 (滚轮调节)" : L"Background Dimming (Scroll to adjust)";
        case ToolbarCommand::CycleMagnifierScale:
            return chinese ? L"切换放大倍率 (1.5x / 2.0x / 3.0x / 4.0x)" : L"Magnification (1.5x / 2.0x / 3.0x / 4.0x)";
        case ToolbarCommand::ToggleMagnifierCallout:
            return chinese ? L"引出放大 (开/关)" : L"Callout Mode (On/Off)";
        case ToolbarCommand::ToggleMagnifierCalloutStyleDropdown:
            return chinese ? L"连线样式 (双切线 / 折线 / 虚线 / 单线)" : L"Line Style (Tangent / Elbow / Dashed / Single)";
        case ToolbarCommand::CycleBeautyBg: return chinese ? L"外壳质感风格 (Shift+B)" : L"Shell Background Theme (Shift+B)";
        case ToolbarCommand::CycleBeautyPadding: return chinese ? L"外壳留白边距 (Alt+B / 滚轮微调)" : L"Shell Padding (Alt+B / Scroll)";
        case ToolbarCommand::CycleBeautyRadius: return chinese ? L"外壳圆角弧度 (滚轮微调)" : L"Shell Corner Radius (Scroll)";
        case ToolbarCommand::Copy: return chinese ? L"复制到剪贴板 (Ctrl+C)" : L"Copy to clipboard (Ctrl+C)";
        case ToolbarCommand::Confirm: return chinese ? L"完成并复制 (Enter)" : L"Done & Copy (Enter)";
        case ToolbarCommand::Cancel: return chinese ? L"取消 (Esc)" : L"Cancel (Esc)";
        case ToolbarCommand::ToggleFill:
            if (button.tool == MarkupTool::Text) {
                return chinese ? L"文字背景底衬 (开/关)" : L"Text Background Pill (On/Off)";
            }
            if (button.tool == MarkupTool::Magnifier) {
                return chinese ? L"显示/隐藏倍率角标" : L"Show/Hide Magnification Badge";
            }
            if (button.tool == MarkupTool::Spotlight) {
                return chinese ? L"显示/隐藏聚光区域描边" : L"Show/Hide Spotlight Border";
            }
            return chinese ? L"切换填充模式 (空心/实心)" : L"Toggle Fill (Outline/Solid)";
        case ToolbarCommand::ToggleTextOutline:
            if (button.tool == MarkupTool::Text) {
                return chinese ? L"文字高反差描边 (开/关 · 滚轮调描边粗细 · Shift+点击改描边色)"
                               : L"Text High-Contrast Outline (On/Off · Scroll for width · Shift+Click for color)";
            }
            return chinese ? L"边框粗细与颜色 (点击切换改边框色 · 滚轮调边框粗细)"
                           : L"Border Width & Color (Click to pick border color · Scroll for width)";
        case ToolbarCommand::SelectTextOutlineColor: {
            if (button.color.isAuto()) return chinese ? L"自适应高对比描边" : L"Adaptive High-Contrast Outline";
            if (button.color == MarkupColor::Black()) return chinese ? L"曜石黑描边" : L"Obsidian Black Outline";
            if (button.color == MarkupColor::White()) return chinese ? L"纯白描边" : L"Pure White Outline";
            return chinese ? L"描边颜色" : L"Outline Color";
        }
        case ToolbarCommand::CycleStrokeWidth:
            switch (button.tool) {
                case MarkupTool::Text:
                    return chinese ? L"调节文本字号 (滚轮微调)" : L"Font Size (Scroll to adjust)";
                case MarkupTool::Mosaic:
                    return chinese ? L"调节马赛克粒度 (滚轮微调)" : L"Mosaic Size (Scroll to adjust)";
                case MarkupTool::Blur:
                    return chinese ? L"调节模糊强度 (滚轮微调)" : L"Blur Strength (Scroll to adjust)";
                case MarkupTool::Inpaint:
                    return chinese ? L"调节涂抹笔刷大小 (滚轮微调)" : L"Brush Size (Scroll to adjust)";
                case MarkupTool::Number:
                    return chinese ? L"调节序号尺寸 (滚轮微调)" : L"Badge Size (Scroll to adjust)";
                case MarkupTool::Magnifier:
                    return chinese ? L"调节镜框粗细 (滚轮微调)" : L"Rim Thickness (Scroll to adjust)";
                default:
                    return chinese ? L"调节线条粗细 (滚轮微调)" : L"Stroke Width (Scroll to adjust)";
            }
        case ToolbarCommand::CycleElementCornerRadius: return chinese ? L"调节标注圆角 (滚轮可直接微调)" : L"Corner Radius (Scroll to adjust)";
        case ToolbarCommand::ToggleLineStyleDropdown: return chinese ? L"线条样式 (实线/虚线)" : L"Line Style (Solid/Dashed)";
        case ToolbarCommand::SelectLineStyle: return chinese ? L"切换线条样式" : L"Select Line Style";
        case ToolbarCommand::ToggleArrowStyleDropdown: return chinese ? L"箭头样式" : L"Arrow Style";
        case ToolbarCommand::SelectArrowStyle: return chinese ? L"切换箭头样式" : L"Select Arrow Style";
        case ToolbarCommand::ToggleCurveMode: return chinese ? L"折线 / 平滑曲线切换" : L"Toggle Smooth Curve";
        case ToolbarCommand::ToggleLineArrow: return chinese ? L"启用端点箭头样式" : L"Enable Endpoint Caps";
        case ToolbarCommand::ToggleStartCapDropdown: return chinese ? L"起点端点样式 (10种)" : L"Start Endpoint Style (10 types)";
        case ToolbarCommand::SelectStartCap: return chinese ? L"选择起点端点样式" : L"Select Start Cap";
        case ToolbarCommand::ToggleEndCapDropdown: return chinese ? L"终点端点样式 (10种)" : L"End Endpoint Style (10 types)";
        case ToolbarCommand::SelectEndCap: return chinese ? L"选择终点端点样式" : L"Select End Cap";
        case ToolbarCommand::SelectMosaicType: return chinese ? (button.intParam == 0 ? L"像素马赛克" : L"高斯模糊") : (button.intParam == 0 ? L"Pixel Mosaic" : L"Gaussian Blur");
        case ToolbarCommand::ResetNumberCounter: return chinese ? L"重置序号起始编号为 1" : L"Reset Number Counter (1)";
        case ToolbarCommand::RecordToggleFormat: return chinese ? L"录制格式 (MP4 / GIF)" : L"Recording Format (MP4 / GIF)";
        case ToolbarCommand::RecordCycleFps: return chinese ? L"录制帧率" : L"Frame Rate";
        case ToolbarCommand::RecordCycleQuality: return chinese ? L"录制画质" : L"Video Quality";
        case ToolbarCommand::RecordCycleClickEffect: return chinese ? L"鼠标点击特效" : L"Click Effects";
        case ToolbarCommand::RecordToggleKeycast: return chinese ? L"按键回显 (Keycast)" : L"Keycast Overlay";
        case ToolbarCommand::RecordToggleSystemAudio:
            return button.boolParam
                ? (chinese ? L"系统声音: 已开启 (点击静音)" : L"System Audio: ON (Click to mute)")
                : (chinese ? L"系统声音: 已静音 (点击开启)" : L"System Audio: Muted (Click to enable)");
        case ToolbarCommand::RecordToggleMicrophone:
            return button.boolParam
                ? (chinese ? L"麦克风: 已开启 (点击静音)" : L"Microphone: ON (Click to mute)")
                : (chinese ? L"麦克风: 已静音 (点击开启)" : L"Microphone: Muted (Click to enable)");
        case ToolbarCommand::RecordStartConfirm: return chinese ? L"开始录制 (Enter)" : L"Start Recording (Enter)";
        default: return L"";
    }
}

D2D1_RECT_F calculateTooltipRect(const D2D1_RECT_F& buttonRect, float tw, float th, float scale,
                                 D2D1_SIZE_F surfaceSize, const D2D1_RECT_F& selRect,
                                 const D2D1_RECT_F& secondaryRect,
                                 const D2D1_RECT_F& primaryRect,
                                 bool isVerticalSideBar) {
    const float safeScale = (scale > 0.0f) ? scale : 1.0f;
    const float edgePad = 4.0f * safeScale;
    const float gap = 6.0f * safeScale;
    const bool selValid = (selRect.right > selRect.left && selRect.bottom > selRect.top);

    // 1. 选区侧边竖排圆钮专属侧向水平弹出（彻底杜绝上下遮挡相邻侧边圆钮）
    if (isVerticalSideBar) {
        const float bcy = (buttonRect.top + buttonRect.bottom) * 0.5f;
        const float maxTy = (std::max)(edgePad, surfaceSize.height - th - edgePad);
        const float ty = std::clamp(bcy - th * 0.5f, edgePad, maxTy);
        const float sideGap = 8.0f * safeScale;

        const bool preferRight = !selValid || buttonRect.left >= selRect.right - 2.0f * safeScale;
        float tx = preferRight ? (buttonRect.right + sideGap) : (buttonRect.left - tw - sideGap);
        if (preferRight && tx + tw > surfaceSize.width - edgePad) {
            tx = buttonRect.left - tw - sideGap;
        } else if (!preferRight && tx < edgePad) {
            tx = buttonRect.right + sideGap;
        }
        const float maxTx = (std::max)(edgePad, surfaceSize.width - tw - edgePad);
        tx = std::clamp(tx, edgePad, maxTx);
        return D2D1::RectF(tx, ty, tx + tw, ty + th);
    }

    // 2. 水平主/副工具栏跨栏双向防遮挡弹出
    float bcx = (buttonRect.left + buttonRect.right) * 0.5f;
    float minTx = 8.0f * safeScale;
    float maxTx = (std::max)(minTx, surfaceSize.width - tw - 8.0f * safeScale);
    float tx = std::clamp(bcx - tw * 0.5f, minTx, maxTx);

    const bool hasPrimary = (primaryRect.right > primaryRect.left && primaryRect.bottom > primaryRect.top);
    const bool hasSecondary = (secondaryRect.right > secondaryRect.left && secondaryRect.bottom > secondaryRect.top);

    auto horizOverlaps = [&](const D2D1_RECT_F& bar) {
        return (tx < bar.right + 4.0f * safeScale) && (tx + tw > bar.left - 4.0f * safeScale);
    };

    // 检查按钮正上方/正下方是否紧贴着另一条工具栏（主栏或二级属性栏）
    bool barImmediatelyAbove = false;
    bool barImmediatelyBelow = false;
    float combinedTop = buttonRect.top;
    float combinedBottom = buttonRect.bottom;

    auto inspectBar = [&](const D2D1_RECT_F& bar) {
        if (bar.right <= bar.left || bar.bottom <= bar.top) return;
        combinedTop = (std::min)(combinedTop, bar.top);
        combinedBottom = (std::max)(combinedBottom, bar.bottom);
        if (!horizOverlaps(bar)) return;
        // 若该 bar 位于当前按钮正上方紧邻区域（距离 <= 18*scale）
        if (bar.bottom <= buttonRect.top + 4.0f * safeScale &&
            buttonRect.top - bar.bottom <= 22.0f * safeScale) {
            barImmediatelyAbove = true;
        }
        // 若该 bar 位于当前按钮正下方紧邻区域
        if (bar.top >= buttonRect.bottom - 4.0f * safeScale &&
            bar.top - buttonRect.bottom <= 22.0f * safeScale) {
            barImmediatelyBelow = true;
        }
    };
    if (hasPrimary) inspectBar(primaryRect);
    if (hasSecondary) inspectBar(secondaryRect);

    float ty = 0.0f;
    if (barImmediatelyAbove && !barImmediatelyBelow) {
        // 典型场景：二级属性栏位于主工具栏正下方 -> 优先向下弹出，绝不盖住上方主工具栏！
        ty = buttonRect.bottom + gap;
        if (ty + th > surfaceSize.height - edgePad) {
            ty = combinedTop - th - gap;
        }
    } else if (barImmediatelyBelow && !barImmediatelyAbove) {
        // 典型场景：主工具栏下方紧贴二级属性栏 -> 优先向上弹出；顶部空间不足翻转时直接跨过二级属性栏底边！
        ty = buttonRect.top - th - gap;
        if (ty < edgePad) {
            ty = combinedBottom + gap;
        }
    } else if (selValid && buttonRect.top >= selRect.bottom - 4.0f * safeScale && !hasSecondary) {
        ty = buttonRect.bottom + gap;
        if (ty + th > surfaceSize.height - edgePad) {
            ty = combinedTop - th - gap;
        }
    } else {
        ty = buttonRect.top - th - gap;
        if (ty < edgePad) {
            ty = combinedBottom + gap;
        }
    }

    const float maxTy = (std::max)(edgePad, surfaceSize.height - th - edgePad);
    ty = std::clamp(ty, edgePad, maxTy);
    return D2D1::RectF(tx, ty, tx + tw, ty + th);
}

void transitionToRecordMode(CaptureState& state) {
    if (state.mode == OverlayMode::RecordRegion) return;
    state.mode = OverlayMode::RecordRegion;
    state.markup.clearAll();
    state.activeElement = nullptr;
    closeAllCapturePopups(state, false);
    state.toolbarLayoutValid = false;
    if (state.options.showShortcutHints && state.state.load() != OverlayState::Idle) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::RecordSelecting);
    }
    const bool zh = tools3000::core::isUiLanguageChinese();
    state.loupeToastUntil = GetTickCount() + 1500;
    state.loupeToastMessage = zh ? L"已切换至区域录屏模式" : L"Switched to Record Mode";
}

SizeMenuLayout computeSizeMenuLayoutFromRect(const D2D1_RECT_F& menuRect, float scale) {
    const float s = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 5.0f);
    SizeMenuLayout out{};
    out.menuRect = menuRect;

    const float menuX = menuRect.left;
    const float menuY = menuRect.top;
    const float menuW = menuRect.right - menuRect.left;
    const float padX = 14.0f * s;

    // 1. 顶部 px / dp 双联分段滑块胶囊 (Segmented Pill Switcher)
    const float segTop = menuY + 12.0f * s;
    const float segH = 30.0f * s;
    out.segmentTrackRect = D2D1::RectF(menuX + padX, segTop, menuX + menuW - padX, segTop + segH);

    const float inset = 2.5f * s;
    const float midX = (out.segmentTrackRect.left + out.segmentTrackRect.right) * 0.5f;
    out.pxSegmentRect = D2D1::RectF(out.segmentTrackRect.left + inset, segTop + inset, midX - 1.0f * s, segTop + segH - inset);
    out.dpSegmentRect = D2D1::RectF(midX + 1.0f * s, segTop + inset, out.segmentTrackRect.right - inset, segTop + segH - inset);

    // 2. 单行换算关系说明条 + 微晶细分隔线
    const float formulaTop = segTop + segH + 6.0f * s;
    const float formulaH = 20.0f * s;
    out.formulaRect = D2D1::RectF(menuX + padX + 2.0f * s, formulaTop, menuX + menuW - padX, formulaTop + formulaH);
    out.dividerY = formulaTop + formulaH + 5.0f * s;

    // 3. 显示坐标行 + 4. 显示单位行 (整行对齐 + 右侧胶囊 Toggle 开关垂直中心绝对同轴)
    const float rowPadX = 8.0f * s;
    const float rowH = 30.0f * s;
    const float swW = 32.0f * s;
    const float swH = 18.0f * s;
    const float swX = menuX + menuW - padX - swW;

    const float row3Top = out.dividerY + 6.0f * s;
    const float sw3Y = row3Top + (rowH - swH) * 0.5f;
    out.positionRowRect = D2D1::RectF(menuX + rowPadX, row3Top, menuX + menuW - rowPadX, row3Top + rowH);
    out.positionSwitchRect = D2D1::RectF(swX, sw3Y, swX + swW, sw3Y + swH);

    const float row4Top = row3Top + rowH + 2.0f * s;
    const float sw4Y = row4Top + (rowH - swH) * 0.5f;
    out.unitRowRect = D2D1::RectF(menuX + rowPadX, row4Top, menuX + menuW - rowPadX, row4Top + rowH);
    out.unitSwitchRect = D2D1::RectF(swX, sw4Y, swX + swW, sw4Y + swH);

    return out;
}

SizeMenuLayout computeSizeMenuLayout(const D2D1_RECT_F& hudRect, D2D1_SIZE_F surfaceSize, float scale) {
    const float s = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 5.0f);
    const float menuW = 216.0f * s;
    const float menuH = 150.0f * s;
    const float margin = 6.0f * s;
    const float gap = 8.0f * s;

    float menuX = std::clamp(hudRect.left, margin, (std::max)(margin, surfaceSize.width - menuW - margin));
    float menuY = hudRect.top - menuH - gap;
    if (menuY < margin) {
        menuY = hudRect.bottom + gap;
    }
    if (menuY + menuH > surfaceSize.height - margin) {
        menuY = hudRect.top - menuH - gap;
    }
    menuY = std::clamp(menuY, margin, (std::max)(margin, surfaceSize.height - menuH - margin));

    return computeSizeMenuLayoutFromRect(D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH), s);
}

void adjustPaletteColorByWheel(CaptureState& state, MarkupTool btnTool, int delta, bool adjustBrightness) {
    const int stepSign = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (stepSign == 0) return;

    auto applyAdjust = [&](MarkupColor& col) {
        if (adjustBrightness) {
            const float rf = col.r / 255.0f;
            const float gf = col.g / 255.0f;
            const float bf = col.b / 255.0f;
            const float cmax = (std::max)({rf, gf, bf});
            const float cmin = (std::min)({rf, gf, bf});
            const float diff = cmax - cmin;
            float h = 0.0f;
            if (diff > 1e-5f) {
                if (cmax == rf) {
                    h = std::fmod((gf - bf) / diff, 6.0f);
                    if (h < 0.0f) h += 6.0f;
                } else if (cmax == gf) {
                    h = (bf - rf) / diff + 2.0f;
                } else {
                    h = (rf - gf) / diff + 4.0f;
                }
                h *= 60.0f;
            }
            float s = (cmax > 1e-5f) ? (diff / cmax) : 0.0f;
            float v = cmax;
            const float step = static_cast<float>(stepSign) * 0.06f;
            if (stepSign > 0 && v >= 0.999f && s > 0.0f) {
                s = std::clamp(s - 0.06f, 0.0f, 1.0f);
            } else {
                v = std::clamp(v + step, 0.0f, 1.0f);
            }
            const float c = v * s;
            const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
            const float m = v - c;
            float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
            if (h < 60.0f)       { r1 = c;    g1 = x;    b1 = 0.0f; }
            else if (h < 120.0f) { r1 = x;    g1 = c;    b1 = 0.0f; }
            else if (h < 180.0f) { r1 = 0.0f; g1 = c;    b1 = x;    }
            else if (h < 240.0f) { r1 = 0.0f; g1 = x;    b1 = c;    }
            else if (h < 300.0f) { r1 = x;    g1 = 0.0f; b1 = c;    }
            else                 { r1 = c;    g1 = 0.0f; b1 = x;    }
            col.r = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((r1 + m) * 255.0f)), 0, 255));
            col.g = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((g1 + m) * 255.0f)), 0, 255));
            col.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround((b1 + m) * 255.0f)), 0, 255));
        } else {
            const int curA = (col.a > 0) ? static_cast<int>(col.a) : 255;
            col.a = static_cast<uint8_t>(std::clamp(curA + stepSign * 15, 30, 255));
        }
    };

    const bool forMagnifier = (btnTool == MarkupTool::Magnifier || state.currentTool == MarkupTool::Magnifier ||
                               (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier));
    if (state.isEditingOutlineColor && !forMagnifier) {
        if (state.activeElement && !state.activeElement->textOutlineColor.isAuto()) {
            state.currentTextOutlineColor = state.activeElement->textOutlineColor;
        } else if (state.currentTextOutlineColor.isAuto()) {
            state.currentTextOutlineColor = MarkupColor::White();
        }
        applyAdjust(state.currentTextOutlineColor);
        if (state.activeElement) {
            state.activeElement->textOutlineColor = state.currentTextOutlineColor;
            state.activeElement->hasCustomOutline = true;
            state.activeElement->textOutline = true;
            if (state.activeElement->tool == MarkupTool::Text) {
                state.activeElement->textRenderSize = cv::Size(0, 0);
            }
        }
        for (auto& btn : state.secondaryToolbarButtons) {
            if (btn.command == ToolbarCommand::ChooseCustomColor) {
                btn.color = state.currentTextOutlineColor;
            }
        }
        state.toolbarLayoutValid = false;
        return;
    }

    if (state.activeElement && !state.activeElement->color.isAuto()) {
        if (forMagnifier) {
            state.currentMagnifierColor = state.activeElement->color;
        } else {
            state.currentColor = state.activeElement->color;
        }
    }

    if (forMagnifier) {
        applyAdjust(state.currentMagnifierColor);
        state.currentColor = state.currentMagnifierColor;
    } else {
        applyAdjust(state.currentColor);
    }
    if (state.activeElement) {
        state.activeElement->color =
            (state.activeElement->tool == MarkupTool::Magnifier)
                ? state.currentMagnifierColor
                : state.currentColor;
        if (state.activeElement->tool == MarkupTool::Text) {
            state.activeElement->textRenderSize = cv::Size(0, 0);
        }
    }
    for (auto& btn : state.secondaryToolbarButtons) {
        if (btn.command == ToolbarCommand::ChooseCustomColor) {
            btn.color = (btn.tool == MarkupTool::Magnifier)
                ? state.currentMagnifierColor
                : state.currentColor;
        }
    }
    state.toolbarLayoutValid = false;
}

}  // namespace tools3000::capture
