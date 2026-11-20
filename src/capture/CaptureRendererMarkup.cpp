// ─────────────────────────────────────────────────────────────────────────────
// CaptureRendererMarkup.cpp — 截图工具栏矢量图标、毛玻璃面板、浮动 Toast 与标注预览渲染子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureRendererInternal.h"
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

void CaptureRenderer::drawVectorButtonIcon(const ToolbarButton& button, const D2D1_RECT_F& rect, ID2D1Brush* brush, float scale) {
    if (!m_renderTarget || !brush) return;

    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;

    // 二级属性栏「左矢量图标 + 右数值文本」步进胶囊黄金光学排版（避免 18px 窄槽位触发 0.63 二次微缩失真）
    auto drawStepperIconAndLabel = [&](CaptureIconId iconId) {
        if (button.label.empty()) {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect, brush, scale);
            return;
        }
        const float btnH = rect.bottom - rect.top;
        const float iconBoxSz = (std::min)(btnH * 0.84f, 26.8f * scale);
        const float iconCx = rect.left + 13.0f * scale;
        const D2D1_RECT_F iconBox = D2D1::RectF(
            iconCx - iconBoxSz * 0.5f, cy - iconBoxSz * 0.5f,
            iconCx + iconBoxSz * 0.5f, cy + iconBoxSz * 0.5f);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, iconBox, brush, scale);
        if (m_infoTextFormat) {
            const float rightInset = button.hasDropdown ? 10.5f * scale : 4.5f * scale;
            const float textLeft = rect.left + 21.5f * scale;
            const D2D1_RECT_F textRect = D2D1::RectF(
                textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
            TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };

    switch (button.command) {
        case ToolbarCommand::Copy:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCopy, rect, brush, scale);
            return;
        case ToolbarCommand::Confirm:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionConfirm, rect, brush, scale);
            return;
        case ToolbarCommand::Cancel:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCancel, rect, brush, scale);
            return;
        case ToolbarCommand::Undo:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionUndo, rect, brush, scale);
            return;
        case ToolbarCommand::Redo:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionRedo, rect, brush, scale);
            return;
        case ToolbarCommand::Clear:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionClear, rect, brush, scale);
            return;
        case ToolbarCommand::SideToggleCornerRadius:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropCornerRadius, rect, brush, scale);
            return;
        case ToolbarCommand::SideInvertSelection:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionInvertSelection, rect, brush, scale);
            return;
        case ToolbarCommand::SideResetSelection:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionResetSelection, rect, brush, scale);
            return;
        case ToolbarCommand::ExtractText:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionExtractText, rect, brush, scale);
            return;
        case ToolbarCommand::PinWindow:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionPinWindow, rect, brush, scale);
            return;
        case ToolbarCommand::ScrollCapture:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionScrollCapture, rect, brush, scale);
            return;
        case ToolbarCommand::StartRecord:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionRecordVideo, rect, brush, scale);
            return;
        case ToolbarCommand::SelectTool: {
            switch (button.tool) {
                case MarkupTool::Rectangle:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolRectangle, rect, brush, scale);
                    return;
                case MarkupTool::Line:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolLine, rect, brush, scale);
                    return;
                case MarkupTool::Ellipse:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolEllipse, rect, brush, scale);
                    return;
                case MarkupTool::Arrow:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolArrow, rect, brush, scale);
                    return;
                case MarkupTool::Pen:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolPen, rect, brush, scale);
                    return;
                case MarkupTool::Highlight:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolHighlight, rect, brush, scale);
                    return;
                case MarkupTool::Mosaic:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolMosaic, rect, brush, scale);
                    return;
                case MarkupTool::Blur:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolBlur, rect, brush, scale);
                    return;
                case MarkupTool::Text:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolText, rect, brush, scale);
                    return;
                case MarkupTool::Number:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolNumber, rect, brush, scale);
                    return;
                case MarkupTool::Magnifier:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolMagnifier, rect, brush, scale);
                    return;
                case MarkupTool::Spotlight:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolSpotlight, rect, brush, scale);
                    return;
                case MarkupTool::Inpaint:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolInpaint, rect, brush, scale);
                    return;
                default: break;
            }
            break;
        }
        case ToolbarCommand::ToggleFill: {
            if (!button.label.empty()) {
                // PixPin 标杆：左侧 13x13 圆角复选框 (勾选时填充科技蓝 #3B82F6 + 纯白对勾，未勾选绘灰框) + 右侧文字
                const float boxSz = 13.0f * scale;
                const float bx = rect.left + 10.5f * scale;
                const D2D1_RECT_F chkBox = D2D1::RectF(bx - boxSz * 0.5f, cy - boxSz * 0.5f, bx + boxSz * 0.5f, cy + boxSz * 0.5f);
                const D2D1_ROUNDED_RECT chkRound = D2D1::RoundedRect(chkBox, 3.0f * scale, 3.0f * scale);
                if (button.boolParam) {
                    ComPtr<ID2D1SolidColorBrush> blueFill, whiteCheck;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(59.0f / 255.0f, 130.0f / 255.0f, 246.0f / 255.0f, 1.0f), blueFill.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteCheck.GetAddressOf());
                    if (blueFill) m_renderTarget->FillRoundedRectangle(chkRound, blueFill.Get());
                    if (whiteCheck) {
                        m_renderTarget->DrawLine(D2D1::Point2F(bx - 3.2f * scale, cy + 0.2f * scale),
                                                 D2D1::Point2F(bx - 0.8f * scale, cy + 2.6f * scale),
                                                 whiteCheck.Get(), 1.65f * scale);
                        m_renderTarget->DrawLine(D2D1::Point2F(bx - 0.8f * scale, cy + 2.6f * scale),
                                                 D2D1::Point2F(bx + 3.5f * scale, cy - 2.3f * scale),
                                                 whiteCheck.Get(), 1.65f * scale);
                    }
                } else {
                    ComPtr<ID2D1SolidColorBrush> grayBorder;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.85f), grayBorder.GetAddressOf());
                    if (grayBorder) m_renderTarget->DrawRoundedRectangle(chkRound, grayBorder.Get(), 1.3f * scale);
                }
                if (m_infoTextFormat) {
                    const D2D1_RECT_F textRect = D2D1::RectF(rect.left + 19.5f * scale, rect.top, rect.right - 2.0f * scale, rect.bottom);
                    TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER,
                                          DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                        m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                return;
            }
            // 文字工具下为背景胶囊开关；局部放大下为倍率角标开关；聚光灯下为专属描边外框开关；几何形状下为实心/空心切换
            const auto fillIcon = (button.tool == MarkupTool::Text)
                ? CaptureIconId::PropTextBackground
                : (button.tool == MarkupTool::Magnifier)
                    ? CaptureIconId::PropMagnifierBadge
                    : (button.tool == MarkupTool::Spotlight)
                        ? CaptureIconId::PropSpotlightBorder
                        : (button.boolParam ? CaptureIconId::PropFillSolid : CaptureIconId::PropFillOutline);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), fillIcon, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleLineArrow: {
            const float boxSz = 13.0f * scale;
            const float bx = rect.left + 10.5f * scale;
            const D2D1_RECT_F chkBox = D2D1::RectF(bx - boxSz * 0.5f, cy - boxSz * 0.5f, bx + boxSz * 0.5f, cy + boxSz * 0.5f);
            const D2D1_ROUNDED_RECT chkRound = D2D1::RoundedRect(chkBox, 3.0f * scale, 3.0f * scale);
            if (button.boolParam) {
                ComPtr<ID2D1SolidColorBrush> blueFill, whiteCheck;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(59.0f / 255.0f, 130.0f / 255.0f, 246.0f / 255.0f, 1.0f), blueFill.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteCheck.GetAddressOf());
                if (blueFill) m_renderTarget->FillRoundedRectangle(chkRound, blueFill.Get());
                if (whiteCheck) {
                    m_renderTarget->DrawLine(D2D1::Point2F(bx - 3.2f * scale, cy + 0.2f * scale),
                                             D2D1::Point2F(bx - 0.8f * scale, cy + 2.6f * scale),
                                             whiteCheck.Get(), 1.65f * scale);
                    m_renderTarget->DrawLine(D2D1::Point2F(bx - 0.8f * scale, cy + 2.6f * scale),
                                             D2D1::Point2F(bx + 3.5f * scale, cy - 2.3f * scale),
                                             whiteCheck.Get(), 1.65f * scale);
                }
            } else {
                ComPtr<ID2D1SolidColorBrush> grayBorder;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.85f), grayBorder.GetAddressOf());
                if (grayBorder) m_renderTarget->DrawRoundedRectangle(chkRound, grayBorder.Get(), 1.3f * scale);
            }
            if (!button.label.empty() && m_infoTextFormat) {
                const D2D1_RECT_F textRect = D2D1::RectF(rect.left + 19.5f * scale, rect.top, rect.right - 2.0f * scale, rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER,
                                      DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::ToggleCurveMode: {
            const CaptureIconId iconId = (button.intParam == 1)
                ? CaptureIconId::PropPolyline
                : CaptureIconId::PropSmoothCurve;
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                iconId, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleStartCapDropdown:
        case ToolbarCommand::ToggleEndCapDropdown: {
            const bool isStart = (button.command == ToolbarCommand::ToggleStartCapDropdown);
            const auto capStyle = static_cast<LineCapStyle>(std::clamp(button.intParam, 0, 9));
            const float rightPad = button.hasDropdown ? 9.5f * scale : 3.0f * scale;
            const float xLeft = rect.left + 4.5f * scale;
            const float xRight = (std::max)(xLeft + 8.0f * scale, rect.right - rightPad);
            const float tipX = isStart ? xLeft : xRight;
            const float tailX = isStart ? xRight : xLeft;
            const float dir = isStart ? 1.0f : -1.0f; // 从 tip 指向 tail 的方向 (+1 向右, -1 向左)
            const float hLen = 6.8f * scale;
            const float lw = 1.65f * scale;
            float shaftStartX = tipX;

            switch (capStyle) {
                case LineCapStyle::None:
                    break;
                case LineCapStyle::OpenArrow: {
                    m_renderTarget->DrawLine(D2D1::Point2F(tipX, cy), D2D1::Point2F(tipX + dir * hLen, cy - 3.6f * scale), brush, lw);
                    m_renderTarget->DrawLine(D2D1::Point2F(tipX, cy), D2D1::Point2F(tipX + dir * hLen, cy + 3.6f * scale), brush, lw);
                    break;
                }
                case LineCapStyle::SolidTriangle:
                case LineCapStyle::HollowTriangle: {
                    const float baseX = tipX + dir * hLen;
                    const float halfH = 3.8f * scale;
                    ComPtr<ID2D1PathGeometry> geo;
                    if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(D2D1::Point2F(tipX, cy), D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(baseX, cy - halfH));
                            sink->AddLine(D2D1::Point2F(baseX, cy + halfH));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            if (capStyle == LineCapStyle::SolidTriangle) {
                                m_renderTarget->FillGeometry(geo.Get(), brush);
                            } else {
                                m_renderTarget->DrawGeometry(geo.Get(), brush, 1.35f * scale);
                            }
                        }
                    }
                    shaftStartX = baseX;
                    break;
                }
                case LineCapStyle::StealthArrow: {
                    const float backX = tipX + dir * hLen;
                    const float notchX = tipX + dir * (hLen * 0.65f);
                    const float halfH = 4.0f * scale;
                    ComPtr<ID2D1PathGeometry> geo;
                    if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(D2D1::Point2F(tipX, cy), D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(backX, cy - halfH));
                            sink->AddLine(D2D1::Point2F(notchX, cy));
                            sink->AddLine(D2D1::Point2F(backX, cy + halfH));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            m_renderTarget->FillGeometry(geo.Get(), brush);
                        }
                    }
                    shaftStartX = notchX;
                    break;
                }
                case LineCapStyle::SolidCircle:
                case LineCapStyle::HollowCircle: {
                    const float rad = 3.2f * scale;
                    const float ccx = tipX + dir * rad;
                    const auto ell = D2D1::Ellipse(D2D1::Point2F(ccx, cy), rad, rad);
                    if (capStyle == LineCapStyle::SolidCircle) {
                        m_renderTarget->FillEllipse(ell, brush);
                    } else {
                        m_renderTarget->DrawEllipse(ell, brush, 1.35f * scale);
                    }
                    shaftStartX = tipX + dir * (rad * 2.0f);
                    break;
                }
                case LineCapStyle::SolidDiamond:
                case LineCapStyle::HollowDiamond: {
                    const float dLen = 7.4f * scale;
                    const float midX = tipX + dir * (dLen * 0.5f);
                    const float backX = tipX + dir * dLen;
                    const float halfH = 3.6f * scale;
                    ComPtr<ID2D1PathGeometry> geo;
                    if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(D2D1::Point2F(tipX, cy), D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine(D2D1::Point2F(midX, cy - halfH));
                            sink->AddLine(D2D1::Point2F(backX, cy));
                            sink->AddLine(D2D1::Point2F(midX, cy + halfH));
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            if (capStyle == LineCapStyle::SolidDiamond) {
                                m_renderTarget->FillGeometry(geo.Get(), brush);
                            } else {
                                m_renderTarget->DrawGeometry(geo.Get(), brush, 1.35f * scale);
                            }
                        }
                    }
                    shaftStartX = backX;
                    break;
                }
                case LineCapStyle::BarStop: {
                    m_renderTarget->DrawLine(D2D1::Point2F(tipX, cy - 4.2f * scale),
                                             D2D1::Point2F(tipX, cy + 4.2f * scale), brush, lw);
                    break;
                }
            }
            m_renderTarget->DrawLine(D2D1::Point2F(shaftStartX, cy), D2D1::Point2F(tailX, cy), brush, lw);
            return;
        }
        case ToolbarCommand::ToggleSpotlightShape: {
            CaptureVectorIcons::renderIcon(
                m_renderTarget.Get(), m_d2dFactory.Get(),
                button.boolParam ? CaptureIconId::ToolEllipse : CaptureIconId::ToolRectangle,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleSpotlightDim: {
            drawStepperIconAndLabel(CaptureIconId::ToolSpotlight);
            return;
        }
        case ToolbarCommand::ToggleNumberFormatDropdown:
        case ToolbarCommand::ToggleMagnifierCalloutStyleDropdown: {
            if (!button.label.empty() && m_infoTextFormat) {
                const float textLeft = rect.left + 4.0f * scale;
                const float rightInset = button.hasDropdown ? 10.0f * scale : 3.0f * scale;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::ToggleTextOutline: {
            if (!button.label.empty()) {
                drawStepperIconAndLabel(CaptureIconId::PropTextOutline);
            } else {
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                    CaptureIconId::PropTextOutline, rect, brush, scale);
            }
            return;
        }
        case ToolbarCommand::ToggleLineStyleDropdown: {
            auto iconId = (button.lineStyleParam == LineStyle::Dashed) ? CaptureIconId::PropDashedLine
                        : (button.lineStyleParam == LineStyle::Dotted) ? CaptureIconId::PropDottedLine
                        : (button.lineStyleParam == LineStyle::DashDot) ? CaptureIconId::PropDashDotLine
                        : (button.lineStyleParam == LineStyle::Wavy) ? CaptureIconId::PropWavyLine
                        : CaptureIconId::PropSolidLine;
            auto lineRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, lineRect, brush, scale);
            return;
        }
        case ToolbarCommand::SelectTextMarkupMode: {
            const auto mode = static_cast<TextMarkupMode>(button.intParam);
            if (mode == TextMarkupMode::RectRegion) {
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                    CaptureIconId::ToolHighlight, rect, brush, scale);
                return;
            }
            const CaptureIconId iconId = (mode == TextMarkupMode::WavyUnderline)
                ? CaptureIconId::TextMarkupWavy
                : (mode == TextMarkupMode::StraightUnderline ? CaptureIconId::TextMarkupUnderline : CaptureIconId::TextMarkupHighlight);
            ComPtr<ID2D1SolidColorBrush> decBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f, button.color.b / 255.0f, 1.0f),
                decBrush.GetAddressOf());
            CaptureVectorIcons::renderIconWithAccent(
                m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect,
                brush, decBrush ? static_cast<ID2D1Brush*>(decBrush.Get()) : brush, scale);
            return;
        }
        case ToolbarCommand::ChooseCustomColor: {
            drawCustomColorIndicator(m_renderTarget.Get(), rect, button.color, scale, false);
            return;
        }
        case ToolbarCommand::ToggleArrowStyleDropdown: {
            auto iconId = (button.arrowStyleParam == ArrowStyle::DoubleEnded) ? CaptureIconId::ToolArrowDouble
                        : (button.arrowStyleParam == ArrowStyle::Thin) ? CaptureIconId::ToolArrowThin
                        : (button.arrowStyleParam == ArrowStyle::Tech) ? CaptureIconId::ToolArrowWing
                        : (button.arrowStyleParam == ArrowStyle::CampaignMarch) ? CaptureIconId::ToolArrowCampaign
                        : (button.arrowStyleParam == ArrowStyle::MarchDoubleTrack) ? CaptureIconId::ToolArrowDoubleTrack
                        : CaptureIconId::ToolArrowStandard;
            auto arrowRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, arrowRect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleStrokeWidth: {
            const auto stepperIcon = (button.tool == MarkupTool::Text)
                ? CaptureIconId::PropFontSize
                : CaptureIconId::PropStrokeWidth;
            drawStepperIconAndLabel(stepperIcon);
            return;
        }
        case ToolbarCommand::ToggleCornerRadius:
        case ToolbarCommand::CycleElementCornerRadius:
        case ToolbarCommand::CycleBeautyRadius: {
            drawStepperIconAndLabel(CaptureIconId::PropCornerRadius);
            return;
        }
        case ToolbarCommand::CycleBeautyPadding: {
            drawStepperIconAndLabel(CaptureIconId::PropPadding);
            return;
        }
        case ToolbarCommand::SelectMosaicType: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                button.intParam == 0 ? CaptureIconId::ToolMosaic : CaptureIconId::ToolBlur,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleBeautyShell: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolBeautyShell, rect, brush, scale);
            return;
        }
        case ToolbarCommand::SideCycleAspectRatio: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropAspectRatio, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleNumberShape: {
            CaptureIconId iconId;
            if (button.tool == MarkupTool::Magnifier) {
                iconId = (button.intParam == static_cast<int>(NumberBadgeShape::RoundedSquare))
                    ? CaptureIconId::PropMagnifierSquare
                    : CaptureIconId::ToolMagnifier;
            } else {
                iconId = (button.intParam == static_cast<int>(NumberBadgeShape::RoundedSquare))
                    ? CaptureIconId::PropNumberSquare
                    : CaptureIconId::ToolNumber;
            }
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleMagnifierScale: {
            if (!button.label.empty() && m_infoTextFormat) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), rect, brush);
            }
            return;
        }
        case ToolbarCommand::ToggleMagnifierCallout: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                CaptureIconId::PropMagnifierCallout, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ResetNumberCounter: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionResetNumber, rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleBeautyBg: {
            D2D1_COLOR_F c1{}, c2{}, c3{};
            getBeautyColors(static_cast<BeautyBackgroundType>(
                                std::clamp(button.intParam, 0, static_cast<int>(BeautyBackgroundType::COUNT) - 1)),
                            c1, c2, c3);
            const float dotR = 3.8f * scale;
            const float dotX = rect.left + 8.5f * scale;
            const float dotY = (rect.top + rect.bottom) * 0.5f;
            ComPtr<ID2D1LinearGradientBrush> dotGrad;
            ComPtr<ID2D1GradientStopCollection> stops;
            D2D1_GRADIENT_STOP gs[2] = {{0.0f, c1}, {1.0f, c3}};
            if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(gs, 2, stops.GetAddressOf())) &&
                SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(dotX - dotR, dotY - dotR),
                        D2D1::Point2F(dotX + dotR, dotY + dotR)),
                    stops.Get(), dotGrad.GetAddressOf()))) {
                m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), dotGrad.Get());
            }
            if (!button.label.empty() && m_infoTextFormat) {
                const float rightInset = button.hasDropdown ? 9.5f * scale : 3.0f * scale;
                const float textLeft = dotX + dotR + 3.5f * scale;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - rightInset), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::RecordToggleFormat:
        case ToolbarCommand::RecordCycleFps:
        case ToolbarCommand::RecordCycleQuality:
        case ToolbarCommand::RecordCycleClickEffect: {
            if (!button.label.empty() && m_infoTextFormat) {
                const float textLeft = rect.left + 4.0f * scale;
                const D2D1_RECT_F textRect = button.hasDropdown
                    ? D2D1::RectF(textLeft, rect.top, (std::max)(textLeft, rect.right - 9.5f * scale), rect.bottom)
                    : rect;
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(),
                    textRect,
                    brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::RecordStartConfirm: {
            if (!button.label.empty() && m_infoTextFormat) {
                const D2D1_SIZE_F tsz = measureText(button.label, m_infoTextFormat.Get());
                const float dotR = 3.3f * scale;
                const float haloR = 5.6f * scale;
                const float dotSpan = haloR * 2.0f;
                const float gap = 5.5f * scale;
                const float totalW = dotSpan + gap + tsz.width;
                const float startX = rect.left + ((rect.right - rect.left) - totalW) * 0.5f;
                const D2D1_POINT_2F dotPt = D2D1::Point2F(startX + haloR, cy);

                ComPtr<ID2D1SolidColorBrush> haloBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), haloBrush.GetAddressOf());
                if (haloBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(dotPt, haloR, haloR), haloBrush.Get());
                }
                if (brush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(dotPt, dotR, dotR), brush);
                }

                const float textLeft = startX + dotSpan + gap;
                const D2D1_RECT_F textRect = D2D1::RectF(
                    textLeft, rect.top, (std::max)(textLeft, rect.right - 4.0f * scale), rect.bottom);
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(), textRect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            return;
        }
        case ToolbarCommand::RecordToggleKeycast: {
            const float s = (std::min)(rect.right - rect.left, rect.bottom - rect.top) / 24.0f * 0.68f;
            const float stroke = (std::max)(1.6f, 1.8f * scale);
            auto p = [&](float x, float y) -> D2D1_POINT_2F {
                return D2D1::Point2F(cx + (x - 12.0f) * s, cy + (y - 12.0f) * s);
            };
            auto kbBox = D2D1::RectF(p(3, 7).x, p(3, 7).y, p(21, 17).x, p(21, 17).y);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(kbBox, 2.0f * s, 2.0f * s), brush, stroke);
            m_renderTarget->DrawLine(p(6, 11), p(8, 11), brush, stroke);
            m_renderTarget->DrawLine(p(11, 11), p(13, 11), brush, stroke);
            m_renderTarget->DrawLine(p(16, 11), p(18, 11), brush, stroke);
            m_renderTarget->DrawLine(p(8, 14), p(16, 14), brush, stroke);
            return;
        }
        case ToolbarCommand::RecordToggleSystemAudio: {
            CaptureVectorIcons::renderIcon(
                m_renderTarget.Get(), m_d2dFactory.Get(),
                button.boolParam ? CaptureIconId::ActionToggleSpeaker : CaptureIconId::ActionToggleSpeakerMuted,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::RecordToggleMicrophone: {
            CaptureVectorIcons::renderIcon(
                m_renderTarget.Get(), m_d2dFactory.Get(),
                button.boolParam ? CaptureIconId::ActionToggleMic : CaptureIconId::ActionToggleMicMuted,
                rect, brush, scale);
            // 当用户显式锁定了特定外接/耳机麦克风（非系统默认）时，在右上角点缀微晶指示圆点
            if (button.intParam == 1 && m_renderTarget) {
                ComPtr<ID2D1SolidColorBrush> pinDotBrush;
                const D2D1_COLOR_F dotCol = button.boolParam
                    ? D2D1::ColorF(0.06f, 0.72f, 0.50f, 0.96f)
                    : D2D1::ColorF(0.23f, 0.51f, 0.96f, 0.80f);
                if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(dotCol, pinDotBrush.GetAddressOf())) && pinDotBrush) {
                    const D2D1_POINT_2F dotPt = D2D1::Point2F(rect.right - 5.2f * scale, rect.top + 6.2f * scale);
                    m_renderTarget->FillEllipse(D2D1::Ellipse(dotPt, 2.0f * scale, 2.0f * scale), pinDotBrush.Get());
                }
            }
            return;
        }
        case ToolbarCommand::RecordSelectMicSource: {
            const float halfW = 3.1f * scale;
            const float halfH = 1.65f * scale;
            const bool expanded = button.boolParam;
            const D2D1_POINT_2F pLeft  = D2D1::Point2F(cx - halfW, expanded ? (cy + halfH) : (cy - halfH));
            const D2D1_POINT_2F pMid   = D2D1::Point2F(cx,         expanded ? (cy - halfH) : (cy + halfH));
            const D2D1_POINT_2F pRight = D2D1::Point2F(cx + halfW, expanded ? (cy + halfH) : (cy - halfH));
            ComPtr<ID2D1StrokeStyle> roundStroke;
            if (m_d2dFactory) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                    D2D1_LINE_JOIN_ROUND, 4.0f);
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
            }
            m_renderTarget->DrawLine(pLeft, pMid, brush, 1.55f * scale, roundStroke.Get());
            m_renderTarget->DrawLine(pMid, pRight, brush, 1.55f * scale, roundStroke.Get());
            return;
        }
        case ToolbarCommand::RecordToggleSettingsMenu: {
            const float outerR = 5.6f * scale;
            const float innerR = 2.45f * scale;
            const float strokeW = 1.65f * scale;
            m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), outerR, outerR), brush, strokeW);
            m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), innerR, innerR), brush, 1.35f * scale);
            for (int t = 0; t < 6; ++t) {
                const float ang = static_cast<float>(t) * (3.14159265f / 3.0f);
                const float cs = std::cos(ang);
                const float sn = std::sin(ang);
                m_renderTarget->DrawLine(
                    D2D1::Point2F(cx + cs * (outerR - 0.3f * scale), cy + sn * (outerR - 0.3f * scale)),
                    D2D1::Point2F(cx + cs * (outerR + 2.1f * scale), cy + sn * (outerR + 2.1f * scale)),
                    brush, 1.9f * scale);
            }
            return;
        }
        default: break;
    }
}

