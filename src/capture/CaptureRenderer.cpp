#include "capture/CaptureRenderer.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "capture/CaptureHistory.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/HudAvoidanceEngine.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/MarkupBaseHelper.h"
#include <format>
#include <algorithm>
#include <cmath>
#include <utility>
#include <opencv2/imgproc.hpp>
#include <dxgi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

using namespace Microsoft::WRL;

namespace tools3000::capture {

namespace {

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

struct ColorFormatEntry {
    ColorFormatType type;
    const wchar_t* tag;
    std::wstring value;
    std::string clipText;
};

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
        std::format(L"cmyk({}%, {}%, {}%, {}%)", cmykC, cmykM, cmykY, cmykK),
        std::format("cmyk({}%, {}%, {}%, {}%)", cmykC, cmykM, cmykY, cmykK)
    });
    list.push_back({
        ColorFormatType::DEC, L"DEC",
        std::format(L"{}", decVal),
        std::format("{}", decVal)
    });

    return list;
}

/// DirectWrite 文本格式作用域保护 RAII，确保文本对齐、段落对齐与换行策略不跨模块污染
struct TextFormatScope {
    IDWriteTextFormat* format = nullptr;
    DWRITE_TEXT_ALIGNMENT origText = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT origPara = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    DWRITE_WORD_WRAPPING origWrap = DWRITE_WORD_WRAPPING_NO_WRAP;

    TextFormatScope(IDWriteTextFormat* fmt,
                    DWRITE_TEXT_ALIGNMENT text = DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                    DWRITE_WORD_WRAPPING wrap = DWRITE_WORD_WRAPPING_NO_WRAP)
        : format(fmt) {
        if (format) {
            origText = format->GetTextAlignment();
            origPara = format->GetParagraphAlignment();
            origWrap = format->GetWordWrapping();
            format->SetTextAlignment(text);
            format->SetParagraphAlignment(para);
            format->SetWordWrapping(wrap);
        }
    }

    ~TextFormatScope() {
        if (format) {
            format->SetTextAlignment(origText);
            format->SetParagraphAlignment(origPara);
            format->SetWordWrapping(origWrap);
        }
    }
};

} // namespace

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

    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 1.0f),
        m_borderBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.6f),
        m_crosshairBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.3f),
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

    // 画笔
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), m_dimBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.8f), m_infoBgBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), m_infoTextBrush.GetAddressOf());
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
    m_markupCacheBitmap.Reset();
    m_historyBitmap.Reset();
    m_markupClipLayer.Reset();
    m_textInputFormat.Reset();
    m_infoTextFormat.Reset();
    m_textScale = 0.0f;
    m_renderTarget.Reset();
    m_hwnd = nullptr;
}

bool CaptureRenderer::updateDpiScale(float scale) {
    scale = std::clamp(scale > 0.0f ? scale : 1.0f, 1.0f, 5.0f);
    if (m_infoTextFormat && m_textInputFormat &&
        std::abs(scale - m_textScale) < 0.01f) {
        return true;
    }
    if (!m_dwriteFactory) return false;

    ComPtr<IDWriteTextFormat> infoFormat;
    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 15.0f * scale, L"zh-CN",
        infoFormat.GetAddressOf());
    if (FAILED(hr) || !infoFormat) return false;
    infoFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    infoFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextFormat> textInputFormat;
    hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 18.0f * scale, L"zh-CN",
        textInputFormat.GetAddressOf());
    if (FAILED(hr) || !textInputFormat) return false;
    textInputFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textInputFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    m_infoTextFormat = std::move(infoFormat);
    m_textInputFormat = std::move(textInputFormat);
    m_textScale = scale;
    return true;
}

bool CaptureRenderer::updateScreenBitmap(const cv::Mat& image) {
    m_screenBitmap.Reset();
    if (!m_renderTarget || image.empty()) {
        LOG_ERROR("截图底图上传失败: 渲染目标或图像为空");
        return false;
    }

    cv::Mat bgra;
    D2D1_ALPHA_MODE alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    if (image.channels() == 4) {
        bgra = image;
        // A 32-bit BI_RGB screen DIB does not define/populate its alpha byte
        // (it is commonly zero). Treat it as opaque without a full-screen pass
        // that writes 255 into every fourth byte.
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
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alphaMode));
    const HRESULT hr = m_renderTarget->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
        bgra.data, static_cast<UINT32>(bgra.step[0]), props, m_screenBitmap.GetAddressOf());
    if (FAILED(hr) || !m_screenBitmap) {
        LOG_ERROR("截图底图上传 Direct2D 失败, hr=0x{:08X}", static_cast<unsigned>(hr));
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

    // 1. 选区边框（高质感实线与圆角自适应）
    if (radius > 0.5f) {
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_borderBrush.Get(), 2.0f * scale);
    } else {
        m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 2.0f * scale);
    }

    // 2. 八个控制点：采用 PixPin 级纯白实心微圆点 + 主题色精致外描边
    float cx = (rect.left + rect.right) / 2;
    float cy = (rect.top + rect.bottom) / 2;

    D2D1_POINT_2F controls[] = {
        {rect.left, rect.top}, {cx, rect.top}, {rect.right, rect.top},
        {rect.left, cy}, {rect.right, cy},
        {rect.left, rect.bottom}, {cx, rect.bottom}, {rect.right, rect.bottom}
    };

    ComPtr<ID2D1SolidColorBrush> handleWhiteBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), handleWhiteBrush.GetAddressOf());

    const float dotRadius = 4.6f * scale;
    for (auto& pt : controls) {
        auto ellipse = D2D1::Ellipse(pt, dotRadius, dotRadius);
        if (handleWhiteBrush) {
            m_renderTarget->FillEllipse(ellipse, handleWhiteBrush.Get());
        }
        m_renderTarget->DrawEllipse(ellipse, m_borderBrush.Get(), 1.5f * scale);
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
    float selW = rect.right - rect.left;
    float selH = rect.bottom - rect.top;
    if (!CornerRadiusHelper::canShowHandles(selW, selH, scale, minDimension)) {
        return;
    }

    // 始终提供 4 个内角手柄 (左上、右上、右下、左下)，保障用户在选区任一角均可直接交互
    auto handles = CornerRadiusHelper::getCornerHandles(
        rect.left, rect.top, rect.right, rect.bottom,
        cornerRadius, scale, false);
    if (handles.empty()) return;

    // 判定鼠标是否靠近角部
    const float triggerDist = 48.0f * scale;
    bool shouldShowCornerHandle = isDragging;
    int activeCornerIdx = -1;
    int checkCount = static_cast<int>(handles.size());

    if (isDragging) {
        if (cornerIndex >= 0 && cornerIndex < checkCount) {
            activeCornerIdx = cornerIndex;
            shouldShowCornerHandle = true;
        } else {
            float minD = 999999.0f;
            for (int i = 0; i < checkCount; ++i) {
                float d = std::hypot(dragStartPos.x - handles[i].x, dragStartPos.y - handles[i].y);
                if (d < minD) { minD = d; activeCornerIdx = i; }
            }
        }
    } else {
        float minD = 999999.0f;
        for (int i = 0; i < checkCount; ++i) {
            float d = std::hypot(currentCursor.x - handles[i].x, currentCursor.y - handles[i].y);
            if (d <= triggerDist && d < minD) {
                minD = d;
                shouldShowCornerHandle = true;
                activeCornerIdx = i;
            }
        }
    }

    // 0. 常驻微晶控制手柄：4 个角始终清晰可见 (PixPin / Figma 顶级体验)
    ComPtr<ID2D1SolidColorBrush> dormantBgBrush, dormantBorderBrush, dormantInnerDotBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), dormantBgBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.45f), dormantBorderBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.95f), dormantInnerDotBrush.GetAddressOf());
    for (int i = 0; i < checkCount; ++i) {
        if (i == activeCornerIdx && shouldShowCornerHandle) continue; // 活跃角稍后绘制完整高亮态
        auto dormantCircle = D2D1::Ellipse(D2D1::Point2F(handles[i].x, handles[i].y), 4.0f * scale, 4.0f * scale);
        if (dormantBgBrush) m_renderTarget->FillEllipse(dormantCircle, dormantBgBrush.Get());
        if (dormantBorderBrush) m_renderTarget->DrawEllipse(dormantCircle, dormantBorderBrush.Get(), 1.0f * scale);
        auto dormantInner = D2D1::Ellipse(D2D1::Point2F(handles[i].x, handles[i].y), 1.3f * scale, 1.3f * scale);
        if (dormantInnerDotBrush) m_renderTarget->FillEllipse(dormantInner, dormantInnerDotBrush.Get());
    }

    // 仅在鼠标靠近或正在调整时，且仅对这唯一的活跃角进行绘制（其余角保持常驻微点）
    if (shouldShowCornerHandle && activeCornerIdx >= 0 && activeCornerIdx < checkCount) {
        ComPtr<ID2D1SolidColorBrush> handleWhiteBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), handleWhiteBrush.GetAddressOf());
        ComPtr<ID2D1SolidColorBrush> cornerRingBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f), cornerRingBrush.GetAddressOf());

        float outerRingRadius = (isDragging ? 5.0f : 4.0f) * scale;
        float innerDotRadius = (isDragging ? 1.7f : 1.3f) * scale;

        const auto& cpt = handles[activeCornerIdx];
        float inDirX = cpt.inDirX;
        float inDirY = cpt.inDirY;

        ComPtr<ID2D1SolidColorBrush> guideLineBrush, guideArrowBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.75f), guideLineBrush.GetAddressOf()); // #38BDF8 发光青天蓝
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.01f, 0.52f, 0.78f, 0.95f), guideArrowBrush.GetAddressOf()); // #0284C7 科技蓝

        float armDist = (isDragging ? 14.0f : 12.0f) * scale;

        if (guideLineBrush && guideArrowBrush) {
            // A. 严格物理对称的对角发光微导轨 (两端到中心黑点等长 armDist)
            D2D1_POINT_2F pOut = { cpt.x - inDirX * armDist, cpt.y - inDirY * armDist };
            D2D1_POINT_2F pIn  = { cpt.x + inDirX * armDist, cpt.y + inDirY * armDist };

            // 导轨细线 (纯白半透明底衬 + 亮青色)
            if (handleWhiteBrush) {
                m_renderTarget->DrawLine(pOut, pIn, handleWhiteBrush.Get(), 2.0f * scale);
            }
            m_renderTarget->DrawLine(pOut, pIn, guideLineBrush.Get(), 1.2f * scale);

            // B. 外向指示箭头 (指向直角方向)
            float arrLen = 4.0f * scale;
            float perpX = -inDirY * arrLen * 0.7f;
            float perpY =  inDirX * arrLen * 0.7f;
            D2D1_POINT_2F aOut1 = { pOut.x + inDirX * arrLen + perpX, pOut.y + inDirY * arrLen + perpY };
            D2D1_POINT_2F aOut2 = { pOut.x + inDirX * arrLen - perpX, pOut.y + inDirY * arrLen - perpY };
            m_renderTarget->DrawLine(pOut, aOut1, guideArrowBrush.Get(), 1.4f * scale);
            m_renderTarget->DrawLine(pOut, aOut2, guideArrowBrush.Get(), 1.4f * scale);

            // C. 内向指示箭头 (指向圆角方向，与外向严格对称)
            D2D1_POINT_2F aIn1 = { pIn.x - inDirX * arrLen + perpX, pIn.y - inDirY * arrLen + perpY };
            D2D1_POINT_2F aIn2 = { pIn.x - inDirX * arrLen - perpX, pIn.y - inDirY * arrLen - perpY };
            m_renderTarget->DrawLine(pIn, aIn1, guideArrowBrush.Get(), 1.4f * scale);
            m_renderTarget->DrawLine(pIn, aIn2, guideArrowBrush.Get(), 1.4f * scale);
        }

        // 外圈细光环 (白底 + 蓝描边)
        auto outerEllipse = D2D1::Ellipse(D2D1::Point2F(cpt.x, cpt.y), outerRingRadius, outerRingRadius);
        if (handleWhiteBrush) {
            m_renderTarget->FillEllipse(outerEllipse, handleWhiteBrush.Get());
        }
        if (cornerRingBrush) {
            m_renderTarget->DrawEllipse(outerEllipse, cornerRingBrush.Get(), 1.0f * scale);
        }
        m_renderTarget->DrawEllipse(outerEllipse, m_borderBrush.Get(), 1.0f * scale);

        // 中心极客黑曜石实心微圆点（精密高反差深黑点 #0F172A，小巧精致且与纯白外环形成强烈对比）
        auto innerEllipse = D2D1::Ellipse(D2D1::Point2F(cpt.x, cpt.y), innerDotRadius, innerDotRadius);
        ComPtr<ID2D1SolidColorBrush> handleDarkDotBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.95f), handleDarkDotBrush.GetAddressOf());
        if (handleDarkDotBrush) {
            m_renderTarget->FillEllipse(innerEllipse, handleDarkDotBrush.Get());
        }

        // 4. PixPin 同款实时圆角半径微胶囊 [ ╭ 42 ]（适度紧凑：留白减少一半，紧凑而绝不压叠）
        const bool canShowBadge = (selW >= 130.0f * scale && selH >= 130.0f * scale);
        if (canShowBadge) {
            int radVal = static_cast<int>(std::round(cornerRadius));
            std::wstring rText = std::format(L"{}", radVal);
            float badgeW = (28.0f + rText.size() * 8.0f) * scale;
            float badgeH = 20.0f * scale;

            // 严密计算几何距离：胶囊中心 = 手柄点 + inDir * (箭头臂长 + 胶囊自身半对角线 + 8px 紧凑留白)
            float badgeHalfDiag = std::hypot(badgeW * 0.5f, badgeH * 0.5f);
            float badgeDist = armDist + badgeHalfDiag + 8.0f * scale;
            float bcX = cpt.x + inDirX * badgeDist;
            float bcY = cpt.y + inDirY * badgeDist;
            float bx = bcX - badgeW * 0.5f;
            float by = bcY - badgeH * 0.5f;

            // 智能边界保护：小选区下严防胶囊溢出或被裁切
            float minBx = rect.left + 4.0f * scale;
            float maxBx = rect.right - badgeW - 4.0f * scale;
            float minBy = rect.top + 4.0f * scale;
            float maxBy = rect.bottom - badgeH - 4.0f * scale;
            if (minBx <= maxBx) bx = std::clamp(bx, minBx, maxBx);
            if (minBy <= maxBy) by = std::clamp(by, minBy, maxBy);

            auto badgeRect = D2D1::RectF(bx, by, bx + badgeW, by + badgeH);
            auto badgeRounded = D2D1::RoundedRect(badgeRect, 4.0f * scale, 4.0f * scale);

            ComPtr<ID2D1SolidColorBrush> badgeBg, badgeBorder, badgeWhite;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.09f, 0.12f, 0.88f), badgeBg.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), badgeBorder.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), badgeWhite.GetAddressOf());

            if (badgeBg) m_renderTarget->FillRoundedRectangle(badgeRounded, badgeBg.Get());
            if (badgeBorder) m_renderTarget->DrawRoundedRectangle(badgeRounded, badgeBorder.Get(), 1.0f * scale);

            // 绘制与截图 100% 标杆一致的封闭圆角扇形切角矢量微图标
            float iconX = bx + 5.0f * scale;
            float iconY = by + (badgeH - 11.0f * scale) * 0.5f;
            if (badgeWhite && m_d2dFactory) {
                ComPtr<ID2D1PathGeometry> cornerIconGeo;
                m_d2dFactory->CreatePathGeometry(cornerIconGeo.GetAddressOf());
                if (cornerIconGeo) {
                    ComPtr<ID2D1GeometrySink> sink;
                    cornerIconGeo->Open(sink.GetAddressOf());
                    if (sink) {
                        float x0 = iconX + 1.2f * scale;
                        float y0 = iconY + 1.2f * scale;
                        float x1 = iconX + 9.8f * scale;
                        float y1 = iconY + 9.8f * scale;

                        sink->BeginFigure(D2D1::Point2F(x0, y1), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(D2D1::Point2F(x0, y0 + 1.2f * scale));
                        sink->AddArc(D2D1::ArcSegment(
                            D2D1::Point2F(x0 + 1.2f * scale, y0),
                            D2D1::SizeF(1.2f * scale, 1.2f * scale),
                            0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->AddLine(D2D1::Point2F(x0 + 2.5f * scale, y0));
                        sink->AddArc(D2D1::ArcSegment(
                            D2D1::Point2F(x1, y1 - 1.2f * scale),
                            D2D1::SizeF(7.0f * scale, 7.0f * scale),
                            0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->AddLine(D2D1::Point2F(x1, y1));
                        sink->AddLine(D2D1::Point2F(x0, y1));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                    }
                    m_renderTarget->DrawGeometry(cornerIconGeo.Get(), badgeWhite.Get(), 1.2f * scale);
                }
            }

            // 绘制当前圆角数值
            if (m_infoTextFormat && badgeWhite) {
                m_renderTarget->DrawText(rText.c_str(), static_cast<UINT32>(rText.size()),
                                         m_infoTextFormat.Get(),
                                         D2D1::RectF(iconX + 13.0f * scale, by + 2.0f * scale, bx + badgeW, by + badgeH),
                                         badgeWhite.Get());
            }
        }
    }
}

void CaptureRenderer::drawSizeInfo(const D2D1_RECT_F& rect, CaptureState& state) {
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    int rawW = static_cast<int>(rect.right - rect.left);
    int rawH = static_cast<int>(rect.bottom - rect.top);
    int curX = static_cast<int>(state.currentCursor.x > 0 ? state.currentCursor.x : rect.left);
    int curY = static_cast<int>(state.currentCursor.y > 0 ? state.currentCursor.y : rect.top);

    // 根据 px / dp 单位换算
    int displayW = (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel) 
        ? static_cast<int>(std::round(rawW / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawW;
    int displayH = (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel) 
        ? static_cast<int>(std::round(rawH / (state.dpiScale > 0.0f ? state.dpiScale : 1.0f))) : rawH;
    std::wstring unitStr = state.showUnitInHud 
        ? (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel ? L" dp" : L" px") : L"";

    std::wstring info;
    if (state.showPositionInHud) {
        info = std::format(L"{},{}  {} × {}{}", curX, curY, displayW, displayH, unitStr);
    } else {
        info = std::format(L"{} × {}{}", displayW, displayH, unitStr);
    }
    if (state.aspectRatio != AspectRatioPreset::Free) {
        std::wstring arTag = L"";
        switch (state.aspectRatio) {
            case AspectRatioPreset::Ratio_16_9: arTag = L" [16:9]"; break;
            case AspectRatioPreset::Ratio_4_3:  arTag = L" [4:3]"; break;
            case AspectRatioPreset::Ratio_1_1:  arTag = L" [1:1]"; break;
            case AspectRatioPreset::Ratio_Golden: arTag = L" [1.618:1]"; break;
            default: break;
        }
        info += arTag;
    }
    if (state.beautyShell.enabled) {
        info += L" [外壳]";
    }
    float effectiveRadius = state.effectiveCornerRadius();
    if (effectiveRadius > 0.5f) {
        info += std::format(L" (R: {}px)", static_cast<int>(effectiveRadius));
    }

    // 1. DirectWrite 动态精准测量文本宽度（100% 杜绝文字溢出！）
    float textWidth = static_cast<float>(info.size()) * 8.5f * scale;
    if (m_dwriteFactory && m_infoTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            info.c_str(), static_cast<UINT32>(info.size()),
            m_infoTextFormat.Get(), 2000.0f * scale, 50.0f * scale, layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            if (tm.width > 0.0f) textWidth = tm.width;
        }
    }

    float labelW = textWidth + 18.0f * scale;
    float labelH = 28.0f * scale;

    // 前置确保工具栏几何布局最新，为 HUD 智能躲避求解器提供精确的主/二级工具栏障碍物矩形
    rebuildCaptureToolbar(state, rect, m_renderTarget->GetSize());

    std::vector<HudObstacle> obstacles;
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
    if (ShortcutHintOverlay::instance().isVisible()) {
        RECT hintRect = ShortcutHintOverlay::instance().getBounds();
        if (hintRect.right > hintRect.left && hintRect.bottom > hintRect.top) {
            obstacles.push_back({
                D2D1::RectF(static_cast<float>(hintRect.left), static_cast<float>(hintRect.top),
                            static_cast<float>(hintRect.right), static_cast<float>(hintRect.bottom)),
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

    // 4. 光标避让策略：
    // 展开尺寸菜单时用户正在点击菜单选项，不主动躲避光标；
    // 处于拖拽、调整选区、手柄操作、图元绘制或光标进入选区内部/靠近把手时，强力避让光标，彻底杜绝挡住鼠标或把手
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
        if (isInteracting || nearAnyHandle || cursorInsideSelection) {
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

    auto placement = HudAvoidanceEngine::solvePlacement(
        rect, labelW, labelH, m_renderTarget->GetSize(), obstacles, hudCfg, scale, state.lastSizeHudEdge);

    state.lastSizeHudEdge = static_cast<int>(placement.edge);
    auto pillRect = placement.rect;
    state.sizeHudRect = pillRect;

    if (ShortcutHintOverlay::instance().isVisible()) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        std::vector<RECT> avoidRects;
        avoidRects.push_back({ static_cast<LONG>(rect.left + vx), static_cast<LONG>(rect.top + vy),
                               static_cast<LONG>(rect.right + vx), static_cast<LONG>(rect.bottom + vy) });
        if (state.primaryToolbarRect.right > state.primaryToolbarRect.left) {
            avoidRects.push_back({ static_cast<LONG>(state.primaryToolbarRect.left + vx), static_cast<LONG>(state.primaryToolbarRect.top + vy),
                                   static_cast<LONG>(state.primaryToolbarRect.right + vx), static_cast<LONG>(state.primaryToolbarRect.bottom + vy) });
        }
        if (state.secondaryToolbarRect.right > state.secondaryToolbarRect.left) {
            avoidRects.push_back({ static_cast<LONG>(state.secondaryToolbarRect.left + vx), static_cast<LONG>(state.secondaryToolbarRect.top + vy),
                                   static_cast<LONG>(state.secondaryToolbarRect.right + vx), static_cast<LONG>(state.secondaryToolbarRect.bottom + vy) });
        }
        if (state.selectionSideRect.right > state.selectionSideRect.left) {
            avoidRects.push_back({ static_cast<LONG>(state.selectionSideRect.left + vx), static_cast<LONG>(state.selectionSideRect.top + vy),
                                   static_cast<LONG>(state.selectionSideRect.right + vx), static_cast<LONG>(state.selectionSideRect.bottom + vy) });
        }
        avoidRects.push_back({ static_cast<LONG>(pillRect.left + vx), static_cast<LONG>(pillRect.top + vy),
                               static_cast<LONG>(pillRect.right + vx), static_cast<LONG>(pillRect.bottom + vy) });
        ShortcutHintOverlay::instance().updateAvoidance(avoidRects);
    }

    float labelX = pillRect.left;
    float labelY = pillRect.top;

    // 判定鼠标悬停状态
    bool isHovered = (state.currentCursor.x >= pillRect.left && state.currentCursor.x <= pillRect.right &&
                      state.currentCursor.y >= pillRect.top && state.currentCursor.y <= pillRect.bottom);
    state.isSizeHudHovered = isHovered;

    auto pillRounded = D2D1::RoundedRect(pillRect, 6.0f * scale, 6.0f * scale);

    // 图 2 悬停/展开时高亮主题蓝色，默认深色磨砂亚克力
    ComPtr<ID2D1SolidColorBrush> pillBg, pillBorder, pillText;
    if (isHovered || state.isSizeMenuOpen) {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 0.95f), pillBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f), pillBorder.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), pillText.GetAddressOf());
    } else {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.09f, 0.12f, 0.88f), pillBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), pillBorder.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), pillText.GetAddressOf());
    }

    if (pillBg) m_renderTarget->FillRoundedRectangle(pillRounded, pillBg.Get());
    if (pillBorder) m_renderTarget->DrawRoundedRectangle(pillRounded, pillBorder.Get(), 1.0f * scale);

    if (m_infoTextFormat && pillText) {
        m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()),
                                 m_infoTextFormat.Get(),
                                 D2D1::RectF(labelX + 9.0f * scale, labelY + 2.0f * scale,
                                            labelX + labelW - 4.0f * scale, labelY + labelH),
                                 pillText.Get());
    }
}

