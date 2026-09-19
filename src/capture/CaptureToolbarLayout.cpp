#include "capture/CaptureToolbarLayout.h"
#include "capture/ShortcutHintOverlay.h"

#include "core/config/ConfigManager.h"
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
    const auto language = tools3000::core::ConfigManager::instance().get<std::string>(
        "/general/language", "auto");
    return language == "zh-CN" ||
           (language == "auto" && tools3000::core::WinUtils::isSystemLanguageChinese());
}

// 1. 主工具栏规格
std::vector<ButtonSpec> primaryButtonSpecs(const CaptureState& state, bool zh) {
    if (state.mode == OverlayMode::RecordRegion) {
        std::vector<ButtonSpec> specs;
        // 1. 录制格式 (MP4 / GIF)
        specs.push_back({
            ToolbarCommand::RecordToggleFormat, MarkupTool::Rectangle, {},
            state.recordSetup.format == RecordFormat::GIF ? L"GIF" : L"MP4",
            44.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
        });

        // 2. 录制帧率 (15 / 30 / 60 fps)
        specs.push_back({
            ToolbarCommand::RecordCycleFps, MarkupTool::Rectangle, {},
            std::format(L"{} fps", state.recordSetup.fps),
            56.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
        });

        // 3. 画质选择 (标准 / 高清 / 原画)
        std::wstring qText = zh
            ? (state.recordSetup.qualityLevel == 0 ? L"标准" : (state.recordSetup.qualityLevel == 1 ? L"高清" : L"原画"))
            : (state.recordSetup.qualityLevel == 0 ? L"Std" : (state.recordSetup.qualityLevel == 1 ? L"HD" : L"Ultra"));
        specs.push_back({
            ToolbarCommand::RecordCycleQuality, MarkupTool::Rectangle, {},
            qText,
            46.0f, state.recordSetup.qualityLevel, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false
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
            zh ? 82.0f : 88.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.showClickEffects || state.recordSetup.showClickZoom,
            false, false, true // separator
        });

        // 5. 按键回显 (Keycast)
        specs.push_back({
            ToolbarCommand::RecordToggleKeycast, MarkupTool::Rectangle, {},
            L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.includeKeycast, false, false, false
        });

        // 6. 系统声音 (GIF 格式下置灰/不可选)
        specs.push_back({
            ToolbarCommand::RecordToggleSystemAudio, MarkupTool::Rectangle, {},
            L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.captureSystemAudio && state.recordSetup.format != RecordFormat::GIF,
            false, false, true // separator
        });

        // 7. 麦克风 (GIF 格式下置灰/不可选)
        specs.push_back({
            ToolbarCommand::RecordToggleMicrophone, MarkupTool::Rectangle, {},
            L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            state.recordSetup.captureMicrophone && state.recordSetup.format != RecordFormat::GIF,
            false, false, false
        });

        // 8. 取消按钮
        specs.push_back({
            ToolbarCommand::Cancel, MarkupTool::Rectangle, {},
            L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            false, false, false, true // separator
        });

        // 9. 正式开始录制 (醒目红点主按钮)
        specs.push_back({
            ToolbarCommand::RecordStartConfirm, MarkupTool::Rectangle, {},
            zh ? L"● 开始录制" : L"● Start",
            zh ? 88.0f : 78.0f, 0, LineStyle::Solid, ArrowStyle::Standard,
            true, false, false, false
        });

        return specs;
    }

    std::vector<ButtonSpec> specs;
    // 形状工具组（矩形 / 椭圆 / 直线）
    MarkupTool shapeTool = MarkupTool::Rectangle;
    if (state.currentTool == MarkupTool::Ellipse) shapeTool = MarkupTool::Ellipse;
    else if (state.currentTool == MarkupTool::Line) shapeTool = MarkupTool::Line;
    specs.push_back({ToolbarCommand::SelectTool, 
                     shapeTool,
                     {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 画笔工具组（铅笔 / 荧光笔）
    specs.push_back({ToolbarCommand::SelectTool,
                     (state.currentTool == MarkupTool::Highlight ? MarkupTool::Highlight : MarkupTool::Pen),
                     {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 箭头工具组
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Arrow, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 文本工具
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Text, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 序号工具
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Number, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 马赛克工具
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Mosaic, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 智能消除 / 橡皮擦
    specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Inpaint, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 操作组（带前置分隔线）
    specs.push_back({ToolbarCommand::Undo, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::Redo, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    // 系统能力（带前置分隔线）
    specs.push_back({ToolbarCommand::ExtractText, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::PinWindow, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    specs.push_back({ToolbarCommand::ScrollCapture, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    specs.push_back({ToolbarCommand::StartRecord, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    specs.push_back({ToolbarCommand::ToggleBeautyShell, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.beautyShell.enabled, false, false, false});

    // 动作组（带前置分隔线）
    specs.push_back({ToolbarCommand::Copy, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, true});
    specs.push_back({ToolbarCommand::Cancel, MarkupTool::Rectangle, {}, L"", 30.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});
    specs.push_back({ToolbarCommand::Confirm, MarkupTool::Rectangle, {}, L"", 38.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, false, false});

    return specs;
}

// 2. 二级属性栏规格（根据 currentTool 动态变幻）
std::vector<ButtonSpec> secondaryButtonSpecs(const CaptureState& state, bool zh) {
    if (state.mode == OverlayMode::RecordRegion || !state.isMarkupToolActive) return {};

    static const std::array colors{
        MarkupColor::Red(), MarkupColor::Orange(), MarkupColor::Yellow(),
        MarkupColor::Green(), MarkupColor::Blue(), MarkupColor::Black(), MarkupColor::White(),
    };

    std::vector<ButtonSpec> specs;

    auto appendColorPalette = [&](MarkupTool tool) {
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
        // 选择任意颜色微晶按钮 (紧邻色块右侧，精致彩虹色环 / 调色盘矢量图标)
        specs.push_back({ToolbarCommand::ChooseCustomColor, tool, state.customColor, L"", 22.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.hasCustomColor, false, true, false});
    };

    switch (state.currentTool) {
        case MarkupTool::Rectangle:
        case MarkupTool::Ellipse: {
            // 子形状切换（矩形 / 椭圆 / 直线）
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Rectangle, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Ellipse, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Line, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            // 填充 Toggle（纯矢量图标，无生硬汉字）
            specs.push_back({ToolbarCommand::ToggleFill, state.currentTool, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, false});

            // 线条样式下拉
            specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});

            // 线宽
            specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 36.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            // 圆角（仅矩形支持）
            if (state.currentTool == MarkupTool::Rectangle) {
                specs.push_back({ToolbarCommand::CycleElementCornerRadius, state.currentTool, {}, std::format(L"{}", static_cast<int>(state.currentElementCornerRadius)), 36.0f, static_cast<int>(state.currentElementCornerRadius), LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            }

            // 调色板与任意选色
            appendColorPalette(state.currentTool);
            break;
        }

        case MarkupTool::Pen:
        case MarkupTool::Highlight: {
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Pen, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Highlight, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});
            specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 36.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            appendColorPalette(state.currentTool);
            break;
        }

        case MarkupTool::Arrow: {
            specs.push_back({ToolbarCommand::ToggleArrowStyleDropdown, MarkupTool::Arrow, {}, L"", 38.0f, 0, LineStyle::Solid, state.currentArrowStyle, false, true, true, false});
            specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, MarkupTool::Arrow, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});
            specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Arrow, {}, std::format(L"{}", state.currentStrokeWidth), 36.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            appendColorPalette(state.currentTool);
            break;
        }

        case MarkupTool::Text: {
            specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Text, {}, std::format(L"{}", state.currentStrokeWidth > 10 ? state.currentStrokeWidth : 18), 38.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Text, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, false});

            // 文字描边体系：微晶描边开关 + 描边色彩选项 (自适应反色/曜石黑/纯白)
            specs.push_back({ToolbarCommand::ToggleTextOutline, MarkupTool::Text, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentTextOutline, false, true, false});
            if (state.currentTextOutline) {
                specs.push_back({ToolbarCommand::SelectTextOutlineColor, MarkupTool::Text, MarkupColor::Auto(), zh ? L"自" : L"Auto", 24.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentTextOutlineColor.isAuto(), false, true, false});
                specs.push_back({ToolbarCommand::SelectTextOutlineColor, MarkupTool::Text, MarkupColor::Black(), L"", 20.0f, 1, LineStyle::Solid, ArrowStyle::Standard, !state.currentTextOutlineColor.isAuto() && state.currentTextOutlineColor == MarkupColor::Black(), false, true, false});
                specs.push_back({ToolbarCommand::SelectTextOutlineColor, MarkupTool::Text, MarkupColor::White(), L"", 20.0f, 2, LineStyle::Solid, ArrowStyle::Standard, !state.currentTextOutlineColor.isAuto() && state.currentTextOutlineColor == MarkupColor::White(), false, true, false});
            }

            appendColorPalette(state.currentTool);
            break;
        }

        case MarkupTool::Mosaic: {
            specs.push_back({ToolbarCommand::SelectMosaicType, MarkupTool::Mosaic, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectMosaicType, MarkupTool::Mosaic, {}, L"", 28.0f, 1, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::CycleStrokeWidth, MarkupTool::Mosaic, {}, std::format(L"{}", state.currentStrokeWidth * 3), 36.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            break;
        }

        case MarkupTool::Line: {
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Rectangle, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Ellipse, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::SelectTool, MarkupTool::Line, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            specs.push_back({ToolbarCommand::ToggleLineStyleDropdown, state.currentTool, {}, L"", 38.0f, 0, state.currentLineStyle, ArrowStyle::Standard, false, true, true, false});
            specs.push_back({ToolbarCommand::CycleStrokeWidth, state.currentTool, {}, std::format(L"{}", state.currentStrokeWidth), 36.0f, state.currentStrokeWidth, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            appendColorPalette(state.currentTool);
            break;
        }

        case MarkupTool::Number: {
            specs.push_back({ToolbarCommand::ResetNumberCounter, MarkupTool::Number, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});
            specs.push_back({ToolbarCommand::ToggleFill, MarkupTool::Number, {}, L"", 28.0f, 0, LineStyle::Solid, ArrowStyle::Standard, state.currentFillMode, false, true, false});
            specs.push_back({ToolbarCommand::ToggleNumberShape, MarkupTool::Number, {}, L"", 28.0f, static_cast<int>(state.currentNumberShape), LineStyle::Solid, ArrowStyle::Standard, false, false, true, false});

            appendColorPalette(state.currentTool);
            break;
        }

        default:
            break;
    }

    if (state.beautyShell.enabled) {
        std::wstring bgName;
        if (zh) {
            switch (state.beautyShell.bgType) {
                case BeautyBackgroundType::StudioSlate: bgName = L"冷灰"; break;
                case BeautyBackgroundType::PaperChalk: bgName = L"纸白"; break;
                case BeautyBackgroundType::SilkMist: bgName = L"晨雾"; break;
                case BeautyBackgroundType::MidnightGraphite: bgName = L"曜石"; break;
                case BeautyBackgroundType::PureMinimal: bgName = L"纯影"; break;
                default: bgName = L"背景"; break;
            }
        } else {
            switch (state.beautyShell.bgType) {
                case BeautyBackgroundType::StudioSlate: bgName = L"Slate"; break;
                case BeautyBackgroundType::PaperChalk: bgName = L"Paper"; break;
                case BeautyBackgroundType::SilkMist: bgName = L"Mist"; break;
                case BeautyBackgroundType::MidnightGraphite: bgName = L"Dark"; break;
                case BeautyBackgroundType::PureMinimal: bgName = L"Pure"; break;
                default: bgName = L"Bg"; break;
            }
        }
        specs.push_back({
            ToolbarCommand::CycleBeautyBg, MarkupTool::Rectangle, {},
            bgName, 44.0f, static_cast<int>(state.beautyShell.bgType),
            LineStyle::Solid, ArrowStyle::Standard, false, false, true, !specs.empty()
        });
        specs.push_back({
            ToolbarCommand::CycleBeautyPadding, MarkupTool::Rectangle, {},
            std::format(L"{}px", state.beautyShell.padding), 42.0f, state.beautyShell.padding,
            LineStyle::Solid, ArrowStyle::Standard, false, false, true, false
        });
        specs.push_back({
            ToolbarCommand::CycleBeautyRadius, MarkupTool::Rectangle, {},
            std::format(L"R{}", static_cast<int>(state.beautyShell.cornerRadius)), 38.0f,
            static_cast<int>(state.beautyShell.cornerRadius),
            LineStyle::Solid, ArrowStyle::Standard, false, false, true, false
        });
    }

    return specs;
}

}  // namespace

void rebuildCaptureToolbar(CaptureState& state, const D2D1_RECT_F& selectionRect,
                           D2D1_SIZE_F surfaceSize) {
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto close = [](float lhs, float rhs) {
        return std::abs(lhs - rhs) < 0.01f;
    };
    if (state.toolbarLayoutValid && !state.toolbarButtons.empty() &&
        state.toolbarLayoutMode == state.mode && close(state.toolbarLayoutScale, scale) &&
        close(state.toolbarLayoutSurface.width, surfaceSize.width) &&
        close(state.toolbarLayoutSurface.height, surfaceSize.height) &&
        close(state.toolbarLayoutSelection.left, selectionRect.left) &&
        close(state.toolbarLayoutSelection.top, selectionRect.top) &&
        close(state.toolbarLayoutSelection.right, selectionRect.right) &&
        close(state.toolbarLayoutSelection.bottom, selectionRect.bottom)) {
        return;
    }

    state.toolbarButtons.clear();
    state.secondaryToolbarButtons.clear();
    state.toolbarLayoutValid = false;
    if (surfaceSize.width <= 0.0f || surfaceSize.height <= 0.0f) return;

    const float buttonHeight = 30.0f * scale;
    const float gapX = 3.5f * scale;
    const float sepWidth = 7.0f * scale;
    const float paddingX = 8.0f * scale;
    const float paddingY = 6.0f * scale;
    const float tierGap = 6.0f * scale;
    const float screenMargin = 8.0f * scale;
    const bool chinese = useChineseLabels();

    // 1. 主工具栏布局
    const auto priSpecs = primaryButtonSpecs(state, chinese);
    float priContentW = 0.0f;
    for (const auto& sp : priSpecs) {
        float extra = sp.isSeparatorBefore ? sepWidth : 0.0f;
        priContentW += extra + sp.baseWidth * scale + gapX;
    }
    if (!priSpecs.empty()) priContentW -= gapX;

    float priPanelW = priContentW + 2.0f * paddingX;
    float priPanelH = buttonHeight + 2.0f * paddingY;

    // 针对极限小屏幕 / 超高 DPI 的响应式宽度自适应压缩 (Responsive Scaling Gate)
    const float maxAvailPriW = std::max(40.0f * scale, surfaceSize.width - 2.0f * screenMargin - 2.0f * paddingX);
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

    const float maxAvailSecW = std::max(40.0f * scale, surfaceSize.width - 2.0f * screenMargin - 2.0f * secPaddingX);
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

    float priPanelX = std::clamp(selectionRect.left, screenMargin,
                                 std::max(screenMargin, surfaceSize.width - priPanelW - screenMargin));
    float priPanelY = 0.0f;

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

    state.primaryToolbarRect = D2D1::RectF(priPanelX, priPanelY, priPanelX + priPanelW, priPanelY + priPanelH);

    // 填充主工具栏按钮
    float curX = priPanelX + paddingX;
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

    // 填充二级属性栏按钮
    if (!secSpecs.empty()) {
        float secPanelX = priPanelX;
        if (secPanelX + secPanelW > surfaceSize.width - screenMargin) {
            secPanelX = surfaceSize.width - secPanelW - screenMargin;
        }
        float secPanelY = priPanelY + priPanelH + tierGap;
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

    // 3. 选区侧边自适应浮动菜单布局 (PixPin 标杆：紧贴选区右侧垂直排列，带屏幕右边缘自适应翻转)
    state.selectionSideButtons.clear();
    float selW = selectionRect.right - selectionRect.left;
    if (state.mode == OverlayMode::Screenshot && selW >= 20.0f * scale && selH >= 20.0f * scale) {
        const float sideBtnSz = 28.0f * scale;
        const float sideGapY = 5.0f * scale;
        const float sidePad = 4.0f * scale;
        const int sideCount = 4; // 1: 圆角, 2: 比例锁定, 3: 反选, 4: 重置直角
        float sideH = sideCount * sideBtnSz + (sideCount - 1) * sideGapY + 2.0f * sidePad;
        float sideW = sideBtnSz + 2.0f * sidePad;

        // 默认紧贴选区右侧
        float sideX = selectionRect.right + 8.0f * scale;
        // 如果右侧空间不足以容纳侧边栏 + 滑块弹窗 (约 220px)，自动平滑翻转至选区左侧
        if (sideX + sideW + 200.0f * scale > surfaceSize.width - screenMargin) {
            sideX = selectionRect.left - sideW - 8.0f * scale;
        }
        // 如果左侧也超出屏幕，则紧贴选区内部右侧
        if (sideX < screenMargin) {
            sideX = std::max(screenMargin, selectionRect.left + 8.0f * scale);
        }

        // Y 轴垂直居中对齐选区
        float sideY = selectionRect.top + (selH - sideH) * 0.5f;
        sideY = std::clamp(sideY, screenMargin, surfaceSize.height - sideH - screenMargin);

        state.selectionSideRect = D2D1::RectF(sideX, sideY, sideX + sideW, sideY + sideH);

        // 按钮 1: 调节选区圆角
        ToolbarButton btnCorner;
        btnCorner.command = ToolbarCommand::SideToggleCornerRadius;
        btnCorner.tool = MarkupTool::Rectangle;
        btnCorner.rect = D2D1::RectF(sideX + sidePad, sideY + sidePad, sideX + sidePad + sideBtnSz, sideY + sidePad + sideBtnSz);
        state.selectionSideButtons.push_back(btnCorner);

        // 按钮 2: 选区固定比例 (自由/16:9/4:3/1:1/黄金比例)
        ToolbarButton btnAspect;
        btnAspect.command = ToolbarCommand::SideCycleAspectRatio;
        btnAspect.tool = MarkupTool::Rectangle;
        btnAspect.intParam = static_cast<int>(state.aspectRatio);
        float y2 = sideY + sidePad + sideBtnSz + sideGapY;
        btnAspect.rect = D2D1::RectF(sideX + sidePad, y2, sideX + sidePad + sideBtnSz, y2 + sideBtnSz);
        state.selectionSideButtons.push_back(btnAspect);

        // 按钮 3: 反向选择 / 选区扩展
        ToolbarButton btnInvert;
        btnInvert.command = ToolbarCommand::SideInvertSelection;
        btnInvert.tool = MarkupTool::Rectangle;
        float y3 = y2 + sideBtnSz + sideGapY;
        btnInvert.rect = D2D1::RectF(sideX + sidePad, y3, sideX + sidePad + sideBtnSz, y3 + sideBtnSz);
        state.selectionSideButtons.push_back(btnInvert);

        // 按钮 4: 重置选区直角 (0px)
        ToolbarButton btnReset;
        btnReset.command = ToolbarCommand::SideResetSelection;
        btnReset.tool = MarkupTool::Rectangle;
        float y4 = y3 + sideBtnSz + sideGapY;
        btnReset.rect = D2D1::RectF(sideX + sidePad, y4, sideX + sidePad + sideBtnSz, y4 + sideBtnSz);
        state.selectionSideButtons.push_back(btnReset);
    } else {
        state.selectionSideRect = {};
    }

    state.toolbarLayoutSelection = selectionRect;
    state.toolbarLayoutSurface = surfaceSize;
    state.toolbarLayoutScale = scale;
    state.toolbarLayoutMode = state.mode;
    state.toolbarLayoutChinese = chinese;
    state.toolbarLayoutValid = true;
}

std::wstring tooltipForButton(const ToolbarButton& button, bool chinese) {
    if (button.command == ToolbarCommand::SelectTool) {
        switch (button.tool) {
            case MarkupTool::Rectangle: return chinese ? L"矩形 (R)" : L"Rectangle (R)";
            case MarkupTool::Line: return chinese ? L"直线 (L)" : L"Line (L)";
            case MarkupTool::Ellipse: return chinese ? L"椭圆 (O)" : L"Ellipse (O)";
            case MarkupTool::Arrow: return chinese ? L"箭头 (A)" : L"Arrow (A)";
            case MarkupTool::Pen: return chinese ? L"涂鸦画笔 (P)" : L"Pen (P)";
            case MarkupTool::Highlight: return chinese ? L"荧光笔 (H)" : L"Highlighter (H)";
            case MarkupTool::Mosaic: return chinese ? L"马赛克 (M)" : L"Mosaic (M)";
            case MarkupTool::Text: return chinese ? L"添加文本 (T)" : L"Text (T)";
            case MarkupTool::Number: return chinese ? L"序号标记 (N)" : L"Numbered Step (N)";
            case MarkupTool::Magnifier: return chinese ? L"局部放大 (Z)" : L"Magnifier (Z)";
            case MarkupTool::Spotlight: return chinese ? L"聚光灯 (S)" : L"Spotlight (S)";
            case MarkupTool::Watermark: return chinese ? L"水印图章" : L"Watermark";
            case MarkupTool::Inpaint: return chinese ? L"智能消除 / 橡皮擦 (E)" : L"Inpaint / Eraser (E)";
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
        case ToolbarCommand::ChooseCustomColor: return chinese ? L"选择自定义颜色 (全色域调色板)" : L"Custom Color (Color Palette)";
        case ToolbarCommand::Undo: return chinese ? L"撤销 (Ctrl+Z)" : L"Undo (Ctrl+Z)";
        case ToolbarCommand::Redo: return chinese ? L"重做 (Ctrl+Y)" : L"Redo (Ctrl+Y)";
        case ToolbarCommand::Clear: return chinese ? L"清空所有标注" : L"Clear all annotations";
        case ToolbarCommand::ToggleCornerRadius: return chinese ? L"调节选区圆角 ([ / ])" : L"Corner radius ([ / ])";
        case ToolbarCommand::SideToggleCornerRadius: return chinese ? L"调节选区圆角半径" : L"Selection Corner Radius";
        case ToolbarCommand::SideInvertSelection: return chinese ? L"反向选择 / 选区扩展" : L"Invert Selection";
        case ToolbarCommand::SideResetSelection: return chinese ? L"重置选区直角 (0px)" : L"Reset Corner Radius";
        case ToolbarCommand::ExtractText: return chinese ? L"提取文字 (OCR)" : L"Extract text (OCR)";
        case ToolbarCommand::PinWindow: return chinese ? L"贴图置顶到屏幕 (Ctrl+T)" : L"Pin to screen (Ctrl+T)";
        case ToolbarCommand::ScrollCapture: return chinese ? L"长截图" : L"Scrolling capture";
        case ToolbarCommand::StartRecord: return chinese ? L"区域录屏" : L"Record Video";
        case ToolbarCommand::ToggleBeautyShell: return chinese ? L"美化外壳导出 (B)" : L"Beauty Shell (B)";
        case ToolbarCommand::SideCycleAspectRatio: return chinese ? L"选区固定比例 (Shift+1..5)" : L"Aspect Ratio (Shift+1..5)";
        case ToolbarCommand::ToggleNumberShape: return chinese ? L"切换序号形状 (圆形/方角)" : L"Toggle Badge Shape (Circle/Square)";
        case ToolbarCommand::CycleBeautyBg: return chinese ? L"外壳质感风格 (Shift+B)" : L"Shell Background Theme (Shift+B)";
        case ToolbarCommand::CycleBeautyPadding: return chinese ? L"外壳留白边距 (Alt+B)" : L"Shell Padding (Alt+B)";
        case ToolbarCommand::CycleBeautyRadius: return chinese ? L"外壳圆角弧度" : L"Shell Corner Radius";
        case ToolbarCommand::Copy: return chinese ? L"复制到剪贴板 (Ctrl+C)" : L"Copy to clipboard (Ctrl+C)";
        case ToolbarCommand::Confirm: return chinese ? L"完成并复制 (Enter)" : L"Done & Copy (Enter)";
        case ToolbarCommand::Cancel: return chinese ? L"取消 (Esc)" : L"Cancel (Esc)";
        case ToolbarCommand::ToggleFill: return chinese ? L"切换填充模式" : L"Toggle Fill";
        case ToolbarCommand::ToggleTextOutline: return chinese ? L"文字高反差描边 (开/关)" : L"Text High-Contrast Outline (On/Off)";
        case ToolbarCommand::SelectTextOutlineColor: {
            if (button.color.isAuto()) return chinese ? L"自适应高对比描边" : L"Adaptive High-Contrast Outline";
            if (button.color == MarkupColor::Black()) return chinese ? L"曜石黑描边" : L"Obsidian Black Outline";
            if (button.color == MarkupColor::White()) return chinese ? L"纯白描边" : L"Pure White Outline";
            return chinese ? L"描边颜色" : L"Outline Color";
        }
        case ToolbarCommand::CycleStrokeWidth: return chinese ? L"调节线宽 / 字号" : L"Adjust Stroke Width";
        case ToolbarCommand::CycleElementCornerRadius: return chinese ? L"调节标注圆角" : L"Corner Radius";
        case ToolbarCommand::ToggleLineStyleDropdown: return chinese ? L"线条样式 (实线/虚线)" : L"Line Style (Solid/Dashed)";
        case ToolbarCommand::ToggleArrowStyleDropdown: return chinese ? L"箭头样式" : L"Arrow Style";
        case ToolbarCommand::SelectMosaicType: return chinese ? (button.intParam == 0 ? L"像素马赛克" : L"高斯模糊") : (button.intParam == 0 ? L"Pixel Mosaic" : L"Gaussian Blur");
        case ToolbarCommand::ResetNumberCounter: return chinese ? L"重置序号起始编号为 1" : L"Reset Number Counter (1)";
        case ToolbarCommand::RecordToggleFormat: return chinese ? L"录制格式" : L"Recording Format";
        case ToolbarCommand::RecordCycleFps: return chinese ? L"录制帧率" : L"Frame Rate";
        case ToolbarCommand::RecordCycleQuality: return chinese ? L"录制画质" : L"Video Quality";
        case ToolbarCommand::RecordCycleClickEffect: return chinese ? L"鼠标点击特效" : L"Click Effects";
        case ToolbarCommand::RecordToggleKeycast: return chinese ? L"按键回显 (Keycast)" : L"Keycast Overlay";
        case ToolbarCommand::RecordToggleSystemAudio: return chinese ? L"录制系统声音" : L"Record System Audio";
        case ToolbarCommand::RecordToggleMicrophone: return chinese ? L"录制麦克风" : L"Record Microphone";
        case ToolbarCommand::RecordStartConfirm: return chinese ? L"开始录制 (Enter)" : L"Start Recording (Enter)";
        default: return L"";
    }
}

D2D1_RECT_F calculateTooltipRect(const D2D1_RECT_F& buttonRect, float tw, float th, float scale,
                                 D2D1_SIZE_F surfaceSize, const D2D1_RECT_F& selRect,
                                 const D2D1_RECT_F& secondaryRect) {
    const float safeScale = (scale > 0.0f) ? scale : 1.0f;
    float bcx = (buttonRect.left + buttonRect.right) * 0.5f;
    float minTx = 8.0f * safeScale;
    float maxTx = (std::max)(minTx, surfaceSize.width - tw - 8.0f * safeScale);
    float tx = std::clamp(bcx - tw * 0.5f, minTx, maxTx);

    float ty = 0.0f;
    const bool selValid = (selRect.right > selRect.left && selRect.bottom > selRect.top);
    const bool hasSecondary = (secondaryRect.right > secondaryRect.left);

    // 智能避让：如果主工具栏下方紧贴着二级工具栏，主工具栏按钮优先向上弹出，杜绝与二级工具栏重叠
    if (hasSecondary && buttonRect.bottom <= secondaryRect.top + 2.0f * scale) {
        ty = buttonRect.top - th - 6.0f * scale;
        if (ty < 4.0f * scale) {
            ty = secondaryRect.bottom + 6.0f * scale;
        }
    } else if (selValid && buttonRect.top >= selRect.bottom - 4.0f * scale && !hasSecondary) {
        ty = buttonRect.bottom + 6.0f * scale;
        if (ty + th > surfaceSize.height - 4.0f * scale) {
            ty = buttonRect.top - th - 6.0f * scale;
        }
    } else {
        ty = buttonRect.top - th - 6.0f * scale;
        if (ty < 4.0f * scale) {
            ty = buttonRect.bottom + 6.0f * scale;
        }
    }

    return D2D1::RectF(tx, ty, tx + tw, ty + th);
}

void transitionToRecordMode(CaptureState& state) {
    if (state.mode == OverlayMode::RecordRegion) return;
    state.mode = OverlayMode::RecordRegion;
    state.markup.clearAll();
    state.activeElement = nullptr;
    state.openSubmenu = SubmenuType::None;
    state.sliderPopup.type = SliderPopupType::None;
    state.toolbarLayoutValid = false;
    if (state.options.showShortcutHints) {
        ShortcutHintOverlay::instance().show(ShortcutHintContext::RecordSelecting);
    }
    const bool zh = tools3000::core::WinUtils::isSystemLanguageChinese();
    state.loupeToastUntil = GetTickCount() + 1500;
    state.loupeToastMessage = zh ? L"已切换至区域录屏模式" : L"Switched to Record Mode";
}

}  // namespace tools3000::capture
