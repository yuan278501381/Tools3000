// ─────────────────────────────────────────────────────────────────────────────
// CaptureRenderer.cpp — 截图渲染器核心生命周期、D2D/DWrite 资源、DPI、选区与尺寸信息 HUD
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

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace renderer_detail {

bool isCaptureUiChinese() {
    return tools3000::core::isUiLanguageChinese();
}

void rgbToHsl(int r, int g, int b, int& h, int& s, int& l) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxVal = std::max({rf, gf, bf}), minVal = std::min({rf, gf, bf});
    float delta = maxVal - minVal;
    float lf = (maxVal + minVal) * 0.5f;
    float sf = 0.0f, hf = 0.0f;
    if (delta > 1e-5f) {
        sf = lf > 0.5f ? delta / (2.0f - maxVal - minVal) : delta / (maxVal + minVal);
        if (maxVal == rf) {
            hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (maxVal == gf) {
            hf = (bf - rf) / delta + 2.0f;
        } else {
            hf = (rf - gf) / delta + 4.0f;
        }
        hf /= 6.0f;
    }
    h = static_cast<int>(std::round(hf * 360.0f)) % 360;
    s = static_cast<int>(std::round(sf * 100.0f));
    l = static_cast<int>(std::round(lf * 100.0f));
}

void rgbToHsv(int r, int g, int b, int& h, int& s, int& v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxVal = std::max({rf, gf, bf}), minVal = std::min({rf, gf, bf});
    float delta = maxVal - minVal;
    float vf = maxVal;
    float sf = maxVal > 1e-5f ? delta / maxVal : 0.0f;
    float hf = 0.0f;
    if (delta > 1e-5f) {
        if (maxVal == rf) {
            hf = (gf - bf) / delta + (gf < bf ? 6.0f : 0.0f);
        } else if (maxVal == gf) {
            hf = (bf - rf) / delta + 2.0f;
        } else {
            hf = (rf - gf) / delta + 4.0f;
        }
        hf /= 6.0f;
    }
    h = static_cast<int>(std::round(hf * 360.0f)) % 360;
    s = static_cast<int>(std::round(sf * 100.0f));
    v = static_cast<int>(std::round(vf * 100.0f));
}

void rgbToCmyk(int r, int g, int b, int& c, int& m, int& y, int& k) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float kf = 1.0f - std::max({rf, gf, bf});
    if (kf < 1.0f - 1e-5f) {
        c = static_cast<int>(std::round((1.0f - rf - kf) / (1.0f - kf) * 100.0f));
        m = static_cast<int>(std::round((1.0f - gf - kf) / (1.0f - kf) * 100.0f));
        y = static_cast<int>(std::round((1.0f - bf - kf) / (1.0f - kf) * 100.0f));
    } else {
        c = m = y = 0;
    }
    k = static_cast<int>(std::round(kf * 100.0f));
}


std::vector<ColorFormatEntry> getAllColorFormats(int r, int g, int b) {
    int hslH, hslS, hslL;
    rgbToHsl(r, g, b, hslH, hslS, hslL);

    int hsvH, hsvS, hsvV;
    rgbToHsv(r, g, b, hsvH, hsvS, hsvV);

    int cmykC, cmykM, cmykY, cmykK;
    rgbToCmyk(r, g, b, cmykC, cmykM, cmykY, cmykK);

    uint32_t decVal = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);

    std::vector<ColorFormatEntry> list;
    list.push_back({
        ColorFormatType::HEX, L"HEX",
        std::format(L"#{:02X}{:02X}{:02X}", r, g, b),
        std::format("#{:02X}{:02X}{:02X}", r, g, b)
    });
    list.push_back({
        ColorFormatType::RGB, L"RGB",
        std::format(L"rgb({}, {}, {})", r, g, b),
        std::format("rgb({}, {}, {})", r, g, b)
    });
    list.push_back({
        ColorFormatType::RGBA, L"RGBA",
        std::format(L"rgba({}, {}, {}, 1.0)", r, g, b),
        std::format("rgba({}, {}, {}, 1.0)", r, g, b)
    });
    list.push_back({
        ColorFormatType::HEX_0x, L"0xHEX",
        std::format(L"0x{:02X}{:02X}{:02X}", r, g, b),
        std::format("0x{:02X}{:02X}{:02X}", r, g, b)
    });
    list.push_back({
        ColorFormatType::HSL, L"HSL",
        std::format(L"hsl({}, {}%, {}%)", hslH, hslS, hslL),
        std::format("hsl({}, {}%, {}%)", hslH, hslS, hslL)
    });
    list.push_back({
        ColorFormatType::HSV, L"HSV",
        std::format(L"hsv({}, {}%, {}%)", hsvH, hsvS, hsvV),
        std::format("hsv({}, {}%, {}%)", hsvH, hsvS, hsvV)
    });
    list.push_back({
        ColorFormatType::CMYK, L"CMYK",
        std::format(L"{}%, {}%, {}%, {}%", cmykC, cmykM, cmykY, cmykK),
        std::format("cmyk({}%, {}%, {}%, {}%)", cmykC, cmykM, cmykY, cmykK)
    });
    list.push_back({
        ColorFormatType::DEC, L"DEC",
        std::format(L"{}", decVal),
        std::format("{}", decVal)
    });

    return list;
}


} // namespace renderer_detail

D2D1_SIZE_F CaptureRenderer::measureText(const std::wstring& text, IDWriteTextFormat* format, float maxWidth, float maxHeight) const {
    if (!m_dwriteFactory || text.empty()) return D2D1::SizeF(0.0f, 0.0f);
    IDWriteTextFormat* fmt = format ? format : m_infoTextFormat.Get();
    if (!fmt) return D2D1::SizeF(0.0f, 0.0f);

    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt,
            maxWidth, maxHeight, layout.GetAddressOf())) && layout) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) {
            return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
        }
    }
    return D2D1::SizeF(static_cast<float>(text.size()) * 12.0f, 18.0f);
}

bool CaptureRenderer::initialize(HWND hwnd, CaptureState& state) {
    releaseWindowResources();
    m_hwnd = hwnd;
    if (createRenderResources(state)) return true;
    releaseWindowResources();
    return false;
}

void CaptureRenderer::shutdown() {
    releaseWindowResources();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void CaptureRenderer::applyThemeColors() {
    if (!m_renderTarget) return;

    const auto pal = FloatingGlassBar::resolveHudPalette();
    m_renderTarget->CreateSolidColorBrush(pal.panelBg, m_infoBgBrush.ReleaseAndGetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, m_infoTextBrush.ReleaseAndGetAddressOf());

    // 截图与录屏选区采用纯净零色偏白晶主线，杜绝彩色边框干扰被截画面原色判断
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
        m_borderBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f),
        m_crosshairBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f),
        m_windowHighlightBrush.ReleaseAndGetAddressOf()
    );
}

bool CaptureRenderer::createRenderResources(CaptureState& state) {
    HRESULT hr;

    if (!m_d2dFactory) {
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    if (!m_dwriteFactory) {
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (FAILED(hr)) return false;
    }

    if (!updateDpiScale(state.dpiScale)) return false;

    RECT rc;
    GetClientRect(m_hwnd, &rc);

    auto rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    auto hwndProps = D2D1::HwndRenderTargetProperties(
        m_hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top),
        D2D1_PRESENT_OPTIONS_IMMEDIATELY
    );

    hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // 禁用 D2D 的自动 DPI 缩放
    m_renderTarget->SetDpi(96.0f, 96.0f);

    // 画笔（信息浮层文本统一对齐雪瓷白底高对比曜石黑令牌）
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), m_dimBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, m_infoBgBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, m_infoTextBrush.GetAddressOf());
    applyThemeColors();

    return true;
}

void CaptureRenderer::releaseWindowResources() {
    m_windowHighlightBrush.Reset();
    m_crosshairBrush.Reset();
    m_infoTextBrush.Reset();
    m_infoBgBrush.Reset();
    m_borderBrush.Reset();
    m_dimBrush.Reset();
    m_screenBitmap.Reset();
    m_screenTiles.clear();
    m_screenTilesW = 0;
    m_screenTilesH = 0;
    m_markupCacheBitmap.Reset();
    m_markupCacheTiles.clear();
    m_markupTilesW = 0;
    m_markupTilesH = 0;
    m_minimapBitmap.Reset();
    m_minimapSrcW = 0;
    m_minimapSrcH = 0;
    m_historyBitmap.Reset();
    m_historyTiles.clear();
    m_historyTilesW = 0;
    m_historyTilesH = 0;
    m_markupClipLayer.Reset();
    m_numberBadgeSmallFormat.Reset();
    m_numberBadgeMediumFormat.Reset();
    m_numberBadgeLargeFormat.Reset();
    m_textInputFormat.Reset();
    m_hudBadgeFormat.Reset();
    m_hudPrimaryFormat.Reset();
    m_infoTextFormat.Reset();
    m_textScale = 0.0f;
    m_renderTarget.Reset();
    m_hwnd = nullptr;
}

