#include "ui/ToastOverlay.h"
#include "ui/ToastStyle.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/WinUtils.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "core/events/MainThreadDispatcher.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace tools3000::ui {

namespace {

constexpr D2D1_COLOR_F scaleColorAlpha(const D2D1_COLOR_F& c, float alphaScale) noexcept {
    return D2D1_COLOR_F{ c.r, c.g, c.b, std::clamp(c.a * alphaScale, 0.0f, 1.0f) };
}

D2D1_COLOR_F colorFromSysColor(int sysIndex, float alpha) noexcept {
    const DWORD rgb = GetSysColor(sysIndex);
    return D2D1_COLOR_F{
        static_cast<float>(GetRValue(rgb)) / 255.0f,
        static_cast<float>(GetGValue(rgb)) / 255.0f,
        static_cast<float>(GetBValue(rgb)) / 255.0f,
        std::clamp(alpha, 0.0f, 1.0f)
    };
}

bool isSystemHighContrastEnabled() noexcept {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> createVerticalLinearBrush(
    ID2D1RenderTarget* rt,
    float topY,
    float bottomY,
    const D2D1_COLOR_F& topColor,
    const D2D1_COLOR_F& bottomColor) {
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
    if (!rt) return brush;

    const D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, topColor },
        { 1.0f, bottomColor }
    };
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (FAILED(rt->CreateGradientStopCollection(
            stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection)) ||
        !stopCollection) {
        return brush;
    }

    rt->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0.0f, topY),
            D2D1::Point2F(0.0f, bottomY)),
        stopCollection.Get(),
        &brush);
    return brush;
}

} // namespace

static constexpr const wchar_t* TOAST_CLASS = L"Tools3000_ToastOverlay";
static constexpr UINT_PTR TIMER_ID = 1;
static constexpr UINT_PTR FADE_IN_TIMER_ID = 3;
static constexpr int HIDE_TIMEOUT_MS = 2000;
static constexpr int FADE_INTERVAL_MS = 16;

ToastOverlay& ToastOverlay::instance() {
    static ToastOverlay inst;
    return inst;
}

bool ToastOverlay::initialize(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wc);

    m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(hInstance, L"Tools3000_ToastHelperOwner");

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        TOAST_CLASS, L"",
        WS_POPUP,
        0, 0, 1, 1,
        m_helperOwnerHwnd, nullptr, hInstance, nullptr
    );

    if (!m_hwnd) {
        if (m_helperOwnerHwnd) {
            DestroyWindow(m_helperOwnerHwnd);
            m_helperOwnerHwnd = nullptr;
        }
        return false;
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(m_hwnd);

    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        if (m_helperOwnerHwnd) {
            DestroyWindow(m_helperOwnerHwnd);
            m_helperOwnerHwnd = nullptr;
        }
        m_d2dFactory.Reset();
        m_dwriteFactory.Reset();
        return false;
    }

    updatePlacement();
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    LOG_INFO("系统通知 Overlay 初始化完成 (Taskbar Safe)");
    return true;
}

void ToastOverlay::shutdown() {
    discardResources();
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_ID);
        KillTimer(m_hwnd, FADE_IN_TIMER_ID);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

void ToastOverlay::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!enabled && m_hwnd) {
        hideNow();
    }
}

void ToastOverlay::hideNow() {
    if (!m_hwnd) return;
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            hideNow();
        });
        return;
    }
    KillTimer(m_hwnd, TIMER_ID);
    KillTimer(m_hwnd, FADE_IN_TIMER_ID);
    ShowWindow(m_hwnd, SW_HIDE);
    tools3000::core::accessibility::hideOverlay(m_hwnd);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_opacity = 0.0f;
        m_animScale = 1.0f;
        m_fadingIn = false;
    }
}

bool ToastOverlay::isEnabled() const {
    return m_enabled;
}

