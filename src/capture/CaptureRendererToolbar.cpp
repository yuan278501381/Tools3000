// ─────────────────────────────────────────────────────────────────────────────
// CaptureRendererToolbar.cpp — 截图主/副工具栏、滑块浮窗、套壳预览与下拉菜单渲染子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureRendererInternal.h"
#include "core/i18n/UiI18n.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/MarkupCurveGeometry.h"
#include "capture/CaptureHudRimStyle.h"
#include "capture/CaptureHudPalette.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "capture/CaptureHistory.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/HudAvoidanceEngine.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ScrollCaptureEditorOverlay.h"
#include "capture/MarkupBaseHelper.h"
#include "capture/NumberBadgeFormatter.h"
#include "ui/ToastStyle.h"
#include <d2d1_1.h>
#include <format>
#include <algorithm>
#include <cmath>
#include <utility>
#include <opencv2/imgproc.hpp>
#include <dxgi.h>

using namespace Microsoft::WRL;

namespace tools3000::capture {

using namespace renderer_detail;

void CaptureRenderer::drawToolbar(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    rebuildCaptureToolbar(state, selectionRect, m_renderTarget->GetSize());
    if (state.toolbarButtons.empty()) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool isActiveDragInteraction =
        state.isMarking || state.isAdjustingSelection || state.isAdjustingCornerRadius || state.isManipulating;
    bool ghostFadeToolbar = false;
    if (isActiveDragInteraction) {
        const float pad = 28.0f * scale;
        const float cx = static_cast<float>(state.currentCursor.x);
        const float cy = static_cast<float>(state.currentCursor.y);
        D2D1_RECT_F activeBox = D2D1::RectF(cx - pad, cy - pad, cx + pad, cy + pad);
        if (state.isMarking) {
            const float mx1 = (std::min)(static_cast<float>(state.markupStart.x), cx) - pad;
            const float my1 = (std::min)(static_cast<float>(state.markupStart.y), cy) - pad;
            const float mx2 = (std::max)(static_cast<float>(state.markupStart.x), cx) + pad;
            const float my2 = (std::max)(static_cast<float>(state.markupStart.y), cy) + pad;
            activeBox = D2D1::RectF(mx1, my1, mx2, my2);
        }
        if (HudAvoidanceEngine::rectsIntersect(activeBox, state.primaryToolbarRect) ||
            (!state.secondaryToolbarButtons.empty() && HudAvoidanceEngine::rectsIntersect(activeBox, state.secondaryToolbarRect)) ||
            (!state.selectionSideButtons.empty() && HudAvoidanceEngine::rectsIntersect(activeBox, state.selectionSideRect))) {
            ghostFadeToolbar = true;
        }
    }

    ComPtr<ID2D1Layer> ghostLayer;
    bool pushedGhostLayer = false;
    if (ghostFadeToolbar && SUCCEEDED(m_renderTarget->CreateLayer(nullptr, ghostLayer.GetAddressOf())) && ghostLayer) {
        m_renderTarget->PushLayer(
            D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                  D2D1::IdentityMatrix(), 0.18f),
            ghostLayer.Get());
        pushedGhostLayer = true;
    }

    const auto pal = FloatingGlassBar::resolveThemePalette();
    const bool isDark = pal.isDark;

    // 1. 绘制主工具栏浮岛卡片 (12.5px 同心光学双环圆角，配内部 7.5px 按钮严守 Concentric Radius)
    drawGlassPanel(state.primaryToolbarRect, 12.5f * scale, false);