void CaptureRenderer::drawSizeMenu(const D2D1_RECT_F& hudRect, CaptureState& state) {
    if (!state.isSizeMenuOpen || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    float menuW = 168.0f * scale;
    float menuH = 142.0f * scale;

    float menuX = hudRect.left;
    float menuY = hudRect.top - menuH - 8.0f * scale;
    if (menuY < 4.0f * scale) {
        menuY = hudRect.bottom + 8.0f * scale;
    }
    auto sz = m_renderTarget->GetSize();
    if (menuX + menuW > sz.width - 4.0f * scale) {
        menuX = sz.width - menuW - 4.0f * scale;
    }
    if (menuX < 4.0f * scale) {
        menuX = 4.0f * scale;
    }
    if (menuY + menuH > sz.height - 4.0f * scale) {
        menuY = hudRect.top - menuH - 8.0f * scale;
    }
    menuY = std::clamp(menuY, 4.0f * scale, (std::max)(4.0f * scale, sz.height - menuH - 4.0f * scale));

    auto menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);
    state.sizeMenuRect = menuRect;

    // 绘制纯白磨砂卡片 + 双层柔和阴影
    drawGlassPanel(menuRect, 8.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> primaryBrush, textBrush, subTextBrush, trackOffBrush, borderBrush, whiteBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f), primaryBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.95f), textBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.48f, 0.52f, 0.60f, 0.95f), subTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.88f, 0.92f, 1.0f), trackOffBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), borderBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());

    // 1. 单选按钮：◉ px    ○ dp
    float row1Y = menuY + 16.0f * scale;
    float radioRadius = 6.0f * scale;

    // px 单选
    D2D1_POINT_2F rPxPt{menuX + 22.0f * scale, row1Y + 7.0f * scale};
    bool isPx = (state.sizeUnit == CaptureState::SizeUnit::Pixel);
    if (isPx) {
        m_renderTarget->DrawEllipse(D2D1::Ellipse(rPxPt, radioRadius, radioRadius), primaryBrush.Get(), 1.8f * scale);
        m_renderTarget->FillEllipse(D2D1::Ellipse(rPxPt, 3.2f * scale, 3.2f * scale), primaryBrush.Get());
    } else {
        m_renderTarget->DrawEllipse(D2D1::Ellipse(rPxPt, radioRadius, radioRadius), trackOffBrush.Get(), 1.5f * scale);
    }
    if (m_infoTextFormat && textBrush) {
        m_renderTarget->DrawText(L"px", 2, m_infoTextFormat.Get(),
            D2D1::RectF(rPxPt.x + 10.0f * scale, row1Y, rPxPt.x + 40.0f * scale, row1Y + 18.0f * scale),
            textBrush.Get());
    }

    // dp 单选
    D2D1_POINT_2F rDpPt{menuX + 92.0f * scale, row1Y + 7.0f * scale};
    bool isDp = (state.sizeUnit == CaptureState::SizeUnit::DeviceIndependentPixel);
    if (isDp) {
        m_renderTarget->DrawEllipse(D2D1::Ellipse(rDpPt, radioRadius, radioRadius), primaryBrush.Get(), 1.8f * scale);
        m_renderTarget->FillEllipse(D2D1::Ellipse(rDpPt, 3.2f * scale, 3.2f * scale), primaryBrush.Get());
    } else {
        m_renderTarget->DrawEllipse(D2D1::Ellipse(rDpPt, radioRadius, radioRadius), trackOffBrush.Get(), 1.5f * scale);
    }
    if (m_infoTextFormat && textBrush) {
        m_renderTarget->DrawText(L"dp", 2, m_infoTextFormat.Get(),
            D2D1::RectF(rDpPt.x + 10.0f * scale, row1Y, rDpPt.x + 40.0f * scale, row1Y + 18.0f * scale),
            textBrush.Get());
    }

    // 2. 辅助提示：* dp = px ÷ 屏幕缩放比
    float row2Y = row1Y + 22.0f * scale;
    std::wstring hint = state.toolbarLayoutChinese ? L"* dp = px ÷ 屏幕缩放比" : L"* dp = px ÷ Scale Factor";
    if (m_infoTextFormat && subTextBrush) {
        m_renderTarget->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()), m_infoTextFormat.Get(),
            D2D1::RectF(menuX + 16.0f * scale, row2Y, menuX + menuW - 10.0f * scale, row2Y + 16.0f * scale),
            subTextBrush.Get());
    }

    // 3. 开关 1：位置 (显示/隐藏坐标)
    float row3Y = row2Y + 26.0f * scale;
    std::wstring posLabel = state.toolbarLayoutChinese ? L"位置" : L"Position";
    if (m_infoTextFormat && textBrush) {
        m_renderTarget->DrawText(posLabel.c_str(), static_cast<UINT32>(posLabel.size()), m_infoTextFormat.Get(),
            D2D1::RectF(menuX + 16.0f * scale, row3Y + 2.0f * scale, menuX + 80.0f * scale, row3Y + 22.0f * scale),
            textBrush.Get());
    }
    float swW = 32.0f * scale;
    float swH = 18.0f * scale;
    float swX = menuX + menuW - swW - 16.0f * scale;
    float swY = row3Y + 1.0f * scale;
    auto sw1Rect = D2D1::RectF(swX, swY, swX + swW, swY + swH);
    auto sw1Round = D2D1::RoundedRect(sw1Rect, swH * 0.5f, swH * 0.5f);
    if (state.showPositionInHud) {
        m_renderTarget->FillRoundedRectangle(sw1Round, primaryBrush.Get());
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(swX + swW - swH * 0.5f, swY + swH * 0.5f), (swH - 4.0f * scale) * 0.5f, (swH - 4.0f * scale) * 0.5f), whiteBrush.Get());
    } else {
        m_renderTarget->FillRoundedRectangle(sw1Round, trackOffBrush.Get());
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(swX + swH * 0.5f, swY + swH * 0.5f), (swH - 4.0f * scale) * 0.5f, (swH - 4.0f * scale) * 0.5f), whiteBrush.Get());
    }

    // 4. 开关 2：单位 (显示/隐藏 px/dp 后缀)
    float row4Y = row3Y + 28.0f * scale;
    std::wstring unitLabel = state.toolbarLayoutChinese ? L"单位" : L"Unit";
    if (m_infoTextFormat && textBrush) {
        m_renderTarget->DrawText(unitLabel.c_str(), static_cast<UINT32>(unitLabel.size()), m_infoTextFormat.Get(),
            D2D1::RectF(menuX + 16.0f * scale, row4Y + 2.0f * scale, menuX + 80.0f * scale, row4Y + 22.0f * scale),
            textBrush.Get());
    }
    float sw2Y = row4Y + 1.0f * scale;
    auto sw2Rect = D2D1::RectF(swX, sw2Y, swX + swW, sw2Y + swH);
    auto sw2Round = D2D1::RoundedRect(sw2Rect, swH * 0.5f, swH * 0.5f);
    if (state.showUnitInHud) {
        m_renderTarget->FillRoundedRectangle(sw2Round, primaryBrush.Get());
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(swX + swW - swH * 0.5f, sw2Y + swH * 0.5f), (swH - 4.0f * scale) * 0.5f, (swH - 4.0f * scale) * 0.5f), whiteBrush.Get());
    } else {
        m_renderTarget->FillRoundedRectangle(sw2Round, trackOffBrush.Get());
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(swX + swH * 0.5f, sw2Y + swH * 0.5f), (swH - 4.0f * scale) * 0.5f, (swH - 4.0f * scale) * 0.5f), whiteBrush.Get());
    }
}