void CaptureRenderer::drawGlassPanel(const D2D1_RECT_F& rect, float radius, bool seeThrough) {
    (void)seeThrough;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), rect, radius, FloatingGlassBar::isEffectiveDarkMode());
}

void CaptureRenderer::drawFloatingToast(const CaptureState& state) {
    if (!m_renderTarget || state.loupeToastUntil == 0 || state.loupeToastMessage.empty()) {
        return;
    }
    const DWORD now = GetTickCount();
    if (now >= state.loupeToastUntil) {
        return;
    }

    const float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    const bool isDark = true;
    IDWriteTextFormat* toastFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    const std::wstring cleanMsg = std::wstring(
        tools3000::ui::ToastStyle::stripMachineStatusPrefix(state.loupeToastMessage));
    if (cleanMsg.empty()) {
        return;
    }

    // 1. 测量文本尺寸
    float textW = 140.0f * scale;
    float textH = 22.0f * scale;
    if (m_dwriteFactory && toastFmt) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            cleanMsg.c_str(),
            static_cast<UINT32>(cleanMsg.size()),
            toastFmt, 1400.0f * scale, 100.0f * scale, layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = metrics.widthIncludingTrailingWhitespace > metrics.width
                    ? metrics.widthIncludingTrailingWhitespace : metrics.width;
                textH = metrics.height;
            }
        }
    }

    // 2. 居中大气微晶浮岛几何布局
    const float padH = 22.0f * scale;
    const float padV = 10.0f * scale;
    const float dotZone = 18.0f * scale;
    const float toastW = (std::max)(textW + dotZone + padH * 2.0f, 148.0f * scale);
    const float toastH = (std::max)(textH + padV * 2.0f, 40.0f * scale);

    const auto rtSize = m_renderTarget->GetSize();
    D2D1_RECT_F monRect{}, workRect{};
    resolveMonitorBoundsInCanvas(
        static_cast<float>(state.currentCursor.x),
        static_cast<float>(state.currentCursor.y),
        rtSize, monRect, workRect);
    const D2D1_RECT_F hostRect =
        (workRect.right - workRect.left >= toastW + 24.0f * scale &&
         workRect.bottom - workRect.top >= toastH + 24.0f * scale)
        ? workRect
        : monRect;
    const float margin = 12.0f * scale;
    const float left = std::clamp(
        hostRect.left + ((hostRect.right - hostRect.left) - toastW) * 0.5f,
        hostRect.left + margin,
        (std::max)(hostRect.left + margin, hostRect.right - toastW - margin));
    float top = hostRect.top + 34.0f * scale;

    // 智能避让顶部尺寸胶囊、展开菜单与上移后的主/副工具栏，杜绝浮层 Toast 遮挡活动控件
    const bool hasSelNow = (state.state.load() == OverlayState::Selected ||
                            state.state.load() == OverlayState::Marking);
    auto pushDownIfOverlaps = [&](const D2D1_RECT_F& obs) {
        if (obs.right <= obs.left || obs.bottom <= obs.top) return;
        const bool xHit = (left < obs.right + 8.0f * scale) && (left + toastW > obs.left - 8.0f * scale);
        const bool yHit = (top < obs.bottom + 8.0f * scale) && (top + toastH > obs.top - 8.0f * scale);
        if (xHit && yHit) {
            top = obs.bottom + 10.0f * scale;
        }
    };
    if (hasSelNow) {
        for (int pass = 0; pass < 3; ++pass) {
            if (state.isLongImageMode()) pushDownIfOverlaps(state.longCanvas.topHeaderRect);
            pushDownIfOverlaps(state.sizeHudRect);
            if (state.isSizeMenuOpen) pushDownIfOverlaps(state.sizeMenuRect);
            pushDownIfOverlaps(state.primaryToolbarRect);
            pushDownIfOverlaps(state.secondaryToolbarRect);
            if (state.dropdownMenu.type != DropdownType::None) {
                pushDownIfOverlaps(state.dropdownMenu.menuRect);
                pushDownIfOverlaps(state.dropdownMenu.childMenuRect);
            }
            if (state.sliderPopup.type != SliderPopupType::None) pushDownIfOverlaps(state.sliderPopup.popupRect);
            if (state.openSubmenu != SubmenuType::None) pushDownIfOverlaps(state.openSubmenuRect);
        }
        const float hostH = hostRect.bottom - hostRect.top;
        if (top + toastH > hostRect.top + hostH * 0.68f) {
            top = (std::max)(hostRect.top + margin, hostRect.bottom - toastH - 42.0f * scale);
        }
    }
    top = std::clamp(
        top,
        hostRect.top + margin,
        (std::max)(hostRect.top + margin, hostRect.bottom - toastH - margin));
    const auto toastRect = D2D1::RectF(left, top, left + toastW, top + toastH);
    const float cornerRadius = 12.0f * scale;

    // 3. 淡出透明度计算 (最后 300ms 线性淡出)
    const DWORD remaining = state.loupeToastUntil - now;
    float alpha = 1.0f;
    if (remaining < 300) {
        alpha = static_cast<float>(remaining) / 300.0f;
    }

    // 4. 使用 Direct2D Layer 实现全局平滑透明度合成
    ComPtr<ID2D1Layer> layer;
    bool pushedLayer = false;
    if (alpha < 0.999f && m_d2dFactory) {
        if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
            m_renderTarget->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                    alpha, nullptr, D2D1_LAYER_OPTIONS_NONE),
                layer.Get());
            pushedLayer = true;
        }
    }

    // 5. 绘制微晶玻璃浮岛底板
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), toastRect, cornerRadius, isDark);

    // 6. 左侧翡翠绿发光状态灯 + 高对比加粗微晶文字
    const float midY = (toastRect.top + toastRect.bottom) * 0.5f;
    const float contentW = dotZone + textW;
    const float startX = toastRect.left + (toastW - contentW) * 0.5f;

    ComPtr<ID2D1SolidColorBrush> dotGlowBrush, dotBrush, textBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.26f), dotGlowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.85f, 0.60f, 1.0f), dotBrush.GetAddressOf());
    const auto textColor = isDark ? D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.98f)
                                  : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.98f);
    m_renderTarget->CreateSolidColorBrush(textColor, textBrush.GetAddressOf());

    const D2D1_POINT_2F dotCenter = D2D1::Point2F(startX + 5.0f * scale, midY);
    if (dotGlowBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(dotCenter, 7.5f * scale, 7.5f * scale), dotGlowBrush.Get());
    }
    if (dotBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(dotCenter, 4.0f * scale, 4.0f * scale), dotBrush.Get());
    }

    if (toastFmt && textBrush) {
        TextFormatScope scope(toastFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_renderTarget->DrawTextW(
            cleanMsg.c_str(),
            static_cast<UINT32>(cleanMsg.size()),
            toastFmt,
            D2D1::RectF(startX + dotZone, toastRect.top, toastRect.right - 8.0f * scale, toastRect.bottom),
            textBrush.Get()
        );
    }

    if (pushedLayer) {
        m_renderTarget->PopLayer();
    }

    if (m_hwnd && now < state.loupeToastUntil) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void CaptureRenderer::drawMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    if (!state.markupBaseReady || state.markup.elementCount() == 0 || !m_renderTarget) return;

    // 仅在标注内容变化时才重新合成并更新 4096px 分片 D2D 位图缓存，突破 16384px 单纹理上限且保持 60fps 零拷贝原地刷新
    if (m_markupCacheDirty || m_markupCacheTiles.empty()) {
        cv::Mat composite = state.markup.getCompositeImage(true);
        if (composite.empty()) return;

        cv::Mat bgra;
        if (composite.channels() == 3) {
            cv::cvtColor(composite, bgra, cv::COLOR_BGR2BGRA);
        } else if (composite.channels() == 4) {
            bgra = composite;
        } else {
            return;
        }

        if (!m_markupCacheTiles.empty() &&
            m_markupTilesW == bgra.cols &&
            m_markupTilesH == bgra.rows) {
            bool updatedInPlace = true;
            for (auto& tile : m_markupCacheTiles) {
                if (!tile.bitmap ||
                    tile.xOffset < 0 || tile.yOffset < 0 ||
                    tile.xOffset + tile.width > bgra.cols ||
                    tile.yOffset + tile.height > bgra.rows) {
                    updatedInPlace = false;
                    break;
                }
                cv::Mat slice = bgra(cv::Rect(tile.xOffset, tile.yOffset, tile.width, tile.height));
                if (!slice.isContinuous()) slice = slice.clone();
                if (FAILED(tile.bitmap->CopyFromMemory(nullptr, slice.data, static_cast<UINT32>(slice.step[0])))) {
                    updatedInPlace = false;
                    break;
                }
            }
            if (updatedInPlace) {
                m_markupCacheDirty = false;
            } else {
                m_markupCacheTiles.clear();
            }
        } else {
            m_markupCacheTiles.clear();
        }

        if (m_markupCacheTiles.empty()) {
            if (!rebuildBitmapTiles(bgra, D2D1_ALPHA_MODE_PREMULTIPLIED,
                                    m_markupCacheTiles, m_markupTilesW, m_markupTilesH)) {
                return;
            }
            m_markupCacheDirty = false;
        }
    }

    if (!m_markupCacheTiles.empty() && m_markupTilesW > 0 && m_markupTilesH > 0) {
        D2D1_RECT_F destRect = calculateMarkupDestRect(
            selectionRect, static_cast<UINT32>(m_markupTilesW), static_cast<UINT32>(m_markupTilesH));
        D2D1_RECT_F srcRect = calculateMarkupSrcRect(
            static_cast<UINT32>(m_markupTilesW), static_cast<UINT32>(m_markupTilesH));

        const float scale = (state.dpiScale > 0.0f ? state.dpiScale : 1.0f);
        float effectiveRadius = state.effectiveCornerRadius();
        float radius = effectiveRadius * (state.isLongImageMode() ? 1.0f : scale);
        const auto interpMode = (state.isLongImageMode() && std::abs(state.longCanvas.zoom - 1.0f) > 0.01f)
            ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
            : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

        if (radius > 0.5f && m_d2dFactory) {
            auto rounded = D2D1::RoundedRect(destRect, radius, radius);
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(rounded, clipGeo.GetAddressOf())) && clipGeo) {
                if (!m_markupClipLayer) {
                    m_renderTarget->CreateLayer(m_markupClipLayer.GetAddressOf());
                }
                if (m_markupClipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), m_markupClipLayer.Get());
                    drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
                    m_renderTarget->PopLayer();
                } else {
                    drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
                }
            } else {
                drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
            }
        } else {
            drawTiledRegion(m_markupCacheTiles, m_markupTilesW, m_markupTilesH, destRect, srcRect, interpMode);
        }
    }

    // Figma / CleanShot X 级交互：当处于激活态的元素为矩形或矩形聚光灯时，在 Direct2D 层叠加动态微晶圆角手柄与 [ ╭ R ] 胶囊反馈
    if (state.activeElement && state.activeElement->isActive &&
        (state.activeElement->tool == MarkupTool::Rectangle ||
         (state.activeElement->tool == MarkupTool::Spotlight && !state.activeElement->fill))) {
        const float uiScale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
        const float handleScale = state.isLongImageMode()
            ? (uiScale / (std::max)(0.15f, state.longCanvas.zoom))
            : uiScale;
        cv::Rect bbox = state.activeElement->getBoundingBox();
        float destLeft = std::round(selectionRect.left);
        float destTop  = std::round(selectionRect.top);
        D2D1_RECT_F elemRect = D2D1::RectF(
            destLeft + static_cast<float>(bbox.x),
            destTop  + static_cast<float>(bbox.y),
            destLeft + static_cast<float>(bbox.x + bbox.width),
            destTop  + static_cast<float>(bbox.y + bbox.height)
        );
        bool isDragging = (state.isManipulating && state.dragHandle == HitArea::CornerRadius);
        const POINT dragStartPt = state.isLongImageMode()
            ? state.screenToDocPoint(state.cornerDragStartPos)
            : state.cornerDragStartPos;
        const POINT cursorPt = state.isLongImageMode()
            ? state.screenToDocPoint(state.currentCursor)
            : state.currentCursor;
        drawCornerRadiusHandleVisual(
            elemRect,
            state.activeElement->cornerRadius,
            handleScale,
            isDragging,
            dragStartPt,
            cursorPt,
            40.0f,
            state.cornerDragIndex
        );
    }
}