    // 1b. 绘制主工具栏左侧 2x3 六点拖拽手柄 (PixPin 风格自由拖拽握把)
    if (state.toolbarGripRect.right > state.toolbarGripRect.left &&
        state.toolbarGripRect.bottom > state.toolbarGripRect.top) {
        const bool gripHovered = state.isDraggingToolbar ||
            (state.currentCursor.x >= state.toolbarGripRect.left &&
             state.currentCursor.x <= state.toolbarGripRect.right &&
             state.currentCursor.y >= state.toolbarGripRect.top &&
             state.currentCursor.y <= state.toolbarGripRect.bottom);
        ComPtr<ID2D1SolidColorBrush> gripDotBrush;
        const D2D1_COLOR_F gripColor = isDark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, gripHovered ? 0.72f : 0.30f)
            : (gripHovered ? CaptureHudPalette::kGripDotHover : CaptureHudPalette::kGripDotIdle);
        m_renderTarget->CreateSolidColorBrush(gripColor, gripDotBrush.GetAddressOf());
        if (gripDotBrush) {
            const float gcx = (state.toolbarGripRect.left + state.toolbarGripRect.right) * 0.5f;
            const float gcy = (state.toolbarGripRect.top + state.toolbarGripRect.bottom) * 0.5f;
            const float colOffset = 2.2f * scale;
            const float rowStep = 5.0f * scale;
            const float dotR = 1.35f * scale;
            for (int rIdx = -1; rIdx <= 1; ++rIdx) {
                const float dy = gcy + static_cast<float>(rIdx) * rowStep;
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(gcx - colOffset, dy), dotR, dotR), gripDotBrush.Get());
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(gcx + colOffset, dy), dotR, dotR), gripDotBrush.Get());
            }
        }
    }

    // 2. 如果存在二级属性栏，绘制二级属性栏浮岛卡片 (11.5px 同心圆角)
    if (!state.secondaryToolbarButtons.empty()) {
        drawGlassPanel(state.secondaryToolbarRect, 11.5f * scale, false);
    }

    ComPtr<ID2D1SolidColorBrush> buttonBrush;
    ComPtr<ID2D1SolidColorBrush> activeBrush;
    ComPtr<ID2D1SolidColorBrush> activeBorderBrush;
    ComPtr<ID2D1SolidColorBrush> dangerHoverBrush;
    ComPtr<ID2D1SolidColorBrush> confirmBrush;
    ComPtr<ID2D1SolidColorBrush> confirmHoverBrush;
    ComPtr<ID2D1SolidColorBrush> confirmHighlightBrush;
    ComPtr<ID2D1SolidColorBrush> hoverBrush;
    ComPtr<ID2D1SolidColorBrush> iconBrush;
    ComPtr<ID2D1SolidColorBrush> disabledIconBrush;
    ComPtr<ID2D1SolidColorBrush> activeIconBrush;
    ComPtr<ID2D1SolidColorBrush> confirmIconBrush;
    ComPtr<ID2D1SolidColorBrush> dangerIconBrush;
    ComPtr<ID2D1SolidColorBrush> tipTextBrush;
    ComPtr<ID2D1SolidColorBrush> secondaryActiveBrush;
    ComPtr<ID2D1SolidColorBrush> separatorBrush;

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), buttonBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.hoverBg, hoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, activeBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, secondaryActiveBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentBorder, activeBorderBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentText, activeIconBrush.GetAddressOf());

    // 完成按钮：立体翡翠绿胶囊与内高光
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.45f, 0.96f), confirmBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.64f, 0.39f, 1.0f), confirmHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), confirmHighlightBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), confirmIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(pal.dangerSoftBg, dangerHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.dangerText, dangerIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(pal.primaryText, iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)
               : D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.28f),
        disabledIconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, tipTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.divider, separatorBrush.GetAddressOf());

    auto drawButtonGroup = [&](const std::vector<ToolbarButton>& btnList) {
        // 先为连续的智能文字批注模式按钮绘制高定内凹微晶分段轨道底槽 (Apple/Figma Segmented Control Track)
        {
            float segL = 1e9f, segT = 1e9f, segR = -1e9f, segB = -1e9f;
            int segCount = 0;
            for (const auto& b : btnList) {
                if (b.command == ToolbarCommand::SelectTextMarkupMode) {
                    segL = (std::min)(segL, b.rect.left);
                    segT = (std::min)(segT, b.rect.top);
                    segR = (std::max)(segR, b.rect.right);
                    segB = (std::max)(segB, b.rect.bottom);
                    ++segCount;
                }
            }
            if (segCount >= 2) {
                const float padX = 2.2f * scale;
                const float padY = 1.6f * scale;
                const D2D1_RECT_F trackRc = D2D1::RectF(segL - padX, segT - padY, segR + padX, segB + padY);
                const float trackR = 7.6f * scale;
                ComPtr<ID2D1SolidColorBrush> trackBg, trackRim;
                m_renderTarget->CreateSolidColorBrush(
                    isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f)
                           : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.050f),
                    trackBg.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(
                    isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.085f)
                           : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.075f),
                    trackRim.GetAddressOf());
                if (trackBg) {
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trackRc, trackR, trackR), trackBg.Get());
                }
                if (trackRim) {
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trackRc, trackR, trackR), trackRim.Get(), 0.9f * scale);
                }
            }
        }

        for (size_t idx = 0; idx < btnList.size(); ++idx) {
            const auto& button = btnList[idx];
            // 绘制前置微刻痕细腻分隔线 (1px 双层微刻痕，严格居中于相邻两枚按钮间距中点)
            if (button.isSeparatorBefore) {
                float sepX = (idx > 0)
                    ? (btnList[idx - 1].rect.right + button.rect.left) * 0.5f
                    : (button.rect.left - 6.0f * scale);
                float sepTop = button.rect.top + 5.0f * scale;
                float sepBottom = button.rect.bottom - 5.0f * scale;
                FloatingGlassBar::drawSeparator(m_renderTarget.Get(), sepX, sepTop, sepBottom, 1.0f, isDark);
            }

            bool isTool = button.command == ToolbarCommand::SelectTool;
            bool isActiveTool = false;
            if (isTool) {
                if (state.isMarkupToolActive || button.isSecondary) {
                    if (button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse ||
                        button.tool == MarkupTool::Line || button.tool == MarkupTool::Spotlight) {
                        isActiveTool = (state.currentTool == MarkupTool::Rectangle ||
                                        state.currentTool == MarkupTool::Ellipse ||
                                        state.currentTool == MarkupTool::Line ||
                                        state.currentTool == MarkupTool::Spotlight);
                    } else if (button.tool == MarkupTool::Pen || button.tool == MarkupTool::Highlight) {
                        isActiveTool = (state.currentTool == MarkupTool::Pen || state.currentTool == MarkupTool::Highlight);
                    } else if (button.tool == MarkupTool::Mosaic || button.tool == MarkupTool::Blur) {
                        isActiveTool = (state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Blur);
                    } else {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                    if (button.isSecondary) {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                }
            }
            if (button.command == ToolbarCommand::ToggleFill) {
                if (button.tool == MarkupTool::Magnifier) {
                    isActiveTool = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                        ? state.activeElement->fill
                        : state.magnifierShowBadge;
                } else if (button.tool == MarkupTool::Spotlight) {
                    isActiveTool = (state.activeElement && state.activeElement->tool == MarkupTool::Spotlight)
                        ? state.activeElement->spotlightShowBorder
                        : state.currentSpotlightBorder;
                } else {
                    isActiveTool = state.currentFillMode;
                }
            }
            if (button.command == ToolbarCommand::SelectMosaicType) {
                const int effectiveMosaicType = (state.currentTool == MarkupTool::Blur) ? 1 : state.currentMosaicType;
                isActiveTool = (button.intParam == effectiveMosaicType);
            }
            if (button.command == ToolbarCommand::SelectTextMarkupMode) {
                isActiveTool = (button.intParam == static_cast<int>(state.currentTextMarkupMode));
            }
            if (button.command == ToolbarCommand::ToggleBeautyShell) {
                isActiveTool = state.beautyShell.enabled;
            }
            if (button.command == ToolbarCommand::SideCycleAspectRatio) {
                isActiveTool = (state.aspectRatio != AspectRatioPreset::Free);
            }
            if (button.command == ToolbarCommand::RecordToggleKeycast ||
                button.command == ToolbarCommand::RecordToggleSystemAudio ||
                button.command == ToolbarCommand::RecordToggleMicrophone) {
                isActiveTool = button.boolParam;
            }
            if (button.command == ToolbarCommand::RecordCycleClickEffect) {
                isActiveTool = (state.recordSetup.showClickEffects || state.recordSetup.zoomEffectMode != 0);
            }

            if (button.command == ToolbarCommand::ToggleTextOutline) {
                isActiveTool = (button.tool != MarkupTool::Text)
                    ? (state.isEditingOutlineColor || state.sliderPopup.type == SliderPopupType::OutlineWidth)
                    : (state.currentTextOutline || state.isEditingOutlineColor);
            }
            if (button.command == ToolbarCommand::ToggleCurveMode) {
                if (button.intParam == 1) {
                    isActiveTool = !state.currentSmoothCurve;
                } else if (button.intParam == 2) {
                    isActiveTool = state.currentSmoothCurve;
                } else {
                    isActiveTool = state.currentSmoothCurve;
                }
            }
            if (button.command == ToolbarCommand::ToggleLineArrow) {
                isActiveTool = state.currentLineArrowEnabled;
            }
            if (button.command == ToolbarCommand::CycleElementCornerRadius) {
                isActiveTool = (button.intParam > 0 || state.sliderPopup.type == SliderPopupType::CornerRadius);
            }
            if (button.command == ToolbarCommand::ToggleCornerRadius) {
                isActiveTool = (state.effectiveCornerRadius() > 0.5f ||
                                state.sliderPopup.type == SliderPopupType::SelectionCornerRadius);
            }
            if (button.command == ToolbarCommand::CycleStrokeWidth) {
                isActiveTool = (state.sliderPopup.type == SliderPopupType::StrokeWidth ||
                                state.sliderPopup.type == SliderPopupType::TextFontSize ||
                                state.sliderPopup.type == SliderPopupType::MosaicBlockSize);
            }
            if (button.command == ToolbarCommand::CycleSpotlightDim) {
                isActiveTool = (state.sliderPopup.type == SliderPopupType::SpotlightDim);
            }
            if (button.command == ToolbarCommand::ToggleLineStyleDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::LineStyle);
            }
            if (button.command == ToolbarCommand::ToggleArrowStyleDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::ArrowStyle);
            }
            if (button.command == ToolbarCommand::ToggleStartCapDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::StartCapStyle);
            }
            if (button.command == ToolbarCommand::ToggleEndCapDropdown) {
                isActiveTool = (state.openSubmenu == SubmenuType::EndCapStyle);
            }
            if (button.command == ToolbarCommand::ToggleNumberFormatDropdown) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::NumberFormat);
            }
            if (button.command == ToolbarCommand::ToggleMagnifierCalloutStyleDropdown) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::MagnifierCalloutStyle);
            }
            if (button.command == ToolbarCommand::CycleBeautyBg) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellBg);
            }
            if (button.command == ToolbarCommand::CycleBeautyPadding) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellPadding);
            }
            if (button.command == ToolbarCommand::CycleBeautyRadius) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::BeautyShellRadius);
            }
            if (button.command == ToolbarCommand::RecordSelectMicSource) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::RecordMicSource &&
                                state.recordSetup.format != RecordFormat::GIF);
            }
            if (button.command == ToolbarCommand::RecordToggleSettingsMenu) {
                isActiveTool = (state.dropdownMenu.type == DropdownType::RecordMoreSettings);
            }

            bool isDisabled = false;
            if (button.command == ToolbarCommand::Undo) isDisabled = !state.markup.canUndo();
            else if (button.command == ToolbarCommand::Redo) isDisabled = !state.markup.canRedo();
            else if (button.command == ToolbarCommand::Clear) isDisabled = (state.markup.elementCount() == 0);
            else if ((button.command == ToolbarCommand::RecordToggleSystemAudio ||
                      button.command == ToolbarCommand::RecordToggleMicrophone ||
                      button.command == ToolbarCommand::RecordSelectMicSource) &&
                     state.recordSetup.format == RecordFormat::GIF) {
                isDisabled = true;
            }

            const bool isSplitMicLeft = (button.command == ToolbarCommand::RecordToggleMicrophone &&
                                         state.mode == OverlayMode::RecordRegion);
            const bool isSplitMicRight = (button.command == ToolbarCommand::RecordSelectMicSource);
            const bool isSplitMicHalf = (isSplitMicLeft || isSplitMicRight);

            bool isColor = button.command == ToolbarCommand::SelectColor;
            bool isOutlineColor = button.command == ToolbarCommand::SelectTextOutlineColor;
            bool isDanger = button.command == ToolbarCommand::Cancel;
            bool isConfirm = button.command == ToolbarCommand::Confirm ||
                             button.command == ToolbarCommand::RecordStartConfirm;
            bool isCheckboxItem = (button.command == ToolbarCommand::ToggleLineArrow ||
                                   (button.command == ToolbarCommand::ToggleFill && !button.label.empty()));
            bool isStepper = (isSplitMicHalf ||
                              button.command == ToolbarCommand::CycleStrokeWidth ||
                              button.command == ToolbarCommand::CycleMagnifierScale ||
                              button.command == ToolbarCommand::CycleElementCornerRadius ||
                              button.command == ToolbarCommand::ToggleCornerRadius ||
                              button.command == ToolbarCommand::CycleSpotlightDim ||
                              button.command == ToolbarCommand::ToggleNumberFormatDropdown ||
                              button.command == ToolbarCommand::ToggleMagnifierCalloutStyleDropdown ||
                              button.command == ToolbarCommand::CycleBeautyBg ||
                              button.command == ToolbarCommand::CycleBeautyPadding ||
                              button.command == ToolbarCommand::CycleBeautyRadius ||
                              button.command == ToolbarCommand::RecordToggleFormat ||
                              button.command == ToolbarCommand::RecordCycleFps ||
                              button.command == ToolbarCommand::RecordCycleQuality ||
                              button.command == ToolbarCommand::RecordCycleClickEffect ||
                              (button.command == ToolbarCommand::ToggleTextOutline && !button.label.empty()));
            bool isHovered = !isDisabled &&
                             state.currentCursor.x >= button.rect.left && state.currentCursor.x <= button.rect.right &&
                             state.currentCursor.y >= button.rect.top  && state.currentCursor.y <= button.rect.bottom;

            const float btnRadius = (button.isSecondary ? 6.5f : 7.5f) * scale;
            auto rounded = D2D1::RoundedRect(button.rect, btnRadius, btnRadius);

            if (isColor) {
                drawPresetColorSwatch(m_renderTarget.Get(), button.rect, button.color, scale, isHovered, hoverBrush.Get());
            } else if (isOutlineColor) {
                // 文字描边色板：采用统一尺寸空心圆环 (Hollow Ring Swatch) 视觉形态，与右侧实心字色圆点形成直觉级区分
                float cx = (button.rect.left + button.rect.right) * 0.5f;
                float cy = (button.rect.top + button.rect.bottom) * 0.5f;
                float r = std::min({button.rect.right - button.rect.left, button.rect.bottom - button.rect.top, 20.0f * scale}) * 0.36f;
                float ringStroke = std::max(2.2f * scale, r * 0.46f);
                float midR = r - ringStroke * 0.35f;
                auto ringEllipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), midR, midR);
                auto outerBorder = D2D1::Ellipse(D2D1::Point2F(cx, cy), midR + ringStroke * 0.5f, midR + ringStroke * 0.5f);
                auto innerBorder = D2D1::Ellipse(D2D1::Point2F(cx, cy), std::max(1.0f, midR - ringStroke * 0.5f), std::max(1.0f, midR - ringStroke * 0.5f));

                bool isActive = button.color.isAuto()
                    ? state.currentTextOutlineColor.isAuto()
                    : (!state.currentTextOutlineColor.isAuto() && button.color == state.currentTextOutlineColor);

                if (button.color.isAuto()) {
                    auto leftClip = D2D1::RectF(cx - r - 2.0f * scale, cy - r - 2.0f * scale, cx, cy + r + 2.0f * scale);
                    auto rightClip = D2D1::RectF(cx, cy - r - 2.0f * scale, cx + r + 2.0f * scale, cy + r + 2.0f * scale);

                    ComPtr<ID2D1SolidColorBrush> blackBrush, whiteBrush;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 1.0f), blackBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.98f, 1.0f, 1.0f), whiteBrush.GetAddressOf());

                    if (blackBrush && whiteBrush) {
                        m_renderTarget->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->DrawEllipse(ringEllipse, blackBrush.Get(), ringStroke);
                        m_renderTarget->PopAxisAlignedClip();

                        m_renderTarget->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->DrawEllipse(ringEllipse, whiteBrush.Get(), ringStroke);
                        m_renderTarget->PopAxisAlignedClip();
                    }
                    if (separatorBrush) {
                        m_renderTarget->DrawEllipse(outerBorder, separatorBrush.Get(), 0.9f * scale);
                        m_renderTarget->DrawEllipse(innerBorder, separatorBrush.Get(), 0.9f * scale);
                    }
                } else {
                    ComPtr<ID2D1SolidColorBrush> swatch;
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f,
                                     button.color.b / 255.0f, 1.0f),
                        swatch.GetAddressOf());
                    if (swatch) {
                        m_renderTarget->DrawEllipse(ringEllipse, swatch.Get(), ringStroke);
                    }
                    if (separatorBrush) {
                        m_renderTarget->DrawEllipse(outerBorder, separatorBrush.Get(), 0.9f * scale);
                        m_renderTarget->DrawEllipse(innerBorder, separatorBrush.Get(), 0.9f * scale);
                    }
                }

                if (isActive) {
                    auto activeOuter = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.4f * scale, r + 2.4f * scale);
                    if (activeIconBrush) m_renderTarget->DrawEllipse(activeOuter, activeIconBrush.Get(), 1.6f * scale);
                } else if (isHovered && hoverBrush) {
                    auto hoverOuter = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.0f * scale, r + 2.0f * scale);
                    m_renderTarget->DrawEllipse(hoverOuter, hoverBrush.Get(), 1.4f * scale);
                }
            } else {
                ComPtr<ID2D1SolidColorBrush> stepperBg, stepperRim;
                if (isStepper) {
                    m_renderTarget->CreateSolidColorBrush(
                        isDark
                            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.125f : 0.062f)
                            : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, isHovered ? 0.075f : 0.044f),
                        stepperBg.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(
                        isDark
                            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, isHovered ? 0.22f : 0.105f)
                            : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, isHovered ? 0.16f : 0.09f),
                        stepperRim.GetAddressOf());
                }

                if (isConfirm) {
                    const bool isRecordStart = (button.command == ToolbarCommand::RecordStartConfirm);
                    // 录制启动采用温润珊瑚绯红微晶胶囊，截图确认保持翡翠绿微晶胶囊
                    D2D1_GRADIENT_STOP cStops[2] = {
                        {0.0f, isRecordStart
                            ? (isHovered ? D2D1::ColorF(251.0f / 255.0f, 75.0f / 255.0f, 102.0f / 255.0f, 1.0f)
                                         : D2D1::ColorF(244.0f / 255.0f, 63.0f / 255.0f, 94.0f / 255.0f, 0.96f))
                            : (isHovered ? D2D1::ColorF(0.08f, 0.76f, 0.47f, 1.0f)
                                         : D2D1::ColorF(0.10f, 0.80f, 0.50f, 0.98f))},
                        {1.0f, isRecordStart
                            ? (isHovered ? D2D1::ColorF(216.0f / 255.0f, 24.0f / 255.0f, 66.0f / 255.0f, 1.0f)
                                         : D2D1::ColorF(225.0f / 255.0f, 29.0f / 255.0f, 72.0f / 255.0f, 0.96f))
                            : (isHovered ? D2D1::ColorF(0.03f, 0.58f, 0.35f, 1.0f)
                                         : D2D1::ColorF(0.04f, 0.64f, 0.39f, 0.98f))}
                    };
                    ComPtr<ID2D1GradientStopCollection> cStopCol;
                    ComPtr<ID2D1LinearGradientBrush> cGrad;
                    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(cStops, 2, cStopCol.GetAddressOf())) &&
                        SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(
                                D2D1::Point2F(button.rect.left, button.rect.top),
                                D2D1::Point2F(button.rect.left, button.rect.bottom)),
                            cStopCol.Get(), cGrad.GetAddressOf())) && cGrad) {
                        m_renderTarget->FillRoundedRectangle(rounded, cGrad.Get());
                    } else if (confirmBrush) {
                        m_renderTarget->FillRoundedRectangle(rounded, isHovered ? confirmHoverBrush.Get() : confirmBrush.Get());
                    }
                    if (confirmHighlightBrush) {
                        const float innerRad = (std::max)(1.0f, btnRadius - 0.6f * scale);
                        auto innerR = D2D1::RoundedRect(
                            D2D1::RectF(button.rect.left + 0.6f * scale, button.rect.top + 0.6f * scale,
                                        button.rect.right - 0.6f * scale, button.rect.bottom - 0.6f * scale),
                            innerRad, innerRad);
                        m_renderTarget->DrawRoundedRectangle(innerR, confirmHighlightBrush.Get(), 0.9f * scale);
                    }
                } else if (isActiveTool && !isCheckboxItem) {
                    if (button.command == ToolbarCommand::SelectTextMarkupMode) {
                        // 高定立体浮起分段滑块卡片 (Elevated Segmented Control Pill) + 底部当前批注色微光指示条
                        ComPtr<ID2D1SolidColorBrush> segShadow, segFill, segRim, segGlowBar;
                        m_renderTarget->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.34f)
                                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.10f),
                            segShadow.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(45.0f / 255.0f, 52.0f / 255.0f, 68.0f / 255.0f, 0.96f)
                                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f),
                            segFill.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)
                                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.13f),
                            segRim.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(state.currentColor.r / 255.0f, state.currentColor.g / 255.0f,
                                         state.currentColor.b / 255.0f, 0.92f),
                            segGlowBar.GetAddressOf());
                        if (segShadow) {
                            auto shRc = D2D1::RectF(button.rect.left, button.rect.top + 1.0f * scale,
                                                    button.rect.right, button.rect.bottom + 1.2f * scale);
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRc, btnRadius, btnRadius), segShadow.Get());
                        }
                        if (segFill) {
                            m_renderTarget->FillRoundedRectangle(rounded, segFill.Get());
                        }
                        if (segRim) {
                            m_renderTarget->DrawRoundedRectangle(rounded, segRim.Get(), 0.95f * scale);
                        }
                        if (segGlowBar) {
                            const float bcx = (button.rect.left + button.rect.right) * 0.5f;
                            const float pillHalfW = 5.4f * scale;
                            const float pillH = 2.0f * scale;
                            auto indPill = D2D1::RoundedRect(
                                D2D1::RectF(bcx - pillHalfW, button.rect.bottom - 3.0f * scale,
                                            bcx + pillHalfW, button.rect.bottom - 3.0f * scale + pillH),
                                1.0f * scale, 1.0f * scale);
                            m_renderTarget->FillRoundedRectangle(indPill, segGlowBar.Get());
                        }
                    } else {
                        // 深浅双态高定立体微浮键帽（Elevated Studio Keycap）+ 品牌主题柔光边 + 底部光学微灯丝
                        const auto themeRgb = FloatingGlassBar::effectiveAccentRGB();
                        D2D1_GRADIENT_STOP keyStops[2] = {
                            {0.0f, isDark
                                ? D2D1::ColorF(
                                    (std::min)(1.0f, themeRgb.r * 0.36f + 0.13f),
                                    (std::min)(1.0f, themeRgb.g * 0.36f + 0.14f),
                                    (std::min)(1.0f, themeRgb.b * 0.36f + 0.18f), 0.48f)
                                : D2D1::ColorF(
                                    (std::min)(1.0f, themeRgb.r * 0.14f + 0.86f),
                                    (std::min)(1.0f, themeRgb.g * 0.14f + 0.86f),
                                    (std::min)(1.0f, themeRgb.b * 0.14f + 0.86f), 0.96f)},
                            {1.0f, isDark
                                ? D2D1::ColorF(
                                    (std::min)(1.0f, themeRgb.r * 0.28f + 0.05f),
                                    (std::min)(1.0f, themeRgb.g * 0.28f + 0.06f),
                                    (std::min)(1.0f, themeRgb.b * 0.28f + 0.09f), 0.34f)
                                : D2D1::ColorF(
                                    (std::min)(1.0f, themeRgb.r * 0.22f + 0.78f),
                                    (std::min)(1.0f, themeRgb.g * 0.22f + 0.78f),
                                    (std::min)(1.0f, themeRgb.b * 0.22f + 0.78f), 0.94f)}
                        };
                        ComPtr<ID2D1GradientStopCollection> kCol;
                        ComPtr<ID2D1LinearGradientBrush> kGrad;
                        ID2D1Brush* keyFill = button.isSecondary ? static_cast<ID2D1Brush*>(secondaryActiveBrush.Get())
                                                                 : static_cast<ID2D1Brush*>(activeBrush.Get());
                        if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(keyStops, 2, kCol.GetAddressOf())) &&
                            SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                                D2D1::LinearGradientBrushProperties(
                                    D2D1::Point2F(button.rect.left, button.rect.top),
                                    D2D1::Point2F(button.rect.left, button.rect.bottom)),
                                kCol.Get(), kGrad.GetAddressOf())) && kGrad) {
                            keyFill = kGrad.Get();
                        }
                        if (keyFill) {
                            if (isSplitMicHalf) {
                                const D2D1_RECT_F unionRc = isSplitMicLeft
                                    ? D2D1::RectF(button.rect.left, button.rect.top, button.rect.right + btnRadius + 2.0f * scale, button.rect.bottom)
                                    : D2D1::RectF(button.rect.left - btnRadius - 2.0f * scale, button.rect.top, button.rect.right, button.rect.bottom);
                                m_renderTarget->PushAxisAlignedClip(button.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(unionRc, btnRadius, btnRadius), keyFill);
                                if (activeBorderBrush) {
                                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(unionRc, btnRadius, btnRadius), activeBorderBrush.Get(), 0.9f * scale);
                                }
                                m_renderTarget->PopAxisAlignedClip();
                            } else {
                                m_renderTarget->FillRoundedRectangle(rounded, keyFill);
                                if (activeBorderBrush) {
                                    m_renderTarget->DrawRoundedRectangle(rounded, activeBorderBrush.Get(), 0.9f * scale);
                                }
                            }
                        }
                        if (!button.isSecondary && button.label.empty() && !isSplitMicRight && activeIconBrush) {
                            const float bcx = (button.rect.left + button.rect.right) * 0.5f;
                            const float pillHalfW = 5.2f * scale;
                            const float pillH = 2.0f * scale;
                            const float indY = button.rect.bottom - 3.0f * scale;
                            ComPtr<ID2D1SolidColorBrush> indGlow;
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.34f), indGlow.GetAddressOf());
                            if (indGlow) {
                                m_renderTarget->FillRoundedRectangle(
                                    D2D1::RoundedRect(
                                        D2D1::RectF(bcx - pillHalfW - 1.5f * scale, indY - 1.0f * scale,
                                                    bcx + pillHalfW + 1.5f * scale, indY + pillH + 1.0f * scale),
                                        2.0f * scale, 2.0f * scale),
                                    indGlow.Get());
                            }
                            auto indPill = D2D1::RoundedRect(
                                D2D1::RectF(bcx - pillHalfW, indY, bcx + pillHalfW, indY + pillH),
                                1.0f * scale, 1.0f * scale);
                            m_renderTarget->FillRoundedRectangle(indPill, activeIconBrush.Get());
                        }
                    }
                } else {
                    auto* fillBrush = isStepper ? stepperBg.Get()
                                    : (isDanger && isHovered) ? dangerHoverBrush.Get()
                                    : isHovered ? hoverBrush.Get()
                                    : buttonBrush.Get();
                    if (fillBrush) {
                        if (isSplitMicHalf) {
                            const D2D1_RECT_F unionRc = isSplitMicLeft
                                ? D2D1::RectF(button.rect.left, button.rect.top, button.rect.right + btnRadius + 2.0f * scale, button.rect.bottom)
                                : D2D1::RectF(button.rect.left - btnRadius - 2.0f * scale, button.rect.top, button.rect.right, button.rect.bottom);
                            m_renderTarget->PushAxisAlignedClip(button.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(unionRc, btnRadius, btnRadius), fillBrush);
                            if (stepperRim) {
                                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(unionRc, btnRadius, btnRadius), stepperRim.Get(), 0.85f * scale);
                            }
                            m_renderTarget->PopAxisAlignedClip();
                        } else {
                            m_renderTarget->FillRoundedRectangle(rounded, fillBrush);
                            if (isStepper && stepperRim) {
                                m_renderTarget->DrawRoundedRectangle(rounded, stepperRim.Get(), 0.85f * scale);
                            } else if (isHovered && !isDanger && separatorBrush) {
                                m_renderTarget->DrawRoundedRectangle(rounded, separatorBrush.Get(), 0.8f * scale);
                            }
                        }
                    }
                }

                // 一体化分体双舱胶囊中间微晶分割丝线（精准像素对齐，上下优雅留白）
                if (isSplitMicRight && separatorBrush) {
                    const float divX = std::floor(button.rect.left) + 0.5f;
                    m_renderTarget->DrawLine(
                        D2D1::Point2F(divX, button.rect.top + 6.0f * scale),
                        D2D1::Point2F(divX, button.rect.bottom - 6.0f * scale),
                        separatorBrush.Get(),
                        1.0f * scale);
                }

                auto* currentIconBrush = isDisabled ? disabledIconBrush.Get()
                                       : isConfirm ? confirmIconBrush.Get()
                                       : (isActiveTool && !isCheckboxItem) ? activeIconBrush.Get()
                                       : (isDanger && isHovered) ? dangerIconBrush.Get()
                                       : iconBrush.Get();
                ToolbarButton renderBtn = button;
                if (renderBtn.command == ToolbarCommand::ChooseCustomColor) {
                    if (renderBtn.tool == MarkupTool::Magnifier) {
                        renderBtn.color = (state.activeElement && state.activeElement->tool == MarkupTool::Magnifier)
                            ? state.activeElement->color
                            : state.currentMagnifierColor;
                    } else if (state.isEditingOutlineColor) {
                        if (state.activeElement && state.activeElement->tool == renderBtn.tool && !state.activeElement->textOutlineColor.isAuto()) {
                            renderBtn.color = state.activeElement->textOutlineColor;
                        } else if (!state.currentTextOutlineColor.isAuto()) {
                            renderBtn.color = state.currentTextOutlineColor;
                        } else {
                            renderBtn.color = MarkupColor::White();
                        }
                    } else {
                        renderBtn.color = (state.activeElement && state.activeElement->tool == renderBtn.tool && !state.activeElement->color.isAuto())
                            ? state.activeElement->color
                            : state.currentColor;
                    }
                }
                drawVectorButtonIcon(renderBtn, renderBtn.rect, currentIconBrush, scale);
            }

            // 如果带有下拉指示器：主工具栏正方形图标按钮采用右下角微斜切角标；二级胶囊采用右侧 V 型折角箭头
            if (button.hasDropdown) {
                auto* chevBrush = isActiveTool ? activeIconBrush.Get() : iconBrush.Get();
                if (chevBrush) {
                    if (!button.isSecondary && button.label.empty()) {
                        const float cx2 = button.rect.right - 3.2f * scale;
                        const float cy2 = button.rect.bottom - 3.2f * scale;
                        ComPtr<ID2D1PathGeometry> cornerTri;
                        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(cornerTri.GetAddressOf())) && cornerTri) {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(cornerTri->Open(sink.GetAddressOf())) && sink) {
                                sink->BeginFigure(D2D1::Point2F(cx2, cy2 - 3.6f * scale), D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddLine(D2D1::Point2F(cx2, cy2));
                                sink->AddLine(D2D1::Point2F(cx2 - 3.6f * scale, cy2));
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                m_renderTarget->FillGeometry(cornerTri.Get(), chevBrush);
                            }
                        }
                    } else {
                        float tx = button.rect.right - 5.2f * scale;
                        float ty = (button.rect.top + button.rect.bottom) * 0.5f;
                        m_renderTarget->DrawLine(D2D1::Point2F(tx - 2.0f * scale, ty - 0.8f * scale),
                                                 D2D1::Point2F(tx, ty + 1.2f * scale), chevBrush, 1.25f * scale);
                        m_renderTarget->DrawLine(D2D1::Point2F(tx, ty + 1.2f * scale),
                                                 D2D1::Point2F(tx + 2.0f * scale, ty - 0.8f * scale), chevBrush, 1.25f * scale);
                    }
                }
            }
        }
    };

    // 绘制主工具栏、二级属性栏与选区侧边独立悬浮正圆微晶圆钮 (PixPin 标杆：无笨重白底板，曜石黑圆底 + 天际蓝高亮激活态)
    drawButtonGroup(state.toolbarButtons);
    if (!state.secondaryToolbarButtons.empty()) {
        drawButtonGroup(state.secondaryToolbarButtons);
    }
    if (!state.selectionSideButtons.empty()) {
        ComPtr<ID2D1SolidColorBrush> sideShadowBrush, sideIdleBg, sideHoverBg, sideActiveBg, sideActiveHoverBg, sideRimBrush, sideIconBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.24f), sideShadowBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.72f), sideIdleBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.88f), sideHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accent, sideActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(pal.accent.r * 0.88f, pal.accent.g * 0.88f, pal.accent.b * 0.88f, 0.99f),
            sideActiveHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), sideRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), sideIconBrush.GetAddressOf());

        for (const auto& sbtn : state.selectionSideButtons) {
            const float cxBtn = (sbtn.rect.left + sbtn.rect.right) * 0.5f;
            const float cyBtn = (sbtn.rect.top + sbtn.rect.bottom) * 0.5f;
            const float radBtn = (sbtn.rect.right - sbtn.rect.left) * 0.5f;
            const float dxCur = static_cast<float>(state.currentCursor.x) - cxBtn;
            const float dyCur = static_cast<float>(state.currentCursor.y) - cyBtn;
            const bool isHovered = (dxCur * dxCur + dyCur * dyCur <= (radBtn + 1.0f * scale) * (radBtn + 1.0f * scale));

            bool isActive = false;
            if (sbtn.command == ToolbarCommand::SideToggleCornerRadius) {
                isActive = (state.effectiveCornerRadius() > 0.5f ||
                            state.sliderPopup.type == SliderPopupType::SelectionCornerRadius);
            } else if (sbtn.command == ToolbarCommand::SideCycleAspectRatio) {
                isActive = (state.aspectRatio != AspectRatioPreset::Free);
            } else if (sbtn.command == ToolbarCommand::SideInvertSelection) {
                isActive = state.hasSavedSelectionBeforeInvert;
            }

            if (sideShadowBrush) {
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(cxBtn, cyBtn + 1.4f * scale), radBtn + 0.8f * scale, radBtn + 0.8f * scale),
                    sideShadowBrush.Get());
            }
            const auto disc = D2D1::Ellipse(D2D1::Point2F(cxBtn, cyBtn), radBtn, radBtn);
            ID2D1SolidColorBrush* fillBr = isActive
                ? (isHovered ? sideActiveHoverBg.Get() : sideActiveBg.Get())
                : (isHovered ? sideHoverBg.Get() : sideIdleBg.Get());
            if (fillBr) {
                m_renderTarget->FillEllipse(disc, fillBr);
            }
            if (sideRimBrush) {
                m_renderTarget->DrawEllipse(disc, sideRimBrush.Get(), 1.0f * scale);
            }
            if (sideIconBrush) {
                drawVectorButtonIcon(sbtn, sbtn.rect, sideIconBrush.Get(), scale);
            }
        }
    }

    // 绘制展开的二级下拉悬浮菜单
    drawSubmenu(state);

    // 绘制无级滑块与预设悬浮弹窗
    drawSliderPopup(state);

    // 绘制多态微晶下拉菜单
    drawDropdownMenu(state);

    // 悬停浮层提示：世界级微晶胶囊 (DirectWrite 精确排版度量 + 严苛 ASCII 快捷键检测 + 跨双栏/侧边栏防遮挡)
    auto showTooltip = [&](const std::vector<ToolbarButton>& btnList, bool isVerticalSideBar = false) -> bool {
        for (const auto& button : btnList) {
            if (state.currentCursor.x < button.rect.left || state.currentCursor.x > button.rect.right ||
                state.currentCursor.y < button.rect.top  || state.currentCursor.y > button.rect.bottom) {
                continue;
            }
            std::wstring tip = tooltipForButton(button, state.toolbarLayoutChinese, &state.options);
            if (tip.empty()) continue;

            std::wstring labelPart = tip;
            std::wstring kbdPart = L"";
            auto openPos = tip.rfind(L'(');
            auto closePos = tip.rfind(L')');
            if (openPos != std::wstring::npos && closePos != std::wstring::npos && closePos > openPos) {
                std::wstring cand = tip.substr(openPos + 1, closePos - openPos - 1);
                bool isAsciiShortcut = !cand.empty();
                for (wchar_t ch : cand) {
                    if (ch > 127 || ch == L'（' || ch == L'）') {
                        isAsciiShortcut = false;
                        break;
                    }
                }
                if (isAsciiShortcut) {
                    labelPart = tip.substr(0, openPos);
                    while (!labelPart.empty() && labelPart.back() == L' ') labelPart.pop_back();
                    kbdPart = cand;
                }
            }

            float labelW = 0.0f;
            float textH = 14.0f * scale;
            if (m_dwriteFactory && m_infoTextFormat) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    labelPart.c_str(), static_cast<UINT32>(labelPart.size()),
                    m_infoTextFormat.Get(), 800.0f * scale, 100.0f * scale, layout.GetAddressOf())) && layout) {
                    DWRITE_TEXT_METRICS m{};
                    if (SUCCEEDED(layout->GetMetrics(&m))) {
                        labelW = m.width;
                        textH = (std::max)(textH, m.height);
                    }
                }
            }
            if (labelW <= 0.0f) {
                labelW = static_cast<float>(labelPart.size()) * 11.5f * scale;
            }

            float kbdW = 0.0f;
            if (!kbdPart.empty()) {
                if (m_dwriteFactory && m_infoTextFormat) {
                    ComPtr<IDWriteTextLayout> kLayout;
                    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        kbdPart.c_str(), static_cast<UINT32>(kbdPart.size()),
                        m_infoTextFormat.Get(), 400.0f * scale, 100.0f * scale, kLayout.GetAddressOf())) && kLayout) {
                        DWRITE_TEXT_METRICS km{};
                        if (SUCCEEDED(kLayout->GetMetrics(&km))) {
                            kbdW = km.width + 10.0f * scale;
                        }
                    }
                }
                if (kbdW <= 0.0f) {
                    kbdW = (static_cast<float>(kbdPart.size()) * 7.5f + 10.0f) * scale;
                }
            }

            const float padX = 10.0f * scale;
            const float kbdGap = kbdPart.empty() ? 0.0f : 8.0f * scale;
            float tw = padX * 2.0f + labelW + kbdGap + kbdW;
            float th = 26.0f * scale;

            auto sz = m_renderTarget->GetSize();
            auto tipRect = calculateTooltipRect(button.rect, tw, th, scale, sz,
                                                state.toolbarLayoutSelection,
                                                state.secondaryToolbarRect,
                                                state.primaryToolbarRect,
                                                isVerticalSideBar);

            // 悬停提示气泡（Tooltip HUD）恒定采用高级暗房深色 + 立体暗银微晶键帽，保持操作心流
            const auto tipPal = FloatingGlassBar::resolveHudPalette();
            FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), tipRect, 7.5f * scale, true);

            ComPtr<ID2D1SolidColorBrush> textBrush, kbdBgBrush, kbdBorderBrush, kbdTextBrush;
            m_renderTarget->CreateSolidColorBrush(tipPal.primaryText, textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(44.0f / 255.0f, 48.0f / 255.0f, 60.0f / 255.0f, 0.96f), kbdBgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), kbdBorderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(tipPal.primaryText, kbdTextBrush.GetAddressOf());

            float curLeft = tipRect.left + padX;
            if (m_infoTextFormat && textBrush) {
                TextFormatScope scopeLabel(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(labelPart.c_str(), static_cast<UINT32>(labelPart.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(curLeft, tipRect.top, curLeft + labelW + 2.0f * scale, tipRect.bottom),
                    textBrush.Get());
            }

            if (!kbdPart.empty() && kbdBgBrush && kbdBorderBrush && kbdTextBrush && m_infoTextFormat) {
                float kx = curLeft + labelW + kbdGap;
                float kh = 18.0f * scale;
                float ky = tipRect.top + (th - kh) * 0.5f;
                auto kbdRect = D2D1::RectF(kx, ky, kx + kbdW, ky + kh);
                auto kbdRound = D2D1::RoundedRect(kbdRect, 4.5f * scale, 4.5f * scale);
                // 立体暗房物理键帽底棱（Raycast / Apple 风格微刻键帽）
                ComPtr<ID2D1SolidColorBrush> kbdBevel;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f), kbdBevel.GetAddressOf());
                if (kbdBevel) {
                    auto bevelRc = D2D1::RectF(kbdRect.left, kbdRect.top + 1.0f * scale, kbdRect.right, kbdRect.bottom + 1.2f * scale);
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(bevelRc, 4.5f * scale, 4.5f * scale), kbdBevel.Get());
                }
                m_renderTarget->FillRoundedRectangle(kbdRound, kbdBgBrush.Get());
                m_renderTarget->DrawRoundedRectangle(kbdRound, kbdBorderBrush.Get(), 0.9f * scale);
                TextFormatScope scopeKbd(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(kbdPart.c_str(), static_cast<UINT32>(kbdPart.size()),
                    m_infoTextFormat.Get(),
                    kbdRect,
                    kbdTextBrush.Get());
            }
            return true;
        }
        return false;
    };

    const bool isEditingTextNow = (state.activeElement != nullptr &&
                                   state.activeElement->tool == MarkupTool::Text &&
                                   state.activeElement->isEditing);
    if (!hasAnyCapturePopupOpen(state) && !isActiveDragInteraction &&
        !state.isDraggingToolbar && !isEditingTextNow) {
        if (!showTooltip(state.toolbarButtons, false)) {
            if (!showTooltip(state.secondaryToolbarButtons, false)) {
                showTooltip(state.selectionSideButtons, true);
            }
        }
    }

    if (pushedGhostLayer) {
        m_renderTarget->PopLayer();
    }
}

