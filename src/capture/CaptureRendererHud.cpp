// ─────────────────────────────────────────────────────────────────────────────
// CaptureRendererHud.cpp — 放大镜/取色器/智能对齐/二维码胶囊/长图工作台与主渲染管线子模块
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

void CaptureRenderer::drawDynamicMagnifier(CaptureState& state) {
    if (!m_renderTarget || !hasScreenContent() || !m_d2dFactory) return;
    if (!state.isMarkupToolActive || state.currentTool != MarkupTool::Magnifier) return;
    if (state.isMarking || state.isManipulating) return;

    const float cx = static_cast<float>(state.currentCursor.x);
    const float cy = static_cast<float>(state.currentCursor.y);
    const D2D1_POINT_2F docPt = state.screenToDoc(cx, cy);

    // 仅当光标在选区内部（长图模式下还须在主视口 stageRect 内部）且未悬停在任何悬浮工具栏/菜单面板上时显示实时光学瞄准器
    if (state.isLongImageMode()) {
        const auto& st = state.longCanvas.stageRect;
        if (cx < st.left || cx > st.right || cy < st.top || cy > st.bottom) return;
    }
    const float sx1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
    const float sy1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
    const float sx2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
    const float sy2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));
    if (docPt.x < sx1 || docPt.x > sx2 || docPt.y < sy1 || docPt.y > sy2) return;

    auto inRect = [&](const D2D1_RECT_F& rc) {
        return rc.right > rc.left && cx >= rc.left && cx <= rc.right && cy >= rc.top && cy <= rc.bottom;
    };
    if (inRect(state.primaryToolbarRect) || inRect(state.secondaryToolbarRect) ||
        inRect(state.selectionSideRect) || hasAnyCapturePopupOpen(state)) {
        return;
    }

    const POINT cursorForPolicy = state.isLongImageMode() ? state.screenToDocPoint(state.currentCursor) : state.currentCursor;
    if (shouldSuppressDynamicMagnifierPreview(state, cursorForPolicy)) {
        return;
    }

    const float uiScale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    const float r = std::max(28.0f * uiScale, static_cast<float>(state.dynamicMagnifierRadius) * uiScale);
    const float magScale = std::clamp(state.dynamicMagnifierScale, 1.5f, 6.0f);
    const float srcR = r / magScale;
    const float docSrcR = state.isLongImageMode() ? (srcR / (std::max)(0.15f, state.longCanvas.zoom)) : srcR;
    const bool isSquare = (state.currentMagnifierShape == NumberBadgeShape::RoundedSquare);

    // 引出式详图模式下：光标悬停时仅绘制 1:1 真实采样范围的晶透三明治光学取景环/框，内部 100% 透明零遮挡
    if (state.magnifierCalloutMode) {
        const float sqrtScale = std::sqrt(std::max(1.0f, uiScale));
        const float ringR = std::clamp(srcR, 10.0f * uiScale, r * 0.67f);
        ComPtr<ID2D1SolidColorBrush> darkHaloBrush, rimBrush, innerRimBrush, tickBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.56f), darkHaloBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                         state.currentMagnifierColor.g / 255.0f,
                         state.currentMagnifierColor.b / 255.0f, 0.96f),
            rimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f), innerRimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f), tickBrush.GetAddressOf());

        const float inR = std::max(3.0f, ringR - 0.95f * sqrtScale);
        if (isSquare) {
            const float srCorner = std::max(4.0f * sqrtScale, ringR * 0.28f);
            auto srr = D2D1::RoundedRect(D2D1::RectF(cx - ringR, cy - ringR, cx + ringR, cy + ringR), srCorner, srCorner);
            auto srrIn = D2D1::RoundedRect(D2D1::RectF(cx - inR, cy - inR, cx + inR, cy + inR),
                                           std::max(2.0f, srCorner - 0.8f), std::max(2.0f, srCorner - 0.8f));
            if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(srr, darkHaloBrush.Get(), 2.8f * sqrtScale);
            if (rimBrush) m_renderTarget->DrawRoundedRectangle(srr, rimBrush.Get(), 1.35f * sqrtScale);
            if (innerRimBrush) m_renderTarget->DrawRoundedRectangle(srrIn, innerRimBrush.Get(), 0.7f * sqrtScale);
        } else {
            auto el = D2D1::Ellipse(D2D1::Point2F(cx, cy), ringR, ringR);
            auto elIn = D2D1::Ellipse(D2D1::Point2F(cx, cy), inR, inR);
            if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), 2.8f * sqrtScale);
            if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), 1.35f * sqrtScale);
            if (innerRimBrush) m_renderTarget->DrawEllipse(elIn, innerRimBrush.Get(), 0.7f * sqrtScale);
        }

        if (tickBrush) {
            const float arm = 4.5f * sqrtScale;
            const float gap = 1.6f * sqrtScale;
            if (darkHaloBrush) {
                m_renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx - gap, cy), darkHaloBrush.Get(), 2.2f * sqrtScale);
                m_renderTarget->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + arm, cy), darkHaloBrush.Get(), 2.2f * sqrtScale);
                m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy - gap), darkHaloBrush.Get(), 2.2f * sqrtScale);
                m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + arm), darkHaloBrush.Get(), 2.2f * sqrtScale);
            }
            m_renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx - gap, cy), tickBrush.Get(), 1.1f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + arm, cy), tickBrush.Get(), 1.1f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy - gap), tickBrush.Get(), 1.1f * sqrtScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + arm), tickBrush.Get(), 1.1f * sqrtScale);
        }
        return;
    }

    const D2D1_RECT_F srcRect = D2D1::RectF(docPt.x - docSrcR, docPt.y - docSrcR, docPt.x + docSrcR, docPt.y + docSrcR);
    const D2D1_RECT_F dstRect = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);
    const float cornerR = r * 0.28f;
    ComPtr<ID2D1Geometry> clipGeo;
    if (isSquare) {
        ComPtr<ID2D1RoundedRectangleGeometry> rrGeo;
        m_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dstRect, cornerR, cornerR), rrGeo.GetAddressOf());
        clipGeo = rrGeo;
    } else {
        ComPtr<ID2D1EllipseGeometry> elGeo;
        m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), elGeo.GetAddressOf());
        clipGeo = elGeo;
    }

    if (clipGeo) {
        ComPtr<ID2D1Layer> layer;
        if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
            m_renderTarget->PushLayer(D2D1::LayerParameters(
                D2D1::InfiniteRect(), clipGeo.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(),
                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE), layer.Get());

            if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                dstRect, srcRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, true);
            } else if (m_screenBitmap) {
                ComPtr<ID2D1DeviceContext> d2dDc;
                if (SUCCEEDED(m_renderTarget.As(&d2dDc)) && d2dDc) {
                    d2dDc->DrawBitmap(m_screenBitmap.Get(), &dstRect, 1.0f,
                                      D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &srcRect);
                } else {
                    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstRect, 1.0f,
                                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcRect);
                }
            }
            m_renderTarget->PopLayer();
        }

        ComPtr<ID2D1SolidColorBrush> darkHaloBrush, rimBrush, crossBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.38f), darkHaloBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(state.currentMagnifierColor.r / 255.0f,
                         state.currentMagnifierColor.g / 255.0f,
                         state.currentMagnifierColor.b / 255.0f, 0.94f),
            rimBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), crossBrush.GetAddressOf());

        const float rimW = std::max(2.2f * uiScale, static_cast<float>(state.currentStrokeWidth) * uiScale);
        if (isSquare) {
            auto rr = D2D1::RoundedRect(dstRect, cornerR, cornerR);
            if (darkHaloBrush) m_renderTarget->DrawRoundedRectangle(rr, darkHaloBrush.Get(), rimW + 2.2f * uiScale);
            if (rimBrush) m_renderTarget->DrawRoundedRectangle(rr, rimBrush.Get(), rimW);
        } else {
            auto el = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
            if (darkHaloBrush) m_renderTarget->DrawEllipse(el, darkHaloBrush.Get(), rimW + 2.2f * uiScale);
            if (rimBrush) m_renderTarget->DrawEllipse(el, rimBrush.Get(), rimW);
        }

        // 极细微光中心锚点准星，不遮挡被放大主体
        if (crossBrush) {
            const float arm = 4.5f * uiScale;
            const float gap = 1.8f * uiScale;
            m_renderTarget->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx - gap, cy), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + arm, cy), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy - gap), crossBrush.Get(), 1.0f * uiScale);
            m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + arm), crossBrush.Get(), 1.0f * uiScale);
        }
    }
}

D2D1_RECT_F CaptureRenderer::computeSelectionLoupeRect(
    float cx, float cy, D2D1_SIZE_F surfaceSize, float scale) {
    return HudAvoidanceEngine::computeSelectionLoupeRect(cx, cy, surfaceSize, scale);
}