void CaptureRenderer::resolveMonitorBoundsInCanvas(float canvasX,
                                                   float canvasY,
                                                   D2D1_SIZE_F surfaceSize,
                                                   D2D1_RECT_F& outMonitorRect,
                                                   D2D1_RECT_F& outWorkRect) const {
    outMonitorRect = D2D1::RectF(0.0f, 0.0f, surfaceSize.width, surfaceSize.height);
    outWorkRect = outMonitorRect;
    if (!m_hwnd || !IsWindow(m_hwnd) || surfaceSize.width <= 0.0f || surfaceSize.height <= 0.0f) {
        return;
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    RECT winRc{};
    if (GetWindowRect(m_hwnd, &winRc)) {
        vx = winRc.left;
        vy = winRc.top;
    }

    const POINT ptScreen{
        vx + static_cast<LONG>(std::lround(canvasX)),
        vy + static_cast<LONG>(std::lround(canvasY))
    };
    const HMONITOR hMon = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!hMon || !GetMonitorInfoW(hMon, &mi)) {
        return;
    }

    const D2D1_RECT_F monInCanvas = D2D1::RectF(
        (std::max)(0.0f, static_cast<float>(mi.rcMonitor.left - vx)),
        (std::max)(0.0f, static_cast<float>(mi.rcMonitor.top - vy)),
        (std::min)(surfaceSize.width, static_cast<float>(mi.rcMonitor.right - vx)),
        (std::min)(surfaceSize.height, static_cast<float>(mi.rcMonitor.bottom - vy)));
    if (monInCanvas.right > monInCanvas.left + 32.0f && monInCanvas.bottom > monInCanvas.top + 32.0f) {
        outMonitorRect = monInCanvas;
    }

    const D2D1_RECT_F workInCanvas = D2D1::RectF(
        (std::max)(0.0f, static_cast<float>(mi.rcWork.left - vx)),
        (std::max)(0.0f, static_cast<float>(mi.rcWork.top - vy)),
        (std::min)(surfaceSize.width, static_cast<float>(mi.rcWork.right - vx)),
        (std::min)(surfaceSize.height, static_cast<float>(mi.rcWork.bottom - vy)));
    if (workInCanvas.right > workInCanvas.left + 32.0f && workInCanvas.bottom > workInCanvas.top + 32.0f) {
        outWorkRect = workInCanvas;
    } else {
        outWorkRect = outMonitorRect;
    }
}

bool CaptureRenderer::createTextFormatsForScale(float scale) {
    if (!m_dwriteFactory) return false;

    const wchar_t* uiFontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());

    // 1. 通用工具栏胶囊按钮 / 弹出菜单 / 滑块控件字体：黄金 13.0px + 强制不折行，彻底消除文字撑破内框与折行重叠
    ComPtr<IDWriteTextFormat> infoFormat;
    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"zh-CN",
        infoFormat.GetAddressOf());
    if (FAILED(hr) || !infoFormat) return false;
    infoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    infoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    infoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 2. 尺寸仪表主读数字体 (W × H) 与顶部主提示：16.0px 醒目高对比
    ComPtr<IDWriteTextFormat> hudPrimaryFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 16.0f * scale, L"zh-CN",
        hudPrimaryFormat.GetAddressOf());
    if (FAILED(hr) || !hudPrimaryFormat) return false;
    hudPrimaryFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    hudPrimaryFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hudPrimaryFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 3. 尺寸单位后缀 (px / dp)、坐标、微晶徽章与辅助说明字体：12.5px 精致次级层级
    ComPtr<IDWriteTextFormat> hudBadgeFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.5f * scale, L"zh-CN",
        hudBadgeFormat.GetAddressOf());
    if (FAILED(hr) || !hudBadgeFormat) return false;
    hudBadgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    hudBadgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    hudBadgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // 4. 画布文本标注输入基准字体
    ComPtr<IDWriteTextFormat> textInputFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        uiFontFamily, nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 18.0f * scale, L"zh-CN",
        textInputFormat.GetAddressOf());
    if (FAILED(hr) || !textInputFormat) return false;
    textInputFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textInputFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // 5. 序号标注实时拖拽徽章三档缓存字体（字号底线 >= 12px，杜绝渲染热路径每帧 CreateTextFormat）
    auto makeNumberBadgeFormat = [&](float basePx, ComPtr<IDWriteTextFormat>& outFmt) -> bool {
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                uiFontFamily, nullptr,
                DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, (std::max)(12.0f, basePx) * scale, L"zh-CN",
                outFmt.GetAddressOf())) || !outFmt) {
            return false;
        }
        outFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        outFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        outFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    };

    ComPtr<IDWriteTextFormat> numLargeFmt, numMediumFmt, numSmallFmt;
    if (!makeNumberBadgeFormat(15.0f, numLargeFmt) ||
        !makeNumberBadgeFormat(13.5f, numMediumFmt) ||
        !makeNumberBadgeFormat(12.0f, numSmallFmt)) {
        return false;
    }

    m_infoTextFormat = std::move(infoFormat);
    m_hudPrimaryFormat = std::move(hudPrimaryFormat);
    m_hudBadgeFormat = std::move(hudBadgeFormat);
    m_textInputFormat = std::move(textInputFormat);
    m_numberBadgeLargeFormat = std::move(numLargeFmt);
    m_numberBadgeMediumFormat = std::move(numMediumFmt);
    m_numberBadgeSmallFormat = std::move(numSmallFmt);
    m_textScale = scale;
    return true;
}

bool CaptureRenderer::updateDpiScale(float scale) {
    scale = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 5.0f);
    if (m_infoTextFormat && m_hudPrimaryFormat && m_hudBadgeFormat && m_textInputFormat &&
        m_numberBadgeLargeFormat && m_numberBadgeMediumFormat && m_numberBadgeSmallFormat &&
        std::abs(scale - m_textScale) < 0.01f) {
        return true;
    }
    return createTextFormatsForScale(scale);
}

bool CaptureRenderer::rebuildBitmapTiles(const cv::Mat& bgra,
                                         D2D1_ALPHA_MODE alphaMode,
                                         std::vector<BitmapTile>& tiles,
                                         int& outW,
                                         int& outH) {
    tiles.clear();
    outW = 0;
    outH = 0;
    if (!m_renderTarget || bgra.empty() || bgra.channels() != 4) {
        return false;
    }

    const UINT32 hwMaxSize = m_renderTarget->GetMaximumBitmapSize();
    const int maxTileDim = static_cast<int>((std::max)(256u, (std::min)(hwMaxSize > 0 ? hwMaxSize : 4096u, 4096u)));
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));

    for (int y = 0; y < bgra.rows; y += maxTileDim) {
        const int sliceH = (std::min)(maxTileDim, bgra.rows - y);
        for (int x = 0; x < bgra.cols; x += maxTileDim) {
            const int sliceW = (std::min)(maxTileDim, bgra.cols - x);
            const cv::Mat slice = bgra(cv::Rect(x, y, sliceW, sliceH));
            BitmapTile tile;
            tile.xOffset = x;
            tile.yOffset = y;
            tile.width = sliceW;
            tile.height = sliceH;
            const HRESULT hr = m_renderTarget->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(sliceW), static_cast<UINT32>(sliceH)),
                slice.data,
                static_cast<UINT32>(slice.step[0]),
                props,
                tile.bitmap.GetAddressOf());
            if (FAILED(hr) || !tile.bitmap) {
                tiles.clear();
                return false;
            }
            tiles.push_back(std::move(tile));
        }
    }

    outW = bgra.cols;
    outH = bgra.rows;
    return !tiles.empty();
}