void CaptureRenderer::drawActiveMarkupPreview(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    if (!state.isMarking || !m_renderTarget) return;

    float x1 = static_cast<float>(state.markupStart.x);
    float y1 = static_cast<float>(state.markupStart.y);
    float x2 = static_cast<float>(state.markupEnd.x);
    float y2 = static_cast<float>(state.markupEnd.y);

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        if (state.currentTool == MarkupTool::Rectangle || state.currentTool == MarkupTool::Ellipse ||
            state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Blur ||
            state.currentTool == MarkupTool::Highlight || state.currentTool == MarkupTool::Inpaint ||
            state.currentTool == MarkupTool::Spotlight || state.currentTool == MarkupTool::Watermark) {
            float w = std::abs(x2 - x1);
            float h = std::abs(y2 - y1);
            float side = std::max(w, h);
            x2 = x1 + (x2 >= x1 ? side : -side);
            y2 = y1 + (y2 >= y1 ? side : -side);
        } else if (state.currentTool == MarkupTool::Arrow || state.currentTool == MarkupTool::Line) {
            const float refX = (state.isDrawingMultiPointLine && !state.multiPointDocVertices.empty())
                ? static_cast<float>(state.multiPointDocVertices.back().x)
                : x1;
            const float refY = (state.isDrawingMultiPointLine && !state.multiPointDocVertices.empty())
                ? static_cast<float>(state.multiPointDocVertices.back().y)
                : y1;
            double dx = x2 - refX;
            double dy = y2 - refY;
            double dist = std::hypot(dx, dy);
            if (dist >= 1.0) {
                double angle = std::atan2(dy, dx);
                constexpr double step = 3.14159265358979323846 / 4.0;
                double snappedAngle = std::round(angle / step) * step;
                x2 = static_cast<float>(refX + dist * std::cos(snappedAngle));
                y2 = static_cast<float>(refY + dist * std::sin(snappedAngle));
            }
        }
    }

    auto rect = D2D1::RectF(std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
    const float strokeW = std::max(1.0f, static_cast<float>(state.currentStrokeWidth));
    ComPtr<ID2D1SolidColorBrush> activeColorBrush;
    const float liveAlpha = (state.currentColor.a > 0) ? (state.currentColor.a / 255.0f) : 1.0f;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(state.currentColor.r / 255.0f,
                     state.currentColor.g / 255.0f,
                     state.currentColor.b / 255.0f,
                     liveAlpha),
        activeColorBrush.GetAddressOf());
    ID2D1Brush* toolBrush = activeColorBrush ? activeColorBrush.Get() : m_borderBrush.Get();

    ComPtr<ID2D1StrokeStyle> customStrokeStyle;
    if (m_d2dFactory) {
        if (state.currentLineStyle != LineStyle::Solid) {
            D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_CUSTOM,
                0.0f);
            if (state.currentLineStyle == LineStyle::Dashed) {
                const float dashes[] = { 2.2f, 2.8f };
                m_d2dFactory->CreateStrokeStyle(ssp, dashes, 2, customStrokeStyle.GetAddressOf());
            } else if (state.currentLineStyle == LineStyle::Dotted) {
                const float dashes[] = { 0.20f, 2.4f };
                m_d2dFactory->CreateStrokeStyle(ssp, dashes, 2, customStrokeStyle.GetAddressOf());
            } else {
                const float dashes[] = { 2.2f, 2.6f, 0.20f, 2.6f };
                m_d2dFactory->CreateStrokeStyle(ssp, dashes, 4, customStrokeStyle.GetAddressOf());
            }
        } else {
            D2D1_STROKE_STYLE_PROPERTIES solidRoundProps = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 4.0f,
                D2D1_DASH_STYLE_SOLID,
                0.0f);
            m_d2dFactory->CreateStrokeStyle(solidRoundProps, nullptr, 0, customStrokeStyle.GetAddressOf());
        }
    }

    switch (state.currentTool) {
        case MarkupTool::Rectangle: {
            const float elemRad = std::max(0.0f, state.currentElementCornerRadius);
            ComPtr<ID2D1SolidColorBrush> borderOverrideBrush;
            ID2D1Brush* rectBorderBrush = toolBrush;
            if (state.currentFillMode && !state.currentTextOutlineColor.isAuto()) {
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(state.currentTextOutlineColor.r / 255.0f,
                                 state.currentTextOutlineColor.g / 255.0f,
                                 state.currentTextOutlineColor.b / 255.0f,
                                 (state.currentTextOutlineColor.a / 255.0f) * liveAlpha),
                    borderOverrideBrush.GetAddressOf());
                if (borderOverrideBrush) rectBorderBrush = borderOverrideBrush.Get();
            }
            const float rectBorderW = state.currentFillMode
                ? static_cast<float>(state.currentOutlineWidth)
                : strokeW;
            if (elemRad > 0.5f) {
                auto rrect = D2D1::RoundedRect(rect, elemRad, elemRad);
                if (state.currentFillMode) {
                    m_renderTarget->FillRoundedRectangle(rrect, toolBrush);
                }
                if (!state.currentFillMode || (rectBorderW > 0.05f && (!state.currentTextOutlineColor.isAuto() || state.currentOutlineWidth != 2))) {
                    m_renderTarget->DrawRoundedRectangle(rrect, rectBorderBrush, (std::max)(1.0f, rectBorderW), customStrokeStyle.Get());
                }
            } else {
                if (state.currentFillMode) {
                    m_renderTarget->FillRectangle(rect, toolBrush);
                }
                if (!state.currentFillMode || (rectBorderW > 0.05f && (!state.currentTextOutlineColor.isAuto() || state.currentOutlineWidth != 2))) {
                    m_renderTarget->DrawRectangle(rect, rectBorderBrush, (std::max)(1.0f, rectBorderW), customStrokeStyle.Get());
                }
            }
            break;
        }

        case MarkupTool::Mosaic:
        case MarkupTool::Blur:
        case MarkupTool::Inpaint:
        case MarkupTool::Watermark: {
            ComPtr<ID2D1SolidColorBrush> boxFill;
            m_renderTarget->CreateSolidColorBrush(
                (state.currentTool == MarkupTool::Inpaint)
                    ? D2D1::ColorF(0.16f, 0.78f, 0.56f, 0.16f)
                    : D2D1::ColorF(0.20f, 0.52f, 0.96f, 0.14f),
                boxFill.GetAddressOf());
            if (boxFill) m_renderTarget->FillRectangle(rect, boxFill.Get());
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 1.5f);
            break;
        }

        case MarkupTool::Spotlight: {
            const bool isEllipseSpot = state.currentSpotlightEllipse;
            const float elemRad = (!isEllipseSpot) ? std::max(0.0f, state.currentElementCornerRadius) : 0.0f;
            const float dimAlpha = std::clamp(state.currentSpotlightDimAlpha, 0.15f, 0.85f);

            bool hasExistingSpotlight = false;
            for (const auto& elem : state.markup.elements()) {
                if (elem && elem->tool == MarkupTool::Spotlight) {
                    hasExistingSpotlight = true;
                    break;
                }
            }

            if (m_d2dFactory && rect.right - rect.left >= 2.0f && rect.bottom - rect.top >= 2.0f) {
                ComPtr<ID2D1Geometry> litGeo;
                if (isEllipseSpot) {
                    ComPtr<ID2D1EllipseGeometry> ellGeo;
                    m_d2dFactory->CreateEllipseGeometry(
                        D2D1::Ellipse(D2D1::Point2F((x1 + x2) * 0.5f, (y1 + y2) * 0.5f),
                                      std::abs(x2 - x1) * 0.5f,
                                      std::abs(y2 - y1) * 0.5f),
                        ellGeo.GetAddressOf());
                    litGeo = ellGeo;
                } else if (elemRad > 0.5f) {
                    ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
                    m_d2dFactory->CreateRoundedRectangleGeometry(
                        D2D1::RoundedRect(rect, elemRad, elemRad), rrGeo.GetAddressOf());
                    litGeo = rrGeo;
                } else {
                    ComPtr<ID2D1RectangleGeometry> rGeo;
                    m_d2dFactory->CreateRectangleGeometry(rect, rGeo.GetAddressOf());
                    litGeo = rGeo;
                }

                if (!hasExistingSpotlight && litGeo) {
                    ComPtr<ID2D1RectangleGeometry> selGeo;
                    ComPtr<ID2D1PathGeometry> maskGeo;
                    if (SUCCEEDED(m_d2dFactory->CreateRectangleGeometry(selectionRect, selGeo.GetAddressOf())) && selGeo &&
                        SUCCEEDED(m_d2dFactory->CreatePathGeometry(maskGeo.GetAddressOf())) && maskGeo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(maskGeo->Open(sink.GetAddressOf())) && sink) {
                            selGeo->CombineWithGeometry(litGeo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
                            sink->Close();
                            ComPtr<ID2D1SolidColorBrush> dimBrush;
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(0.0f, 0.0f, 0.0f, dimAlpha), dimBrush.GetAddressOf());
                            if (dimBrush) {
                                m_renderTarget->FillGeometry(maskGeo.Get(), dimBrush.Get());
                            }
                        }
                    }
                } else if (hasExistingSpotlight && litGeo && (m_screenBitmap || !m_screenTiles.empty())) {
                    // 画布已有聚光灯时，在当前新亮区内实时回填原图，实现多聚光灯并集亮区预览（兼容超长图瓦片流）
                    ComPtr<ID2D1Layer> litLayer;
                    if (SUCCEEDED(m_renderTarget->CreateLayer(litLayer.GetAddressOf())) && litLayer) {
                        m_renderTarget->PushLayer(
                            D2D1::LayerParameters(rect, litGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                            litLayer.Get());
                        if (!m_screenTiles.empty()) {
                            drawTiledRegion(
                                m_screenTiles, m_screenTilesW, m_screenTilesH,
                                rect, rect,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        } else if (m_screenBitmap) {
                            auto bmpSz = m_screenBitmap->GetPixelSize();
                            m_renderTarget->DrawBitmap(
                                m_screenBitmap.Get(),
                                D2D1::RectF(0.0f, 0.0f, static_cast<float>(bmpSz.width), static_cast<float>(bmpSz.height)),
                                1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                        }
                        m_renderTarget->PopLayer();
                    }
                }
            }

            ComPtr<ID2D1SolidColorBrush> guideBrush;
            if (!state.currentSpotlightBorder) {
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), guideBrush.GetAddressOf());
            }
            ID2D1Brush* rimBrush = state.currentSpotlightBorder
                ? toolBrush
                : (guideBrush ? static_cast<ID2D1Brush*>(guideBrush.Get()) : static_cast<ID2D1Brush*>(m_borderBrush.Get()));
            const float rimW = state.currentSpotlightBorder ? strokeW : 1.5f;

            if (isEllipseSpot) {
                auto ell = D2D1::Ellipse(
                    D2D1::Point2F((x1 + x2) * 0.5f, (y1 + y2) * 0.5f),
                    std::abs(x2 - x1) * 0.5f,
                    std::abs(y2 - y1) * 0.5f);
                m_renderTarget->DrawEllipse(ell, rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            } else if (elemRad > 0.5f) {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(rect, elemRad, elemRad), rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            } else {
                m_renderTarget->DrawRectangle(rect, rimBrush, rimW,
                    state.currentSpotlightBorder ? customStrokeStyle.Get() : nullptr);
            }
            break;
        }

        case MarkupTool::Highlight: {
            auto drawD2dHandDrawnRibbon = [&](const HandDrawnRibbon& ribbon) {
                if (!m_d2dFactory || ribbon.outline.size() < 3) return;
                ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(geo->Open(sk.GetAddressOf())) && sk) {
                        sk->SetFillMode(D2D1_FILL_MODE_WINDING);
                        sk->BeginFigure(D2D1::Point2F(ribbon.outline[0].x, ribbon.outline[0].y), D2D1_FIGURE_BEGIN_FILLED);
                        for (size_t i = 1; i < ribbon.outline.size(); ++i) {
                            sk->AddLine(D2D1::Point2F(ribbon.outline[i].x, ribbon.outline[i].y));
                        }
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        m_renderTarget->FillGeometry(geo.Get(), toolBrush);
                    }
                }
            };

            if (state.isTextMarkupStreaming && !state.liveTextMarkupPreviewSpans.empty()) {
                ComPtr<ID2D1SolidColorBrush> highlightBrush;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(state.currentColor.r / 255.0f,
                                 state.currentColor.g / 255.0f,
                                 state.currentColor.b / 255.0f,
                                 0.38f),
                    highlightBrush.GetAddressOf());
                for (const auto& sp : state.liveTextMarkupPreviewSpans) {
                    if (sp.width <= 0 || sp.height <= 0) continue;
                    const float sx0 = selectionRect.left + static_cast<float>(sp.x);
                    const float sy0 = selectionRect.top + static_cast<float>(sp.y);
                    const float sx1 = sx0 + static_cast<float>(sp.width);
                    const float sy1 = sy0 + static_cast<float>(sp.height);
                    if (state.currentTextMarkupMode == TextMarkupMode::Highlight) {
                        const D2D1_RECT_F band = D2D1::RectF(sx0 - 1.8f, sy0 - 1.2f, sx1 + 1.8f, sy1 + 1.8f);
                        const float r = std::clamp(static_cast<float>(sp.height) * 0.16f, 2.2f, 4.2f);
                        if (highlightBrush) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(band, r, r), highlightBrush.Get());
                        }
                    } else if (state.currentTextMarkupMode == TextMarkupMode::WavyUnderline) {
                        float peerGap = 18.0f;
                        const bool hasBelow = HandDrawnStrokeGenerator::findNearestBelowSpanGap(
                            sp, state.liveTextMarkupPreviewSpans, peerGap);
                        const auto pl = HandDrawnStrokeGenerator::resolveAdaptiveWavyPlacement(
                            sy1, static_cast<float>(sp.height), peerGap, hasBelow, 0.0f);
                        drawD2dHandDrawnRibbon(
                            HandDrawnStrokeGenerator::buildWavyStroke(sx0, pl.baselineY, sx1, pl.baselineY,
                                                                      pl.strokeWidth, pl.amplitude, pl.halfWavelength, true));
                    } else {
                        float peerGap = 18.0f;
                        const bool hasBelow = HandDrawnStrokeGenerator::findNearestBelowSpanGap(
                            sp, state.liveTextMarkupPreviewSpans, peerGap);
                        const auto pl = HandDrawnStrokeGenerator::resolveAdaptiveStraightPlacement(
                            sy1, static_cast<float>(sp.height), peerGap, hasBelow, 0.0f);
                        drawD2dHandDrawnRibbon(
                            HandDrawnStrokeGenerator::buildStraightStroke(sx0, pl.baselineY, sx1, pl.baselineY,
                                                                          pl.strokeWidth, true, pl.straightBowMax));
                    }
                }
                break;
            }

            if (state.currentTextMarkupMode == TextMarkupMode::WavyUnderline) {
                const float sw = (std::max)(2.6f, static_cast<float>(state.currentStrokeWidth) * 1.08f);
                drawD2dHandDrawnRibbon(HandDrawnStrokeGenerator::buildWavyStroke(
                    x1, y1, x2, y2, sw,
                    std::clamp(sw * 1.15f + 1.4f, 2.8f, 9.5f),
                    std::clamp(sw * 3.8f + 7.5f, 13.5f, 32.0f), false));
                break;
            }
            if (state.currentTextMarkupMode == TextMarkupMode::StraightUnderline) {
                const float sw = (std::max)(3.0f, static_cast<float>(state.currentStrokeWidth) * 1.12f);
                drawD2dHandDrawnRibbon(HandDrawnStrokeGenerator::buildStraightStroke(x1, y1, x2, y2, sw, false));
                break;
            }

            ComPtr<ID2D1SolidColorBrush> highlightBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentColor.r / 255.0f,
                             state.currentColor.g / 255.0f,
                             state.currentColor.b / 255.0f,
                             0.35f),
                highlightBrush.GetAddressOf());
            if (highlightBrush) m_renderTarget->FillRectangle(rect, highlightBrush.Get());
            m_renderTarget->DrawRectangle(rect, toolBrush, 1.0f);
            break;
        }

        case MarkupTool::Line:
        case MarkupTool::Arrow: {
            const bool isMultiLinePreview =
                (state.currentTool == MarkupTool::Line &&
                 state.isDrawingMultiPointLine &&
                 !state.multiPointDocVertices.empty());
            const float dpiSnapScale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            bool snappedCloseLoop = false;
            if (isMultiLinePreview && state.multiPointDocVertices.size() >= 3) {
                const float startX = static_cast<float>(state.multiPointDocVertices.front().x);
                const float startY = static_cast<float>(state.multiPointDocVertices.front().y);
                if (std::hypot(x2 - startX, y2 - startY) <= 10.0f * dpiSnapScale) {
                    x2 = startX;
                    y2 = startY;
                    snappedCloseLoop = true;
                }
            }

            float dist = isMultiLinePreview ? 10.0f : std::hypot(x2 - x1, y2 - y1);
            if (dist >= 2.0f) {
                auto evalCatmullRom = [](const std::vector<D2D1_POINT_2F>& ctrl, int steps) -> std::vector<D2D1_POINT_2F> {
                    if (ctrl.size() < 3) return ctrl;
                    std::vector<D2D1_POINT_2F> ext(ctrl.size() + 2);
                    ext[0] = D2D1::Point2F(2.0f * ctrl[0].x - ctrl[1].x, 2.0f * ctrl[0].y - ctrl[1].y);
                    for (size_t i = 0; i < ctrl.size(); ++i) ext[i + 1] = ctrl[i];
                    ext.back() = D2D1::Point2F(2.0f * ctrl.back().x - ctrl[ctrl.size() - 2].x,
                                               2.0f * ctrl.back().y - ctrl[ctrl.size() - 2].y);
                    std::vector<D2D1_POINT_2F> out;
                    out.reserve((ctrl.size() - 1) * steps + 1);
                    for (size_t i = 1; i < ext.size() - 2; ++i) {
                        const auto& p0 = ext[i - 1];
                        const auto& p1 = ext[i];
                        const auto& p2 = ext[i + 1];
                        const auto& p3 = ext[i + 2];
                        const int limit = (i == ext.size() - 3) ? (steps + 1) : steps;
                        for (int s = 0; s < limit; ++s) {
                            float t = static_cast<float>(s) / static_cast<float>(steps);
                            float t2 = t * t;
                            float t3 = t2 * t;
                            float px = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                                               (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                                               (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
                            float py = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                                               (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                                               (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
                            out.push_back(D2D1::Point2F(px, py));
                        }
                    }
                    return out;
                };

                std::vector<D2D1_POINT_2F> pathPts;
                if (isMultiLinePreview) {
                    std::vector<FloatPoint2D> rawAnchors;
                    rawAnchors.reserve(state.multiPointDocVertices.size() + 1);
                    for (const auto& v : state.multiPointDocVertices) {
                        rawAnchors.push_back({static_cast<float>(v.x), static_cast<float>(v.y)});
                    }
                    if (std::hypot(x2 - rawAnchors.back().x, y2 - rawAnchors.back().y) >= 1.0f) {
                        rawAnchors.push_back({x2, y2});
                    }
                    if (state.currentSmoothCurve && rawAnchors.size() >= 2) {
                        const auto sampled = sampleCentripetalCatmullRom(rawAnchors, 3.5f);
                        pathPts.reserve(sampled.size());
                        for (const auto& sp : sampled) {
                            pathPts.push_back(D2D1::Point2F(sp.x, sp.y));
                        }
                    } else {
                        pathPts.reserve(rawAnchors.size());
                        for (const auto& sp : rawAnchors) {
                            pathPts.push_back(D2D1::Point2F(sp.x, sp.y));
                        }
                    }
                } else if (state.currentSmoothCurve && dist >= 6.0f) {
                    const float dx = x2 - x1;
                    const float dy = y2 - y1;
                    if (state.currentTool == MarkupTool::Arrow) {
                        float nx = -dy / dist;
                        float ny = dx / dist;
                        if (ny > 0.0f) {
                            nx = -nx;
                            ny = -ny;
                        }
                        const float arch = std::clamp(dist * 0.18f, 8.0f, 48.0f);
                        const float midX = (x1 + x2) * 0.5f + nx * arch;
                        const float midY = (y1 + y2) * 0.5f + ny * arch;
                        const float ctrlX = 2.0f * midX - 0.5f * (x1 + x2);
                        const float ctrlY = 2.0f * midY - 0.5f * (y1 + y2);
                        const int bSteps = 28;
                        pathPts.reserve(bSteps + 1);
                        for (int i = 0; i <= bSteps; ++i) {
                            const float t = static_cast<float>(i) / static_cast<float>(bSteps);
                            const float u = 1.0f - t;
                            pathPts.push_back(D2D1::Point2F(
                                u * u * x1 + 2.0f * u * t * ctrlX + t * t * x2,
                                u * u * y1 + 2.0f * u * t * ctrlY + t * t * y2));
                        }
                    } else {
                        const float nx = -dy / dist;
                        const float ny = dx / dist;
                        const float arch = std::clamp(dist * 0.14f, 4.0f, 32.0f);
                        D2D1_POINT_2F c1 = D2D1::Point2F(x1 + dx * 0.33f + nx * arch, y1 + dy * 0.33f + ny * arch);
                        D2D1_POINT_2F c2 = D2D1::Point2F(x1 + dx * 0.67f - nx * arch, y1 + dy * 0.67f - ny * arch);
                        pathPts = evalCatmullRom({ D2D1::Point2F(x1, y1), c1, c2, D2D1::Point2F(x2, y2) }, 10);
                    }
                } else {
                    pathPts = { D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2) };
                }

                auto drawMultiPointAnchorDots = [&]() {
                    if (!isMultiLinePreview) return;
                    ComPtr<ID2D1SolidColorBrush> dotCoreBrush;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), dotCoreBrush.GetAddressOf());
                    const float dotR = 3.6f * dpiSnapScale;
                    for (size_t idx = 0; idx < state.multiPointDocVertices.size(); ++idx) {
                        const D2D1_POINT_2F vp = D2D1::Point2F(
                            static_cast<float>(state.multiPointDocVertices[idx].x),
                            static_cast<float>(state.multiPointDocVertices[idx].y));
                        const float curR = (idx == 0 && snappedCloseLoop) ? (dotR * 1.45f) : dotR;
                        if (dotCoreBrush) {
                            m_renderTarget->FillEllipse(D2D1::Ellipse(vp, curR, curR), dotCoreBrush.Get());
                        }
                        m_renderTarget->DrawEllipse(D2D1::Ellipse(vp, curR, curR), toolBrush, 1.5f * dpiSnapScale);
                    }
                };

                auto drawPolylineD2D = [&](const std::vector<D2D1_POINT_2F>& pts) {
                    if (pts.size() < 2) return;
                    if (pts.size() == 2) {
                        m_renderTarget->DrawLine(pts[0], pts[1], toolBrush, strokeW, customStrokeStyle.Get());
                        return;
                    }
                    if (!m_d2dFactory) return;
                    ComPtr<ID2D1PathGeometry> geo;
                    if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                            for (size_t i = 1; i < pts.size(); ++i) {
                                sink->AddLine(pts[i]);
                            }
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            m_renderTarget->DrawGeometry(geo.Get(), toolBrush, strokeW, customStrokeStyle.Get());
                        }
                    }
                };

                const bool useCaps = (state.currentTool == MarkupTool::Arrow) || state.currentLineArrowEnabled;
                if (!useCaps) {
                    if (pathPts.size() >= 2) {
                        drawPolylineD2D(pathPts);
                    }
                    drawMultiPointAnchorDots();
                    break;
                }
                if (pathPts.size() < 2) {
                    drawMultiPointAnchorDots();
                    break;
                }

                // 历史课本地图经典行军路线箭头实时预览（战役立体阴阳浮雕大箭头 / 长征路线起点圆环+连续流向标箭头）
                if (state.currentTool == MarkupTool::Arrow &&
                    (state.currentArrowStyle == ArrowStyle::CampaignMarch ||
                     state.currentArrowStyle == ArrowStyle::MarchDoubleTrack) &&
                    m_d2dFactory && dist >= 4.0f) {
                    std::vector<D2D1_POINT_2F> spinePts = pathPts;
                    if (spinePts.size() == 2) {
                        const int segs = 24;
                        std::vector<D2D1_POINT_2F> sub;
                        sub.reserve(segs + 1);
                        for (int i = 0; i <= segs; ++i) {
                            const float t = static_cast<float>(i) / static_cast<float>(segs);
                            sub.push_back(D2D1::Point2F(
                                spinePts[0].x + (spinePts[1].x - spinePts[0].x) * t,
                                spinePts[0].y + (spinePts[1].y - spinePts[0].y) * t));
                        }
                        spinePts = std::move(sub);
                    }
                    std::vector<float> cum(spinePts.size(), 0.0f);
                    for (size_t i = 1; i < spinePts.size(); ++i) {
                        cum[i] = cum[i - 1] + std::hypot(spinePts[i].x - spinePts[i - 1].x, spinePts[i].y - spinePts[i - 1].y);
                    }
                    const float totL = cum.back();
                    if (totL >= 4.0f) {
                        const bool isCampaign = (state.currentArrowStyle == ArrowStyle::CampaignMarch);
                        const float effThick = std::max(2.0f, strokeW);

                        auto interpPt = [&](float dVal) -> D2D1_POINT_2F {
                            dVal = std::clamp(dVal, 0.0f, totL);
                            for (size_t i = 1; i < spinePts.size(); ++i) {
                                if (cum[i] >= dVal || i == spinePts.size() - 1) {
                                    const float sl = cum[i] - cum[i - 1];
                                    const float rt = (sl > 1e-4f) ? std::clamp((dVal - cum[i - 1]) / sl, 0.0f, 1.0f) : 0.0f;
                                    return D2D1::Point2F(spinePts[i - 1].x + (spinePts[i].x - spinePts[i - 1].x) * rt,
                                                         spinePts[i - 1].y + (spinePts[i].y - spinePts[i - 1].y) * rt);
                                }
                            }
                            return spinePts.back();
                        };
                        auto sampleSpine = [&](float dVal, D2D1_POINT_2F& oPt, D2D1_POINT_2F& oTan) {
                            oPt = interpPt(dVal);
                            const D2D1_POINT_2F pB = interpPt(std::max(0.0f, dVal - 4.0f));
                            const D2D1_POINT_2F pA = interpPt(std::min(totL, dVal + 4.0f));
                            const float tx = pA.x - pB.x;
                            const float ty = pA.y - pB.y;
                            const float tn = std::hypot(tx, ty);
                            oTan = (tn > 1e-4f) ? D2D1::Point2F(tx / tn, ty / tn) : D2D1::Point2F(1.0f, 0.0f);
                        };

                        auto fillPolyD2D = [&](const std::vector<D2D1_POINT_2F>& poly, ID2D1Brush* br) {
                            if (poly.size() < 3 || !br) return;
                            ComPtr<ID2D1PathGeometry> g;
                            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(g.GetAddressOf())) && g) {
                                ComPtr<ID2D1GeometrySink> s;
                                if (SUCCEEDED(g->Open(s.GetAddressOf())) && s) {
                                    s->BeginFigure(poly[0], D2D1_FIGURE_BEGIN_FILLED);
                                    for (size_t i = 1; i < poly.size(); ++i) s->AddLine(poly[i]);
                                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                                    s->Close();
                                    m_renderTarget->FillGeometry(g.Get(), br);
                                }
                            }
                        };
                        auto strokeOpenPolyD2D = [&](const std::vector<D2D1_POINT_2F>& poly, ID2D1Brush* br, float w) {
                            if (poly.size() < 2 || !br) return;
                            ComPtr<ID2D1PathGeometry> g;
                            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(g.GetAddressOf())) && g) {
                                ComPtr<ID2D1GeometrySink> s;
                                if (SUCCEEDED(g->Open(s.GetAddressOf())) && s) {
                                    s->BeginFigure(poly[0], D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (size_t i = 1; i < poly.size(); ++i) s->AddLine(poly[i]);
                                    s->EndFigure(D2D1_FIGURE_END_OPEN);
                                    s->Close();
                                    m_renderTarget->DrawGeometry(g.Get(), br, w);
                                }
                            }
                        };

                        const float rf = state.currentColor.r / 255.0f;
                        const float gf = state.currentColor.g / 255.0f;
                        const float bf = state.currentColor.b / 255.0f;
                        const float lum = 0.299f * rf + 0.587f * gf + 0.114f * bf;

                        ComPtr<ID2D1SolidColorBrush> borderBr, litBr, shadeBr, ridgeBr, corridorBr, whiteBr;
                        if (!state.currentTextOutlineColor.isAuto()) {
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(state.currentTextOutlineColor.r / 255.0f,
                                             state.currentTextOutlineColor.g / 255.0f,
                                             state.currentTextOutlineColor.b / 255.0f, liveAlpha),
                                borderBr.GetAddressOf());
                        } else {
                            m_renderTarget->CreateSolidColorBrush(
                                lum > 0.78f ? D2D1::ColorF(12 / 255.0f, 20 / 255.0f, 36 / 255.0f, liveAlpha)
                                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, liveAlpha),
                                borderBr.GetAddressOf());
                        }
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(std::min(1.0f, rf + (1.0f - rf) * 0.24f),
                                         std::min(1.0f, gf + (1.0f - gf) * 0.24f),
                                         std::min(1.0f, bf + (1.0f - bf) * 0.24f), liveAlpha),
                            litBr.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(rf * 0.70f, gf * 0.70f, bf * 0.70f, liveAlpha),
                            shadeBr.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(std::min(1.0f, rf + (1.0f - rf) * 0.55f),
                                         std::min(1.0f, gf + (1.0f - gf) * 0.55f),
                                         std::min(1.0f, bf + (1.0f - bf) * 0.55f), liveAlpha),
                            ridgeBr.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(0.988f * 0.80f + rf * 0.20f,
                                         0.980f * 0.80f + gf * 0.20f,
                                         0.972f * 0.80f + bf * 0.20f, liveAlpha),
                            corridorBr.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, liveAlpha), whiteBr.GetAddressOf());

                        if (isCampaign) {
                            const float hLen = std::clamp(std::max(26.0f, effThick * 5.8f), 16.0f, totL * 0.45f);
                            const float hWingHalfW = hLen * 0.60f;
                            const float neckBack = hLen * 0.72f;
                            const float shaftEnd = std::max(2.0f, totL - neckBack);
                            const float neckHalfW = std::max(3.4f, hWingHalfW * 0.42f);
                            const float tailHalfW = std::max(1.6f, neckHalfW * 0.28f);
                            const float bOut = static_cast<float>(state.currentOutlineWidth) * 0.75f;

                            const int sSteps = 24;
                            std::vector<D2D1_POINT_2F> sTrack, lTrack, rTrack, lOuter, rOuter;
                            sTrack.reserve(sSteps + 1);
                            lTrack.reserve(sSteps + 1);
                            rTrack.reserve(sSteps + 1);
                            lOuter.reserve(sSteps + 1);
                            rOuter.reserve(sSteps + 1);
                            for (int i = 0; i <= sSteps; ++i) {
                                const float u = static_cast<float>(i) / static_cast<float>(sSteps);
                                D2D1_POINT_2F sp, tanV;
                                sampleSpine(u * shaftEnd, sp, tanV);
                                const D2D1_POINT_2F normV = D2D1::Point2F(-tanV.y, tanV.x);
                                const float hw = tailHalfW + (neckHalfW - tailHalfW) * std::pow(u, 1.15f);
                                const float ow = hw + bOut;
                                const D2D1_POINT_2F bShift = (i == 0) ? D2D1::Point2F(-tanV.x * bOut, -tanV.y * bOut) : D2D1::Point2F(0.0f, 0.0f);
                                sTrack.push_back(sp);
                                lTrack.push_back(D2D1::Point2F(sp.x + normV.x * hw, sp.y + normV.y * hw));
                                rTrack.push_back(D2D1::Point2F(sp.x - normV.x * hw, sp.y - normV.y * hw));
                                lOuter.push_back(D2D1::Point2F(sp.x + normV.x * ow + bShift.x, sp.y + normV.y * ow + bShift.y));
                                rOuter.push_back(D2D1::Point2F(sp.x - normV.x * ow + bShift.x, sp.y - normV.y * ow + bShift.y));
                            }

                            const D2D1_POINT_2F tipPt = spinePts.back();
                            D2D1_POINT_2F endRef, endTan;
                            sampleSpine(std::max(0.0f, totL - std::min(hLen * 0.45f, totL * 0.22f)), endRef, endTan);
                            const float tdx = tipPt.x - endRef.x;
                            const float tdy = tipPt.y - endRef.y;
                            const float tdn = std::hypot(tdx, tdy);
                            if (tdn > 1e-3f) endTan = D2D1::Point2F(tdx / tdn, tdy / tdn);
                            const D2D1_POINT_2F endNorm = D2D1::Point2F(-endTan.y, endTan.x);
                            const D2D1_POINT_2F wingBase = D2D1::Point2F(tipPt.x - endTan.x * hLen, tipPt.y - endTan.y * hLen);
                            const D2D1_POINT_2F lWing = D2D1::Point2F(wingBase.x + endNorm.x * hWingHalfW, wingBase.y + endNorm.y * hWingHalfW);
                            const D2D1_POINT_2F rWing = D2D1::Point2F(wingBase.x - endNorm.x * hWingHalfW, wingBase.y - endNorm.y * hWingHalfW);

                            const D2D1_POINT_2F oTip = D2D1::Point2F(tipPt.x + endTan.x * (bOut * 1.65f), tipPt.y + endTan.y * (bOut * 1.65f));
                            const D2D1_POINT_2F oLW = D2D1::Point2F(lWing.x + (endNorm.x - endTan.x * 0.45f) * (bOut * 1.35f),
                                                                    lWing.y + (endNorm.y - endTan.y * 0.45f) * (bOut * 1.35f));
                            const D2D1_POINT_2F oRW = D2D1::Point2F(rWing.x + (-endNorm.x - endTan.x * 0.45f) * (bOut * 1.35f),
                                                                    rWing.y + (-endNorm.y - endTan.y * 0.45f) * (bOut * 1.35f));

                            D2D1_POINT_2F startPt, startTan;
                            sampleSpine(0.0f, startPt, startTan);
                            const float notchDepth = std::min(shaftEnd * 0.22f, std::max(4.8f, tailHalfW * 2.25f));
                            const D2D1_POINT_2F notchPt = D2D1::Point2F(startPt.x + startTan.x * notchDepth, startPt.y + startTan.y * notchDepth);
                            const D2D1_POINT_2F oNotch = D2D1::Point2F(notchPt.x - startTan.x * (bOut * 1.1f), notchPt.y - startTan.y * (bOut * 1.1f));

                            if (bOut > 0.05f) {
                                std::vector<D2D1_POINT_2F> outerPoly = lOuter;
                                outerPoly.push_back(oLW);
                                outerPoly.push_back(oTip);
                                outerPoly.push_back(oRW);
                                for (int i = static_cast<int>(rOuter.size()) - 1; i >= 0; --i) outerPoly.push_back(rOuter[static_cast<size_t>(i)]);
                                outerPoly.push_back(oNotch);
                                fillPolyD2D(outerPoly, borderBr.Get());
                            }

                            std::vector<D2D1_POINT_2F> leftHalf = { notchPt };
                            for (const auto& p : lTrack) leftHalf.push_back(p);
                            leftHalf.push_back(lWing);
                            leftHalf.push_back(tipPt);
                            for (int i = static_cast<int>(sTrack.size()) - 1; i >= 1; --i) leftHalf.push_back(sTrack[static_cast<size_t>(i)]);
                            fillPolyD2D(leftHalf, litBr.Get());

                            std::vector<D2D1_POINT_2F> rightHalf = { notchPt };
                            for (size_t i = 1; i < sTrack.size(); ++i) rightHalf.push_back(sTrack[i]);
                            rightHalf.push_back(tipPt);
                            rightHalf.push_back(rWing);
                            for (int i = static_cast<int>(rTrack.size()) - 1; i >= 0; --i) rightHalf.push_back(rTrack[static_cast<size_t>(i)]);
                            fillPolyD2D(rightHalf, shadeBr.Get());

                            std::vector<D2D1_POINT_2F> ridgePts = { notchPt };
                            for (size_t i = 1; i < sTrack.size(); ++i) ridgePts.push_back(sTrack[i]);
                            ridgePts.push_back(tipPt);
                            strokeOpenPolyD2D(ridgePts, ridgeBr.Get(), 1.1f);
                        } else {
                            const float trackHalfW = std::clamp(effThick * 0.78f + 2.2f, 3.6f, 8.8f);
                            const float hLen = std::clamp(std::max(24.0f, trackHalfW * 4.2f), 14.0f, totL * 0.42f);
                            const float hWingHalfW = hLen * 0.58f;
                            const float neckBack = hLen * 0.72f;
                            const float shaftEnd = std::max(2.0f, totL - neckBack);

                            const int sSteps = 24;
                            std::vector<D2D1_POINT_2F> lTrack, rTrack;
                            lTrack.reserve(sSteps + 1);
                            rTrack.reserve(sSteps + 1);
                            for (int i = 0; i <= sSteps; ++i) {
                                const float u = static_cast<float>(i) / static_cast<float>(sSteps);
                                D2D1_POINT_2F sp, tanV;
                                sampleSpine(u * shaftEnd, sp, tanV);
                                const D2D1_POINT_2F normV = D2D1::Point2F(-tanV.y, tanV.x);
                                lTrack.push_back(D2D1::Point2F(sp.x + normV.x * trackHalfW, sp.y + normV.y * trackHalfW));
                                rTrack.push_back(D2D1::Point2F(sp.x - normV.x * trackHalfW, sp.y - normV.y * trackHalfW));
                            }

                            std::vector<D2D1_POINT_2F> corridorPoly = lTrack;
                            for (int i = static_cast<int>(rTrack.size()) - 1; i >= 0; --i) corridorPoly.push_back(rTrack[static_cast<size_t>(i)]);
                            fillPolyD2D(corridorPoly, corridorBr.Get());

                            const float railW = std::clamp(effThick * 0.46f, 1.8f, 3.4f);
                            const float bOut = static_cast<float>(state.currentOutlineWidth) * 0.75f;
                            if (bOut > 0.05f) {
                                strokeOpenPolyD2D(lTrack, borderBr.Get(), railW + bOut * 1.2f);
                                strokeOpenPolyD2D(rTrack, borderBr.Get(), railW + bOut * 1.2f);
                            }
                            strokeOpenPolyD2D(lTrack, toolBrush, railW);
                            strokeOpenPolyD2D(rTrack, toolBrush, railW);

                            const float originR = std::clamp(trackHalfW * 1.35f, 5.0f, 12.0f);
                            const float chevLen = std::clamp(trackHalfW * 1.65f, 6.0f, 14.0f);
                            const float chevWingW = trackHalfW * 0.64f;
                            const float stepDist = std::max(26.0f, trackHalfW * 5.2f);
                            const float startMargin = originR + chevLen * 0.9f;
                            const float endMargin = hLen * 0.45f;
                            for (float d = startMargin + stepDist * 0.45f; d + chevLen * 0.5f < shaftEnd - endMargin; d += stepDist) {
                                D2D1_POINT_2F cPt, cTan;
                                sampleSpine(d, cPt, cTan);
                                const D2D1_POINT_2F cNorm = D2D1::Point2F(-cTan.y, cTan.x);
                                const D2D1_POINT_2F cTip = D2D1::Point2F(cPt.x + cTan.x * (chevLen * 0.55f), cPt.y + cTan.y * (chevLen * 0.55f));
                                const D2D1_POINT_2F cBase = D2D1::Point2F(cPt.x - cTan.x * (chevLen * 0.45f), cPt.y - cTan.y * (chevLen * 0.45f));
                                const D2D1_POINT_2F cNotch = D2D1::Point2F(cPt.x - cTan.x * (chevLen * 0.18f), cPt.y - cTan.y * (chevLen * 0.18f));
                                const D2D1_POINT_2F cLW = D2D1::Point2F(cBase.x + cNorm.x * chevWingW, cBase.y + cNorm.y * chevWingW);
                                const D2D1_POINT_2F cRW = D2D1::Point2F(cBase.x - cNorm.x * chevWingW, cBase.y - cNorm.y * chevWingW);
                                fillPolyD2D({ cTip, cLW, cNotch, cRW }, toolBrush);
                            }

                            const D2D1_POINT_2F startCenter = spinePts.front();
                            if (borderBr) m_renderTarget->FillEllipse(D2D1::Ellipse(startCenter, originR + 1.4f, originR + 1.4f), borderBr.Get());
                            m_renderTarget->FillEllipse(D2D1::Ellipse(startCenter, originR, originR), toolBrush);
                            if (whiteBr) m_renderTarget->FillEllipse(D2D1::Ellipse(startCenter, originR * 0.62f, originR * 0.62f), whiteBr.Get());
                            m_renderTarget->FillEllipse(D2D1::Ellipse(startCenter, originR * 0.32f, originR * 0.32f), toolBrush);

                            const D2D1_POINT_2F tipPt = spinePts.back();
                            D2D1_POINT_2F endRef, endTan;
                            sampleSpine(std::max(0.0f, totL - std::min(hLen * 0.45f, totL * 0.22f)), endRef, endTan);
                            const float tdx = tipPt.x - endRef.x;
                            const float tdy = tipPt.y - endRef.y;
                            const float tdn = std::hypot(tdx, tdy);
                            if (tdn > 1e-3f) endTan = D2D1::Point2F(tdx / tdn, tdy / tdn);
                            const D2D1_POINT_2F endNorm = D2D1::Point2F(-endTan.y, endTan.x);
                            const D2D1_POINT_2F wingBase = D2D1::Point2F(tipPt.x - endTan.x * hLen, tipPt.y - endTan.y * hLen);
                            const D2D1_POINT_2F hNotch = D2D1::Point2F(tipPt.x - endTan.x * (hLen * 0.72f), tipPt.y - endTan.y * (hLen * 0.72f));
                            const D2D1_POINT_2F lWing = D2D1::Point2F(wingBase.x + endNorm.x * hWingHalfW, wingBase.y + endNorm.y * hWingHalfW);
                            const D2D1_POINT_2F rWing = D2D1::Point2F(wingBase.x - endNorm.x * hWingHalfW, wingBase.y - endNorm.y * hWingHalfW);

                            const D2D1_POINT_2F oTip = D2D1::Point2F(tipPt.x + endTan.x * (bOut * 1.6f), tipPt.y + endTan.y * (bOut * 1.6f));
                            const D2D1_POINT_2F oLW = D2D1::Point2F(lWing.x + (endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                                                    lWing.y + (endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
                            const D2D1_POINT_2F oRW = D2D1::Point2F(rWing.x + (-endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                                                    rWing.y + (-endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
                            const D2D1_POINT_2F oNotch = D2D1::Point2F(hNotch.x - endTan.x * bOut, hNotch.y - endTan.y * bOut);
                            if (bOut > 0.05f) {
                                fillPolyD2D({ oTip, oLW, oNotch, oRW }, borderBr.Get());
                            }
                            fillPolyD2D({ tipPt, lWing, hNotch }, litBr.Get());
                            fillPolyD2D({ tipPt, hNotch, rWing }, shadeBr.Get());
                        }
                    }
                    break;
                }

                double totalLen = 0.0;
                for (size_t i = 1; i < pathPts.size(); ++i) {
                    totalLen += std::hypot(pathPts[i].x - pathPts[i - 1].x, pathPts[i].y - pathPts[i - 1].y);
                }
                const bool dualCaps = (state.currentStartCap != LineCapStyle::None && state.currentEndCap != LineCapStyle::None);
                const double maxHeadLen = dualCaps
                    ? (std::max)(4.0, (totalLen - static_cast<double>(strokeW) * 1.2) * 0.42)
                    : (totalLen * 0.45);
                double headLen = (std::max)(12.0, static_cast<double>(strokeW) * 3.5);
                if (maxHeadLen > 4.0 && headLen > maxHeadLen) headLen = maxHeadLen;
                const double lookbackDist = (std::min)(maxHeadLen, (std::max)(10.0, static_cast<double>(strokeW) * 3.0));
                ID2D1Brush* aBrush = toolBrush;

                auto drawCapPreview = [&](float tipX, float tipY, double a, LineCapStyle capStyle, float& outTrim) {
                    outTrim = 0.0f;
                    if (capStyle == LineCapStyle::None) return;
                    double cA = std::cos(a);
                    double sA = std::sin(a);
                    double nX = -sA;
                    double nY = cA;
                    D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                    const float outlineW = (std::max)(1.0f, strokeW * 0.85f);

                    switch (capStyle) {
                        case LineCapStyle::None:
                            break;
                        case LineCapStyle::OpenArrow: {
                            double wingAngle = 3.14159265358979323846 / 6.0;
                            D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a - wingAngle)),
                                                             static_cast<float>(tipY - headLen * std::sin(a - wingAngle)));
                            D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a + wingAngle)),
                                                             static_cast<float>(tipY - headLen * std::sin(a + wingAngle)));
                            m_renderTarget->DrawLine(tip, w1, aBrush, strokeW);
                            m_renderTarget->DrawLine(tip, w2, aBrush, strokeW);
                            outTrim = 0.0f;
                            break;
                        }
                        case LineCapStyle::SolidTriangle:
                        case LineCapStyle::HollowTriangle: {
                            double triLen = (capStyle == LineCapStyle::HollowTriangle)
                                ? (std::max)(15.0, static_cast<double>(strokeW) * 3.8)
                                : headLen;
                            if (maxHeadLen > 4.0 && triLen > maxHeadLen) triLen = maxHeadLen;
                            double halfW = triLen * ((capStyle == LineCapStyle::HollowTriangle) ? 0.54 : 0.45);
                            D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(tipX - triLen * cA), static_cast<float>(tipY - triLen * sA));
                            D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + halfW * nX), static_cast<float>(basePt.y + halfW * nY));
                            D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - halfW * nX), static_cast<float>(basePt.y - halfW * nY));
                            ComPtr<ID2D1PathGeometry> headGeo;
                            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(headGeo.GetAddressOf())) && headGeo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(headGeo->Open(sink.GetAddressOf())) && sink) {
                                    sink->BeginFigure(tip, capStyle == LineCapStyle::SolidTriangle ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                                    sink->AddLine(w1);
                                    sink->AddLine(w2);
                                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                    sink->Close();
                                    if (capStyle == LineCapStyle::SolidTriangle) {
                                        m_renderTarget->FillGeometry(headGeo.Get(), aBrush);
                                    } else {
                                        m_renderTarget->DrawGeometry(headGeo.Get(), aBrush, outlineW);
                                    }
                                }
                            }
                            outTrim = (capStyle == LineCapStyle::HollowTriangle)
                                ? static_cast<float>(triLen + static_cast<double>(strokeW) * 0.45)
                                : static_cast<float>(triLen);
                            break;
                        }
                        case LineCapStyle::StealthArrow: {
                            double halfW = headLen * 0.55;
                            D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(tipX - headLen * cA + halfW * nX),
                                                             static_cast<float>(tipY - headLen * sA + halfW * nY));
                            D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(tipX - headLen * cA - halfW * nX),
                                                             static_cast<float>(tipY - headLen * sA - halfW * nY));
                            D2D1_POINT_2F notch = D2D1::Point2F(static_cast<float>(tipX - headLen * 0.65 * cA),
                                                                static_cast<float>(tipY - headLen * 0.65 * sA));
                            ComPtr<ID2D1PathGeometry> wingGeo;
                            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(wingGeo.GetAddressOf())) && wingGeo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(wingGeo->Open(sink.GetAddressOf())) && sink) {
                                    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                                    sink->AddLine(w1);
                                    sink->AddLine(notch);
                                    sink->AddLine(w2);
                                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                    sink->Close();
                                    m_renderTarget->FillGeometry(wingGeo.Get(), aBrush);
                                }
                            }
                            outTrim = static_cast<float>(headLen * 0.65);
                            break;
                        }
                        case LineCapStyle::SolidCircle: {
                            double rad = (std::max)(4.5, static_cast<double>(strokeW) * 1.45);
                            if (maxHeadLen > 4.0 && rad * 1.6 > maxHeadLen) rad = (std::max)(2.5, maxHeadLen / 1.6);
                            D2D1_POINT_2F center = D2D1::Point2F(static_cast<float>(tipX - rad * 0.6 * cA),
                                                                 static_cast<float>(tipY - rad * 0.6 * sA));
                            m_renderTarget->FillEllipse(D2D1::Ellipse(center, static_cast<float>(rad), static_cast<float>(rad)), aBrush);
                            outTrim = static_cast<float>(rad * 0.6);
                            break;
                        }
                        case LineCapStyle::HollowCircle: {
                            double rad = (std::max)(5.5, static_cast<double>(strokeW) * 1.65);
                            if (maxHeadLen > 4.0 && rad * 2.0 > maxHeadLen) rad = (std::max)(2.5, maxHeadLen * 0.5);
                            D2D1_POINT_2F center = D2D1::Point2F(static_cast<float>(tipX - rad * cA),
                                                                 static_cast<float>(tipY - rad * sA));
                            m_renderTarget->DrawEllipse(D2D1::Ellipse(center, static_cast<float>(rad), static_cast<float>(rad)), aBrush, outlineW);
                            outTrim = static_cast<float>(rad * 2.0 + static_cast<double>(strokeW) * 0.35);
                            break;
                        }
                        case LineCapStyle::SolidDiamond:
                        case LineCapStyle::HollowDiamond: {
                            double dLen = headLen * ((capStyle == LineCapStyle::HollowDiamond) ? 1.05 : 0.96);
                            if (maxHeadLen > 4.0 && dLen > maxHeadLen) dLen = maxHeadLen;
                            double halfW = dLen * ((capStyle == LineCapStyle::HollowDiamond) ? 0.50 : 0.48);
                            D2D1_POINT_2F midPt = D2D1::Point2F(static_cast<float>(tipX - dLen * 0.5 * cA), static_cast<float>(tipY - dLen * 0.5 * sA));
                            D2D1_POINT_2F backPt = D2D1::Point2F(static_cast<float>(tipX - dLen * cA), static_cast<float>(tipY - dLen * sA));
                            D2D1_POINT_2F s1 = D2D1::Point2F(static_cast<float>(midPt.x + halfW * nX), static_cast<float>(midPt.y + halfW * nY));
                            D2D1_POINT_2F s2 = D2D1::Point2F(static_cast<float>(midPt.x - halfW * nX), static_cast<float>(midPt.y - halfW * nY));
                            ComPtr<ID2D1PathGeometry> diaGeo;
                            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(diaGeo.GetAddressOf())) && diaGeo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(diaGeo->Open(sink.GetAddressOf())) && sink) {
                                    sink->BeginFigure(tip, capStyle == LineCapStyle::SolidDiamond ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                                    sink->AddLine(s1);
                                    sink->AddLine(backPt);
                                    sink->AddLine(s2);
                                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                    sink->Close();
                                    if (capStyle == LineCapStyle::SolidDiamond) {
                                        m_renderTarget->FillGeometry(diaGeo.Get(), aBrush);
                                    } else {
                                        m_renderTarget->DrawGeometry(diaGeo.Get(), aBrush, outlineW);
                                    }
                                }
                            }
                            outTrim = (capStyle == LineCapStyle::HollowDiamond)
                                ? static_cast<float>(dLen + static_cast<double>(strokeW) * 0.35)
                                : static_cast<float>(dLen * 0.85);
                            break;
                        }
                        case LineCapStyle::BarStop: {
                            double barHalf = (std::max)(6.0, static_cast<double>(strokeW) * 2.1);
                            D2D1_POINT_2F b1 = D2D1::Point2F(static_cast<float>(tipX + barHalf * nX), static_cast<float>(tipY + barHalf * nY));
                            D2D1_POINT_2F b2 = D2D1::Point2F(static_cast<float>(tipX - barHalf * nX), static_cast<float>(tipY - barHalf * nY));
                            m_renderTarget->DrawLine(b1, b2, aBrush, strokeW);
                            outTrim = 0.0f;
                            break;
                        }
                    }
                };

                D2D1_POINT_2F startRef = pathPts[1];
                double acc = 0.0;
                for (size_t i = 1; i < pathPts.size(); ++i) {
                    acc += std::hypot(pathPts[i].x - pathPts[i - 1].x, pathPts[i].y - pathPts[i - 1].y);
                    startRef = pathPts[i];
                    if (acc >= lookbackDist) break;
                }
                const double startAngle = std::atan2(static_cast<double>(pathPts.front().y - startRef.y),
                                                     static_cast<double>(pathPts.front().x - startRef.x));

                D2D1_POINT_2F endRef = pathPts[pathPts.size() - 2];
                acc = 0.0;
                for (size_t i = pathPts.size() - 1; i > 0; --i) {
                    acc += std::hypot(pathPts[i].x - pathPts[i - 1].x, pathPts[i].y - pathPts[i - 1].y);
                    endRef = pathPts[i - 1];
                    if (acc >= lookbackDist) break;
                }
                const double endAngle = std::atan2(static_cast<double>(pathPts.back().y - endRef.y),
                                                   static_cast<double>(pathPts.back().x - endRef.x));

                float startTrim = 0.0f;
                float endTrim = 0.0f;
                drawCapPreview(pathPts.front().x, pathPts.front().y, startAngle, state.currentStartCap, startTrim);
                drawCapPreview(pathPts.back().x, pathPts.back().y, endAngle, state.currentEndCap, endTrim);

                std::vector<float> cumLen(pathPts.size(), 0.0f);
                for (size_t i = 1; i < pathPts.size(); ++i) {
                    cumLen[i] = cumLen[i - 1] + std::hypot(pathPts[i].x - pathPts[i - 1].x, pathPts[i].y - pathPts[i - 1].y);
                }
                const float totF = cumLen.back();
                if (totF > 1.0f && startTrim + endTrim < totF - 1.0f) {
                    const float tStart = std::clamp(startTrim, 0.0f, totF);
                    const float tEnd = std::clamp(totF - endTrim, tStart, totF);
                    auto sampleAt = [&](float targetDist) -> D2D1_POINT_2F {
                        for (size_t i = 1; i < pathPts.size(); ++i) {
                            if (cumLen[i] >= targetDist || i == pathPts.size() - 1) {
                                const float segLen = cumLen[i] - cumLen[i - 1];
                                const float ratio = (segLen > 1e-4f) ? std::clamp((targetDist - cumLen[i - 1]) / segLen, 0.0f, 1.0f) : 0.0f;
                                return D2D1::Point2F(pathPts[i - 1].x + (pathPts[i].x - pathPts[i - 1].x) * ratio,
                                                     pathPts[i - 1].y + (pathPts[i].y - pathPts[i - 1].y) * ratio);
                            }
                        }
                        return pathPts.back();
                    };
                    std::vector<D2D1_POINT_2F> shaftPts;
                    shaftPts.push_back(sampleAt(tStart));
                    for (size_t i = 1; i + 1 < pathPts.size(); ++i) {
                        if (cumLen[i] > tStart + 0.5f && cumLen[i] < tEnd - 0.5f) {
                            shaftPts.push_back(pathPts[i]);
                        }
                    }
                    shaftPts.push_back(sampleAt(tEnd));
                    drawPolylineD2D(shaftPts);
                }
                drawMultiPointAnchorDots();
            }
            break;
        }

        case MarkupTool::Ellipse: {
            auto ell = D2D1::Ellipse(
                D2D1::Point2F((x1 + x2) / 2.0f, (y1 + y2) / 2.0f),
                std::abs(x2 - x1) / 2.0f,
                std::abs(y2 - y1) / 2.0f
            );
            ComPtr<ID2D1SolidColorBrush> borderOverrideBrush;
            ID2D1Brush* ellBorderBrush = toolBrush;
            if (state.currentFillMode && !state.currentTextOutlineColor.isAuto()) {
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(state.currentTextOutlineColor.r / 255.0f,
                                 state.currentTextOutlineColor.g / 255.0f,
                                 state.currentTextOutlineColor.b / 255.0f,
                                 (state.currentTextOutlineColor.a / 255.0f) * liveAlpha),
                    borderOverrideBrush.GetAddressOf());
                if (borderOverrideBrush) ellBorderBrush = borderOverrideBrush.Get();
            }
            const float ellBorderW = state.currentFillMode
                ? static_cast<float>(state.currentOutlineWidth)
                : strokeW;
            if (state.currentFillMode) {
                m_renderTarget->FillEllipse(ell, toolBrush);
            }
            if (!state.currentFillMode || (ellBorderW > 0.05f && (!state.currentTextOutlineColor.isAuto() || state.currentOutlineWidth != 2))) {
                m_renderTarget->DrawEllipse(ell, ellBorderBrush, (std::max)(1.0f, ellBorderW), customStrokeStyle.Get());
            }
            break;
        }

        case MarkupTool::Pen:
            if (state.penPoints.size() >= 2 && m_d2dFactory) {
                std::vector<D2D1_POINT_2F> previewPts;
                previewPts.reserve(state.penPoints.size());
                for (const auto& pt : state.penPoints) {
                    const D2D1_POINT_2F p = D2D1::Point2F(
                        selectionRect.left + static_cast<float>(pt.x),
                        selectionRect.top + static_cast<float>(pt.y));
                    if (previewPts.empty() ||
                        std::hypot(previewPts.back().x - p.x, previewPts.back().y - p.y) > 0.5f) {
                        previewPts.push_back(p);
                    }
                }
                if (state.currentSmoothCurve && previewPts.size() >= 3) {
                    std::vector<D2D1_POINT_2F> ext(previewPts.size() + 2);
                    ext[0] = D2D1::Point2F(2.0f * previewPts[0].x - previewPts[1].x,
                                           2.0f * previewPts[0].y - previewPts[1].y);
                    for (size_t i = 0; i < previewPts.size(); ++i) ext[i + 1] = previewPts[i];
                    ext.back() = D2D1::Point2F(2.0f * previewPts.back().x - previewPts[previewPts.size() - 2].x,
                                               2.0f * previewPts.back().y - previewPts[previewPts.size() - 2].y);
                    std::vector<D2D1_POINT_2F> smoothPts;
                    const int steps = 6;
                    smoothPts.reserve((previewPts.size() - 1) * steps + 1);
                    for (size_t i = 1; i < ext.size() - 2; ++i) {
                        const auto& p0 = ext[i - 1];
                        const auto& p1 = ext[i];
                        const auto& p2 = ext[i + 1];
                        const auto& p3 = ext[i + 2];
                        const int limit = (i == ext.size() - 3) ? (steps + 1) : steps;
                        for (int s = 0; s < limit; ++s) {
                            float t = static_cast<float>(s) / static_cast<float>(steps);
                            float t2 = t * t;
                            float t3 = t2 * t;
                            float px = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                                               (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                                               (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
                            float py = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                                               (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                                               (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
                            smoothPts.push_back(D2D1::Point2F(px, py));
                        }
                    }
                    previewPts = std::move(smoothPts);
                }
                if (previewPts.size() >= 2) {
                    ComPtr<ID2D1PathGeometry> penGeo;
                    if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(penGeo.GetAddressOf())) && penGeo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(penGeo->Open(sink.GetAddressOf())) && sink) {
                            sink->BeginFigure(previewPts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                            for (size_t i = 1; i < previewPts.size(); ++i) {
                                sink->AddLine(previewPts[i]);
                            }
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            m_renderTarget->DrawGeometry(penGeo.Get(), toolBrush, strokeW, customStrokeStyle.Get());
                        }
                    }
                }
            }
            break;

        case MarkupTool::Number: {
            const float baseDpi = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            const float sizeFactor = std::clamp(0.76f + static_cast<float>(state.currentStrokeWidth) * 0.08f, 0.80f, 3.00f);
            float scale = baseDpi * sizeFactor;
            float radius = 15.0f * scale;
            float dist = std::hypot(x2 - x1, y2 - y1);

            const float luminance = (0.299f * state.currentColor.r +
                                     0.587f * state.currentColor.g +
                                     0.114f * state.currentColor.b) / 255.0f;
            const bool isBrightColor = luminance > 0.72f;
            const bool fillBadge = state.currentFillMode;

            // 绘制起点序号标号徽章画刷与文字画刷（与 MarkupEngine 落盘效果 1:1 对齐，实时响应不透明度）
            ComPtr<ID2D1SolidColorBrush> accentBrush, bgBrush, ringBrush, badgeTextBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentColor.r / 255.0f, state.currentColor.g / 255.0f, state.currentColor.b / 255.0f, liveAlpha),
                accentBrush.GetAddressOf());
            if (fillBadge) {
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, 0.30f * liveAlpha)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.86f * liveAlpha),
                    ringBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, liveAlpha)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, liveAlpha),
                    badgeTextBrush.GetAddressOf());
            } else {
                m_renderTarget->CreateSolidColorBrush(
                    isBrightColor ? D2D1::ColorF(15 / 255.0f, 23 / 255.0f, 42 / 255.0f, 0.86f * liveAlpha)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f * liveAlpha),
                    bgBrush.GetAddressOf());
                badgeTextBrush = accentBrush;
            }
            ID2D1Brush* arrowBrush = accentBrush ? static_cast<ID2D1Brush*>(accentBrush.Get()) : static_cast<ID2D1Brush*>(m_borderBrush.Get());

            const int curNum = state.markup.currentNumber(state.currentNumberFormat);
            const std::wstring numStr = formatNumberBadgeLabel(curNum, state.currentNumberFormat);
            float halfW = radius;
            if (state.currentNumberShape == NumberBadgeShape::RoundedSquare && numStr.size() > 1) {
                halfW = radius + static_cast<float>(numStr.size() - 1) * 4.8f * scale;
            }

            // 如果拖拽距离大于阈值 (10px)，实时绘制指向目标点的引出箭头
            if (dist >= 10.0f * baseDpi) {
                double angle = std::atan2(y2 - y1, x2 - x1);
                float numStrokeW = std::max(2.0f, 2.2f * scale);
                double headLen = std::max(11.0 * static_cast<double>(scale), 4.2 * numStrokeW);
                if (headLen > dist * 0.45) headLen = dist * 0.45;
                double cA = std::cos(angle), sA = std::sin(angle);
                double nX = -sA, nY = cA;
                double arrowHalfW = headLen * 0.45;
                D2D1_POINT_2F tip = D2D1::Point2F(x2, y2);
                D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(x2 - headLen * cA), static_cast<float>(y2 - headLen * sA));
                D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + arrowHalfW * nX), static_cast<float>(basePt.y + arrowHalfW * nY));
                D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - arrowHalfW * nX), static_cast<float>(basePt.y - arrowHalfW * nY));

                const double startReach = (state.currentNumberShape == NumberBadgeShape::RoundedSquare)
                    ? std::hypot(halfW * cA, radius * sA)
                    : static_cast<double>(radius);
                D2D1_POINT_2F lineStart = D2D1::Point2F(static_cast<float>(x1 + startReach * cA), static_cast<float>(y1 + startReach * sA));

                m_renderTarget->DrawLine(
                    lineStart,
                    basePt,
                    arrowBrush,
                    numStrokeW
                );

                ComPtr<ID2D1PathGeometry> headGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(headGeo.GetAddressOf())) && headGeo) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(headGeo->Open(sink.GetAddressOf())) && sink) {
                        sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(w1);
                        sink->AddLine(w2);
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        m_renderTarget->FillGeometry(headGeo.Get(), arrowBrush);
                    }
                }
            }

            if (accentBrush) {
                if (state.currentNumberShape == NumberBadgeShape::RoundedSquare) {
                    const float boxRad = std::max(5.5f * scale, radius * 0.42f);
                    auto sqBox = D2D1::RectF(x1 - halfW, y1 - radius, x1 + halfW, y1 + radius);
                    if (fillBadge) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), accentBrush.Get());
                        if (ringBrush) {
                            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), ringBrush.Get(), 1.2f * scale);
                        }
                    } else {
                        if (bgBrush) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), bgBrush.Get());
                        }
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, boxRad, boxRad), accentBrush.Get(), 2.2f * scale);
                    }
                } else {
                    auto circle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius, radius);
                    if (fillBadge) {
                        m_renderTarget->FillEllipse(circle, accentBrush.Get());
                        if (ringBrush) {
                            auto innerCircle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius - 1.0f * scale, radius - 1.0f * scale);
                            m_renderTarget->DrawEllipse(innerCircle, ringBrush.Get(), 1.5f * scale);
                        }
                    } else {
                        if (bgBrush) {
                            m_renderTarget->FillEllipse(circle, bgBrush.Get());
                        }
                        auto ringCircle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius - 1.0f * scale, radius - 1.0f * scale);
                        m_renderTarget->DrawEllipse(ringCircle, accentBrush.Get(), 2.2f * scale);
                    }
                }
            }

            if (badgeTextBrush) {
                IDWriteTextFormat* badgeFmt = (numStr.size() >= 3)
                    ? m_numberBadgeSmallFormat.Get()
                    : (numStr.size() == 2 ? m_numberBadgeMediumFormat.Get() : m_numberBadgeLargeFormat.Get());
                if (!badgeFmt) badgeFmt = m_infoTextFormat.Get();
                if (badgeFmt) {
                    TextFormatScope badgeScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(
                        numStr.c_str(), static_cast<UINT32>(numStr.size()),
                        badgeFmt,
                        D2D1::RectF(x1 - halfW, y1 - radius - 0.5f * scale, x1 + halfW, y1 + radius - 0.5f * scale),
                        badgeTextBrush.Get());
                }
            }
            break;
        }

        case MarkupTool::Magnifier: {
            if (!hasScreenContent() || !m_d2dFactory) break;
            const float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            const float sqrtScale = std::sqrt(scale);
            const float magScale = std::clamp(state.dynamicMagnifierScale, 1.5f, 6.0f);
            const float dragDist = std::hypot(x2 - x1, y2 - y1);
            const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            // 默认直接拖拽即进入零遮挡引出式详图放大；按住 Shift 拖拽则为原位同心自定义半径
            const bool isOffsetMode = !shiftHeld && (dragDist >= 10.0f * scale);

            const float srcCx = x1;
            const float srcCy = y1;
            const float r = (!isOffsetMode && shiftHeld && dragDist > 6.0f * scale)
                ? std::clamp(dragDist, 24.0f * scale, 420.0f)
                : std::max(28.0f * scale, static_cast<float>(state.dynamicMagnifierRadius) * scale);

            float dstCx = isOffsetMode ? x2 : x1;
            float dstCy = isOffsetMode ? y2 : y1;
            if (isOffsetMode) {
                const float margin = 4.0f * scale;
                const float minLx = selectionRect.left + r + margin;
                const float maxLx = selectionRect.right - r - margin;
                const float minLy = selectionRect.top + r + margin;
                const float maxLy = selectionRect.bottom - r - margin;
                dstCx = (minLx <= maxLx) ? std::clamp(dstCx, minLx, maxLx) : (selectionRect.left + selectionRect.right) * 0.5f;
                dstCy = (minLy <= maxLy) ? std::clamp(dstCy, minLy, maxLy) : (selectionRect.top + selectionRect.bottom) * 0.5f;
            }

            const float srcR = r / magScale;
            const D2D1_RECT_F srcRect = D2D1::RectF(srcCx - srcR, srcCy - srcR, srcCx + srcR, srcCy + srcR);
            const D2D1_RECT_F dstRect = D2D1::RectF(dstCx - r, dstCy - r, dstCx + r, dstCy + r);
            const bool isSquare = (state.currentMagnifierShape == NumberBadgeShape::RoundedSquare);
            const float cornerR = std::clamp(r * 0.28f, 8.0f * scale, 32.0f * scale);
            const float rimLum = (0.299f * state.currentMagnifierColor.r +
                                  0.587f * state.currentMagnifierColor.g +
                                  0.114f * state.currentMagnifierColor.b) / 255.0f;

            ComPtr<ID2D1SolidColorBrush> rimBrush, darkHaloBrush, innerRimBrush, jewelCoreBrush, glareBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                             state.currentMagnifierColor.g / 255.0f,
                             state.currentMagnifierColor.b / 255.0f, 0.98f),
                rimBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.46f), darkHaloBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                rimLum > 0.72f ? D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.35f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f),
                innerRimBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                rimLum > 0.72f ? D2D1::ColorF(0.12f, 0.16f, 0.23f, 0.94f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                jewelCoreBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f), glareBrush.GetAddressOf());

            auto drawD2DJewelAnchor = [&](D2D1_POINT_2F pt, float jr) {
                if (darkHaloBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pt.x, pt.y + 0.5f * sqrtScale),
                                                              jr + 1.1f * sqrtScale, jr + 1.1f * sqrtScale),
                                                darkHaloBrush.Get());
                }
                if (rimBrush) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(pt, jr, jr), rimBrush.Get());
                }
                if (jewelCoreBrush) {
                    const float cr = std::max(1.0f, jr * 0.44f);
                    m_renderTarget->FillEllipse(D2D1::Ellipse(pt, cr, cr), jewelCoreBrush.Get());
                }
            };

            bool hasBridge = false;
            D2D1_POINT_2F pLensAnchor{};

            // 1. 学术期刊级 4 种引出样式实时预览（与 MarkupEngine 落盘效果 100% 视觉同构）
            if (isOffsetMode && rimBrush) {
                const float dx = dstCx - srcCx;
                const float dy = dstCy - srcCy;
                const float dist = std::hypot(dx, dy);
                if (dist > 1.0f) {
                    const float ux = dx / dist;
                    const float uy = dy / dist;
                    const float lensEdgeD = computeLoupeBoundaryDistance(-ux, -uy, r, r, cornerR, isSquare);
                    const float apertureR = std::clamp(srcR, 8.0f * sqrtScale, std::max(10.0f * sqrtScale, r * 0.66f));
                    const float srcCornerR = isSquare
                        ? std::clamp(apertureR * 0.28f, 3.5f * sqrtScale, 14.0f * sqrtScale)
                        : 0.0f;
                    const float srcEdgeD = computeLoupeBoundaryDistance(ux, uy, apertureR, apertureR, srcCornerR, isSquare);

                    ComPtr<ID2D1StrokeStyle> calloutDashStyle;
                    if (m_d2dFactory) {
                        const float dashes[] = {4.0f, 3.0f};
                        m_d2dFactory->CreateStrokeStyle(
                            D2D1::StrokeStyleProperties(
                                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                            dashes, 2, calloutDashStyle.GetAddressOf());
                    }

                    auto drawD2DSourceAperture = [&](bool dashedBox) {
                        ID2D1StrokeStyle* boxStyle = (dashedBox && calloutDashStyle) ? calloutDashStyle.Get() : nullptr;
                        if (isSquare) {
                            auto sBox = D2D1::RoundedRect(
                                D2D1::RectF(srcCx - apertureR, srcCy - apertureR, srcCx + apertureR, srcCy + apertureR),
                                srcCornerR, srcCornerR);
                            if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(sBox, darkHaloBrush.Get(), 2.5f * sqrtScale, boxStyle);
                            m_renderTarget->DrawRoundedRectangle(sBox, rimBrush.Get(), 1.35f * sqrtScale, boxStyle);
                        } else {
                            auto sEll = D2D1::Ellipse(D2D1::Point2F(srcCx, srcCy), apertureR, apertureR);
                            if (darkHaloBrush) m_renderTarget->DrawEllipse(sEll, darkHaloBrush.Get(), 2.5f * sqrtScale, boxStyle);
                            m_renderTarget->DrawEllipse(sEll, rimBrush.Get(), 1.35f * sqrtScale, boxStyle);
                        }
                        const float crossArm = std::clamp(apertureR * 0.24f, 3.0f * sqrtScale, 5.2f * sqrtScale);
                        if (darkHaloBrush) {
                            m_renderTarget->DrawLine(D2D1::Point2F(srcCx - crossArm, srcCy), D2D1::Point2F(srcCx + crossArm, srcCy), darkHaloBrush.Get(), 2.1f * sqrtScale);
                            m_renderTarget->DrawLine(D2D1::Point2F(srcCx, srcCy - crossArm), D2D1::Point2F(srcCx, srcCy + crossArm), darkHaloBrush.Get(), 2.1f * sqrtScale);
                        }
                        m_renderTarget->DrawLine(D2D1::Point2F(srcCx - crossArm, srcCy), D2D1::Point2F(srcCx + crossArm, srcCy), rimBrush.Get(), 1.15f * sqrtScale);
                        m_renderTarget->DrawLine(D2D1::Point2F(srcCx, srcCy - crossArm), D2D1::Point2F(srcCx, srcCy + crossArm), rimBrush.Get(), 1.15f * sqrtScale);
                    };

                    const MagnifierCalloutStyle calloutStyle = state.currentMagnifierCalloutStyle;

                    if (calloutStyle == MagnifierCalloutStyle::CenterPointer) {
                        if (dist > lensEdgeD + 4.0f * sqrtScale) {
                            hasBridge = true;
                            const D2D1_POINT_2F pCenter = D2D1::Point2F(srcCx, srcCy);
                            pLensAnchor = D2D1::Point2F(dstCx - ux * lensEdgeD, dstCy - uy * lensEdgeD);
                            const float spanLen = dist - lensEdgeD;

                            const float dirX = -ux;
                            const float dirY = -uy;
                            const float normX = -dirY;
                            const float normY = dirX;
                            const float pinR = 2.6f * sqrtScale;
                            const float tipDist = pinR + 0.8f * sqrtScale;
                            const float headLen = std::clamp(spanLen * 0.32f, 7.5f * sqrtScale, 11.5f * sqrtScale);
                            const float headHalfW = headLen * 0.43f;
                            const bool showArrow = (spanLen > headLen + tipDist + 4.0f * sqrtScale);

                            const D2D1_POINT_2F pShaftEnd = showArrow
                                ? D2D1::Point2F(srcCx - dirX * (tipDist + headLen * 0.68f),
                                                srcCy - dirY * (tipDist + headLen * 0.68f))
                                : pCenter;

                            if (darkHaloBrush) {
                                m_renderTarget->DrawLine(pLensAnchor, pShaftEnd, darkHaloBrush.Get(), 3.1f * sqrtScale);
                            }
                            m_renderTarget->DrawLine(pLensAnchor, pShaftEnd, rimBrush.Get(), 1.85f * sqrtScale);

                            if (showArrow && m_d2dFactory) {
                                const D2D1_POINT_2F pTip = D2D1::Point2F(srcCx - dirX * tipDist, srcCy - dirY * tipDist);
                                const D2D1_POINT_2F pBase = D2D1::Point2F(pTip.x - dirX * headLen, pTip.y - dirY * headLen);
                                const D2D1_POINT_2F pNotch = D2D1::Point2F(pTip.x - dirX * (headLen * 0.74f), pTip.y - dirY * (headLen * 0.74f));
                                const D2D1_POINT_2F pW1 = D2D1::Point2F(pBase.x + normX * headHalfW, pBase.y + normY * headHalfW);
                                const D2D1_POINT_2F pW2 = D2D1::Point2F(pBase.x - normX * headHalfW, pBase.y - normY * headHalfW);
                                ComPtr<ID2D1PathGeometry> arrowGeo;
                                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arrowGeo.GetAddressOf())) && arrowGeo) {
                                    ComPtr<ID2D1GeometrySink> sink;
                                    if (SUCCEEDED(arrowGeo->Open(sink.GetAddressOf())) && sink) {
                                        sink->BeginFigure(pTip, D2D1_FIGURE_BEGIN_FILLED);
                                        sink->AddLine(pW1);
                                        sink->AddLine(pNotch);
                                        sink->AddLine(pW2);
                                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                        sink->Close();
                                        if (darkHaloBrush) {
                                            m_renderTarget->DrawGeometry(arrowGeo.Get(), darkHaloBrush.Get(), 1.6f * sqrtScale);
                                        }
                                        m_renderTarget->FillGeometry(arrowGeo.Get(), rimBrush.Get());
                                    }
                                }
                            }
                            drawD2DJewelAnchor(pCenter, pinR);
                        }
                    } else if (calloutStyle == MagnifierCalloutStyle::ElbowLeader) {
                        drawD2DSourceAperture(false);
                        if (dist > lensEdgeD + srcEdgeD + 4.0f * sqrtScale) {
                            const float signX = (dx >= 0.0f) ? 1.0f : -1.0f;
                            const float signY = (dy >= 0.0f) ? 1.0f : -1.0f;
                            const float diagUx = signX * 0.70710678f;
                            const float diagUy = signY * 0.70710678f;
                            const float srcDiagD = computeLoupeBoundaryDistance(diagUx, diagUy, apertureR, apertureR, srcCornerR, isSquare);
                            const D2D1_POINT_2F pSrcEdge = D2D1::Point2F(srcCx + diagUx * srcDiagD, srcCy + diagUy * srcDiagD);
                            const D2D1_POINT_2F pLensEdge = D2D1::Point2F(dstCx - ux * lensEdgeD, dstCy - uy * lensEdgeD);
                            const float diagStep = std::min(std::abs(pLensEdge.x - pSrcEdge.x), std::abs(pLensEdge.y - pSrcEdge.y)) * 0.65f;
                            const D2D1_POINT_2F pElbow = D2D1::Point2F(pSrcEdge.x + signX * diagStep, pSrcEdge.y + signY * diagStep);
                            if (darkHaloBrush) {
                                m_renderTarget->DrawLine(pSrcEdge, pElbow, darkHaloBrush.Get(), 2.6f * sqrtScale);
                                m_renderTarget->DrawLine(pElbow, pLensEdge, darkHaloBrush.Get(), 2.6f * sqrtScale);
                            }
                            m_renderTarget->DrawLine(pSrcEdge, pElbow, rimBrush.Get(), 1.45f * sqrtScale);
                            m_renderTarget->DrawLine(pElbow, pLensEdge, rimBrush.Get(), 1.45f * sqrtScale);
                        }
                    } else if (calloutStyle == MagnifierCalloutStyle::DashedDirect) {
                        drawD2DSourceAperture(true);
                        if (dist > lensEdgeD + srcEdgeD + 3.0f * sqrtScale) {
                            const D2D1_POINT_2F pSrcEdge = D2D1::Point2F(srcCx + ux * srcEdgeD, srcCy + uy * srcEdgeD);
                            const D2D1_POINT_2F pLensEdge = D2D1::Point2F(dstCx - ux * lensEdgeD, dstCy - uy * lensEdgeD);
                            ID2D1StrokeStyle* ds = calloutDashStyle.Get();
                            if (darkHaloBrush) {
                                m_renderTarget->DrawLine(pSrcEdge, pLensEdge, darkHaloBrush.Get(), 2.5f * sqrtScale, ds);
                            }
                            m_renderTarget->DrawLine(pSrcEdge, pLensEdge, rimBrush.Get(), 1.35f * sqrtScale, ds);
                        }
                    } else {
                        // TangentFrustum: 学术双外切展开细线
                        drawD2DSourceAperture(false);
                        if (dist > lensEdgeD + 2.0f * sqrtScale) {
                            const float nx = -uy;
                            const float ny = ux;
                            const float sinT = std::clamp((r - apertureR) / std::max(dist, 1.0f), -0.82f, 0.82f);
                            const float cosT = std::sqrt(std::max(0.05f, 1.0f - sinT * sinT));
                            for (float side : {-1.0f, 1.0f}) {
                                const float vx = -sinT * ux + side * cosT * nx;
                                const float vy = -sinT * uy + side * cosT * ny;
                                const float sDist = computeLoupeBoundaryDistance(vx, vy, apertureR, apertureR, srcCornerR, isSquare);
                                const float lDist = computeLoupeBoundaryDistance(vx, vy, r, r, cornerR, isSquare);
                                const D2D1_POINT_2F pSrcSide = D2D1::Point2F(srcCx + vx * sDist, srcCy + vy * sDist);
                                const D2D1_POINT_2F pLensSide = D2D1::Point2F(dstCx + vx * lDist, dstCy + vy * lDist);
                                if (darkHaloBrush) {
                                    m_renderTarget->DrawLine(pSrcSide, pLensSide, darkHaloBrush.Get(), 2.5f * sqrtScale);
                                }
                                m_renderTarget->DrawLine(pSrcSide, pLensSide, rimBrush.Get(), 1.35f * sqrtScale);
                            }
                        }
                    }
                }
            }

            // 2. 多层漫射悬浮投影
            struct D2DShadowStep { float dy; float expand; float alpha; };
            static constexpr D2DShadowStep kD2DShadows[] = {
                {1.2f, 1.0f, 0.20f}, {3.0f, 3.2f, 0.14f}, {5.6f, 6.4f, 0.09f},
                {8.8f, 10.5f, 0.05f}, {12.0f, 15.0f, 0.025f}
            };
            for (const auto& sh : kD2DShadows) {
                ComPtr<ID2D1SolidColorBrush> shBr;
                if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.04f, 0.08f, sh.alpha), shBr.GetAddressOf())) && shBr) {
                    const float ex = sh.expand * sqrtScale;
                    const float dy = sh.dy * sqrtScale;
                    if (isSquare) {
                        auto sRect = D2D1::RectF(dstCx - r - ex, dstCy - r - ex + dy, dstCx + r + ex, dstCy + r + ex + dy);
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, cornerR + ex, cornerR + ex), shBr.Get());
                    } else {
                        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy + dy), r + ex, r + ex), shBr.Get());
                    }
                }
            }

            ComPtr<ID2D1Geometry> clipGeo;
            if (isSquare) {
                ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
                m_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dstRect, cornerR, cornerR), rrGeo.GetAddressOf());
                clipGeo = rrGeo;
            } else {
                ComPtr<ID2D1EllipseGeometry> elGeo;
                m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), r, r), elGeo.GetAddressOf());
                clipGeo = elGeo;
            }

            if (clipGeo) {
                ComPtr<ID2D1Layer> layer;
                if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(
                        D2D1::InfiniteRect(), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE), layer.Get());
                    if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                        dstRect, srcRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, true);
                    } else if (m_screenBitmap) {
                        ComPtr<ID2D1DeviceContext> dc;
                        if (SUCCEEDED(m_renderTarget.As(&dc)) && dc) {
                            dc->DrawBitmap(m_screenBitmap.Get(), &dstRect, 1.0f,
                                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcRect, nullptr);
                        } else {
                            m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstRect, 1.0f,
                                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect);
                        }
                    }
                    // 顶部微高光折射线
                    if (glareBrush && r >= 28.0f) {
                        m_renderTarget->DrawLine(
                            D2D1::Point2F(dstCx - r * 0.55f, dstCy - r + 3.5f * sqrtScale),
                            D2D1::Point2F(dstCx + r * 0.55f, dstCy - r + 3.5f * sqrtScale),
                            glareBrush.Get(), 1.4f * sqrtScale);
                    }
                    m_renderTarget->PopLayer();
                }

                const float rimW = std::clamp((1.55f + static_cast<float>(state.currentStrokeWidth) * 0.34f) * sqrtScale,
                                              2.2f, 4.8f * sqrtScale);
                const float inR = std::max(4.0f, r - rimW * 0.52f);
                if (isSquare) {
                    auto rr = D2D1::RoundedRect(dstRect, cornerR, cornerR);
                    auto rrIn = D2D1::RoundedRect(D2D1::RectF(dstCx - inR, dstCy - inR, dstCx + inR, dstCy + inR),
                                                  std::max(2.0f, cornerR - rimW * 0.5f), std::max(2.0f, cornerR - rimW * 0.5f));
                    if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(rr, darkHaloBrush.Get(), rimW + 1.8f * sqrtScale);
                    if (rimBrush) m_renderTarget->DrawRoundedRectangle(rr, rimBrush.Get(), rimW);
                    if (innerRimBrush) m_renderTarget->DrawRoundedRectangle(rrIn, innerRimBrush.Get(), 0.95f * sqrtScale);
                } else {
                    auto el = D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), r, r);
                    auto elIn = D2D1::Ellipse(D2D1::Point2F(dstCx, dstCy), inR, inR);
                    if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), rimW + 1.8f * sqrtScale);
                    if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), rimW);
                    if (innerRimBrush) m_renderTarget->DrawEllipse(elIn, innerRimBrush.Get(), 0.95f * sqrtScale);
                }

                if (hasBridge) {
                    drawD2DJewelAnchor(pLensAnchor, 2.9f * sqrtScale);
                }
            }
            break;
        }

        default:
            break;
    }
}

} // namespace tools3000::capture