void CaptureRenderer::drawToolbar(const D2D1_RECT_F& selectionRect, CaptureState& state) {
    rebuildCaptureToolbar(state, selectionRect, m_renderTarget->GetSize());
    if (state.toolbarButtons.empty()) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    
    // 1. 绘制主工具栏浮岛卡片 (Squircle 11px 现代平滑连续圆角)
    drawGlassPanel(state.primaryToolbarRect, 11.0f * scale, false);

    // 2. 如果存在二级属性栏，绘制二级属性栏浮岛卡片 (Squircle 10.5px)
    if (!state.secondaryToolbarButtons.empty()) {
        drawGlassPanel(state.secondaryToolbarRect, 10.5f * scale, false);
    }

    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

    ComPtr<ID2D1SolidColorBrush> buttonBrush;
    ComPtr<ID2D1SolidColorBrush> activeBrush;
    ComPtr<ID2D1SolidColorBrush> activeBorderBrush;
    ComPtr<ID2D1SolidColorBrush> dangerHoverBrush;
    ComPtr<ID2D1SolidColorBrush> confirmBrush;
    ComPtr<ID2D1SolidColorBrush> confirmHoverBrush;
    ComPtr<ID2D1SolidColorBrush> hoverBrush;
    ComPtr<ID2D1SolidColorBrush> iconBrush;
    ComPtr<ID2D1SolidColorBrush> activeIconBrush;
    ComPtr<ID2D1SolidColorBrush> confirmIconBrush;
    ComPtr<ID2D1SolidColorBrush> dangerIconBrush;
    ComPtr<ID2D1SolidColorBrush> tipTextBrush;
    ComPtr<ID2D1SolidColorBrush> secondaryActiveBrush;
    ComPtr<ID2D1SolidColorBrush> separatorBrush;

    const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), buttonBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f), hoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, isDark ? 0.22f : 0.12f), activeBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, isDark ? 0.30f : 0.20f), secondaryActiveBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.85f), activeBorderBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 1.0f), activeIconBrush.GetAddressOf());
    
    // 完成按钮：高亮品牌/翡翠绿实心药丸
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.44f, 0.95f), confirmBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.62f, 0.38f, 1.0f), confirmHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), confirmIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.26f, 0.26f, isDark ? 0.22f : 0.12f), dangerHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.22f, 0.22f, 1.0f), dangerIconBrush.GetAddressOf());

    m_renderTarget->CreateSolidColorBrush(isDark ? D2D1::ColorF(0.90f, 0.92f, 0.95f, 0.95f) : D2D1::ColorF(0.20f, 0.22f, 0.27f, 0.95f), iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(isDark ? D2D1::ColorF(0.95f, 0.95f, 0.98f, 0.95f) : D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.95f), tipTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f), separatorBrush.GetAddressOf());

    auto drawButtonGroup = [&](const std::vector<ToolbarButton>& btnList) {
        for (const auto& button : btnList) {
            // 绘制前置微刻痕细腻分隔线 (1px 双层微刻痕)
            if (button.isSeparatorBefore) {
                float sepX = button.rect.left - 4.5f * scale;
                float sepTop = button.rect.top + 5.0f * scale;
                float sepBottom = button.rect.bottom - 5.0f * scale;
                FloatingGlassBar::drawSeparator(m_renderTarget.Get(), sepX, sepTop, sepBottom, 1.0f, isDark);
            }

            bool isTool = button.command == ToolbarCommand::SelectTool;
            bool isActiveTool = false;
            if (isTool) {
                if (state.isMarkupToolActive || button.isSecondary) {
                    if (button.tool == MarkupTool::Rectangle || button.tool == MarkupTool::Ellipse || button.tool == MarkupTool::Line) {
                        isActiveTool = (state.currentTool == MarkupTool::Rectangle || state.currentTool == MarkupTool::Ellipse || state.currentTool == MarkupTool::Line);
                    } else if (button.tool == MarkupTool::Pen || button.tool == MarkupTool::Highlight) {
                        isActiveTool = (state.currentTool == MarkupTool::Pen || state.currentTool == MarkupTool::Highlight);
                    } else {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                    if (button.isSecondary) {
                        isActiveTool = (button.tool == state.currentTool);
                    }
                }
            }
            if (button.command == ToolbarCommand::ToggleFill) {
                isActiveTool = state.currentFillMode;
            }
            if (button.command == ToolbarCommand::SelectMosaicType) {
                isActiveTool = (button.intParam == state.currentMosaicType);
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
                isActiveTool = (state.recordSetup.showClickEffects || state.recordSetup.showClickZoom);
            }

            if (button.command == ToolbarCommand::ToggleTextOutline) {
                isActiveTool = state.currentTextOutline;
            }

            bool isColor = button.command == ToolbarCommand::SelectColor;
            bool isOutlineColor = button.command == ToolbarCommand::SelectTextOutlineColor;
            bool isDanger = button.command == ToolbarCommand::Cancel;
            bool isConfirm = button.command == ToolbarCommand::Confirm ||
                             button.command == ToolbarCommand::RecordStartConfirm;
            bool isStepper = (button.command == ToolbarCommand::CycleStrokeWidth ||
                              button.command == ToolbarCommand::CycleElementCornerRadius ||
                              button.command == ToolbarCommand::ToggleCornerRadius ||
                              button.command == ToolbarCommand::CycleBeautyBg ||
                              button.command == ToolbarCommand::CycleBeautyPadding ||
                              button.command == ToolbarCommand::CycleBeautyRadius ||
                              button.command == ToolbarCommand::RecordToggleFormat ||
                              button.command == ToolbarCommand::RecordCycleFps ||
                              button.command == ToolbarCommand::RecordCycleQuality ||
                              button.command == ToolbarCommand::RecordCycleClickEffect);
            bool isHovered = state.currentCursor.x >= button.rect.left && state.currentCursor.x <= button.rect.right &&
                             state.currentCursor.y >= button.rect.top  && state.currentCursor.y <= button.rect.bottom;

            auto rounded = D2D1::RoundedRect(button.rect, 6.0f * scale, 6.0f * scale);

            if (isColor) {
                // 颜色色板: 圆形色球 + 双层高亮光环
                float cx = (button.rect.left + button.rect.right) * 0.5f;
                float cy = (button.rect.top + button.rect.bottom) * 0.5f;
                float r = std::min(button.rect.right - button.rect.left, button.rect.bottom - button.rect.top) * 0.38f;
                auto colorCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);

                ComPtr<ID2D1SolidColorBrush> swatch;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f,
                                 button.color.b / 255.0f, 1.0f),
                    swatch.GetAddressOf());
                if (swatch) m_renderTarget->FillEllipse(colorCircle, swatch.Get());

                // 白色色球增加 1px 浅灰色外边缘
                if (button.color.r > 240 && button.color.g > 240 && button.color.b > 240) {
                    m_renderTarget->DrawEllipse(colorCircle, separatorBrush.Get(), 1.0f);
                }

                bool isActiveColor = (button.color.r == state.currentColor.r &&
                                     button.color.g == state.currentColor.g &&
                                     button.color.b == state.currentColor.b);
                if (isActiveColor) {
                    // 内部白色间隙隔离环 + 外部同色扩散光晕
                    auto innerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 1.2f * scale, r + 1.2f * scale);
                    ComPtr<ID2D1SolidColorBrush> whiteRing;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteRing.GetAddressOf());
                    if (whiteRing) m_renderTarget->DrawEllipse(innerRing, whiteRing.Get(), 1.2f * scale);

                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.8f * scale, r + 2.8f * scale);
                    if (swatch) m_renderTarget->DrawEllipse(outerRing, swatch.Get(), 1.8f * scale);
                } else if (isHovered && hoverBrush) {
                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.2f * scale, r + 2.2f * scale);
                    m_renderTarget->DrawEllipse(outerRing, hoverBrush.Get(), 1.5f * scale);
                }
            } else if (isOutlineColor) {
                // 文字描边色板：支持自适应双色球、曜石黑、纯白
                float cx = (button.rect.left + button.rect.right) * 0.5f;
                float cy = (button.rect.top + button.rect.bottom) * 0.5f;
                float r = std::min(button.rect.right - button.rect.left, button.rect.bottom - button.rect.top) * 0.38f;
                auto colorCircle = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);

                bool isActive = button.color.isAuto()
                    ? state.currentTextOutlineColor.isAuto()
                    : (!state.currentTextOutlineColor.isAuto() && button.color == state.currentTextOutlineColor);

                if (button.color.isAuto()) {
                    auto leftClip = D2D1::RectF(cx - r - 1.0f, cy - r - 1.0f, cx, cy + r + 1.0f);
                    auto rightClip = D2D1::RectF(cx, cy - r - 1.0f, cx + r + 1.0f, cy + r + 1.0f);

                    ComPtr<ID2D1SolidColorBrush> blackBrush, whiteBrush;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f), blackBrush.GetAddressOf());
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 1.0f), whiteBrush.GetAddressOf());

                    if (blackBrush && whiteBrush) {
                        m_renderTarget->PushAxisAlignedClip(leftClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->FillEllipse(colorCircle, blackBrush.Get());
                        m_renderTarget->PopAxisAlignedClip();

                        m_renderTarget->PushAxisAlignedClip(rightClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                        m_renderTarget->FillEllipse(colorCircle, whiteBrush.Get());
                        m_renderTarget->PopAxisAlignedClip();
                    }
                    if (separatorBrush) {
                        m_renderTarget->DrawEllipse(colorCircle, separatorBrush.Get(), 1.0f);
                        m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - r), D2D1::Point2F(cx, cy + r), separatorBrush.Get(), 1.0f);
                    }
                } else {
                    ComPtr<ID2D1SolidColorBrush> swatch;
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f,
                                     button.color.b / 255.0f, 1.0f),
                        swatch.GetAddressOf());
                    if (swatch) m_renderTarget->FillEllipse(colorCircle, swatch.Get());
                    if (button.color.r > 240 && button.color.g > 240 && button.color.b > 240) {
                        if (separatorBrush) m_renderTarget->DrawEllipse(colorCircle, separatorBrush.Get(), 1.0f);
                    }
                }

                if (isActive) {
                    auto innerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 1.2f * scale, r + 1.2f * scale);
                    ComPtr<ID2D1SolidColorBrush> whiteRing;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteRing.GetAddressOf());
                    if (whiteRing) m_renderTarget->DrawEllipse(innerRing, whiteRing.Get(), 1.2f * scale);

                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.8f * scale, r + 2.8f * scale);
                    ComPtr<ID2D1SolidColorBrush> activeRingBrush;
                    m_renderTarget->CreateSolidColorBrush(
                        button.color.isAuto() ? D2D1::ColorF(0.0f, 0.478f, 1.0f, 1.0f) :
                        D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f, button.color.b / 255.0f, 1.0f),
                        activeRingBrush.GetAddressOf());
                    if (activeRingBrush) m_renderTarget->DrawEllipse(outerRing, activeRingBrush.Get(), 1.8f * scale);
                } else if (isHovered && hoverBrush) {
                    auto outerRing = D2D1::Ellipse(D2D1::Point2F(cx, cy), r + 2.2f * scale, r + 2.2f * scale);
                    m_renderTarget->DrawEllipse(outerRing, hoverBrush.Get(), 1.5f * scale);
                }
            } else {
                ComPtr<ID2D1SolidColorBrush> stepperBg;
                if (isStepper) {
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(0.0f, 0.0f, 0.0f, isHovered ? 0.065f : 0.035f), stepperBg.GetAddressOf());
                }

                auto* fillBrush = isConfirm ? (isHovered ? confirmHoverBrush.Get() : confirmBrush.Get())
                                : isStepper ? stepperBg.Get()
                                : isActiveTool ? (button.isSecondary ? secondaryActiveBrush.Get() : activeBrush.Get())
                                : (isDanger && isHovered) ? dangerHoverBrush.Get()
                                : isHovered ? hoverBrush.Get()
                                : buttonBrush.Get();
                if (fillBrush) m_renderTarget->FillRoundedRectangle(rounded, fillBrush);

                if (isActiveTool && activeBorderBrush && !isConfirm && !isStepper) {
                    m_renderTarget->DrawRoundedRectangle(rounded, activeBorderBrush.Get(), 1.2f * scale);
                }

                auto* currentIconBrush = isConfirm ? confirmIconBrush.Get()
                                       : isActiveTool ? activeIconBrush.Get()
                                       : (isDanger && isHovered) ? dangerIconBrush.Get()
                                       : iconBrush.Get();
                drawVectorButtonIcon(button, button.rect, currentIconBrush, scale);
            }

            // 如果带有下拉三角指示器，绘制极精致微型实心倒三角 (3.0x2.0px)
            if (button.hasDropdown) {
                float tx = button.rect.right - 5.0f * scale;
                float ty = (button.rect.top + button.rect.bottom) * 0.5f;
                m_renderTarget->DrawLine(D2D1::Point2F(tx - 1.5f * scale, ty - 1.0f * scale),
                                         D2D1::Point2F(tx + 1.5f * scale, ty - 1.0f * scale), iconBrush.Get(), 1.0f * scale);
                m_renderTarget->DrawLine(D2D1::Point2F(tx - 1.5f * scale, ty - 1.0f * scale),
                                         D2D1::Point2F(tx, ty + 1.2f * scale), iconBrush.Get(), 1.0f * scale);
                m_renderTarget->DrawLine(D2D1::Point2F(tx + 1.5f * scale, ty - 1.0f * scale),
                                         D2D1::Point2F(tx, ty + 1.2f * scale), iconBrush.Get(), 1.0f * scale);
            }
        }
    };

    // 绘制主工具栏、二级属性栏与选区侧边浮动菜单
    drawButtonGroup(state.toolbarButtons);
    if (!state.secondaryToolbarButtons.empty()) {
        drawButtonGroup(state.secondaryToolbarButtons);
    }
    if (!state.selectionSideButtons.empty()) {
        drawGlassPanel(state.selectionSideRect, 8.0f * scale, false);
        drawButtonGroup(state.selectionSideButtons);
    }

    // 绘制展开的二级下拉悬浮菜单
    drawSubmenu(state);

    // 绘制无级滑块与预设悬浮弹窗
    drawSliderPopup(state);

    // 绘制多态微晶下拉菜单
    drawDropdownMenu(state);

    // 悬停浮层提示：世界级暗色曜石微晶胶囊 (DirectWrite 精确排版度量 + 严苛 ASCII 快捷键检测 + 几何边缘避让)
    auto showTooltip = [&](const std::vector<ToolbarButton>& btnList) -> bool {
        for (const auto& button : btnList) {
            if (state.currentCursor.x < button.rect.left || state.currentCursor.x > button.rect.right ||
                state.currentCursor.y < button.rect.top  || state.currentCursor.y > button.rect.bottom) {
                continue;
            }
            std::wstring tip = tooltipForButton(button, state.toolbarLayoutChinese);
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
                                                state.secondaryToolbarRect);
            if (m_infoTextFormat) {
                m_infoTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }

            auto tipRounded = D2D1::RoundedRect(tipRect, 6.0f * scale, 6.0f * scale);

            ComPtr<ID2D1SolidColorBrush> panelBg, panelBorder, textBrush, kbdBgBrush, kbdBorderBrush, kbdTextBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 20.0f / 255.0f, 0.94f), panelBg.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), panelBorder.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.93f, 0.93f, 1.0f), textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), kbdBgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), kbdBorderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), kbdTextBrush.GetAddressOf());

            if (panelBg) m_renderTarget->FillRoundedRectangle(tipRounded, panelBg.Get());
            if (panelBorder) m_renderTarget->DrawRoundedRectangle(tipRounded, panelBorder.Get(), 1.0f * scale);

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
                float kh = 16.0f * scale;
                float ky = tipRect.top + (th - kh) * 0.5f;
                auto kbdRect = D2D1::RectF(kx, ky, kx + kbdW, ky + kh);
                auto kbdRound = D2D1::RoundedRect(kbdRect, 3.5f * scale, 3.5f * scale);
                m_renderTarget->FillRoundedRectangle(kbdRound, kbdBgBrush.Get());
                m_renderTarget->DrawRoundedRectangle(kbdRound, kbdBorderBrush.Get(), 1.0f * scale);
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

    if (state.openSubmenu == SubmenuType::None) {
        if (!showTooltip(state.toolbarButtons)) {
            if (!showTooltip(state.secondaryToolbarButtons)) {
                showTooltip(state.selectionSideButtons);
            }
        }
    }
}