void CaptureRenderer::drawSelectionLoupe(float cx, float cy, CaptureState& state) {
    if (!m_renderTarget || !hasScreenContent()) return;

    const bool zh = isCaptureUiChinese();
    int px = static_cast<int>(cx);
    int py = static_cast<int>(cy);
    int r = 0, g = 0, b = 0;
    bool hasColor = sampleScreenColor(px, py, r, g, b, state);

    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;
    constexpr float zoom = 8.0f;

    // 严格保持单一事实源外框尺寸 216x186 * scale，确保 HudAvoidanceEngine 与测试 100% 精确对齐
    int gridCountX = 27;
    int gridCountY = 13;
    float loupeBoxW = gridCountX * zoom * scale; // 216px * scale
    float loupeBoxH = gridCountY * zoom * scale; // 104px * scale
    float panelH = 82.0f * scale;                // 82px * scale
    const float totalH = loupeBoxH + panelH;
    const float outerRadius = 12.0f * scale;

    auto size = m_renderTarget->GetSize();
    D2D1_RECT_F monRect{}, workRect{};
    resolveMonitorBoundsInCanvas(cx, cy, size, monRect, workRect);
    const D2D1_RECT_F hostRect =
        (workRect.right - workRect.left >= loupeBoxW + 16.0f * scale &&
         workRect.bottom - workRect.top >= totalH + 16.0f * scale)
        ? workRect
        : monRect;
    const D2D1_SIZE_F localHostSize = D2D1::SizeF(
        (std::max)(loupeBoxW, hostRect.right - hostRect.left),
        (std::max)(totalH, hostRect.bottom - hostRect.top));
    D2D1_RECT_F localLoupeRect = computeSelectionLoupeRect(
        cx - hostRect.left, cy - hostRect.top, localHostSize, scale);
    D2D1_RECT_F fullLoupeRect = D2D1::RectF(
        localLoupeRect.left + hostRect.left,
        localLoupeRect.top + hostRect.top,
        localLoupeRect.right + hostRect.left,
        localLoupeRect.bottom + hostRect.top);
    const float clampMargin = 8.0f * scale;
    if (hostRect.right - hostRect.left > loupeBoxW + clampMargin * 2.0f) {
        const float clampedL = std::clamp(
            fullLoupeRect.left, hostRect.left + clampMargin, hostRect.right - loupeBoxW - clampMargin);
        fullLoupeRect.left = clampedL;
        fullLoupeRect.right = clampedL + loupeBoxW;
    }
    if (hostRect.bottom - hostRect.top > totalH + clampMargin * 2.0f) {
        const float clampedT = std::clamp(
            fullLoupeRect.top, hostRect.top + clampMargin, hostRect.bottom - totalH - clampMargin);
        fullLoupeRect.top = clampedT;
        fullLoupeRect.bottom = clampedT + totalH;
    }
    state.loupeHudRect = fullLoupeRect;
    float lx = fullLoupeRect.left;
    float ly = fullLoupeRect.top;

    // 0. 轻盈向下自然微投影（仅在底部提供柔和悬浮层次，杜绝四周浓黑阴影包裹）
    ComPtr<ID2D1SolidColorBrush> shadowOuter, shadowInner;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), shadowOuter.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f), shadowInner.GetAddressOf());
    if (shadowOuter) {
        D2D1_RECT_F s1 = D2D1::RectF(fullLoupeRect.left - 1.2f * scale, fullLoupeRect.top + 3.0f * scale,
                                     fullLoupeRect.right + 1.2f * scale, fullLoupeRect.bottom + 6.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(s1, outerRadius + 1.2f * scale, outerRadius + 1.2f * scale), shadowOuter.Get());
    }
    if (shadowInner) {
        D2D1_RECT_F s2 = D2D1::RectF(fullLoupeRect.left, fullLoupeRect.top + 1.5f * scale,
                                     fullLoupeRect.right, fullLoupeRect.bottom + 3.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(s2, outerRadius, outerRadius), shadowInner.Get());
    }

    // 启用圆角几何裁剪层，使上半区像素网格与下半区信息舱融合为一体式圆角微晶光学舱
    ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
    ComPtr<ID2D1Layer> clipLayer;
    bool pushedClip = false;
    if (m_d2dFactory &&
        SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(fullLoupeRect, outerRadius, outerRadius), clipGeo.GetAddressOf())) &&
        clipGeo &&
        SUCCEEDED(m_renderTarget->CreateLayer(clipLayer.GetAddressOf())) && clipLayer) {
        m_renderTarget->PushLayer(
            D2D1::LayerParameters(fullLoupeRect, clipGeo.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
            clipLayer.Get());
        pushedClip = true;
    }

    D2D1_RECT_F dst = D2D1::RectF(lx, ly, lx + loupeBoxW, ly + loupeBoxH);
    float srcHalfX = gridCountX / 2.0f;
    float srcHalfY = gridCountY / 2.0f;
    D2D1_RECT_F src = D2D1::RectF(cx - srcHalfX, cy - srcHalfY, cx + srcHalfX, cy + srcHalfY);

    if (m_screenBitmap) {
        m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dst, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, src);
    } else if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
        ComPtr<ID2D1SolidColorBrush> oobBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), oobBrush.GetAddressOf());
        if (oobBrush) {
            m_renderTarget->FillRectangle(dst, oobBrush.Get());
        }
        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                        dst, src, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, false);
    }

    // 1. 细腻半透明像素网格线
    ComPtr<ID2D1SolidColorBrush> gridBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.76f, 0.80f, 0.86f, 0.24f), gridBrush.GetAddressOf());
    if (gridBrush) {
        for (int i = 0; i <= gridCountY; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx, ly + lineOffset), D2D1::Point2F(lx + loupeBoxW, ly + lineOffset), gridBrush.Get(), 0.75f);
        }
        for (int i = 0; i <= gridCountX; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx + lineOffset, ly), D2D1::Point2F(lx + lineOffset, ly + loupeBoxH), gridBrush.Get(), 0.75f);
        }
    }

    // 2. 中心十字柔雾紫晶准星带
    ComPtr<ID2D1SolidColorBrush> loupeCrosshair;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeCrosshairBand, loupeCrosshair.GetAddressOf());

    float pixelSz = zoom * scale;
    float mcx = lx + (gridCountX / 2) * pixelSz;
    float mcy = ly + (gridCountY / 2) * pixelSz;

    if (loupeCrosshair) {
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, ly, mcx + pixelSz, mcy), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, mcy + pixelSz, mcx + pixelSz, ly + loupeBoxH), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(lx, mcy, mcx, mcy + pixelSz), loupeCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx + pixelSz, mcy, lx + loupeBoxW, mcy + pixelSz), loupeCrosshair.Get());
    }

    // 3. 基础画刷定义（高通透曜石纯黑信息舱 + 100% 纯白高对比度文字与实心键帽）
    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, whiteTextBrush, secondaryTextBrush, tagAccentBrush, loupePanelBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.94f), blackBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupePrimaryText, whiteTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeSecondaryText, secondaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupePrimaryText, tagAccentBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeInfoBg, loupePanelBrush.GetAddressOf());

    // 4. 中心单像素目标双层高反差锁定框
    if (whiteBrush && blackBrush) {
        D2D1_RECT_F centerCell = D2D1::RectF(mcx, mcy, mcx + pixelSz, mcy + pixelSz);
        m_renderTarget->DrawRectangle(centerCell, blackBrush.Get(), 2.2f);
        m_renderTarget->DrawRectangle(centerCell, whiteBrush.Get(), 1.25f);
    }

    // 5. 下半区高对比曜石纯黑信息舱（与左下角快捷键 HUD 家族统一，明亮清脆）
    D2D1_RECT_F panelRect = D2D1::RectF(lx, ly + loupeBoxH, lx + loupeBoxW, ly + totalH);
    if (loupePanelBrush) {
        m_renderTarget->FillRectangle(panelRect, loupePanelBrush.Get());
    }

    // 上下半区交界处：微晶亮白发丝分隔线
    ComPtr<ID2D1SolidColorBrush> seamHairline;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), seamHairline.GetAddressOf());
    if (seamHairline) {
        m_renderTarget->DrawLine(
            D2D1::Point2F(dst.left, dst.bottom),
            D2D1::Point2F(dst.right, dst.bottom),
            seamHairline.Get(), 1.0f * scale);
    }

    // 6. 三行大气仪表排版（色晶+色值+格式徽章 / 坐标 / 纯白实心键帽引导栏）
    IDWriteTextFormat* primaryFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
    if (hasColor && primaryFmt && badgeFmt && m_dwriteFactory) {
        auto formats = getAllColorFormats(r, g, b);
        size_t curIdx = static_cast<size_t>(state.colorFormat) % formats.size();
        const auto& curEntry = formats[curIdx];

        const float padX = 10.0f * scale;
        const float innerLeft = lx + padX;
        const float innerRight = lx + loupeBoxW - padX;
        const float innerW = innerRight - innerLeft;

        // 第 1 行：左侧圆角采样色晶 + 中间加粗纯白色值 + 右侧格式微晶徽章
        const float row1Top = panelRect.top + 7.0f * scale;
        const float row1H = 23.0f * scale;
        const float swatchSz = 16.0f * scale;
        const float swatchTop = row1Top + (row1H - swatchSz) * 0.5f;
        const D2D1_RECT_F swatchRect = D2D1::RectF(innerLeft, swatchTop, innerLeft + swatchSz, swatchTop + swatchSz);

        ComPtr<ID2D1SolidColorBrush> colorBox, swatchBorder;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorBox.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f), swatchBorder.GetAddressOf());
        if (colorBox) {
            auto swatchRounded = D2D1::RoundedRect(swatchRect, 4.5f * scale, 4.5f * scale);
            m_renderTarget->FillRoundedRectangle(swatchRounded, colorBox.Get());
            if (swatchBorder) m_renderTarget->DrawRoundedRectangle(swatchRounded, swatchBorder.Get(), 1.0f * scale);
        }

        // 右侧格式标签小胶囊 (HEX / RGB / HSL...)
        const std::wstring tagStr = curEntry.tag;
        const float tagW = (tagStr.size() >= 4 ? 42.0f : 34.0f) * scale;
        const float tagH = 18.0f * scale;
        const float tagTop = row1Top + (row1H - tagH) * 0.5f;
        const D2D1_RECT_F tagRect = D2D1::RectF(innerRight - tagW, tagTop, innerRight, tagTop + tagH);
        ComPtr<ID2D1SolidColorBrush> tagBg, tagBorder;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), tagBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), tagBorder.GetAddressOf());
        if (tagBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tagRect, 4.5f * scale, 4.5f * scale), tagBg.Get());
        if (tagBorder) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tagRect, 4.5f * scale, 4.5f * scale), tagBorder.Get(), 0.9f * scale);
        if (tagAccentBrush) {
            TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_renderTarget->DrawTextW(tagStr.c_str(), static_cast<UINT32>(tagStr.size()), badgeFmt, tagRect, tagAccentBrush.Get());
        }

        // 色值文本（长格式如 CMYK / RGBA 自动平滑使用紧凑字号，确保单行零折行零截断）
        const std::wstring& valStr = curEntry.value;
        IDWriteTextFormat* valFmt = (valStr.size() > 14) ? badgeFmt : primaryFmt;
        if (whiteTextBrush) {
            const float valLeft = swatchRect.right + 7.0f * scale;
            const D2D1_RECT_F valRect = D2D1::RectF(
                valLeft, row1Top, (std::max)(valLeft, tagRect.left - 4.0f * scale), row1Top + row1H);
            TextFormatScope scope(valFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                valStr.c_str(), static_cast<UINT32>(valStr.size()), valFmt,
                valRect, whiteTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 第 2 行：精准像素坐标
        const float row2Top = row1Top + row1H + 2.0f * scale;
        const float row2H = 19.0f * scale;
        const std::wstring coordText = std::format(L"({}, {})", px, py);
        if (secondaryTextBrush) {
            TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                coordText.c_str(), static_cast<UINT32>(coordText.size()), badgeFmt,
                D2D1::RectF(innerLeft, row2Top, innerLeft + innerW, row2Top + row2H),
                secondaryTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 第 3 行：PixPin 同款 100% 纯白实心圆角小键帽 + 纯白清晰说明 / 复制成功翡翠绿胶囊
        const bool toast = (state.loupeToastUntil != 0 && GetTickCount() < state.loupeToastUntil);
        const std::wstring fallbackOk = tools3000::core::trWCstrFor(zh, L"已复制到剪贴板", L"Copied to clipboard");
        const std::wstring cleanedToast = std::wstring(
            tools3000::ui::ToastStyle::stripMachineStatusPrefix(state.loupeToastMessage));

        const float row3Top = row2Top + row2H + 3.0f * scale;
        const float row3H = 22.0f * scale;
        const D2D1_RECT_F footerRect = D2D1::RectF(lx + 7.0f * scale, row3Top, lx + loupeBoxW - 7.0f * scale, row3Top + row3H);
        const auto footerRounded = D2D1::RoundedRect(footerRect, 5.0f * scale, 5.0f * scale);

        if (toast) {
            const std::wstring footerText = cleanedToast.empty() ? fallbackOk : cleanedToast;
            ComPtr<ID2D1SolidColorBrush> footerBg, footerBorder, footerTextBrush;
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastBg, footerBg.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastBorder, footerBorder.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kMintToastText, footerTextBrush.GetAddressOf());
            if (footerBg) m_renderTarget->FillRoundedRectangle(footerRounded, footerBg.Get());
            if (footerBorder) m_renderTarget->DrawRoundedRectangle(footerRounded, footerBorder.Get(), 0.9f * scale);
            if (footerTextBrush) {
                const D2D1_RECT_F footerTextRect = D2D1::RectF(
                    footerRect.left + 4.0f * scale, footerRect.top,
                    (std::max)(footerRect.left + 4.0f * scale, footerRect.right - 4.0f * scale), footerRect.bottom);
                TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(
                    footerText.c_str(), static_cast<UINT32>(footerText.size()), badgeFmt,
                    footerTextRect, footerTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            ComPtr<ID2D1SolidColorBrush> keycapBgBrush, keycapTextBrush;
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeKeycapBg, keycapBgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kLoupeKeycapText, keycapTextBrush.GetAddressOf());

            const std::wstring k1 = L"Shift";
            const std::wstring d1 = tools3000::core::trWCstrFor(zh, L"切换格式", L"Fmt");
            const std::wstring k2 = L"C";
            const std::wstring d2 = tools3000::core::trWCstrFor(zh, L"复制色值", L"Copy");

            const float k1W = 34.0f * scale;
            const float k2W = 16.0f * scale;
            const float capH = 16.5f * scale;
            const float capY = row3Top + (row3H - capH) * 0.5f;
            const float d1W = measureText(d1, badgeFmt, 200.0f * scale, 32.0f * scale).width;
            const float d2W = measureText(d2, badgeFmt, 200.0f * scale, 32.0f * scale).width;
            const float gapIn = 4.5f * scale;
            const float gapGroup = 10.0f * scale;
            const float totalRowW = k1W + gapIn + d1W + gapGroup + k2W + gapIn + d2W;
            float curX = lx + (loupeBoxW - totalRowW) * 0.5f;

            auto drawKeycapPair = [&](const std::wstring& kText, float kw, const std::wstring& desc, float dw) {
                const D2D1_RECT_F capRect = D2D1::RectF(curX, capY, curX + kw, capY + capH);
                if (keycapBgBrush) {
                    m_renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(capRect, 3.8f * scale, 3.8f * scale), keycapBgBrush.Get());
                }
                if (keycapTextBrush) {
                    TextFormatScope cScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(kText.c_str(), static_cast<UINT32>(kText.size()), badgeFmt, capRect, keycapTextBrush.Get());
                }
                curX += kw + gapIn;
                if (whiteTextBrush) {
                    const D2D1_RECT_F descRect = D2D1::RectF(curX, row3Top, curX + dw + 2.0f * scale, row3Top + row3H);
                    TextFormatScope dScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawTextW(desc.c_str(), static_cast<UINT32>(desc.size()), badgeFmt, descRect, whiteTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                curX += dw + gapGroup;
            };

            drawKeycapPair(k1, k1W, d1, d1W);
            drawKeycapPair(k2, k2W, d2, d2W);
        }
    }

    if (pushedClip) {
        m_renderTarget->PopLayer();
    }

    // 7. 全天候明暗背景自适应双层微晶纯白外框（亮色背景显银灰分离丝不融底，暗色背景呈现清脆透亮纯白边框）
    drawAllWeatherWhiteRim(
        m_renderTarget.Get(), fullLoupeRect, outerRadius, scale, CaptureHudRimTokens::forLoupe());
}

void CaptureRenderer::drawSmartAlignmentGuides(const D2D1_RECT_F& selRect, CaptureState& state) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;

    ComPtr<ID2D1SolidColorBrush> guideBrush;
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSmartAlignmentGuide, guideBrush.GetAddressOf());
    if (!guideBrush) return;

    float cx = (selRect.left + selRect.right) * 0.5f;
    float cy = (selRect.top + selRect.bottom) * 0.5f;
    D2D1_RECT_F monRect{}, workRect{};
    resolveMonitorBoundsInCanvas(cx, cy, size, monRect, workRect);
    const D2D1_RECT_F guideBounds =
        (workRect.right > workRect.left && workRect.bottom > workRect.top) ? workRect : monRect;
    float midScreenX = (guideBounds.left + guideBounds.right) * 0.5f;
    float midScreenY = (guideBounds.top + guideBounds.bottom) * 0.5f;

    // 1. 水平中线对齐 (当前物理显示器工作区 50% Y)
    if (std::abs(cy - midScreenY) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float x = guideBounds.left; x < guideBounds.right; x += seg + sp) {
            m_renderTarget->DrawLine(
                D2D1::Point2F(x, midScreenY),
                D2D1::Point2F((std::min)(guideBounds.right, x + seg), midScreenY),
                guideBrush.Get(), 1.0f * scale);
        }
    }

    // 2. 垂直中线对齐 (当前物理显示器工作区 50% X)
    if (std::abs(cx - midScreenX) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float y = guideBounds.top; y < guideBounds.bottom; y += seg + sp) {
            m_renderTarget->DrawLine(
                D2D1::Point2F(midScreenX, y),
                D2D1::Point2F(midScreenX, (std::min)(guideBounds.bottom, y + seg)),
                guideBrush.Get(), 1.0f * scale);
        }
    }
}