void ToastOverlay::showToast(const std::string& text) {
    if (!m_enabled || text.empty()) return;

    const std::wstring cleanWide = std::wstring(
        ToastStyle::stripMachineStatusPrefix(tools3000::core::WinUtils::utf8ToWstring(text)));
    const std::string cleanText = tools3000::core::WinUtils::wstringToUtf8(cleanWide);
    if (cleanText.empty()) return;

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, cleanText]() {
            showToast(cleanText);
        });
        return;
    }

    if (!m_hwnd) return;

    {
        std::lock_guard lock(m_mutex);
        m_displayText = cleanText;
        if (m_opacity > 0.0f && !m_fadingIn) {
            m_opacity = 1.0f;
            m_animScale = 1.0f;
        } else {
            m_opacity = 0.0f;
            m_animScale = 0.94f;
            m_fadingIn = true;
        }
    }

    updatePlacement();

    KillTimer(m_hwnd, TIMER_ID);
    KillTimer(m_hwnd, FADE_IN_TIMER_ID);

    // 截图或长截图采集期间主动静默全局 Toast，彻底杜绝通知浮层侵入截取画面
    HWND captureOverlay = FindWindowW(L"Tools3000_CaptureOverlay", nullptr);
    HWND scrollOverlay = FindWindowW(L"Tools3000_ScrollCaptureOverlay", nullptr);
    if ((captureOverlay && IsWindowVisible(captureOverlay)) ||
        (scrollOverlay && IsWindowVisible(scrollOverlay))) {
        ShowWindow(m_hwnd, SW_HIDE);
        return;
    }

    render();  // Replace retained layered-window pixels before showing again.
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    tools3000::core::accessibility::announceOverlay(m_hwnd, cleanWide);
    
    if (m_fadingIn) {
        SetTimer(m_hwnd, FADE_IN_TIMER_ID, FADE_INTERVAL_MS, nullptr);
    } else {
        render();
        SetTimer(m_hwnd, TIMER_ID, HIDE_TIMEOUT_MS, nullptr);
    }
}

bool ToastOverlay::createResources() {
    if (!m_renderTarget && m_d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget)) ||
            !m_renderTarget) {
            return false;
        }
        m_renderTarget->SetDpi(96.0f, 96.0f);
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        if (m_renderTarget) {
            const ToastGlassPalette defaultPalette = ToastGlassPalette::forTheme(false);
            m_renderTarget->CreateSolidColorBrush(defaultPalette.bgTop, &m_bgBrush);
            m_renderTarget->CreateSolidColorBrush(defaultPalette.outerContour, &m_strokeBrush);
            m_renderTarget->CreateSolidColorBrush(defaultPalette.text, &m_textBrush);
        }

        if (m_dwriteFactory && !m_textFormat) {
            const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
            const wchar_t* localeName = tools3000::core::isUiLanguageChinese() ? L"zh-CN" : L"en-US";
            const HRESULT hr = m_dwriteFactory->CreateTextFormat(
                fontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                ToastStyle::BaseFontSize * m_dpiScale, localeName, &m_textFormat
            );
            if (FAILED(hr) || !m_textFormat) return false;
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    if (!m_bgBrush || !m_strokeBrush || !m_textBrush) {
        discardResources();
        return false;
    }
    return m_renderTarget && m_textFormat;
}

void ToastOverlay::discardResources() {
    m_textBrush.Reset();
    m_strokeBrush.Reset();
    m_bgBrush.Reset();
    m_textFormat.Reset();
    m_renderTarget.Reset();
    releaseSurface();
}

void ToastOverlay::releaseSurface() {
    if (m_memoryDC && m_oldBitmap) {
        SelectObject(m_memoryDC, m_oldBitmap);
    }
    m_oldBitmap = nullptr;
    if (m_memoryBitmap) DeleteObject(m_memoryBitmap);
    if (m_memoryDC) DeleteDC(m_memoryDC);
    m_memoryBitmap = nullptr;
    m_memoryDC = nullptr;
    m_surfaceWidth = 0;
    m_surfaceHeight = 0;
}