void CaptureRenderer::drawSubmenu(CaptureState& state) {
    if (state.openSubmenu == SubmenuType::None || !m_renderTarget) return;

    float scale = (state.dpiScale > 0.1f) ? state.dpiScale : 1.0f;
    
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
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        float menuX = state.secondaryToolbarRect.left + 8.0f * scale;
        float menuY = state.secondaryToolbarRect.bottom + 6.0f * scale;
        auto sz = m_renderTarget->GetSize();
        if (menuY + menuH > sz.height - 4.0f * scale) {
            menuY = state.secondaryToolbarRect.top - menuH - 6.0f * scale;
        }

        auto menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();
        auto& cfg = tools3000::core::ConfigManager::instance();
        const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
        const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f),
            itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.25f) : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.12f),
            itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(0.92f, 0.94f, 0.96f, 0.95f) : D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.95f),
            textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(0.85f, 0.88f, 0.92f, 0.95f) : D2D1::ColorF(0.20f, 0.22f, 0.26f, 0.95f),
            lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 1.0f),
            activeLineBrush.GetAddressOf());

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
    // 箭头样式下拉菜单 (标准单向、细线折角、圆润机翼、双向对称)
    else if (state.openSubmenu == SubmenuType::ArrowStyle) {
        float menuW = 152.0f * scale;
        float itemH = 32.0f * scale;
        float itemGap = 4.0f * scale;
        float padX = 8.0f * scale;
        float padY = 8.0f * scale;

        struct ArrowItem { ArrowStyle style; std::wstring label; };
        std::vector<ArrowItem> items = {
            { ArrowStyle::Standard, state.toolbarLayoutChinese ? L"标准单向" : L"Standard" },
            { ArrowStyle::Thin, state.toolbarLayoutChinese ? L"细线箭头" : L"Thin" },
            { ArrowStyle::Tech, state.toolbarLayoutChinese ? L"圆润机翼" : L"Swept Wing" },
            { ArrowStyle::DoubleEnded, state.toolbarLayoutChinese ? L"双向箭头" : L"Double" },
        };

        float menuH = items.size() * itemH + (items.size() - 1) * itemGap + 2.0f * padY;

        float menuX = state.secondaryToolbarRect.left + 8.0f * scale;
        float menuY = state.secondaryToolbarRect.bottom + 6.0f * scale;
        auto sz = m_renderTarget->GetSize();
        if (menuY + menuH > sz.height - 4.0f * scale) {
            menuY = state.secondaryToolbarRect.top - menuH - 6.0f * scale;
        }

        auto menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);
        state.openSubmenuRect = menuRect;
        state.submenuButtons.clear();

        drawGlassPanel(menuRect, 9.0f * scale, false);

        const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();
        auto& cfg = tools3000::core::ConfigManager::instance();
        const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
        const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

        ComPtr<ID2D1SolidColorBrush> itemHoverBg, itemActiveBg, textBrush, lineBrush, activeLineBrush;
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f),
            itemHoverBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.25f) : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.12f),
            itemActiveBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(0.92f, 0.94f, 0.96f, 0.95f) : D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.95f),
            textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            isDark ? D2D1::ColorF(0.85f, 0.88f, 0.92f, 0.95f) : D2D1::ColorF(0.20f, 0.22f, 0.26f, 0.95f),
            lineBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 1.0f),
            activeLineBrush.GetAddressOf());

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
}

void CaptureRenderer::drawSliderPopup(CaptureState& state) {
    if (state.sliderPopup.type == SliderPopupType::None || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    float popW = 200.0f * scale;
    float popH = 88.0f * scale;

    // 默认定位在二级属性栏的左侧或居中；若为选区侧边圆角则紧贴侧边栏
    float popX = state.secondaryToolbarRect.left + 12.0f * scale;
    float popY = state.secondaryToolbarRect.bottom + 8.0f * scale;

    if (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) {
        if (state.selectionSideRect.left >= state.toolbarLayoutSelection.right) {
            popX = state.selectionSideRect.right + 8.0f * scale;
        } else {
            popX = state.selectionSideRect.left - popW - 8.0f * scale;
        }
        popY = state.selectionSideRect.top - 10.0f * scale;
    }

    auto sz = m_renderTarget->GetSize();
    if (popY + popH > sz.height - 8.0f * scale) {
        popY = sz.height - popH - 8.0f * scale;
    }
    if (popY < 8.0f * scale) {
        popY = 8.0f * scale;
    }
    if (popX + popW > sz.width - 8.0f * scale) {
        popX = sz.width - popW - 8.0f * scale;
    }
    if (popX < 8.0f * scale) {
        popX = 8.0f * scale;
    }

    auto popRect = D2D1::RectF(popX, popY, popX + popW, popY + popH);
    state.sliderPopup.popupRect = popRect;

    // 1. 磨砂亚克力微浮岛
    drawGlassPanel(popRect, 10.0f * scale, false);

    ComPtr<ID2D1SolidColorBrush> titleBrush, badgeBg, badgeText, trackBg, trackFill, thumbBg, thumbBorder, thumbGlow, presetBg, presetActiveBg, presetText, presetActiveText;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.22f, 0.26f, 0.95f), titleBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f), badgeBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f), badgeText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), trackBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 0.95f), trackFill.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), thumbBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f), thumbBorder.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 0.25f), thumbGlow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.04f), presetBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 0.14f), presetActiveBg.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.28f, 0.35f, 0.95f), presetText.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f), presetActiveText.GetAddressOf());

    std::wstring title = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? (state.beautyShell.enabled
            ? (state.toolbarLayoutChinese ? L"外壳圆角半径" : L"Shell Radius")
            : (state.toolbarLayoutChinese ? L"选区圆角半径" : L"Selection Radius"))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? (state.toolbarLayoutChinese ? L"标注圆角" : L"Corner Radius")
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? (state.toolbarLayoutChinese ? L"文本字号" : L"Font Size")
        : (state.sliderPopup.type == SliderPopupType::MosaicBlockSize)
        ? (state.toolbarLayoutChinese ? L"马赛克粗细" : L"Mosaic Size")
        : (state.toolbarLayoutChinese ? L"标注线宽" : L"Stroke Width");

    int curVal = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? static_cast<int>(std::round(state.effectiveCornerRadius()))
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? static_cast<int>(std::round(state.currentElementCornerRadius))
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? static_cast<int>(std::round(state.activeElement && state.activeElement->tool == MarkupTool::Text ? state.activeElement->fontSize : tools3000::core::ConfigManager::instance().get<double>("/capture/markup/text/fontSize", 18.0)))
        : state.currentStrokeWidth;

    state.sliderPopup.currentValue = curVal;
    int minV = (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 10 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius || state.sliderPopup.type == SliderPopupType::CornerRadius) ? 0 : 1;
    int maxV = (state.sliderPopup.type == SliderPopupType::TextFontSize) ? 72 :
               (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius) ? 80 :
               (state.sliderPopup.type == SliderPopupType::CornerRadius) ? 40 : 28;
    state.sliderPopup.minValue = minV;
    state.sliderPopup.maxValue = maxV;

    // 1. 顶部标题与数值徽章
    float topY = popY + 8.0f * scale;
    if (m_infoTextFormat && titleBrush) {
        TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(title.c_str(), static_cast<UINT32>(title.size()), m_infoTextFormat.Get(),
            D2D1::RectF(popX + 12.0f * scale, topY, popX + 110.0f * scale, topY + 16.0f * scale),
            titleBrush.Get());
    }

    std::wstring valStr = std::format(L"{} px", curVal);
    float badgeW = (16.0f + valStr.size() * 7.5f) * scale;
    float badgeH = 18.0f * scale;
    auto valBadgeRect = D2D1::RectF(popX + popW - badgeW - 12.0f * scale, topY - 1.0f * scale, popX + popW - 12.0f * scale, topY + badgeH - 1.0f * scale);
    if (badgeBg) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(valBadgeRect, 4.0f * scale, 4.0f * scale), badgeBg.Get());
    if (m_infoTextFormat && badgeText) {
        TextFormatScope scopeBadge(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
        m_renderTarget->DrawText(valStr.c_str(), static_cast<UINT32>(valStr.size()), m_infoTextFormat.Get(),
            valBadgeRect,
            badgeText.Get());
    }

    // 2. 中部无级滑动轨道与发光游标
    float trackX = popX + 12.0f * scale;
    float trackY = topY + 24.0f * scale;
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

    // 发光游标 Handle
    float thumbR = (state.sliderPopup.isDragging ? 7.5f : 6.0f) * scale;
    auto thumbEllipse = D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR, thumbR);
    state.sliderPopup.thumbRect = D2D1::RectF(thumbX - thumbR - 2.0f * scale, thumbY - thumbR - 2.0f * scale, thumbX + thumbR + 2.0f * scale, thumbY + thumbR + 2.0f * scale);

    if (thumbGlow) m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, thumbY), thumbR + 3.0f * scale, thumbR + 3.0f * scale), thumbGlow.Get());
    if (thumbBg) m_renderTarget->FillEllipse(thumbEllipse, thumbBg.Get());
    if (thumbBorder) m_renderTarget->DrawEllipse(thumbEllipse, thumbBorder.Get(), 1.5f * scale);

    // 3. 底部快捷预设胶囊
    std::vector<int> presets = (state.sliderPopup.type == SliderPopupType::SelectionCornerRadius)
        ? (state.beautyShell.enabled ? std::vector<int>{ 0, 10, 16, 24, 32 } : std::vector<int>{ 0, 8, 14, 24, 40, 60 })
        : (state.sliderPopup.type == SliderPopupType::CornerRadius)
        ? std::vector<int>{ 0, 6, 12, 18, 24 }
        : (state.sliderPopup.type == SliderPopupType::TextFontSize)
        ? std::vector<int>{ 14, 18, 24, 32, 48, 64 }
        : (state.sliderPopup.type == SliderPopupType::MosaicBlockSize)
        ? std::vector<int>{ 4, 8, 12, 16, 24, 32 }
        : std::vector<int>{ 2, 4, 6, 8, 14, 20 };

    state.sliderPopup.presetButtons.clear();
    float presetY = trackY + 16.0f * scale;
    float presetH = 20.0f * scale;
    float slotW = trackW / static_cast<float>(presets.size());

    for (size_t i = 0; i < presets.size(); ++i) {
        int val = presets[i];
        float px = trackX + i * slotW;
        auto pRect = D2D1::RectF(px + 2.0f * scale, presetY, px + slotW - 2.0f * scale, presetY + presetH);
        state.sliderPopup.presetButtons.push_back({ val, pRect });

        bool isActivePreset = (val == curVal);
        auto pRound = D2D1::RoundedRect(pRect, 4.0f * scale, 4.0f * scale);
        auto* pBgBrush = isActivePreset ? presetActiveBg.Get() : presetBg.Get();
        auto* pTxBrush = isActivePreset ? presetActiveText.Get() : presetText.Get();

        if (pBgBrush) m_renderTarget->FillRoundedRectangle(pRound, pBgBrush);

        std::wstring pText = std::format(L"{}", val);
        if (m_infoTextFormat && pTxBrush) {
            m_renderTarget->DrawText(pText.c_str(), static_cast<UINT32>(pText.size()), m_infoTextFormat.Get(),
                D2D1::RectF(pRect.left, pRect.top + 2.0f * scale, pRect.right, pRect.bottom),
                pTxBrush);
        }
    }
}