void CaptureRenderer::drawQrChip(const D2D1_RECT_F& selRect, CaptureState& state) {
    if (state.detectedQrText.empty() || !m_renderTarget || !m_dwriteFactory) {
        state.qrChipRect = D2D1::RectF(0, 0, 0, 0);
        return;
    }

    float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
    float chipH = 26.0f * scale;
    float pad = 8.0f * scale;
    const bool zh = isCaptureUiChinese();

    std::wstring displayMsg = (tools3000::core::trWCstrFor(zh, L"二维码: ", L"QR: ")) + tools3000::core::WinUtils::utf8ToWstring(state.detectedQrText);
    if (displayMsg.size() > 28) {
        displayMsg = displayMsg.substr(0, 26) + L"...";
    }

    // 测量文字宽度（maxWidth 随 DPI scale 放大并强制 NO_WRAP，防止高分屏测宽折行偏窄）
    float textW = 120.0f * scale;
    if (m_infoTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                m_infoTextFormat.Get(), 2000.0f * scale, chipH * 2.0f, layout.GetAddressOf())) && layout) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = metrics.widthIncludingTrailingWhitespace;
            }
        }
    }

    auto surfaceSize = m_renderTarget->GetSize();
    const float margin = 6.0f * scale;
    float chipW = (std::min)(24.0f * scale + textW + pad * 2.0f,
                             (std::max)(80.0f * scale, surfaceSize.width - 2.0f * margin));
    const float cxRight = std::clamp(selRect.right - chipW, margin, (std::max)(margin, surfaceSize.width - chipW - margin));
    const float cxLeft = std::clamp(selRect.left, margin, (std::max)(margin, surfaceSize.width - chipW - margin));

    std::vector<D2D1_RECT_F> candidates = {
        D2D1::RectF(cxRight, selRect.top - chipH - margin, cxRight + chipW, selRect.top - margin),
        D2D1::RectF(cxRight, selRect.top + margin, cxRight + chipW, selRect.top + margin + chipH),
        D2D1::RectF(cxLeft, selRect.bottom + margin, cxLeft + chipW, selRect.bottom + margin + chipH),
        D2D1::RectF(cxLeft, selRect.top - chipH - margin, cxLeft + chipW, selRect.top - margin),
        D2D1::RectF(cxRight, selRect.bottom - chipH - margin, cxRight + chipW, selRect.bottom - margin),
    };

    const bool isInteracting = state.dragging || state.isMarking || state.isManipulating ||
                               state.isAdjustingSelection || state.isAdjustingCornerRadius;
    const float curFx = static_cast<float>(state.currentCursor.x);
    const float curFy = static_cast<float>(state.currentCursor.y);
    const float curR = 28.0f * scale;
    const D2D1_RECT_F cursorBox = D2D1::RectF(curFx - curR, curFy - curR, curFx + curR, curFy + curR);

    auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
        if (b.right <= b.left || b.bottom <= b.top) return false;
        return (a.left < b.right + gap && a.right > b.left - gap &&
                a.top < b.bottom + gap && a.bottom > b.top - gap);
    };

    D2D1_RECT_F chosen = candidates.front();
    if (chosen.top < margin) {
        chosen = candidates[1];
    }
    for (const auto& cand : candidates) {
        if (cand.top < margin || cand.bottom > surfaceSize.height - margin) continue;
        bool collides = rectsOverlap(cand, state.sizeHudRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.primaryToolbarRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.secondaryToolbarRect, 4.0f * scale) ||
                        rectsOverlap(cand, state.selectionSideRect, 4.0f * scale) ||
                        (isInteracting && rectsOverlap(cand, cursorBox, 4.0f * scale));
        if (!collides) {
            chosen = cand;
            break;
        }
    }
    state.qrChipRect = chosen;

    // 绘制动态主题二维码胶囊底板与细腻描边
    const auto pal = FloatingGlassBar::resolveThemePalette();
    ComPtr<ID2D1SolidColorBrush> chipShadow, chipBg, chipBorder, iconBrush, textBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.06f, 0.09f, 0.16f, pal.isDark ? 0.24f : 0.09f), chipShadow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.panelBg, chipBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        state.isQrChipHovered ? pal.accentBorder : pal.panelBorder, chipBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentText, iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, textBrush.GetAddressOf());

    if (chipShadow) {
        auto shRect = D2D1::RectF(state.qrChipRect.left, state.qrChipRect.top + 1.5f * scale,
                                  state.qrChipRect.right, state.qrChipRect.bottom + 2.5f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRect, 6.5f * scale, 6.5f * scale), chipShadow.Get());
    }
    if (chipBg && chipBorder) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.5f * scale, 6.5f * scale), chipBg.Get());
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.5f * scale, 6.5f * scale), chipBorder.Get(), 1.0f * scale);
    }

    // 绘制二维码纯矢量图标
    auto iconRect = D2D1::RectF(state.qrChipRect.left + 6.0f * scale, state.qrChipRect.top + 3.0f * scale,
                                state.qrChipRect.left + 22.0f * scale, state.qrChipRect.bottom - 3.0f * scale);
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropQrCode, iconRect, iconBrush.Get(), scale);

    // 绘制文字
    if (m_infoTextFormat && textBrush) {
        TextFormatScope qrScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        const float qrTextLeft = state.qrChipRect.left + 25.0f * scale;
        auto textRect = D2D1::RectF(qrTextLeft, state.qrChipRect.top,
                                    (std::max)(qrTextLeft, state.qrChipRect.right - 6.0f * scale), state.qrChipRect.bottom);
        m_renderTarget->DrawTextW(displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                                  m_infoTextFormat.Get(), textRect, textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void CaptureRenderer::drawCrosshair(float x, float y, CaptureState* pState) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    ComPtr<ID2D1SolidColorBrush> crossShadow;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.22f), crossShadow.GetAddressOf());
    if (crossShadow) {
        m_renderTarget->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, size.height), crossShadow.Get(), 2.0f);
        m_renderTarget->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(size.width, y), crossShadow.Get(), 2.0f);
    }
    m_renderTarget->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, size.height), m_crosshairBrush.Get(), 1.0f);
    m_renderTarget->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(size.width, y), m_crosshairBrush.Get(), 1.0f);

    if (pState && m_infoTextFormat) {
        int px = static_cast<int>(x);
        int py = static_cast<int>(y);
        int r = 0, g = 0, b = 0;
        if (sampleScreenColor(px, py, r, g, b, *pState)) {
            const float scale = std::clamp(pState->dpiScale > 0.0f ? pState->dpiScale : 1.0f, 1.0f, 5.0f);
            std::wstring hudText = std::format(L"X: {}  Y: {}  #{:02X}{:02X}{:02X}", px, py, r, g, b);

            IDWriteTextFormat* crossFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
            const D2D1_SIZE_F crossTsz = measureText(hudText, crossFmt);
            float badgeW = (crossTsz.width > 0.0f ? crossTsz.width : static_cast<float>(hudText.size()) * 7.2f * scale) + 26.0f * scale;
            float badgeH = 22.0f * scale;
            const float margin = 8.0f * scale;

            auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
                if (b.right <= b.left || b.bottom <= b.top) return false;
                return (a.left < b.right + gap && a.right > b.left - gap &&
                        a.top < b.bottom + gap && a.bottom > b.top - gap);
            };

            const D2D1_POINT_2F quadOrigins[] = {
                {x + 14.0f * scale, y - badgeH - 8.0f * scale},          // 1. 右上象限 (默认)
                {x - badgeW - 14.0f * scale, y - badgeH - 8.0f * scale}, // 2. 左上象限
                {x - badgeW - 14.0f * scale, y + 14.0f * scale},         // 3. 左下象限
                {x + 14.0f * scale, y + 14.0f * scale},                  // 4. 右下象限
            };

            float badgeX = std::clamp(quadOrigins[0].x, margin, (std::max)(margin, size.width - badgeW - margin));
            float badgeY = std::clamp(quadOrigins[0].y, margin, (std::max)(margin, size.height - badgeH - margin));
            for (const auto& orig : quadOrigins) {
                if (orig.x < margin || orig.x + badgeW > size.width - margin ||
                    orig.y < margin || orig.y + badgeH > size.height - margin) {
                    continue;
                }
                D2D1_RECT_F cand = D2D1::RectF(orig.x, orig.y, orig.x + badgeW, orig.y + badgeH);
                if (!rectsOverlap(cand, pState->loupeHudRect, 6.0f * scale) &&
                    !rectsOverlap(cand, pState->sizeHudRect, 6.0f * scale)) {
                    badgeX = orig.x;
                    badgeY = orig.y;
                    break;
                }
            }

            auto badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
            auto badgeRound = D2D1::RoundedRect(badgeRect, 6.0f * scale, 6.0f * scale);

            const auto pal = FloatingGlassBar::resolveHudPalette();
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush, bgBrush, borderBrush, textBrush, colorChipBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.06f, 0.09f, 0.16f, pal.isDark ? 0.24f : 0.09f), shadowBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(pal.panelBg, bgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(pal.panelBorder, borderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(pal.primaryText, textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorChipBrush.GetAddressOf());

            if (shadowBrush) {
                auto shRect = D2D1::RectF(badgeRect.left, badgeRect.top + 1.5f * scale, badgeRect.right, badgeRect.bottom + 2.5f * scale);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRect, 6.0f * scale, 6.0f * scale), shadowBrush.Get());
            }
            if (bgBrush) m_renderTarget->FillRoundedRectangle(badgeRound, bgBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(badgeRound, borderBrush.Get(), 1.0f * scale);

            float chipSz = 9.0f * scale;
            float chipX = badgeX + 6.0f * scale;
            float chipY = badgeY + (badgeH - chipSz) * 0.5f;
            auto chipRect = D2D1::RectF(chipX, chipY, chipX + chipSz, chipY + chipSz);
            if (colorChipBrush) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), colorChipBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), borderBrush.Get(), 0.8f * scale);

            TextFormatScope crossScope(crossFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(hudText.c_str(), static_cast<UINT32>(hudText.size()), crossFmt,
                D2D1::RectF(chipX + chipSz + 5.0f * scale, badgeY, badgeX + badgeW - 4.0f * scale, badgeY + badgeH),
                textBrush.Get());
        }
    }
}

