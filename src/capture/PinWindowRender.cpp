// ─────────────────────────────────────────────────────────────────────────────
// PinWindowRender.cpp — 贴图窗口 Direct2D / DirectWrite 渲染管线子模块
//
// 职责:
//   - Direct2D 渲染目标、二维超长瓦片位图与 DirectWrite 字体资源管理
//   - 悬停工具栏平滑淡入动画与微晶矢量图标绘制
//   - 全场景贴图合成渲染（主图/像素网格/OCR 选字/悬浮工坊/原位改字焦框/二维码徽章/分组角标）
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindowInternal.h"
#include "core/i18n/UiI18n.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/PinGroupManager.h"
#include "capture/PinInputController.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include <opencv2/imgproc.hpp>
#include <d2d1_1.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace tools3000::capture {

using namespace Microsoft::WRL;
using namespace pin_detail;

bool PinWindow::ensureTextResources(float dpiScale) {
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const bool isZh = tools3000::core::isUiLanguageChinese();

    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    if (m_uiLeadTextFormat && m_uiCenterTextFormat &&
        std::abs(m_cachedTextScale - safeScale) <= 0.005f &&
        m_cachedTextChinese == isZh) {
        return true;
    }

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = tools3000::core::trWCstrFor(isZh, L"zh-CN", L"en-US");
    const float fontPx = (std::max)(12.0f, 12.0f * safeScale);

    ComPtr<IDWriteTextFormat> leadFmt;
    ComPtr<IDWriteTextFormat> centerFmt;
    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, localeName, leadFmt.GetAddressOf())) ||
        FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, localeName, centerFmt.GetAddressOf()))) {
        return false;
    }

    leadFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    leadFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    leadFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    centerFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    centerFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    centerFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteInlineObject> leadEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(leadFmt.Get(), &leadEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        leadFmt->SetTrimming(&trimming, leadEllipsis.Get());
    }
    ComPtr<IDWriteInlineObject> centerEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(centerFmt.Get(), &centerEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        centerFmt->SetTrimming(&trimming, centerEllipsis.Get());
    }

    m_uiLeadTextFormat = std::move(leadFmt);
    m_uiCenterTextFormat = std::move(centerFmt);
    m_cachedTextScale = safeScale;
    m_cachedTextChinese = isZh;
    return true;
}

float PinWindow::measureUiTextWidth(const std::wstring& text, float dpiScale) {
    if (text.empty()) return 0.0f;
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    if (ensureTextResources(safeScale) && m_dwriteFactory && m_uiLeadTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()),
                m_uiLeadTextFormat.Get(),
                4096.0f * (std::max)(1.0f, safeScale),
                128.0f * (std::max)(1.0f, safeScale),
                layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                return std::ceil(metrics.widthIncludingTrailingWhitespace);
            }
        }
    }
    return std::ceil(static_cast<float>(text.size()) * 12.0f * safeScale);
}

SIZE PinWindow::computeFoldCapsuleSize(float dpiScale) {
    const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const wchar_t* txt = tools3000::core::trWCstrFor(isZh, L"贴图 (双击展开)", L"Pinned (Folded)");
    const float textW = measureUiTextWidth(txt, safeScale);
    const int foldW = static_cast<int>(std::ceil(
        (std::max)(148.0f * safeScale, 34.0f * safeScale + textW + 14.0f * safeScale)));
    const int foldH = static_cast<int>(std::lround(38.0f * safeScale));
    return SIZE{foldW, foldH};
}

void PinWindow::updateHoverAnimation() {
    bool needsRender = false;
    float dt = (GetTickCount64() - m_hoverTime) / 150.0f;
    if (dt > 1.0f) dt = 1.0f;
    
    float targetAlpha = m_isHovering ? 1.0f : 0.0f;
    float startAlpha = m_isHovering ? 0.0f : 1.0f;
    
    float newAlpha = m_isHovering ? 
        startAlpha + (targetAlpha - startAlpha) * (1.0f - pow(1.0f - dt, 3.0f)) : 
        startAlpha + (targetAlpha - startAlpha) * dt; // linear fade out
        
    if (abs(m_hoverAlpha - newAlpha) > 0.01f) {
        m_hoverAlpha = newAlpha;
        needsRender = true;
    }
    
    if (dt >= 1.0f) {
        m_hoverAlpha = targetAlpha;
        KillTimer(m_hwnd, 1);
        needsRender = true;
    }
    
    if (needsRender) {
        render();
    }
}

void PinWindow::drawHoverToolbar() {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();

    const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
    m_dpiScale = dpiScale;

    // 悬浮工具栏：折叠、旋转、反色、编辑、文字识别/复制、原位翻译、复制图片、保存、关闭 9 个高精度矢量按钮
    // 自适应多行网格与紧凑缩放，保障窄贴图（如 400x20 旋转为 20x400 后）全部按钮仍在可视区内
    const auto layout = PinInputController::computeHoverToolbarLayout(size.width, size.height, dpiScale);
    m_toolbarRect      = layout.toolbarRect;
    m_btnFoldRect      = layout.buttons[0];
    m_btnRotateRect    = layout.buttons[1];
    m_btnInvertRect    = layout.buttons[2];
    m_btnEditRect      = layout.buttons[3];
    m_btnTextRect      = layout.buttons[4];
    m_btnTranslateRect = layout.buttons[5];
    m_btnCopyRect      = layout.buttons[6];
    m_btnSaveRect      = layout.buttons[7];
    m_btnCloseRect     = layout.buttons[8];
    
    // 绘制磨砂玻璃背景（跟随动态明暗主题与自定义强调色）
    const auto pal = FloatingGlassBar::resolveThemePalette();
    m_lastToolbarIsDark = pal.isDark;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), m_toolbarRect, 6.0f * dpiScale, pal.isDark);

    ComPtr<ID2D1SolidColorBrush> textBrush;
    D2D1_COLOR_F iconColor = pal.textPrimary;
    iconColor.a *= m_hoverAlpha;
    m_renderTarget->CreateSolidColorBrush(iconColor, textBrush.GetAddressOf());
    
    const bool translateActive = isShowingTranslation() || isTranslationInFlight();
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnFoldRect, 4.0f * dpiScale, m_hoverFold, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnRotateRect, 4.0f * dpiScale, m_hoverRotate, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnInvertRect, 4.0f * dpiScale, m_hoverInvert, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnEditRect, 4.0f * dpiScale, m_hoverEdit, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnTextRect, 4.0f * dpiScale, m_hoverText, m_textSelectionMode, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnTranslateRect, 4.0f * dpiScale, m_hoverTranslate, translateActive, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCopyRect, 4.0f * dpiScale, m_hoverCopy, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnSaveRect, 4.0f * dpiScale, m_hoverSave, false, false, false, m_hoverAlpha, pal.isDark);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCloseRect, 4.0f * dpiScale, m_hoverClose, false, true, false, m_hoverAlpha, pal.isDark);
    
    // 1. Fold 图标 (Reicon: ActionFold)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionFold, m_btnFoldRect, textBrush.Get(), dpiScale);

    // 2. Rotate 图标 (Lucide: RotateCw)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionRotate, m_btnRotateRect, textBrush.Get(), dpiScale);

    // 3. Invert 图标 (Reicon: ActionInvert)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionInvert, m_btnInvertRect, textBrush.Get(), dpiScale);

    // 4. Edit 图标 (Lucide: Pen)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ToolPen, m_btnEditRect, textBrush.Get(), dpiScale);

    // 5. Text/OCR 图标：普通图片贴图采用与主工具栏同源的 OCR 取景扫描图标，纯文本字签采用文本图标
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        m_pinnedSourceText.empty() ? CaptureIconId::ActionExtractText : CaptureIconId::ToolText,
        m_btnTextRect, textBrush.Get(), dpiScale);

    // 6. Translate 原位翻译图标 (双语互译「文 / A」)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionTranslate, m_btnTranslateRect, textBrush.Get(), dpiScale);

    // 7. Copy 图标 (Lucide: Copy)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCopy, m_btnCopyRect, textBrush.Get(), dpiScale);
    
    // 8. Save 图标 (Lucide: Download)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionSave, m_btnSaveRect, textBrush.Get(), dpiScale);
    
    // 9. Close 图标 (Lucide: X)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCancel, m_btnCloseRect, textBrush.Get(), dpiScale);

    // 悬停微晶快捷键提示胶囊 (Tooltip Pill)
    const bool isZh = tools3000::core::isUiLanguageChinese();
    const wchar_t* tipText = nullptr;
    float anchorX = 0.0f;
    if (m_hoverFold) {
        tipText = tools3000::core::trWCstrFor(isZh, L"折叠胶囊 (M)", L"Fold Capsule (M)");
        anchorX = (m_btnFoldRect.left + m_btnFoldRect.right) * 0.5f;
    } else if (m_hoverRotate) {
        tipText = tools3000::core::trWCstrFor(isZh, L"旋转 90° (R / 1·2)", L"Rotate 90° (R / 1·2)");
        anchorX = (m_btnRotateRect.left + m_btnRotateRect.right) * 0.5f;
    } else if (m_hoverInvert) {
        tipText = tools3000::core::trWCstrFor(isZh, L"色彩反转 (I)", L"Invert Colors (I)");
        anchorX = (m_btnInvertRect.left + m_btnInvertRect.right) * 0.5f;
    } else if (m_hoverEdit) {
        tipText = tools3000::core::trWCstrFor(isZh, L"二次标注 (E)", L"Annotate (E)");
        anchorX = (m_btnEditRect.left + m_btnEditRect.right) * 0.5f;
    } else if (m_hoverText) {
        if (!m_pinnedSourceText.empty()) {
            tipText = tools3000::core::trWCstrFor(isZh, L"复制原文 (Ctrl+C)", L"Copy Text (Ctrl+C)");
        } else {
            tipText = tools3000::core::trWCstrFor(isZh, L"识别文字 / 划词 (Alt)", L"Recognize Text (Alt)");
        }
        anchorX = (m_btnTextRect.left + m_btnTextRect.right) * 0.5f;
    } else if (m_hoverTranslate) {
        if (isTranslationInFlight()) {
            tipText = tools3000::core::trWCstrFor(isZh, L"正在翻译...", L"Translating...");
        } else if (isShowingTranslation()) {
            tipText = tools3000::core::trWCstrFor(isZh, L"切回原图 (Tab · Shift 重译)", L"Original (Tab · Shift Retranslate)");
        } else if (hasTranslationResult()) {
            tipText = tools3000::core::trWCstrFor(isZh, L"切回译图 (Tab · Shift 重译)", L"Translated (Tab · Shift Retranslate)");
        } else {
            tipText = tools3000::core::trWCstrFor(isZh, L"原位翻译 (Ctrl+Shift+T)", L"Translate In-Place (Ctrl+Shift+T)");
        }
        anchorX = (m_btnTranslateRect.left + m_btnTranslateRect.right) * 0.5f;
    } else if (m_hoverCopy) {
        tipText = tools3000::core::trWCstrFor(isZh, L"复制贴图 (Ctrl+C)", L"Copy Pin (Ctrl+C)");
        anchorX = (m_btnCopyRect.left + m_btnCopyRect.right) * 0.5f;
    } else if (m_hoverSave) {
        tipText = tools3000::core::trWCstrFor(isZh, L"保存图片 (Ctrl+S)", L"Save Image (Ctrl+S)");
        anchorX = (m_btnSaveRect.left + m_btnSaveRect.right) * 0.5f;
    } else if (m_hoverClose) {
        tipText = tools3000::core::trWCstrFor(isZh, L"关闭贴图 (Esc)", L"Close Pin (Esc)");
        anchorX = (m_btnCloseRect.left + m_btnCloseRect.right) * 0.5f;
    }

    if (tipText) {
        const D2D1_RECT_F baseTipRect = PinInputController::computeTooltipRect(
            m_toolbarRect, anchorX, size.width, size.height, dpiScale);
        if (baseTipRect.right > baseTipRect.left && baseTipRect.bottom > baseTipRect.top &&
            ensureTextResources(dpiScale) && m_uiCenterTextFormat && textBrush) {
            const float margin = 4.0f * dpiScale;
            const float measuredW = measureUiTextWidth(tipText, dpiScale);
            const float maxAvailW = (std::max)(40.0f * dpiScale, size.width - 2.0f * margin);
            const float tipW = std::clamp(std::ceil(measuredW + 20.0f * dpiScale), 44.0f * dpiScale, maxAvailW);
            const float maxTipX = (std::max)(margin, size.width - tipW - margin);
            const float tipX = std::clamp(anchorX - tipW * 0.5f, margin, maxTipX);
            const float desiredTipH = (std::max)(baseTipRect.bottom - baseTipRect.top, 24.0f * dpiScale);
            const float tipH = (baseTipRect.top + desiredTipH + margin <= size.height)
                ? desiredTipH
                : (baseTipRect.bottom - baseTipRect.top);
            const D2D1_RECT_F tipRect = D2D1::RectF(tipX, baseTipRect.top, tipX + tipW, baseTipRect.top + tipH);

            // 贴图工具栏悬停提示气泡（Tooltip HUD）恒定采用高级暗房深色，保持操作心流
            const auto tipPal = FloatingGlassBar::resolveHudPalette();
            FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), tipRect, 5.0f * dpiScale, true);
            ComPtr<ID2D1SolidColorBrush> tipTextBr;
            D2D1_COLOR_F tipTxtCol = tipPal.textPrimary;
            tipTxtCol.a *= m_hoverAlpha;
            m_renderTarget->CreateSolidColorBrush(tipTxtCol, tipTextBr.GetAddressOf());
            const D2D1_RECT_F tipTextRect = D2D1::RectF(
                tipRect.left + 6.0f * dpiScale,
                tipRect.top,
                (std::max)(tipRect.left + 6.0f * dpiScale, tipRect.right - 6.0f * dpiScale),
                tipRect.bottom);
            m_renderTarget->DrawTextW(
                tipText, static_cast<UINT32>(wcslen(tipText)),
                m_uiCenterTextFormat.Get(), tipTextRect, tipTextBr ? tipTextBr.Get() : textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

bool PinWindow::createRenderResources(const cv::Mat& image) {
    invalidateZoomStageTexture();
    HRESULT hr;

    if (!m_d2dFactory) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const UINT targetW = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
    const UINT targetH = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));

    if (!m_renderTarget) {
        auto rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f
        );
        // DXGI 硬件交换链单边上限为 16384；若超长贴图窗口超过硬件交换链上限则钳制后备缓冲或回退软件渲染目标
        constexpr UINT kSafeInitMaxRtDim = 16384u;
        auto hwndProps = D2D1::HwndRenderTargetProperties(
            m_hwnd,
            D2D1::SizeU((std::min)(targetW, kSafeInitMaxRtDim), (std::min)(targetH, kSafeInitMaxRtDim)),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY
        );

        hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
        if (FAILED(hr)) {
            auto swProps = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f
            );
            hr = m_d2dFactory->CreateHwndRenderTarget(swProps, hwndProps, m_renderTarget.GetAddressOf());
            if (FAILED(hr)) return false;
        }

        // 禁用 D2D 的自动 DPI 缩放
        m_renderTarget->SetDpi(96.0f, 96.0f);
    } else {
        m_renderTarget->SetDpi(96.0f, 96.0f);
        const UINT32 maxRt = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
        m_renderTarget->Resize(D2D1::SizeU((std::min)(targetW, maxRt), (std::min)(targetH, maxRt)));
    }

    // 转为 BGRA 并构建单块或二维分块 D2D Bitmap 列表（支持 >16384px 超长截图贴图）
    cv::Mat bgra;
    if (image.channels() == 3) {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    } else if (image.channels() == 1) {
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    } else if (image.channels() == 4) {
        bgra = image.clone();
        for (int r = 0; r < bgra.rows; ++r) {
            auto* p = reinterpret_cast<cv::Vec4b*>(bgra.data + static_cast<size_t>(r) * bgra.step[0]);
            for (int c = 0; c < bgra.cols; ++c) {
                const uchar a = p[c][3];
                if (a == 0) {
                    p[c][0] = 0; p[c][1] = 0; p[c][2] = 0;
                } else if (a < 255) {
                    p[c][0] = static_cast<uchar>((p[c][0] * a) / 255);
                    p[c][1] = static_cast<uchar>((p[c][1] * a) / 255);
                    p[c][2] = static_cast<uchar>((p[c][2] * a) / 255);
                }
            }
        }
    } else {
        return false;
    }

    if (!bgra.isContinuous()) {
        bgra = bgra.clone();
    }

    D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    m_bitmap.Reset();
    m_bitmapTiles.clear();

    const UINT32 maxBmp = (std::max)(256u, m_renderTarget->GetMaximumBitmapSize());
    if (static_cast<UINT32>(bgra.cols) <= maxBmp && static_cast<UINT32>(bgra.rows) <= maxBmp) {
        hr = m_renderTarget->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
            bgra.data,
            static_cast<UINT32>(bgra.step[0]),
            bitmapProps,
            m_bitmap.GetAddressOf()
        );
        if (SUCCEEDED(hr) && m_bitmap) {
            return true;
        }
        m_bitmap.Reset();
    }

    // 超出硬件最大单张纹理尺寸（如 >16384px 长截图或 8192px 显卡），自动切分为二维 ID2D1Bitmap 瓦片网格
    const int tileStep = static_cast<int>((std::min)(maxBmp, 4096u));
    for (int ty = 0; ty < bgra.rows; ty += tileStep) {
        const int th = (std::min)(tileStep, bgra.rows - ty);
        for (int tx = 0; tx < bgra.cols; tx += tileStep) {
            const int tw = (std::min)(tileStep, bgra.cols - tx);
            cv::Mat sub = bgra(cv::Rect(tx, ty, tw, th));
            if (!sub.isContinuous()) {
                sub = sub.clone();
            }
            PinBitmapTile tile;
            tile.x = tx;
            tile.y = ty;
            tile.width = tw;
            tile.height = th;
            hr = m_renderTarget->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(tw), static_cast<UINT32>(th)),
                sub.data,
                static_cast<UINT32>(sub.step[0]),
                bitmapProps,
                tile.bitmap.GetAddressOf()
            );
            if (FAILED(hr) || !tile.bitmap) {
                m_bitmapTiles.clear();
                return false;
            }
            m_bitmapTiles.push_back(std::move(tile));
        }
    }

    return !m_bitmapTiles.empty();
}