static void getBeautyColors(BeautyBackgroundType type, D2D1_COLOR_F& c1, D2D1_COLOR_F& c2) {
    switch (type) {
        case BeautyBackgroundType::StudioSlate:
            c1 = D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(203.0f / 255.0f, 213.0f / 255.0f, 225.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PaperChalk:
            c1 = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::SilkMist:
            c1 = D2D1::ColorF(238.0f / 255.0f, 242.0f / 255.0f, 246.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(226.0f / 255.0f, 232.0f / 255.0f, 240.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::MidnightGraphite:
            c1 = D2D1::ColorF(30.0f / 255.0f, 41.0f / 255.0f, 59.0f / 255.0f, 1.0f);
            c2 = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 1.0f);
            break;
        case BeautyBackgroundType::PureMinimal:
        default:
            c1 = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            c2 = D2D1::ColorF(0.96f, 0.96f, 0.97f, 1.0f);
            break;
    }
}

void CaptureRenderer::drawBeautyShellPreview(const D2D1_RECT_F& rect, CaptureState& state) {
    if (!state.beautyShell.enabled || !m_renderTarget) return;
    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const float pad = state.beautyShell.padding * scale;
    if (pad <= 2.0f) return;

    D2D1_RECT_F shellRect = D2D1::RectF(rect.left - pad, rect.top - pad, rect.right + pad, rect.bottom + pad);

    // 1. 绘制对角双色平滑渐变底板画布
    D2D1_COLOR_F c1, c2;
    getBeautyColors(state.beautyShell.bgType, c1, c2);

    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0.0f; stops[0].color = c1;
    stops[1].position = 1.0f; stops[1].color = c2;

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCol;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 2, stopCol.GetAddressOf())) && stopCol) {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradBrush;
        auto gradProps = D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(shellRect.left, shellRect.top),
            D2D1::Point2F(shellRect.right, shellRect.bottom));
        if (SUCCEEDED(m_renderTarget->CreateLinearGradientBrush(gradProps, stopCol.Get(), gradBrush.GetAddressOf())) && gradBrush) {
            float shellCornerR = (std::max)(12.0f * scale, (state.beautyShell.cornerRadius + 8.0f) * scale);
            auto roundedShell = D2D1::RoundedRect(shellRect, shellCornerR, shellCornerR);
            m_renderTarget->FillRoundedRectangle(roundedShell, gradBrush.Get());

            // 1px 微晶边界描边
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shellBorder;
            m_renderTarget->CreateSolidColorBrush(
                state.beautyShell.bgType == BeautyBackgroundType::MidnightGraphite
                    ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                    : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f),
                shellBorder.GetAddressOf());
            if (shellBorder) {
                m_renderTarget->DrawRoundedRectangle(roundedShell, shellBorder.Get(), 1.0f);
            }
        }
    }

    // 2. 渲染物理双层高斯弥散投影
    float r = state.beautyShell.cornerRadius * scale;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ambShadow, keyShadow;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f), ambShadow.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f), keyShadow.GetAddressOf());
    if (ambShadow && keyShadow) {
        auto ambRect = D2D1::RoundedRect(D2D1::RectF(rect.left - 2.0f, rect.top + 6.0f * scale, rect.right + 2.0f, rect.bottom + 12.0f * scale), r + 4.0f, r + 4.0f);
        m_renderTarget->FillRoundedRectangle(ambRect, ambShadow.Get());
        auto keyRect = D2D1::RoundedRect(D2D1::RectF(rect.left, rect.top + 2.0f * scale, rect.right, rect.bottom + 4.0f * scale), r, r);
        m_renderTarget->FillRoundedRectangle(keyRect, keyShadow.Get());
    }

    // 3. 将原图裁切为指定圆角后重新绘制于 rect 内部
    if (m_screenBitmap && m_d2dFactory) {
        auto roundedClip = D2D1::RoundedRect(rect, r, r);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedClip, clipGeo.GetAddressOf())) && clipGeo) {
            Microsoft::WRL::ComPtr<ID2D1Layer> layer;
            if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), layer.Get());
                auto sz = m_renderTarget->GetSize();
                m_renderTarget->DrawBitmap(m_screenBitmap.Get(), D2D1::RectF(0, 0, sz.width, sz.height));
                m_renderTarget->PopLayer();
            }
        }
    }
}

void CaptureRenderer::drawDropdownMenu(CaptureState& state) {
    if (state.dropdownMenu.type == DropdownType::None || state.dropdownMenu.items.empty() || !m_renderTarget) return;

    const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
    const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();

    // 1. 响应式微晶双列排版框架：严格测量全项主标题与辅助描述的最大宽度
    float maxLabelW = 0.0f;
    float maxHintW = 0.0f;
    bool hasAnySwatch = false;

    for (const auto& it : state.dropdownMenu.items) {
        if (!it.label.empty()) {
            D2D1_SIZE_F lsz = measureText(it.label, m_infoTextFormat.Get());
            if (lsz.width > maxLabelW) maxLabelW = lsz.width;
        }
        if (!it.hint.empty()) {
            D2D1_SIZE_F hsz = measureText(it.hint, m_infoTextFormat.Get());
            if (hsz.width > maxHintW) maxHintW = hsz.width;
        }
        if (it.hasSwatch) hasAnySwatch = true;
    }

    // 几何规格与自适应留白 (MECE 双列对齐，杜绝重叠与横向跑动)
    const float itemH = 32.0f * scale;
    const float padY = 7.0f * scale;
    const float padX = 10.0f * scale;
    const float swatchSpace = hasAnySwatch ? (12.0f * scale + 10.0f * scale) : 0.0f;
    const float columnGap = (maxHintW > 0.0f) ? 20.0f * scale : 0.0f; // 保证主副文本之间有绝对安全的呼吸间距
    const float checkSpace = 24.0f * scale; // 选中对勾区域

    float contentW = swatchSpace + maxLabelW + columnGap + maxHintW + checkSpace;
    float menuW = contentW + padX * 2.0f + 16.0f * scale;
    if (menuW < 148.0f * scale) menuW = 148.0f * scale;

    float menuH = padY * 2.0f + static_cast<float>(state.dropdownMenu.items.size()) * itemH;

    const auto& anchor = state.dropdownMenu.anchorButtonRect;
    float menuX = anchor.left;
    float menuY = anchor.top - menuH - 6.0f * scale; // 优先在工具栏上方弹出
    if (menuY < 4.0f * scale) {
        menuY = anchor.bottom + 6.0f * scale; // 屏幕顶部容纳不下时向下弹出
    }
    auto rtSize = m_renderTarget->GetSize();
    if (menuX + menuW > rtSize.width - 4.0f * scale) {
        menuX = rtSize.width - menuW - 4.0f * scale;
    }
    if (menuX < 4.0f * scale) menuX = 4.0f * scale;

    state.dropdownMenu.menuRect = D2D1::RectF(menuX, menuY, menuX + menuW, menuY + menuH);

    // 2. 绘制微晶玻璃浮岛底板
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), state.dropdownMenu.menuRect, 8.0f * scale, isDark);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush, hintBrush, checkBrush;
    m_renderTarget->CreateSolidColorBrush(
        isDark ? D2D1::ColorF(0.92f, 0.94f, 0.96f, 1.0f) : D2D1::ColorF(0.12f, 0.14f, 0.18f, 1.0f),
        textBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        isDark ? D2D1::ColorF(0.58f, 0.62f, 0.68f, 0.95f) : D2D1::ColorF(0.45f, 0.48f, 0.54f, 0.95f),
        hintBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.48f, 1.0f, 1.0f),
        checkBrush.GetAddressOf());

    // 确保文本格式作用域隔离为左对齐、垂直居中、不自动换行
    TextFormatScope menuTextScope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);

    // 3. 绘制各选项 (双列严格对齐)
    float curY = menuY + padY;
    const float labelStartX = menuX + padX + 8.0f * scale + swatchSpace;
    const float hintStartX = labelStartX + maxLabelW + columnGap;

    for (size_t i = 0; i < state.dropdownMenu.items.size(); ++i) {
        auto& item = state.dropdownMenu.items[i];
        item.rect = D2D1::RectF(menuX + padX, curY, menuX + menuW - padX, curY + itemH);

        bool isHover = (static_cast<int>(i) == state.dropdownMenu.hoveredIndex);
        item.isHovered = isHover;

        if (isHover || item.isSelected) {
            FloatingGlassBar::drawButtonPill(
                m_renderTarget.Get(), item.rect, 5.0f * scale,
                isHover, item.isSelected, false, false, 1.0f, isDark);
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

        // 绘制标题 Label (首列)
        if (m_infoTextFormat && textBrush && !item.label.empty()) {
            D2D1_RECT_F textRect = D2D1::RectF(labelStartX, curY, labelStartX + maxLabelW + 2.0f * scale, curY + itemH);
            m_renderTarget->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()),
                                     m_infoTextFormat.Get(), textRect, textBrush.Get());
        }

        // 绘制副标题 Hint (次列：全列表横向绝对统一对齐，杜绝跑动与重叠)
        if (m_infoTextFormat && hintBrush && !item.hint.empty()) {
            D2D1_RECT_F hintRect = D2D1::RectF(hintStartX, curY, item.rect.right - checkSpace, curY + itemH);
            m_renderTarget->DrawText(item.hint.c_str(), static_cast<UINT32>(item.hint.size()),
                                     m_infoTextFormat.Get(), hintRect, hintBrush.Get());
        }

        // 绘制选中对勾 (Checkmark)
        if (item.isSelected && checkBrush) {
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
}

void CaptureRenderer::drawVectorButtonIcon(const ToolbarButton& button, const D2D1_RECT_F& rect, ID2D1Brush* brush, float scale) {
    if (!m_renderTarget || !brush) return;

    float cx = (rect.left + rect.right) * 0.5f;
    float cy = (rect.top + rect.bottom) * 0.5f;

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
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolArrowDouble, rect, brush, scale);
            return;
        case ToolbarCommand::SideResetSelection:
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionUndo, rect, brush, scale);
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
                case MarkupTool::Text:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolText, rect, brush, scale);
                    return;
                case MarkupTool::Number:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolNumber, rect, brush, scale);
                    return;
                case MarkupTool::Inpaint:
                    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ToolInpaint, rect, brush, scale);
                    return;
                default: break;
            }
            break;
        }
        case ToolbarCommand::ToggleFill: {
            // 纯矢量填充/描边切换图标（居中方块）
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                button.boolParam ? CaptureIconId::PropFillSolid : CaptureIconId::PropFillOutline,
                rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleTextOutline: {
            // 文字描边开关图标
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                CaptureIconId::PropTextOutline, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ToggleLineStyleDropdown: {
            auto iconId = (button.lineStyleParam == LineStyle::Dashed) ? CaptureIconId::PropDashedLine
                        : (button.lineStyleParam == LineStyle::Dotted) ? CaptureIconId::PropDottedLine
                        : (button.lineStyleParam == LineStyle::DashDot) ? CaptureIconId::PropDashDotLine
                        : CaptureIconId::PropSolidLine;
            auto lineRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, lineRect, brush, scale);
            return;
        }
        case ToolbarCommand::ChooseCustomColor: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropRainbowWheel, rect, brush, scale);
            if (button.boolParam) {
                // 用户已选取自定义颜色: 在彩虹环中央绘制高亮实心微晶圆点 (button.color) 与 1px 纯白边框
                float dotR = 2.6f * scale;
                ComPtr<ID2D1SolidColorBrush> customBrush, whiteBorder;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(button.color.r / 255.0f, button.color.g / 255.0f, button.color.b / 255.0f, 1.0f),
                    customBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f),
                    whiteBorder.GetAddressOf());
                if (customBrush && whiteBorder) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), dotR, dotR), customBrush.Get());
                    m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), dotR, dotR), whiteBorder.Get(), 1.0f * scale);
                }
            }
            return;
        }
        case ToolbarCommand::ToggleArrowStyleDropdown: {
            auto iconId = (button.arrowStyleParam == ArrowStyle::DoubleEnded) ? CaptureIconId::ToolArrowDouble
                        : (button.arrowStyleParam == ArrowStyle::Thin) ? CaptureIconId::ToolArrowThin
                        : (button.arrowStyleParam == ArrowStyle::Tech) ? CaptureIconId::ToolArrowWing
                        : CaptureIconId::ToolArrowStandard;
            auto arrowRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.right - 9.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, arrowRect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleStrokeWidth: {
            auto iconRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.left + 15.0f * scale, rect.bottom);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropStrokeWidth, iconRect, brush, scale);
            if (!button.label.empty() && m_infoTextFormat) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(rect.left + 17.0f * scale, rect.top, rect.right - 2.0f * scale, rect.bottom),
                    brush);
            }
            return;
        }
        case ToolbarCommand::ToggleCornerRadius:
        case ToolbarCommand::CycleElementCornerRadius: {
            if (button.label.empty()) {
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropCornerRadius, rect, brush, scale);
            } else {
                auto iconRect = D2D1::RectF(rect.left + 3.0f * scale, rect.top, rect.left + 15.0f * scale, rect.bottom);
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropCornerRadius, iconRect, brush, scale);
                if (m_infoTextFormat) {
                    TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                    m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                        m_infoTextFormat.Get(),
                        D2D1::RectF(rect.left + 17.0f * scale, rect.top, rect.right - 2.0f * scale, rect.bottom),
                        brush);
                }
            }
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
            auto iconId = (button.intParam == static_cast<int>(NumberBadgeShape::RoundedSquare))
                ? CaptureIconId::PropNumberSquare
                : CaptureIconId::ToolNumber;
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, rect, brush, scale);
            return;
        }
        case ToolbarCommand::ResetNumberCounter: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionResetNumber, rect, brush, scale);
            return;
        }
        case ToolbarCommand::CycleBeautyBg:
        case ToolbarCommand::CycleBeautyPadding:
        case ToolbarCommand::CycleBeautyRadius:
        case ToolbarCommand::RecordToggleFormat:
        case ToolbarCommand::RecordCycleFps:
        case ToolbarCommand::RecordCycleQuality:
        case ToolbarCommand::RecordCycleClickEffect:
        case ToolbarCommand::RecordStartConfirm: {
            if (!button.label.empty() && m_infoTextFormat) {
                TextFormatScope scope(m_infoTextFormat.Get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, DWRITE_WORD_WRAPPING_NO_WRAP);
                m_renderTarget->DrawText(button.label.c_str(), static_cast<UINT32>(button.label.size()),
                    m_infoTextFormat.Get(),
                    rect,
                    brush);
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
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionToggleSpeaker, rect, brush, scale);
            return;
        }
        case ToolbarCommand::RecordToggleMicrophone: {
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionToggleMic, rect, brush, scale);
            return;
        }
        default: break;
    }
}