void CaptureRenderer::updateShortcutHintAvoidance(const CaptureState& state) {
    if (!ShortcutHintOverlay::instance().isVisible() || !m_renderTarget) return;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    RECT winRc{};
    if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
        vx = winRc.left;
        vy = winRc.top;
    }
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto surfaceSize = m_renderTarget->GetSize();

    auto toScreenRect = [&](const D2D1_RECT_F& r) -> RECT {
        return RECT{
            static_cast<LONG>(std::lround(r.left)) + vx,
            static_cast<LONG>(std::lround(r.top)) + vy,
            static_cast<LONG>(std::lround(r.right)) + vx,
            static_cast<LONG>(std::lround(r.bottom)) + vy
        };
    };
    auto pushIfValid = [&](std::vector<RECT>& vec, const D2D1_RECT_F& r) {
        if (r.right > r.left && r.bottom > r.top) {
            vec.push_back(toScreenRect(r));
        }
    };

    std::vector<RECT> avoidRects;
    avoidRects.reserve(12);

    // 1. 鼠标光标高优先级安全避让区（任何阶段只要鼠标靠近左下角快捷键卡片，提前 72px*scale 主动预判避让！）
    if (state.currentCursor.x >= 0 && state.currentCursor.y >= 0) {
        const LONG curScreenX = state.currentCursor.x + vx;
        const LONG curScreenY = state.currentCursor.y + vy;
        const LONG cursorPad = static_cast<LONG>(std::lround(72.0f * scale));
        avoidRects.push_back(RECT{
            curScreenX - cursorPad,
            curScreenY - cursorPad,
            curScreenX + cursorPad,
            curScreenY + cursorPad
        });
    }

    // 2. 像素取色放大镜（Selection Loupe）与动态放大镜（Dynamic Magnifier）
    pushIfValid(avoidRects, state.loupeHudRect);
    const auto overlayState = state.state.load();
    const bool hasLockedSelection =
        ((int)overlayState == (int)OverlayState::Selected ||
         (int)overlayState == (int)OverlayState::Marking);
    if (hasLockedSelection && state.currentTool == MarkupTool::Magnifier &&
        !state.isMarking && !state.isManipulating) {
        const float magR = (static_cast<float>(state.dynamicMagnifierRadius) + 14.0f) * scale;
        const float curFx = static_cast<float>(state.currentCursor.x);
        const float curFy = static_cast<float>(state.currentCursor.y);
        pushIfValid(avoidRects, D2D1::RectF(curFx - magR, curFy - magR, curFx + magR, curFy + magR));
    }

    // 3. 尺寸胶囊、尺寸菜单、层级提示胶囊与二维码胶囊
    pushIfValid(avoidRects, state.sizeHudRect);
    if (state.isSizeMenuOpen) {
        pushIfValid(avoidRects, state.sizeMenuRect);
    }
    pushIfValid(avoidRects, state.hierarchyBadgeRect);
    if (!state.detectedQrText.empty()) {
        pushIfValid(avoidRects, state.qrChipRect);
    }

    // 4. 主工具栏、二级属性栏、侧边悬浮栏及展开的二级弹窗/滑块/下拉菜单
    if (hasLockedSelection) {
        pushIfValid(avoidRects, state.primaryToolbarRect);
        if (!state.secondaryToolbarButtons.empty()) {
            pushIfValid(avoidRects, state.secondaryToolbarRect);
        }
        if (!state.selectionSideButtons.empty()) {
            pushIfValid(avoidRects, state.selectionSideRect);
        }
        if (state.openSubmenu != SubmenuType::None) {
            pushIfValid(avoidRects, state.openSubmenuRect);
        }
        if (state.sliderPopup.type != SliderPopupType::None) {
            pushIfValid(avoidRects, state.sliderPopup.popupRect);
        }
        if (state.dropdownMenu.type != DropdownType::None && !state.dropdownMenu.items.empty()) {
            D2D1_RECT_F dmRect = state.dropdownMenu.items.front().rect;
            for (const auto& item : state.dropdownMenu.items) {
                dmRect.left = (std::min)(dmRect.left, item.rect.left);
                dmRect.top = (std::min)(dmRect.top, item.rect.top);
                dmRect.right = (std::max)(dmRect.right, item.rect.right);
                dmRect.bottom = (std::max)(dmRect.bottom, item.rect.bottom);
            }
            pushIfValid(avoidRects, dmRect);
            pushIfValid(avoidRects, state.dropdownMenu.childMenuRect);
        }
    }

    // 5. 当前正在拖拽或已锁定的选区矩形（长图模式下仅避让顶栏与右侧 Minimap，使左下角快捷键提示安稳驻留）
    if (state.isLongImageMode()) {
        pushIfValid(avoidRects, state.longCanvas.topHeaderRect);
        pushIfValid(avoidRects, state.longCanvas.minimapPanelRect);
    } else if (hasLockedSelection || state.dragging) {
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));
        pushIfValid(avoidRects, D2D1::RectF(x1, y1, x2, y2));
    }

    ShortcutHintOverlay::instance().updateAvoidance(avoidRects);
}

bool CaptureRenderer::sampleScreenColor(int x, int y, int& r, int& g, int& b, CaptureState& state) const {
    if (state.frozenScreen.empty()) return false;
    if (x < 0 || y < 0 || x >= state.frozenScreen.cols || y >= state.frozenScreen.rows) return false;
    if (state.frozenScreen.channels() == 4) {
        const cv::Vec4b& px = state.frozenScreen.at<cv::Vec4b>(y, x);
        b = px[0]; g = px[1]; r = px[2];
    } else if (state.frozenScreen.channels() == 3) {
        const cv::Vec3b& px = state.frozenScreen.at<cv::Vec3b>(y, x);
        b = px[0]; g = px[1]; r = px[2];
    } else {
        return false;
    }
    return true;
}

void CaptureRenderer::updateHistoryBitmap(CaptureState& state) {
    m_historyBitmap.Reset();
    m_historyTiles.clear();
    m_historyTilesW = 0;
    m_historyTilesH = 0;
    auto entry = CaptureHistory::instance().get(state.historyIndex);
    if (entry && !entry->image.empty() && m_renderTarget) {
        cv::Mat bgra;
        if (entry->image.channels() == 3) {
            cv::cvtColor(entry->image, bgra, cv::COLOR_BGR2BGRA);
        } else if (entry->image.channels() == 4) {
            bgra = entry->image;
        } else if (entry->image.channels() == 1) {
            cv::cvtColor(entry->image, bgra, cv::COLOR_GRAY2BGRA);
        }
        if (!bgra.empty() && bgra.cols > 0 && bgra.rows > 0) {
            const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            const UINT32 hwMaxSize = m_renderTarget->GetMaximumBitmapSize();
            const UINT32 safeMaxBmp = (hwMaxSize > 0) ? hwMaxSize : 4096u;
            if (static_cast<UINT32>(bgra.cols) <= safeMaxBmp &&
                static_cast<UINT32>(bgra.rows) <= safeMaxBmp) {
                m_renderTarget->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                    bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_historyBitmap.GetAddressOf()
                );
            }
            if (!m_historyBitmap) {
                if (!rebuildBitmapTiles(bgra, D2D1_ALPHA_MODE_PREMULTIPLIED,
                                        m_historyTiles, m_historyTilesW, m_historyTilesH)) {
                    const double scaleRatio = (std::min)(
                        static_cast<double>(safeMaxBmp) / static_cast<double>(bgra.cols),
                        static_cast<double>(safeMaxBmp) / static_cast<double>(bgra.rows));
                    const int scaledW = (std::max)(1, static_cast<int>(std::floor(bgra.cols * scaleRatio)));
                    const int scaledH = (std::max)(1, static_cast<int>(std::floor(bgra.rows * scaleRatio)));
                    cv::Mat preview;
                    try {
                        cv::resize(bgra, preview, cv::Size(scaledW, scaledH), 0.0, 0.0, cv::INTER_AREA);
                        if (!preview.empty()) {
                            m_renderTarget->CreateBitmap(
                                D2D1::SizeU(static_cast<UINT32>(preview.cols), static_cast<UINT32>(preview.rows)),
                                preview.data, static_cast<UINT32>(preview.step[0]), props, m_historyBitmap.GetAddressOf()
                            );
                        }
                    } catch (const cv::Exception&) {
                    }
                }
            }
        }
    }
    m_needsRender = true;
}

void CaptureRenderer::updateLongCanvasLayout(CaptureState& state) {
    if (!m_renderTarget || !state.isLongImageMode()) return;
    const auto sz = m_renderTarget->GetSize();
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    auto lay = ScrollCaptureEditorOverlay::computeEditorLayout(
        sz.width, sz.height, state.longCanvas.docWidth, state.longCanvas.docHeight, scale);

    // 底部为全功能标准主工具栏 + 二级属性栏预留充足工作台底座高度 (106px * scale)
    const float bottomDockReserve = 108.0f * scale;
    const float topReserve = 58.0f * scale;
    lay.topHeaderRect.top = 10.0f * scale;
    lay.topHeaderRect.bottom = 50.0f * scale;
    lay.stageRect.top = topReserve;
    lay.stageRect.bottom = (std::max)(lay.stageRect.top + 120.0f * scale, sz.height - bottomDockReserve);
    if (lay.minimapPanelRect.right > lay.minimapPanelRect.left) {
        lay.minimapPanelRect.top = lay.stageRect.top;
        lay.minimapPanelRect.bottom = lay.stageRect.bottom;
        const float mmPad = 8.0f * scale;
        const float availW = (std::max)(16.0f, (lay.minimapPanelRect.right - lay.minimapPanelRect.left) - mmPad * 2.0f);
        const float availH = (std::max)(48.0f, (lay.minimapPanelRect.bottom - lay.minimapPanelRect.top) - mmPad * 2.0f);
        const float aspect = static_cast<float>(state.longCanvas.docWidth) /
                             static_cast<float>((std::max)(1, state.longCanvas.docHeight));
        float imgH = availH;
        float imgW = imgH * aspect;
        if (imgW > availW) {
            imgW = availW;
            imgH = (std::max)(36.0f, imgW / (std::max)(0.01f, aspect));
        }
        imgW = std::clamp(imgW, 18.0f * scale, availW);
        const float cx = (lay.minimapPanelRect.left + lay.minimapPanelRect.right) * 0.5f;
        const float cy = (lay.minimapPanelRect.top + lay.minimapPanelRect.bottom) * 0.5f;
        lay.minimapImageRect = D2D1::RectF(cx - imgW * 0.5f, cy - imgH * 0.5f, cx + imgW * 0.5f, cy + imgH * 0.5f);
    }

    state.longCanvas.stageRect = lay.stageRect;
    state.longCanvas.topHeaderRect = lay.topHeaderRect;
    state.longCanvas.minimapPanelRect = lay.minimapPanelRect;
    state.longCanvas.minimapImageRect = lay.minimapImageRect;

    const float stageW = (std::max)(80.0f, lay.stageRect.right - lay.stageRect.left - 48.0f * scale);
    state.longCanvas.fitZoom = std::clamp(
        stageW / static_cast<float>((std::max)(1, state.longCanvas.docWidth)), 0.15f, 1.0f);
    if (state.longCanvas.zoom <= 0.01f) {
        state.longCanvas.zoom = state.longCanvas.fitZoom;
    }
    state.clampLongCanvasScroll();
}