void CaptureRenderer::drawTiledRegion(const std::vector<BitmapTile>& tiles,
                                      int totalW,
                                      int totalH,
                                      const D2D1_RECT_F& dstRect,
                                      const D2D1_RECT_F& srcRect,
                                      D2D1_BITMAP_INTERPOLATION_MODE interp,
                                      bool highQualityCubic) {
    if (!m_renderTarget || tiles.empty() || totalW <= 0 || totalH <= 0) return;
    const float srcW = srcRect.right - srcRect.left;
    const float srcH = srcRect.bottom - srcRect.top;
    const float dstW = dstRect.right - dstRect.left;
    const float dstH = dstRect.bottom - dstRect.top;
    if (srcW <= 0.001f || srcH <= 0.001f || dstW <= 0.001f || dstH <= 0.001f) return;

    const float scaleX = dstW / srcW;
    const float scaleY = dstH / srcH;
    const float clampedSx0 = std::clamp(srcRect.left, 0.0f, static_cast<float>(totalW));
    const float clampedSx1 = std::clamp(srcRect.right, 0.0f, static_cast<float>(totalW));
    const float clampedSy0 = std::clamp(srcRect.top, 0.0f, static_cast<float>(totalH));
    const float clampedSy1 = std::clamp(srcRect.bottom, 0.0f, static_cast<float>(totalH));
    if (clampedSx1 <= clampedSx0 || clampedSy1 <= clampedSy0) return;

    ComPtr<ID2D1DeviceContext> dc;
    if (highQualityCubic) {
        m_renderTarget.As(&dc);
    }

    for (const auto& tile : tiles) {
        if (!tile.bitmap || tile.width <= 0 || tile.height <= 0) continue;
        const float tx0 = static_cast<float>(tile.xOffset);
        const float tx1 = static_cast<float>(tile.xOffset + tile.width);
        const float ty0 = static_cast<float>(tile.yOffset);
        const float ty1 = static_cast<float>(tile.yOffset + tile.height);
        const float ix0 = (std::max)(clampedSx0, tx0);
        const float ix1 = (std::min)(clampedSx1, tx1);
        const float iy0 = (std::max)(clampedSy0, ty0);
        const float iy1 = (std::min)(clampedSy1, ty1);
        if (ix1 <= ix0 || iy1 <= iy0) continue;

        const D2D1_RECT_F tileSrc = D2D1::RectF(ix0 - tx0, iy0 - ty0, ix1 - tx0, iy1 - ty0);
        const D2D1_RECT_F tileDst = D2D1::RectF(
            dstRect.left + (ix0 - srcRect.left) * scaleX,
            dstRect.top + (iy0 - srcRect.top) * scaleY,
            dstRect.left + (ix1 - srcRect.left) * scaleX,
            dstRect.top + (iy1 - srcRect.top) * scaleY);

        if (dc) {
            dc->DrawBitmap(tile.bitmap.Get(), &tileDst, 1.0f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &tileSrc, nullptr);
        } else {
            m_renderTarget->DrawBitmap(tile.bitmap.Get(), tileDst, 1.0f, interp, tileSrc);
        }
    }
}

bool CaptureRenderer::updateScreenBitmap(const cv::Mat& image) {
    m_screenBitmap.Reset();
    m_screenTiles.clear();
    m_screenTilesW = 0;
    m_screenTilesH = 0;
    m_minimapBitmap.Reset();
    m_minimapSrcW = 0;
    m_minimapSrcH = 0;
    if (!m_renderTarget || image.empty()) {
        LOG_ERROR("截图底图上传失败: 渲染目标或图像为空");
        return false;
    }

    cv::Mat bgra;
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    if (image.channels() == 4) {
        bgra = image;
        alphaMode = D2D1_ALPHA_MODE_IGNORE;
    } else if (image.channels() == 3) {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    } else if (image.channels() == 1) {
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    } else {
        LOG_ERROR("截图底图上传失败: 不支持的通道数={}", image.channels());
        return false;
    }

    if (!bgra.isContinuous()) bgra = bgra.clone();

    // 单纹理快路径优先：屏幕宽高 <= safeMaxBmp（如双 2K/4K <= 16384）时直接单纹理创建 m_screenBitmap，
    // 跳过 rebuildBitmapTiles，彻底消灭首帧双倍上传与常驻冗余 GPU 瓦片纹理；仅超限巨幕或失败时才启用瓦片流。
    const UINT32 hwMaxSize = m_renderTarget->GetMaximumBitmapSize();
    const UINT32 safeMaxBmp = (hwMaxSize > 0) ? (std::min)(hwMaxSize, 16384u) : 4096u;
    if (static_cast<UINT32>(bgra.cols) <= safeMaxBmp &&
        static_cast<UINT32>(bgra.rows) <= safeMaxBmp) {
        const auto props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));
        const HRESULT hr = m_renderTarget->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
            bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_screenBitmap.GetAddressOf());
        if (SUCCEEDED(hr) && m_screenBitmap) {
            m_screenTiles.clear();
            m_screenTilesW = bgra.cols;
            m_screenTilesH = bgra.rows;
            invalidate();
            return true;
        }
        m_screenBitmap.Reset();
    }

    if (!rebuildBitmapTiles(bgra, alphaMode, m_screenTiles, m_screenTilesW, m_screenTilesH)) {
        LOG_ERROR("截图底图上传 Direct2D 失败, hr=0x{:08X}", static_cast<unsigned>(E_FAIL));
        return false;
    }

    invalidate();
    return true;
}

void CaptureRenderer::drawDimOverlay(const D2D1_RECT_F& selRect, CaptureState& state) {
    auto size = m_renderTarget->GetSize();
    const float scale = (state.dpiScale > 0.0f ? state.dpiScale : 1.0f);
    
    // 如果启用了美化外壳且留白大于0，遮罩打洞区域需扩展至外壳边界
    D2D1_RECT_F holeRect = selRect;
    float holeRadius = state.cornerRadius * scale;
    if (state.beautyShell.enabled && state.beautyShell.padding > 0 && state.mode != OverlayMode::RecordRegion) {
        float pad = state.beautyShell.padding * scale;
        holeRect = D2D1::RectF(selRect.left - pad, selRect.top - pad, selRect.right + pad, selRect.bottom + pad);
        holeRadius = (std::max)(10.0f * scale, (state.beautyShell.cornerRadius + 6.0f) * scale);
    } else if (state.beautyShell.enabled) {
        holeRadius = state.beautyShell.cornerRadius * scale;
    }

    if (holeRadius > 0.5f && m_d2dFactory) {
        // 圆角遮罩打洞：排除选区圆角矩形
        auto rounded = D2D1::RoundedRect(holeRect, holeRadius, holeRadius);
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(rounded, geo.GetAddressOf())) && geo) {
            ComPtr<ID2D1RectangleGeometry> fullGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRectangleGeometry(D2D1::RectF(0, 0, size.width, size.height), fullGeo.GetAddressOf())) && fullGeo) {
                ComPtr<ID2D1PathGeometry> path;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(path.GetAddressOf())) && path) {
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(path->Open(sink.GetAddressOf())) && sink) {
                        fullGeo->CombineWithGeometry(geo.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
                        sink->Close();
                        m_renderTarget->FillGeometry(path.Get(), m_dimBrush.Get());
                        return;
                    }
                }
            }
        }
    }

    // 上
    m_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, holeRect.top), m_dimBrush.Get());
    // 下
    m_renderTarget->FillRectangle(D2D1::RectF(0, holeRect.bottom, size.width, size.height), m_dimBrush.Get());
    // 左
    m_renderTarget->FillRectangle(D2D1::RectF(0, holeRect.top, holeRect.left, holeRect.bottom), m_dimBrush.Get());
    // 右
    m_renderTarget->FillRectangle(D2D1::RectF(holeRect.right, holeRect.top, size.width, holeRect.bottom), m_dimBrush.Get());
}

void CaptureRenderer::drawSelection(const D2D1_RECT_F& rect, CaptureState& state) {
    const float scale = std::clamp(
        state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    float effectiveRadius = state.effectiveCornerRadius();
    float radius = effectiveRadius * scale;

    ComPtr<ID2D1SolidColorBrush> slateShadowBrush, slateRingBrush, handleWhiteBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.34f),
        slateShadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 0.86f),
        slateRingBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        handleWhiteBrush.GetAddressOf());

    // 1. 选区边框：全天候无色偏双层白晶框（外衬细腻石板微影防纯白网页融边，内叠清脆白晶主线零色偏）
    if (!(state.beautyShell.enabled && state.beautyShell.padding > 0 && state.mode != OverlayMode::RecordRegion)) {
        if (radius > 0.5f) {
            auto rrect = D2D1::RoundedRect(rect, radius, radius);
            if (slateShadowBrush) {
                m_renderTarget->DrawRoundedRectangle(rrect, slateShadowBrush.Get(), 2.6f * scale);
            }
            m_renderTarget->DrawRoundedRectangle(rrect, m_borderBrush.Get(), 1.6f * scale);
        } else {
            if (slateShadowBrush) {
                m_renderTarget->DrawRectangle(rect, slateShadowBrush.Get(), 2.6f * scale);
            }
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 1.6f * scale);
        }
    }

    // 2. 控制点：直角选区展示八个锚点；开启圆角 (effectiveRadius > 2px) 时隐藏四角悬空圆点，仅展示紧贴四条直边的四个中点锚点
    float cx = (rect.left + rect.right) / 2;
    float cy = (rect.top + rect.bottom) / 2;

    D2D1_POINT_2F controls[] = {
        {rect.left, rect.top}, {cx, rect.top}, {rect.right, rect.top},
        {rect.left, cy}, {rect.right, cy},
        {rect.left, rect.bottom}, {cx, rect.bottom}, {rect.right, rect.bottom}
    };

    const bool hideCornerDots = (effectiveRadius > 2.0f * scale);
    const float dotRadius = 4.5f * scale;
    for (size_t idx = 0; idx < 8; ++idx) {
        if (hideCornerDots && (idx == 0 || idx == 2 || idx == 5 || idx == 7)) {
            continue;
        }
        const auto& pt = controls[idx];
        if (slateShadowBrush) {
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(pt.x, pt.y + 0.8f * scale), dotRadius + 0.8f * scale, dotRadius + 0.8f * scale),
                slateShadowBrush.Get());
        }
        auto ellipse = D2D1::Ellipse(pt, dotRadius, dotRadius);
        if (handleWhiteBrush) {
            m_renderTarget->FillEllipse(ellipse, handleWhiteBrush.Get());
        }
        if (slateRingBrush) {
            m_renderTarget->DrawEllipse(ellipse, slateRingBrush.Get(), 1.35f * scale);
        }
    }

    // 3. Figma / PixPin 级内侧圆角调节手柄（小选区内部彻底隐藏手柄，避免遮挡微型内容，改由外部工具栏调节）
    drawCornerRadiusHandleVisual(rect, effectiveRadius, scale, state.isAdjustingCornerRadius, state.cornerDragStartPos, state.currentCursor, 64.0f, state.cornerDragIndex);
}

