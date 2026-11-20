// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowDrawers.cpp — 演播室左侧导航、标注卡片与抽屉、水印抽屉渲染子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioPreviewWindowInternal.h"
#include "capture/StudioBackdropCatalog.h"
#include "../resource.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "ui/KeyboardPipeline.h"

#include <opencv2/imgproc.hpp>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;
using namespace studio_detail;

void StudioPreviewWindow::insertAnnotationCardPreset(int cardIndex, const float* dropNormX, const float* dropNormY) {
    m_playing = false;
    pushUndoSnapshot();

    const int64_t rawTotalMs = (std::max<int64_t>)(400, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    const int64_t curSrcMs = static_cast<int64_t>(std::llround(currentDisplayedSourceSec() * 1000.0));
    const int64_t stMs = std::clamp<int64_t>(curSrcMs, 0, rawTotalMs - 200);
    const int64_t edMs = (std::min)(rawTotalMs, stMs + 3000);
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    if (cardIndex == 18) {
        StudioZoomSegment z{};
        z.startMs = stMs;
        z.endMs = edMs;
        const float cxNorm = dropNormX ? std::clamp(*dropNormX, 0.08f, 0.92f) : 0.5f;
        const float cyNorm = dropNormY ? std::clamp(*dropNormY, 0.08f, 0.92f) : 0.5f;
        z.focusX = static_cast<float>(m_timeline.sourceWidth) * cxNorm;
        z.focusY = static_cast<float>(m_timeline.sourceHeight) * cyNorm;
        z.targetZoom = (m_config.defaultZoomFactor >= 1.05f || m_config.focusSpotlightDim > 0.01f)
            ? m_config.defaultZoomFactor : 1.32f;
        z.enabled = true;
        z.manualInserted = true;
        m_config.autoZoomEnabled = true;
        if (m_config.zoomMode == StudioZoomMode::None) {
            m_config.zoomMode = StudioZoomMode::Zoom2D;
        }
        m_timeline.zoomSegments.push_back(z);
        m_selectedZoomIdx = static_cast<int>(m_timeline.zoomSegments.size()) - 1;
        m_selectedClipIdx = -1;
        m_selectedAnnIdx = -1;
        rebakeTimelinePreview();
        requestImmediatePreviewRefresh(true);
        return;
    }

    StudioAnnotationItem ann{};
    ann.startMs = stMs;
    ann.endMs = edMs;
    ann.x0 = 0.36f;
    ann.y0 = 0.38f;
    ann.x1 = 0.64f;
    ann.y1 = 0.58f;
    ann.strokeWidth = 3.5f;
    ann.enabled = true;

    switch (cardIndex) {
        case 0:
            ann.kind = StudioAnnotationKind::TextPlain;
            ann.colorHex = "#F59E0B";
            ann.text = isZh ? "重点说明" : "Text";
            break;
        case 1:
            ann.kind = StudioAnnotationKind::Text;
            ann.colorHex = "#1E293B";
            ann.text = isZh ? "操作提示" : "Text";
            break;
        case 2:
            ann.kind = StudioAnnotationKind::TextBubble;
            ann.colorHex = "#A855F7";
            ann.text = isZh ? "点击这里" : "Text";
            break;
        case 3:
            ann.kind = StudioAnnotationKind::TextDashed;
            ann.colorHex = "#94A3B8";
            ann.text = isZh ? "补充备注" : "Text";
            break;
        case 4:
            ann.kind = StudioAnnotationKind::TextNeon;
            ann.colorHex = "#EC4899";
            ann.text = isZh ? "关键步骤" : "Text";
            break;
        case 5:
            ann.kind = StudioAnnotationKind::StepBadge;
            ann.colorHex = "#3B82F6";
            ann.customStepText = false;
            ann.text = "1";
            ann.x0 = 0.46f; ann.y0 = 0.44f; ann.x1 = 0.54f; ann.y1 = 0.52f;
            break;
        case 6:
            ann.kind = StudioAnnotationKind::GlowLine;
            ann.colorHex = "#10B981";
            ann.x0 = 0.32f; ann.y0 = 0.52f; ann.x1 = 0.68f; ann.y1 = 0.52f;
            break;
        case 7:
            ann.kind = StudioAnnotationKind::DashedLine;
            ann.colorHex = "#38BDF8";
            ann.x0 = 0.32f; ann.y0 = 0.50f; ann.x1 = 0.68f; ann.y1 = 0.50f;
            break;
        case 8:
            ann.kind = StudioAnnotationKind::Arrow;
            ann.colorHex = "#F43F5E";
            ann.x0 = 0.34f; ann.y0 = 0.38f; ann.x1 = 0.56f; ann.y1 = 0.54f;
            break;
        case 9:
            ann.kind = StudioAnnotationKind::Rectangle;
            ann.colorHex = "#EF4444";
            break;
        case 10:
            ann.kind = StudioAnnotationKind::RoundedBox;
            ann.colorHex = "#F59E0B";
            break;
        case 11:
            ann.kind = StudioAnnotationKind::CircleRing;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.40f; ann.y0 = 0.36f; ann.x1 = 0.60f; ann.y1 = 0.64f;
            break;
        case 12:
            ann.kind = StudioAnnotationKind::SketchCircle;
            ann.colorHex = "#F97316";
            ann.x0 = 0.38f; ann.y0 = 0.38f; ann.x1 = 0.62f; ann.y1 = 0.60f;
            break;
        case 13:
            ann.kind = StudioAnnotationKind::BoldArrow;
            ann.colorHex = "#EC4899";
            ann.x0 = 0.50f; ann.y0 = 0.28f;
            ann.x1 = 0.50f; ann.y1 = 0.48f;
            ann.strokeWidth = 5.0f;
            break;
        case 14:
            ann.kind = StudioAnnotationKind::HandPointer;
            ann.colorHex = "#EC4899";
            ann.x0 = 0.46f; ann.y0 = 0.45f; ann.x1 = 0.54f; ann.y1 = 0.55f;
            break;
        case 15:
            ann.kind = StudioAnnotationKind::Spotlight;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.30f; ann.y0 = 0.32f; ann.x1 = 0.70f; ann.y1 = 0.66f;
            break;
        case 16:
            ann.kind = StudioAnnotationKind::BlurMask;
            ann.colorHex = "#94A3B8";
            ann.x0 = 0.34f; ann.y0 = 0.38f; ann.x1 = 0.66f; ann.y1 = 0.56f;
            break;
        case 17:
        default:
            ann.kind = StudioAnnotationKind::Magnifier;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.40f; ann.y0 = 0.36f; ann.x1 = 0.60f; ann.y1 = 0.64f;
            break;
    }

    if (dropNormX && dropNormY) {
        const float spanW = ann.x1 - ann.x0;
        const float spanH = ann.y1 - ann.y0;
        const float cx = std::clamp(*dropNormX, 0.04f, 0.96f);
        const float cy = std::clamp(*dropNormY, 0.04f, 0.96f);
        if (isPointDirectedAnnotation(ann.kind)) {
            // 指向型箭头/线段：让箭头尖端 (x1, y1) 精准对准用户松手目标点
            ann.x1 = cx;
            ann.y1 = cy;
            ann.x0 = std::clamp(cx - spanW, 0.02f, 0.98f);
            ann.y0 = std::clamp(cy - spanH, 0.02f, 0.98f);
        } else {
            const float halfW = std::abs(spanW) * 0.5f;
            const float halfH = std::abs(spanH) * 0.5f;
            ann.x0 = std::clamp(cx - halfW, 0.01f, 0.99f - halfW * 2.0f);
            ann.y0 = std::clamp(cy - halfH, 0.01f, 0.99f - halfH * 2.0f);
            ann.x1 = std::clamp(ann.x0 + std::abs(spanW), 0.02f, 0.99f);
            ann.y1 = std::clamp(ann.y0 + std::abs(spanH), 0.02f, 0.99f);
        }
    }

    m_timeline.annotations.push_back(ann);
    m_selectedAnnIdx = static_cast<int>(m_timeline.annotations.size()) - 1;
    m_selectedZoomIdx = -1;
    m_selectedClipIdx = -1;
    m_editingAnnotationText = (cardIndex >= 0 && cardIndex <= 4);
    resequenceAutoStepBadges();
    m_annDemoStartTick = (ann.kind == StudioAnnotationKind::BlurMask) ? 0 : GetTickCount64();

    requestImmediatePreviewRefresh(true);
}

void StudioPreviewWindow::renderAnnotationDrawer(
    float padL, float padR, float startY, float bottomLimit,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.64f, 0.70f, 0.80f, 1.0f) : D2D1::ColorF(0.35f, 0.41f, 0.51f, 1.0f);
    const D2D1_COLOR_F colTileBg = dark ? D2D1::ColorF(0.125f, 0.145f, 0.205f, 1.0f) : D2D1::ColorF(0.935f, 0.950f, 0.975f, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.12f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const float fullW = padR - padL;
    float y = startY;
    const float tilePhase = static_cast<float>(GetTickCount64() % 1400ULL) / 1400.0f;
    const float tileSin = std::sin(tilePhase * 6.2831853f);

    drawTxt(isZh ? L"文本" : L"Text", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        const float gap = 6.0f;
        const float cw = (fullW - gap * 2.0f) / 3.0f;
        const float ch = 36.0f;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3;
            const int row = i / 3;
            const float tx = padL + col * (cw + gap);
            const float ty = y + row * (ch + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + cw, ty + ch);
            const int hId = Hit_AnnCardBase + i;
            const bool hov = (m_hoveredHitId == hId);
            m_hitZones.push_back({hId, rc, i});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, hov ? 1.5f : 0.9f);

            const float liftY = hov ? (-1.8f * std::abs(tileSin)) : 0.0f;
            const D2D1_RECT_F inner = D2D1::RectF(rc.left + 9.0f, rc.top + 6.5f + liftY, rc.right - 9.0f, rc.bottom - 6.5f + liftY);
            if (i == 0) {
                drawTxt(L"Text", m_bodyFmt.Get(), D2D1::RectF(rc.left, rc.top + liftY, rc.right, rc.bottom + liftY), colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 1) {
                setBrush(D2D1::ColorF(0.18f, 0.22f, 0.30f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(inner, 6.0f, 6.0f), brush);
                drawTxt(L"Text", m_captionFmt.Get(), inner, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 2) {
                const float bob = hov ? (tileSin * 1.8f) : 0.0f;
                const D2D1_RECT_F brc = D2D1::RectF(inner.left, inner.top - 1.0f + bob, inner.right, inner.bottom - 3.0f + bob);
                setBrush(D2D1::ColorF(0.66f, 0.33f, 0.97f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 7.0f, 7.0f), brush);
                const float bcx = (brc.left + brc.right) * 0.5f;
                m_renderTarget->DrawLine({bcx - 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                m_renderTarget->DrawLine({bcx + 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                drawTxt(L"Text", m_captionFmt.Get(), brc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 3) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashOffset = hov ? (tilePhase * 6.0f) : 0.0f;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(hov ? colAccent : colSecondary);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.0f, 5.0f), brush, 1.3f, ds.Get());
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 4) {
                const float glowAlpha = hov ? (0.28f + 0.34f * (0.5f + 0.5f * tileSin)) : 0.35f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, glowAlpha));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, hov ? 4.4f : 3.4f);
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, 1.4f);
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                const float cx = (rc.left + rc.right) * 0.5f;
                const float cy = (rc.top + rc.bottom) * 0.5f;
                if (hov) {
                    setBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, (1.0f - tilePhase) * 0.60f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 11.5f + tilePhase * 6.0f, 11.5f + tilePhase * 6.0f), brush, 1.5f);
                }
                setBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({cx, cy}, 11.5f, 11.5f), brush);
                drawTxt(L"1", m_bodyFmt.Get(), rc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        y += ch * 2.0f + gap + 9.0f;
    }

    drawTxt(isZh ? L"形状" : L"Shapes", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        const float gap = 6.0f;
        const float cw = (fullW - gap * 2.0f) / 3.0f;
        const float ch = 40.0f;
        ComPtr<ID2D1StrokeStyle> roundCapStyle;
        if (m_d2dFactory) {
            D2D1_STROKE_STYLE_PROPERTIES rsp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            m_d2dFactory->CreateStrokeStyle(rsp, nullptr, 0, roundCapStyle.GetAddressOf());
        }
        for (int i = 0; i < 9; ++i) {
            const int cardIdx = 6 + i;
            const int col = i % 3;
            const int row = i / 3;
            const float tx = padL + col * (cw + gap);
            const float ty = y + row * (ch + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + cw, ty + ch);
            const int hId = Hit_AnnCardBase + cardIdx;
            const bool hov = (m_hoveredHitId == hId);
            m_hitZones.push_back({hId, rc, cardIdx});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, hov ? 1.5f : 0.9f);

            const float cx = (rc.left + rc.right) * 0.5f;
            const float cy = (rc.top + rc.bottom) * 0.5f;
            if (i == 0) {
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.30f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 6.0f, roundCapStyle.Get());
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 2.4f, roundCapStyle.Get());
                if (hov) {
                    const float sx = (cx - 16.0f) + tilePhase * 26.0f;
                    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f));
                    m_renderTarget->DrawLine({sx, cy}, {(std::min)(cx + 16.0f, sx + 6.5f), cy}, brush, 2.2f, roundCapStyle.Get());
                }
            } else if (i == 1) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashOffset = hov ? (-tilePhase * 8.0f) : 0.0f;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(D2D1::ColorF(0.22f, 0.74f, 0.97f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 2.4f, ds.Get());
            } else if (i == 2) {
                const float dx = hov ? (tileSin * 2.8f) : 0.0f;
                setBrush(D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f + dx, cy}, {cx + 15.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
                m_renderTarget->DrawLine({cx + 9.0f + dx, cy - 5.2f}, {cx + 16.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
                m_renderTarget->DrawLine({cx + 9.0f + dx, cy + 5.2f}, {cx + 16.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
            } else if (i == 3) {
                if (hov) {
                    const float exp = tilePhase * 4.5f;
                    setBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, (1.0f - tilePhase) * 0.55f));
                    m_renderTarget->DrawRectangle(D2D1::RectF(cx - 10.5f - exp, cy - 9.5f - exp, cx + 10.5f + exp, cy + 9.5f + exp), brush, 1.3f);
                }
                setBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f));
                m_renderTarget->DrawRectangle(D2D1::RectF(cx - 10.5f, cy - 9.5f, cx + 10.5f, cy + 9.5f), brush, 2.3f);
            } else if (i == 4) {
                if (hov) {
                    const float exp = tilePhase * 4.5f;
                    setBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, (1.0f - tilePhase) * 0.55f));
                    m_renderTarget->DrawRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(cx - 10.5f - exp, cy - 9.5f - exp, cx + 10.5f + exp, cy + 9.5f + exp), 5.0f + exp, 5.0f + exp), brush, 1.3f);
                }
                setBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 10.5f, cy - 9.5f, cx + 10.5f, cy + 9.5f), 5.0f, 5.0f), brush, 2.3f);
            } else if (i == 5) {
                if (hov) {
                    const float exp = tilePhase * 5.0f;
                    setBrush(D2D1::ColorF(colPrimary.r, colPrimary.g, colPrimary.b, (1.0f - tilePhase) * 0.50f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 10.2f + exp, 10.2f + exp), brush, 1.3f);
                }
                setBrush(colPrimary);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 10.2f, 10.2f), brush, 2.4f);
            } else if (i == 6) {
                // 手绘重点圈图标：圆润单笔螺旋收尾马克笔弧线（与舞台真实渲染完全同构）
                ComPtr<ID2D1PathGeometry> loopGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(loopGeo.GetAddressOf())) && loopGeo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(loopGeo->Open(sk.GetAddressOf())) && sk) {
                        const int maxS = hov ? (std::max)(8, static_cast<int>(std::round(48.0f * std::min(1.0f, tilePhase * 1.45f)))) : 48;
                        auto sampleIconLoop = [&](int s) -> D2D1_POINT_2F {
                            const float u = static_cast<float>(s) / 48.0f;
                            const float th = -1.88f + u * 6.84f;
                            const float sp = 0.92f + 0.14f * u;
                            return {cx + std::cos(th) * 14.5f * sp, cy + std::sin(th) * 9.2f * sp};
                        };
                        sk->BeginFigure(sampleIconLoop(0), D2D1_FIGURE_BEGIN_HOLLOW);
                        for (int s = 1; s <= maxS; ++s) sk->AddLine(sampleIconLoop(s));
                        sk->EndFigure(D2D1_FIGURE_END_OPEN);
                        sk->Close();
                        setBrush(D2D1::ColorF(0.98f, 0.45f, 0.09f, 0.22f));
                        m_renderTarget->DrawGeometry(loopGeo.Get(), brush, 4.4f, roundCapStyle.Get());
                        setBrush(D2D1::ColorF(0.98f, 0.45f, 0.09f, 1.0f));
                        m_renderTarget->DrawGeometry(loopGeo.Get(), brush, 2.2f, roundCapStyle.Get());
                    }
                }
            } else if (i == 7) {
                const float dy = hov ? (tileSin * 2.6f) : 0.0f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(geo->Open(sk.GetAddressOf())) && sk) {
                        sk->BeginFigure({cx - 3.8f, cy - 12.0f + dy}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({cx + 3.8f, cy - 12.0f + dy});
                        sk->AddLine({cx + 3.8f, cy + 1.0f + dy});
                        sk->AddLine({cx + 9.2f, cy + 1.0f + dy});
                        sk->AddLine({cx, cy + 12.0f + dy});
                        sk->AddLine({cx - 9.2f, cy + 1.0f + dy});
                        sk->AddLine({cx - 3.8f, cy + 1.0f + dy});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        m_renderTarget->FillGeometry(geo.Get(), brush);
                    }
                }
            } else {
                // 点击手势图标：精致食指轻触矢量轮廓 + 指尖同心点击波纹弧
                const float tapY = hov ? (std::max)(0.0f, tileSin) * 1.8f : 0.0f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, hov ? (1.0f - tilePhase) * 0.85f : 0.70f));
                const float ripR = hov ? (4.5f + tilePhase * 6.5f) : 5.5f;
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx - 1.5f, cy - 8.5f}, ripR, ripR * 0.72f), brush, 1.4f);

                ComPtr<ID2D1PathGeometry> handGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(handGeo.GetAddressOf())) && handGeo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(handGeo->Open(sk.GetAddressOf())) && sk) {
                        const float hy = cy + tapY;
                        sk->BeginFigure({cx - 3.5f, hy - 8.0f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({cx + 0.5f, hy - 8.0f});
                        sk->AddLine({cx + 0.5f, hy - 1.0f});
                        sk->AddLine({cx + 7.5f, hy + 0.5f});
                        sk->AddLine({cx + 8.0f, hy + 8.5f});
                        sk->AddLine({cx + 4.0f, hy + 12.5f});
                        sk->AddLine({cx - 3.5f, hy + 12.5f});
                        sk->AddLine({cx - 8.0f, hy + 5.5f});
                        sk->AddLine({cx - 5.5f, hy + 3.0f});
                        sk->AddLine({cx - 3.5f, hy + 5.0f});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        setBrush(colPrimary);
                        m_renderTarget->DrawGeometry(handGeo.Get(), brush, 1.8f, roundCapStyle.Get());
                    }
                }
            }
        }
        y += ch * 3.0f + gap * 2.0f + 9.0f;
    }

    drawTxt(isZh ? L"遮罩" : L"Mask & Zoom", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        static const wchar_t* maskZh[4] = { L"聚光灯", L"马赛克", L"放大镜", L"局部缩放" };
        static const wchar_t* maskEn[4] = { L"Spot", L"Mosaic", L"Loupe", L"Zoom" };
        const float gap = 5.5f;
        const float cw = (fullW - gap * 3.0f) / 4.0f;
        const float ch = 48.0f;
        for (int m = 0; m < 4; ++m) {
            const int cardIdx = 15 + m;
            const float tx = padL + m * (cw + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, y, tx + cw, y + ch);
            const int hId = Hit_AnnCardBase + cardIdx;
            m_hitZones.push_back({hId, rc, cardIdx});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);

            const float cx = (rc.left + rc.right) * 0.5f;
            const float iy = rc.top + 15.0f;
            setBrush(colPrimary);
            if (m == 0) {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 7.0f, iy - 6.0f, cx + 7.0f, iy - 1.0f), 2.0f, 2.0f), brush, 1.5f);
                m_renderTarget->DrawLine({cx - 5.0f, iy + 1.0f}, {cx - 9.0f, iy + 8.0f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx, iy + 1.0f}, {cx, iy + 8.5f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx + 5.0f, iy + 1.0f}, {cx + 9.0f, iy + 8.0f}, brush, 1.3f);
            } else if (m == 1) {
                for (int gy = 0; gy < 3; ++gy) {
                    for (int gx = 0; gx < 3; ++gx) {
                        if (((gx + gy) & 1) == 0) {
                            m_renderTarget->FillRectangle(
                                D2D1::RectF(cx - 7.5f + gx * 5.0f, iy - 7.0f + gy * 5.0f,
                                            cx - 3.0f + gx * 5.0f, iy - 2.5f + gy * 5.0f), brush);
                        }
                    }
                }
            } else if (m == 2) {
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx - 2.0f, iy - 1.5f}, 6.5f, 6.5f), brush, 1.6f);
                m_renderTarget->DrawLine({cx + 3.0f, iy + 3.5f}, {cx + 8.5f, iy + 9.0f}, brush, 2.0f);
                m_renderTarget->DrawLine({cx - 4.5f, iy - 1.5f}, {cx + 0.5f, iy - 1.5f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx - 2.0f, iy - 4.0f}, {cx - 2.0f, iy + 1.0f}, brush, 1.3f);
            } else {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 9.0f, iy - 6.5f, cx + 9.0f, iy + 6.5f), 2.5f, 2.5f), brush, 1.5f);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 4.0f, iy - 2.5f, cx + 5.0f, iy + 3.5f), 1.5f, 1.5f), brush);
            }
            drawTxt(isZh ? maskZh[m] : maskEn[m], m_captionFmt.Get(),
                    D2D1::RectF(rc.left, rc.bottom - 18.0f, rc.right, rc.bottom - 2.0f),
                    colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += ch + 10.0f;
    }

    setBrush(colBorder);
    m_renderTarget->DrawLine({padL, y}, {padR, y}, brush, 1.0f);
    y += 8.0f;

    if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size()) &&
        y + 52.0f <= bottomLimit) {
        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
        drawTxt(isZh ? L"选中标注样式 (可在右侧画面直接拖拽移位)" : L"Selected Style (Drag on stage to move)",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colSecondary);
        y += 22.0f;

        const float dotStep = fullW / static_cast<float>(kAnnotationPaletteHex.size());
        for (size_t c = 0; c < kAnnotationPaletteHex.size(); ++c) {
            const float dcx = padL + dotStep * (static_cast<float>(c) + 0.5f);
            const float dcy = y + 10.0f;
            const D2D1_RECT_F hitRc = D2D1::RectF(dcx - 11.0f, dcy - 11.0f, dcx + 11.0f, dcy + 11.0f);
            m_hitZones.push_back({Hit_AnnColorBase + static_cast<int>(c), hitRc, static_cast<int>(c)});
            setBrush(hexToD2DColor(kAnnotationPaletteHex[c]));
            m_renderTarget->FillEllipse(D2D1::Ellipse({dcx, dcy}, 8.5f, 8.5f), brush);
            setBrush(ann.colorHex == kAnnotationPaletteHex[c] ? colAccent : colBorder);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({dcx, dcy}, 10.2f, 10.2f), brush,
                                        ann.colorHex == kAnnotationPaletteHex[c] ? 2.0f : 1.0f);
        }
        y += 26.0f;

        if (isTextLikeAnnotation(ann.kind) && y + 30.0f <= bottomLimit) {
            const D2D1_RECT_F txtBox = D2D1::RectF(padL, y, padR, y + 28.0f);
            m_hitZones.push_back({Hit_AnnTextInputBox, txtBox, -1});
            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(txtBox, 6.0f, 6.0f), brush);
            setBrush(m_editingAnnotationText ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(txtBox, 6.0f, 6.0f), brush, m_editingAnnotationText ? 1.5f : 1.0f);

            std::wstring shown = tools3000::core::WinUtils::utf8ToWstring(ann.text);
            if (m_editingAnnotationText && ((GetTickCount() / 500) % 2 == 0)) {
                shown.push_back(L'|');
            }
            drawTxt(shown.empty() ? (isZh ? L"点击输入文字..." : L"Click to type...") : shown,
                    m_bodyFmt.Get(), D2D1::RectF(txtBox.left + 8.0f, txtBox.top, txtBox.right - 8.0f, txtBox.bottom), colPrimary);
        }
    } else if (y + 24.0f <= bottomLimit) {
        drawTxt(isZh ? L"点击或拖拽图标到右侧画面即可添加" : L"Click or drag icon onto video stage",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 22.0f), colSecondary);
    }

    // 悬停在任意标注卡片上时，在该卡片上方弹出清晰精致的名称气泡标签
    if (!m_draggingPresetToStage && m_hoveredHitId >= Hit_AnnCardBase && m_hoveredHitId < Hit_AnnCardBase + 19) {
        const int cardIdx = m_hoveredHitId - Hit_AnnCardBase;
        D2D1_RECT_F targetRc{};
        bool found = false;
        for (const auto& hz : m_hitZones) {
            if (hz.id == m_hoveredHitId) {
                targetRc = hz.rect;
                found = true;
                break;
            }
        }
        if (found) {
            const std::wstring tipLabel = annotationCardDisplayName(cardIdx, isZh);
            float estW = 0.0f;
            for (wchar_t wc : tipLabel) estW += (wc > 0x7F) ? 12.6f : 7.2f;
            const float pillW = std::clamp(estW + 20.0f, 54.0f, fullW);
            const float pillH = 25.0f;
            const float acx = (targetRc.left + targetRc.right) * 0.5f;
            const float pillL = std::clamp(acx - pillW * 0.5f, padL - 4.0f, padR + 4.0f - pillW);
            const bool placeBelow = (targetRc.top - pillH - 6.0f < startY - 4.0f);
            const float pillT = placeBelow ? (targetRc.bottom + 6.0f) : (targetRc.top - pillH - 6.0f);
            const D2D1_RECT_F pillRc = D2D1::RectF(pillL, pillT, pillL + pillW, pillT + pillH);

            // 柔和悬浮投影 + 高对比微晶底衬
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, dark ? 0.45f : 0.18f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pillRc.left - 1.0f, pillRc.top + 2.0f, pillRc.right + 1.0f, pillRc.bottom + 3.0f), 7.0f, 7.0f),
                brush);

            const D2D1_COLOR_F tipBg = dark
                ? D2D1::ColorF(0.16f, 0.20f, 0.29f, 0.98f)
                : D2D1::ColorF(0.09f, 0.13f, 0.21f, 0.95f);
            setBrush(tipBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(pillRc, 6.0f, 6.0f), brush);

            const float tipX = std::clamp(acx, pillRc.left + 10.0f, pillRc.right - 10.0f);
            ComPtr<ID2D1PathGeometry> caret;
            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(caret.GetAddressOf())) && caret) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(caret->Open(sk.GetAddressOf())) && sk) {
                    if (placeBelow) {
                        sk->BeginFigure({tipX - 4.5f, pillRc.top + 0.5f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({tipX, pillRc.top - 4.5f});
                        sk->AddLine({tipX + 4.5f, pillRc.top + 0.5f});
                    } else {
                        sk->BeginFigure({tipX - 4.5f, pillRc.bottom - 0.5f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({tipX, pillRc.bottom + 4.5f});
                        sk->AddLine({tipX + 4.5f, pillRc.bottom - 0.5f});
                    }
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    m_renderTarget->FillGeometry(caret.Get(), brush);
                }
            }
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pillRc, 6.0f, 6.0f), brush, 0.9f);
            drawTxt(tipLabel, m_captionFmt.Get(), pillRc, D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void StudioPreviewWindow::renderWatermarkDrawer(
    float padL, float padR, float startY, float bottomLimit,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.10f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colFieldBg = dark ? D2D1::ColorF(0.065f, 0.082f, 0.122f, 0.96f) : D2D1::ColorF(0.948f, 0.960f, 0.980f, 1.0f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const float fullW = padR - padL;
    float y = startY;
    auto& wm = m_config.watermark;

    // 1. 顶部开关行（支持企业组策略只读锁定徽章）
    drawTxt(isZh ? L"启用画面水印" : L"Enable Watermark", m_sectionFmt.Get(),
            D2D1::RectF(padL, y, padR - 46.0f, y + 20.0f), colPrimary);
    {
        const D2D1_RECT_F swRc = D2D1::RectF(padR - 36.0f, y + 0.5f, padR, y + 19.5f);
        if (!wm.policyForced) {
            m_hitZones.push_back({Hit_WatermarkToggle, swRc, -1});
        }
        setBrush(wm.enabled ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.75f, 0.80f, 0.86f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(swRc, 9.5f, 9.5f), brush);
        const float kx = wm.enabled ? (swRc.right - 9.5f) : (swRc.left + 9.5f);
        setBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, y + 10.8f}, 7.0f, 7.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, wm.policyForced ? 0.78f : 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, y + 10.0f}, 6.8f, 6.8f), brush);
    }
    y += 25.0f;

    if (wm.policyForced) {
        drawTxt(isZh ? L"[企业合规策略已强制锁定开启]" : L"[Enforced by Enterprise Policy]",
                m_captionFmt.Get(), D2D1::RectF(padL, y - 3.0f, padR, y + 15.0f), colAccent);
        y += 18.0f;
    }

    // 2. 一体式凹槽分段器：图片 Logo | 文字角标 | 企业平铺
    {
        const D2D1_RECT_F trough = D2D1::RectF(padL, y, padR, y + 28.0f);
        setBrush(dark ? D2D1::ColorF(0.045f, 0.058f, 0.090f, 0.95f) : D2D1::ColorF(0.918f, 0.935f, 0.962f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 7.5f, 7.5f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trough, 7.5f, 7.5f), brush, 0.8f);

        static const wchar_t* modeZh[3] = { L"图片 Logo", L"文字角标", L"企业平铺" };
        static const wchar_t* modeEn[3] = { L"Logo Img", L"Corner Txt", L"Tiled DLP" };
        const float cellW = (fullW - 6.0f) / 3.0f;
        for (int m = 0; m < 3; ++m) {
            const D2D1_RECT_F crc = D2D1::RectF(
                trough.left + 3.0f + m * cellW, trough.top + 2.5f,
                trough.left + 3.0f + (m + 1) * cellW, trough.bottom - 2.5f);
            m_hitZones.push_back({Hit_WatermarkModeBase + m, crc, m});
            const bool act = (static_cast<int>(wm.mode) == m);
            if (act) {
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.36f : 0.09f));
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(crc.left, crc.top + 1.0f, crc.right, crc.bottom + 1.2f), 5.5f, 5.5f), brush);
                fillLinearVGradRounded(m_renderTarget.Get(), crc, 5.5f,
                    dark ? D2D1::ColorF(0.165f, 0.205f, 0.295f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.130f, 0.165f, 0.245f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(crc, 5.5f, 5.5f), brush, 0.9f);
            }
            drawTxt(isZh ? modeZh[m] : modeEn[m], m_captionFmt.Get(), crc,
                    act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    y += 35.0f;

    if (wm.mode == WatermarkMode::ImageLogo) {
        // 导入图片按钮 + 当前 Logo 文件名展示
        const D2D1_RECT_F pickBtn = D2D1::RectF(padL, y, padR, y + 32.0f);
        m_hitZones.push_back({Hit_WatermarkPickImage, pickBtn, -1});
        const bool hovPick = (m_hoveredHitId == Hit_WatermarkPickImage);
        fillLinearVGradRounded(m_renderTarget.Get(), pickBtn, 7.0f,
            dark ? D2D1::ColorF(0.14f, 0.18f, 0.27f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
            dark ? D2D1::ColorF(0.11f, 0.14f, 0.22f, 1.0f) : D2D1::ColorF(0.965f, 0.978f, 0.996f, 1.0f));
        setBrush(hovPick ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pickBtn, 7.0f, 7.0f), brush, hovPick ? 1.4f : 1.0f);

        std::wstring btnText = isZh ? L"+ 导入透明图片 / 企业 Logo" : L"+ Import Transparent Logo";
        if (!wm.imagePathUtf8.empty()) {
            const std::wstring fname = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(wm.imagePathUtf8)).filename().wstring();
            if (!fname.empty()) {
                btnText = (isZh ? L"更换图片: " : L"Change: ") + fname;
            }
        }
        drawTxt(btnText, m_captionFmt.Get(), D2D1::RectF(pickBtn.left + 10.0f, pickBtn.top, pickBtn.right - 10.0f, pickBtn.bottom),
                hovPick ? colAccent : colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 39.0f;

        // 历史常用 Logo 快捷缩略图宫格 (最多 6 张) 或空状态引导提示
        if (!m_recentLogoPaths.empty()) {
            drawTxt(isZh ? L"最近使用的图片徽标" : L"Recent Logos", m_captionFmt.Get(),
                    D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
            y += 20.0f;
            const float gap = 6.0f;
            const float cellSz = (fullW - gap * 5.0f) / 6.0f;
            for (size_t i = 0; i < m_recentLogoPaths.size() && i < 6; ++i) {
                const float lx = padL + static_cast<float>(i) * (cellSz + gap);
                const D2D1_RECT_F trc = D2D1::RectF(lx, y, lx + cellSz, y + cellSz);
                const int hId = Hit_WatermarkRecentBase + static_cast<int>(i);
                m_hitZones.push_back({hId, trc, static_cast<int>(i)});

                setBrush(colFieldBg);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush);
                if (m_recentLogoThumbBitmaps[i]) {
                    const D2D1_RECT_F imgRc = D2D1::RectF(trc.left + 3.0f, trc.top + 3.0f, trc.right - 3.0f, trc.bottom - 3.0f);
                    m_renderTarget->DrawBitmap(m_recentLogoThumbBitmaps[i].Get(), imgRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                const bool sel = (wm.imagePathUtf8 == m_recentLogoPaths[i]);
                setBrush(sel ? colAccent : (m_hoveredHitId == hId ? colAccent : colBorder));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, sel ? 1.8f : 1.0f);
            }
            y += cellSz + 10.0f;
        } else if (wm.imagePathUtf8.empty()) {
            drawTxt(isZh ? L"[提示] 请先点击上方按钮选择 PNG / WebP 水印图片" : L"[Info] Import a PNG / WebP image above first",
                    m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 19.0f), colAccent);
            y += 24.0f;
        }
    } else {
        // 文字模板编辑框 + 动态占位符智能双向切换胶囊
        drawTxt(isZh ? L"水印文字内容 (支持动态变量)" : L"Watermark Text & Tokens", m_captionFmt.Get(),
                D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
        y += 20.0f;

        const D2D1_RECT_F txtBox = D2D1::RectF(padL, y, padR, y + 30.0f);
        m_hitZones.push_back({Hit_WatermarkTextInput, txtBox, -1});
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(txtBox, 6.5f, 6.5f), brush);
        setBrush(m_editingWatermarkText ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(txtBox, 6.5f, 6.5f), brush, m_editingWatermarkText ? 1.5f : 1.0f);

        std::wstring shown = tools3000::core::WinUtils::utf8ToWstring(wm.textTemplate);
        {
            size_t nlPos = 0;
            while ((nlPos = shown.find(L'\n', nlPos)) != std::wstring::npos) {
                shown.replace(nlPos, 1, L"\\n");
                nlPos += 2;
            }
        }
        if (m_editingWatermarkText && ((GetTickCount() / 500) % 2 == 0)) {
            shown.push_back(L'|');
        }
        drawTxt(shown.empty() ? (isZh ? L"点击输入文字或点下方变量..." : L"Type text or toggle token...") : shown,
                m_bodyFmt.Get(), D2D1::RectF(txtBox.left + 8.0f, txtBox.top, txtBox.right - 8.0f, txtBox.bottom), colPrimary);
        y += 35.0f;

        // 4 枚智能变量胶囊（已包含时高亮激活，点击一键添加或移除）
        static const wchar_t* tokLblZh[4] = { L"+用户名", L"+设备名", L"+日期时间", L"+严禁外传" };
        static const wchar_t* tokActZh[4] = { L"用户名", L"设备名", L"日期时间", L"严禁外传" };
        static const wchar_t* tokLblEn[4] = { L"+User", L"+Host", L"+Date", L"+Confid." };
        static const wchar_t* tokActEn[4] = { L"User", L"Host", L"Date", L"Confid." };
        auto hasTokenInTemplate = [&](int idx) -> bool {
            const std::string& s = wm.textTemplate;
            if (idx == 0) return s.find("{user}") != std::string::npos || s.find("{USER}") != std::string::npos;
            if (idx == 1) return s.find("{host}") != std::string::npos || s.find("{HOST}") != std::string::npos;
            if (idx == 2) return s.find("{date}") != std::string::npos || s.find("{time}") != std::string::npos || s.find("{datetime}") != std::string::npos;
            return s.find("严禁外传") != std::string::npos || s.find("CONFIDENTIAL") != std::string::npos;
        };
        const float tGap = 4.5f;
        const float tw = (fullW - tGap * 3.0f) / 4.0f;
        for (int k = 0; k < 4; ++k) {
            const D2D1_RECT_F krc = D2D1::RectF(padL + k * (tw + tGap), y, padL + k * (tw + tGap) + tw, y + 24.0f);
            const int hId = Hit_WatermarkTokenBase + k;
            m_hitZones.push_back({hId, krc, k});
            const bool tokOn = hasTokenInTemplate(k);
            const bool hov = (m_hoveredHitId == hId);
            setBrush(tokOn
                ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.24f : 0.13f)
                : (hov ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 0.10f) : colFieldBg));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(krc, 5.5f, 5.5f), brush);
            setBrush((tokOn || hov) ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(krc, 5.5f, 5.5f), brush, tokOn ? 1.2f : 0.9f);
            const wchar_t* label = tokOn ? (isZh ? tokActZh[k] : tokActEn[k])
                                         : (isZh ? tokLblZh[k] : tokLblEn[k]);
            drawTxt(label, m_captionFmt.Get(), krc,
                    (tokOn || hov) ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += 29.0f;

        // 换行符插入 + 企业双排预设（第一行账号 / 第二行日期时间）
        const float nlW = (fullW - tGap) * 0.42f;
        const D2D1_RECT_F nlRc = D2D1::RectF(padL, y, padL + nlW, y + 24.0f);
        const D2D1_RECT_F dualRc = D2D1::RectF(padL + nlW + tGap, y, padR, y + 24.0f);
        m_hitZones.push_back({Hit_WatermarkNewLine, nlRc, 0});
        m_hitZones.push_back({Hit_WatermarkPresetDualLine, dualRc, 0});
        const bool isDualLine = (wm.textTemplate.find("\\n") != std::string::npos || wm.textTemplate.find('\n') != std::string::npos);
        const bool nlHov = (m_hoveredHitId == Hit_WatermarkNewLine);
        const bool dualHov = (m_hoveredHitId == Hit_WatermarkPresetDualLine);

        setBrush(nlHov ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 0.10f) : colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(nlRc, 5.5f, 5.5f), brush);
        setBrush(nlHov ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(nlRc, 5.5f, 5.5f), brush, 0.9f);
        drawTxt(isZh ? L"+换行 (\\n)" : L"+Newline (\\n)", m_captionFmt.Get(), nlRc,
                nlHov ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

        setBrush(isDualLine
            ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.24f : 0.13f)
            : (dualHov ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 0.10f) : colFieldBg));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(dualRc, 5.5f, 5.5f), brush);
        setBrush((isDualLine || dualHov) ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(dualRc, 5.5f, 5.5f), brush, isDualLine ? 1.2f : 0.9f);
        drawTxt(isZh ? L"企业双行 (账号/时间)" : L"2-Line (User / Time)", m_captionFmt.Get(), dualRc,
                (isDualLine || dualHov) ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 31.0f;
    }

    // 3. 锚定位置一体式分段器（仅在单点图片或文字角标模式下展示，同时支持右侧画面直接拖拽自由定位）
    if (wm.mode != WatermarkMode::TiledTraceability) {
        drawTxt(isZh ? L"对齐位置 (亦可直接在右侧画面拖拽)" : L"Position (Or drag directly on stage)",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
        y += 20.0f;
        const D2D1_RECT_F trough = D2D1::RectF(padL, y, padR, y + 26.0f);
        setBrush(dark ? D2D1::ColorF(0.045f, 0.058f, 0.090f, 0.95f) : D2D1::ColorF(0.918f, 0.935f, 0.962f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 6.5f, 6.5f), brush);

        static const wchar_t* posZh[5] = { L"左上", L"右上", L"左下", L"右下", L"居中" };
        static const wchar_t* posEn[5] = { L"TL", L"TR", L"BL", L"BR", L"Mid" };
        const float pw = (fullW - 5.0f) / 5.0f;
        for (int p = 0; p < 5; ++p) {
            const D2D1_RECT_F prc = D2D1::RectF(trough.left + 2.5f + p * pw, trough.top + 2.2f,
                                                trough.left + 2.5f + (p + 1) * pw, trough.bottom - 2.2f);
            m_hitZones.push_back({Hit_WatermarkAnchorBase + p, prc, p});
            const bool act = (static_cast<int>(wm.anchor) == p);
            if (act) {
                fillLinearVGradRounded(m_renderTarget.Get(), prc, 4.8f,
                    dark ? D2D1::ColorF(0.165f, 0.205f, 0.295f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.130f, 0.165f, 0.245f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
                setBrush(colBorder);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(prc, 4.8f, 4.8f), brush, 0.85f);
            }
            drawTxt(isZh ? posZh[p] : posEn[p], m_captionFmt.Get(), prc,
                    act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += 33.0f;
    }

    // 4. 大小比例与不透明度滑块
    auto drawWmSlider = [&](SliderDragKind kind, int hitId, const std::wstring& lbl, float norm01, const std::wstring& vStr) {
        drawTxt(lbl, m_captionFmt.Get(), D2D1::RectF(padL, y, padL + 62.0f, y + 20.0f), colSecondary);
        const D2D1_RECT_F trk = D2D1::RectF(padL + 66.0f, y + 8.0f, padR - 48.0f, y + 12.0f);
        m_sliderTrackRects[static_cast<size_t>(kind)] = trk;
        m_hitZones.push_back({hitId, D2D1::RectF(trk.left - 5.0f, y, trk.right + 5.0f, y + 20.0f), -1});

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.09f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trk, 2.0f, 2.0f), brush);
        const float cl = std::clamp(norm01, 0.0f, 1.0f);
        const float kx = trk.left + (trk.right - trk.left) * cl;
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, (std::max)(trk.left + 2.0f, kx), trk.bottom), 2.0f, 2.0f), brush);

        // 白瓷立体圆珠旋钮
        const float ky = (trk.top + trk.bottom) * 0.5f;
        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, 0.22f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky + 1.0f}, 6.2f, 6.2f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky}, 5.8f, 5.8f), brush);
        setBrush(colAccent);
        m_renderTarget->DrawEllipse(D2D1::Ellipse({kx, ky}, 5.8f, 5.8f), brush, 1.4f);

        drawTxt(vStr, m_monoFmt.Get(), D2D1::RectF(padR - 44.0f, y, padR, y + 20.0f), colPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 26.0f;
    };

    wchar_t sVal[24]{};
    std::swprintf(sVal, std::size(sVal), L"%d%%", static_cast<int>(std::round(wm.scale * 100.0f)));
    drawWmSlider(SliderDragKind::WatermarkScale, Hit_WatermarkSliderScale,
                 isZh ? L"水印大小" : L"Scale", (wm.scale - 0.35f) / (2.80f - 0.35f), sVal);

    std::swprintf(sVal, std::size(sVal), L"%d%%", static_cast<int>(std::round(wm.opacity * 100.0f)));
    drawWmSlider(SliderDragKind::WatermarkOpacity, Hit_WatermarkSliderOpacity,
                 isZh ? L"不透明度" : L"Opacity", (wm.opacity - 0.04f) / (1.0f - 0.04f), sVal);

    if (y + 36.0f <= bottomLimit) {
        y += 4.0f;
        const std::wstring hint = (wm.mode == WatermarkMode::TiledTraceability)
            ? (isZh ? L"平铺水印可自动适应深色与浅色画面背景，始终保持清晰柔和，且固定在顶层不随镜头缩放晃动。"
                    : L"Tiled watermark automatically adapts to both light and dark backgrounds, staying clear, subtle, and fixed above camera zoom.")
            : (isZh ? L"可在右侧预览画面直接按住鼠标拖拽水印至任意位置，或拖拽水印右下角控制点无级缩放；导出 MP4 与 GIF 均完整保留。"
                    : L"Drag the watermark directly on the preview stage to reposition, or drag its bottom-right handle to resize.");
        drawTxt(hint, m_captionWrapFmt.Get(), D2D1::RectF(padL, y, padR, bottomLimit), colSecondary);
    }
}