void CaptureRenderer::drawLongCanvasTopHeader(const D2D1_RECT_F& docSelRect, CaptureState& state) {
    if (!m_renderTarget || !m_infoTextFormat) return;
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool zh = isCaptureUiChinese();
    const auto sz = m_renderTarget->GetSize();

    const int netW = (std::max)(1, static_cast<int>(std::lround(docSelRect.right - docSelRect.left)));
    const int netH = (std::max)(1, static_cast<int>(std::lround(docSelRect.bottom - docSelRect.top)));
    const int trimmedPx = (std::max)(0, state.longCanvas.docHeight - netH) +
                          (std::max)(0, state.longCanvas.docWidth - netW);
    const bool isCropped =
        (std::lround(docSelRect.top) > 0 ||
         std::lround(docSelRect.left) > 0 ||
         std::lround(docSelRect.bottom) < state.longCanvas.docHeight ||
         std::lround(docSelRect.right) < state.longCanvas.docWidth);
    const int zoomPct = static_cast<int>(std::lround(state.longCanvas.zoom * 100.0f));
    const bool near100 = std::abs(state.longCanvas.zoom - 1.0f) < 0.03f;

    const std::wstring frameCntStr = std::to_wstring(state.longCanvas.frameCount);
    const std::wstring titleText = state.longCanvas.frameCount > 1
        ? tools3000::core::trFmtW(L"长截图精修 ({0} 帧)", L"Long Capture ({0} frames)", {frameCntStr})
        : std::wstring(tools3000::core::trWCstrFor(zh, L"长截图精修", L"Long Capture"));
    const std::wstring sizeBadge = isCropped
        ? std::format(L"{} × {} px (-{}px)", netW, netH, trimmedPx)
        : std::format(L"{} × {} px", netW, netH);

    const std::wstring lblTop = tools3000::core::trWCstrFor(zh, L"头部 Home", L"Top");
    const std::wstring lblBot = tools3000::core::trWCstrFor(zh, L"尾部 End", L"Bottom");
    const std::wstring lblReset = tools3000::core::trWCstrFor(zh, L"重置裁切", L"Reset Crop");
    const std::wstring lblZoom = near100
        ? std::format(L"{}% · {}", zoomPct, tools3000::core::trWCstrFor(zh, L"适宽", L"Fit"))
        : std::format(L"{}% · 1:1", zoomPct);

    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();
    const float padX = 12.0f * scale;
    const float btnH = 26.0f * scale;
    const float btnGap = 6.0f * scale;
    const float barH = 38.0f * scale;

    const float titleW = measureText(titleText, m_infoTextFormat.Get(), 400.0f * scale, barH).width + 6.0f * scale;
    const float sizeW = measureText(sizeBadge, badgeFmt, 420.0f * scale, barH).width + 16.0f * scale;
    const float wTop = measureText(lblTop, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wBot = measureText(lblBot, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wReset = measureText(lblReset, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;
    const float wZoom = measureText(lblZoom, badgeFmt, 200.0f * scale, btnH).width + 20.0f * scale;

    const float totalW = padX + titleW + 8.0f * scale + sizeW + 14.0f * scale +
                         wTop + btnGap + wBot + btnGap + wReset + btnGap + wZoom + padX;
    const float stageCx = (state.longCanvas.stageRect.left + state.longCanvas.stageRect.right) * 0.5f;
    const float barLeft = std::clamp(stageCx - totalW * 0.5f, 8.0f * scale,
                                     (std::max)(8.0f * scale, sz.width - totalW - 8.0f * scale));
    const float barTop = 10.0f * scale;
    const D2D1_RECT_F headerRect = D2D1::RectF(barLeft, barTop, barLeft + totalW, barTop + barH);
    state.longCanvas.topHeaderRect = headerRect;

    const auto pal = FloatingGlassBar::resolveThemePalette();
    drawGlassPanel(headerRect, 11.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> priText, secText, chipBg, chipHoverBg, chipBorder, accentText, disabledText, dividerBr;
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, priText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.secondaryText, secText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, chipBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.hoverBg, chipHoverBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentBorder, chipBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentText, accentText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.mutedText, disabledText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.divider, dividerBr.GetAddressOf());

    float curX = barLeft + padX;
    const float midY = barTop + barH * 0.5f;

    if (priText) {
        TextFormatScope s(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                                  m_infoTextFormat.Get(),
                                  D2D1::RectF(curX, barTop, curX + titleW, barTop + barH),
                                  priText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    curX += titleW + 8.0f * scale;

    // 净尺寸胶囊（点击同样可展开 px/dp 尺寸菜单）
    const D2D1_RECT_F sizeChipRect = D2D1::RectF(curX, midY - btnH * 0.5f, curX + sizeW, midY + btnH * 0.5f);
    state.sizeHudRect = sizeChipRect;
    const bool sizeHov = (state.currentCursor.x >= sizeChipRect.left && state.currentCursor.x <= sizeChipRect.right &&
                          state.currentCursor.y >= sizeChipRect.top && state.currentCursor.y <= sizeChipRect.bottom);
    state.isSizeHudHovered = sizeHov;
    if (sizeHov ? chipHoverBg : chipBg) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sizeChipRect, 6.0f * scale, 6.0f * scale),
                                             (sizeHov ? chipHoverBg : chipBg).Get());
    }
    if (chipBorder) {
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sizeChipRect, 6.0f * scale, 6.0f * scale),
                                             chipBorder.Get(), 0.9f * scale);
    }
    if (badgeFmt && accentText) {
        TextFormatScope s(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(sizeBadge.c_str(), static_cast<UINT32>(sizeBadge.size()),
                                  badgeFmt, sizeChipRect, accentText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    curX += sizeW + 7.0f * scale;

    if (dividerBr) {
        m_renderTarget->DrawLine(D2D1::Point2F(curX, barTop + 9.0f * scale),
                                 D2D1::Point2F(curX, barTop + barH - 9.0f * scale),
                                 dividerBr.Get(), 1.0f * scale);
    }
    curX += 7.0f * scale;

    state.longCanvas.hoveredHeaderBtn = -1;
    auto drawActionChip = [&](int idx, float w, const std::wstring& label, bool enabled, D2D1_RECT_F& outRect) {
        outRect = D2D1::RectF(curX, midY - btnH * 0.5f, curX + w, midY + btnH * 0.5f);
        const bool hov = enabled &&
                         state.currentCursor.x >= outRect.left && state.currentCursor.x <= outRect.right &&
                         state.currentCursor.y >= outRect.top && state.currentCursor.y <= outRect.bottom;
        if (hov) {
            state.longCanvas.hoveredHeaderBtn = idx;
            if (chipHoverBg) {
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(outRect, 6.5f * scale, 6.5f * scale), chipHoverBg.Get());
            }
            if (chipBorder) {
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(outRect, 6.5f * scale, 6.5f * scale), chipBorder.Get(), 1.0f * scale);
            }
        }
        ID2D1Brush* tBr = !enabled ? disabledText.Get() : (hov ? priText.Get() : secText.Get());
        if (badgeFmt && tBr) {
            TextFormatScope s(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                                      badgeFmt, outRect, tBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        curX += w + btnGap;
    };

    drawActionChip(0, wTop, lblTop, true, state.longCanvas.btnJumpTopRect);
    drawActionChip(1, wBot, lblBot, true, state.longCanvas.btnJumpBottomRect);
    drawActionChip(2, wReset, lblReset, isCropped, state.longCanvas.btnResetCropRect);
    drawActionChip(3, wZoom, lblZoom, true, state.longCanvas.btnZoomToggleRect);
}

void CaptureRenderer::drawLongCanvasMinimap(const D2D1_RECT_F& docSelRect, CaptureState& state) {
    if (!m_renderTarget) return;
    const auto& panel = state.longCanvas.minimapPanelRect;
    const auto& imgRect = state.longCanvas.minimapImageRect;
    if (panel.right <= panel.left || imgRect.bottom <= imgRect.top) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    // 惰性构建高品质全景缩略位图 (最大高度 1024px)
    if ((!m_minimapBitmap || m_minimapSrcW != state.longCanvas.docWidth || m_minimapSrcH != state.longCanvas.docHeight) &&
        !state.frozenScreen.empty()) {
        m_minimapBitmap.Reset();
        const int maxThumbH = 1024;
        const double ratio = (state.frozenScreen.rows > maxThumbH)
            ? (static_cast<double>(maxThumbH) / static_cast<double>(state.frozenScreen.rows))
            : 1.0;
        const int tw = (std::max)(1, static_cast<int>(std::lround(state.frozenScreen.cols * ratio)));
        const int th = (std::max)(1, static_cast<int>(std::lround(state.frozenScreen.rows * ratio)));
        cv::Mat thumb;
        if (ratio < 0.999) {
            cv::resize(state.frozenScreen, thumb, cv::Size(tw, th), 0.0, 0.0, cv::INTER_AREA);
        } else {
            thumb = state.frozenScreen;
        }
        cv::Mat bgra;
        D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        if (thumb.channels() == 4) {
            bgra = thumb;
            alphaMode = D2D1_ALPHA_MODE_IGNORE;
        } else if (thumb.channels() == 3) {
            cv::cvtColor(thumb, bgra, cv::COLOR_BGR2BGRA);
        }
        if (!bgra.empty()) {
            if (!bgra.isContinuous()) bgra = bgra.clone();
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));
            if (SUCCEEDED(m_renderTarget->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                    bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_minimapBitmap.GetAddressOf()))) {
                m_minimapSrcW = state.longCanvas.docWidth;
                m_minimapSrcH = state.longCanvas.docHeight;
            }
        }
    }

    const auto pal = FloatingGlassBar::resolveThemePalette();
    ComPtr<ID2D1SolidColorBrush> panelBg, panelBorder, trimMaskBr, cropBarBr, viewFillBr, viewBorderBr;
    m_renderTarget->CreateSolidColorBrush(pal.panelBg, panelBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.panelBorder, panelBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.72f), trimMaskBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accent, cropBarBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentSoftBg, viewFillBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f) : pal.accentText, viewBorderBr.GetAddressOf());

    const auto panelRound = D2D1::RoundedRect(panel, 11.0f * scale, 11.0f * scale);
    if (panelBg) m_renderTarget->FillRoundedRectangle(panelRound, panelBg.Get());
    if (panelBorder) m_renderTarget->DrawRoundedRectangle(panelRound, panelBorder.Get(), 1.0f * scale);

    const int topRow = std::clamp(static_cast<int>(std::lround(docSelRect.top)), 0, state.longCanvas.docHeight);
    const int botRow = std::clamp(static_cast<int>(std::lround(docSelRect.bottom)), topRow, state.longCanvas.docHeight);
    const float mmCropTopY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(topRow, imgRect, state.longCanvas.docHeight);
    const float mmCropBotY = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(botRow, imgRect, state.longCanvas.docHeight);

    const float previewScale =
        (imgRect.right - imgRect.left) / (std::max)(1.0f, static_cast<float>(state.longCanvas.docWidth));
    const float maxMinimapCornerR =
        0.5f * (std::min)(imgRect.right - imgRect.left, (std::max)(1.0f, mmCropBotY - mmCropTopY));
    const float minimapCornerR =
        std::clamp(state.effectiveCornerRadius() * previewScale, 0.0f, maxMinimapCornerR);

    if (m_minimapBitmap) {
        if (minimapCornerR > 0.5f && m_d2dFactory && mmCropBotY > mmCropTopY + 1.0f) {
            // 先绘制全图压暗底衬，再以 previewScale 圆角裁剪高亮保留区，实现缩略图圆角所见即所得
            m_renderTarget->DrawBitmap(m_minimapBitmap.Get(), imgRect, 0.28f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            const D2D1_RECT_F keptMinimapRect = D2D1::RectF(imgRect.left, mmCropTopY, imgRect.right, mmCropBotY);
            ComPtr<ID2D1RoundedRectangleGeometry> mmClipGeo;
            ComPtr<ID2D1Layer> mmLayer;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(keptMinimapRect, minimapCornerR, minimapCornerR), mmClipGeo.GetAddressOf())) &&
                mmClipGeo &&
                SUCCEEDED(m_renderTarget->CreateLayer(mmLayer.GetAddressOf())) && mmLayer) {
                m_renderTarget->PushLayer(
                    D2D1::LayerParameters(keptMinimapRect, mmClipGeo.Get(),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                        1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                    mmLayer.Get());
                m_renderTarget->DrawBitmap(m_minimapBitmap.Get(), imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                m_renderTarget->PopLayer();
            } else {
                m_renderTarget->DrawBitmap(m_minimapBitmap.Get(), imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            m_renderTarget->DrawBitmap(m_minimapBitmap.Get(), imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // 缩略图上下被裁除区间压暗
    if (trimMaskBr) {
        if (mmCropTopY > imgRect.top + 0.5f) {
            m_renderTarget->FillRectangle(D2D1::RectF(imgRect.left, imgRect.top, imgRect.right, mmCropTopY), trimMaskBr.Get());
        }
        if (mmCropBotY < imgRect.bottom - 0.5f) {
            m_renderTarget->FillRectangle(D2D1::RectF(imgRect.left, mmCropBotY, imgRect.right, imgRect.bottom), trimMaskBr.Get());
        }
    }

    // 当前主画布可见视口高亮框
    const auto visTopDoc = state.screenToDoc(state.longCanvas.stageRect.left, state.longCanvas.stageRect.top);
    const auto visBotDoc = state.screenToDoc(state.longCanvas.stageRect.left, state.longCanvas.stageRect.bottom);
    const int visRow0 = std::clamp(static_cast<int>(std::lround(visTopDoc.y)), 0, state.longCanvas.docHeight);
    const int visRow1 = std::clamp(static_cast<int>(std::lround(visBotDoc.y)), visRow0, state.longCanvas.docHeight);
    const float mmVisTop = ScrollCaptureEditorOverlay::mapImageRowToMinimapY(visRow0, imgRect, state.longCanvas.docHeight);
    const float mmVisBot = (std::max)(mmVisTop + 8.0f * scale,
        ScrollCaptureEditorOverlay::mapImageRowToMinimapY(visRow1, imgRect, state.longCanvas.docHeight));
    const D2D1_RECT_F vpBox = D2D1::RectF(
        imgRect.left - 2.0f * scale,
        std::clamp(mmVisTop, imgRect.top, imgRect.bottom - 6.0f * scale),
        imgRect.right + 2.0f * scale,
        std::clamp(mmVisBot, imgRect.top + 6.0f * scale, imgRect.bottom));
    if (viewFillBr) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(vpBox, 3.0f * scale, 3.0f * scale), viewFillBr.Get());
    if (viewBorderBr) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(vpBox, 3.0f * scale, 3.0f * scale), viewBorderBr.Get(), 1.4f * scale);

    // 缩略图顶/底 Smartisan 裁切滑块横梁
    if (cropBarBr) {
        const float barOverhang = 4.0f * scale;
        const float pillH = 4.0f * scale;
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(imgRect.left - barOverhang, mmCropTopY - pillH * 0.5f,
                                          imgRect.right + barOverhang, mmCropTopY + pillH * 0.5f),
                              2.0f * scale, 2.0f * scale),
            cropBarBr.Get());
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(imgRect.left - barOverhang, mmCropBotY - pillH * 0.5f,
                                          imgRect.right + barOverhang, mmCropBotY + pillH * 0.5f),
                              2.0f * scale, 2.0f * scale),
            cropBarBr.Get());
    }
}