void CaptureRenderer::drawGlassPanel(const D2D1_RECT_F& rect, float radius, bool seeThrough) {
    (void)seeThrough;
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), rect, radius, tools3000::core::WinUtils::isSystemDarkMode());
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
    const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();

    // 1. 测量文本尺寸
    float textW = 120.0f * scale;
    float textH = 20.0f * scale;
    if (m_dwriteFactory && m_infoTextFormat) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            state.loupeToastMessage.c_str(),
            static_cast<UINT32>(state.loupeToastMessage.size()),
            m_infoTextFormat.Get(), 1200.0f * scale, 100.0f * scale, layout.GetAddressOf())) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                textW = metrics.width;
                textH = metrics.height;
            }
        }
    }

    // 2. 居中微晶浮层几何布局
    const float padH = 18.0f * scale;
    const float padV = 8.0f * scale;
    const float toastW = (std::max)(textW + padH * 2.0f, 100.0f * scale);
    const float toastH = (std::max)(textH + padV * 2.0f, 32.0f * scale);

    const auto rtSize = m_renderTarget->GetSize();
    const float left = (rtSize.width - toastW) * 0.5f;
    const float top = 28.0f * scale;
    const auto toastRect = D2D1::RectF(left, top, left + toastW, top + toastH);
    const float cornerRadius = 8.0f * scale;

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

    // 6. 绘制高对比微晶文字
    if (m_infoTextFormat) {
        ComPtr<ID2D1SolidColorBrush> textBrush;
        const auto textColor = isDark ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.96f)
                                      : D2D1::ColorF(0.12f, 0.13f, 0.16f, 0.96f);
        m_renderTarget->CreateSolidColorBrush(textColor, textBrush.GetAddressOf());
        if (textBrush) {
            m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_infoTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_renderTarget->DrawTextW(
                state.loupeToastMessage.c_str(),
                static_cast<UINT32>(state.loupeToastMessage.size()),
                m_infoTextFormat.Get(),
                toastRect,
                textBrush.Get()
            );
            m_infoTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
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

    // 仅在标注内容变化时才重新合成并重建 D2D 位图，否则复用缓存。
    // 这条路径原本每帧（16ms）都 clone 整图 + 重绘全部标注 + 新建位图，是主要性能浪费点。
    if (m_markupCacheDirty || !m_markupCacheBitmap) {
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

        if (m_markupCacheBitmap) {
            auto sz = m_markupCacheBitmap->GetPixelSize();
            if (sz.width == static_cast<UINT32>(bgra.cols) && sz.height == static_cast<UINT32>(bgra.rows)) {
                m_markupCacheBitmap->CopyFromMemory(nullptr, bgra.data, static_cast<UINT32>(bgra.cols * 4));
                m_markupCacheDirty = false;
            } else {
                m_markupCacheBitmap.Reset();
            }
        }

        if (!m_markupCacheBitmap) {
            D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );

            ComPtr<ID2D1Bitmap> bitmap;
            HRESULT hr = m_renderTarget->CreateBitmap(
                D2D1::SizeU(bgra.cols, bgra.rows),
                bgra.data,
                bgra.cols * 4,
                bitmapProps,
                bitmap.GetAddressOf()
            );
            if (FAILED(hr) || !bitmap) return;
            m_markupCacheBitmap = bitmap;
            m_markupCacheDirty = false;
        }
    }

    if (m_markupCacheBitmap) {
        auto sz = m_markupCacheBitmap->GetPixelSize();
        // 目标矩形严格对齐整数物理像素，尺寸与位图像素 1:1 绝对对齐，消除双线性重采样引起的模糊与拉伸
        D2D1_RECT_F destRect = calculateMarkupDestRect(selectionRect, sz.width, sz.height);
        D2D1_RECT_F srcRect = calculateMarkupSrcRect(sz.width, sz.height);

        const float scale = (state.dpiScale > 0.0f ? state.dpiScale : 1.0f);
        float effectiveRadius = state.effectiveCornerRadius();
        float radius = effectiveRadius * scale;

        // 如果选区启用了圆角，使用几何图层进行超平滑抗锯齿裁剪，防止尖角溢出底层圆角
        if (radius > 0.5f && m_d2dFactory) {
            auto rounded = D2D1::RoundedRect(destRect, radius, radius);
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(rounded, clipGeo.GetAddressOf())) && clipGeo) {
                if (!m_markupClipLayer) {
                    m_renderTarget->CreateLayer(m_markupClipLayer.GetAddressOf());
                }
                if (m_markupClipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), m_markupClipLayer.Get());
                    m_renderTarget->DrawBitmap(
                        m_markupCacheBitmap.Get(),
                        destRect,
                        1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        srcRect
                    );
                    m_renderTarget->PopLayer();
                } else {
                    m_renderTarget->DrawBitmap(
                        m_markupCacheBitmap.Get(),
                        destRect,
                        1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        srcRect
                    );
                }
            } else {
                m_renderTarget->DrawBitmap(
                    m_markupCacheBitmap.Get(),
                    destRect,
                    1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                    srcRect
                );
            }
        } else {
            m_renderTarget->DrawBitmap(
                m_markupCacheBitmap.Get(),
                destRect,
                1.0f,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                srcRect
            );
        }
    }

    // Figma / CleanShot X 级交互：当处于激活态的元素为矩形时，在 Direct2D 层叠加动态微晶圆角手柄与 [ ╭ R ] 胶囊反馈
    if (state.activeElement && state.activeElement->isActive && state.activeElement->tool == MarkupTool::Rectangle) {
        const float scale = std::clamp(state.dpiScale > 0.0f ? state.dpiScale : 1.0f, 1.0f, 5.0f);
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
        drawCornerRadiusHandleVisual(
            elemRect,
            state.activeElement->cornerRadius,
            scale,
            isDragging,
            state.cornerDragStartPos,
            state.currentCursor,
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
            state.currentTool == MarkupTool::Mosaic || state.currentTool == MarkupTool::Highlight) {
            float w = std::abs(x2 - x1);
            float h = std::abs(y2 - y1);
            float side = std::max(w, h);
            x2 = x1 + (x2 >= x1 ? side : -side);
            y2 = y1 + (y2 >= y1 ? side : -side);
        } else if (state.currentTool == MarkupTool::Arrow || state.currentTool == MarkupTool::Line) {
            double dx = x2 - x1;
            double dy = y2 - y1;
            double dist = std::hypot(dx, dy);
            if (dist >= 1.0) {
                double angle = std::atan2(dy, dx);
                constexpr double step = 3.14159265358979323846 / 4.0;
                double snappedAngle = std::round(angle / step) * step;
                x2 = static_cast<float>(x1 + dist * std::cos(snappedAngle));
                y2 = static_cast<float>(y1 + dist * std::sin(snappedAngle));
            }
        }
    }

    auto rect = D2D1::RectF(std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));

    switch (state.currentTool) {
        case MarkupTool::Rectangle:
        case MarkupTool::Mosaic:
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 2.0f);
            break;

        case MarkupTool::Line:
            m_renderTarget->DrawLine(
                D2D1::Point2F(x1, y1),
                D2D1::Point2F(x2, y2),
                m_borderBrush.Get(),
                2.0f
            );
            break;

        case MarkupTool::Highlight: {
            ComPtr<ID2D1SolidColorBrush> highlightBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.78f, 0.18f, 0.28f), highlightBrush.GetAddressOf());
            m_renderTarget->FillRectangle(rect, highlightBrush.Get());
            m_renderTarget->DrawRectangle(rect, m_borderBrush.Get(), 1.0f);
            break;
        }

        case MarkupTool::Arrow: {
            float dist = std::hypot(x2 - x1, y2 - y1);
            if (dist >= 2.0f) {
                float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
                float strokeW = std::max(1.8f, static_cast<float>(state.currentStrokeWidth) * scale * 0.6f);
                double angle = std::atan2(y2 - y1, x2 - x1);
                double headLen = std::max(12.0, static_cast<double>(strokeW) * 3.5);
                if (headLen > dist * 0.45) headLen = dist * 0.45;

                ComPtr<ID2D1SolidColorBrush> arrowColorBrush;
                m_renderTarget->CreateSolidColorBrush(
                    D2D1::ColorF(state.currentColor.r / 255.0f, state.currentColor.g / 255.0f, state.currentColor.b / 255.0f, 1.0f),
                    arrowColorBrush.GetAddressOf());
                ID2D1Brush* aBrush = arrowColorBrush ? arrowColorBrush.Get() : m_borderBrush.Get();

                auto drawHeadAt = [&](float tipX, float tipY, double a, ArrowStyle style, D2D1_POINT_2F& shaftPt) {
                    double cA = std::cos(a);
                    double sA = std::sin(a);
                    double nX = -sA;
                    double nY = cA;

                    if (style == ArrowStyle::Thin) {
                        double wingAngle = 3.14159265358979323846 / 6.0;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                        D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a - wingAngle)),
                                                        static_cast<float>(tipY - headLen * std::sin(a - wingAngle)));
                        D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(tipX - headLen * std::cos(a + wingAngle)),
                                                        static_cast<float>(tipY - headLen * std::sin(a + wingAngle)));
                        m_renderTarget->DrawLine(tip, w1, aBrush, strokeW);
                        m_renderTarget->DrawLine(tip, w2, aBrush, strokeW);
                        shaftPt = tip;
                    } else if (style == ArrowStyle::Tech) {
                        double halfW = headLen * 0.55;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
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
                        shaftPt = notch;
                    } else {
                        double halfW = headLen * 0.45;
                        D2D1_POINT_2F tip = D2D1::Point2F(tipX, tipY);
                        D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(tipX - headLen * cA), static_cast<float>(tipY - headLen * sA));
                        D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + halfW * nX), static_cast<float>(basePt.y + halfW * nY));
                        D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - halfW * nX), static_cast<float>(basePt.y - halfW * nY));
                        ComPtr<ID2D1PathGeometry> headGeo;
                        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(headGeo.GetAddressOf())) && headGeo) {
                            ComPtr<ID2D1GeometrySink> sink;
                            if (SUCCEEDED(headGeo->Open(sink.GetAddressOf())) && sink) {
                                sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                                sink->AddLine(w1);
                                sink->AddLine(w2);
                                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                                sink->Close();
                                m_renderTarget->FillGeometry(headGeo.Get(), aBrush);
                            }
                        }
                        shaftPt = basePt;
                    }
                };

                if (state.currentArrowStyle == ArrowStyle::DoubleEnded) {
                    D2D1_POINT_2F shaftStart = D2D1::Point2F(x1, y1);
                    D2D1_POINT_2F shaftEnd = D2D1::Point2F(x2, y2);
                    drawHeadAt(x2, y2, angle, ArrowStyle::Standard, shaftEnd);
                    drawHeadAt(x1, y1, angle + 3.14159265358979323846, ArrowStyle::Standard, shaftStart);
                    m_renderTarget->DrawLine(shaftStart, shaftEnd, aBrush, strokeW);
                } else {
                    D2D1_POINT_2F shaftEnd = D2D1::Point2F(x2, y2);
                    drawHeadAt(x2, y2, angle, state.currentArrowStyle, shaftEnd);
                    m_renderTarget->DrawLine(D2D1::Point2F(x1, y1), shaftEnd, aBrush, strokeW);
                }
            }
            break;
        }

        case MarkupTool::Ellipse:
            m_renderTarget->DrawEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F((x1 + x2) / 2.0f, (y1 + y2) / 2.0f),
                    std::abs(x2 - x1) / 2.0f,
                    std::abs(y2 - y1) / 2.0f
                ),
                m_borderBrush.Get(),
                2.0f
            );
            break;

        case MarkupTool::Pen:
            if (state.penPoints.size() >= 2) {
                for (size_t i = 1; i < state.penPoints.size(); ++i) {
                    m_renderTarget->DrawLine(
                        D2D1::Point2F(selectionRect.left + static_cast<float>(state.penPoints[i - 1].x),
                                      selectionRect.top + static_cast<float>(state.penPoints[i - 1].y)),
                        D2D1::Point2F(selectionRect.left + static_cast<float>(state.penPoints[i].x),
                                      selectionRect.top + static_cast<float>(state.penPoints[i].y)),
                        m_borderBrush.Get(),
                        2.0f
                    );
                }
            }
            break;

        case MarkupTool::Number: {
            float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            float radius = 15.0f * scale;
            float dist = std::hypot(x2 - x1, y2 - y1);

            // 绘制起点序号标号徽章画刷与文字画刷
            ComPtr<ID2D1SolidColorBrush> badgeFillBrush, badgeTextBrush;
            m_renderTarget->CreateSolidColorBrush(
                D2D1::ColorF(state.currentColor.r / 255.0f, state.currentColor.g / 255.0f, state.currentColor.b / 255.0f, 1.0f),
                badgeFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), badgeTextBrush.GetAddressOf());
            ID2D1Brush* arrowBrush = badgeFillBrush ? static_cast<ID2D1Brush*>(badgeFillBrush.Get()) : static_cast<ID2D1Brush*>(m_borderBrush.Get());

            // 如果拖拽距离大于阈值 (10px)，实时绘制指向目标点的引出箭头
            if (dist >= 10.0f * scale) {
                double angle = std::atan2(y2 - y1, x2 - x1);
                float strokeW = std::max(1.5f, 2.0f * scale);
                double headLen = std::max(11.0, 3.5 * strokeW);
                if (headLen > dist * 0.45) headLen = dist * 0.45;
                double cA = std::cos(angle), sA = std::sin(angle);
                double nX = -sA, nY = cA;
                double halfW = headLen * 0.45;
                D2D1_POINT_2F tip = D2D1::Point2F(x2, y2);
                D2D1_POINT_2F basePt = D2D1::Point2F(static_cast<float>(x2 - headLen * cA), static_cast<float>(y2 - headLen * sA));
                D2D1_POINT_2F w1 = D2D1::Point2F(static_cast<float>(basePt.x + halfW * nX), static_cast<float>(basePt.y + halfW * nY));
                D2D1_POINT_2F w2 = D2D1::Point2F(static_cast<float>(basePt.x - halfW * nX), static_cast<float>(basePt.y - halfW * nY));

                D2D1_POINT_2F lineStart = D2D1::Point2F(static_cast<float>(x1 + radius * cA), static_cast<float>(y1 + radius * sA));

                m_renderTarget->DrawLine(
                    lineStart,
                    basePt,
                    arrowBrush,
                    strokeW
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

            if (badgeFillBrush) {
                if (state.currentNumberShape == NumberBadgeShape::RoundedSquare) {
                    auto sqBox = D2D1::RectF(x1 - radius, y1 - radius, x1 + radius, y1 + radius);
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sqBox, 5.0f * scale, 5.0f * scale), badgeFillBrush.Get());
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, 5.0f * scale, 5.0f * scale), badgeTextBrush.Get(), 1.2f * scale);
                } else {
                    auto circle = D2D1::Ellipse(D2D1::Point2F(x1, y1), radius, radius);
                    m_renderTarget->FillEllipse(circle, badgeFillBrush.Get());
                    m_renderTarget->DrawEllipse(circle, badgeTextBrush.Get(), 1.2f * scale);
                }
            }

            std::wstring numStr = std::format(L"{}", state.markup.currentNumber());
            if (m_infoTextFormat && badgeTextBrush) {
                m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_infoTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawTextW(
                    numStr.c_str(), static_cast<UINT32>(numStr.size()),
                    m_infoTextFormat.Get(),
                    D2D1::RectF(x1 - radius, y1 - radius, x1 + radius, y1 + radius),
                    badgeTextBrush.Get());
                m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            break;
        }

        default:
            break;
    }
}