void StudioPreviewWindow::renderLeftRailAndDrawer(
    const D2D1_RECT_F& railRc, const D2D1_RECT_F& drawerRc,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colAccentSoft = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.20f : 0.11f);
    const D2D1_COLOR_F colCardBg = dark ? D2D1::ColorF(0.092f, 0.114f, 0.165f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F colRailBg = dark ? D2D1::ColorF(0.068f, 0.084f, 0.125f, 1.0f) : D2D1::ColorF(0.942f, 0.955f, 0.978f, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.095f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colChipBg = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.05f);

    const ComPtr<ID2D1StrokeStyle> roundSt = createRoundStrokeStyle(m_d2dFactory.Get());

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    setBrush(colRailBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(railRc, 12.0f, 12.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(railRc, 12.0f, 12.0f), brush, 1.0f);

    static const wchar_t* tabShortZh[5] = { L"背景", L"标注", L"运镜", L"水印", L"字幕" };
    static const wchar_t* tabShortEn[5] = { L"Canvas", L"Mark", L"Cam", L"Brand", L"Sub" };
    for (int t = 0; t < 5; ++t) {
        const float by = railRc.top + 8.0f + t * 53.0f;
        const D2D1_RECT_F brc = D2D1::RectF(railRc.left + 4.0f, by, railRc.right - 4.0f, by + 46.0f);
        const int hId = Hit_LeftRailTabBase + t;
        m_hitZones.push_back({hId, brc, t});

        const bool active = (static_cast<int>(m_activeTab) == t);
        if (active) {
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.34f : 0.08f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(brc.left, brc.top + 1.2f, brc.right, brc.bottom + 1.5f), 8.5f, 8.5f), brush);
            fillLinearVGradRounded(m_renderTarget.Get(), brc, 8.5f,
                dark ? D2D1::ColorF(0.155f, 0.195f, 0.285f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(0.120f, 0.155f, 0.235f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(brc, 8.5f, 8.5f), brush, 0.9f);
            // 左侧 Linear 级垂直发光指示胶囊条
            setBrush(colAccent);
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(brc.left + 0.5f, brc.top + 11.0f, brc.left + 3.1f, brc.bottom - 11.0f), 1.3f, 1.3f), brush);
        } else if (m_hoveredHitId == hId) {
            setBrush(colChipBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.5f, 8.5f), brush);
        }

        const float icx = (brc.left + brc.right) * 0.5f;
        const float icy = brc.top + 14.5f;
        setBrush(active ? colAccent : colSecondary);
        if (t == 0) {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 7.8f, icy - 6.2f, icx + 7.8f, icy + 6.2f), 2.5f, 2.5f), brush, 1.55f, roundSt.Get());
            m_renderTarget->FillEllipse(D2D1::Ellipse({icx - 3.0f, icy - 2.0f}, 1.5f, 1.5f), brush);
            m_renderTarget->DrawLine({icx - 5.8f, icy + 4.8f}, {icx - 0.8f, icy + 0.4f}, brush, 1.45f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 0.8f, icy + 0.4f}, {icx + 5.8f, icy + 4.8f}, brush, 1.45f, roundSt.Get());
        } else if (t == 1) {
            drawCrispPenIcon(m_d2dFactory.Get(), m_renderTarget.Get(), brush, icx, icy, roundSt.Get());
        } else if (t == 2) {
            drawCrispCameraFocusIcon(m_renderTarget.Get(), brush, icx, icy, roundSt.Get());
        } else if (t == 3) {
            // 精密企业盾徽/印章矢量图标 (Watermark / Brand Shield)
            ComPtr<ID2D1PathGeometry> shGeo;
            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(&shGeo)) && shGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(shGeo->Open(&sk)) && sk) {
                    sk->BeginFigure({icx, icy - 6.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddLine({icx + 6.5f, icy - 4.2f});
                    sk->AddLine({icx + 5.8f, icy + 1.8f});
                    sk->AddLine({icx, icy + 6.8f});
                    sk->AddLine({icx - 5.8f, icy + 1.8f});
                    sk->AddLine({icx - 6.5f, icy - 4.2f});
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    m_renderTarget->DrawGeometry(shGeo.Get(), brush, 1.55f, roundSt.Get());
                    m_renderTarget->DrawLine({icx - 2.4f, icy + 0.2f}, {icx - 0.4f, icy + 2.2f}, brush, 1.45f, roundSt.Get());
                    m_renderTarget->DrawLine({icx - 0.4f, icy + 2.2f}, {icx + 2.8f, icy - 1.4f}, brush, 1.45f, roundSt.Get());
                }
            }
            if (m_config.watermark.enabled) {
                setBrush(D2D1::ColorF(0.10f, 0.78f, 0.52f, 1.0f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({brc.right - 7.0f, brc.top + 7.5f}, 2.6f, 2.6f), brush);
            }
        } else {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 7.8f, icy - 5.8f, icx + 7.8f, icy + 5.8f), 2.5f, 2.5f), brush, 1.5f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 4.5f, icy + 1.6f}, {icx + 4.5f, icy + 1.6f}, brush, 1.5f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 4.5f, icy - 1.6f}, {icx + 1.5f, icy - 1.6f}, brush, 1.4f, roundSt.Get());
        }
        drawTxt(isZh ? tabShortZh[t] : tabShortEn[t], m_captionFmt.Get(),
                D2D1::RectF(brc.left, brc.bottom - 17.5f, brc.right, brc.bottom - 1.5f),
                active ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(drawerRc, 12.0f, 12.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(drawerRc, 12.0f, 12.0f), brush, 1.0f);

    const float padL = drawerRc.left + 13.0f;
    const float padR = drawerRc.right - 13.0f;
    const float fullW = padR - padL;
    float curY = drawerRc.top + 11.0f;

    auto drawTogglePill = [&](int hitId, float rightEdge, float topY, bool on) {
        const D2D1_RECT_F rc = D2D1::RectF(rightEdge - 36.0f, topY, rightEdge, topY + 19.0f);
        m_hitZones.push_back({hitId, rc, -1});
        setBrush(on ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.76f, 0.81f, 0.87f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.5f, 9.5f), brush);
        const float knobX = on ? (rc.right - 9.5f) : (rc.left + 9.5f);
        setBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({knobX, topY + 10.4f}, 7.0f, 7.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({knobX, topY + 9.5f}, 6.8f, 6.8f), brush);
    };

    // 微晶陶瓷次级独立按钮 / 卡片
    auto drawOptionChip = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label, bool active) {
        m_hitZones.push_back({hitId, rc, -1});
        const bool hov = (m_hoveredHitId == hitId);
        if (active) {
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.30f : 0.07f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rc.left, rc.top + 1.0f, rc.right, rc.bottom + 1.2f), 6.0f, 6.0f), brush);
            fillLinearVGradRounded(m_renderTarget.Get(), rc, 6.0f,
                dark ? D2D1::ColorF(accentRgb.r * 0.32f + 0.10f, accentRgb.g * 0.32f + 0.12f, accentRgb.b * 0.32f + 0.18f, 0.98f)
                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(accentRgb.r * 0.24f + 0.08f, accentRgb.g * 0.24f + 0.10f, accentRgb.b * 0.24f + 0.15f, 0.98f)
                     : D2D1::ColorF(0.965f, 0.978f, 1.0f, 1.0f));
            setBrush(colAccent);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush, 1.25f);
        } else {
            fillLinearVGradRounded(m_renderTarget.Get(), rc, 6.0f,
                dark ? D2D1::ColorF(0.125f, 0.152f, 0.215f, hov ? 0.95f : 0.75f)
                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, hov ? 1.0f : 0.85f),
                dark ? D2D1::ColorF(0.102f, 0.125f, 0.182f, hov ? 0.95f : 0.75f)
                     : D2D1::ColorF(0.960f, 0.970f, 0.986f, hov ? 1.0f : 0.85f));
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush, 0.9f);
        }
        drawTxt(label, m_captionFmt.Get(), rc, active ? (dark ? colPrimary : colAccent) : (hov ? colPrimary : colSecondary),
                DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    // macOS Sequoia / Linear 级一体式凹槽分段控制器 (Unified Recessed Segmented Control)
    auto drawSegmentedGroup = [&](float topY, float height, int count,
                                  const int* hitIds, const wchar_t* const* labels, int activeIndex) {
        const D2D1_RECT_F trough = D2D1::RectF(padL, topY, padR, topY + height);
        setBrush(dark ? D2D1::ColorF(0.048f, 0.060f, 0.094f, 0.96f) : D2D1::ColorF(0.922f, 0.938f, 0.964f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 7.0f, 7.0f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trough, 7.0f, 7.0f), brush, 0.8f);

        const float innerPad = 2.4f;
        const float cellW = (fullW - innerPad * 2.0f) / static_cast<float>((std::max)(1, count));
        for (int i = 0; i < count; ++i) {
            const D2D1_RECT_F crc = D2D1::RectF(
                trough.left + innerPad + i * cellW, trough.top + innerPad,
                trough.left + innerPad + (i + 1) * cellW, trough.bottom - innerPad);
            m_hitZones.push_back({hitIds[i], crc, i});
            const bool act = (i == activeIndex);
            if (act) {
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.38f : 0.09f));
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(crc.left, crc.top + 1.0f, crc.right, crc.bottom + 1.2f), 5.2f, 5.2f), brush);
                fillLinearVGradRounded(m_renderTarget.Get(), crc, 5.2f,
                    dark ? D2D1::ColorF(0.172f, 0.214f, 0.308f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.135f, 0.170f, 0.252f, 1.0f) : D2D1::ColorF(0.980f, 0.988f, 1.0f, 1.0f));
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.085f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(crc, 5.2f, 5.2f), brush, 0.85f);
            } else if (m_hoveredHitId == hitIds[i]) {
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(crc, 5.2f, 5.2f), brush);
            }
            drawTxt(labels[i], m_captionFmt.Get(), crc, act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };

    auto drawSliderRow = [&](SliderDragKind kind, int hitId, float topY,
                             const std::wstring& label, float norm01, const std::wstring& valStr,
                             bool showBorderColorDot = false) {
        drawTxt(label, m_captionFmt.Get(), D2D1::RectF(padL, topY, padL + 60.0f, topY + 20.0f), colSecondary);
        float trackL = padL + 64.0f;
        if (showBorderColorDot) {
            const float dcx = padL + 54.0f;
            const float dcy = topY + 10.0f;
            m_hitZones.push_back({Hit_CycleInsetBorderColor, D2D1::RectF(dcx - 9.0f, dcy - 9.0f, dcx + 9.0f, dcy + 9.0f), -1});
            setBrush(hexToD2DColor(m_config.insetBorderColorHex));
            m_renderTarget->FillEllipse(D2D1::Ellipse({dcx, dcy}, 5.5f, 5.5f), brush);
            setBrush(colBorder);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({dcx, dcy}, 6.6f, 6.6f), brush, 1.1f);
            trackL = padL + 68.0f;
        }
        const D2D1_RECT_F trk = D2D1::RectF(trackL, topY + 8.2f, padR - 48.0f, topY + 11.8f);
        m_sliderTrackRects[static_cast<size_t>(kind)] = trk;
        m_hitZones.push_back({hitId, D2D1::RectF(trk.left - 5.0f, topY, trk.right + 5.0f, topY + 20.0f), -1});

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.085f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trk, 1.8f, 1.8f), brush);
        const float clamped = std::clamp(norm01, 0.0f, 1.0f);
        const float kx = trk.left + (trk.right - trk.left) * clamped;
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, (std::max)(trk.left + 1.8f, kx), trk.bottom), 1.8f, 1.8f), brush);

        // 双层微投影白瓷圆珠旋钮 (White Porcelain Thumb)
        const float ky = (trk.top + trk.bottom) * 0.5f;
        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.36f : 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky + 1.0f}, 6.0f, 6.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky}, 5.5f, 5.5f), brush);
        setBrush(m_activeSlider == kind ? colAccent : (dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0.10f, 0.16f, 0.28f, 0.24f)));
        m_renderTarget->DrawEllipse(D2D1::Ellipse({kx, ky}, 5.5f, 5.5f), brush, m_activeSlider == kind ? 1.5f : 1.0f);

        drawTxt(valStr, m_monoFmt.Get(), D2D1::RectF(padR - 44.0f, topY, padR, topY + 20.0f),
                colPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };

    if (m_activeTab == LeftRailTab::Annotations) {
        renderAnnotationDrawer(padL, padR, curY, drawerRc.bottom - 10.0f, dark, isZh, brush);
        return;
    }

    if (m_activeTab == LeftRailTab::Watermark) {
        renderWatermarkDrawer(padL, padR, curY, drawerRc.bottom - 10.0f, dark, isZh, brush);
        return;
    }

    if (m_activeTab == LeftRailTab::CameraCursor) {
        drawTxt(isZh ? L"鼠标自动运镜" : L"Auto Zoom", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
        curY += 22.0f;
        {
            const int ids[3] = { Hit_ZoomMode_None, Hit_ZoomMode_2D, Hit_ZoomMode_3D };
            static const wchar_t* lblZh[3] = { L"关闭", L"2D 运镜", L"3D 运镜" };
            static const wchar_t* lblEn[3] = { L"Off", L"2D Zoom", L"3D Motion" };
            const int actIdx = (!m_config.autoZoomEnabled || m_config.zoomMode == StudioZoomMode::None)
                ? 0 : (m_config.zoomMode == StudioZoomMode::Motion3D ? 2 : 1);
            drawSegmentedGroup(curY, 26.0f, 3, ids, isZh ? lblZh : lblEn, actIdx);
        }
        curY += 32.0f;

        drawTxt(isZh ? L"运镜档位" : L"Preset", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
        curY += 21.0f;
        {
            const int ids[3] = { Hit_ZoomPreset_Gentle, Hit_ZoomPreset_Balanced, Hit_ZoomPreset_Dynamic };
            static const wchar_t* lblZh[3] = { L"柔和", L"标准", L"动感" };
            static const wchar_t* lblEn[3] = { L"Gentle", L"Balanced", L"Dynamic" };
            const int actIdx = detectStudioZoomPreset(m_config);
            drawSegmentedGroup(curY, 25.0f, 3, ids, isZh ? lblZh : lblEn, actIdx);
        }
        curY += 31.0f;

        const bool editingSelectedZoomSeg =
            (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size()));
        const float shownZoom = editingSelectedZoomSeg
            ? (m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)].manualInserted
                   ? m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)].targetZoom
                   : m_config.defaultZoomFactor)
            : m_config.defaultZoomFactor;
        wchar_t zvBuf[32]{};
        std::swprintf(zvBuf, std::size(zvBuf), L"%.2fx", std::clamp(shownZoom, 1.00f, 2.60f));
        drawSliderRow(SliderDragKind::ZoomFactor, Hit_SliderZoomFactor, curY,
                      editingSelectedZoomSeg ? (isZh ? L"此段倍率" : L"Seg Zoom")
                                             : (isZh ? L"放大倍率" : L"Zoom"),
                      (std::clamp(shownZoom, 1.00f, 2.60f) - 1.00f) / 1.60f, zvBuf);
        curY += 22.5f;

        std::swprintf(zvBuf, std::size(zvBuf), L"%d%%", static_cast<int>(std::round(m_config.zoomPacing * 100.0f)));
        drawSliderRow(SliderDragKind::ZoomPacing, Hit_SliderZoomPacing, curY,
                      isZh ? L"运镜节奏" : L"Pacing", m_config.zoomPacing, zvBuf);
        curY += 22.5f;

        std::swprintf(zvBuf, std::size(zvBuf), L"%d%%", static_cast<int>(std::round(m_config.autoZoomSensitivity * 100.0f)));
        drawSliderRow(SliderDragKind::ZoomSensitivity, Hit_SliderZoomSensitivity, curY,
                      isZh ? L"触发频率" : L"Density", m_config.autoZoomSensitivity, zvBuf);
        curY += 22.5f;

        std::swprintf(zvBuf, std::size(zvBuf), L"%d%%", static_cast<int>(std::round(m_config.focusSpotlightDim * 100.0f)));
        drawSliderRow(SliderDragKind::FocusSpotlight, Hit_SliderFocusSpotlight, curY,
                      isZh ? L"聚焦暗角" : L"Dimming", m_config.focusSpotlightDim / 0.65f, zvBuf);
        curY += 25.0f;

        drawTxt(isZh ? L"锁定视角 (防晃)" : L"Lock View (Steady)", m_bodyFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleLockCamera, padR, curY, m_config.lockCameraWhileZoomed);
        curY += 25.0f;

        drawTxt(isZh ? L"推拉运动模糊" : L"Motion Blur", m_bodyFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleMotionBlur, padR, curY, m_config.motionBlurEnabled);
        curY += 26.0f;

        setBrush(colBorder);
        m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
        curY += 10.0f;

        drawTxt(isZh ? L"平滑矢量光标" : L"Smooth Vector Cursor", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleSmoothCursor, padR, curY, m_config.smoothCursorEnabled);
        curY += 23.0f;
        {
            const int ids[3] = { Hit_CursorScale_100, Hit_CursorScale_135, Hit_CursorScale_170 };
            static const wchar_t* lbls[3] = { L"1.0x", L"1.35x", L"1.7x" };
            const int actIdx = !m_config.smoothCursorEnabled ? -1
                : (std::abs(m_config.cursorScale - 1.00f) < 0.15f ? 0
                   : (std::abs(m_config.cursorScale - 1.70f) < 0.15f ? 2 : 1));
            drawSegmentedGroup(curY, 25.0f, 3, ids, lbls, actIdx);
        }
        curY += 31.0f;

        drawTxt(isZh ? L"光标聚光环跟随" : L"Cursor Highlight Halo", m_bodyFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCursorHalo, padR, curY, m_config.cursorHighlightHalo);
        curY += 26.0f;

        setBrush(colBorder);
        m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
        curY += 10.0f;

        drawTxt(isZh ? L"鼠标点击动效" : L"Click Effect", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
        curY += 22.0f;
        {
            const float w2 = (fullW - 6.0f) * 0.5f;
            const StudioClickEffectStyle effStyle = m_config.showClickRipple
                ? m_config.clickEffectStyle
                : StudioClickEffectStyle::TactileOnly;
            drawOptionChip(Hit_ClickStyleBase + 0,
                           D2D1::RectF(padL, curY, padL + w2, curY + 25.0f),
                           isZh ? L"微晶单波 (默认)" : L"Crystal (Def)",
                           effStyle == StudioClickEffectStyle::CrystalPulse);
            drawOptionChip(Hit_ClickStyleBase + 1,
                           D2D1::RectF(padL + w2 + 6.0f, curY, padR, curY + 25.0f),
                           isZh ? L"柔雾触点" : L"Soft Touch",
                           effStyle == StudioClickEffectStyle::SoftTouch);
            curY += 30.0f;
            drawOptionChip(Hit_ClickStyleBase + 2,
                           D2D1::RectF(padL, curY, padL + w2, curY + 25.0f),
                           isZh ? L"错峰涟漪" : L"Dual Ripple",
                           effStyle == StudioClickEffectStyle::DualRipple);
            drawOptionChip(Hit_ClickStyleBase + 3,
                           D2D1::RectF(padL + w2 + 6.0f, curY, padR, curY + 25.0f),
                           isZh ? L"仅物理按压" : L"Tactile Only",
                           effStyle == StudioClickEffectStyle::TactileOnly);
            curY += 30.0f;
        }
        if (curY + 34.0f <= drawerRc.bottom) {
            drawTxt(isZh ? L"可将倍率拉至 1.00x 并调节聚焦暗角，实现零放大静稳聚焦。"
                         : L"Set zoom to 1.00x with dimming for steady zero-zoom spotlight focus.",
                    m_captionWrapFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 34.0f), colSecondary);
        }
        return;
    }

    if (m_activeTab == LeftRailTab::Captions) {
        drawTxt(isZh ? L"离线语音字幕对齐" : L"Offline Auto Captions", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCaptions, padR, curY, m_config.autoCaptionsEnabled);
        curY += 30.0f;
        drawTxt(isZh ? L"开启麦克风录制时，自动识别语音生成底部字幕并导出同名 .srt 文件。"
                     : L"Transcribes mic audio into synced bottom captions and .srt file.",
                m_captionWrapFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 46.0f), colSecondary);
        return;
    }

    drawTxt(isZh ? L"视频尺寸" : L"Video Size", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
    curY += 22.0f;
    {
        const int aspIds[5] = { Hit_AspectBase + 0, Hit_AspectBase + 1, Hit_AspectBase + 2, Hit_AspectBase + 3, Hit_AspectBase + 4 };
        static const wchar_t* aspZh[5] = { L"原始", L"16:9", L"9:16", L"4:3", L"1:1" };
        static const wchar_t* aspEn[5] = { L"Orig", L"16:9", L"9:16", L"4:3", L"1:1" };
        drawSegmentedGroup(curY, 25.0f, 5, aspIds, isZh ? aspZh : aspEn, static_cast<int>(m_config.aspectRatio));
    }
    curY += 30.0f;

    drawTxt(isZh ? L"样式" : L"Style", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padR - 44.0f, curY + 19.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoFrame, padR, curY, m_config.autoFrameEnabled);
    curY += 23.0f;

    wchar_t vBuf[32]{};
    std::swprintf(vBuf, std::size(vBuf), L"%d%%", static_cast<int>(std::round(m_config.paddingRatio * 100.0f)));
    drawSliderRow(SliderDragKind::Padding, Hit_SliderPadding, curY, isZh ? L"内边距" : L"Padding", m_config.paddingRatio / 0.22f, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.insetBorderWidth)));
    drawSliderRow(SliderDragKind::InsetBorder, Hit_SliderInsetBorder, curY, isZh ? L"水晶框" : L"Glass", m_config.insetBorderWidth / 16.0f, vBuf, true);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.cornerRadius)));
    drawSliderRow(SliderDragKind::Radius, Hit_SliderRadius, curY, isZh ? L"圆角" : L"Radius", m_config.cornerRadius / 36.0f, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%d%%", static_cast<int>(std::round(m_config.shadowIntensity * 100.0f)));
    drawSliderRow(SliderDragKind::Shadow, Hit_SliderShadow, curY, isZh ? L"阴影" : L"Shadow", m_config.shadowIntensity, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.backgroundBlur)));
    drawSliderRow(SliderDragKind::BgBlur, Hit_SliderBgBlur, curY, isZh ? L"背景模糊" : L"BG Blur", m_config.backgroundBlur / 40.0f, vBuf);
    curY += 26.0f;

    setBrush(colBorder);
    m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
    curY += 8.0f;

    const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
    const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
    const std::string effectiveActivePresetKey(
        StudioBackdropCatalog::resolvePreset(m_config.backgroundPresetKey, m_config.backgroundTheme).key);

    // 悬停在任意背景缩略图时，在标题旁实时展示该背景雅名；否则显示「背景」
    std::wstring bgHeaderLabel = isZh ? L"背景" : L"Background";
    if (m_hoveredHitId >= Hit_ThemeSlotBase &&
        m_hoveredHitId < Hit_ThemeSlotBase + static_cast<int>(kWallpaperSlotsPerPage)) {
        const size_t hovSlot = static_cast<size_t>(m_hoveredHitId - Hit_ThemeSlotBase);
        const size_t hovFlt = static_cast<size_t>(m_wallpaperPage) * kWallpaperSlotsPerPage + hovSlot;
        if (hovFlt < m_filteredWallpaperIndices.size()) {
            const auto& hovEntry = m_allWallpapers[static_cast<size_t>(m_filteredWallpaperIndices[hovFlt])];
            if (!hovEntry.displayName.empty()) {
                bgHeaderLabel = (isZh ? L"背景 · " : L"Bg · ") + hovEntry.displayName;
            }
        }
    }
    const float bgHeaderRight = padR - (totalPages > 1 ? 136.0f : 60.0f);
    drawTxt(bgHeaderLabel, m_sectionFmt.Get(), D2D1::RectF(padL, curY, (std::max)(padL + 60.0f, bgHeaderRight), curY + 20.0f), colPrimary);

    const D2D1_RECT_F pickRc = D2D1::RectF(padR - (totalPages > 1 ? 132.0f : 56.0f), curY + 1.0f,
                                           padR - (totalPages > 1 ? 78.0f : 0.0f), curY + 20.0f);
    drawOptionChip(Hit_WallPickCustom, pickRc, isZh ? L"+选图" : L"+Img", false);
    if (totalPages > 1) {
        const D2D1_RECT_F prevRc = D2D1::RectF(padR - 72.0f, curY + 1.0f, padR - 53.0f, curY + 20.0f);
        const D2D1_RECT_F nextRc = D2D1::RectF(padR - 19.0f, curY + 1.0f, padR, curY + 20.0f);
        drawOptionChip(Hit_WallPrevPage, prevRc, L"<", false);
        drawOptionChip(Hit_WallNextPage, nextRc, L">", false);
        wchar_t pgTxt[24]{};
        std::swprintf(pgTxt, std::size(pgTxt), L"%d/%d", m_wallpaperPage + 1, totalPages);
        drawTxt(pgTxt, m_monoFmt.Get(), D2D1::RectF(prevRc.right, curY, nextRc.left, curY + 20.0f),
                colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    curY += 24.0f;

    {
        const int catIds[6] = {
            Hit_WallCatBase + 0, Hit_WallCatBase + 1, Hit_WallCatBase + 2,
            Hit_WallCatBase + 3, Hit_WallCatBase + 4, Hit_WallCatBase + 5
        };
        static const wchar_t* catZh[6] = { L"全部", L"沉稳", L"轻盈", L"风景", L"渐变", L"自定义" };
        static const wchar_t* catEn[6] = { L"All", L"Dark", L"Light", L"Scene", L"Mesh", L"Custom" };
        drawSegmentedGroup(curY, 24.0f, 6, catIds, isZh ? catZh : catEn, m_wallpaperCategory);
    }
    curY += 29.0f;

    {
        const float gap = 5.5f;
        const float boxSize = (fullW - gap * static_cast<float>(kWallpaperCols - 1)) / static_cast<float>(kWallpaperCols);
        m_wallpaperGridRect = D2D1::RectF(padL, curY, padR, curY + boxSize * kWallpaperRows + gap * (kWallpaperRows - 1));

        const size_t startIdx = static_cast<size_t>(m_wallpaperPage) * kWallpaperSlotsPerPage;
        for (size_t slot = 0; slot < kWallpaperSlotsPerPage; ++slot) {
            const size_t fltPos = startIdx + slot;
            const size_t col = slot % kWallpaperCols;
            const size_t row = slot / kWallpaperCols;
            const float tx = padL + static_cast<float>(col) * (boxSize + gap);
            const float ty = curY + static_cast<float>(row) * (boxSize + gap);
            const D2D1_RECT_F trc = D2D1::RectF(tx, ty, tx + boxSize, ty + boxSize);

            if (m_wallpaperCategory == 5 && slot == 0 && m_filteredWallpaperIndices.empty()) {
                m_hitZones.push_back({Hit_WallPickCustom, trc, -1});
                setBrush(colChipBg);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush);
                setBrush(colAccent);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, 1.2f);
                drawTxt(L"+", m_titleFmt.Get(), trc, colAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            }

            if (fltPos >= m_filteredWallpaperIndices.size()) break;
            const auto& entry = m_allWallpapers[static_cast<size_t>(m_filteredWallpaperIndices[fltPos])];
            const int hId = Hit_ThemeSlotBase + static_cast<int>(slot);
            m_hitZones.push_back({hId, trc, m_filteredWallpaperIndices[fltPos]});

            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(trc, 6.0f, 6.0f), clipGeo.GetAddressOf())) && clipGeo && m_themeThumbBitmaps[slot]) {
                ComPtr<ID2D1Layer> clipLayer;
                if (SUCCEEDED(m_renderTarget->CreateLayer(nullptr, clipLayer.GetAddressOf())) && clipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(trc, clipGeo.Get()), clipLayer.Get());
                    m_renderTarget->DrawBitmap(m_themeThumbBitmaps[slot].Get(), trc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_renderTarget->PopLayer();
                }
            }
            const bool selected = entry.isBuiltin
                ? (m_config.customBackgroundPath.empty() && effectiveActivePresetKey == entry.presetKey)
                : (!m_config.customBackgroundPath.empty() && m_config.customBackgroundPath == entry.filePathUtf8);
            if (selected) {
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, 2.5f);
                setBrush(colAccent);
                const D2D1_RECT_F outRc = D2D1::RectF(trc.left - 1.2f, trc.top - 1.2f, trc.right + 1.2f, trc.bottom + 1.2f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(outRc, 7.0f, 7.0f), brush, 1.6f);
            } else {
                setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);
            }
        }
    }
}

} // namespace tools3000::capture