void CaptureRenderer::drawCornerRadiusHandleVisual(
    const D2D1_RECT_F& rect,
    float cornerRadius,
    float scale,
    bool isDragging,
    POINT dragStartPos,
    POINT currentCursor,
    float minDimension,
    int cornerIndex)
{
    const float selW = rect.right - rect.left;
    const float selH = rect.bottom - rect.top;
    if (!CornerRadiusHelper::canShowHandles(selW, selH, scale, minDimension)) {
        return;
    }

    auto handles = CornerRadiusHelper::getCornerHandles(
        rect.left, rect.top, rect.right, rect.bottom,
        cornerRadius, scale, false);
    if (handles.empty()) return;

    // Figma 纯净隐形流：平时选区内部 100% 干净零圆点，仅在鼠标移近内角或拖拽时轻盈浮现当前单角
    const CornerProximityState proximity = CornerRadiusHelper::evaluateProximity(
        handles,
        static_cast<float>(currentCursor.x),
        static_cast<float>(currentCursor.y),
        scale,
        isDragging,
        cornerIndex,
        static_cast<float>(dragStartPos.x),
        static_cast<float>(dragStartPos.y));

    if (proximity.activeCornerIndex < 0 ||
        proximity.activeCornerIndex >= static_cast<int>(handles.size()) ||
        proximity.visibilityAlpha <= 0.01f) {
        return;
    }

    const float s = (std::max)(0.5f, scale);
    const float vis = std::clamp(proximity.visibilityAlpha, 0.0f, 1.0f);
    const bool activeFocus = proximity.isDirectHover || isDragging;
    const auto& cpt = handles[proximity.activeCornerIndex];

    const auto withAlpha = [](D2D1_COLOR_F c, float a) noexcept {
        c.a = std::clamp(c.a * a, 0.0f, 1.0f);
        return c;
    };

    ComPtr<ID2D1StrokeStyle> roundStroke;
    if (m_d2dFactory) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
    }

    // 1. 贴合选区真实圆角的 90 度丝滑曲率高光导弧 (Osculating Corner Curvature Arc)
    if (m_d2dFactory) {
        const bool hasRealRadius = (cornerRadius > 0.5f);
        const float inset = hasRealRadius ? 0.0f : (3.2f * s);
        const float maxArcR = (std::max)(4.0f * s, (std::min)(selW, selH) * 0.5f - inset);
        const float arcR = hasRealRadius
            ? std::clamp(cornerRadius * s, 4.0f * s, maxArcR)
            : (std::min)(9.5f * s, maxArcR);

        D2D1_POINT_2F arcStart{};
        D2D1_POINT_2F arcEnd{};
        switch (cpt.cornerIndex) {
            case 0: // LT: 左切点 -> 上切点 (顺时针)
                arcStart = D2D1::Point2F(rect.left + inset, rect.top + inset + arcR);
                arcEnd   = D2D1::Point2F(rect.left + inset + arcR, rect.top + inset);
                break;
            case 1: // RT: 上切点 -> 右切点 (顺时针)
                arcStart = D2D1::Point2F(rect.right - inset - arcR, rect.top + inset);
                arcEnd   = D2D1::Point2F(rect.right - inset, rect.top + inset + arcR);
                break;
            case 2: // RB: 右切点 -> 下切点 (顺时针)
                arcStart = D2D1::Point2F(rect.right - inset, rect.bottom - inset - arcR);
                arcEnd   = D2D1::Point2F(rect.right - inset - arcR, rect.bottom - inset);
                break;
            default: // LB: 下切点 -> 左切点 (顺时针)
                arcStart = D2D1::Point2F(rect.left + inset + arcR, rect.bottom - inset);
                arcEnd   = D2D1::Point2F(rect.left + inset, rect.bottom - inset - arcR);
                break;
        }

        ComPtr<ID2D1PathGeometry> arcGeo;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(arcStart, D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddArc(D2D1::ArcSegment(
                    arcEnd,
                    D2D1::SizeF(arcR, arcR),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
            }

            const float arcAlpha = activeFocus ? (hasRealRadius ? 0.96f : 0.72f) : (0.55f * vis);
            ComPtr<ID2D1SolidColorBrush> arcHaloBrush, arcIrisBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.52f * arcAlpha), arcHaloBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(
                withAlpha(CaptureHudPalette::kCornerGuideArrow, arcAlpha), arcIrisBrush.GetAddressOf());

            if (arcHaloBrush) {
                m_renderTarget->DrawGeometry(
                    arcGeo.Get(), arcHaloBrush.Get(), (activeFocus ? 3.4f : 2.8f) * s, roundStroke.Get());
            }
            if (arcIrisBrush) {
                m_renderTarget->DrawGeometry(
                    arcGeo.Get(), arcIrisBrush.Get(), (activeFocus ? 2.1f : 1.6f) * s, roundStroke.Get());
            }
        }
    }

    // 2. 温润珍珠微晶圆环锚点 (Pearl & Graphite Ring Handle — 零杂色偏，明暗底皆清透立体)
    const float ringR = (isDragging ? 5.6f : (proximity.isDirectHover ? 5.0f : 4.3f)) * s;
    const D2D1_POINT_2F centerPt = D2D1::Point2F(cpt.x, cpt.y);

    // 2a. 悬停/拖拽纯净白晶呼吸柔光晕
    if (activeFocus) {
        ComPtr<ID2D1SolidColorBrush> haloBrush;
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, isDragging ? 0.34f : 0.24f),
            haloBrush.GetAddressOf());
        if (haloBrush) {
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(centerPt, ringR + 3.2f * s, ringR + 3.2f * s), haloBrush.Get());
        }
    }

    // 2b. 极轻向下水滴悬浮微影 + 纯白珠体 + 深石墨精致内环
    ComPtr<ID2D1SolidColorBrush> shadowBrush, silverRimBrush, pearlWhiteBrush, irisRingBrush;
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.22f * vis), shadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(71.0f / 255.0f, 85.0f / 255.0f, 105.0f / 255.0f, 0.32f * vis), silverRimBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f * vis), pearlWhiteBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, (activeFocus ? 0.94f : 0.80f) * vis),
        irisRingBrush.GetAddressOf());

    if (shadowBrush) {
        m_renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cpt.x, cpt.y + 1.0f * s), ringR + 1.1f * s, ringR + 1.1f * s),
            shadowBrush.Get());
    }
    if (pearlWhiteBrush) {
        m_renderTarget->FillEllipse(D2D1::Ellipse(centerPt, ringR, ringR), pearlWhiteBrush.Get());
    }
    if (silverRimBrush) {
        m_renderTarget->DrawEllipse(
            D2D1::Ellipse(centerPt, ringR + 0.35f * s, ringR + 0.35f * s), silverRimBrush.Get(), 0.7f * s);
    }
    if (irisRingBrush) {
        if (isDragging) {
            // 拖拽态点亮深石墨实心微芯
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(centerPt, 2.3f * s, 2.3f * s), irisRingBrush.Get());
        } else {
            // 悬停/近场态呈现通透空心石墨内环
            const float innerRingR = (std::max)(1.6f * s, ringR - 1.65f * s);
            m_renderTarget->DrawEllipse(
                D2D1::Ellipse(centerPt, innerRingR, innerRingR), irisRingBrush.Get(), 1.35f * s);
        }
    }

    // 3. 温润雪瓷白药丸胶囊读数 [ R 12 ]（仅在直接悬停锚点或拖拽推拉中轻盈浮现，松手即隐）
    if (activeFocus && selW >= 120.0f * s && selH >= 120.0f * s && m_infoTextFormat && m_dwriteFactory) {
        const int radVal = static_cast<int>(std::round(cornerRadius));
        const std::wstring prefixStr = L"R ";
        const std::wstring numStr = std::format(L"{}", radVal);

        auto measureWidth = [&](const std::wstring& wstr) -> float {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    wstr.c_str(), static_cast<UINT32>(wstr.size()),
                    m_infoTextFormat.Get(), 240.0f * s, 40.0f * s, layout.GetAddressOf())) && layout) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(layout->GetMetrics(&m))) {
                    return (std::max)(m.width, m.widthIncludingTrailingWhitespace);
                }
            }
            return static_cast<float>(wstr.size()) * 7.5f * s;
        };

        const float prefixW = measureWidth(prefixStr);
        const float numW = measureWidth(numStr);
        const float padX = 8.5f * s;
        const float badgeW = std::ceil(prefixW + numW + padX * 2.0f);
        const float badgeH = 21.0f * s;
        const float pillRadius = badgeH * 0.5f;

        const float badgeHalfDiag = std::hypot(badgeW * 0.5f, badgeH * 0.5f);
        const float badgeDist = ringR + badgeHalfDiag + 8.5f * s;
        float bx = cpt.x + cpt.inDirX * badgeDist - badgeW * 0.5f;
        float by = cpt.y + cpt.inDirY * badgeDist - badgeH * 0.5f;

        const float minBx = rect.left + 6.0f * s;
        const float maxBx = rect.right - badgeW - 6.0f * s;
        const float minBy = rect.top + 6.0f * s;
        const float maxBy = rect.bottom - badgeH - 6.0f * s;
        if (minBx <= maxBx) bx = std::clamp(bx, minBx, maxBx);
        if (minBy <= maxBy) by = std::clamp(by, minBy, maxBy);

        const D2D1_RECT_F badgeRect = D2D1::RectF(bx, by, bx + badgeW, by + badgeH);
        const D2D1_ROUNDED_RECT badgePill = D2D1::RoundedRect(badgeRect, pillRadius, pillRadius);

        ComPtr<ID2D1SolidColorBrush> badgeShadow, badgeBg, badgeBorder, prefixBrush, numBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.16f), badgeShadow.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBg, badgeBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSizeHudBorder, badgeBorder.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kSecondaryText, prefixBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(CaptureHudPalette::kPrimaryText, numBrush.GetAddressOf());

        if (badgeShadow) {
            D2D1_RECT_F sRect = D2D1::RectF(badgeRect.left, badgeRect.top + 1.5f * s, badgeRect.right, badgeRect.bottom + 2.5f * s);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, pillRadius, pillRadius), badgeShadow.Get());
        }
        if (badgeBg) m_renderTarget->FillRoundedRectangle(badgePill, badgeBg.Get());
        if (badgeBorder) m_renderTarget->DrawRoundedRectangle(badgePill, badgeBorder.Get(), 0.95f * s);

        auto drawCenteredSegment = [&](const std::wstring& seg, float segLeft, float segW, ID2D1Brush* brush) {
            if (!brush) return;
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    seg.c_str(), static_cast<UINT32>(seg.size()),
                    m_infoTextFormat.Get(), segW + 8.0f * s, badgeH, layout.GetAddressOf())) && layout) {
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextLayout(D2D1::Point2F(segLeft, by), layout.Get(), brush);
            }
        };

        drawCenteredSegment(prefixStr, bx + padX, prefixW, prefixBrush.Get());
        drawCenteredSegment(numStr, bx + padX + prefixW, numW, numBrush.Get());
    }
}