void CaptureRenderer::renderLongImageStudio(CaptureState& state) {
    updateLongCanvasLayout(state);

    const auto surfaceSize = m_renderTarget->GetSize();
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);

    // 1. 专业暗房曜石灰底衬
    ComPtr<ID2D1SolidColorBrush> studioBg, stageShadowBr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.055f, 0.072f, 0.110f, 0.97f), studioBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.01f, 0.02f, 0.05f, 0.42f), stageShadowBr.GetAddressOf());
    if (studioBg) {
        m_renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, surfaceSize.width, surfaceSize.height), studioBg.Get());
    }

    state.loupeHudRect = D2D1::RectF(0, 0, 0, 0);
    state.hierarchyBadgeRect = D2D1::RectF(0, 0, 0, 0);

    const float x1 = static_cast<float>((std::min)(state.dragStart.x, state.dragEnd.x));
    const float y1 = static_cast<float>((std::min)(state.dragStart.y, state.dragEnd.y));
    const float x2 = static_cast<float>((std::max)(state.dragStart.x, state.dragEnd.x));
    const float y2 = static_cast<float>((std::max)(state.dragStart.y, state.dragEnd.y));
    const D2D1_RECT_F docSelRect = D2D1::RectF(x1, y1, x2, y2);
    const D2D1_RECT_F fullDocRect = D2D1::RectF(
        0.0f, 0.0f, static_cast<float>(state.longCanvas.docWidth), static_cast<float>(state.longCanvas.docHeight));
    const D2D1_RECT_F docScreenRect = state.docRectToScreen(fullDocRect);
    const D2D1_RECT_F selScreenRect = state.docRectToScreen(docSelRect);

    // 2. 主画布裁剪视口渲染（支持缩放与平移矩阵，使全部标注工具在文档空间 1:1 精确工作）
    m_renderTarget->PushAxisAlignedClip(state.longCanvas.stageRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (stageShadowBr) {
        const D2D1_RECT_F sh = D2D1::RectF(
            docScreenRect.left - 3.0f * scale, docScreenRect.top + 2.0f * scale,
            docScreenRect.right + 3.0f * scale, docScreenRect.bottom + 8.0f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sh, 6.0f * scale, 6.0f * scale), stageShadowBr.Get());
    }

    const float z = (std::max)(0.05f, state.longCanvas.zoom);
    const auto orig = state.longCanvasOrigin();
    const D2D1_MATRIX_3X2_F canvasXform =
        D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(orig.x, orig.y);
    m_renderTarget->SetTransform(canvasXform);

    // 2.1 绘制完整长图底图（单纹理快路径直绘可见视口，或超巨幕二维瓦片流裁剪）
    if (m_screenBitmap) {
        const auto bmpSz = m_screenBitmap->GetSize();
        const auto visLt = state.screenToDoc(state.longCanvas.stageRect.left - 8.0f, state.longCanvas.stageRect.top - 8.0f);
        const auto visRb = state.screenToDoc(state.longCanvas.stageRect.right + 8.0f, state.longCanvas.stageRect.bottom + 8.0f);
        const D2D1_RECT_F visDoc = D2D1::RectF(
            std::clamp(visLt.x, 0.0f, bmpSz.width),
            std::clamp(visLt.y, 0.0f, bmpSz.height),
            std::clamp(visRb.x, 0.0f, bmpSz.width),
            std::clamp(visRb.y, 0.0f, bmpSz.height));
        const auto interp = (std::abs(z - 1.0f) < 0.01f)
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        if (visDoc.right > visDoc.left && visDoc.bottom > visDoc.top) {
            m_renderTarget->DrawBitmap(m_screenBitmap.Get(), visDoc, 1.0f, interp, visDoc);
        }
    } else if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
        const auto visLt = state.screenToDoc(state.longCanvas.stageRect.left - 8.0f, state.longCanvas.stageRect.top - 8.0f);
        const auto visRb = state.screenToDoc(state.longCanvas.stageRect.right + 8.0f, state.longCanvas.stageRect.bottom + 8.0f);
        const D2D1_RECT_F visDoc = D2D1::RectF(
            std::clamp(visLt.x, 0.0f, static_cast<float>(m_screenTilesW)),
            std::clamp(visLt.y, 0.0f, static_cast<float>(m_screenTilesH)),
            std::clamp(visRb.x, 0.0f, static_cast<float>(m_screenTilesW)),
            std::clamp(visRb.y, 0.0f, static_cast<float>(m_screenTilesH)));
        const auto interp = (std::abs(z - 1.0f) < 0.01f)
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH, visDoc, visDoc, interp);
    }

    // 2.2 美化外壳底板 + 已合成标注缓存瓦片 + 正在拖拽的实时标注预览（100% 复用标准截图引擎！）
    if (state.beautyShell.enabled) {
        drawBeautyShellPreview(docSelRect, state);
    }
    drawMarkupPreview(docSelRect, state);
    drawActiveMarkupPreview(docSelRect, state);

    // 恢复 1:1 屏幕像素空间绘制非破坏性裁切遮罩与 Smartisan 推拉握把，确保任意缩放下握把清晰恒定
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());

    // 2.3 Smartisan 非破坏性已裁除区域半透暗罩 + 45 度导引斜线（可随时往回拉无损恢复像素）
    ComPtr<ID2D1SolidColorBrush> cropMaskBr, hatchBr, borderShadowBr, handleFillBr, handleRingBr, handleAccentBr, gripLineBr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.03f, 0.05f, 0.09f, 0.66f), cropMaskBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f), hatchBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.42f), borderShadowBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), handleFillBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.16f, 0.23f, 0.85f), handleRingBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.56f, 0.98f, 0.98f), handleAccentBr.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.34f, 0.43f, 0.85f), gripLineBr.GetAddressOf());

    auto drawTrimmedRegion = [&](const D2D1_RECT_F& r) {
        const D2D1_RECT_F clipped = D2D1::RectF(
            (std::max)(r.left, state.longCanvas.stageRect.left),
            (std::max)(r.top, state.longCanvas.stageRect.top),
            (std::min)(r.right, state.longCanvas.stageRect.right),
            (std::min)(r.bottom, state.longCanvas.stageRect.bottom));
        if (clipped.right <= clipped.left + 0.5f || clipped.bottom <= clipped.top + 0.5f) return;
        if (cropMaskBr) m_renderTarget->FillRectangle(clipped, cropMaskBr.Get());
        if (hatchBr) {
            m_renderTarget->PushAxisAlignedClip(clipped, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float step = 16.0f * scale;
            const float h = clipped.bottom - clipped.top;
            for (float x = clipped.left - h; x < clipped.right + step; x += step) {
                m_renderTarget->DrawLine(
                    D2D1::Point2F(x, clipped.bottom),
                    D2D1::Point2F(x + h, clipped.top),
                    hatchBr.Get(), 1.0f * scale);
            }
            m_renderTarget->PopAxisAlignedClip();
        }
    };

    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, docScreenRect.top, docScreenRect.right, selScreenRect.top));
    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, selScreenRect.bottom, docScreenRect.right, docScreenRect.bottom));
    drawTrimmedRegion(D2D1::RectF(docScreenRect.left, selScreenRect.top, selScreenRect.left, selScreenRect.bottom));
    drawTrimmedRegion(D2D1::RectF(selScreenRect.right, selScreenRect.top, docScreenRect.right, selScreenRect.bottom));

    // 2.4 保留区高对比白晶边框与 Smartisan 顶/底推拉裁切胶囊标尺
    const float effRadiusScreen = state.effectiveCornerRadius() * z;
    if (!(state.beautyShell.enabled && state.beautyShell.padding > 0)) {
        if (effRadiusScreen > 0.5f) {
            const auto rr = D2D1::RoundedRect(selScreenRect, effRadiusScreen, effRadiusScreen);
            if (borderShadowBr) m_renderTarget->DrawRoundedRectangle(rr, borderShadowBr.Get(), 2.8f * scale);
            if (m_borderBrush) m_renderTarget->DrawRoundedRectangle(rr, m_borderBrush.Get(), 1.7f * scale);
        } else {
            if (borderShadowBr) m_renderTarget->DrawRectangle(selScreenRect, borderShadowBr.Get(), 2.8f * scale);
            if (m_borderBrush) m_renderTarget->DrawRectangle(selScreenRect, m_borderBrush.Get(), 1.7f * scale);
        }
    }

    const float pillW = (std::min)(92.0f * scale, (std::max)(44.0f * scale, (selScreenRect.right - selScreenRect.left) * 0.35f));
    const float pillH = 14.0f * scale;
    const float selMidX = (selScreenRect.left + selScreenRect.right) * 0.5f;
    const float curX = static_cast<float>(state.currentCursor.x);
    const float curY = static_cast<float>(state.currentCursor.y);

    auto drawSmartisanCropBar = [&](float yEdge, bool isTop) {
        const bool draggingThis =
            (state.isAdjustingSelection && state.selAdjustHandle == (isTop ? HitArea::T : HitArea::B)) ||
            (state.longCanvas.dragMode == (isTop ? CaptureState::LongCanvasDragMode::MinimapCropTop
                                                 : CaptureState::LongCanvasDragMode::MinimapCropBottom));
        const D2D1_RECT_F pill = D2D1::RectF(selMidX - pillW * 0.5f, yEdge - pillH * 0.5f,
                                             selMidX + pillW * 0.5f, yEdge + pillH * 0.5f);
        const bool hovered = draggingThis ||
            (curX >= selScreenRect.left && curX <= selScreenRect.right &&
             std::abs(curY - yEdge) <= 10.0f * scale);

        if (hovered && handleAccentBr) {
            m_renderTarget->DrawLine(D2D1::Point2F(selScreenRect.left, yEdge),
                                     D2D1::Point2F(selScreenRect.right, yEdge),
                                     handleAccentBr.Get(), 2.2f * scale);
        }
        const auto pillRound = D2D1::RoundedRect(pill, pillH * 0.5f, pillH * 0.5f);
        if (borderShadowBr) {
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pill.left, pill.top + 1.2f * scale, pill.right, pill.bottom + 1.8f * scale),
                                  pillH * 0.5f, pillH * 0.5f),
                borderShadowBr.Get());
        }
        if (handleFillBr) m_renderTarget->FillRoundedRectangle(pillRound, handleFillBr.Get());
        ID2D1Brush* ring = (hovered && handleAccentBr) ? handleAccentBr.Get() : handleRingBr.Get();
        if (ring) m_renderTarget->DrawRoundedRectangle(pillRound, ring, (hovered ? 1.6f : 1.1f) * scale);

        if (gripLineBr) {
            const float gw = 18.0f * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(selMidX - gw * 0.5f, yEdge - 2.0f * scale),
                                     D2D1::Point2F(selMidX + gw * 0.5f, yEdge - 2.0f * scale),
                                     (hovered && handleAccentBr) ? handleAccentBr.Get() : gripLineBr.Get(), 1.2f * scale);
            m_renderTarget->DrawLine(D2D1::Point2F(selMidX - gw * 0.5f, yEdge + 2.0f * scale),
                                     D2D1::Point2F(selMidX + gw * 0.5f, yEdge + 2.0f * scale),
                                     (hovered && handleAccentBr) ? handleAccentBr.Get() : gripLineBr.Get(), 1.2f * scale);
        }

        if (draggingThis && m_hudBadgeFormat) {
            const int cutPx = isTop
                ? (std::max)(0, static_cast<int>(std::lround(docSelRect.top)))
                : (std::max)(0, state.longCanvas.docHeight - static_cast<int>(std::lround(docSelRect.bottom)));
            const std::wstring cutStr = std::to_wstring(cutPx);
            const std::wstring tip = isTop
                ? tools3000::core::trFmtW(L"顶部裁去 {0} px", L"Trim Top {0} px", {cutStr})
                : tools3000::core::trFmtW(L"底部裁去 {0} px", L"Trim Bottom {0} px", {cutStr});
            const float tw = measureText(tip, m_hudBadgeFormat.Get(), 260.0f * scale, 28.0f * scale).width + 18.0f * scale;
            const float th = 24.0f * scale;
            const float ty = isTop ? (pill.bottom + 6.0f * scale) : (pill.top - th - 6.0f * scale);
            const D2D1_RECT_F tipRect = D2D1::RectF(selMidX - tw * 0.5f, ty, selMidX + tw * 0.5f, ty + th);
            drawGlassPanel(tipRect, 6.0f * scale, false);
            if (m_infoTextBrush) {
                TextFormatScope s(m_hudBadgeFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(tip.c_str(), static_cast<UINT32>(tip.size()),
                                          m_hudBadgeFormat.Get(), tipRect, m_infoTextBrush.Get());
            }
        }
    };

    drawSmartisanCropBar(selScreenRect.top, true);
    drawSmartisanCropBar(selScreenRect.bottom, false);

    // 左右中点微调锚点与四角内侧圆角手柄
    const float selMidY = (selScreenRect.top + selScreenRect.bottom) * 0.5f;
    const float sideDotR = 4.2f * scale;
    for (float sxEdge : {selScreenRect.left, selScreenRect.right}) {
        auto el = D2D1::Ellipse(D2D1::Point2F(sxEdge, selMidY), sideDotR, sideDotR);
        if (handleFillBr) m_renderTarget->FillEllipse(el, handleFillBr.Get());
        if (handleRingBr) m_renderTarget->DrawEllipse(el, handleRingBr.Get(), 1.25f * scale);
    }
    drawCornerRadiusHandleVisual(
        selScreenRect, effRadiusScreen / scale, scale, state.isAdjustingCornerRadius,
        state.cornerDragStartPos, state.currentCursor, 64.0f, state.cornerDragIndex);

    m_renderTarget->PopAxisAlignedClip();

    // 3. 实时光学放大镜瞄准器 + 顶部快捷导航舱 + 右侧全景缩略导航条 + 全功能标准主/副/侧边工具栏
    if (state.isMarkupToolActive && state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
        drawDynamicMagnifier(state);
    }

    drawLongCanvasTopHeader(docSelRect, state);
    drawLongCanvasMinimap(docSelRect, state);

    const D2D1_RECT_F anchorRect = state.toolbarAnchorRect(docSelRect);
    drawQrChip(anchorRect, state);
    drawToolbar(anchorRect, state);
    drawSizeMenu(state.sizeHudRect, state);
    updateShortcutHintAvoidance(state);
    drawFloatingToast(state);
}