bool ToastOverlay::ensureSurface(int width, int height) {
    if (m_memoryDC && m_memoryBitmap &&
        m_surfaceWidth == width && m_surfaceHeight == height) {
        return true;
    }
    releaseSurface();
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        return false;
    }

    m_memoryDC = memory;
    m_memoryBitmap = bitmap;
    m_oldBitmap = static_cast<HBITMAP>(SelectObject(memory, bitmap));
    m_surfaceWidth = width;
    m_surfaceHeight = height;
    return true;
}

SIZE ToastOverlay::measureWindowSizeForText(const std::wstring& wText, int workAreaWidth) {
    const int marginPx = tools3000::core::dpi::scaleMetric(static_cast<int>(ToastStyle::OuterMargin), m_dpiScale);
    const int screenEdgeGuard = tools3000::core::dpi::scaleMetric(16, m_dpiScale);
    const int safeMaxWinW = (workAreaWidth > 0)
        ? (std::max)(marginPx * 2 + 64, workAreaWidth - screenEdgeGuard)
        : 4096;
    const int safeMaxCapW = (std::max)(64, safeMaxWinW - marginPx * 2);

    if (wText.empty() || !createResources() || !m_dwriteFactory || !m_textFormat) {
        const int fallbackW = (std::min)(
            tools3000::core::dpi::scaleMetric(ToastStyle::BaseWidth, m_dpiScale),
            safeMaxWinW);
        const int fallbackH = tools3000::core::dpi::scaleMetric(ToastStyle::BaseHeight, m_dpiScale);
        m_capsuleWidth = (std::max)(1, (std::min)(fallbackW - marginPx * 2, safeMaxCapW));
        m_capsuleHeight = (std::max)(1, fallbackH - marginPx * 2);
        return SIZE{m_capsuleWidth + marginPx * 2, fallbackH};
    }

    const int horizPadPx = tools3000::core::dpi::scaleMetric(
        static_cast<int>(ToastStyle::HorizontalPadding * 2.0f), m_dpiScale);
    const float maxTextW = (std::min)(
        ToastStyle::maxTextLayoutWidth(m_dpiScale, workAreaWidth),
        (std::max)(48.0f, static_cast<float>(safeMaxCapW - horizPadPx)));
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwriteFactory->CreateTextLayout(
            wText.c_str(),
            static_cast<UINT32>(wText.length()),
            m_textFormat.Get(),
            maxTextW,
            2000.0f * m_dpiScale,
            &layout)) || !layout) {
        const int fallbackW = (std::min)(
            tools3000::core::dpi::scaleMetric(ToastStyle::BaseWidth, m_dpiScale),
            safeMaxWinW);
        const int fallbackH = tools3000::core::dpi::scaleMetric(ToastStyle::BaseHeight, m_dpiScale);
        m_capsuleWidth = (std::max)(1, (std::min)(fallbackW - marginPx * 2, safeMaxCapW));
        m_capsuleHeight = (std::max)(1, fallbackH - marginPx * 2);
        return SIZE{m_capsuleWidth + marginPx * 2, fallbackH};
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const float measuredW = (std::max)(metrics.width, metrics.widthIncludingTrailingWhitespace);
    const SIZE capSize = ToastStyle::capsuleSizeForTextMetrics(
        measuredW, metrics.height, m_dpiScale, workAreaWidth);

    m_capsuleWidth = (std::min)(static_cast<int>(capSize.cx), safeMaxCapW);
    m_capsuleHeight = capSize.cy;
    return SIZE{m_capsuleWidth + marginPx * 2, capSize.cy + marginPx * 2};
}