void PinWindow::render() {
    if ((!m_renderTarget || (!m_bitmap && m_bitmapTiles.empty())) &&
        !activeDisplayImage().empty() && m_hwnd && IsWindow(m_hwnd)) {
        createRenderResources(activeDisplayImage());
    }
    if (!m_renderTarget || (!m_bitmap && m_bitmapTiles.empty())) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto pal = FloatingGlassBar::resolveThemePalette();
    m_lastChromeIsDark = pal.isDark;
    auto size = m_renderTarget->GetSize();
    if (m_folded) {
        const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
        m_dpiScale = dpiScale;
        const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();

        ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush, accentBrush;
        m_renderTarget->CreateSolidColorBrush(pal.panelBg, bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(pal.textPrimary, textBrush.GetAddressOf());
        const D2D1_COLOR_F foldAccent = (m_groupId >= PinGroupConstants::kMinGroupId)
            ? PinGroupPalette::accentColor(m_groupId, 0.96f)
            : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f);
        m_renderTarget->CreateSolidColorBrush(foldAccent, accentBrush.GetAddressOf());

        const float foldStroke = 1.5f * dpiScale;
        const float foldInset = 0.75f * dpiScale;
        const float capsuleR = (std::max)(1.0f, 8.0f * dpiScale - foldInset);
        D2D1_ROUNDED_RECT capsule = D2D1::RoundedRect(
            D2D1::RectF(foldInset, foldInset,
                        (std::max)(foldInset, size.width - foldInset),
                        (std::max)(foldInset, size.height - foldInset)),
            capsuleR, capsuleR);
        if (bgBrush) m_renderTarget->FillRoundedRectangle(&capsule, bgBrush.Get());
        if (accentBrush) m_renderTarget->DrawRoundedRectangle(&capsule, accentBrush.Get(), foldStroke);

        // Fold 图标居左
        auto iconRect = D2D1::RectF(6.0f * dpiScale, 6.0f * dpiScale, 30.0f * dpiScale, size.height - 6.0f * dpiScale);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionFold, iconRect, textBrush.Get(), dpiScale);

        // 文字居右（复用缓存 DWrite 12px 格式，强制 NO_WRAP）
        if (ensureTextResources(dpiScale) && m_uiLeadTextFormat && textBrush) {
            const bool isZh = tools3000::core::isUiLanguageChinese();
            const wchar_t* txt = tools3000::core::trWCstrFor(isZh, L"贴图 (双击展开)", L"Pinned (Folded)");
            const D2D1_RECT_F textRect = D2D1::RectF(
                34.0f * dpiScale, 0.0f,
                (std::max)(34.0f * dpiScale, size.width - 6.0f * dpiScale),
                size.height);
            m_renderTarget->DrawTextW(
                txt, static_cast<UINT32>(wcslen(txt)),
                m_uiLeadTextFormat.Get(), textRect, textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else {
        // 利用 Direct2D SetTransform 硬件矩阵直接缩放绘制源纹理（或二维瓦片），
        // 避免高倍放大 (5.0x) 时分配超限重采样位图，并按视口可见范围裁剪绘制超长截图瓦片
        const float sx = (m_origWidth > 0) ? (size.width / static_cast<float>(m_origWidth)) : 1.0f;
        const float sy = (m_origHeight > 0) ? (size.height / static_cast<float>(m_origHeight)) : 1.0f;
        D2D1_MATRIX_3X2_F prevTransform = D2D1::Matrix3x2F::Identity();
        m_renderTarget->GetTransform(&prevTransform);
        m_renderTarget->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * prevTransform);

        ComPtr<ID2D1DeviceContext> dc;
        m_renderTarget.As(&dc);
        const bool pixelGridZoom = (m_scale >= 2.995f || (sx >= 2.995f && sy >= 2.995f));
        // 缩放动画进行中切换至极速硬件双线性采样跑满 120FPS 奶油丝滑，停止收敛终帧无缝切回 16-tap 高清双三次锐化
        const bool highQualityZoom = !pixelGridZoom && !m_zoomAnimating &&
            (std::abs(sx - 1.0f) > 0.01f || std::abs(sy - 1.0f) > 0.01f);
        const D2D1_INTERPOLATION_MODE dcInterp = pixelGridZoom
            ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : (highQualityZoom ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_LINEAR);
        const D2D1_BITMAP_INTERPOLATION_MODE rtInterp = pixelGridZoom
            ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;

        float visLeft = 0.0f;
        float visTop = 0.0f;
        float visRight = static_cast<float>((std::max)(1, m_origWidth));
        float visBottom = static_cast<float>((std::max)(1, m_origHeight));
        if (m_hwnd && sx > 0.0001f && sy > 0.0001f) {
            RECT winRc{};
            if (GetWindowRect(m_hwnd, &winRc)) {
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (vw > 0 && vh > 0) {
                    visLeft = (std::max)(0.0f, static_cast<float>(vx - winRc.left) / sx - 64.0f);
                    visTop = (std::max)(0.0f, static_cast<float>(vy - winRc.top) / sy - 64.0f);
                    visRight = (std::min)(visRight, static_cast<float>(vx + vw - winRc.left) / sx + 64.0f);
                    visBottom = (std::min)(visBottom, static_cast<float>(vy + vh - winRc.top) / sy + 64.0f);
                }
            }
        }

        if (m_bitmap) {
            const D2D1_RECT_F srcSpaceRect = D2D1::RectF(
                0.0f, 0.0f,
                static_cast<float>((std::max)(1, m_origWidth)),
                static_cast<float>((std::max)(1, m_origHeight)));
            if (dc) {
                dc->DrawBitmap(m_bitmap.Get(), &srcSpaceRect, 1.0f, dcInterp, nullptr, nullptr);
            } else {
                m_renderTarget->DrawBitmap(m_bitmap.Get(), srcSpaceRect, 1.0f, rtInterp);
            }
        } else {
            for (const auto& tile : m_bitmapTiles) {
                if (!tile.bitmap || tile.width <= 0 || tile.height <= 0) continue;
                const float tx0 = static_cast<float>(tile.x);
                const float ty0 = static_cast<float>(tile.y);
                const float tx1 = static_cast<float>(tile.x + tile.width);
                const float ty1 = static_cast<float>(tile.y + tile.height);
                if (tx1 <= visLeft || ty1 <= visTop || tx0 >= visRight || ty0 >= visBottom) {
                    continue;
                }
                const D2D1_RECT_F tileDst = D2D1::RectF(tx0, ty0, tx1, ty1);
                if (dc) {
                    dc->DrawBitmap(tile.bitmap.Get(), &tileDst, 1.0f, dcInterp, nullptr, nullptr);
                } else {
                    m_renderTarget->DrawBitmap(tile.bitmap.Get(), tileDst, 1.0f, rtInterp);
                }
            }
        }
        m_renderTarget->SetTransform(prevTransform);

        m_lastRenderedPixelGrid = false;
        if (pixelGridZoom && sx >= 2.995f && sy >= 2.995f) {
            ComPtr<ID2D1SolidColorBrush> gridBrush;
            const float gridAlpha = std::clamp(0.14f + (sx - 3.0f) * 0.06f, 0.14f, 0.26f);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.50f, 0.54f, 0.60f, gridAlpha), gridBrush.GetAddressOf());
            if (gridBrush) {
                const int startCol = (std::max)(1, static_cast<int>(std::floor(visLeft)));
                const int endCol = (std::min)(m_origWidth - 1, static_cast<int>(std::ceil(visRight)));
                const int startRow = (std::max)(1, static_cast<int>(std::floor(visTop)));
                const int endRow = (std::min)(m_origHeight - 1, static_cast<int>(std::ceil(visBottom)));
                const float clipTop = (std::max)(0.0f, visTop * sy);
                const float clipBottom = (std::min)(size.height, visBottom * sy);
                const float clipLeft = (std::max)(0.0f, visLeft * sx);
                const float clipRight = (std::min)(size.width, visRight * sx);
                for (int col = startCol; col <= endCol; ++col) {
                    const float xPx = std::floor(static_cast<float>(col) * sx) + 0.5f;
                    m_renderTarget->DrawLine(D2D1::Point2F(xPx, clipTop), D2D1::Point2F(xPx, clipBottom), gridBrush.Get(), 1.0f);
                }
                for (int row = startRow; row <= endRow; ++row) {
                    const float yPx = std::floor(static_cast<float>(row) * sy) + 0.5f;
                    m_renderTarget->DrawLine(D2D1::Point2F(clipLeft, yPx), D2D1::Point2F(clipRight, yPx), gridBrush.Get(), 1.0f);
                }
                m_lastRenderedPixelGrid = true;
            }
        }

        // 渲染划选与悬停的文字高亮块、微晶操作胶囊组与二维码角标 (Live Text Selection Overlay)
        m_btnCopyFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnCopyTableFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnOpenPanelFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnHighlightFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnWavyFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnUnderlineFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnTextReplaceFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnTranslateFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnTranslateStatusRect = D2D1::RectF(0, 0, 0, 0);
        m_btnUndoFloatingRect = D2D1::RectF(0, 0, 0, 0);
        m_btnColorDotFloatingRects.fill(D2D1::RectF(0, 0, 0, 0));
        clearTextReplaceToolbarRects();
        if (m_ocrReady) {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            const float dpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
            m_dpiScale = dpiScale;
            const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();

            ComPtr<ID2D1SolidColorBrush> hoverFillBrush, hoverStrokeBrush, subtleHoverBrush;
            ComPtr<ID2D1SolidColorBrush> selFillBrush, selStrokeBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.16f), hoverFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.50f), hoverStrokeBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.08f), subtleHoverBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.32f), selFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.88f), selStrokeBrush.GetAddressOf());

            // 0. 显式文字划选模式下：为全部未被选中的文字块绘制半透明引导虚光胶囊
            if (m_textSelectionMode) {
                ComPtr<ID2D1SolidColorBrush> guideFillBrush, guideStrokeBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.04f), guideFillBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.22f), guideStrokeBrush.GetAddressOf());
                for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                    bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(), i) != m_selectedBlockIndices.end();
                    if (!alreadySelected && static_cast<int>(i) != m_hoveredBlockIndex) {
                        const auto& b = m_ocrBlocks[i].origBox;
                        D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                        if (guideFillBrush) m_renderTarget->FillRoundedRectangle(&rr, guideFillBrush.Get());
                        if (guideStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, guideStrokeBrush.Get(), 0.8f * dpiScale);
                    }
                }
            }

            // 1. 悬停高亮：真正水平对齐且同区域的同行展示贯穿柔光行高亮条带，未对齐/独立块仅紧凑高亮自身
            if (m_hoveredBlockIndex >= 0 && static_cast<size_t>(m_hoveredBlockIndex) < m_ocrBlocks.size()) {
                bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(),
                                                 static_cast<size_t>(m_hoveredBlockIndex)) != m_selectedBlockIndices.end();
                if (!alreadySelected) {
                    const auto& hovBlk = m_ocrBlocks[static_cast<size_t>(m_hoveredBlockIndex)];
                    const auto& b = hovBlk.origBox;
                    const bool explicitMode = m_textSelectionMode || ((GetKeyState(VK_MENU) & 0x8000) != 0);

                    if (hovBlk.alignedRowId >= 0) {
                        int rowMinX = b.x;
                        int rowMinY = b.y;
                        int rowMaxX = b.x + b.width;
                        int rowMaxY = b.y + b.height;
                        size_t peerCount = 0;
                        for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                            const auto& peer = m_ocrBlocks[i];
                            if (peer.alignedRowId == hovBlk.alignedRowId &&
                                peer.layoutRegionId == hovBlk.layoutRegionId) {
                                rowMinX = (std::min)(rowMinX, peer.origBox.x);
                                rowMinY = (std::min)(rowMinY, peer.origBox.y);
                                rowMaxX = (std::max)(rowMaxX, peer.origBox.x + peer.origBox.width);
                                rowMaxY = (std::max)(rowMaxY, peer.origBox.y + peer.origBox.height);
                                ++peerCount;
                            }
                        }
                        if (peerCount >= 2) {
                            ComPtr<ID2D1SolidColorBrush> rowBandBrush;
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, explicitMode ? 0.11f : 0.068f),
                                rowBandBrush.GetAddressOf());
                            const D2D1_RECT_F bandRc = D2D1::RectF(
                                rowMinX * m_scale, rowMinY * m_scale,
                                rowMaxX * m_scale, rowMaxY * m_scale);
                            const D2D1_ROUNDED_RECT bandRr = D2D1::RoundedRect(bandRc, 3.0f * dpiScale, 3.0f * dpiScale);
                            if (rowBandBrush) {
                                m_renderTarget->FillRoundedRectangle(&bandRr, rowBandBrush.Get());
                            }
                        }
                    }

                    D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                    if (explicitMode) {
                        if (hoverFillBrush) m_renderTarget->FillRoundedRectangle(&rr, hoverFillBrush.Get());
                        if (hoverStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, hoverStrokeBrush.Get(), 1.0f * dpiScale);
                    } else if (subtleHoverBrush) {
                        m_renderTarget->FillRoundedRectangle(&rr, subtleHoverBrush.Get());
                    }
                }
            }

            // 2. 选中文字高亮（优先绘制字符级紧贴选区 m_selectedCharSpans）
            float minSelY = size.height;
            float maxSelY = 0.0f;
            float sumSelCenterX = 0.0f;
            size_t selRectCount = 0;

            auto recordAndDrawSelRect = [&](const D2D1_RECT_F& r) {
                if (!m_ocrMarkupPreviewClean || m_isSelectingText) {
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 2.5f * dpiScale, 2.5f * dpiScale);
                    if (selFillBrush) m_renderTarget->FillRoundedRectangle(&rr, selFillBrush.Get());
                    if (selStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, selStrokeBrush.Get(), 1.2f * dpiScale);
                }
                if (r.top < minSelY) minSelY = r.top;
                if (r.bottom > maxSelY) maxSelY = r.bottom;
                sumSelCenterX += (r.left + r.right) * 0.5f;
                ++selRectCount;
            };

            if (!m_selectedCharSpans.empty()) {
                for (const auto& sp : m_selectedCharSpans) {
                    if (sp.blockIndex >= m_ocrBlocks.size() || sp.startChar >= sp.endExclusive) continue;
                    const auto& blk = m_ocrBlocks[sp.blockIndex];
                    if (blk.charBoxes.empty() || sp.startChar >= blk.charBoxes.size()) {
                        const auto& b = blk.origBox;
                        recordAndDrawSelRect(D2D1::RectF(
                            b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale));
                        continue;
                    }
                    const size_t endIdx = (std::min)(sp.endExclusive, blk.charBoxes.size()) - 1;
                    int x1 = blk.charBoxes[sp.startChar].x;
                    int y1 = blk.charBoxes[sp.startChar].y;
                    int x2 = blk.charBoxes[sp.startChar].x + blk.charBoxes[sp.startChar].width;
                    int y2 = blk.charBoxes[sp.startChar].y + blk.charBoxes[sp.startChar].height;
                    for (size_t k = sp.startChar; k <= endIdx; ++k) {
                        const auto& cb = blk.charBoxes[k];
                        x1 = (std::min)(x1, cb.x);
                        y1 = (std::min)(y1, cb.y);
                        x2 = (std::max)(x2, cb.x + cb.width);
                        y2 = (std::max)(y2, cb.y + cb.height);
                    }
                    recordAndDrawSelRect(D2D1::RectF(
                        x1 * m_scale, y1 * m_scale, x2 * m_scale, y2 * m_scale));
                }
            } else {
                for (size_t idx : m_selectedBlockIndices) {
                    if (idx < m_ocrBlocks.size()) {
                        const auto& b = m_ocrBlocks[idx].origBox;
                        recordAndDrawSelRect(D2D1::RectF(
                            b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale));
                    }
                }
            }
            const float centerSelX = (selRectCount > 0)
                ? (sumSelCenterX / static_cast<float>(selRectCount))
                : (size.width * 0.5f);

            // 3. Alt 矩形框选模式或真正跨左右双栏围选时绘制橡皮筋引导框，确保所见即所得（杜绝无视觉提示的“隐形竖刀”截断）
            if (m_isSelectingText && (!m_isCharStreamSelecting || m_showCrossRegionRectGuide)) {
                float sx1 = static_cast<float>((std::min)(m_selectStart.x, m_selectEnd.x));
                float sy1 = static_cast<float>((std::min)(m_selectStart.y, m_selectEnd.y));
                float sx2 = static_cast<float>((std::max)(m_selectStart.x, m_selectEnd.x));
                float sy2 = static_cast<float>((std::max)(m_selectStart.y, m_selectEnd.y));
                D2D1_RECT_F selBox = D2D1::RectF(sx1, sy1, sx2, sy2);
                if (hoverFillBrush) m_renderTarget->FillRectangle(&selBox, hoverFillBrush.Get());
                if (selStrokeBrush) m_renderTarget->DrawRectangle(&selBox, selStrokeBrush.Get(), 1.0f * dpiScale);
            }

            // 4. 选区旁世界级微晶悬浮工坊（Apple / Figma 级材质与分段层级，恒定单行避让架构）：
            //    [复制] (表格) [对照] | 〔荧光笔 · 波浪线 · 直线 · 改字〕 | 5 枚高定珠宝色珠 | (撤销)
            if (!m_selectedBlockIndices.empty() && !m_isSelectingText && m_editingTextReplaceIdx < 0) {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                const bool floatingCopyAsImage = (m_ocrMarkupPreviewClean && m_pinnedSourceText.empty());
                const wchar_t* lblCopy = floatingCopyAsImage
                    ? (tools3000::core::trWCstrFor(isZh, L"复制图", L"Copy Img"))
                    : (tools3000::core::trWCstrFor(isZh, L"复制", L"Copy"));
                const wchar_t* lblTable = tools3000::core::trWCstrFor(isZh, L"复制表格", L"Table");
                const wchar_t* lblPanel = tools3000::core::trWCstrFor(isZh, L"对照", L"Panel");
                const wchar_t* lblTranslate = tools3000::core::trWCstrFor(isZh, L"翻译", L"Trans");
                const wchar_t* lblHl = tools3000::core::trWCstrFor(isZh, L"荧光笔", L"Highlight");
                const wchar_t* lblWavy = tools3000::core::trWCstrFor(isZh, L"波浪线", L"Wavy");
                const wchar_t* lblLine = tools3000::core::trWCstrFor(isZh, L"直线", L"Line");
                const wchar_t* lblReplace = tools3000::core::trWCstrFor(isZh, L"改字", L"Edit");

                auto calcBtnW = [&](const wchar_t* txt, float minDip = 54.0f) -> float {
                    const float tw = measureUiTextWidth(txt, dpiScale);
                    return std::ceil((std::max)(minDip * dpiScale, 24.5f * dpiScale + tw + 9.5f * dpiScale));
                };

                const float wCopyFull = calcBtnW(lblCopy, 54.0f);
                const float wTableFull = m_hasTableStructure ? calcBtnW(lblTable, 68.0f) : 0.0f;
                const float wPanelFull = calcBtnW(lblPanel, 54.0f);
                const float wTranslateFull = calcBtnW(lblTranslate, 54.0f);
                const float wHlFull = calcBtnW(lblHl, 64.0f);
                const float wWavyFull = calcBtnW(lblWavy, 64.0f);
                const float wLineFull = calcBtnW(lblLine, 56.0f);
                const float wReplaceFull = calcBtnW(lblReplace, 56.0f);
                const float iconBtnW = std::ceil(24.0f * dpiScale);
                const float dotCellFullW = std::ceil(18.0f * dpiScale);
                const float dotCellCompactW = std::ceil(13.5f * dpiScale);
                const float segTrackPadX = 2.0f * dpiScale;
                const float sepSpace = 6.5f * dpiScale;
                const float btnGap = 2.0f * dpiScale;
                const float outerPad = 4.0f * dpiScale;
                const float rowH = 30.5f * dpiScale;
                const float availBarW = (std::max)(36.0f * dpiScale, size.width - 12.0f * dpiScale);
                const bool canAnnotateImage = m_pinnedSourceText.empty() && !m_cleanBaseImage.empty() &&
                                              size.width >= 180.0f * dpiScale && size.height >= 44.0f * dpiScale;
                const bool showUndoBtn = canAnnotateImage && !m_pinUndoStack.empty();

                // 检查当前选区已挂载的文字批注激活态
                bool activeHl = false;
                bool activeWavy = false;
                bool activeStraight = false;
                if (canAnnotateImage && !m_markupElements.empty()) {
                    const auto curSpans = collectCoalescedSelectedTextRectsLocked();
                    if (!curSpans.empty()) {
                        for (const auto& el : m_markupElements) {
                            if (el.textSpans.empty() ||
                                !TextAnnotationHelper::spansGeometryEqual(el.textSpans, curSpans)) {
                                continue;
                            }
                            if (el.tool == MarkupTool::Highlight) activeHl = true;
                            else if (el.tool == MarkupTool::Line && el.lineStyle == LineStyle::Wavy) activeWavy = true;
                            else if (el.tool == MarkupTool::Line) activeStraight = true;
                        }
                    }
                }

                bool showTableBtn = false;
                bool showPanelBtn = false;
                bool showTranslateBtn = false;
                if (m_hasTableStructure && (outerPad * 2.0f + wCopyFull + btnGap + wTableFull + btnGap + wPanelFull) <= availBarW) {
                    showTableBtn = true;
                    showPanelBtn = true;
                } else if (m_hasTableStructure && (outerPad * 2.0f + wCopyFull + btnGap + wTableFull) <= availBarW) {
                    showTableBtn = true;
                } else if ((outerPad * 2.0f + wCopyFull + btnGap + wPanelFull) <= availBarW) {
                    showPanelBtn = true;
                }
                const float curCopyBaseW = wCopyFull +
                    (showTableBtn ? (btnGap + wTableFull) : 0.0f) +
                    (showPanelBtn ? (btnGap + wPanelFull) : 0.0f);
                if (outerPad * 2.0f + curCopyBaseW + btnGap + wTranslateFull <= availBarW) {
                    showTranslateBtn = true;
                }

                const float copyGroupFullW = curCopyBaseW +
                    (showTranslateBtn ? (btnGap + wTranslateFull) : 0.0f);
                const int copyBtnCount = 1 + (showTableBtn ? 1 : 0) + (showPanelBtn ? 1 : 0) + (showTranslateBtn ? 1 : 0);
                const float copyGroupIconW = iconBtnW * static_cast<float>(copyBtnCount) +
                    btnGap * static_cast<float>(copyBtnCount - 1);

                const float undoExtraW = showUndoBtn ? (btnGap + iconBtnW) : 0.0f;
                const float markupFullW = segTrackPadX * 2.0f + wHlFull + btnGap + wWavyFull + btnGap + wLineFull +
                    btnGap + wReplaceFull + sepSpace + (dotCellFullW * 5.0f) + undoExtraW;
                const float markupIcon5DotsW = segTrackPadX * 2.0f + iconBtnW * 4.0f + btnGap * 3.0f +
                    sepSpace + (dotCellCompactW * 5.0f) + undoExtraW;
                const float markupIcon1DotW = segTrackPadX * 2.0f + iconBtnW * 4.0f + btnGap * 3.0f +
                    sepSpace + dotCellFullW + undoExtraW;

                bool showMarkupTools = false;
                bool copyWithLabels = true;
                bool markupWithLabels = false;
                bool singleColorDot = false;
                float activeDotCellW = dotCellFullW;
                float totalBarW = wCopyFull;
                const float totalBarH = rowH;

                if (canAnnotateImage) {
                    const float tier1FullW = outerPad * 2.0f + copyGroupFullW + sepSpace + markupFullW;
                    const float tier2CopyLblMarkupIconW = outerPad * 2.0f + copyGroupFullW + sepSpace + markupIcon5DotsW;
                    const float tier3AllIcon5DotsW = outerPad * 2.0f + copyGroupIconW + sepSpace + markupIcon5DotsW;
                    const float tier4AllIcon1DotW = outerPad * 2.0f + copyGroupIconW + sepSpace + markupIcon1DotW;
                    if (tier1FullW <= availBarW) {
                        showMarkupTools = true;
                        copyWithLabels = true;
                        markupWithLabels = true;
                        singleColorDot = false;
                        activeDotCellW = dotCellFullW;
                        totalBarW = tier1FullW;
                    } else if (tier2CopyLblMarkupIconW <= availBarW) {
                        showMarkupTools = true;
                        copyWithLabels = true;
                        markupWithLabels = false;
                        singleColorDot = false;
                        activeDotCellW = dotCellCompactW;
                        totalBarW = tier2CopyLblMarkupIconW;
                    } else if (tier3AllIcon5DotsW <= availBarW) {
                        showMarkupTools = true;
                        copyWithLabels = false;
                        markupWithLabels = false;
                        singleColorDot = false;
                        activeDotCellW = dotCellCompactW;
                        totalBarW = tier3AllIcon5DotsW;
                    } else if (tier4AllIcon1DotW <= availBarW) {
                        showMarkupTools = true;
                        copyWithLabels = false;
                        markupWithLabels = false;
                        singleColorDot = true;
                        activeDotCellW = dotCellFullW;
                        totalBarW = tier4AllIcon1DotW;
                    }
                }

                if (!showMarkupTools) {
                    if (showTableBtn || showPanelBtn || showTranslateBtn) {
                        totalBarW = outerPad * 2.0f + copyGroupFullW;
                    } else if (size.width >= 36.0f * dpiScale) {
                        totalBarW = (std::min)(wCopyFull, availBarW);
                    }
                }

                const float maxBarX = (std::max)(6.0f * dpiScale, size.width - totalBarW - 6.0f * dpiScale);
                const float barX = std::clamp(centerSelX - totalBarW * 0.5f, 6.0f * dpiScale, maxBarX);
                float barY = minSelY - totalBarH - 7.0f * dpiScale;
                if (barY < 6.0f * dpiScale) {
                    barY = maxSelY + 7.0f * dpiScale;
                }
                if (m_hoverAlpha > 0.01f &&
                    PinInputController::shouldShowHoverToolbar(
                        size.width, size.height, dpiScale, !m_pinnedSourceText.empty())) {
                    const auto tbLayout = PinInputController::computeHoverToolbarLayout(
                        size.width, size.height, dpiScale);
                    const D2D1_RECT_F tb = tbLayout.toolbarRect;
                    const bool overlapsTb =
                        (barX < tb.right && barX + totalBarW > tb.left &&
                         barY < tb.bottom && barY + totalBarH > tb.top);
                    if (overlapsTb) {
                        barY = (std::max)(maxSelY + 7.0f * dpiScale, tb.bottom + 6.0f * dpiScale);
                    }
                }
                const float maxBarY = (std::max)(6.0f * dpiScale, size.height - totalBarH - 6.0f * dpiScale);
                barY = std::clamp(barY, 6.0f * dpiScale, maxBarY);

                const D2D1_RECT_F barRect = D2D1::RectF(barX, barY, barX + totalBarW, barY + totalBarH);
                FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), barRect, 9.5f * dpiScale, pal.isDark);

                ComPtr<ID2D1SolidColorBrush> pillTextBrush, activeAccentBrush;
                m_renderTarget->CreateSolidColorBrush(pal.textPrimary, pillTextBrush.GetAddressOf());
                const MarkupColor activeColor = currentTextMarkupColor();
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(activeColor.r / 255.0f, activeColor.g / 255.0f, activeColor.b / 255.0f, 1.0f),
                    activeAccentBrush.GetAddressOf());

                auto drawDivider = [&](float xPos, float topY, float botY) {
                    const float sxPos = std::floor(xPos) + 0.5f;
                    FloatingGlassBar::drawSeparator(
                        m_renderTarget.Get(), sxPos, topY + 2.5f * dpiScale, botY - 2.5f * dpiScale, 1.0f, pal.isDark);
                };

                auto drawCapsuleAction = [&](const D2D1_RECT_F& rc, CaptureIconId iconId, const wchar_t* txt,
                                             bool hovered, bool toggled = false, ID2D1Brush* customAccent = nullptr,
                                             bool segmentedElevated = false) {
                    const float pillR = 6.2f * dpiScale;
                    if (segmentedElevated && toggled) {
                        // 高定立体浮起微晶滑块卡片（告别生硬警示红框）
                        ComPtr<ID2D1SolidColorBrush> shBrush, bodyBrush, rimBrush;
                        m_renderTarget->CreateSolidColorBrush(
                            pal.isDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.36f)
                                       : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.11f),
                            shBrush.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            pal.isDark ? D2D1::ColorF(46.0f / 255.0f, 54.0f / 255.0f, 71.0f / 255.0f, 0.98f)
                                       : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f),
                            bodyBrush.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)
                                       : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.13f),
                            rimBrush.GetAddressOf());
                        if (shBrush) {
                            const D2D1_RECT_F shRc = D2D1::RectF(rc.left, rc.top + 0.9f * dpiScale,
                                                                 rc.right, rc.bottom + 1.1f * dpiScale);
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(shRc, pillR, pillR), shBrush.Get());
                        }
                        if (bodyBrush) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, pillR, pillR), bodyBrush.Get());
                        }
                        if (rimBrush) {
                            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, pillR, pillR), rimBrush.Get(), 0.9f * dpiScale);
                        }
                        if (customAccent) {
                            const float cxBtn = (rc.left + rc.right) * 0.5f;
                            const float barHalfW = (std::min)(7.0f * dpiScale, (rc.right - rc.left) * 0.24f);
                            const D2D1_RECT_F glowBar = D2D1::RectF(
                                cxBtn - barHalfW, rc.bottom - 2.3f * dpiScale,
                                cxBtn + barHalfW, rc.bottom - 0.5f * dpiScale);
                            m_renderTarget->FillRoundedRectangle(
                                D2D1::RoundedRect(glowBar, 0.9f * dpiScale, 0.9f * dpiScale), customAccent);
                        }
                    } else {
                        // 正确路由 toggled -> isActive (第 5 参)，绝不误传至 isDanger
                        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), rc, pillR,
                                                         hovered, toggled, false, false, 0.96f, pal.isDark);
                    }
                    const bool iconOnly = (!txt || txt[0] == L'\0' || (rc.right - rc.left) <= 32.0f * dpiScale);
                    const D2D1_RECT_F icRc = iconOnly
                        ? D2D1::RectF(rc.left + 2.8f * dpiScale, rc.top + 2.2f * dpiScale,
                                      rc.right - 2.8f * dpiScale, rc.bottom - 2.2f * dpiScale)
                        : D2D1::RectF(rc.left + 4.5f * dpiScale, rc.top + 2.4f * dpiScale,
                                      rc.left + 22.0f * dpiScale, rc.bottom - 2.4f * dpiScale);
                    CaptureVectorIcons::renderIconWithAccent(
                        m_renderTarget.Get(), m_d2dFactory.Get(), iconId, icRc,
                        pillTextBrush.Get(), customAccent ? customAccent : pillTextBrush.Get(), dpiScale);
                    if (!iconOnly && ensureTextResources(dpiScale) && m_uiLeadTextFormat && pillTextBrush) {
                        const D2D1_RECT_F lblRc = D2D1::RectF(
                            rc.left + 23.0f * dpiScale, rc.top,
                            (std::max)(rc.left + 23.0f * dpiScale, rc.right - 4.2f * dpiScale),
                            rc.bottom);
                        m_renderTarget->DrawTextW(
                            txt, static_cast<UINT32>(wcslen(txt)),
                            m_uiLeadTextFormat.Get(), lblRc, pillTextBrush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                };

                auto drawColorSwatches = [&](float& mx, float topY, float botY) {
                    const float midY = (topY + botY) * 0.5f;
                    ComPtr<ID2D1SolidColorBrush> ringWhiteBrush, dotShadowBrush, specGlintBrush, rimEdgeBrush;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), ringWhiteBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, pal.isDark ? 0.36f : 0.14f), dotShadowBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f), specGlintBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(
                        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f)
                                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.14f),
                        rimEdgeBrush.GetAddressOf());
                    const size_t curIdx = static_cast<size_t>(std::clamp(
                        m_textMarkupColorIndex, 0, static_cast<int>(TextAnnotationConstants::kPresetColorCount) - 1));
                    for (size_t ci = 0; ci < TextAnnotationConstants::kPresetColorCount; ++ci) {
                        if (singleColorDot && ci != curIdx) continue;
                        const auto& pc = TextAnnotationConstants::kPresetColors[ci];
                        const D2D1_RECT_F cellRc = D2D1::RectF(mx, topY, mx + activeDotCellW, botY);
                        m_btnColorDotFloatingRects[ci] = cellRc;
                        const float dotCx = (cellRc.left + cellRc.right) * 0.5f;
                        const bool isSel = (ci == curIdx);
                        const bool isHov = (static_cast<int>(ci) == m_hoveredColorDotIndex);
                        const float rDot = (isSel ? 6.2f : (isHov ? 5.8f : 5.1f)) * dpiScale;

                        ComPtr<ID2D1SolidColorBrush> dotBrush, glowBrush;
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(pc.r / 255.0f, pc.g / 255.0f, pc.b / 255.0f, 1.0f), dotBrush.GetAddressOf());
                        if (isSel || isHov) {
                            m_renderTarget->CreateSolidColorBrush(
                                D2D1::ColorF(pc.r / 255.0f, pc.g / 255.0f, pc.b / 255.0f, isSel ? 0.32f : 0.18f),
                                glowBrush.GetAddressOf());
                            if (glowBrush) {
                                m_renderTarget->FillEllipse(
                                    D2D1::Ellipse(D2D1::Point2F(dotCx, midY), rDot + 2.4f * dpiScale, rDot + 2.4f * dpiScale),
                                    glowBrush.Get());
                            }
                        }
                        if (dotShadowBrush) {
                            m_renderTarget->FillEllipse(
                                D2D1::Ellipse(D2D1::Point2F(dotCx, midY + 0.85f * dpiScale), rDot, rDot),
                                dotShadowBrush.Get());
                        }
                        if (dotBrush) {
                            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, midY), rDot, rDot), dotBrush.Get());
                        }
                        // 高定珠宝顶部镜面高光微点
                        if (specGlintBrush) {
                            m_renderTarget->FillEllipse(
                                D2D1::Ellipse(D2D1::Point2F(dotCx - rDot * 0.24f, midY - rDot * 0.32f),
                                              rDot * 0.42f, rDot * 0.24f),
                                specGlintBrush.Get());
                        }
                        if (isSel && ringWhiteBrush) {
                            m_renderTarget->DrawEllipse(
                                D2D1::Ellipse(D2D1::Point2F(dotCx, midY), rDot - 1.15f * dpiScale, rDot - 1.15f * dpiScale),
                                ringWhiteBrush.Get(), 1.35f * dpiScale);
                        } else if (rimEdgeBrush) {
                            m_renderTarget->DrawEllipse(
                                D2D1::Ellipse(D2D1::Point2F(dotCx, midY), rDot, rDot),
                                rimEdgeBrush.Get(), 0.85f * dpiScale);
                        }
                        mx += activeDotCellW;
                    }
                };

                if (showMarkupTools || showTableBtn || showPanelBtn || showTranslateBtn) {
                    float curX = barX + outerPad;
                    const float innerTop = barY + 3.2f * dpiScale;
                    const float innerBot = barY + rowH - 3.2f * dpiScale;
                    const float wCopyCur = copyWithLabels ? wCopyFull : iconBtnW;
                    const float wTableCur = copyWithLabels ? wTableFull : iconBtnW;
                    const float wPanelCur = copyWithLabels ? wPanelFull : iconBtnW;
                    const float wTranslateCur = copyWithLabels ? wTranslateFull : iconBtnW;

                    m_btnCopyFloatingRect = D2D1::RectF(curX, innerTop, curX + wCopyCur, innerBot);
                    drawCapsuleAction(m_btnCopyFloatingRect, CaptureIconId::ActionCopy,
                                      copyWithLabels ? lblCopy : L"", m_hoverCopyFloating);
                    curX += wCopyCur;

                    if (showTableBtn) {
                        curX += btnGap;
                        m_btnCopyTableFloatingRect = D2D1::RectF(curX, innerTop, curX + wTableCur, innerBot);
                        drawCapsuleAction(m_btnCopyTableFloatingRect, CaptureIconId::ActionArrangeGrid,
                                          copyWithLabels ? lblTable : L"", m_hoverCopyTableFloating);
                        curX += wTableCur;
                    }
                    if (showPanelBtn) {
                        curX += btnGap;
                        m_btnOpenPanelFloatingRect = D2D1::RectF(curX, innerTop, curX + wPanelCur, innerBot);
                        drawCapsuleAction(m_btnOpenPanelFloatingRect, CaptureIconId::ActionExtractText,
                                          copyWithLabels ? lblPanel : L"", m_hoverOpenPanelFloating);
                        curX += wPanelCur;
                    }
                    if (showTranslateBtn) {
                        curX += btnGap;
                        m_btnTranslateFloatingRect = D2D1::RectF(curX, innerTop, curX + wTranslateCur, innerBot);
                        drawCapsuleAction(m_btnTranslateFloatingRect, CaptureIconId::ActionTranslate,
                                          copyWithLabels ? lblTranslate : L"", m_hoverTranslateFloating);
                        curX += wTranslateCur;
                    }
                    if (showMarkupTools) {
                        drawDivider(curX + sepSpace * 0.5f, innerTop, innerBot);
                        curX += sepSpace;

                        const float wH = markupWithLabels ? wHlFull : iconBtnW;
                        const float wW = markupWithLabels ? wWavyFull : iconBtnW;
                        const float wU = markupWithLabels ? wLineFull : iconBtnW;
                        const float wR = markupWithLabels ? wReplaceFull : iconBtnW;
                        const float segTotalW = segTrackPadX * 2.0f + wH + btnGap + wW + btnGap + wU + btnGap + wR;

                        // 内凹微晶分段轨道槽（Recessed Segmented Control Track）
                        const D2D1_RECT_F trackRc = D2D1::RectF(
                            curX, innerTop - 1.2f * dpiScale, curX + segTotalW, innerBot + 1.2f * dpiScale);
                        const float trackR = 7.4f * dpiScale;
                        ComPtr<ID2D1SolidColorBrush> trackBg, trackRim;
                        m_renderTarget->CreateSolidColorBrush(
                            pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.052f)
                                       : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.048f),
                            trackBg.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.080f)
                                       : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.070f),
                            trackRim.GetAddressOf());
                        if (trackBg) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trackRc, trackR, trackR), trackBg.Get());
                        }
                        if (trackRim) {
                            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trackRc, trackR, trackR), trackRim.Get(), 0.85f * dpiScale);
                        }

                        curX += segTrackPadX;
                        m_btnHighlightFloatingRect = D2D1::RectF(curX, innerTop, curX + wH, innerBot);
                        drawCapsuleAction(m_btnHighlightFloatingRect, CaptureIconId::TextMarkupHighlight,
                                          markupWithLabels ? lblHl : L"", m_hoverHighlightFloating, activeHl,
                                          activeAccentBrush.Get(), true);
                        curX += wH + btnGap;

                        m_btnWavyFloatingRect = D2D1::RectF(curX, innerTop, curX + wW, innerBot);
                        drawCapsuleAction(m_btnWavyFloatingRect, CaptureIconId::TextMarkupWavy,
                                          markupWithLabels ? lblWavy : L"", m_hoverWavyFloating, activeWavy,
                                          activeAccentBrush.Get(), true);
                        curX += wW + btnGap;

                        m_btnUnderlineFloatingRect = D2D1::RectF(curX, innerTop, curX + wU, innerBot);
                        drawCapsuleAction(m_btnUnderlineFloatingRect, CaptureIconId::TextMarkupUnderline,
                                          markupWithLabels ? lblLine : L"", m_hoverUnderlineFloating, activeStraight,
                                          activeAccentBrush.Get(), true);
                        curX += wU + btnGap;

                        ComPtr<ID2D1SolidColorBrush> replaceAccentBrush;
                        m_renderTarget->CreateSolidColorBrush(
                            D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.96f), replaceAccentBrush.GetAddressOf());
                        m_btnTextReplaceFloatingRect = D2D1::RectF(curX, innerTop, curX + wR, innerBot);
                        drawCapsuleAction(m_btnTextReplaceFloatingRect, CaptureIconId::TextReplaceEdit,
                                          markupWithLabels ? lblReplace : L"", m_hoverTextReplaceFloating, false,
                                          replaceAccentBrush.Get(), true);
                        curX += wR + segTrackPadX;

                        drawDivider(curX + sepSpace * 0.5f, innerTop, innerBot);
                        curX += sepSpace;
                        drawColorSwatches(curX, innerTop, innerBot);

                        if (showUndoBtn) {
                            curX += btnGap;
                            m_btnUndoFloatingRect = D2D1::RectF(curX, innerTop, curX + iconBtnW, innerBot);
                            drawCapsuleAction(m_btnUndoFloatingRect, CaptureIconId::ActionUndo, L"", m_hoverUndoFloating);
                        }
                    }
                } else {
                    m_btnCopyFloatingRect = barRect;
                    drawCapsuleAction(m_btnCopyFloatingRect, CaptureIconId::ActionCopy, lblCopy, m_hoverCopyFloating);
                }
            }

            // 4B. 原位无痕改字实时编辑态：Direct2D GPU 顶层焦框 + 选中高亮 + 60FPS 呼吸插入光标 + 精调浮岛
            if (m_editingTextReplaceIdx >= 0 &&
                m_editingTextReplaceIdx < static_cast<int>(m_markupElements.size()) &&
                ensureTextResources(dpiScale) && m_uiCenterTextFormat) {
                const auto& ed = m_markupElements[static_cast<size_t>(m_editingTextReplaceIdx)];
                const bool isZh = tools3000::core::isUiLanguageChinese();

                // 1) GPU 顶层高定编辑焦框 + 文字选区高亮 + 清晰锐利呼吸光标（0 CPU 合成，100% 永不污染图片）
                const auto ov = TextReplaceEngine::computeEditOverlayMetrics(
                    m_cleanBaseImage, buildTextReplaceSpecForOverlay(ed));
                float anchorCx = (static_cast<float>(ed.replaceSourceRect.x) +
                                  static_cast<float>(ed.replaceSourceRect.width) * 0.5f) * sx;
                float anchorTop = static_cast<float>(ed.replaceSourceRect.y) * sy;
                float anchorBot = static_cast<float>(ed.replaceSourceRect.y + ed.replaceSourceRect.height) * sy;

                const float pad = 4.5f * dpiScale;
                const float gap = 3.0f * dpiScale;
                const float barH = 30.0f * dpiScale;
                const bool barBelow = (anchorTop - barH - 9.0f * dpiScale < 6.0f * dpiScale);

                if (ov.valid) {
                    const D2D1_RECT_F focusRc = D2D1::RectF(
                        std::clamp(ov.boxLeft * sx, 1.5f, (std::max)(2.0f, size.width - 1.5f)),
                        std::clamp(ov.boxTop * sy, 1.5f, (std::max)(2.0f, size.height - 1.5f)),
                        std::clamp(ov.boxRight * sx, 2.5f, (std::max)(3.0f, size.width - 1.5f)),
                        std::clamp(ov.boxBottom * sy, 2.5f, (std::max)(3.0f, size.height - 1.5f)));
                    m_trEditBoxClientRect = focusRc;
                    anchorCx = (focusRc.left + focusRc.right) * 0.5f;
                    anchorTop = focusRc.top;
                    anchorBot = focusRc.bottom;

                    const bool effBarBelow = (anchorTop - barH - 9.0f * dpiScale < 6.0f * dpiScale);
                    const float focusR = 4.2f * dpiScale;
                    const D2D1_RECT_F haloRc = D2D1::RectF(
                        (std::max)(0.5f, focusRc.left - 2.2f * dpiScale),
                        (std::max)(0.5f, focusRc.top - 2.2f * dpiScale),
                        (std::min)(size.width - 0.5f, focusRc.right + 2.2f * dpiScale),
                        (std::min)(size.height - 0.5f, focusRc.bottom + 2.2f * dpiScale));

                    ComPtr<ID2D1SolidColorBrush> haloBr, tintBr, borderBr, selBr, caretBr, caretGlowBr;
                    const float accR = ov.darkBackground ? (std::min)(1.0f, themeRgb.r * 0.65f + 0.35f) : themeRgb.r;
                    const float accG = ov.darkBackground ? (std::min)(1.0f, themeRgb.g * 0.65f + 0.35f) : themeRgb.g;
                    const float accB = ov.darkBackground ? (std::min)(1.0f, themeRgb.b * 0.65f + 0.35f) : themeRgb.b;

                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accR, accG, accB, 0.22f), haloBr.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accR, accG, accB, ov.darkBackground ? 0.08f : 0.055f), tintBr.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accR, accG, accB, 0.94f), borderBr.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accR, accG, accB, ov.darkBackground ? 0.36f : 0.26f), selBr.GetAddressOf());
                    // 深底光标用极高对比度纯白核心 + 主题蓝微光，浅底用饱满主题蓝核心
                    m_renderTarget->CreateSolidColorBrush(
                        ov.darkBackground ? D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.98f)
                                          : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.98f),
                        caretBr.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accR, accG, accB, 0.38f), caretGlowBr.GetAddressOf());

                    // 拖拽偏移离开原擦除区时，在旧字源区绘制极淡虚线参考幽灵框，直观提示擦除源位置（提交后自动消失）
                    if ((m_isDraggingReplacePos ||
                         std::abs(ed.replaceOffsetX) > 2.0f || std::abs(ed.replaceOffsetY) > 2.0f) &&
                        ed.replaceSourceRect.width > 0 && ed.replaceSourceRect.height > 0 && m_d2dFactory) {
                        const D2D1_RECT_F ghostRc = D2D1::RectF(
                            std::clamp(static_cast<float>(ed.replaceSourceRect.x) * sx, 1.5f, (std::max)(2.0f, size.width - 1.5f)),
                            std::clamp(static_cast<float>(ed.replaceSourceRect.y) * sy, 1.5f, (std::max)(2.0f, size.height - 1.5f)),
                            std::clamp(static_cast<float>(ed.replaceSourceRect.x + ed.replaceSourceRect.width) * sx, 2.5f, (std::max)(3.0f, size.width - 1.5f)),
                            std::clamp(static_cast<float>(ed.replaceSourceRect.y + ed.replaceSourceRect.height) * sy, 2.5f, (std::max)(3.0f, size.height - 1.5f)));
                        ComPtr<ID2D1SolidColorBrush> ghostBr;
                        ComPtr<ID2D1StrokeStyle> dashStyle;
                        const float dashes[] = {2.5f, 2.5f};
                        D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f);
                        m_d2dFactory->CreateStrokeStyle(props, dashes, 2, dashStyle.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            ov.darkBackground ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f)
                                              : D2D1::ColorF(0.15f, 0.23f, 0.35f, 0.26f),
                            ghostBr.GetAddressOf());
                        if (ghostBr && dashStyle) {
                            m_renderTarget->DrawRoundedRectangle(
                                D2D1::RoundedRect(ghostRc, 3.0f * dpiScale, 3.0f * dpiScale),
                                ghostBr.Get(), 1.05f * dpiScale, dashStyle.Get());
                        }
                    }

                    // 拖拽且与原行水平基线齐平时，左右侧外延极淡智能对齐参考丝线（0 遮挡文字，松手即隐）
                    if (m_isDraggingReplacePos && std::abs(ed.replaceOffsetY) <= 0.25f && m_d2dFactory) {
                        const float guideY = std::floor((focusRc.top + focusRc.bottom) * 0.5f) + 0.5f;
                        const float guideSpan = 28.0f * dpiScale;
                        ComPtr<ID2D1SolidColorBrush> guideBr;
                        ComPtr<ID2D1StrokeStyle> guideDash;
                        const float gDashes[] = {3.0f, 2.5f};
                        m_d2dFactory->CreateStrokeStyle(
                            D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                            gDashes, 2, guideDash.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(
                            ov.darkBackground ? D2D1::ColorF(0.20f, 0.86f, 0.68f, 0.72f)
                                              : D2D1::ColorF(0.06f, 0.65f, 0.50f, 0.72f),
                            guideBr.GetAddressOf());
                        if (guideBr && guideDash) {
                            const float l0 = (std::max)(2.0f, focusRc.left - guideSpan);
                            const float l1 = (std::max)(l0, focusRc.left - 3.0f * dpiScale);
                            const float r0 = (std::min)(size.width - 2.0f, focusRc.right + 3.0f * dpiScale);
                            const float r1 = (std::min)(size.width - 2.0f, focusRc.right + guideSpan);
                            if (l1 > l0 + 2.0f) {
                                m_renderTarget->DrawLine({l0, guideY}, {l1, guideY}, guideBr.Get(), 1.1f * dpiScale, guideDash.Get());
                            }
                            if (r1 > r0 + 2.0f) {
                                m_renderTarget->DrawLine({r0, guideY}, {r1, guideY}, guideBr.Get(), 1.1f * dpiScale, guideDash.Get());
                            }
                        }
                    }

                    // 柔光聚焦外环 + 微弱可编辑底色 + 清晰圆角编辑框
                    if (haloBr) {
                        m_renderTarget->DrawRoundedRectangle(
                            D2D1::RoundedRect(haloRc, focusR + 1.5f * dpiScale, focusR + 1.5f * dpiScale),
                            haloBr.Get(), 2.4f * dpiScale);
                    }
                    if (tintBr) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(focusRc, focusR, focusR), tintBr.Get());
                    }
                    if (selBr) {
                        for (const auto& sr : ov.selectionRects) {
                            const D2D1_RECT_F sRc = D2D1::RectF(
                                sr.x * sx, sr.y * sy, (sr.x + sr.width) * sx, (sr.y + sr.height) * sy);
                            m_renderTarget->FillRoundedRectangle(
                                D2D1::RoundedRect(sRc, 2.5f * dpiScale, 2.5f * dpiScale), selBr.Get());
                        }
                    }
                    if (borderBr) {
                        m_renderTarget->DrawRoundedRectangle(
                            D2D1::RoundedRect(focusRc, focusR, focusR), borderBr.Get(), 1.45f * dpiScale);
                    }

                    // 底部居中横向 6 点微晶药丸拖柄（彻底释放左右同行视野便于像素级对齐；
                    // 若上方工具浮岛被迫翻转到下方或贴近底边，则自动切到编辑框正上方居中，永不挡左右文字）
                    const float gripW = std::clamp((focusRc.right - focusRc.left) * 0.55f, 26.0f * dpiScale, 36.0f * dpiScale);
                    const float gripH = 11.5f * dpiScale;
                    const float gripGap = 4.0f * dpiScale;
                    const bool placeBelow = !effBarBelow && (focusRc.bottom + gripGap + gripH <= size.height - 2.0f);
                    const float gx0 = std::clamp(anchorCx - gripW * 0.5f, 2.0f, (std::max)(2.0f, size.width - gripW - 2.0f));
                    const float gy0 = placeBelow
                        ? (focusRc.bottom + gripGap)
                        : (std::max)(2.0f, focusRc.top - gripGap - gripH);
                    m_trDragHandleRect = D2D1::RectF(gx0, gy0, gx0 + gripW, gy0 + gripH);

                    const bool gripActive = m_isDraggingReplacePos || m_hoverReplaceDragHandle;
                    ComPtr<ID2D1SolidColorBrush> gripBgBr, gripDotBr;
                    m_renderTarget->CreateSolidColorBrush(
                        gripActive
                            ? D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.96f)
                            : (ov.darkBackground
                                ? D2D1::ColorF(36.0f / 255.0f, 44.0f / 255.0f, 58.0f / 255.0f, 0.92f)
                                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f)),
                        gripBgBr.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(
                        gripActive
                            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f)
                            : D2D1::ColorF(accR, accG, accB, 0.92f),
                        gripDotBr.GetAddressOf());
                    const float gripR = gripH * 0.5f;
                    if (gripBgBr) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_trDragHandleRect, gripR, gripR), gripBgBr.Get());
                    }
                    if (borderBr) {
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(m_trDragHandleRect, gripR, gripR), borderBr.Get(), 1.0f * dpiScale);
                    }
                    if (gripDotBr) {
                        const float gCx = (m_trDragHandleRect.left + m_trDragHandleRect.right) * 0.5f;
                        const float gCy = (m_trDragHandleRect.top + m_trDragHandleRect.bottom) * 0.5f;
                        const float dotR = 1.05f * dpiScale;
                        const float dxDot = 3.8f * dpiScale;
                        const float dyDot = 1.95f * dpiScale;
                        for (int col = -1; col <= 1; ++col) {
                            m_renderTarget->FillEllipse(D2D1::Ellipse({gCx + col * dxDot, gCy - dyDot}, dotR, dotR), gripDotBr.Get());
                            m_renderTarget->FillEllipse(D2D1::Ellipse({gCx + col * dxDot, gCy + dyDot}, dotR, dotR), gripDotBr.Get());
                        }
                    }

                    // 拖拽移动位置时，顺滑衔接在拖柄外侧展示 0.5px 偏移微胶囊（零遮挡左右同行文字）
                    if (m_isDraggingReplacePos && m_uiCenterTextFormat) {
                        const std::wstring posLabel = std::format(L"X {:+.1f}  Y {:+.1f}", ed.replaceOffsetX, ed.replaceOffsetY);
                        const float badgeW = (std::max)(88.0f * dpiScale, measureUiTextWidth(posLabel, dpiScale) + 14.0f * dpiScale);
                        const float badgeH = 20.0f * dpiScale;
                        const float badgeX = std::clamp(anchorCx - badgeW * 0.5f, 4.0f * dpiScale,
                                                        (std::max)(4.0f * dpiScale, size.width - badgeW - 4.0f * dpiScale));
                        float badgeY = placeBelow
                            ? (m_trDragHandleRect.bottom + 5.0f * dpiScale)
                            : (m_trDragHandleRect.top - badgeH - 5.0f * dpiScale);
                        if (badgeY + badgeH > size.height - 4.0f * dpiScale) {
                            badgeY = (std::max)(4.0f * dpiScale, focusRc.top - badgeH - 6.0f * dpiScale);
                        } else if (badgeY < 4.0f * dpiScale) {
                            badgeY = (std::min)(size.height - badgeH - 4.0f * dpiScale, focusRc.bottom + 6.0f * dpiScale);
                        }
                        const D2D1_RECT_F badgeRc = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
                        ComPtr<ID2D1SolidColorBrush> badgeBg, badgeTxt;
                        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.88f), badgeBg.GetAddressOf());
                        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.98f), badgeTxt.GetAddressOf());
                        if (badgeBg) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRc, 5.0f * dpiScale, 5.0f * dpiScale), badgeBg.Get());
                        }
                        if (badgeTxt) {
                            m_renderTarget->DrawTextW(posLabel.c_str(), static_cast<UINT32>(posLabel.size()),
                                                      m_uiCenterTextFormat.Get(), badgeRc, badgeTxt.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    }

                    // 清晰锐利 530ms 呼吸插入光标（打字/移动后 520ms 内瞬间常亮；拖拽位置期间暂隐免干扰对齐）
                    const uint32_t nowTick = GetTickCount();
                    const bool caretVisible = !m_isDraggingReplacePos && (
                        (nowTick - ed.textLastCaretTick < 520u) ||
                        (((nowTick - ed.textLastCaretTick) / 530u) % 2u == 0u));
                    if (caretVisible && caretBr) {
                        const float cxPx = std::floor(ov.caretX * sx) + 0.5f;
                        const float cTopPx = ov.caretTop * sy;
                        const float cBotPx = ov.caretBottom * sy;
                        if (caretGlowBr) {
                            m_renderTarget->DrawLine({cxPx, cTopPx}, {cxPx, cBotPx}, caretGlowBr.Get(), 4.0f * dpiScale);
                        }
                        m_renderTarget->DrawLine({cxPx, cTopPx}, {cxPx, cBotPx}, caretBr.Get(), 1.75f * dpiScale);
                    }
                } else {
                    m_trDragHandleRect = {};
                    m_trEditBoxClientRect = {};
                }
                (void)barBelow;

                const std::wstring sizeStr = std::format(L"{:.1f}", ed.fontSize);
                const wchar_t* weightLbl = (ed.replaceFontWeight >= 650) ? (tools3000::core::trWCstrFor(isZh, L"粗体", L"Bold"))
                    : ((ed.replaceFontWeight >= 550) ? (tools3000::core::trWCstrFor(isZh, L"半粗", L"Semi"))
                    : ((ed.replaceFontWeight >= 460) ? (tools3000::core::trWCstrFor(isZh, L"中等", L"Med"))
                    : (tools3000::core::trWCstrFor(isZh, L"常规", L"Reg"))));
                const wchar_t* famLbl = (ed.replaceFontClass == 1) ? (tools3000::core::trWCstrFor(isZh, L"等宽", L"Mono"))
                    : ((ed.replaceFontClass == 2) ? (tools3000::core::trWCstrFor(isZh, L"宋体", L"Serif"))
                    : (tools3000::core::trWCstrFor(isZh, L"无衬线", L"Sans")));
                const wchar_t* bgLbl = (ed.replaceBgMode == 1) ? (tools3000::core::trWCstrFor(isZh, L"平面底", L"Plane"))
                    : ((ed.replaceBgMode == 2) ? (tools3000::core::trWCstrFor(isZh, L"AI修复", L"AI Fill"))
                    : (tools3000::core::trWCstrFor(isZh, L"智能底", L"Auto")));
                const bool showReflowBtn = (ed.replaceRightTailRect.width > 0 && ed.replaceRightTailRect.height > 0);
                const wchar_t* reflowLbl = ed.replaceReflowRight
                    ? (tools3000::core::trWCstrFor(isZh, L"顺延", L"Reflow"))
                    : (tools3000::core::trWCstrFor(isZh, L"原位", L"Fixed"));
                const wchar_t* doneLbl = tools3000::core::trWCstrFor(isZh, L"完成", L"Done");

                const float wDec = 24.0f * dpiScale;
                const float wSize = 36.0f * dpiScale;
                const float wInc = 24.0f * dpiScale;
                const float wWgt = 44.0f * dpiScale;
                const float wFam = 52.0f * dpiScale;
                const float wBg  = 54.0f * dpiScale;
                const float wReflow = showReflowBtn ? (46.0f * dpiScale) : 0.0f;
                const float wDone = 48.0f * dpiScale;
                const float totalW = pad * 2.0f + wDec + wSize + wInc + wWgt + wFam + wBg +
                                     (showReflowBtn ? (wReflow + gap) : 0.0f) + wDone + gap * 6.0f;

                const float bx = std::clamp(anchorCx - totalW * 0.5f, 6.0f * dpiScale,
                                            (std::max)(6.0f * dpiScale, size.width - totalW - 6.0f * dpiScale));
                float by = anchorTop - barH - 9.0f * dpiScale;
                if (by < 6.0f * dpiScale) {
                    by = anchorBot + 9.0f * dpiScale;
                }
                by = std::clamp(by, 6.0f * dpiScale, (std::max)(6.0f * dpiScale, size.height - barH - 6.0f * dpiScale));

                const D2D1_RECT_F trBarRc = D2D1::RectF(bx, by, bx + totalW, by + barH);
                FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), trBarRc, 9.5f * dpiScale, pal.isDark);

                ComPtr<ID2D1SolidColorBrush> txtBr, subBr, accentBr, accentSoftBr, whiteBr;
                m_renderTarget->CreateSolidColorBrush(pal.textPrimary, txtBr.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(pal.textSecondary, subBr.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.96f), accentBr.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, pal.isDark ? 0.28f : 0.16f), accentSoftBr.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), whiteBr.GetAddressOf());

                const float iTop = by + 3.5f * dpiScale;
                const float iBot = by + barH - 3.5f * dpiScale;
                float cx = bx + pad;

                auto drawTrBtn = [&](D2D1_RECT_F& outRc, float w, const wchar_t* label, int btnId,
                                     bool isPrimary = false, bool isActive = false) {
                    outRc = D2D1::RectF(cx, iTop, cx + w, iBot);
                    const bool hov = (m_hoveredTrBtnId == btnId);
                    if (isPrimary && accentBr) {
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(outRc, 6.5f * dpiScale, 6.5f * dpiScale), accentBr.Get());
                        if (whiteBr) {
                            m_renderTarget->DrawTextW(label, static_cast<UINT32>(wcslen(label)),
                                                      m_uiCenterTextFormat.Get(), outRc, whiteBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    } else {
                        if (isActive && accentSoftBr) {
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(outRc, 6.0f * dpiScale, 6.0f * dpiScale), accentSoftBr.Get());
                        }
                        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), outRc, 6.0f * dpiScale,
                                                         hov, false, false, false, 0.96f, pal.isDark);
                        ID2D1SolidColorBrush* useTxt = (isActive && accentBr && !pal.isDark) ? accentBr.Get() : txtBr.Get();
                        if (useTxt) {
                            m_renderTarget->DrawTextW(label, static_cast<UINT32>(wcslen(label)),
                                                      m_uiCenterTextFormat.Get(), outRc, useTxt, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    }
                    cx += w + gap;
                };

                drawTrBtn(m_btnTrFontDecRect, wDec, L"A-", kPinBtnTrFontDec);
                {
                    const D2D1_RECT_F szRc = D2D1::RectF(cx, iTop, cx + wSize, iBot);
                    if (subBr) {
                        m_renderTarget->DrawTextW(sizeStr.c_str(), static_cast<UINT32>(sizeStr.size()),
                                                  m_uiCenterTextFormat.Get(), szRc, subBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    cx += wSize + gap;
                }
                drawTrBtn(m_btnTrFontIncRect, wInc, L"A+", kPinBtnTrFontInc);
                drawTrBtn(m_btnTrWeightRect, wWgt, weightLbl, kPinBtnTrWeight);
                drawTrBtn(m_btnTrFamilyRect, wFam, famLbl, kPinBtnTrFamily);
                drawTrBtn(m_btnTrBgModeRect, wBg, bgLbl, kPinBtnTrBgMode);
                if (showReflowBtn) {
                    drawTrBtn(m_btnTrReflowRect, wReflow, reflowLbl, kPinBtnTrReflow, false, ed.replaceReflowRight);
                } else {
                    m_btnTrReflowRect = {};
                }
                drawTrBtn(m_btnTrDoneRect, wDone, doneLbl, kPinBtnTrDone, true);
            }

            // 5. 图中二维码热区四角取景框与清透一键复制角标
            for (size_t q = 0; q < m_qrCodes.size(); ++q) {
                m_qrCodes[q].badgeRect = D2D1::RectF(0, 0, 0, 0);
                if (m_isSelectingText || size.width < 76.0f * dpiScale || size.height < 42.0f * dpiScale) {
                    continue;
                }
                const auto& qb = m_qrCodes[q].origBox;
                const D2D1_RECT_F qrc = D2D1::RectF(
                    qb.x * m_scale, qb.y * m_scale,
                    (qb.x + qb.width) * m_scale, (qb.y + qb.height) * m_scale);

                if (selStrokeBrush && (qrc.right - qrc.left) >= 16.0f && (qrc.bottom - qrc.top) >= 16.0f) {
                    const float arm = (std::min)(10.0f * dpiScale, (std::min)(qrc.right - qrc.left, qrc.bottom - qrc.top) * 0.25f);
                    const float sw = 1.6f * dpiScale;
                    // 左上、右上、右下、左下 L 型取景线
                    m_renderTarget->DrawLine({qrc.left, qrc.top}, {qrc.left + arm, qrc.top}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.left, qrc.top}, {qrc.left, qrc.top + arm}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.right - arm, qrc.top}, {qrc.right, qrc.top}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.right, qrc.top}, {qrc.right, qrc.top + arm}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.right - arm, qrc.bottom}, {qrc.right, qrc.bottom}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.right, qrc.bottom - arm}, {qrc.right, qrc.bottom}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.left, qrc.bottom}, {qrc.left + arm, qrc.bottom}, selStrokeBrush.Get(), sw);
                    m_renderTarget->DrawLine({qrc.left, qrc.bottom - arm}, {qrc.left, qrc.bottom}, selStrokeBrush.Get(), sw);
                }

                const bool isZh = tools3000::core::isUiLanguageChinese();
                const wchar_t* qrLabel = tools3000::core::trWCstrFor(isZh, L"复制二维码", L"Copy QR");
                const float qrTw = measureUiTextWidth(qrLabel, dpiScale);
                const float qrW = (std::min)(
                    std::ceil((std::max)(72.0f * dpiScale, 24.0f * dpiScale + qrTw + 10.0f * dpiScale)),
                    (std::max)(48.0f * dpiScale, size.width - 12.0f * dpiScale));
                const float qrH = 24.0f * dpiScale;
                const float qrCenterX = (qrc.left + qrc.right) * 0.5f;
                const float qrX = std::clamp(qrCenterX - qrW * 0.5f, 6.0f * dpiScale,
                                             (std::max)(6.0f * dpiScale, size.width - qrW - 6.0f * dpiScale));
                float qrY = qrc.bottom + 5.0f * dpiScale;
                if (qrY + qrH + 4.0f * dpiScale > size.height) {
                    qrY = (std::max)(6.0f * dpiScale, qrc.top - qrH - 5.0f * dpiScale);
                }
                qrY = std::clamp(qrY, 6.0f * dpiScale, (std::max)(6.0f * dpiScale, size.height - qrH - 6.0f * dpiScale));

                const D2D1_RECT_F badgeRc = D2D1::RectF(qrX, qrY, qrX + qrW, qrY + qrH);
                m_qrCodes[q].badgeRect = badgeRc;

                const bool qrHovered = (m_hoveredQrBadgeIndex == static_cast<int>(q));
                FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), badgeRc, 5.0f * dpiScale, pal.isDark);
                FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), badgeRc, 4.0f * dpiScale,
                                                qrHovered, false, false, false, 0.94f, pal.isDark);

                ComPtr<ID2D1SolidColorBrush> qrTxtBr;
                m_renderTarget->CreateSolidColorBrush(pal.textPrimary, qrTxtBr.GetAddressOf());
                const D2D1_RECT_F qrIcRc = D2D1::RectF(
                    qrX + 4.0f * dpiScale, qrY + 3.0f * dpiScale,
                    qrX + 21.0f * dpiScale, qrY + qrH - 3.0f * dpiScale);
                CaptureVectorIcons::renderIcon(
                    m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropQrCode, qrIcRc, qrTxtBr.Get(), dpiScale);
                if (ensureTextResources(dpiScale) && m_uiLeadTextFormat && qrTxtBr && qrW > 34.0f * dpiScale) {
                    const D2D1_RECT_F lblRc = D2D1::RectF(
                        qrX + 22.5f * dpiScale, qrY,
                        (std::max)(qrX + 22.5f * dpiScale, qrX + qrW - 4.0f * dpiScale), qrY + qrH);
                    m_renderTarget->DrawTextW(
                        qrLabel, static_cast<UINT32>(wcslen(qrLabel)),
                        m_uiLeadTextFormat.Get(), lblRc, qrTxtBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }

        const float curDpiScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : m_dpiScale;
        const bool showHoverToolbar = PinInputController::shouldShowHoverToolbar(
            size.width, size.height, curDpiScale, !m_pinnedSourceText.empty());
        if (m_hoverAlpha > 0.01f && !m_isSelectingText && showHoverToolbar) {
            drawHoverToolbar();
        } else {
            clearHoverToolbarRects();
        }

        // 左上角微晶分组胶囊：已分组贴图常驻显示宝石色组标；未分组贴图仅在大尺寸贴图悬停时浮现分组快捷入口（小贴图保持 100% 零遮挡）
        m_btnGroupBadgeRect = {};
        if (!m_isSelectingText) {
            const float dpiScale = curDpiScale;
            const bool hasGroup = (m_groupId >= PinGroupConstants::kMinGroupId);
            const bool showUngroupedBtn = (!hasGroup && m_hoverAlpha > 0.05f && showHoverToolbar);
            if ((hasGroup || showUngroupedBtn) && size.width >= 68.0f * dpiScale && size.height >= 36.0f * dpiScale) {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                const float pad = 8.0f * dpiScale;
                const float pillH = 24.0f * dpiScale;
                std::wstring badgeLabel;
                float pillW = 28.0f * dpiScale;
                if (hasGroup) {
                    badgeLabel = (m_hoverAlpha > 0.25f && size.width >= 180.0f * dpiScale)
                        ? PinGroupPalette::shortLabel(m_groupId, isZh)
                        : std::format(L"{}", m_groupId);
                    const std::wstring customLabel = PinGroupPalette::customName(m_groupId);
                    if (!customLabel.empty() && size.width >= 108.0f * dpiScale) {
                        if (m_hoverAlpha > 0.25f && size.width >= 156.0f * dpiScale) {
                            badgeLabel = PinGroupPalette::shortLabel(m_groupId, isZh);
                        } else if (customLabel.size() > 8) {
                            badgeLabel = customLabel.substr(0, 8) + L"…";
                        } else {
                            badgeLabel = customLabel;
                        }
                    }
                    const float tw = measureUiTextWidth(badgeLabel, dpiScale);
                    pillW = std::ceil(18.0f * dpiScale + tw + 8.0f * dpiScale);
                }

                const D2D1_RECT_F candidate = D2D1::RectF(pad, pad, pad + pillW, pad + pillH);
                const bool overlapsToolbar =
                    (m_hoverAlpha > 0.01f && m_toolbarRect.right > m_toolbarRect.left &&
                     candidate.right + 4.0f * dpiScale > m_toolbarRect.left &&
                     candidate.top < m_toolbarRect.bottom && candidate.bottom > m_toolbarRect.top);
                if (!overlapsToolbar) {
                    m_btnGroupBadgeRect = candidate;
                    const float badgeAlpha = hasGroup ? 0.96f : (0.92f * m_hoverAlpha);
                    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), candidate, 6.0f * dpiScale, pal.isDark);
                    if (m_hoverGroupBadge) {
                        FloatingGlassBar::drawButtonPill(
                            m_renderTarget.Get(), candidate, 5.0f * dpiScale, true, false, false, false, badgeAlpha, pal.isDark);
                    }
                    if (hasGroup) {
                        ComPtr<ID2D1SolidColorBrush> dotBr, txtBr;
                        m_renderTarget->CreateSolidColorBrush(
                            PinGroupPalette::accentColor(m_groupId, badgeAlpha), dotBr.GetAddressOf());
                        D2D1_COLOR_F badgeTxt = pal.textPrimary;
                        badgeTxt.a *= badgeAlpha;
                        m_renderTarget->CreateSolidColorBrush(badgeTxt, txtBr.GetAddressOf());
                        const float dotR = 3.8f * dpiScale;
                        const float dotCx = candidate.left + 8.0f * dpiScale + dotR;
                        const float dotCy = (candidate.top + candidate.bottom) * 0.5f;
                        if (dotBr) {
                            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotCx, dotCy), dotR, dotR), dotBr.Get());
                        }
                        if (txtBr && ensureTextResources(dpiScale) && m_uiCenterTextFormat) {
                            const D2D1_RECT_F tRect = D2D1::RectF(
                                dotCx + dotR + 2.5f * dpiScale, candidate.top,
                                candidate.right - 5.0f * dpiScale, candidate.bottom);
                            m_renderTarget->DrawTextW(
                                badgeLabel.c_str(), static_cast<UINT32>(badgeLabel.size()),
                                m_uiCenterTextFormat.Get(), tRect, txtBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                    } else {
                        ComPtr<ID2D1SolidColorBrush> iconBr;
                        D2D1_COLOR_F badgeIcon = pal.textPrimary;
                        badgeIcon.a *= badgeAlpha;
                        m_renderTarget->CreateSolidColorBrush(badgeIcon, iconBr.GetAddressOf());
                        if (iconBr) {
                            CaptureVectorIcons::renderIcon(
                                m_renderTarget.Get(), m_d2dFactory.Get(),
                                CaptureIconId::ActionPinGroup, candidate, iconBr.Get(), dpiScale);
                        }
                    }
                }
            }
        }

        // 边框：全部按半线宽内缩，防止 HwndRenderTarget 边界裁掉外半边线宽；微晶字签/色卡签同步采用同心真圆角描边
        const float scaledCardR = (m_cardCornerRadius > 0.5f)
            ? std::clamp(m_cardCornerRadius * m_scale, 3.0f, (std::max)(3.0f, (std::min)(size.width, size.height) * 0.5f))
            : 0.0f;
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (m_focused) {
            const tools3000::core::AccentColorRGB themeRgb = FloatingGlassBar::effectiveAccentRGB();
            const D2D1_COLOR_F focusCol = (m_groupId >= PinGroupConstants::kMinGroupId)
                ? PinGroupPalette::accentColor(m_groupId, 0.96f)
                : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f);
            m_renderTarget->CreateSolidColorBrush(focusCol, borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(1.0f, 1.0f, (std::max)(1.0f, size.width - 1.0f), (std::max)(1.0f, size.height - 1.0f));
            if (scaledCardR > 0.5f) {
                const float r = (std::max)(2.0f, scaledCardR - 1.0f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 2.0f);
            } else {
                m_renderTarget->DrawRectangle(rc, borderBrush.Get(), 2.0f);
            }
        } else if (m_clickThrough) {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.85f, 1.0f, 0.65f), borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(0.75f, 0.75f, (std::max)(0.75f, size.width - 0.75f), (std::max)(0.75f, size.height - 0.75f));
            if (scaledCardR > 0.5f) {
                const float r = (std::max)(2.0f, scaledCardR - 0.75f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 1.5f);
            } else {
                m_renderTarget->DrawRectangle(rc, borderBrush.Get(), 1.5f);
            }
        } else if (scaledCardR > 0.5f) {
            // 纯白微晶字签/色卡签在未获焦态补充 1.0f 浅灰外边框保护，确保贴在白色网页上也边界分明
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.80f, 0.86f, 0.78f), borderBrush.GetAddressOf());
            const D2D1_RECT_F rc = D2D1::RectF(0.5f, 0.5f, (std::max)(0.5f, size.width - 0.5f), (std::max)(0.5f, size.height - 0.5f));
            const float r = (std::max)(2.0f, scaledCardR - 0.5f);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), borderBrush.Get(), 1.0f);
        } else {
            // 双层抗融边高反差微晶轮廓：外环深影防纯白网页融边，内环微高光防纯黑终端/IDE 融边
            ComPtr<ID2D1SolidColorBrush> innerRimBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f), innerRimBrush.GetAddressOf());
            if (innerRimBrush && size.width > 3.0f && size.height > 3.0f) {
                m_renderTarget->DrawRectangle(
                    D2D1::RectF(1.5f, 1.5f, size.width - 1.5f, size.height - 1.5f),
                    innerRimBrush.Get(), 1.0f);
            }
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.45f), borderBrush.GetAddressOf());
            m_renderTarget->DrawRectangle(
                D2D1::RectF(0.5f, 0.5f, (std::max)(0.5f, size.width - 0.5f), (std::max)(0.5f, size.height - 0.5f)),
                borderBrush.Get(), 1.0f);
        }

        // 后台首次显式异步 OCR / 原位翻译等待期微胶囊与译图对照交互胶囊
        m_lastRenderedOcrBadge = false;
        const bool translateWaiting = m_translationInFlight.load(std::memory_order_acquire);
        const bool ocrWaiting = ((!m_silentOcrPreload.load(std::memory_order_acquire) &&
                                  m_ocrInFlightVersion.load(std::memory_order_acquire) != 0 &&
                                  !m_ocrReady.load(std::memory_order_acquire)) ||
                                 m_extractTextInFlight.load(std::memory_order_acquire));
        const uint64_t nowHudTick = GetTickCount64();
        const bool showPctBadge = (!m_hudBadgeText.empty() && nowHudTick < m_hudBadgeUntilTick);
        const bool showTranslatedPill = (isShowingTranslation() && !translateWaiting &&
                                         !m_isSelectingText && m_editingTextReplaceIdx < 0 &&
                                         !showPctBadge &&
                                         size.width >= 110.0f * curDpiScale && size.height >= 48.0f * curDpiScale);
        if ((ocrWaiting || translateWaiting || showPctBadge || showTranslatedPill) &&
            size.width >= 44.0f && size.height >= 24.0f) {
            const float hudScale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : (m_dpiScale > 0.0f ? m_dpiScale : 1.0f);
            if (ensureTextResources(hudScale) && m_uiCenterTextFormat) {
                const auto hudPal = FloatingGlassBar::resolveHudPalette();
                ComPtr<ID2D1SolidColorBrush> hudTxtBr;
                m_renderTarget->CreateSolidColorBrush(hudPal.textPrimary, hudTxtBr.GetAddressOf());

                auto drawCapsulePill = [&](const std::wstring& pillText, float preferCenterY, bool hoveredPill = false) -> D2D1_RECT_F {
                    const float textW = measureUiTextWidth(pillText, hudScale);
                    const float pillH = (std::min)(size.height - 4.0f, 24.0f * hudScale);
                    const float pillW = std::clamp(textW + 20.0f * hudScale, 42.0f * hudScale, (std::max)(42.0f * hudScale, size.width - 8.0f));
                    const float left = (size.width - pillW) * 0.5f;
                    const float top = std::clamp(preferCenterY - pillH * 0.5f, 4.0f, (std::max)(4.0f, size.height - pillH - 4.0f));
                    const D2D1_RECT_F pillRect = D2D1::RectF(left, top, left + pillW, top + pillH);
                    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), pillRect, pillH * 0.5f, hudPal.isDark);
                    if (hoveredPill) {
                        FloatingGlassBar::drawButtonPill(
                            m_renderTarget.Get(), pillRect, pillH * 0.5f, true, false, false, false, 0.96f, hudPal.isDark);
                    }
                    if (hudTxtBr) {
                        m_renderTarget->DrawTextW(
                            pillText.c_str(), static_cast<UINT32>(pillText.size()),
                            m_uiCenterTextFormat.Get(), pillRect, hudTxtBr.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    return pillRect;
                };

                if (ocrWaiting || translateWaiting) {
                    const bool isZh = tools3000::core::isUiLanguageChinese();
                    const std::wstring waitText = translateWaiting
                        ? tools3000::core::trWCstrFor(isZh, L"正在翻译...", L"Translating...")
                        : tools3000::core::trWCstrFor(isZh, L"正在识别...", L"Recognizing...");
                    const float topAnchor = (m_hoverAlpha > 0.05f && m_toolbarRect.bottom > m_toolbarRect.top)
                        ? (m_toolbarRect.bottom + 16.0f * hudScale)
                        : (size.height * 0.5f);
                    drawCapsulePill(waitText, topAnchor);
                    m_lastRenderedOcrBadge = true;
                }
                if (showPctBadge) {
                    const float bottomAnchor = size.height - 20.0f * hudScale;
                    drawCapsulePill(m_hudBadgeText, bottomAnchor);
                } else if (showTranslatedPill) {
                    const bool isZh = tools3000::core::isUiLanguageChinese();
                    const std::wstring statusText = (size.width >= 200.0f * hudScale)
                        ? tools3000::core::trWCstrFor(isZh, L"译图 · 点击/Tab 切原图", L"Translated · Tab: Original")
                        : tools3000::core::trWCstrFor(isZh, L"译图 · 原图", L"Translated");
                    const float bottomAnchor = size.height - 18.0f * hudScale;
                    m_btnTranslateStatusRect = drawCapsulePill(statusText, bottomAnchor, m_hoverTranslateStatus);
                }
            }
        }
    }

    const HRESULT hrEnd = m_renderTarget->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET ||
        hrEnd == DXGI_ERROR_DEVICE_REMOVED ||
        hrEnd == DXGI_ERROR_DEVICE_RESET) {
        m_renderTarget.Reset();
        m_bitmap.Reset();
        m_bitmapTiles.clear();
        if (m_hwnd && IsWindow(m_hwnd)) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}
} // namespace tools3000::capture