static D2D1_RECT_F resolveSecondaryPopupRect(const CaptureState& state,
                                             ToolbarCommand triggerCmd,
                                             float popupW,
                                             float popupH,
                                             D2D1_SIZE_F surfaceSize,
                                             float scale) {
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
}

void CaptureRenderer::drawSubmenu(CaptureState& state) {
    if (state.openSubmenu == SubmenuType::None || !m_renderTarget) return;

    float scale = (state.dpiScale > 0.1f) ? state.dpiScale : 1.0f;
    const auto pal = FloatingGlassBar::resolveThemePalette();
    
    // 线条样式下拉菜单 (实线、长虚线、点虚线、点划线)
    if (state.openSubmenu == SubmenuType::LineStyle) {
        float menuW = 152.0f * scale;
        float itemH = 32.0f * scale;
        float itemGap = 4.0f * scale;
        float padX = 8.0f * scale;
        float padY = 8.0f * scale;

        struct StyleItem { LineStyle style; std::wstring label; };
        std::vector<StyleItem> items = {
            { LineStyle::Solid, state.toolbarLayoutChinese ? L"实线" : L"Solid" },
            { LineStyle::Dashed, state.toolbarLayoutChinese ? L"长虚线" : L"Dashed" },
            { LineStyle::Dotted, state.toolbarLayoutChinese ? L"点虚线" : L"Dotted" },
            { LineStyle::DashDot, state.toolbarLayoutChinese ? L"点划线" : L"DashDot" },
            { LineStyle::Wavy, state.toolbarLayoutChinese ? L"波浪线" : L"Wavy" },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        auto sz = m_renderTarget->GetSize();
        auto menuRect = resolveSecondaryPopupRect(
            state, ToolbarCommand::ToggleLineStyleDropdown, menuW, menuH, sz, scale);
        float menuX = menuRect.left;
        float menuY = menuRect.top;
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(pal.hoverBg, itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentText, activeLineBrush.GetAddressOf());

        for (size_t i = 0; i < items.size(); ++i) {
            float iy = menuY + padY + i * (itemH + itemGap);
            auto itemRect = D2D1::RectF(menuX + padX, iy, menuX + menuW - padX, iy + itemH);

            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectLineStyle;
            sbtn.lineStyleParam = items[i].style;
            sbtn.rect = itemRect;
            state.submenuButtons.push_back(sbtn);

            bool isSelected = (state.currentLineStyle == items[i].style);
            bool isHovered = (state.currentCursor.x >= itemRect.left && state.currentCursor.x <= itemRect.right &&
                              state.currentCursor.y >= itemRect.top  && state.currentCursor.y <= itemRect.bottom);

            if (isSelected) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemActiveBg.Get());
            } else if (isHovered) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemHoverBg.Get());
            }

            // 绘制线条样式矢量线段
            auto iconId = (items[i].style == LineStyle::Dashed) ? CaptureIconId::PropDashedLine
                        : (items[i].style == LineStyle::Dotted) ? CaptureIconId::PropDottedLine
                        : (items[i].style == LineStyle::DashDot) ? CaptureIconId::PropDashDotLine
                        : (items[i].style == LineStyle::Wavy) ? CaptureIconId::PropWavyLine
                        : CaptureIconId::PropSolidLine;
            auto lineRect = D2D1::RectF(itemRect.left + 6.0f * scale, itemRect.top, itemRect.left + 48.0f * scale, itemRect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, lineRect,
                                           isSelected ? activeLineBrush.Get() : lineBrush.Get(), scale);

            // 绘制文字说明
            if (m_infoTextFormat && textBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(items[i].label.c_str(), static_cast<UINT32>(items[i].label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(itemRect.left + 54.0f * scale, iy, itemRect.right - 20.0f * scale, iy + itemH),
                    isSelected ? activeLineBrush.Get() : textBrush.Get());
            }

            // 选中项右侧微对勾 ✓
            if (isSelected) {
                float rcx = itemRect.right - 14.0f * scale;
                float rcy = iy + itemH * 0.5f;
                D2D1_POINT_2F p1{rcx - 3.5f * scale, rcy};
                D2D1_POINT_2F p2{rcx - 1.0f * scale, rcy + 2.8f * scale};
                D2D1_POINT_2F p3{rcx + 3.8f * scale, rcy - 3.2f * scale};
                m_renderTarget->DrawLine(p1, p2, activeLineBrush.Get(), 1.5f * scale);
                m_renderTarget->DrawLine(p2, p3, activeLineBrush.Get(), 1.5f * scale);
            }
        }
    }
    // 箭头样式下拉菜单 (标准箭头、细线箭头、燕尾箭头、双向箭头、立体箭头、路线箭头)
    else if (state.openSubmenu == SubmenuType::ArrowStyle) {
        float menuW = 142.0f * scale;
        float itemH = 31.0f * scale;
        float itemGap = 3.5f * scale;
        float padX = 8.0f * scale;
        float padY = 8.0f * scale;

        struct ArrowItem { ArrowStyle style; std::wstring label; };
        std::vector<ArrowItem> items = {
            { ArrowStyle::Standard,         state.toolbarLayoutChinese ? L"标准箭头" : L"Standard" },
            { ArrowStyle::Thin,             state.toolbarLayoutChinese ? L"细线箭头" : L"Thin" },
            { ArrowStyle::Tech,             state.toolbarLayoutChinese ? L"燕尾箭头" : L"Stealth" },
            { ArrowStyle::DoubleEnded,      state.toolbarLayoutChinese ? L"双向箭头" : L"Double" },
            { ArrowStyle::CampaignMarch,    state.toolbarLayoutChinese ? L"立体箭头" : L"3D Arrow" },
            { ArrowStyle::MarchDoubleTrack, state.toolbarLayoutChinese ? L"路线箭头" : L"Route" },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        auto sz = m_renderTarget->GetSize();
        auto menuRect = resolveSecondaryPopupRect(
            state, ToolbarCommand::ToggleArrowStyleDropdown, menuW, menuH, sz, scale);
        float menuX = menuRect.left;
        float menuY = menuRect.top;
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(pal.hoverBg, itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentText, activeLineBrush.GetAddressOf());

        for (size_t i = 0; i < items.size(); ++i) {
            float iy = menuY + padY + i * (itemH + itemGap);
            auto itemRect = D2D1::RectF(menuX + padX, iy, menuX + menuW - padX, iy + itemH);

            ToolbarButton sbtn;
            sbtn.command = ToolbarCommand::SelectArrowStyle;
            sbtn.arrowStyleParam = items[i].style;
            sbtn.rect = itemRect;
            state.submenuButtons.push_back(sbtn);

            bool isSelected = (state.currentArrowStyle == items[i].style);
            bool isHovered = (state.currentCursor.x >= itemRect.left && state.currentCursor.x <= itemRect.right &&
                              state.currentCursor.y >= itemRect.top  && state.currentCursor.y <= itemRect.bottom);

            if (isSelected) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemActiveBg.Get());
            } else if (isHovered) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemHoverBg.Get());
            }

            auto arrowIconId = (items[i].style == ArrowStyle::DoubleEnded) ? CaptureIconId::ToolArrowDouble
                             : (items[i].style == ArrowStyle::Thin) ? CaptureIconId::ToolArrowThin
                             : (items[i].style == ArrowStyle::Tech) ? CaptureIconId::ToolArrowWing
                             : (items[i].style == ArrowStyle::CampaignMarch) ? CaptureIconId::ToolArrowCampaign
                             : (items[i].style == ArrowStyle::MarchDoubleTrack) ? CaptureIconId::ToolArrowDoubleTrack
                             : CaptureIconId::ToolArrowStandard;
            auto arrowRect = D2D1::RectF(itemRect.left + 6.0f * scale, itemRect.top, itemRect.left + 42.0f * scale, itemRect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), arrowIconId, arrowRect,
                                           isSelected ? activeLineBrush.Get() : lineBrush.Get(), scale);

            if (m_infoTextFormat && textBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(items[i].label.c_str(), static_cast<UINT32>(items[i].label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(itemRect.left + 48.0f * scale, iy, itemRect.right - 20.0f * scale, iy + itemH),
                    isSelected ? activeLineBrush.Get() : textBrush.Get());
            }

            // 选中项右侧微对勾
            if (isSelected) {
                float rcx = itemRect.right - 14.0f * scale;
                float rcy = iy + itemH * 0.5f;
                D2D1_POINT_2F p1{rcx - 3.5f * scale, rcy};
                D2D1_POINT_2F p2{rcx - 1.0f * scale, rcy + 2.8f * scale};
                D2D1_POINT_2F p3{rcx + 3.8f * scale, rcy - 3.2f * scale};
                m_renderTarget->DrawLine(p1, p2, activeLineBrush.Get(), 1.5f * scale);
                m_renderTarget->DrawLine(p2, p3, activeLineBrush.Get(), 1.5f * scale);
            }
        }
    }
    // 起点 / 终点 10 种端点样式下拉菜单 (PixPin 标杆：起点图标朝左，终点图标朝右)
    else if (state.openSubmenu == SubmenuType::StartCapStyle ||
             state.openSubmenu == SubmenuType::EndCapStyle) {
        const bool isStartMenu = (state.openSubmenu == SubmenuType::StartCapStyle);
        float menuW = 158.0f * scale;
        float itemH = 29.0f * scale;
        float itemGap = 3.0f * scale;
        float padX = 8.0f * scale;
        float padY = 7.0f * scale;

        struct CapItem { LineCapStyle style; std::wstring label; };
        const bool zh = state.toolbarLayoutChinese;
        std::vector<CapItem> items = {
            { LineCapStyle::None,           tools3000::core::trWCstrFor(zh, L"无端点", L"None") },
            { LineCapStyle::OpenArrow,      tools3000::core::trWCstrFor(zh, L"细线箭头", L"Open Arrow") },
            { LineCapStyle::SolidTriangle,  tools3000::core::trWCstrFor(zh, L"实心三角", L"Solid Triangle") },
            { LineCapStyle::HollowTriangle, tools3000::core::trWCstrFor(zh, L"空心三角", L"Hollow Triangle") },
            { LineCapStyle::StealthArrow,   tools3000::core::trWCstrFor(zh, L"燕尾箭头", L"Stealth Arrow") },
            { LineCapStyle::SolidCircle,    tools3000::core::trWCstrFor(zh, L"实心圆点", L"Solid Circle") },
            { LineCapStyle::HollowCircle,   tools3000::core::trWCstrFor(zh, L"空心圆环", L"Hollow Circle") },
            { LineCapStyle::SolidDiamond,   tools3000::core::trWCstrFor(zh, L"实心菱形", L"Solid Diamond") },
            { LineCapStyle::HollowDiamond,  tools3000::core::trWCstrFor(zh, L"空心菱形", L"Hollow Diamond") },
            { LineCapStyle::BarStop,        tools3000::core::trWCstrFor(zh, L"截止竖线", L"Bar Stop") },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        auto sz = m_renderTarget->GetSize();
        auto menuRect = resolveSecondaryPopupRect(
            state,
            isStartMenu ? ToolbarCommand::ToggleStartCapDropdown : ToolbarCommand::ToggleEndCapDropdown,
            menuW, menuH, sz, scale);
        float menuX = menuRect.left;
        float menuY = menuRect.top;
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(pal.hoverBg, itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.secondaryText, lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.accentText, activeLineBrush.GetAddressOf());

        const LineCapStyle currentCap = isStartMenu ? state.currentStartCap : state.currentEndCap;

        for (size_t i = 0; i < items.size(); ++i) {
            float iy = menuY + padY + i * (itemH + itemGap);
            auto itemRect = D2D1::RectF(menuX + padX, iy, menuX + menuW - padX, iy + itemH);

            ToolbarButton sbtn;
            sbtn.command = isStartMenu ? ToolbarCommand::SelectStartCap : ToolbarCommand::SelectEndCap;
            sbtn.intParam = static_cast<int>(items[i].style);
            sbtn.rect = itemRect;
            state.submenuButtons.push_back(sbtn);

            bool isSelected = (currentCap == items[i].style);
            bool isHovered = (state.currentCursor.x >= itemRect.left && state.currentCursor.x <= itemRect.right &&
                              state.currentCursor.y >= itemRect.top  && state.currentCursor.y <= itemRect.bottom);

            if (isSelected) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemActiveBg.Get());
            } else if (isHovered) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRect, 5.5f * scale, 5.5f * scale), itemHoverBg.Get());
            }

            // 绘制端点预览图标 (起点朝左，终点朝右)
            ToolbarButton previewBtn{};
            previewBtn.command = isStartMenu ? ToolbarCommand::ToggleStartCapDropdown : ToolbarCommand::ToggleEndCapDropdown;
            previewBtn.intParam = static_cast<int>(items[i].style);
            auto capPreviewRect = D2D1::RectF(itemRect.left + 6.0f * scale, itemRect.top, itemRect.left + 44.0f * scale, itemRect.bottom);
            drawVectorButtonIcon(previewBtn, capPreviewRect, isSelected ? activeLineBrush.Get() : lineBrush.Get(), scale);

            if (m_infoTextFormat && textBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(items[i].label.c_str(), static_cast<UINT32>(items[i].label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(itemRect.left + 50.0f * scale, iy, itemRect.right - 20.0f * scale, iy + itemH),
                    isSelected ? activeLineBrush.Get() : textBrush.Get());
            }

            if (isSelected) {
                float rcx = itemRect.right - 14.0f * scale;
                float rcy = iy + itemH * 0.5f;
                D2D1_POINT_2F p1{rcx - 3.5f * scale, rcy};
                D2D1_POINT_2F p2{rcx - 1.0f * scale, rcy + 2.8f * scale};
                D2D1_POINT_2F p3{rcx + 3.8f * scale, rcy - 3.2f * scale};
                m_renderTarget->DrawLine(p1, p2, activeLineBrush.Get(), 1.5f * scale);
                m_renderTarget->DrawLine(p2, p3, activeLineBrush.Get(), 1.5f * scale);
            }
        }
    }
}