void ToastOverlay::updatePlacement() {
    if (!m_hwnd || m_updatingPlacement) return;
    m_updatingPlacement = true;
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const float newScale = tools3000::core::dpi::scaleForMonitor(monitor);
    if (std::abs(newScale - m_dpiScale) >= 0.01f) {
        m_dpiScale = newScale;
        discardResources();
    }

    std::wstring wText;
    {
        std::lock_guard lock(m_mutex);
        if (!m_displayText.empty()) {
            wText = tools3000::core::WinUtils::utf8ToWstring(m_displayText);
        }
    }

    const RECT work = tools3000::core::dpi::workArea(monitor);
    const int workWidth = work.right - work.left;
    const SIZE winSize = measureWindowSizeForText(wText, workWidth);
    const int screenEdgeGuard = tools3000::core::dpi::scaleMetric(16, m_dpiScale);
    const int width = (workWidth > screenEdgeGuard)
        ? (std::min)(static_cast<int>(winSize.cx), workWidth - screenEdgeGuard)
        : winSize.cx;
    const int height = winSize.cy;
    const int x = std::clamp(
        static_cast<int>(work.left) + (workWidth - width) / 2,
        static_cast<int>(work.left),
        static_cast<int>((std::max)(work.left, work.right - width)));
    const int y = ToastStyle::originYForWorkArea(
        work.top, work.bottom, height,
        tools3000::core::dpi::scaleMetric(ToastStyle::BaseBottomMargin, m_dpiScale));
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    m_updatingPlacement = false;
}