void CaptureRenderer::drawDynamicMagnifier(CaptureState& state) {
    if (!m_renderTarget || !m_screenBitmap) return;

    float r = static_cast<float>(state.dynamicMagnifierRadius);
    float scale = state.dynamicMagnifierScale;

    // 获取光标在覆盖层中的位置 (屏幕坐标 -> 覆盖层坐标)
    float cx = static_cast<float>(state.currentCursor.x);
    float cy = static_cast<float>(state.currentCursor.y);

    float srcR = r / scale;
    D2D1_RECT_F srcRect = D2D1::RectF(cx - srcR, cy - srcR, cx + srcR, cy + srcR);
    D2D1_RECT_F dstRect = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);

    // 创建圆形裁剪
    ComPtr<ID2D1EllipseGeometry> ellipse;
    m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), ellipse.GetAddressOf());

    if (ellipse) {
        ComPtr<ID2D1Layer> layer;
        m_renderTarget->CreateLayer(layer.GetAddressOf());
        m_renderTarget->PushLayer(D2D1::LayerParameters(
            D2D1::InfiniteRect(), ellipse.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(),
            1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE), layer.Get());

        m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dstRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, srcRect);

        m_renderTarget->PopLayer();

        // 边框
        m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), m_borderBrush.Get(), 2.0f);
        
        // 十字准星
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 5, cy), D2D1::Point2F(cx + 5, cy), m_borderBrush.Get(), 1.0f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx, cy - 5), D2D1::Point2F(cx, cy + 5), m_borderBrush.Get(), 1.0f);
    }
}

void CaptureRenderer::drawSelectionLoupe(float cx, float cy, CaptureState& state) {
    if (!m_renderTarget || !m_screenBitmap) return;

    int px = static_cast<int>(cx);
    int py = static_cast<int>(cy);
    int r = 0, g = 0, b = 0;
    bool hasColor = sampleScreenColor(px, py, r, g, b, state);

    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;
    constexpr float zoom = 8.0f; 
    
    // 宽敞舒适的黄金比例：27x13 点阵(宽216px)，全面适配大号清晰字体，彻底杜绝 CMYK 等长文本溢出
    int gridCountX = 27;
    int gridCountY = 13;
    float loupeBoxW = gridCountX * zoom * scale; // 216px * scale (极致宽敞，长色彩格式两端留白舒展)
    float loupeBoxH = gridCountY * zoom * scale; // 104px * scale (上半区开阔像素观察区)
    float panelH = 82.0f * scale;                // 82px * scale (下半区大号字体舒展排版，上下 104:82 黄金比例)
    const float totalH = loupeBoxH + panelH;
    const float pad = 12.0f * scale;

    auto size = m_renderTarget->GetSize();

    float lx = cx + 22.0f * scale;
    float ly = cy + 22.0f * scale;

    if (lx + loupeBoxW + pad > size.width) lx = cx - 22.0f * scale - loupeBoxW;
    if (ly + totalH + pad > size.height) ly = cy - 22.0f * scale - totalH;
    lx = std::clamp(lx, pad, std::max(pad, size.width - loupeBoxW - pad));
    ly = std::clamp(ly, pad, std::max(pad, size.height - totalH - pad));

    D2D1_RECT_F dst = D2D1::RectF(lx, ly, lx + loupeBoxW, ly + loupeBoxH);
    
    float srcHalfX = gridCountX / 2.0f; 
    float srcHalfY = gridCountY / 2.0f; 
    D2D1_RECT_F src = D2D1::RectF(cx - srcHalfX, cy - srcHalfY, cx + srcHalfX, cy + srcHalfY);
    
    m_renderTarget->DrawBitmap(m_screenBitmap.Get(), dst, 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, src);

    // 1. 网格细线
    ComPtr<ID2D1SolidColorBrush> gridBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.7f, 0.28f), gridBrush.GetAddressOf());
    if (gridBrush) {
        for (int i = 0; i <= gridCountY; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx, ly + lineOffset), D2D1::Point2F(lx + loupeBoxW, ly + lineOffset), gridBrush.Get(), 0.8f);
        }
        for (int i = 0; i <= gridCountX; ++i) {
            float lineOffset = i * zoom * scale;
            m_renderTarget->DrawLine(D2D1::Point2F(lx + lineOffset, ly), D2D1::Point2F(lx + lineOffset, ly + loupeBoxH), gridBrush.Get(), 0.8f);
        }
    }

    // 2. 中心十字交叉蓝带 (PixPin 经典科技蓝)
    ComPtr<ID2D1SolidColorBrush> blueCrosshair;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.42f, 0.80f, 0.65f), blueCrosshair.GetAddressOf());
    
    float pixelSz = zoom * scale;
    float mcx = lx + (gridCountX / 2) * pixelSz; 
    float mcy = ly + (gridCountY / 2) * pixelSz; 

    if (blueCrosshair) {
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, ly, mcx + pixelSz, mcy), blueCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx, mcy + pixelSz, mcx + pixelSz, ly + loupeBoxH), blueCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(lx, mcy, mcx, mcy + pixelSz), blueCrosshair.Get());
        m_renderTarget->FillRectangle(D2D1::RectF(mcx + pixelSz, mcy, lx + loupeBoxW, mcy + pixelSz), blueCrosshair.Get());
    }

    // 3. 基础画刷定义 (纯白、纯黑、提示灰)
    ComPtr<ID2D1SolidColorBrush> whiteBrush, blackBrush, whiteTextBrush, hintTextBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.06f, 0.08f, 1.0f), blackBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteTextBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f), hintTextBrush.GetAddressOf());

    // 4. 中心单像素目标外框 (双层黑白高反差框，确保在任何背景下绝对聚焦)
    if (whiteBrush && blackBrush) {
        D2D1_RECT_F centerCell = D2D1::RectF(mcx, mcy, mcx + pixelSz, mcy + pixelSz);
        m_renderTarget->DrawRectangle(centerCell, blackBrush.Get(), 2.0f);
        m_renderTarget->DrawRectangle(centerCell, whiteBrush.Get(), 1.2f);
    }
    
    // 5. 下半部分信息面板背景 (先填充纯黑深邃背景)
    D2D1_RECT_F panelRect = D2D1::RectF(lx, ly + loupeBoxH, lx + loupeBoxW, ly + totalH);
    ComPtr<ID2D1SolidColorBrush> panelBg;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), panelBg.GetAddressOf());
    if (panelBg) {
        m_renderTarget->FillRectangle(panelRect, panelBg.Get());
    }
    if (blackBrush) {
        m_renderTarget->DrawRectangle(panelRect, blackBrush.Get(), 1.0f * scale);
    }

    // 6. 上半区网格专属恒定双层微边框（外层 1px 细黑 + 内层 1px 纯白，完整闭合，包含上下半框交界底线）
    if (whiteBrush && blackBrush) {
        D2D1_RECT_F outerBlackRect = D2D1::RectF(dst.left - 1.0f * scale, dst.top - 1.0f * scale, dst.right + 1.0f * scale, dst.bottom + 1.0f * scale);
        m_renderTarget->DrawRectangle(outerBlackRect, blackBrush.Get(), 1.0f * scale);
        m_renderTarget->DrawRectangle(dst, whiteBrush.Get(), 1.0f * scale);
        // 显式加固上下半区交界处的底边白线，确保 100% 完整清晰
        m_renderTarget->DrawLine(D2D1::Point2F(dst.left, dst.bottom), D2D1::Point2F(dst.right, dst.bottom), whiteBrush.Get(), 1.0f * scale);
    }

    // 7. 纯白大号文字与色块排版 (严格单行不换行，极简世界级排版)
    if (hasColor && m_infoTextFormat && m_dwriteFactory) {
        m_infoTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        auto formats = getAllColorFormats(r, g, b);
        size_t curIdx = static_cast<size_t>(state.colorFormat) % formats.size();
        const auto& curEntry = formats[curIdx];

        std::wstring text1 = std::format(L"({} , {})", px, py);
        // 去除大写前缀，直接显示纯净颜色格式值（例如 rgb(58, 134, 255) 或 cmyk(100%, 100%, 100%, 100%)）
        std::wstring text2 = curEntry.value;
        std::wstring text3 = L"Shift: 切换颜色格式";
        
        bool toast = (state.loupeToastUntil != 0 && GetTickCount() < state.loupeToastUntil);
        std::wstring text4 = toast
            ? (state.loupeToastMessage.empty() ? L"✓ 已复制到剪贴板!" : state.loupeToastMessage)
            : L"C: 复制 HEX  |  Shift+C: 复制 RGB";

        float tx = lx + 6.0f * scale;
        float tw = loupeBoxW - 12.0f * scale;
        float currY = ly + loupeBoxH + 4.0f * scale;
        
        // 第 1 行：坐标 (大号纯白居中)
        m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_renderTarget->DrawTextW(text1.c_str(), (UINT32)text1.size(), m_infoTextFormat.Get(),
            D2D1::RectF(tx, currY, tx + tw, currY + 17.0f * scale), whiteTextBrush.Get());
        currY += 19.0f * scale;

        // 第 2 行：当前颜色值 (大号采样色块 + 纯净颜色值，无溢出舒展排版)
        m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        ComPtr<IDWriteTextLayout> layout;
        m_dwriteFactory->CreateTextLayout(text2.c_str(), (UINT32)text2.size(), m_infoTextFormat.Get(), 1000.0f, 20.0f * scale, layout.GetAddressOf());
        float textW = 0;
        if (layout) {
            DWRITE_TEXT_METRICS metrics;
            layout->GetMetrics(&metrics);
            textW = metrics.width;
        }

        float boxSz = 12.0f * scale;
        float gap = 6.0f * scale;
        float totalBlockW = boxSz + gap + textW;
        float startX = tx + (tw - totalBlockW) / 2.0f;

        ComPtr<ID2D1SolidColorBrush> colorBox;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorBox.GetAddressOf());
        if (colorBox) {
            float boxY = currY + (18.0f * scale - boxSz) / 2.0f;
            D2D1_RECT_F colorRect = D2D1::RectF(startX, boxY, startX + boxSz, boxY + boxSz);
            m_renderTarget->FillRectangle(colorRect, colorBox.Get());
            if (whiteBrush) {
                m_renderTarget->DrawRectangle(colorRect, whiteBrush.Get(), 1.0f * scale);
            }
            
            m_renderTarget->DrawTextW(text2.c_str(), (UINT32)text2.size(), m_infoTextFormat.Get(),
                D2D1::RectF(startX + boxSz + gap, currY, startX + totalBlockW + 20.0f * scale, currY + 18.0f * scale), whiteTextBrush.Get());
        }
        currY += 20.0f * scale;

        // 第 3 行：Shift 切换操作指引 (Shift: 切换颜色格式)
        m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hintTextBrush) {
            m_renderTarget->DrawTextW(text3.c_str(), (UINT32)text3.size(), m_infoTextFormat.Get(),
                D2D1::RectF(tx, currY, tx + tw, currY + 17.0f * scale), hintTextBrush.Get());
        }
        currY += 18.0f * scale;

        // 第 4 行：C 复制状态 (大号居中 / 复制成功亮绿)
        ComPtr<ID2D1SolidColorBrush> actionBrush;
        if (toast) {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.90f, 0.55f, 1.0f), actionBrush.GetAddressOf());
        } else {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f), actionBrush.GetAddressOf());
        }
        if (actionBrush) {
            m_renderTarget->DrawTextW(text4.c_str(), (UINT32)text4.size(), m_infoTextFormat.Get(),
                D2D1::RectF(tx, currY, tx + tw, currY + 17.0f * scale), actionBrush.Get());
        }
    }
}