void CaptureRenderer::drawSliderPopup(CaptureState& state) {
    if (state.sliderPopup.type == SliderPopupType::None || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    std::wstring title = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? (state.beautyShell.enabled
            ? (state.toolbarLayoutChinese ? L"外壳圆角半径" : L"Shell Radius")
            : (state.toolbarLayoutChinese ? L"选区圆角半径" : L"Selection Radius"))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? (state.toolbarLayoutChinese ? L"标注圆角" : L"Corner Radius")
        : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? (state.toolbarLayoutChinese ? L"聚光灯暗化" : L"Spotlight Dim")
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? (state.toolbarLayoutChinese ? L"文本字号" : L"Font Size")
        : (state.sliderPopup.type == SliderPopupType::OutlineWidth)
        ? (state.toolbarLayoutChinese ? L"边框粗细" : L"Border Width")
        : (state.sliderPopup.type == SliderPopupType::MosaicBlockSize)
        ? ((state.currentTool == MarkupTool::Blur || state.currentMosaicType == 1)
            ? (state.toolbarLayoutChinese ? L"模糊强度" : L"Blur Strength")
            : (state.toolbarLayoutChinese ? L"马赛克粒度" : L"Mosaic Size"))
        : (state.currentTool == MarkupTool::Inpaint)
        ? (state.toolbarLayoutChinese ? L"涂抹笔刷" : L"Brush Size")
        : (state.currentTool == MarkupTool::Number)
        ? (state.toolbarLayoutChinese ? L"序号尺寸" : L"Badge Size")
        : (state.currentTool == MarkupTool::Magnifier)
        ? (state.toolbarLayoutChinese ? L"镜框粗细" : L"Rim Width")
        : (state.toolbarLayoutChinese ? L"线条粗细" : L"Stroke Width");

    int curVal = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? static_cast<int>(std::round(state.effectiveCornerRadius()))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? static_cast<int>(std::round(
              (state.activeElement &&
               (state.activeElement->tool == MarkupTool::Rectangle ||
                state.activeElement->tool == MarkupTool::Spotlight))
                  ? state.activeElement->cornerRadius
                  : state.currentElementCornerRadius))
        : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? std::clamp(static_cast<int>(std::round(state.currentSpotlightDimAlpha * 100.0f)), 15, 85)
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? static_cast<int>(std::round(state.activeElement && state.activeElement->tool == MarkupTool::Text ? state.activeElement->fontSize : tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0)))
        : (state.sliderPopup.type == SliderPopupType::OutlineWidth)
        ? ((state.activeElement && state.activeElement->hasCustomOutline)
               ? std::clamp(static_cast<int>(std::round(state.activeElement->outlineThickness)), 0, 12)
               : std::clamp(state.currentOutlineWidth, 0, 12))
        : state.currentStrokeWidth;

    state.sliderPopup.currentValue = curVal;
    int minV = (state.sliderPopup.type == SliderPopupType::SpotlightDim) ? 15 :
               (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 10 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius ||
                state.sliderPopup.type == SliderPopupType::CornerRadius ||
                state.sliderPopup.type == SliderPopupType::OutlineWidth) ? 0 : 1;
    int maxV = (state.sliderPopup.type == SliderPopupType::SpotlightDim) ? 85 :
               (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 72 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) ? 80 :
               (state.sliderPopup.type == SliderPopupType::CornerRadius) ? 40 :
               (state.sliderPopup.type == SliderPopupType::OutlineWidth) ? 12 :
               (state.sliderPopup.type == SliderPopupType::MosaicBlockSize) ? 32 : 28;
    state.sliderPopup.minValue = minV;
    state.sliderPopup.maxValue = maxV;
    state.sliderPopup.presetButtons.clear();

    const std::wstring valStr = (state.sliderPopup.type == SliderPopupType::SpotlightDim)
        ? std::format(L"{}%", curVal)
        : std::format(L"{} px", curVal);
    const std::wstring wheelHint = state.toolbarLayoutChinese ? L"也可以用滚轮调整" : L"Scroll wheel also adjusts";
    const float titleW = measureText(title, m_infoTextFormat.Get(), 800.0f * scale, 64.0f * scale).width;
    const float valTextW = measureText(valStr, m_infoTextFormat.Get(), 400.0f * scale, 64.0f * scale).width;
    const float hintW = measureText(wheelHint, m_infoTextFormat.Get(), 800.0f * scale, 64.0f * scale).width;
    const float badgeW = (std::max)(44.0f * scale, valTextW + 14.0f * scale);
    const float popW = std::max({204.0f * scale, titleW + badgeW + 52.0f * scale, hintW + 28.0f * scale});
    const float popH = 72.0f * scale;

    auto sz = m_renderTarget->GetSize();
    const float margin = 8.0f * scale;
    D2D1_RECT_F popRect{};
    if (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
        float popX = 0.0f;
        if (state.selectionSideRect.left >= state.toolbarLayoutSelection.right &&
            state.selectionSideRect.right + margin + popW <= sz.width - margin) {
            popX = state.selectionSideRect.right + margin;
        } else {
            popX = state.selectionSideRect.left - popW - margin;
        }
        popX = std::clamp(popX, margin, (std::max)(margin, sz.width - popW - margin));
        const float popY = std::clamp(state.selectionSideRect.top - 10.0f * scale,
                                      margin, (std::max)(margin, sz.height - popH - margin));
        popRect = D2D1::RectF(popX, popY, popX + popW, popY + popH);
    } else {
        const ToolbarCommand triggerCmd = (state.sliderPopup.type == SliderPopupType::CornerRadius)
            ? ToolbarCommand::CycleElementCornerRadius
            : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
                ? ToolbarCommand::CycleSpotlightDim
                : (state.sliderPopup.type == SliderPopupType::OutlineWidth)
                    ? ToolbarCommand::ToggleTextOutline
                    : ToolbarCommand::CycleStrokeWidth;
        popRect = resolveSecondaryPopupRect(state, triggerCmd, popW, popH, sz, scale);
    }

    const float popX = popRect.left;
    const float popY = popRect.top;
    state.sliderPopup.popupRect = popRect;

    // 1. 动态主题微晶浮岛
    const auto pal = FloatingGlassBar::resolveThemePalette();
    drawGlassPanel(popRect, 10.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> titleBrush, badgeBg, badgeText, trackBg, trackFill, thumbBg, thumbBorder, thumbGlow, hintBrush;
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, titleBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, badgeBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentText, badgeText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
                   : D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 0.98f),
        trackBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accent, trackFill.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), thumbBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accent, thumbBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, thumbGlow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.secondaryText, hintBrush.GetAddressOf());

    // 1. 顶部前置矢量微图标、标题与实时无级数值徽章（图标左边缘与下方滑轨严格垂直对齐）
    float topY = popY + 8.0f * scale;
    float badgeH = 19.0f * scale;
    auto valBadgeRect = D2D1::RectF(popX + popW - badgeW - 12.0f * scale, topY, popX + popW - 12.0f * scale, topY + badgeH);
    if (titleBrush) {
        const CaptureIconId popupIcon =
            (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius ||
             state.sliderPopup.type == SliderPopupType::CornerRadius)
                ? CaptureIconId::PropCornerRadius
                : (state.sliderPopup.type == SliderPopupType::SpotlightDim)
                    ? CaptureIconId::ToolSpotlight
                    : (state.sliderPopup.type == SliderPopupType::TextFontSize)
                        ? CaptureIconId::PropFontSize
                        : CaptureIconId::PropStrokeWidth;
        const float iconBoxSz = 23.0f * scale;
        const float iconCx = popX + 16.8f * scale;
        const float iconCy = topY + badgeH * 0.5f;
        const D2D1_RECT_F popupIconBox = D2D1::RectF(
            iconCx - iconBoxSz * 0.5f, iconCy - iconBoxSz * 0.5f,
            iconCx + iconBoxSz * 0.5f, iconCy + iconBoxSz * 0.5f);
        CaptureVectorIcons::renderIcon(
            m_renderTarget.Get(), m_d2dFactory.Get(), popupIcon, popupIconBox, titleBrush.Get(), scale);
    }
    if (m_infoTextFormat && titleBrush) {
        const float titleLeft = popX + 27.0f * scale;
        const D2D1_RECT_F titleRect = D2D1::RectF(
            titleLeft, topY, (std::max)(titleLeft, valBadgeRect.left - 8.0f * scale), topY + badgeH);
        TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(title.c_str(), static_cast<UINT32>(title.size()), m_infoTextFormat.Get(),
            titleRect, titleBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (badgeBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(valBadgeRect, 4.0f * scale, 4.0f * scale), badgeBg.Get());
    if (m_infoTextFormat && badgeText) {
        TextFormatScope scopeBadge(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(valStr.c_str(), static_cast<UINT32>(valStr.size()), m_infoTextFormat.Get(),
            valBadgeRect,
            badgeText.Get());
    }

    // 2. 中部无级滑动轨道与发光游标
    float trackX = popX + 12.0f * scale;
    float trackY = topY + 25.0f * scale;
    float trackW = popW - 24.0f * scale;
    float trackH = 5.0f * scale;
    auto trackRect = D2D1::RectF(trackX, trackY, trackX + trackW, trackY + trackH);
    state.sliderPopup.trackRect = trackRect;

    if (trackBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trackRect, 2.5f * scale, 2.5f * scale), trackBg.Get());

    float pct = (maxV > minV) ? std::clamp(static_cast<float>(curVal - minV) / static_cast<float>(maxV - minV), 0.0f, 1.0f) : 0.0f;
    float thumbX = trackX + pct * trackW;
    float thumbY = trackY + trackH * 0.5f;

    auto fillRect = D2D1::RectF(trackX, trackY, thumbX, trackY + trackH);
    if (trackFill && thumbX > trackX) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(fillRect, 2.5f * scale, 2.5f * scale), trackFill.Get());
    }

    float thumbR = (state.sliderPopup.isDragging ? 7.2f : 6.0f) * scale;
    auto thumbEllipse = D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR);
    state.sliderPopup.thumbRect = D2D1::RectF(thumbX - thumbR - 2.0f * scale, thumbY - thumbR - 2.0f * scale, thumbX + thumbR + 2.0f * scale, thumbY + thumbR + 2.0f * scale);

    if (thumbGlow) m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR + 3.0f * scale, thumbR + 3.0f * scale), thumbGlow.Get());
    if (thumbBg) m_renderTarget->FillEllipse(thumbEllipse, thumbBg.Get());
    if (thumbBorder) m_renderTarget->DrawEllipse(thumbEllipse, thumbBorder.Get(), 1.5f * scale);

    // 3. 底部温和滚轮微调引导（替代刻板固定档位按钮行）
    if (m_infoTextFormat && hintBrush) {
        const float hintTop = trackY + 11.0f * scale;
        const D2D1_RECT_F hintRect = D2D1::RectF(popX + 12.0f * scale, hintTop, popX + popW - 12.0f * scale, popY + popH - 5.0f * scale);
        TextFormatScope hScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(wheelHint.c_str(), static_cast<UINT32>(wheelHint.size()), m_infoTextFormat.Get(),
            hintRect, hintBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

namespace renderer_detail {
void getBeautyColors(BeautyBackgroundType type, D2D1_COLOR_F& c1, D2D1_COLOR_F& c2, D2D1_COLOR_F& c3) {
    switch (type) {
        case BeautyBackgroundType::StudioSlate:
            // 极光星云: Indigo (#6366F1) -> Violet (#8B5CF6) -> Rose (#EC4899)
            c1 = D2D1::ColorF(99.0f / 255.0f, 102.0f / 255.0f, 241.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(139.0f / 255.0f, 92.0f / 255.0f, 246.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(236.0f / 255.0f, 72.0f / 255.0f, 153.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PaperChalk:
            // 晨曦落日: Coral (#FB7185) -> Amber (#F97316) -> Gold (#FACC15)
            c1 = D2D1::ColorF(251.0f / 255.0f, 113.0f / 255.0f, 133.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(249.0f / 255.0f, 115.0f / 255.0f, 22.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(250.0f / 255.0f, 204.0f / 255.0f, 21.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::SilkMist:
            // 碧海极光: Sky (#0EA5E9) -> Teal (#14B8A6) -> Emerald (#10B981)
            c1 = D2D1::ColorF(14.0f / 255.0f, 165.0f / 255.0f, 233.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(20.0f / 255.0f, 184.0f / 255.0f, 166.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(16.0f / 255.0f, 185.0f / 255.0f, 129.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::MidnightGraphite:
            // 曜石黑晶: Slate (#334155) -> Obsidian (#1E293B) -> Carbon (#090D16)
            c1 = D2D1::ColorF(51.0f / 255.0f, 65.0f / 255.0f, 85.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(9.0f / 255.0f, 13.0f / 255.0f, 22.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PureMinimal:
        default:
            // 钛金画廊: Alabaster (#F8FAFC) -> Silver (#E2E8F0) -> Titanium (#CBD5E1)
            c1 = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 1.0f);
            c3 = D2D1::ColorF(203.0f / 255.0f, 213.0f / 255.0f, 225.0f / 255.0f, 1.0f);
            break;
    }
}

} // namespace renderer_detail

void CaptureRenderer::drawBeautyShellPreview(const D2D1_RECT_F& rect, CaptureState& state) {
    if (!state.beautyShell.enabled || !m_renderTarget) return;
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const float geomScale = state.isLongImageMode() ? 1.0f : scale;
    const float pad = state.beautyShell.padding * geomScale;
    if (pad <= 2.0f * geomScale) return;

    D2D1_RECT_F shellRect = D2D1::RectF(rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad);
    const float shellCornerR = (std::max)(12.0f * geomScale, (state.beautyShell.cornerRadius + 6.0f) * geomScale);
    auto roundedShell = D2D1::RoundedRect(shellRect, shellCornerR, shellCornerR);

    // 1. 绘制三色段高饱和网格渐变底板画布
    D2D1_COLOR_F c1, c2, c3;
    getBeautyColors(state.beautyShell.bgType, c1, c2, c3);

    D2D1_GRADIENT_STOP stops[3];
    stops[0].position = 0.0f; stops[0].color = c1;
    stops[1].position = 0.5f; stops[1].color = c2;
    stops[2].position = 1.0f; stops[2].color = c3;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCol;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 3, stopCol.GetAddressOf())) && stopCol) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto gradProps = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(shellRect.left, shellRect.top),
            D2D1::Point2F(shellRect.right, shellRect.bottom));
        if (SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(gradProps, stopCol.Get(), gradBrush.GetAddressOf())) && gradBrush) {
            m_renderTarget->FillRoundedRectangle(roundedShell, gradBrush.Get());
        }
    }

    // 叠加左上角环境柔光高光 (Mesh Light Bloom)
    D2D1_GRADIENT_STOP bloomStops[2];
    const float bloomAlpha = (state.beautyShell.bgType == BeautyBackgroundType::MidnightGraphite) ? 0.10f : 0.22f;
    bloomStops[0].position = 0.0f; bloomStops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, bloomAlpha);
    bloomStops[1].position = 1.0f; bloomStops[1].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f);
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> bloomStopCol;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(bloomStops, 2, bloomStopCol.GetAddressOf())) && bloomStopCol) {
        const float shellW = shellRect.right - shellRect.left;
        const float shellH = shellRect.bottom - shellRect.top;
        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> bloomBrush;
        auto radialProps = D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(shellRect.left + shellW * 0.18f, shellRect.top + shellH * 0.15f),
            D2D1::Point2F(0.0f, 0.0f),
            shellW * 0.65f, shellH * 0.65f);
        if (SUCCEEDED(m_renderTarget->CreateRadialGradientBrush(radialProps, bloomStopCol.Get(), bloomBrush.GetAddressOf())) && bloomBrush) {
            m_renderTarget->FillRoundedRectangle(roundedShell, bloomBrush.Get());
        }
    }

    // 外壳边缘微晶高光描边
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shellBorder;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), shellBorder.GetAddressOf());
    if (shellBorder) {
        m_renderTarget->DrawRoundedRectangle(roundedShell, shellBorder.Get(), 1.2f * geomScale);
    }

    // 2. 渲染 10 层平滑指数衰减立体悬浮弥散阴影 (True Multi-Step Soft Elevation Shadow)
    const float r = state.beautyShell.cornerRadius * geomScale;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, 1.0f), shadowBrush.GetAddressOf());
    if (shadowBrush) {
        const float maxSpread = (std::min)(pad * 0.78f, 26.0f * geomScale);
        const float maxDropY = (std::min)(pad * 0.42f, 12.0f * geomScale);
        constexpr int kShadowSteps = 10;
        for (int i = kShadowSteps; i >= 1; --i) {
            const float t = static_cast<float>(i) / static_cast<float>(kShadowSteps);
            const float spread = maxSpread * (t * t);
            const float dropY = maxDropY * std::pow(t, 1.35f);
            const float layerAlpha = (1.0f - t * 0.72f) * 0.052f;
            shadowBrush->SetOpacity(layerAlpha);
            auto sRect = D2D1::RoundedRect(
                D2D1::RectF(rect.left - spread, rect.top + dropY - spread * 0.45f,
                            rect.right + spread, rect.bottom + dropY + spread),
                r + spread, r + spread);
            m_renderTarget->FillRoundedRectangle(sRect, shadowBrush.Get());
        }
    }

    // 3. 将原图裁切为指定圆角后重新绘制于 rect 内部，并叠加双层微晶玻璃立体倒角
    if (hasScreenContent() && m_d2dFactory) {
        auto roundedClip = D2D1::RoundedRect(rect, r, r);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedClip, clipGeo.GetAddressOf())) && clipGeo) {
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), layer.Get());
                if (m_screenBitmap) {
                    const auto bmpSz = m_screenBitmap->GetSize();
                    const auto rtSz = m_renderTarget->GetSize();
                    const D2D1_RECT_F dstR = state.isLongImageMode()
                        ? D2D1::RectF(0.0f, 0.0f, bmpSz.width, bmpSz.height)
                        : D2D1::RectF(0.0f, 0.0f, rtSz.width, rtSz.height);
                    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstR);
                } else if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                    const D2D1_RECT_F fullSrcRect = D2D1::RectF(
                        0.0f, 0.0f, static_cast<float>(m_screenTilesW), static_cast<float>(m_screenTilesH));
                    const D2D1_RECT_F fullDstRect = state.isLongImageMode()
                        ? fullSrcRect
                        : D2D1::RectF(0.0f, 0.0f, m_renderTarget->GetSize().width, m_renderTarget->GetSize().height);
                    drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                    fullDstRect, fullSrcRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                m_renderTarget->PopLayer();
            }
        }

        // 外沿 1px 暗部接触环 + 内沿 1.2px 高光玻璃环
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outerRimBrush, innerSpecBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f), outerRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f), innerSpecBrush.GetAddressOf());
        if (outerRimBrush) {
            auto outerRing = D2D1::RoundedRect(
                D2D1::RectF(rect.left - 0.5f * geomScale, rect.top - 0.5f * geomScale,
                            rect.right + 0.5f * geomScale, rect.bottom + 0.5f * geomScale),
                r + 0.5f * geomScale, r + 0.5f * geomScale);
            m_renderTarget->DrawRoundedRectangle(outerRing, outerRimBrush.Get(), 1.0f * geomScale);
        }
        if (innerSpecBrush) {
            auto innerRing = D2D1::RoundedRect(
                D2D1::RectF(rect.left + 0.6f * geomScale, rect.top + 0.6f * geomScale,
                            rect.right - 0.6f * geomScale, rect.bottom - 0.6f * geomScale),
                (std::max)(0.0f, r - 0.6f * geomScale), (std::max)(0.0f, r - 0.6f * geomScale));
            m_renderTarget->DrawRoundedRectangle(innerRing, innerSpecBrush.Get(), 1.2f * geomScale);
        }
    }
}