void ToastOverlay::render() {
    if (!m_hwnd) return;
    if (!createResources()) return;

    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    if (!ensureSurface(width, height)) return;

    RECT clientRc = {0, 0, width, height};
    if (FAILED(m_renderTarget->BindDC(m_memoryDC, &clientRc))) return;
    m_renderTarget->SetDpi(96.0f, 96.0f);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    std::string text;
    float alpha;
    float scaleAnim;
    {
        std::lock_guard lock(m_mutex);
        text = m_displayText;
        alpha = m_opacity;
        scaleAnim = m_animScale;
    }

    if (!text.empty() && alpha > 0.0f) {
        std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(text);

        const float cx = width / 2.0f;
        const float cy = height / 2.0f;
        const float outerMargin = ToastStyle::OuterMargin * m_dpiScale;
        const float capW = (m_capsuleWidth > 0)
            ? (std::min)(static_cast<float>(m_capsuleWidth), (std::max)(1.0f, width - outerMargin * 2.0f))
            : (std::max)(1.0f, width - outerMargin * 2.0f);
        const float capH = (m_capsuleHeight > 0)
            ? (std::min)(static_cast<float>(m_capsuleHeight), (std::max)(1.0f, height - outerMargin * 2.0f))
            : (std::max)(1.0f, height - outerMargin * 2.0f);

        // 应用进场微弹缩放动画
        const float s = (std::max)(0.5f, m_dpiScale);
        D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Scale(scaleAnim, scaleAnim, D2D1::Point2F(cx, cy));
        m_renderTarget->SetTransform(transform);

        D2D1_RECT_F bgRect = D2D1::RectF(
            cx - capW / 2.0f, cy - capH / 2.0f,
            cx + capW / 2.0f, cy + capH / 2.0f);

        // 单行胶囊采用全药丸半圆端头，多行长提示平滑过渡为现代圆角卡片
        const float cornerRadius = (capH <= 64.0f * s) ? (capH * 0.5f) : (16.0f * s);
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(bgRect, cornerRadius, cornerRadius);

        const bool highContrast = isSystemHighContrastEnabled();
        D2D1_COLOR_F activeTextColor{};
        D2D1_COLOR_F activeTextHaloColor{};
        bool drawTextHalo = false;

        if (highContrast) {
            m_bgBrush->SetColor(colorFromSysColor(COLOR_WINDOW, alpha));
            m_bgBrush->SetOpacity(1.0f);
            m_renderTarget->FillRoundedRectangle(roundedRect, m_bgBrush.Get());

            m_strokeBrush->SetColor(colorFromSysColor(COLOR_HIGHLIGHT, alpha));
            m_strokeBrush->SetOpacity(1.0f);
            m_renderTarget->DrawRoundedRectangle(
                roundedRect, m_strokeBrush.Get(), ToastStyle::StrokeWidth * s);

            activeTextColor = colorFromSysColor(COLOR_WINDOWTEXT, alpha);
        } else {
            const std::string configuredTheme =
                tools3000::core::ConfigManager::instance().get<std::string>("/general/theme", "system");
            const bool isDark = ToastGlassPalette::resolveIsDark(
                configuredTheme, tools3000::core::WinUtils::isSystemDarkMode());
            const ToastGlassPalette palette = ToastGlassPalette::forTheme(isDark);

            // 1. 仅向下轻柔偏移的双层水滴微阴影（严格收束在 OuterMargin 内，零矩形截断印子）
            const D2D1_RECT_F shadowFarRect = D2D1::RectF(
                bgRect.left - 2.2f * s, bgRect.top + 0.8f * s,
                bgRect.right + 2.2f * s, bgRect.bottom + 4.4f * s);
            m_bgBrush->SetColor(scaleColorAlpha(palette.shadowFar, alpha));
            m_bgBrush->SetOpacity(1.0f);
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(shadowFarRect, cornerRadius + 2.2f * s, cornerRadius + 2.2f * s),
                m_bgBrush.Get());

            const D2D1_RECT_F shadowNearRect = D2D1::RectF(
                bgRect.left - 0.8f * s, bgRect.top + 0.4f * s,
                bgRect.right + 0.8f * s, bgRect.bottom + 2.2f * s);
            m_bgBrush->SetColor(scaleColorAlpha(palette.shadowNear, alpha));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(shadowNearRect, cornerRadius + 0.8f * s, cornerRadius + 0.8f * s),
                m_bgBrush.Get());

            // 2. 清透冰晶毛玻璃半透主体（垂直折射渐变，告别死黑膏药块）
            auto glassFill = createVerticalLinearBrush(
                m_renderTarget.Get(), bgRect.top, bgRect.bottom,
                scaleColorAlpha(palette.bgTop, alpha),
                scaleColorAlpha(palette.bgBottom, alpha));
            if (glassFill) {
                m_renderTarget->FillRoundedRectangle(roundedRect, glassFill.Get());
            } else {
                m_bgBrush->SetColor(scaleColorAlpha(palette.bgTop, alpha));
                m_renderTarget->FillRoundedRectangle(roundedRect, m_bgBrush.Get());
            }

            // 3. 外侧极淡冷银分离丝（防止亮色冰白胶囊在纯白设置页面上融边，同时零黑框感）
            const float outerOff = 0.38f * s;
            const D2D1_RECT_F outerRect = D2D1::RectF(
                bgRect.left - outerOff, bgRect.top - outerOff,
                bgRect.right + outerOff, bgRect.bottom + outerOff);
            m_strokeBrush->SetColor(scaleColorAlpha(palette.outerContour, alpha));
            m_strokeBrush->SetOpacity(1.0f);
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(outerRect, cornerRadius + outerOff, cornerRadius + outerOff),
                m_strokeBrush.Get(), 0.75f * s);

            // 4. 内侧纯白镜面折射内高光环（顶部透亮、底部柔和的水滴立体光泽）
            const float innerIn = 0.62f * s;
            const D2D1_RECT_F innerRect = D2D1::RectF(
                bgRect.left + innerIn, bgRect.top + innerIn,
                bgRect.right - innerIn, bgRect.bottom - innerIn);
            const float innerR = (std::max)(1.0f, cornerRadius - innerIn);
            auto rimBrush = createVerticalLinearBrush(
                m_renderTarget.Get(), innerRect.top, innerRect.bottom,
                scaleColorAlpha(palette.innerRimTop, alpha),
                scaleColorAlpha(palette.innerRimBottom, alpha));
            if (rimBrush) {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(innerRect, innerR, innerR),
                    rimBrush.Get(), 1.15f * s);
            } else {
                m_strokeBrush->SetColor(scaleColorAlpha(palette.innerRimTop, alpha));
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(innerRect, innerR, innerR),
                    m_strokeBrush.Get(), 1.15f * s);
            }

            activeTextColor = scaleColorAlpha(palette.text, alpha);
            activeTextHaloColor = scaleColorAlpha(palette.textHalo, alpha);
            drawTextHalo = true;
        }

        const float padX = ToastStyle::HorizontalPadding * s * 0.75f;
        const float textLayoutW = (std::max)(1.0f, capW - padX * 2.0f);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                wText.c_str(),
                static_cast<UINT32>(wText.length()),
                m_textFormat.Get(),
                textLayoutW,
                capH,
                &textLayout)) && textLayout) {
            const float textOriginX = cx - textLayoutW / 2.0f;
            const float textOriginY = cy - capH / 2.0f;
            if (drawTextHalo) {
                m_textBrush->SetColor(activeTextHaloColor);
                m_textBrush->SetOpacity(1.0f);
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textOriginX, textOriginY + 0.75f * s),
                    textLayout.Get(),
                    m_textBrush.Get());
            }
            m_textBrush->SetColor(activeTextColor);
            m_textBrush->SetOpacity(1.0f);
            m_renderTarget->DrawTextLayout(
                D2D1::Point2F(textOriginX, textOriginY),
                textLayout.Get(),
                m_textBrush.Get());
        } else {
            m_textBrush->SetColor(activeTextColor);
            m_textBrush->SetOpacity(1.0f);
            m_renderTarget->DrawTextW(
                wText.c_str(), static_cast<UINT32>(wText.length()), m_textFormat.Get(),
                bgRect,
                m_textBrush.Get()
            );
        }
        m_renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    HRESULT hr = m_renderTarget->EndDraw();
    if (FAILED(hr)) {
        discardResources();
        return;
    }

    POINT ptSrc = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    
    HDC screen = GetDC(nullptr);
    if (screen) {
        UpdateLayeredWindow(
            m_hwnd, screen, nullptr, &size, m_memoryDC, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }
}