void CaptureRenderer::drawSmartAlignmentGuides(const D2D1_RECT_F& selRect, CaptureState& state) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    float scale = (state.dpiScale > 0) ? state.dpiScale : 1.0f;

    ComPtr<ID2D1SolidColorBrush> guideBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.94f, 1.0f, 0.75f), guideBrush.GetAddressOf());
    if (!guideBrush) return;

    float cx = (selRect.left + selRect.right) * 0.5f;
    float cy = (selRect.top + selRect.bottom) * 0.5f;
    float midScreenX = size.width * 0.5f;
    float midScreenY = size.height * 0.5f;

    // 1. 水平中线对齐 (50% Screen Y)
    if (std::abs(cy - midScreenY) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float x = 0; x < size.width; x += seg + sp) {
            m_renderTarget->DrawLine(D2D1::Point2F(x, midScreenY), D2D1::Point2F(std::min(size.width, x + seg), midScreenY), guideBrush.Get(), 1.0f * scale);
        }
    }

    // 2. 垂直中线对齐 (50% Screen X)
    if (std::abs(cx - midScreenX) < 4.0f * scale) {
        float seg = 6.0f * scale, sp = 4.0f * scale;
        for (float y = 0; y < size.height; y += seg + sp) {
            m_renderTarget->DrawLine(D2D1::Point2F(midScreenX, y), D2D1::Point2F(midScreenX, std::min(size.height, y + seg)), guideBrush.Get(), 1.0f * scale);
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

    std::wstring displayMsg = L"二维码: " + tools3000::core::WinUtils::utf8ToWstring(state.detectedQrText);
    if (displayMsg.size() > 28) {
        displayMsg = displayMsg.substr(0, 26) + L"...";
    }

    // 测量文字宽度
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                                      m_infoTextFormat.Get(), 600.0f, chipH, layout.GetAddressOf());
    float textW = 120.0f * scale;
    if (layout) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        textW = metrics.width;
    }

    float chipW = 24.0f * scale + textW + pad * 2;
    float cx2 = selRect.right;
    float cy1 = selRect.top - chipH - 6.0f * scale;
    if (cy1 < 10.0f * scale) {
        cy1 = selRect.top + 6.0f * scale; // 选区靠近屏幕顶端时移至内部
    }
    float cx1 = std::max(selRect.left, cx2 - chipW);
    state.qrChipRect = D2D1::RectF(cx1, cy1, cx1 + chipW, cy1 + chipH);

    // 绘制毛玻璃微胶囊底框与悬停高亮
    ComPtr<ID2D1SolidColorBrush> chipBg, chipBorder, iconBrush, textBrush;
    if (state.isQrChipHovered) {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.53f, 0.90f, 0.92f), chipBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), chipBorder.GetAddressOf());
    } else {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.14f, 0.88f), chipBg.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.35f, 0.35f, 0.42f, 0.80f), chipBorder.GetAddressOf());
    }
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), iconBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.98f, 1.0f), textBrush.GetAddressOf());

    if (chipBg && chipBorder) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.0f * scale, 6.0f * scale), chipBg.Get());
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(state.qrChipRect, 6.0f * scale, 6.0f * scale), chipBorder.Get(), 1.0f * scale);
    }

    // 绘制二维码纯矢量图标
    auto iconRect = D2D1::RectF(state.qrChipRect.left + 6.0f * scale, state.qrChipRect.top + 3.0f * scale,
                                state.qrChipRect.left + 22.0f * scale, state.qrChipRect.bottom - 3.0f * scale);
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::PropQrCode, iconRect, iconBrush.Get(), scale);

    // 绘制文字
    if (m_infoTextFormat && textBrush) {
        m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        auto textRect = D2D1::RectF(state.qrChipRect.left + 25.0f * scale, state.qrChipRect.top + 4.0f * scale,
                                    state.qrChipRect.right - 6.0f * scale, state.qrChipRect.bottom);
        m_renderTarget->DrawTextW(displayMsg.c_str(), static_cast<UINT32>(displayMsg.size()),
                                  m_infoTextFormat.Get(), textRect, textBrush.Get());
    }
}

void CaptureRenderer::drawCrosshair(float x, float y, CaptureState* pState) {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();
    m_renderTarget->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, size.height), m_crosshairBrush.Get(), 1.0f);
    m_renderTarget->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(size.width, y), m_crosshairBrush.Get(), 1.0f);

    if (pState && m_infoTextFormat) {
        int px = static_cast<int>(x);
        int py = static_cast<int>(y);
        int r = 0, g = 0, b = 0;
        if (sampleScreenColor(px, py, r, g, b, *pState)) {
            const float scale = std::clamp(pState->dpiScale > 0.0f ? pState->dpiScale : 1.0f, 1.0f, 5.0f);
            std::wstring hudText = std::format(L"X: {}  Y: {}  #{:02X}{:02X}{:02X}", px, py, r, g, b);

            float badgeW = static_cast<float>(hudText.size()) * 7.2f * scale + 24.0f * scale;
            float badgeH = 20.0f * scale;
            float badgeX = x + 14.0f * scale;
            float badgeY = y - badgeH - 8.0f * scale;
            if (badgeY < 8.0f * scale) badgeY = y + 14.0f * scale;
            if (badgeX + badgeW > size.width - 8.0f * scale) badgeX = x - badgeW - 14.0f * scale;

            auto badgeRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
            auto badgeRound = D2D1::RoundedRect(badgeRect, 5.0f * scale, 5.0f * scale);

            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush, borderBrush, textBrush, colorChipBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.09f, 0.12f, 0.88f), bgBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), borderBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), textBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f), colorChipBrush.GetAddressOf());

            if (bgBrush) m_renderTarget->FillRoundedRectangle(badgeRound, bgBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(badgeRound, borderBrush.Get(), 1.0f * scale);

            float chipSz = 9.0f * scale;
            float chipX = badgeX + 6.0f * scale;
            float chipY = badgeY + (badgeH - chipSz) * 0.5f;
            auto chipRect = D2D1::RectF(chipX, chipY, chipX + chipSz, chipY + chipSz);
            if (colorChipBrush) m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), colorChipBrush.Get());
            if (borderBrush) m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(chipRect, 2.0f * scale, 2.0f * scale), borderBrush.Get(), 0.8f * scale);

            m_renderTarget->DrawTextW(hudText.c_str(), static_cast<UINT32>(hudText.size()), m_infoTextFormat.Get(),
                D2D1::RectF(chipX + chipSz + 5.0f * scale, badgeY + 1.0f * scale, badgeX + badgeW - 4.0f * scale, badgeY + badgeH),
                textBrush.Get());
        }
    }
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
    auto entry = CaptureHistory::instance().get(state.historyIndex);
    if (entry && !entry->image.empty() && m_renderTarget) {
        cv::Mat bgra;
        if (entry->image.channels() == 3) {
            cv::cvtColor(entry->image, bgra, cv::COLOR_BGR2BGRA);
        } else if (entry->image.channels() == 4) {
            bgra = entry->image;
        }
        if (!bgra.empty()) {
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );
            m_renderTarget->CreateBitmap(
                D2D1::SizeU(bgra.cols, bgra.rows),
                bgra.data, bgra.cols * 4, props, m_historyBitmap.GetAddressOf()
            );
        }
    }
    m_needsRender = true;
}

void CaptureRenderer::render(CaptureState& state) {
    if (!m_renderTarget) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (state.historyMode) {
        auto size = m_renderTarget->GetSize();
        m_renderTarget->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), m_dimBrush.Get());
        
        if (m_historyBitmap) {
            auto bmpSize = m_historyBitmap->GetSize();
            float x = (size.width - bmpSize.width) / 2.0f;
            float y = (size.height - bmpSize.height) / 2.0f;
            D2D1_RECT_F dstRect = D2D1::RectF(x, y, x + bmpSize.width, y + bmpSize.height);
            m_renderTarget->DrawBitmap(m_historyBitmap.Get(), dstRect);
            m_renderTarget->DrawRectangle(dstRect, m_borderBrush.Get(), 2.0f);
            
            auto info = std::format(L"历史回放: {} / {}", state.historyIndex + 1, CaptureHistory::instance().count());
            drawGlassPanel(D2D1::RectF(x, y - 30.0f, x + 200.0f, y - 6.0f), 5.0f, false);
            m_renderTarget->DrawText(info.c_str(), static_cast<UINT32>(info.size()),
                                     m_infoTextFormat.Get(),
                                     D2D1::RectF(x, y - 30.0f, x + 200.0f, y - 6.0f),
                                     m_infoTextBrush.Get());
        } else {
            auto info = L"暂无历史截图";
            m_renderTarget->DrawText(info, 6, m_infoTextFormat.Get(),
                                     D2D1::RectF(0, 0, size.width, size.height), m_infoTextBrush.Get());
        }
        
        const HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LOG_WARN("CaptureRenderer: EndDraw device lost (hr=0x{:08X}), releasing window resources", static_cast<uint32_t>(hr));
            releaseWindowResources();
        }
        return;
    }

    // 绘制冻结的屏幕
    if (m_screenBitmap) {
        auto size = m_renderTarget->GetSize();
        m_renderTarget->DrawBitmap(m_screenBitmap.Get(),
                                   D2D1::RectF(0, 0, size.width, size.height));
    }


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

        if (state.currentTool == MarkupTool::Magnifier && !state.isMarking && !state.isManipulating) {
            drawDynamicMagnifier(state);
        }

        drawSmartAlignmentGuides(selRect, state);
        drawSelection(selRect, state);
        drawSizeInfo(selRect, state);
        drawToolbar(selRect, state);
        drawSizeMenu(state.sizeHudRect, state);
        drawDropdownMenu(state);
    } else {
        // 空闲/选区前 — 全屏变暗 + 十字准星 + 窗口高亮
        auto size = m_renderTarget->GetSize();
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
            
            float scale = (state.dpiScale > 0.0f) ? state.dpiScale : 1.0f;
            float r = state.detectedWindowCornerRadius * scale;
            auto roundedWin = D2D1::RoundedRect(winRect, r, r);

            // 窗口区域采用圆角几何裁剪透出原始画面（去掉变暗效果）
            if (m_screenBitmap && m_d2dFactory) {
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(roundedWin, geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1Layer> layer;
                    if (SUCCEEDED(m_renderTarget->CreateLayer(layer.GetAddressOf())) && layer) {
                        m_renderTarget->PushLayer(
                            D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
                                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                            layer.Get());
                        m_renderTarget->DrawBitmap(m_screenBitmap.Get(), winRect, 1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &winRect);
                        m_renderTarget->PopLayer();
                    }
                }
            }
            // 紫色优雅圆角高亮外框
            m_renderTarget->DrawRoundedRectangle(roundedWin, m_borderBrush.Get(), 2.0f * scale);
            // 显示窗口尺寸与圆角提示
            drawSizeInfo(winRect, state);

            // 动态渲染微晶层级提示徽章 (如 "[2/4] 控件: 搜索框 (680x42)")
            if (GetTickCount() < state.hierarchyBadgeFadeOutUntil && !state.hierarchyBadgeText.empty() && m_dwriteFactory && m_infoTextFormat) {
                float badgeH = 26.0f * scale;
                float padH = 12.0f * scale;
                ComPtr<IDWriteTextLayout> badgeLayout;
                m_dwriteFactory->CreateTextLayout(
                    state.hierarchyBadgeText.c_str(),
                    static_cast<UINT32>(state.hierarchyBadgeText.size()),
                    m_infoTextFormat.Get(), 800.0f, badgeH, badgeLayout.GetAddressOf());
                float textW = 120.0f * scale;
                if (badgeLayout) {
                    DWRITE_TEXT_METRICS m{};
                    badgeLayout->GetMetrics(&m);
                    textW = m.width;
                }
                float badgeW = textW + padH * 2.0f;
                float bx1 = winRect.left + (winRect.right - winRect.left - badgeW) * 0.5f;
                float by1 = winRect.top - badgeH - 8.0f * scale;
                if (by1 < 10.0f * scale) by1 = winRect.top + 8.0f * scale;
                D2D1_RECT_F badgeRect = D2D1::RectF(bx1, by1, bx1 + badgeW, by1 + badgeH);

                if (m_infoBgBrush && m_borderBrush && m_infoTextBrush) {
                    DWORD now = GetTickCount();
                    DWORD remaining = (state.hierarchyBadgeFadeOutUntil > now) ? (state.hierarchyBadgeFadeOutUntil - now) : 0;
                    float alphaMultiplier = 1.0f;
                    if (remaining < 300) {
                        alphaMultiplier = static_cast<float>(remaining) / 300.0f;
                    }
                    float prevBgOpacity = m_infoBgBrush->GetOpacity();
                    float prevBorderOpacity = m_borderBrush->GetOpacity();
                    float prevTextOpacity = m_infoTextBrush->GetOpacity();

                    m_infoBgBrush->SetOpacity(prevBgOpacity * alphaMultiplier);
                    m_borderBrush->SetOpacity(prevBorderOpacity * alphaMultiplier);
                    m_infoTextBrush->SetOpacity(prevTextOpacity * alphaMultiplier);

                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, 6.0f * scale, 6.0f * scale), m_infoBgBrush.Get());
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(badgeRect, 6.0f * scale, 6.0f * scale), m_borderBrush.Get(), 1.2f * scale);
                    m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    auto textR = D2D1::RectF(badgeRect.left, badgeRect.top + 4.0f * scale, badgeRect.right, badgeRect.bottom);
                    m_renderTarget->DrawTextW(state.hierarchyBadgeText.c_str(),
                        static_cast<UINT32>(state.hierarchyBadgeText.size()),
                        m_infoTextFormat.Get(), textR, m_infoTextBrush.Get());
                    m_infoTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                    m_infoBgBrush->SetOpacity(prevBgOpacity);
                    m_borderBrush->SetOpacity(prevBorderOpacity);
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

    // 渲染微晶浮层数值反馈 (Toast)
    drawFloatingToast(state);

    const HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("CaptureRenderer: EndDraw device lost (hr=0x{:08X}), releasing window resources", static_cast<uint32_t>(hr));
        releaseWindowResources();
    }
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

} // namespace tools3000::capture