void CaptureRenderer::drawDropdownMenu(CaptureState& state) {
    if (state.dropdownMenu.type == DropdownType::None || state.dropdownMenu.items.empty() || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto pal = FloatingGlassBar::resolveThemePalette();
    const bool isDark = pal.isDark;
    const bool isCascadingCheckMenu = (state.dropdownMenu.type == DropdownType::RecordMoreSettings);

    // 1. 响应式微晶双列排版框架：严格测量全项主标题与辅助描述的最大宽度
    float maxLabelW = 0.0f;
    float maxHintW = 0.0f;
    bool hasAnySwatch = false;
    bool hasAnyChild = false;

    for (const auto& it : state.dropdownMenu.items) {
        if (it.isSeparator) continue;
        if (!it.label.empty()) {
            D2D1_SIZE_F lsz = measureText(it.label, m_infoTextFormat.Get());
            if (lsz.width > maxLabelW) maxLabelW = lsz.width;
        }
        if (!it.hint.empty()) {
            D2D1_SIZE_F hsz = measureText(it.hint, m_infoTextFormat.Get());
            if (hsz.width > maxHintW) maxHintW = hsz.width;
        }
        if (it.hasSwatch) hasAnySwatch = true;
        if (it.hasChildMenu) hasAnyChild = true;
    }

    // 几何规格与自适应留白 (MECE 双列对齐，杜绝重叠与横向跑动)
    const float itemH = 32.0f * scale;
    const float sepH = 10.0f * scale;
    const float padY = 7.0f * scale;
    const float padX = 10.0f * scale;
    const float swatchSpace = hasAnySwatch ? (12.0f * scale + 10.0f * scale) : 0.0f;
    const float leadCheckSpace = isCascadingCheckMenu ? (22.0f * scale) : 0.0f;
    const float columnGap = (maxHintW > 0.0f) ? 20.0f * scale : 0.0f; // 保证主副文本之间有绝对安全的呼吸间距
    const float checkSpace = isCascadingCheckMenu
        ? (hasAnyChild ? 22.0f * scale : 8.0f * scale)
        : 24.0f * scale; // 选中对勾或右向箭头区域

    float contentW = swatchSpace + leadCheckSpace + maxLabelW + columnGap + maxHintW + checkSpace;
    float menuW = contentW + padX * 2.0f + 16.0f * scale;
    const float minMenuW = (isCascadingCheckMenu ? 156.0f : (maxHintW > 0.0f ? 144.0f : 112.0f)) * scale;
    if (menuW < minMenuW) menuW = minMenuW;

    float totalItemsH = 0.0f;
    for (const auto& it : state.dropdownMenu.items) {
        totalItemsH += it.isSeparator ? sepH : itemH;
    }
    float menuH = padY * 2.0f + totalItemsH;

    const auto& anchor = state.dropdownMenu.anchorButtonRect;
    auto rtSize = m_renderTarget->GetSize();
    float menuX = anchor.left;
    if (menuX + menuW > rtSize.width - 4.0f * scale) {
        menuX = rtSize.width - menuW - 4.0f * scale;
    }
    if (menuX < 4.0f * scale) menuX = 4.0f * scale;

    // 双向跨工具栏避让：若触发按钮正上方紧贴另一条工具栏（如二级栏在主工具栏下方），优先向下弹出；
    // 若优先向上但顶部空间不足向下翻转时，自动跨过紧贴的下层二级属性栏，绝不遮挡相邻工具栏！
    const float gapY = 6.0f * scale;
    float combinedTop = anchor.top;
    float combinedBottom = anchor.bottom;
    bool barAboveAnchor = false;
    bool barBelowAnchor = false;
    auto inspectBarForDropdown = [&](const D2D1_RECT_F& bar) {
        if (bar.right <= bar.left || bar.bottom <= bar.top) return;
        combinedTop = (std::min)(combinedTop, bar.top);
        combinedBottom = (std::max)(combinedBottom, bar.bottom);
        const bool xOverlap = (menuX < bar.right + 4.0f * scale) && (menuX + menuW > bar.left - 4.0f * scale);
        if (!xOverlap) return;
        if (bar.bottom <= anchor.top + 4.0f * scale && anchor.top - bar.bottom <= 22.0f * scale) {
            barAboveAnchor = true;
        }
        if (bar.top >= anchor.bottom - 4.0f * scale && bar.top - anchor.bottom <= 22.0f * scale) {
            barBelowAnchor = true;
        }
    };
    inspectBarForDropdown(state.primaryToolbarRect);
    inspectBarForDropdown(state.secondaryToolbarRect);

    float menuY = 0.0f;
    if (barAboveAnchor && !barBelowAnchor) {
        menuY = anchor.bottom + gapY;
        if (menuY + menuH > rtSize.height - 4.0f * scale) {
            menuY = combinedTop - menuH - gapY;
        }
    } else {
        menuY = (barAboveAnchor ? combinedTop : anchor.top) - menuH - gapY;
        if (menuY < 4.0f * scale) {
            menuY = (barBelowAnchor ? combinedBottom : anchor.bottom) + gapY;
        }
    }
    menuY = std::clamp(menuY, 4.0f * scale, (std::max)(4.0f * scale, rtSize.height - menuH - 4.0f * scale));

    state.dropdownMenu.menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);

    // 2. 绘制微晶玻璃浮岛底板
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), state.dropdownMenu.menuRect, 8.0f * scale, isDark);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush, hintBrush, checkBrush;
    m_renderTarget->CreateSolidColorBrush(
        pal.primaryText,
        textBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.secondaryText,
        hintBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.accentText,
        checkBrush.GetAddressOf());

    // 确保文本格式作用域隔离为左对齐、垂直居中、不自动换行
    TextFormatScope menuTextScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);

    // 3. 绘制各选项 (双列严格对齐 + 级联子菜单)
    float curY = menuY + padY;
    const float labelStartX = menuX + padX + 8.0f * scale + swatchSpace + leadCheckSpace;
    const float hintStartX = labelStartX + maxLabelW + columnGap;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> sepBrush;
    m_renderTarget->CreateSolidColorBrush(
        isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f),
        sepBrush.GetAddressOf());

    for (size_t i = 0; i < state.dropdownMenu.items.size(); ++i) {
        auto& item = state.dropdownMenu.items[i];
        if (item.isSeparator) {
            item.rect = D2D1::RectF(menuX + padX, curY, menuX + menuW - padX, curY + sepH);
            item.isHovered = false;
            if (sepBrush) {
                const float lineY = std::floor(curY + sepH * 0.5f) + 0.5f;
                m_renderTarget->DrawLine(
                    D2D1::Point2F(menuX + padX + 4.0f * scale, lineY),
                    D2D1::Point2F(menuX + menuW - padX - 4.0f * scale, lineY),
                    sepBrush.Get(), 1.0f * scale);
            }
            curY += sepH;
            continue;
        }

        item.rect = D2D1::RectF(menuX + padX, curY, menuX + menuW - padX, curY + itemH);

        const bool isActiveParent = (static_cast<int>(i) == state.dropdownMenu.activeParentIndex && item.hasChildMenu);
        bool isHover = (static_cast<int>(i) == state.dropdownMenu.hoveredIndex) || isActiveParent;
        item.isHovered = isHover;

        const bool highlightPill = isHover || (!isCascadingCheckMenu && item.isSelected);
        if (highlightPill) {
            FloatingGlassBar::drawButtonPill(
                m_renderTarget.Get(), item.rect, 6.0f * scale,
                isHover, (!isCascadingCheckMenu && item.isSelected), false, false, 1.0f, isDark);
        }

        // 微型渐变色板圆球
        if (item.hasSwatch) {
            float swatchRadius = 6.0f * scale;
            float swatchCx = item.rect.left + 8.0f * scale + swatchRadius;
            float swatchCy = curY + itemH * 0.5f;

            D2D1_GRADIENT_STOP swatchStops[2];
            swatchStops[0].position = 0.0f; swatchStops[0].color = item.swatchColor1;
            swatchStops[1].position = 1.0f; swatchStops[1].color = item.swatchColor2;

            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> scCol;
            if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(swatchStops, 2, scCol.GetAddressOf())) && scCol) {
                Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> scBrush;
                auto scProps = D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(swatchCx - swatchRadius, swatchCy - swatchRadius),
                    D2D1::Point2F(swatchCx + swatchRadius, swatchCy + swatchRadius));
                if (SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(scProps, scCol.Get(), scBrush.GetAddressOf())) && scBrush) {
                    auto ellipse = D2D1::Ellipse(D2D1::Point2F(swatchCx, swatchCy), swatchRadius, swatchRadius);
                    m_renderTarget->FillEllipse(ellipse, scBrush.Get());
                    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> scBorder;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f), scBorder.GetAddressOf());
                    if (scBorder) m_renderTarget->DrawEllipse(ellipse, scBorder.Get(), 1.0f);
                }
            }
        }

        // 级联设置主菜单：左侧选中对勾 (与 FocuSee 原生视觉 1:1 对齐)
        if (isCascadingCheckMenu && item.isSelected && textBrush) {
            const float ckX = item.rect.left + 13.0f * scale;
            const float ckY = curY + itemH * 0.5f;
            D2D1_POINT_2F p1{ckX - 4.2f * scale, ckY + 0.2f * scale};
            D2D1_POINT_2F p2{ckX - 1.2f * scale, ckY + 3.2f * scale};
            D2D1_POINT_2F p3{ckX + 4.8f * scale, ckY - 3.0f * scale};
            m_renderTarget->DrawLine(p1, p2, textBrush.Get(), 1.85f * scale);
            m_renderTarget->DrawLine(p2, p3, textBrush.Get(), 1.85f * scale);
        }

        // 绘制标题 Label (首列)
        if (m_infoTextFormat && textBrush && !item.label.empty()) {
            D2D1_RECT_F textRect = D2D1::RectF(labelStartX, curY, labelStartX + maxLabelW + 4.0f * scale, curY + itemH);
            m_renderTarget->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                     m_infoTextFormat.Get(), textRect, textBrush.Get());
        }

        // 绘制副标题 Hint (次列：全列表横向绝对统一对齐，杜绝跑动与重叠)
        if (m_infoTextFormat && hintBrush && !item.hint.empty()) {
            D2D1_RECT_F hintRect = D2D1::RectF(hintStartX, curY, item.rect.right - checkSpace, curY + itemH);
            m_renderTarget->DrawText(item.hint.c_str(), static_cast<UINT32>(item.hint.size()),
                                     m_infoTextFormat.Get(), hintRect, hintBrush.Get());
        }

        // 右侧子菜单箭头 `>` 或常规选中对勾
        if (item.hasChildMenu && hintBrush) {
            const float chevBox = 14.0f * scale;
            const float chevCx = item.rect.right - 12.0f * scale;
            const float chevCy = curY + itemH * 0.5f;
            D2D1_RECT_F chevRect = D2D1::RectF(
                chevCx - chevBox * 0.5f, chevCy - chevBox * 0.5f,
                chevCx + chevBox * 0.5f, chevCy + chevBox * 0.5f);
            CaptureVectorIcons::renderIcon(
                m_renderTarget.Get(), m_d2dFactory.Get(),
                CaptureIconId::UiChevronRight, chevRect,
                isHover ? textBrush.Get() : hintBrush.Get(), scale);
        } else if (!isCascadingCheckMenu && item.isSelected && checkBrush) {
            float ckX = item.rect.right - 14.0f * scale;
            float ckY = curY + itemH * 0.5f;
            D2D1_POINT_2F p1{ckX - 4.0f * scale, ckY};
            D2D1_POINT_2F p2{ckX - 1.0f * scale, ckY + 3.0f * scale};
            D2D1_POINT_2F p3{ckX + 5.0f * scale, ckY - 3.0f * scale};
            m_renderTarget->DrawLine(p1, p2, checkBrush.Get(), 1.8f * scale);
            m_renderTarget->DrawLine(p2, p3, checkBrush.Get(), 1.8f * scale);
        }

        curY += itemH;
    }

    // 4. 若当前激活了二级级联子菜单（优先向左弹出，空间不足自动向右翻转）
    state.dropdownMenu.childMenuRect = {};
    if (state.dropdownMenu.activeParentIndex >= 0 &&
        state.dropdownMenu.activeParentIndex < static_cast<int>(state.dropdownMenu.items.size())) {
        auto& parent = state.dropdownMenu.items[state.dropdownMenu.activeParentIndex];
        if (parent.hasChildMenu && !parent.childItems.empty()) {
            float maxChildW = 0.0f;
            for (const auto& ci : parent.childItems) {
                if (!ci.label.empty()) {
                    D2D1_SIZE_F csz = measureText(ci.label, m_infoTextFormat.Get());
                    if (csz.width > maxChildW) maxChildW = csz.width;
                }
            }
            float childW = (std::max)(118.0f * scale, maxChildW + 26.0f * scale + padX * 2.0f + 14.0f * scale);
            float childH = padY * 2.0f + static_cast<float>(parent.childItems.size()) * itemH;
            const float sideGap = 6.0f * scale;
            float childX = menuX - childW - sideGap;
            if (childX < 4.0f * scale) {
                childX = menuX + menuW + sideGap;
                if (childX + childW > rtSize.width - 4.0f * scale) {
                    childX = (std::max)(4.0f * scale, rtSize.width - childW - 4.0f * scale);
                }
            }
            float childY = std::clamp(
                parent.rect.top - padY,
                4.0f * scale,
                (std::max)(4.0f * scale, rtSize.height - childH - 4.0f * scale));
            state.dropdownMenu.childMenuRect = D2D1::RectF(childX, childY, childX + childW, childY + childH);

            FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), state.dropdownMenu.childMenuRect, 9.0f * scale, isDark);

            float cyChild = childY + padY;
            for (size_t c = 0; c < parent.childItems.size(); ++c) {
                auto& cItem = parent.childItems[c];
                cItem.rect = D2D1::RectF(childX + padX, cyChild, childX + childW - padX, cyChild + itemH);
                const bool cHover = (static_cast<int>(c) == state.dropdownMenu.hoveredChildIndex);
                cItem.isHovered = cHover;
                if (cHover || cItem.isSelected) {
                    FloatingGlassBar::drawButtonPill(
                        m_renderTarget.Get(), cItem.rect, 6.0f * scale,
                        cHover, cItem.isSelected, false, false, 1.0f, isDark);
                }
                if (m_infoTextFormat && textBrush && !cItem.label.empty()) {
                    D2D1_RECT_F cTextRect = D2D1::RectF(
                        cItem.rect.left + 10.0f * scale, cyChild,
                        cItem.rect.right - 24.0f * scale, cyChild + itemH);
                    m_renderTarget->DrawText(
                        cItem.label.c_str(), static_cast<UINT32>(cItem.label.size()),
                        m_infoTextFormat.Get(), cTextRect,
                        (cHover || cItem.isSelected) ? textBrush.Get() : hintBrush.Get());
                }
                if (cItem.isSelected && (textBrush || checkBrush)) {
                    ID2D1Brush* ckB = textBrush ? static_cast<ID2D1Brush*>(textBrush.Get()) : static_cast<ID2D1Brush*>(checkBrush.Get());
                    const float ckX = cItem.rect.right - 13.0f * scale;
                    const float ckY = cyChild + itemH * 0.5f;
                    D2D1_POINT_2F p1{ckX - 4.2f * scale, ckY + 0.2f * scale};
                    D2D1_POINT_2F p2{ckX - 1.2f * scale, ckY + 3.2f * scale};
                    D2D1_POINT_2F p3{ckX + 4.8f * scale, ckY - 3.0f * scale};
                    m_renderTarget->DrawLine(p1, p2, ckB, 1.85f * scale);
                    m_renderTarget->DrawLine(p2, p3, ckB, 1.85f * scale);
                }
                cyChild += itemH;
            }
        }
    }
}

} // namespace tools3000::capture