LRESULT CALLBACK ToastOverlay::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ToastOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 窗口过程兜底：任何 std 异常都不得逃逸到 Win32 派发层（否则 std::terminate 崩溃）。
    try {
        switch (msg) {
            case WM_GETOBJECT:
                return tools3000::core::accessibility::respondToOverlayUiaGetObject(
                    hwnd, wParam, lParam,
                    {L"Tools3000.Toast", L"Tools3000 notification",
                     tools3000::core::accessibility::OverlayUiaRole::Status, true});
            case WM_TIMER: {
                if (self) {
                    if (wParam == TIMER_ID) {
                        self->hideNow();
                    } else if (wParam == FADE_IN_TIMER_ID) {
                        bool done = false;
                        {
                            std::lock_guard lock(self->m_mutex);
                            self->m_opacity += 0.1f;
                            self->m_animScale = (std::min)(1.0f, self->m_animScale + 0.006f);
                            if (self->m_opacity >= 1.0f) {
                                self->m_opacity = 1.0f;
                                self->m_animScale = 1.0f;
                                self->m_fadingIn = false;
                                done = true;
                            }
                        }
                        if (done) {
                            KillTimer(hwnd, FADE_IN_TIMER_ID);
                            SetTimer(hwnd, TIMER_ID, HIDE_TIMEOUT_MS, nullptr);
                        }
                        self->render();
                    }
                }
                return 0;
            }
            case WM_APP + 0x31:
                if (self) self->hideNow();
                return 0;
            case WM_DPICHANGED:
            case WM_DISPLAYCHANGE:
                if (self && IsWindowVisible(hwnd)) {
                    self->updatePlacement();
                    self->render();
                }
                return 0;
            case WM_DESTROY:
                tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
                KillTimer(hwnd, TIMER_ID);
                KillTimer(hwnd, FADE_IN_TIMER_ID);
                return 0;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("ToastOverlay 窗口过程异常: {}", e.what());
    } catch (...) {
        LOG_ERROR("ToastOverlay 窗口过程未知异常");
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::ui