void CaptureRenderer::render(CaptureState& state) {
    if (!m_renderTarget && m_hwnd && IsWindow(m_hwnd)) {
        if (!createRenderResources(state)) return;
        if (!state.frozenScreen.empty()) {
            updateScreenBitmap(state.frozenScreen);
        }
        if (state.historyMode) {
            updateHistoryBitmap(state);
        }
        m_markupCacheDirty = true;
    }
    if (!m_renderTarget) return;

    const auto recoverFromDeviceLoss = [this](HRESULT hr) {
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LOG_WARN("CaptureRenderer: EndDraw device lost (hr=0x{:08X}), scheduling target recovery", static_cast<uint32_t>(hr));
            const HWND savedHwnd = m_hwnd;
            releaseWindowResources();
            m_hwnd = savedHwnd;
            m_markupCacheDirty = true;
            m_needsRender = true;
            if (m_hwnd && IsWindow(m_hwnd)) {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }
    };

    applyThemeColors();
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (state.isLongImageMode()) {
        renderLongImageStudio(state);
        recoverFromDeviceLoss(m_renderTarget->EndDraw());
        return;
    }

    if (state.historyMode) {
        const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
        const bool zh = isCaptureUiChinese();
        auto size = m_renderTarget->GetSize();
        m_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), m_dimBrush.Get());

        D2D1_RECT_F monRect{}, workRect{};
        resolveMonitorBoundsInCanvas(
            static_cast<float>(state.currentCursor.x),
            static_cast<float>(state.currentCursor.y),
            size, monRect, workRect);
        const D2D1_RECT_F hostRect =
            (workRect.right - workRect.left > 160.0f && workRect.bottom - workRect.top > 160.0f)
            ? workRect
            : monRect;
        const float hostW = (std::max)(1.0f, hostRect.right - hostRect.left);
        const float hostH = (std::max)(1.0f, hostRect.bottom - hostRect.top);
        
        if (hasHistoryContent()) {
            float srcW = 0.0f;
            float srcH = 0.0f;
            if (m_historyBitmap) {
                auto bmpSize = m_historyBitmap->GetSize();
                srcW = bmpSize.width;
                srcH = bmpSize.height;
            } else {
                srcW = static_cast<float>(m_historyTilesW);
                srcH = static_cast<float>(m_historyTilesH);
            }

            const float maxDispW = (std::max)(64.0f, hostW * 0.88f);
            const float maxDispH = (std::max)(64.0f, hostH * 0.88f);
            float fitScale = 1.0f;
            if (srcW > maxDispW || srcH > maxDispH) {
                fitScale = (std::min)(maxDispW / (std::max)(1.0f, srcW),
                                      maxDispH / (std::max)(1.0f, srcH));
            }
            const float dispW = (std::max)(1.0f, srcW * fitScale);
            const float dispH = (std::max)(1.0f, srcH * fitScale);
            float x = hostRect.left + (hostW - dispW) * 0.5f;
            float y = hostRect.top + (hostH - dispH) * 0.5f;
            D2D1_RECT_F dstRect = D2D1::RectF(x, y, x + dispW, y + dispH);
            if (m_historyBitmap) {
                m_renderTarget->DrawBitmap(
                    m_historyBitmap.Get(), dstRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                const D2D1_RECT_F fullSrc = D2D1::RectF(0.0f, 0.0f, srcW, srcH);
                drawTiledRegion(
                    m_historyTiles, m_historyTilesW, m_historyTilesH,
                    dstRect, fullSrc, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, true);
            }
            if (m_borderBrush) {
                m_renderTarget->DrawRectangle(dstRect, m_borderBrush.Get(), 2.0f * scale);
            }
            
            const std::wstring info = zh
                ? std::format(L"历史回放: {} / {}", state.historyIndex + 1, CaptureHistory::instance().count())
                : std::format(L"History: {} / {}", state.historyIndex + 1, CaptureHistory::instance().count());
            const float textW = measureText(info, m_infoTextFormat.Get(), 1000.0f * scale, 64.0f * scale).width;
            const float pillW = (std::max)(160.0f * scale, textW + 28.0f * scale);
            const float pillH = 28.0f * scale;
            const float margin = 8.0f * scale;
            const float pillX = std::clamp(
                x, hostRect.left + margin, (std::max)(hostRect.left + margin, hostRect.right - pillW - margin));
            float pillY = y - pillH - 8.0f * scale;
            if (pillY < hostRect.top + margin) {
                pillY = (std::min)(y + 8.0f * scale, (std::max)(hostRect.top + margin, hostRect.bottom - pillH - margin));
            }
            const D2D1_RECT_F pillRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);
            drawGlassPanel(pillRect, 7.0f * scale, false);
            if (m_infoTextFormat && m_infoTextBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()),
                                         m_infoTextFormat.Get(), pillRect,
                                         m_infoTextBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else {
            const std::wstring info = tools3000::core::trWCstrFor(zh, L"暂无历史截图", L"No capture history");
            if (m_infoTextFormat && m_infoTextBrush) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()), m_infoTextFormat.Get(),
                                         hostRect, m_infoTextBrush.Get());
            }
        }
        
        recoverFromDeviceLoss(m_renderTarget->EndDraw());
        return;
    }

    // 绘制冻结的屏幕（单纹理快路径或超宽多 4K / 8192 上限显卡二维瓦片回退）
    if (m_screenBitmap) {
        auto size = m_renderTarget->GetSize();
        m_renderTarget->DrawBitmap(m_screenBitmap.Get(),
                                   D2D1::RectF(0, 0, size.width, size.height));
    } else if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
        auto size = m_renderTarget->GetSize();
        const D2D1_RECT_F fullDst = D2D1::RectF(0.0f, 0.0f, size.width, size.height);
        const D2D1_RECT_F fullSrc = D2D1::RectF(
            0.0f, 0.0f, static_cast<float>(m_screenTilesW), static_cast<float>(m_screenTilesH));
        drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                        fullDst, fullSrc, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    state.loupeHudRect = D2D1::RectF(0, 0, 0, 0);
    state.hierarchyBadgeRect = D2D1::RectF(0, 0, 0, 0);

    if ((int)state.state.load() == (int)OverlayState::Selecting && state.dragging) {
        // 正在拖拽选区
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));

        D2D1_RECT_F selRect = D2D1::RectF(x1, y1, x2, y2);
        drawDimOverlay(selRect, state);
        if (state.beautyShell.enabled && state.mode != OverlayMode::RecordRegion) {
            drawBeautyShellPreview(selRect, state);
        }
        drawSmartAlignmentGuides(selRect, state);
        drawSelection(selRect, state);
        drawSizeInfo(selRect, state);
        // 拖拽中也显示取色放大镜，便于像素级对齐选区边缘
        drawSelectionLoupe(static_cast<float>(state.currentCursor.x), static_cast<float>(state.currentCursor.y), state);
    } else if ((int)state.state.load() == (int)OverlayState::Selected || (int)state.state.load() == (int)OverlayState::Marking) {
        // 选区已确认
        float x1 = static_cast<float>(std::min(state.dragStart.x, state.dragEnd.x));
        float y1 = static_cast<float>(std::min(state.dragStart.y, state.dragEnd.y));
        float x2 = static_cast<float>(std::max(state.dragStart.x, state.dragEnd.x));
        float y2 = static_cast<float>(std::max(state.dragStart.y, state.dragEnd.y));

        D2D1_RECT_F selRect = D2D1::RectF(x1, y1, x2, y2);
        drawDimOverlay(selRect, state);

        // 如果启用了美化外壳模式，必须在标注之前绘制底板与圆角原图，作为底层画布，绝不可覆盖标注！
        if (state.beautyShell.enabled && state.mode != OverlayMode::RecordRegion) {
            drawBeautyShellPreview(selRect, state);
        }

        drawMarkupPreview(selRect, state);
        drawActiveMarkupPreview(selRect, state);

        if (state.isMarkupToolActive && state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
            drawDynamicMagnifier(state);
        }

        drawSmartAlignmentGuides(selRect, state);
        drawSelection(selRect, state);
        drawSizeInfo(selRect, state);
        drawQrChip(selRect, state);
        drawToolbar(selRect, state);
        drawSizeMenu(state.sizeHudRect, state);
    } else {
        // 空闲/选区前 — 全屏变暗 + 十字准星 + 窗口高亮
        auto size = m_renderTarget->GetSize();
        float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
        {
            const float curFx = static_cast<float>(state.currentCursor.x);
            const float curFy = static_cast<float>(state.currentCursor.y);
            D2D1_RECT_F monRect{}, workRect{};
            resolveMonitorBoundsInCanvas(curFx, curFy, size, monRect, workRect);
            const float loupeW = 216.0f * scale;
            const float loupeH = 186.0f * scale;
            const D2D1_RECT_F hostRect =
                (workRect.right - workRect.left >= loupeW + 16.0f * scale &&
                 workRect.bottom - workRect.top >= loupeH + 16.0f * scale)
                ? workRect
                : monRect;
            const D2D1_SIZE_F localHostSize = D2D1::SizeF(
                (std::max)(loupeW, hostRect.right - hostRect.left),
                (std::max)(loupeH, hostRect.bottom - hostRect.top));
            const D2D1_RECT_F localRect = computeSelectionLoupeRect(
                curFx - hostRect.left, curFy - hostRect.top, localHostSize, scale);
            state.loupeHudRect = D2D1::RectF(
                localRect.left + hostRect.left,
                localRect.top + hostRect.top,
                localRect.right + hostRect.left,
                localRect.bottom + hostRect.top);
        }
        state.sizeHudRect = D2D1::RectF(0, 0, 0, 0);

        m_renderTarget->FillRectangle(
            D2D1::RectF(0, 0, size.width, size.height), m_dimBrush.Get()
        );

        // 检测光标下的窗口并高亮其边界（采用 Windows 11 现代圆角贴合与 Spring 阻尼平滑过渡）
        bool hasDetectedWindow = (state.animDetectedWindowRect.right > state.animDetectedWindowRect.left &&
                                  state.animDetectedWindowRect.bottom > state.animDetectedWindowRect.top) ||
                                 (state.detectedWindow.right > state.detectedWindow.left &&
                                  state.detectedWindow.bottom > state.detectedWindow.top);
        if (hasDetectedWindow) {
            D2D1_RECT_F winRect;
            if (state.animDetectedWindowRect.right > state.animDetectedWindowRect.left) {
                winRect = state.animDetectedWindowRect;
            } else {
                winRect = D2D1::RectF(
                    static_cast<float>(state.detectedWindow.left),
                    static_cast<float>(state.detectedWindow.top),
                    static_cast<float>(state.detectedWindow.right),
                    static_cast<float>(state.detectedWindow.bottom)
                );
            }
            
            float r = state.detectedWindowCornerRadius * scale;
            auto roundedWin = D2D1::RoundedRect(winRect, r, r);

            // 窗口区域采用圆角几何裁剪透出原始画面（去掉变暗效果）
            if (hasScreenContent() && m_d2dFactory) {
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedWin, geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1Layer> layer;
                    if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                        m_renderTarget->PushLayer(
                            D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                            layer.Get());
                        if (m_screenBitmap) {
                            m_renderTarget->DrawBitmap(m_screenBitmap.Get(), winRect, 1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &winRect);
                        } else if (!m_screenTiles.empty() && m_screenTilesW > 0 && m_screenTilesH > 0) {
                            drawTiledRegion(m_screenTiles, m_screenTilesW, m_screenTilesH,
                                            winRect, winRect, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        }
                        m_renderTarget->PopLayer();
                    }
                }
            }
            // 全天候无色偏双层白晶圆角高亮外框（细腻石板外衬 + 纯白晶主线，白底/暗底皆清晰锐利）
            {
                ComPtr<ID2D1SolidColorBrush> winOutlineShadow;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.28f), winOutlineShadow.GetAddressOf());
                if (winOutlineShadow) {
                    m_renderTarget->DrawRoundedRectangle(roundedWin, winOutlineShadow.Get(), 3.0f * scale);
                }
                m_renderTarget->DrawRoundedRectangle(roundedWin, m_borderBrush.Get(), 1.8f * scale);
            }
            // 显示窗口尺寸与圆角提示
            drawSizeInfo(winRect, state);

            // 动态渲染温润雪瓷白层级提示徽章 (如 "[2/4] 控件: 搜索框 (680x42)")，自动躲避尺寸胶囊、取色放大镜与鼠标光标
            if (GetTickCount() < state.hierarchyBadgeFadeOutUntil && !state.hierarchyBadgeText.empty() && m_dwriteFactory && m_infoTextFormat) {
                float badgeH = 34.0f * scale;
                float padH = 16.0f * scale;
                float dotZone = 16.0f * scale;
                ComPtr<IDWriteTextLayout> badgeLayout;
                m_dwriteFactory->CreateTextLayout(
                    state.hierarchyBadgeText.c_str(),
                    static_cast<UINT32>(state.hierarchyBadgeText.size()),
                    m_infoTextFormat.Get(), 1000.0f * scale, badgeH, badgeLayout.GetAddressOf());
                float textW = 140.0f * scale;
                if (badgeLayout) {
                    DWRITE_TEXT_METRICS m{};
                    badgeLayout->GetMetrics(&m);
                    textW = m.widthIncludingTrailingWhitespace > m.width ? m.widthIncludingTrailingWhitespace : m.width;
                }
                float badgeW = textW + dotZone + padH * 2.0f;
                const float margin = 8.0f * scale;
                const float centerBx = std::clamp(
                    winRect.left + (winRect.right - winRect.left - badgeW) * 0.5f,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float rightBx = std::clamp(
                    winRect.right - badgeW,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float afterHudBx = std::clamp(
                    state.sizeHudRect.right + 12.0f * scale,
                    margin, (std::max)(margin, size.width - badgeW - margin));
                const float topOutY = winRect.top - badgeH - 10.0f * scale;
                const float topInY = winRect.top + 10.0f * scale;
                const float botOutY = winRect.bottom + 10.0f * scale;
                const float botInY = winRect.bottom - badgeH - 10.0f * scale;

                std::vector<D2D1_RECT_F> badgeCandidates = {
                    D2D1::RectF(centerBx, topOutY >= margin ? topOutY : topInY, centerBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(afterHudBx, topOutY >= margin ? topOutY : topInY, afterHudBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(rightBx, topOutY >= margin ? topOutY : topInY, rightBx + badgeW, (topOutY >= margin ? topOutY : topInY) + badgeH),
                    D2D1::RectF(centerBx, botOutY + badgeH <= size.height - margin ? botOutY : botInY, centerBx + badgeW, (botOutY + badgeH <= size.height - margin ? botOutY : botInY) + badgeH),
                    D2D1::RectF(centerBx, topInY, centerBx + badgeW, topInY + badgeH),
                };

                const float curFx = static_cast<float>(state.currentCursor.x);
                const float curFy = static_cast<float>(state.currentCursor.y);
                const D2D1_RECT_F cursorBox = D2D1::RectF(curFx - 32.0f * scale, curFy - 32.0f * scale, curFx + 32.0f * scale, curFy + 32.0f * scale);
                auto rectsOverlap = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b, float gap) -> bool {
                    if (b.right <= b.left || b.bottom <= b.top) return false;
                    return (a.left < b.right + gap && a.right > b.left - gap &&
                            a.top < b.bottom + gap && a.bottom > b.top - gap);
                };

                D2D1_RECT_F badgeRect = badgeCandidates.front();
                for (const auto& cand : badgeCandidates) {
                    if (cand.top < margin || cand.bottom > size.height - margin) continue;
                    if (!rectsOverlap(cand, state.sizeHudRect, 8.0f * scale) &&
                        !rectsOverlap(cand, state.loupeHudRect, 8.0f * scale) &&
                        !rectsOverlap(cand, cursorBox, 6.0f * scale)) {
                        badgeRect = cand;
                        break;
                    }
                }
                state.hierarchyBadgeRect = badgeRect;

                if (m_infoTextBrush) {
                    DWORD now = GetTickCount();
                    DWORD remaining = (state.hierarchyBadgeFadeOutUntil > now) ? (state.hierarchyBadgeFadeOutUntil - now) : 0;
                    float alphaMultiplier = 1.0f;
                    if (remaining < 300) {
                        alphaMultiplier = static_cast<float>(remaining) / 300.0f;
                    }

                    const auto pal = FloatingGlassBar::resolveHudPalette();
                    const float badgeRadius = 9.5f * scale;
                    ComPtr<ID2D1SolidColorBrush> badgeShadow, badgeBg, badgeBorder, badgeDot;
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(0.06f, 0.09f, 0.16f, (pal.isDark ? 0.28f : 0.11f) * alphaMultiplier),
                        badgeShadow.GetAddressOf());
                    D2D1_COLOR_F bgCol = pal.panelBg;
                    bgCol.a *= alphaMultiplier;
                    D2D1_COLOR_F bdCol = pal.panelBorder;
                    bdCol.a *= alphaMultiplier;
                    D2D1_COLOR_F dotCol = pal.accent;
                    dotCol.a *= alphaMultiplier;
                    m_renderTarget->CreateSolidColorBrush(bgCol, badgeBg.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(bdCol, badgeBorder.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(dotCol, badgeDot.GetAddressOf());

                    if (badgeShadow) {
                        D2D1_RECT_F sh = D2D1::RectF(badgeRect.left, badgeRect.top + 2.0f * scale, badgeRect.right, badgeRect.bottom + 3.5f * scale);
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sh, badgeRadius, badgeRadius), badgeShadow.Get());
                    }

                    float prevTextOpacity = m_infoTextBrush->GetOpacity();
                    m_infoTextBrush->SetOpacity(prevTextOpacity * alphaMultiplier);

                    if (badgeBg) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius), badgeBg.Get());
                    }
                    if (badgeBorder) {
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(badgeRect, badgeRadius, badgeRadius), badgeBorder.Get(), 1.0f * scale);
                    }

                    const float midY = (badgeRect.top + badgeRect.bottom) * 0.5f;
                    if (badgeDot) {
                        m_renderTarget->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F(badgeRect.left + padH + 3.5f * scale, midY), 3.6f * scale, 3.6f * scale),
                            badgeDot.Get());
                    }

                    TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    auto textR = D2D1::RectF(badgeRect.left + padH + dotZone, badgeRect.top, badgeRect.right - padH + 4.0f * scale, badgeRect.bottom);
                    m_renderTarget->DrawTextW(state.hierarchyBadgeText.c_str(),
                        static_cast<UINT32>(state.hierarchyBadgeText.size()),
                        m_infoTextFormat.Get(), textR, m_infoTextBrush.Get());

                    m_infoTextBrush->SetOpacity(prevTextOpacity);
                }
            }
        }

        if (state.options.showCrosshair) {
            drawCrosshair(static_cast<float>(state.currentCursor.x),
                          static_cast<float>(state.currentCursor.y),
                          &state);
        }
        // 预选悬停时显示像素级取色放大镜（坐标 + RGB/HEX，按 C 复制）
        drawSelectionLoupe(static_cast<float>(state.currentCursor.x), static_cast<float>(state.currentCursor.y), state);
    }

    // 全阶段实时同步左下角快捷键 HUD 几何避让（含 Selecting 悬停/拖拽与 Selected/Marking 标注阶段）
    updateShortcutHintAvoidance(state);

    // 渲染微晶浮层数值反馈 (Toast)
    drawFloatingToast(state);

    recoverFromDeviceLoss(m_renderTarget->EndDraw());
}