void CaptureRenderer::drawSizeInfo(const D2D1_RECT_F& rect, CaptureState& state) {
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool zh = isCaptureUiChinese();
    const auto overlayState = state.state.load();
    const bool hasLockedSelection =
        ((int)overlayState == (int)OverlayState::Selected ||
         (int)overlayState == (int)OverlayState::Marking);

    int rawW = static_cast<int>(std::round(rect.right - rect.left));
    int rawH = static_cast<int>(std::round(rect.bottom - rect.top));
    int posX = hasLockedSelection
        ? static_cast<int>(std::round(rect.left))
        : static_cast<int>(state.currentCursor.x >= 0 ? state.currentCursor.x : rect.left);
    int posY = hasLockedSelection
        ? static_cast<int>(std::round(rect.top))
        : static_cast<int>(state.currentCursor.y >= 0 ? state.currentCursor.y : rect.top);

    // 根据 px / dp 单位换算，将主数字与单位后缀拆分为独立排印层级
    const bool isDpMode = (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel);
    int displayW = isDpMode
        ? static_cast<int>(std::round(rawW / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawW;
    int displayH = isDpMode
        ? static_cast<int>(std::round(rawH / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawH;
    const std::wstring unitTagStr = state.showUnitInHud ? (isDpMode ? L"dp" : L"px") : L"";

    const std::wstring posStr = state.showPositionInHud
        ? std::format(L"{}, {}", posX, posY) : L"";
    const std::wstring mainSizeStr = std::format(L"{} \u00D7 {}", displayW, displayH);

    std::vector<std::wstring> badges;
    if (state.aspectRatio != AspectRatioPreset::Free) {
        switch (state.aspectRatio) {
            case AspectRatioPreset::Ratio_16_9:   badges.push_back(L"16:9"); break;
            case AspectRatioPreset::Ratio_4_3:    badges.push_back(L"4:3"); break;
            case AspectRatioPreset::Ratio_1_1:    badges.push_back(L"1:1"); break;
            case AspectRatioPreset::Ratio_Golden: badges.push_back(L"1.618:1"); break;
            default: break;
        }
    }
    const float effectiveRadius = state.effectiveCornerRadius();
    if (effectiveRadius > 0.5f) {
        badges.push_back(std::format(L"R{}px", static_cast<int>(effectiveRadius)));
    }
    if (state.beautyShell.enabled) {
        badges.push_back(zh ? L"外壳" : L"Shell");
    }

    auto measureSpanWidth = [&](const std::wstring& txt, IDWriteTextFormat* fmt, float fallbackPerChar) -> float {
        if (txt.empty()) return 0.0f;
        float w = static_cast<float>(txt.size()) * fallbackPerChar * scale;
        if (m_dwriteFactory && fmt) {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    txt.c_str(), static_cast<UINT32>(txt.size()),
                    fmt, 2000.0f * scale, 60.0f * scale, layout.GetAddressOf())) && layout) {
                DWRITE_TEXT_METRICS tm{};
                if (SUCCEEDED(layout->GetMetrics(&tm)) && tm.width > 0.0f) {
                    w = tm.widthIncludingTrailingWhitespace > tm.width
                        ? tm.widthIncludingTrailingWhitespace : tm.width;
                }
            }
        }
        return std::ceil(w);
    };

    IDWriteTextFormat* primaryFmt = m_hudPrimaryFormat ? m_hudPrimaryFormat.Get() : m_infoTextFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : m_infoTextFormat.Get();

    const float padX = 13.0f * scale;
    const float iconSz = 14.0f * scale;
    const float iconGap = 8.0f * scale;
    const float dividerZone = 13.0f * scale;
    const float badgePadX = 7.0f * scale;
    const float badgeGap = 6.0f * scale;
    const float chevronZone = 18.0f * scale;

    const float posW = posStr.empty() ? 0.0f : measureSpanWidth(posStr, badgeFmt, 7.6f);
    const float mainW = measureSpanWidth(mainSizeStr, primaryFmt, 9.6f);
    const float unitGap = unitTagStr.empty() ? 0.0f : (5.0f * scale);
    const float rawUnitW = unitTagStr.empty() ? 0.0f : measureSpanWidth(unitTagStr, badgeFmt, 7.4f);
    const float unitW = unitTagStr.empty()
        ? 0.0f
        : (isDpMode ? (rawUnitW + 10.0f * scale) : (rawUnitW + 2.0f * scale));

    std::vector<float> badgeWidths;
    badgeWidths.reserve(badges.size());
    float totalBadgesW = 0.0f;
    for (const auto& b : badges) {
        float bw = measureSpanWidth(b, badgeFmt, 8.0f) + badgePadX * 2.0f;
        badgeWidths.push_back(bw);
        totalBadgesW += badgeGap + bw;
    }

    const float labelW = padX + iconSz + iconGap +
                         (posW > 0.0f ? (posW + dividerZone) : 0.0f) +
                         mainW + unitGap + unitW + totalBadgesW + chevronZone + padX;
    const float labelH = 36.0f * scale;

    const auto surfaceSize = m_renderTarget->GetSize();

    // 仅在选区已锁定（Selected/Marking）时构建工具栏几何布局并提供工具栏障碍物矩形；
    // 在初始未选区（Selecting）悬停探测窗口预览阶段不构建工具栏，也不驱赶左下角快捷键 HUD。
    if (hasLockedSelection) {
        rebuildCaptureToolbar(state, rect, surfaceSize);
    }

    std::vector<HudObstacle> obstacles;
    if (hasLockedSelection) {
        if (state.primaryToolbarRect.right > state.primaryToolbarRect.left &&
            state.primaryToolbarRect.bottom > state.primaryToolbarRect.top) {
            obstacles.push_back({state.primaryToolbarRect, HudObstacleType::PrimaryToolbar, 2.5f});
        }
        if (!state.secondaryToolbarButtons.empty() &&
            state.secondaryToolbarRect.right > state.secondaryToolbarRect.left &&
            state.secondaryToolbarRect.bottom > state.secondaryToolbarRect.top) {
            obstacles.push_back({state.secondaryToolbarRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
        if (!state.selectionSideButtons.empty() &&
            state.selectionSideRect.right > state.selectionSideRect.left &&
            state.selectionSideRect.bottom > state.selectionSideRect.top) {
            obstacles.push_back({state.selectionSideRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
        if (state.openSubmenu != SubmenuType::None &&
            state.openSubmenuRect.right > state.openSubmenuRect.left &&
            state.openSubmenuRect.bottom > state.openSubmenuRect.top) {
            obstacles.push_back({state.openSubmenuRect, HudObstacleType::SecondaryToolbar, 2.2f});
        }
        if (state.sliderPopup.type != SliderPopupType::None &&
            state.sliderPopup.popupRect.right > state.sliderPopup.popupRect.left &&
            state.sliderPopup.popupRect.bottom > state.sliderPopup.popupRect.top) {
            obstacles.push_back({state.sliderPopup.popupRect, HudObstacleType::SecondaryToolbar, 2.2f});
        }
        if (state.dropdownMenu.type != DropdownType::None && !state.dropdownMenu.items.empty()) {
            D2D1_RECT_F dmRect = state.dropdownMenu.items.front().rect;
            for (const auto& item : state.dropdownMenu.items) {
                dmRect.left = (std::min)(dmRect.left, item.rect.left);
                dmRect.top = (std::min)(dmRect.top, item.rect.top);
                dmRect.right = (std::max)(dmRect.right, item.rect.right);
                dmRect.bottom = (std::max)(dmRect.bottom, item.rect.bottom);
            }
            obstacles.push_back({dmRect, HudObstacleType::SecondaryToolbar, 2.2f});
            if (state.dropdownMenu.childMenuRect.right > state.dropdownMenu.childMenuRect.left &&
                state.dropdownMenu.childMenuRect.bottom > state.dropdownMenu.childMenuRect.top) {
                obstacles.push_back({state.dropdownMenu.childMenuRect, HudObstacleType::SecondaryToolbar, 2.2f});
            }
        }
        if (!state.detectedQrText.empty() &&
            state.qrChipRect.right > state.qrChipRect.left &&
            state.qrChipRect.bottom > state.qrChipRect.top) {
            obstacles.push_back({state.qrChipRect, HudObstacleType::SecondaryToolbar, 2.0f});
        }
    }
    if (ShortcutHintOverlay::instance().isVisible()) {
        RECT hintRect = ShortcutHintOverlay::instance().getBounds();
        if (hintRect.right > hintRect.left && hintRect.bottom > hintRect.top) {
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            RECT winRc{};
            if (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &winRc)) {
                vx = winRc.left;
                vy = winRc.top;
            }
            obstacles.push_back({
                D2D1::RectF(static_cast<float>(hintRect.left - vx), static_cast<float>(hintRect.top - vy),
                            static_cast<float>(hintRect.right - vx), static_cast<float>(hintRect.bottom - vy)),
                HudObstacleType::ActiveAnnotation,
                2.0f
            });
        }
    }

    // 1. 注册 8 个调节手柄热区为障碍物，杜绝尺寸胶囊遮挡把手与圆角指示点
    const float handleBoxR = 12.0f * scale;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const D2D1_POINT_2F handlePoints[8] = {
        {rect.left, rect.top}, {cx, rect.top}, {rect.right, rect.top},
        {rect.left, cy},                       {rect.right, cy},
        {rect.left, rect.bottom}, {cx, rect.bottom}, {rect.right, rect.bottom}
    };
    for (const auto& hp : handlePoints) {
        obstacles.push_back({
            D2D1::RectF(hp.x - handleBoxR, hp.y - handleBoxR, hp.x + handleBoxR, hp.y + handleBoxR),
            HudObstacleType::SelectionHandle,
            1.5f
        });
    }

    // 2. 注册所有截图标注框（文本框、矩形、箭头、序列号等）为障碍物，彻底杜绝挡住标注
    for (const auto& elem : state.markup.elements()) {
        if (!elem) continue;
        cv::Rect bbox = elem->getBoundingBox();
        if (bbox.width <= 0 || bbox.height <= 0) continue;
        float bLeft = rect.left + static_cast<float>(bbox.x);
        float bTop = rect.top + static_cast<float>(bbox.y);
        float bRight = bLeft + static_cast<float>(bbox.width);
        float bBottom = bTop + static_cast<float>(bbox.height);
        obstacles.push_back({D2D1::RectF(bLeft, bTop, bRight, bBottom), HudObstacleType::ActiveAnnotation, 2.0f});
    }

    // 3. 正在绘制图元（isMarking）时的动态拉框预览避让
    if (state.isMarking) {
        float mx1 = (std::min)(static_cast<float>(state.markupStart.x), static_cast<float>(state.markupEnd.x));
        float my1 = (std::min)(static_cast<float>(state.markupStart.y), static_cast<float>(state.markupEnd.y));
        float mx2 = (std::max)(static_cast<float>(state.markupStart.x), static_cast<float>(state.markupEnd.x));
        float my2 = (std::max)(static_cast<float>(state.markupStart.y), static_cast<float>(state.markupEnd.y));
        const float pad = 8.0f * scale;
        obstacles.push_back({D2D1::RectF(mx1 - pad, my1 - pad, mx2 + pad, my2 + pad), HudObstacleType::ActiveAnnotation, 2.2f});
    }

    D2D1_RECT_F curMonRect{}, curWorkRect{};
    resolveMonitorBoundsInCanvas(
        static_cast<float>(state.currentCursor.x),
        static_cast<float>(state.currentCursor.y),
        surfaceSize, curMonRect, curWorkRect);
    const D2D1_SIZE_F localWorkSize = D2D1::SizeF(
        (std::max)(64.0f, curWorkRect.right - curWorkRect.left),
        (std::max)(64.0f, curWorkRect.bottom - curWorkRect.top));

    // 3.5. 预选/拖拽阶段像素取色放大镜（Loupe）与动态放大镜（Magnifier）几何避让
    if (!hasLockedSelection || state.dragging) {
        const D2D1_RECT_F localLoupe = computeSelectionLoupeRect(
            static_cast<float>(state.currentCursor.x) - curWorkRect.left,
            static_cast<float>(state.currentCursor.y) - curWorkRect.top,
            localWorkSize, scale);
        state.loupeHudRect = D2D1::RectF(
            localLoupe.left + curWorkRect.left,
            localLoupe.top + curWorkRect.top,
            localLoupe.right + curWorkRect.left,
            localLoupe.bottom + curWorkRect.top);
        obstacles.push_back({state.loupeHudRect, HudObstacleType::Cursor, 4.0f});
    } else if (state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
        const float magR = (static_cast<float>(state.dynamicMagnifierRadius) + 12.0f) * scale;
        const float curFx = static_cast<float>(state.currentCursor.x);
        const float curFy = static_cast<float>(state.currentCursor.y);
        obstacles.push_back({
            D2D1::RectF(curFx - magR, curFy - magR, curFx + magR, curFy + magR),
            HudObstacleType::Cursor,
            3.5f
        });
    }

    // 4. 光标避让策略：
    // 展开尺寸菜单时用户正在点击菜单选项，不主动躲避光标；
    // 处于预选探测(!hasLockedSelection)、拖拽、调整选区、手柄操作、图元绘制或光标进入选区内部/靠近把手时，强力避让光标
    const bool isInteracting = state.dragging || state.isMarking || state.isManipulating ||
                               state.isAdjustingSelection || state.isAdjustingCornerRadius;
    const float handleSafeR = 26.0f * scale;
    bool nearAnyHandle = false;
    for (const auto& hp : handlePoints) {
        if (std::hypot(static_cast<float>(state.currentCursor.x) - hp.x,
                       static_cast<float>(state.currentCursor.y) - hp.y) <= handleSafeR) {
            nearAnyHandle = true;
            break;
        }
    }
    const bool cursorInsideSelection = (state.currentCursor.x >= rect.left && state.currentCursor.x <= rect.right &&
                                        state.currentCursor.y >= rect.top && state.currentCursor.y <= rect.bottom);

    if (!state.isSizeMenuOpen) {
        float curSafeR = 28.0f * scale;
        D2D1_RECT_F cursorObstacle = D2D1::RectF(
            static_cast<float>(state.currentCursor.x) - curSafeR,
            static_cast<float>(state.currentCursor.y) - curSafeR,
            static_cast<float>(state.currentCursor.x) + curSafeR,
            static_cast<float>(state.currentCursor.y) + curSafeR);
        if (!hasLockedSelection || isInteracting || nearAnyHandle || cursorInsideSelection) {
            obstacles.push_back({cursorObstacle, HudObstacleType::Cursor, 3.0f});
        }
    }

    HudPlacementConfig hudCfg;
    hudCfg.edgeGap = 16.0f;
    hudCfg.screenMargin = 8.0f;
    hudCfg.cursorSafeRadius = 28.0f;
    hudCfg.stabilityBonus = 45.0f;
    hudCfg.allowInsideCandidates = true;
    hudCfg.avoidCursor = true;

    const D2D1_RECT_F localAnchorRect = D2D1::RectF(
        rect.left - curWorkRect.left,
        rect.top - curWorkRect.top,
        rect.right - curWorkRect.left,
        rect.bottom - curWorkRect.top);
    std::vector<HudObstacle> localObstacles;
    localObstacles.reserve(obstacles.size());
    for (const auto& obs : obstacles) {
        localObstacles.push_back({
            D2D1::RectF(obs.rect.left - curWorkRect.left,
                        obs.rect.top - curWorkRect.top,
                        obs.rect.right - curWorkRect.left,
                        obs.rect.bottom - curWorkRect.top),
            obs.type,
            obs.weight
        });
    }

    auto placement = HudAvoidanceEngine::solvePlacement(
        localAnchorRect, labelW, labelH, localWorkSize, localObstacles, hudCfg, scale, state.lastSizeHudEdge);

    state.lastSizeHudEdge = static_cast<int>(placement.edge);
    auto pillRect = D2D1::RectF(
        placement.rect.left + curWorkRect.left,
        placement.rect.top + curWorkRect.top,
        placement.rect.right + curWorkRect.left,
        placement.rect.bottom + curWorkRect.top);
    state.sizeHudRect = pillRect;

    float labelX = pillRect.left;
    float labelY = pillRect.top;

    // 判定鼠标悬停状态
    bool isHovered = (state.currentCursor.x >= pillRect.left && state.currentCursor.x <= pillRect.right &&
                      state.currentCursor.y >= pillRect.top && state.currentCursor.y <= pillRect.bottom);
    state.isSizeHudHovered = isHovered;

    const float pillRadius = 10.0f * scale;
    auto pillRounded = D2D1::RoundedRect(pillRect, pillRadius, pillRadius);

    // 1. 统一复用动态明暗主题微晶浮岛底板（雪瓷白 / 黑曜石微晶玻璃 + 动态强调色）
    const auto pal = FloatingGlassBar::resolveHudPalette();
    const bool activeState = isHovered || state.isSizeMenuOpen;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), pillRect, pillRadius, pal.isDark);

    ComPtr<ID2D1SolidColorBrush> pillBorder, pillText, posTextBrush, accentBrush, dividerBrush, subPillBg, subPillBorder;
    m_renderTarget->CreateSolidColorBrush(
        activeState ? pal.accentBorder : pal.panelBorder,
        pillBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, pillText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.secondaryText, posTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentText, accentBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.divider, dividerBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        activeState ? pal.hoverBg : pal.accentSoftBg,
        subPillBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.accentBorder,
        subPillBorder.GetAddressOf());

    if (activeState && pillBorder) {
        m_renderTarget->DrawRoundedRectangle(pillRounded, pillBorder.Get(), 1.0f * scale);
    }

    // 3. 多段式结构化仪表渲染
    float cursorX = labelX + padX;
    const float midY = labelY + labelH * 0.5f;

    // 3.1 左侧四角取景框矢量图标
    if (accentBrush) {
        const float ix1 = cursorX + 1.0f * scale;
        const float iy1 = midY - iconSz * 0.5f + 1.0f * scale;
        const float ix2 = cursorX + iconSz - 1.0f * scale;
        const float iy2 = midY + iconSz * 0.5f - 1.0f * scale;
        const float arm = 4.2f * scale;
        const float sw = 1.6f * scale;
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy1 + arm), D2D1::Point2F(ix1, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy1), D2D1::Point2F(ix1 + arm, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2 - arm, iy1), D2D1::Point2F(ix2, iy1), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2, iy1), D2D1::Point2F(ix2, iy1 + arm), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy2 - arm), D2D1::Point2F(ix1, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix1, iy2), D2D1::Point2F(ix1 + arm, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2 - arm, iy2), D2D1::Point2F(ix2, iy2), accentBrush.Get(), sw);
        m_renderTarget->DrawLine(D2D1::Point2F(ix2, iy2), D2D1::Point2F(ix2, iy2 - arm), accentBrush.Get(), sw);
    }
    cursorX += iconSz + iconGap;

    // 3.2 可选光标/选区坐标段 + 竖向微晶刻痕
    if (!posStr.empty() && badgeFmt && posTextBrush) {
        TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_renderTarget->DrawTextW(
            posStr.c_str(), static_cast<UINT32>(posStr.size()), badgeFmt,
            D2D1::RectF(cursorX, labelY, cursorX + posW + 4.0f * scale, labelY + labelH),
            posTextBrush.Get());
        cursorX += posW;
        if (dividerBrush) {
            const float divX = cursorX + dividerZone * 0.5f;
            m_renderTarget->DrawLine(
                D2D1::Point2F(divX, labelY + 9.5f * scale),
                D2D1::Point2F(divX, labelY + labelH - 9.5f * scale),
                dividerBrush.Get(), 1.0f * scale);
        }
        cursorX += dividerZone;
    }

    // 3.3 主尺寸数字段 (W × H) + 独立排印的单位后缀 (px / dp)
    if (primaryFmt && pillText) {
        TextFormatScope scope(primaryFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(
            mainSizeStr.c_str(), static_cast<UINT32>(mainSizeStr.size()), primaryFmt,
            D2D1::RectF(cursorX, labelY, cursorX + mainW + 6.0f * scale, labelY + labelH),
            pillText.Get());
        cursorX += mainW;
    }

    if (!unitTagStr.empty() && badgeFmt) {
        cursorX += unitGap;
        if (isDpMode) {
            // dp 逻辑像素换算态：以精致温润晨雾紫晶微晶胶囊凸显，一眼区分物理与逻辑尺寸
            const float unitChipH = 20.0f * scale;
            const float unitTop = midY - unitChipH * 0.5f;
            D2D1_RECT_F uRect = D2D1::RectF(cursorX, unitTop, cursorX + unitW, unitTop + unitChipH);
            auto uRound = D2D1::RoundedRect(uRect, 5.0f * scale, 5.0f * scale);
            if (subPillBg) m_renderTarget->FillRoundedRectangle(uRound, subPillBg.Get());
            if (subPillBorder) m_renderTarget->DrawRoundedRectangle(uRound, subPillBorder.Get(), 0.9f * scale);
            if (accentBrush) {
                TextFormatScope uScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawTextW(unitTagStr.c_str(), static_cast<UINT32>(unitTagStr.size()), badgeFmt, uRect, accentBrush.Get());
            }
        } else if (posTextBrush) {
            // px 物理像素默认态：采用暖月银次级字号并自然微沉 0.8px 对齐大数字视觉基线，清爽不抢戏
            TextFormatScope uScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            m_renderTarget->DrawTextW(
                unitTagStr.c_str(), static_cast<UINT32>(unitTagStr.size()), badgeFmt,
                D2D1::RectF(cursorX, labelY + 0.8f * scale, cursorX + unitW + 4.0f * scale, labelY + labelH),
                posTextBrush.Get());
        }
        cursorX += unitW;
    }

    // 3.4 内嵌圆角微晶状态徽章 (比例锁定 / 圆角半径 / 美化外壳)
    if (badgeFmt && accentBrush) {
        const float subH = 22.0f * scale;
        const float subTop = midY - subH * 0.5f;
        const float subRadius = 5.5f * scale;
        TextFormatScope scope(badgeFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        for (size_t i = 0; i < badges.size(); ++i) {
            cursorX += badgeGap;
            const float bw = badgeWidths[i];
            D2D1_RECT_F bRect = D2D1::RectF(cursorX, subTop, cursorX + bw, subTop + subH);
            auto bRounded = D2D1::RoundedRect(bRect, subRadius, subRadius);
            if (subPillBg) m_renderTarget->FillRoundedRectangle(bRounded, subPillBg.Get());
            if (subPillBorder) m_renderTarget->DrawRoundedRectangle(bRounded, subPillBorder.Get(), 1.0f * scale);
            m_renderTarget->DrawTextW(
                badges[i].c_str(), static_cast<UINT32>(badges[i].size()), badgeFmt,
                bRect, accentBrush.Get());
            cursorX += bw;
        }
    }

    // 3.5 右侧下拉菜单指示微箭头（悬停或展开时点亮晨雾紫晶微光）
    ID2D1Brush* chevBrush = activeState && accentBrush ? accentBrush.Get() : posTextBrush.Get();
    if (chevBrush) {
        const float chevCx = pillRect.right - padX - 3.5f * scale;
        const float chevHalfW = 3.4f * scale;
        const float chevHalfH = 2.0f * scale;
        const float dir = state.isSizeMenuOpen ? -1.0f : 1.0f;
        m_renderTarget->DrawLine(
            D2D1::Point2F(chevCx - chevHalfW, midY - chevHalfH * dir),
            D2D1::Point2F(chevCx, midY + chevHalfH * dir),
            chevBrush, 1.5f * scale);
        m_renderTarget->DrawLine(
            D2D1::Point2F(chevCx, midY + chevHalfH * dir),
            D2D1::Point2F(chevCx + chevHalfW, midY - chevHalfH * dir),
            chevBrush, 1.5f * scale);
    }
}

void CaptureRenderer::drawSizeMenu(const D2D1_RECT_F& hudRect, CaptureState& state) {
    if (!state.isSizeMenuOpen || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const auto lay = computeSizeMenuLayout(hudRect, m_renderTarget->GetSize(), scale);
    const auto& menuRect = lay.menuRect;
    state.sizeMenuRect = menuRect;

    const float menuRadius = 11.0f * scale;
    const auto pal = FloatingGlassBar::resolveThemePalette();
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), menuRect, menuRadius, pal.isDark);

    ComPtr<ID2D1SolidColorBrush> dividerBrush;
    m_renderTarget->CreateSolidColorBrush(pal.divider, dividerBrush.GetAddressOf());

    ComPtr<ID2D1SolidColorBrush> segTrackBg, segTrackBorder, segActiveShadow, segActiveBg, segActiveBorder;
    ComPtr<ID2D1SolidColorBrush> primaryTextBrush, secondaryTextBrush, formulaTextBrush;
    ComPtr<ID2D1SolidColorBrush> rowHoverBg, switchOnBg, switchOffBg, knobShadowBrush, knobBrush;
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)
                   : D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 0.98f),
        segTrackBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.panelBorder, segTrackBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f)
                   : D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.07f),
        segActiveShadow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(0.18f, 0.22f, 0.30f, 0.96f)
                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        segActiveBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accentBorder, segActiveBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.primaryText, primaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.secondaryText, secondaryTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.mutedText, formulaTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.hoverBg, rowHoverBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(pal.accent, switchOnBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f)
                   : D2D1::ColorF(203.0f / 255.0f, 213.0f / 255.0f, 225.0f / 255.0f, 0.95f),
        switchOffBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, pal.isDark ? 0.32f : 0.14f),
        knobShadowBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), knobBrush.GetAddressOf());

    const float curX = static_cast<float>(state.currentCursor.x);
    const float curY = static_cast<float>(state.currentCursor.y);
    const auto isCursorIn = [&](const D2D1_RECT_F& r) {
        return curX >= r.left && curX <= r.right && curY >= r.top && curY <= r.bottom;
    };

    // 2. 第 1 区：Apple Studio 级纯白立体浮起双联分段滑块 (px | dp Segmented Control)
    const auto trackRound = D2D1::RoundedRect(lay.segmentTrackRect, 7.5f * scale, 7.5f * scale);
    if (segTrackBg) m_renderTarget->FillRoundedRectangle(trackRound, segTrackBg.Get());
    if (segTrackBorder) m_renderTarget->DrawRoundedRectangle(trackRound, segTrackBorder.Get(), 0.9f * scale);

    const bool isPx = (state.sizeUnit == CaptureState::SizeUnit::Pixel);
    const D2D1_RECT_F& activeSeg = isPx ? lay.pxSegmentRect : lay.dpSegmentRect;
    const D2D1_RECT_F& inactiveSeg = isPx ? lay.dpSegmentRect : lay.pxSegmentRect;
    if (isCursorIn(inactiveSeg) && rowHoverBg) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(inactiveSeg, 5.5f * scale, 5.5f * scale), rowHoverBg.Get());
    }
    if (segActiveShadow) {
        const D2D1_RECT_F sRect = D2D1::RectF(activeSeg.left, activeSeg.top + 1.0f * scale, activeSeg.right, activeSeg.bottom + 1.5f * scale);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRect, 5.5f * scale, 5.5f * scale), segActiveShadow.Get());
    }
    const auto activeSegRound = D2D1::RoundedRect(activeSeg, 5.5f * scale, 5.5f * scale);
    if (segActiveBg) m_renderTarget->FillRoundedRectangle(activeSegRound, segActiveBg.Get());
    if (segActiveBorder) m_renderTarget->DrawRoundedRectangle(activeSegRound, segActiveBorder.Get(), 0.9f * scale);

    IDWriteTextFormat* ctrlFmt = m_infoTextFormat ? m_infoTextFormat.Get() : m_hudBadgeFormat.Get();
    IDWriteTextFormat* badgeFmt = m_hudBadgeFormat ? m_hudBadgeFormat.Get() : ctrlFmt;
    if (ctrlFmt && primaryTextBrush && secondaryTextBrush) {
        TextFormatScope segScope(ctrlFmt, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        const std::wstring pxLabel = state.toolbarLayoutChinese ? L"px 物理" : L"px";
        const std::wstring dpLabel = state.toolbarLayoutChinese ? L"dp 逻辑" : L"dp";
        m_renderTarget->DrawTextW(pxLabel.c_str(), static_cast<UINT32>(pxLabel.size()), ctrlFmt,
            lay.pxSegmentRect, isPx ? primaryTextBrush.Get() : secondaryTextBrush.Get());
        m_renderTarget->DrawTextW(dpLabel.c_str(), static_cast<UINT32>(dpLabel.size()), ctrlFmt,
            lay.dpSegmentRect, !isPx ? primaryTextBrush.Get() : secondaryTextBrush.Get());
    }

    // 3. 第 2 区：单行换算说明条（动态呈现当前显示器真实缩放比，零折行零重叠）
    const int scalePct = static_cast<int>(std::round(scale * 100.0f));
    const std::wstring formulaText = state.toolbarLayoutChinese
        ? std::format(L"dp = px \u00F7 {:.2g} ({}% \u7F29\u653E)", scale, scalePct)
        : std::format(L"dp = px \u00F7 {:.2g} ({}% scale)", scale, scalePct);
    if (badgeFmt && formulaTextBrush) {
        TextFormatScope fScope(badgeFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawTextW(formulaText.c_str(), static_cast<UINT32>(formulaText.size()), badgeFmt,
            lay.formulaRect, formulaTextBrush.Get());
    }

    if (dividerBrush) {
        m_renderTarget->DrawLine(
            D2D1::Point2F(menuRect.left + 14.0f * scale, lay.dividerY),
            D2D1::Point2F(menuRect.right - 14.0f * scale, lay.dividerY),
            dividerBrush.Get(), 1.0f * scale);
    }

    // 4. 第 3 & 第 4 区：整行同轴对齐的开关项（深石墨开启态 + 浅银灰关闭态）
    auto drawToggleRow = [&](const D2D1_RECT_F& rowRect, const D2D1_RECT_F& swRect, const std::wstring& label, bool checked) {
        if (isCursorIn(rowRect) && rowHoverBg) {
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 6.0f * scale, 6.0f * scale), rowHoverBg.Get());
        }
        if (ctrlFmt && primaryTextBrush) {
            TextFormatScope rScope(ctrlFmt, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
            const D2D1_RECT_F labelRect = D2D1::RectF(
                menuRect.left + 14.0f * scale, rowRect.top,
                swRect.left - 8.0f * scale, rowRect.bottom);
            m_renderTarget->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), ctrlFmt,
                labelRect, primaryTextBrush.Get());
        }
        const float swH = swRect.bottom - swRect.top;
        const auto swRound = D2D1::RoundedRect(swRect, swH * 0.5f, swH * 0.5f);
        ID2D1Brush* trackBrush = checked ? switchOnBg.Get() : switchOffBg.Get();
        if (trackBrush) m_renderTarget->FillRoundedRectangle(swRound, trackBrush);
        if (knobBrush) {
            const float knobR = (swH - 4.0f * scale) * 0.5f;
            const float knobCx = checked ? (swRect.right - swH * 0.5f) : (swRect.left + swH * 0.5f);
            const float knobCy = (swRect.top + swRect.bottom) * 0.5f;
            if (knobShadowBrush) {
                m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCx, knobCy + 0.8f * scale), knobR, knobR), knobShadowBrush.Get());
            }
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCx, knobCy), knobR, knobR), knobBrush.Get());
        }
    };

    drawToggleRow(lay.positionRowRect, lay.positionSwitchRect,
                  state.toolbarLayoutChinese ? L"显示坐标" : L"Position",
                  state.showPositionInHud);
    drawToggleRow(lay.unitRowRect, lay.unitSwitchRect,
                  state.toolbarLayoutChinese ? L"显示单位" : L"Unit Suffix",
                  state.showUnitInHud);
}

} // namespace tools3000::capture