void CaptureRenderer::drawOutlinedText(ID2D1RenderTarget* rt,
                                       const std::wstring& text,
                                       IDWriteTextFormat* format,
                                       const D2D1_RECT_F& layoutRect,
                                       ID2D1Brush* textBrush,
                                       ID2D1Brush* outlineBrush,
                                       float strokeWidth) {
    if (!rt || !format || text.empty()) return;

    if (outlineBrush && strokeWidth > 0.0f) {
        // 8 方向偏移微晶外扩描边
        const float offsets[8][2] = {
            {-strokeWidth, 0.0f},
            { strokeWidth, 0.0f},
            {0.0f, -strokeWidth},
            {0.0f,  strokeWidth},
            {-strokeWidth * 0.7071f, -strokeWidth * 0.7071f},
            {-strokeWidth * 0.7071f,  strokeWidth * 0.7071f},
            { strokeWidth * 0.7071f, -strokeWidth * 0.7071f},
            { strokeWidth * 0.7071f,  strokeWidth * 0.7071f}
        };
        for (const auto& off : offsets) {
            D2D1_RECT_F oRect = layoutRect;
            oRect.left += off[0];
            oRect.right += off[0];
            oRect.top += off[1];
            oRect.bottom += off[1];
            rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, oRect, outlineBrush);
        }
    }

    if (textBrush) {
        rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, layoutRect, textBrush);
    }
}

void CaptureRenderer::drawCustomColorIndicator(ID2D1RenderTarget* rt,
                                               const D2D1_RECT_F& rect,
                                               const MarkupColor& refColor,
                                               float scale,
                                               bool isDark) {
    CaptureVectorIcons::drawCustomColorIndicator(rt, rect, refColor, scale, isDark);
}

void CaptureRenderer::drawPresetColorSwatch(ID2D1RenderTarget* rt,
                                            const D2D1_RECT_F& rect,
                                            const MarkupColor& color,
                                            float scale,
                                            bool isHovered,
                                            ID2D1Brush* hoverBrush) {
    CaptureVectorIcons::drawPresetColorSwatch(rt, rect, color, scale, isHovered, hoverBrush);
}

} // namespace tools3000::capture
