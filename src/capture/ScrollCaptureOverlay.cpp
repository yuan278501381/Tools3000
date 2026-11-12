#include "ScrollCaptureOverlay.h"
#include "capture/CaptureHudPalette.h"
#include "capture/FloatingGlassBar.h"
#include "capture/ScrollCapture.h"
#include "capture/ShortcutHintOverlay.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include <opencv2/imgproc.hpp>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace tools3000::capture {

namespace {

constexpr const wchar_t* kOverlayClassName = L"Tools3000_ScrollCaptureOverlay";
constexpr const wchar_t* kControlClassName = L"Tools3000_ScrollCaptureControlBar";

constexpr int kHotkeyIdTogglePause = ScrollCaptureHotkeyIds::kTogglePause;
constexpr int kHotkeyIdConfirm     = ScrollCaptureHotkeyIds::kConfirm;
constexpr int kHotkeyIdPin         = ScrollCaptureHotkeyIds::kPin;
constexpr int kHotkeyIdSaveAs      = ScrollCaptureHotkeyIds::kSaveAs;
constexpr int kHotkeyIdCancel      = ScrollCaptureHotkeyIds::kCancel;
constexpr int kHotkeyIdCopy        = ScrollCaptureHotkeyIds::kCopy;

float scrollOverlayDpiScale(const RECT& rect) {
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    return tools3000::core::dpi::scaleAtPoint(center);
}

bool rectsOverlap(const RECT& a, const RECT& b, int pad = 0) {
    return (a.left - pad < b.right) && (a.right + pad > b.left) &&
           (a.top - pad < b.bottom) && (a.bottom + pad > b.top);
}

// 纯矢量绘制控制栏按钮图标（严禁任何 Unicode Emoji）
void drawControlVectorIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                           ID2D1StrokeStyle* roundStroke,
                           int btnIndex, bool isPausedState,
                           const D2D1_RECT_F& iconBox, ID2D1Brush* brush, float dpiScale) {
    const float cx = (iconBox.left + iconBox.right) * 0.5f;
    const float cy = (iconBox.top + iconBox.bottom) * 0.5f;
    const float r = (iconBox.right - iconBox.left) * 0.42f;
    const float stroke = std::max(1.5f, 1.65f * dpiScale);

    switch (btnIndex) {
        case 0: { // TogglePause: 运行时显示双竖线暂停，暂停/驻留时显示实心播放三角
            if (!isPausedState) {
                const float barW = std::max(2.0f, 2.4f * dpiScale);
                const float gap = 2.6f * dpiScale;
                const float h = r * 1.55f;
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - gap - barW, cy - h * 0.5f, cx - gap, cy + h * 0.5f),
                                      1.0f * dpiScale, 1.0f * dpiScale), brush);
                rt->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx + gap, cy - h * 0.5f, cx + gap + barW, cy + h * 0.5f),
                                      1.0f * dpiScale, 1.0f * dpiScale), brush);
            } else {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (factory && SUCCEEDED(factory->CreatePathGeometry(&geo))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(&sink))) {
                        sink->BeginFigure(D2D1::Point2F(cx - r * 0.55f, cy - r * 0.82f), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(D2D1::Point2F(cx + r * 0.85f, cy));
                        sink->AddLine(D2D1::Point2F(cx - r * 0.55f, cy + r * 0.82f));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        rt->FillGeometry(geo.Get(), brush);
                    }
                }
            }
            break;
        }
        case 1: { // Pin (图钉矢量)
            rt->DrawLine(D2D1::Point2F(cx - r * 0.65f, cy - r * 0.75f),
                         D2D1::Point2F(cx + r * 0.65f, cy - r * 0.75f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.42f, cy - r * 0.75f),
                         D2D1::Point2F(cx - r * 0.58f, cy + r * 0.12f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + r * 0.42f, cy - r * 0.75f),
                         D2D1::Point2F(cx + r * 0.58f, cy + r * 0.12f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.80f, cy + r * 0.12f),
                         D2D1::Point2F(cx + r * 0.80f, cy + r * 0.12f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx, cy + r * 0.12f),
                         D2D1::Point2F(cx, cy + r * 0.92f), brush, stroke, roundStroke);
            break;
        }
        case 2: { // SaveAs (下载归档矢量)
            rt->DrawLine(D2D1::Point2F(cx, cy - r * 0.82f),
                         D2D1::Point2F(cx, cy + r * 0.30f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.48f, cy - r * 0.15f),
                         D2D1::Point2F(cx, cy + r * 0.34f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + r * 0.48f, cy - r * 0.15f),
                         D2D1::Point2F(cx, cy + r * 0.34f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.82f, cy + r * 0.52f),
                         D2D1::Point2F(cx - r * 0.82f, cy + r * 0.82f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.82f, cy + r * 0.82f),
                         D2D1::Point2F(cx + r * 0.82f, cy + r * 0.82f), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + r * 0.82f, cy + r * 0.82f),
                         D2D1::Point2F(cx + r * 0.82f, cy + r * 0.52f), brush, stroke, roundStroke);
            break;
        }
        case 3: { // Cancel (关闭交叉线)
            const float d = r * 0.65f;
            rt->DrawLine(D2D1::Point2F(cx - d, cy - d), D2D1::Point2F(cx + d, cy + d), brush, stroke, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx + d, cy - d), D2D1::Point2F(cx - d, cy + d), brush, stroke, roundStroke);
            break;
        }
        case 4: { // Confirm (完成对勾)
            rt->DrawLine(D2D1::Point2F(cx - r * 0.72f, cy + r * 0.05f),
                         D2D1::Point2F(cx - r * 0.18f, cy + r * 0.62f), brush, stroke * 1.15f, roundStroke);
            rt->DrawLine(D2D1::Point2F(cx - r * 0.18f, cy + r * 0.62f),
                         D2D1::Point2F(cx + r * 0.78f, cy - r * 0.58f), brush, stroke * 1.15f, roundStroke);
            break;
        }
        default:
            break;
    }
}

}  // namespace

bool ScrollCaptureOverlay::initialize() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    if (!m_hwnd) {
        WNDCLASSEXW wcex = {sizeof(WNDCLASSEXW)};
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = wndProc;
        wcex.hInstance = hInst;
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = kOverlayClassName;
        RegisterClassExW(&wcex);

        const int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int screenW = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
        const int screenH = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);

        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClassName,
            L"Scroll Capture Overlay",
            WS_POPUP,
            screenX, screenY, screenW, screenH,
            nullptr, nullptr, hInst, nullptr);
        if (!m_hwnd) return false;

        SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
        // 遵循全局 Zero WDA_EXCLUDEFROMCAPTURE 契约 (WDA_NONE)，支持用户随时外部截图；
        // 防自污染通过外扩边框与多级选区外侧几何停靠实现
        tools3000::core::WinUtils::allowWindowCapture(m_hwnd);
    }

    if (!m_controlHwnd) {
        WNDCLASSEXW ctrlClass = {sizeof(WNDCLASSEXW)};
        ctrlClass.style = CS_HREDRAW | CS_VREDRAW;
        ctrlClass.lpfnWndProc = controlWndProc;
        ctrlClass.hInstance = hInst;
        ctrlClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        ctrlClass.lpszClassName = kControlClassName;
        RegisterClassExW(&ctrlClass);

        m_controlHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kControlClassName,
            L"Scroll Capture Controls",
            WS_POPUP,
            0, 0, 1, 1,
            nullptr, nullptr, hInst, nullptr);
        if (!m_controlHwnd) return false;

        SetPropW(m_controlHwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
        tools3000::core::WinUtils::allowWindowCapture(m_controlHwnd);
    }

    if (!createResources()) return false;
    return true;
}

void ScrollCaptureOverlay::applyTransientOverlayShield(bool active) {
    // 严格遵守全局 WDA_NONE 策略，仅在长截图启动时收起可能遮挡选区的瞬态提示浮层
    if (!active) return;
    if (HWND toastHwnd = FindWindowW(L"Tools3000_ToastOverlay", nullptr)) {
        if (IsWindowVisible(toastHwnd)) {
            SendMessageW(toastHwnd, WM_APP + 0x31, 0, 0);
            ShowWindow(toastHwnd, SW_HIDE);
        }
        tools3000::core::WinUtils::allowWindowCapture(toastHwnd);
    }
    if (HWND keycastHwnd = FindWindowW(L"Tools3000_KeycastOverlay", nullptr)) {
        tools3000::core::WinUtils::allowWindowCapture(keycastHwnd);
    }
    if (HWND hintHwnd = FindWindowW(L"Tools3000_ShortcutHintOverlay", nullptr)) {
        if (IsWindowVisible(hintHwnd)) {
            ShortcutHintOverlay::instance().hide();
        }
        tools3000::core::WinUtils::allowWindowCapture(hintHwnd);
    }
}

void ScrollCaptureOverlay::syncPauseHotkeyWithRuntimeState() {
    if (!m_hotkeysRegistered) return;
    auto& kh = tools3000::core::KeyboardHook::instance();
    const auto state = ScrollCapture::instance().runtimeState();
    const bool wantSpacePause = !ScrollCapture::instance().isRunning() ||
                                (state == ScrollRuntimeState::AutoScrolling) ||
                                (state == ScrollRuntimeState::Idle);
    if (wantSpacePause && !m_spaceHotkeyRegistered) {
        kh.registerHookHotkey(kHotkeyIdTogglePause, {tools3000::core::ModKey::None, VK_SPACE},
                              "ScrollCapture.TogglePause", []() {
                                  if (ScrollCapture::instance().isRunning()) {
                                      ScrollCapture::instance().pause();
                                      ScrollCaptureOverlay::instance().syncPauseHotkeyWithRuntimeState();
                                      ScrollCaptureOverlay::instance().renderControlBar();
                                  }
                              });
        m_spaceHotkeyRegistered = true;
    } else if (!wantSpacePause && m_spaceHotkeyRegistered) {
        kh.unregisterHookHotkey(kHotkeyIdTogglePause);
        m_spaceHotkeyRegistered = false;
    }
}

void ScrollCaptureOverlay::registerTransientHotkeys() {
    if (m_hotkeysRegistered) return;
    auto& kh = tools3000::core::KeyboardHook::instance();
    kh.install();

    const auto specs = transientHotkeySpecs(true);
    for (const auto& spec : specs) {
        const tools3000::core::HotkeyDef def{
            static_cast<tools3000::core::ModKey>(spec.modifiers),
            spec.vk
        };
        if (spec.isTogglePause) {
            kh.registerHookHotkey(spec.id, def, spec.name, []() {
                if (ScrollCapture::instance().isRunning()) {
                    ScrollCapture::instance().pause();
                    ScrollCaptureOverlay::instance().syncPauseHotkeyWithRuntimeState();
                    ScrollCaptureOverlay::instance().renderControlBar();
                }
            });
            m_spaceHotkeyRegistered = true;
        } else if (spec.finishAction == ScrollFinishAction::Cancelled) {
            kh.registerHookHotkey(spec.id, def, spec.name, []() {
                if (ScrollCapture::instance().isRunning()) {
                    ScrollCapture::instance().requestCancel();
                }
            });
        } else {
            const ScrollFinishAction action = spec.finishAction;
            kh.registerHookHotkey(spec.id, def, spec.name, [action]() {
                if (ScrollCapture::instance().isRunning()) {
                    ScrollCapture::instance().requestFinish(action);
                }
            });
        }
    }
    m_hotkeysRegistered = true;
    syncPauseHotkeyWithRuntimeState();
}

void ScrollCaptureOverlay::unregisterTransientHotkeys() {
    if (!m_hotkeysRegistered) return;
    auto& kh = tools3000::core::KeyboardHook::instance();
    kh.unregisterHookHotkey(kHotkeyIdTogglePause);
    kh.unregisterHookHotkey(kHotkeyIdConfirm);
    kh.unregisterHookHotkey(kHotkeyIdPin);
    kh.unregisterHookHotkey(kHotkeyIdSaveAs);
    kh.unregisterHookHotkey(kHotkeyIdCopy);
    kh.unregisterHookHotkey(kHotkeyIdCancel);
    m_spaceHotkeyRegistered = false;
    m_hotkeysRegistered = false;
}

void ScrollCaptureOverlay::releaseLayeredDib(LayeredDibCache& cache) noexcept {
    if (cache.memDc && cache.oldBmp) {
        SelectObject(cache.memDc, cache.oldBmp);
        cache.oldBmp = nullptr;
    }
    if (cache.dib) {
        DeleteObject(cache.dib);
        cache.dib = nullptr;
    }
    cache.bits = nullptr;
    if (cache.memDc) {
        DeleteDC(cache.memDc);
        cache.memDc = nullptr;
    }
    cache.width = 0;
    cache.height = 0;
}

bool ScrollCaptureOverlay::ensureLayeredDib(HDC screenDc, int width, int height, LayeredDibCache& cache) noexcept {
    if (!screenDc || width <= 0 || height <= 0 || width > 32768 || height > 32768) {
        return false;
    }
    const uint64_t totalBytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
    if (totalBytes == 0 || totalBytes > (512ULL * 1024ULL * 1024ULL)) {
        return false;
    }

    if (cache.memDc && cache.dib && cache.bits && cache.width == width && cache.height == height) {
        std::memset(cache.bits, 0, static_cast<std::size_t>(totalBytes));
        return true;
    }

    releaseLayeredDib(cache);

    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDc);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, dib);
    if (!oldBmp || oldBmp == HGDI_ERROR) {
        DeleteObject(dib);
        DeleteDC(memDc);
        return false;
    }

    cache.memDc = memDc;
    cache.dib = dib;
    cache.oldBmp = oldBmp;
    cache.bits = bits;
    cache.width = width;
    cache.height = height;
    return true;
}

void ScrollCaptureOverlay::handleMainTargetDeviceLost() noexcept {
    m_d2dBitmap.Reset();
    m_brushFlash.Reset();
    m_brushBorder.Reset();
    m_brushBg.Reset();
    m_renderTarget.Reset();
    m_previewDirty = true;
}

void ScrollCaptureOverlay::handleControlTargetDeviceLost() noexcept {
    m_ctrlPanelBg.Reset();
    m_ctrlPanelBorder.Reset();
    m_ctrlPanelInnerHighlight.Reset();
    m_ctrlDividerBrush.Reset();
    m_ctrlTextPrimary.Reset();
    m_ctrlTextSecondary.Reset();
    m_ctrlTextOnPrimary.Reset();
    m_ctrlDotBrush.Reset();
    m_ctrlBtnHoverBg.Reset();
    m_ctrlBtnPressBg.Reset();
    m_ctrlKeycapBg.Reset();
    m_ctrlKeycapPrimaryBg.Reset();
    m_ctrlAccentBg.Reset();
    m_ctrlAccentHoverBg.Reset();
    m_ctrlDangerHoverBg.Reset();
    m_ctrlDangerIconBrush.Reset();
    m_controlRenderTarget.Reset();
    m_controlBarDirty = true;
}

void ScrollCaptureOverlay::hide() {
    if (m_overlayVisibleFlag) {
        m_overlayVisibleFlag = false;
        tools3000::core::KeyboardHook::instance().setScrollCaptureOverlayVisible(false);
    }
    unregisterTransientHotkeys();
    applyTransientOverlayShield(false);

    if (m_controlHwnd) {
        DestroyWindow(m_controlHwnd);
        m_controlHwnd = nullptr;
    }
    if (m_hwnd) {
        KillTimer(m_hwnd, 1);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(m_controlRectMutex);
        m_controlScreenRect = {};
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_preview.release();
        m_previewDirty = false;
        m_d2dBitmap.Reset();
        m_frameCount = 0;
    }
    m_hoveredBtn = ControlButtonId::None;
    m_pressedBtn = ControlButtonId::None;
    m_trackingMouseLeave = false;
    m_layoutCacheValid = false;
    m_controlBarDirty = true;

    handleControlTargetDeviceLost();
    m_roundStrokeStyle.Reset();

    handleMainTargetDeviceLost();
    m_statusFormat.Reset();
    m_metricsFormat.Reset();
    m_btnLabelFormat.Reset();
    m_keycapFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();

    releaseLayeredDib(m_controlDibCache);
    releaseLayeredDib(m_overlayDibCache);
}

void ScrollCaptureOverlay::reloadTheme() {
    if (!m_hwnd && !m_controlHwnd) return;
    if (!createResources()) return;
    m_controlBarDirty = true;
    renderControlBar(true);
    render();
}

bool ScrollCaptureOverlay::createResources() {
    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    if (!m_roundStrokeStyle) {
        D2D1_STROKE_STYLE_PROPERTIES ssp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND);
        m_d2dFactory->CreateStrokeStyle(ssp, nullptr, 0, &m_roundStrokeStyle);
    }

    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT);

    if (!m_renderTarget) {
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget))) return false;
    }
    if (!m_controlRenderTarget) {
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_controlRenderTarget))) return false;
    }

    const auto pal = FloatingGlassBar::resolveThemePalette();
    m_brushFlash.Reset();
    m_brushBorder.Reset();
    m_brushBg.Reset();
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brushFlash);
    m_renderTarget->CreateSolidColorBrush(pal.accent, &m_brushBorder);
    m_renderTarget->CreateSolidColorBrush(pal.panelBg, &m_brushBg);

    m_ctrlPanelBg.Reset();
    m_ctrlPanelBorder.Reset();
    m_ctrlPanelInnerHighlight.Reset();
    m_ctrlDividerBrush.Reset();
    m_ctrlTextPrimary.Reset();
    m_ctrlTextSecondary.Reset();
    m_ctrlTextOnPrimary.Reset();
    m_ctrlDotBrush.Reset();
    m_ctrlBtnHoverBg.Reset();
    m_ctrlBtnPressBg.Reset();
    m_ctrlKeycapBg.Reset();
    m_ctrlKeycapPrimaryBg.Reset();
    m_ctrlAccentBg.Reset();
    m_ctrlAccentHoverBg.Reset();
    m_ctrlDangerHoverBg.Reset();
    m_ctrlDangerIconBrush.Reset();

    const float accentLum = 0.2126f * pal.accent.r + 0.7152f * pal.accent.g + 0.0722f * pal.accent.b;
    const D2D1_COLOR_F onPrimary = accentLum > 0.68f
        ? D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 1.0f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F accentHover = D2D1::ColorF(
        std::clamp(pal.accent.r + (pal.isDark ? 0.06f : -0.05f), 0.0f, 1.0f),
        std::clamp(pal.accent.g + (pal.isDark ? 0.06f : -0.05f), 0.0f, 1.0f),
        std::clamp(pal.accent.b + (pal.isDark ? 0.06f : -0.05f), 0.0f, 1.0f),
        1.0f);

    m_controlRenderTarget->CreateSolidColorBrush(pal.panelBg, &m_ctrlPanelBg);
    m_controlRenderTarget->CreateSolidColorBrush(pal.panelOuterBorder, &m_ctrlPanelBorder);
    m_controlRenderTarget->CreateSolidColorBrush(pal.panelInnerHighlight, &m_ctrlPanelInnerHighlight);
    m_controlRenderTarget->CreateSolidColorBrush(pal.divider, &m_ctrlDividerBrush);
    m_controlRenderTarget->CreateSolidColorBrush(pal.textPrimary, &m_ctrlTextPrimary);
    m_controlRenderTarget->CreateSolidColorBrush(pal.textSecondary, &m_ctrlTextSecondary);
    m_controlRenderTarget->CreateSolidColorBrush(onPrimary, &m_ctrlTextOnPrimary);
    m_controlRenderTarget->CreateSolidColorBrush(pal.hoverBg, &m_ctrlBtnHoverBg);
    m_controlRenderTarget->CreateSolidColorBrush(pal.pressBg, &m_ctrlBtnPressBg);
    m_controlRenderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : CaptureHudPalette::kChipAccentBg, &m_ctrlKeycapBg);
    m_controlRenderTarget->CreateSolidColorBrush(
        accentLum > 0.68f ? D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.14f)
                          : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f),
        &m_ctrlKeycapPrimaryBg);
    m_controlRenderTarget->CreateSolidColorBrush(pal.accent, &m_ctrlAccentBg);
    m_controlRenderTarget->CreateSolidColorBrush(accentHover, &m_ctrlAccentHoverBg);
    m_controlRenderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.94f, 0.26f, 0.26f, pal.isDark ? 0.18f : 0.12f), &m_ctrlDangerHoverBg);
    m_controlRenderTarget->CreateSolidColorBrush(
        pal.isDark ? D2D1::ColorF(1.0f, 0.42f, 0.46f, 1.0f) : D2D1::ColorF(0.90f, 0.22f, 0.26f, 1.0f),
        &m_ctrlDangerIconBrush);
    m_controlRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.13f, 0.80f, 0.42f, 1.0f), &m_ctrlDotBrush);

    m_statusFormat.Reset();
    m_metricsFormat.Reset();
    m_btnLabelFormat.Reset();
    m_keycapFormat.Reset();
    m_layoutCacheValid = false;
    m_controlBarDirty = true;

    const float statusFontPx  = (std::max)(12.0f, 12.6f * m_dpiScale);
    const float metricsFontPx = (std::max)(12.0f, 12.0f * m_dpiScale);
    const float btnFontPx     = (std::max)(12.0f, 12.4f * m_dpiScale);
    const float keycapFontPx  = (std::max)(12.0f, 12.0f * m_dpiScale);

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = m_isChinese ? L"zh-CN" : L"en-US";

    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            statusFontPx, localeName, &m_statusFormat))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            metricsFontPx, localeName, &m_metricsFormat))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            btnFontPx, localeName, &m_btnLabelFormat))) return false;

    if (FAILED(m_dwriteFactory->CreateTextFormat(
            monoFont, nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            keycapFontPx, L"en-US", &m_keycapFormat))) return false;

    m_statusFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_statusFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_statusFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_metricsFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_metricsFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_metricsFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    Microsoft::WRL::ComPtr<IDWriteInlineObject> statusEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_statusFormat.Get(), &statusEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        m_statusFormat->SetTrimming(&trimming, statusEllipsis.Get());
    }
    Microsoft::WRL::ComPtr<IDWriteInlineObject> metricsEllipsis;
    if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_metricsFormat.Get(), &metricsEllipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        m_metricsFormat->SetTrimming(&trimming, metricsEllipsis.Get());
    }

    m_btnLabelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_keycapFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    m_btnLabelFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_btnLabelFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_keycapFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_keycapFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    return true;
}

float ScrollCaptureOverlay::measureTextWidth(const std::wstring& text, IDWriteTextFormat* fmt) const {
    if (text.empty() || !m_dwriteFactory || !fmt) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), fmt, 4096.0f, 256.0f, &layout)) || !layout) {
        return static_cast<float>(text.size()) * 13.0f * m_dpiScale;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return static_cast<float>(text.size()) * 13.0f * m_dpiScale;
    }
    return std::ceil(metrics.widthIncludingTrailingWhitespace);
}

ScrollControlBarLayoutSnapshot ScrollCaptureOverlay::rebuildLayout(
    int workWidth, const std::wstring& statusText, const std::wstring& metricsStr, bool isPausedOrHold) {
    if (m_layoutCacheValid &&
        m_cachedLayoutWorkWidth == workWidth &&
        std::abs(m_cachedLayoutScale - m_dpiScale) <= 0.001f &&
        m_cachedLayoutChinese == m_isChinese &&
        m_cachedLayoutPausedOrHold == isPausedOrHold &&
        m_cachedLayoutStatus == statusText &&
        m_cachedLayoutMetrics == metricsStr) {
        return m_cachedLayoutSnap;
    }

    const auto snap = buildControlBarLayout(
        m_dpiScale, m_isChinese, isPausedOrHold, workWidth,
        m_dwriteFactory.Get(), m_statusFormat.Get(), m_metricsFormat.Get(),
        m_btnLabelFormat.Get(), m_keycapFormat.Get(),
        statusText, metricsStr);

    static constexpr std::array<ControlButtonId, 5> kButtonIds = {
        ControlButtonId::TogglePause,
        ControlButtonId::Pin,
        ControlButtonId::SaveAs,
        ControlButtonId::Cancel,
        ControlButtonId::Confirm
    };
    for (std::size_t i = 0; i < kButtonIds.size(); ++i) {
        m_buttonRects[i] = {kButtonIds[i], snap.buttonRects[i]};
    }
    m_cachedLayoutSnap = snap;
    m_cachedLayoutWorkWidth = workWidth;
    m_cachedLayoutScale = m_dpiScale;
    m_cachedLayoutChinese = m_isChinese;
    m_cachedLayoutPausedOrHold = isPausedOrHold;
    m_cachedLayoutStatus = statusText;
    m_cachedLayoutMetrics = metricsStr;
    m_layoutCacheValid = true;
    return snap;
}

ScrollControlBarLayoutSnapshot ScrollCaptureOverlay::computeControlBarLayoutForTest(
    float dpiScale, bool isChinese, bool isPaused, int workWidth) {
    const float prevScale = m_dpiScale;
    const bool prevZh = m_isChinese;
    m_dpiScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
    m_isChinese = isChinese;
    createResources();

    const auto snap = buildControlBarLayout(
        m_dpiScale, m_isChinese, isPaused, workWidth,
        m_dwriteFactory.Get(), m_statusFormat.Get(), m_metricsFormat.Get(),
        m_btnLabelFormat.Get(), m_keycapFormat.Get());

    m_dpiScale = prevScale;
    m_isChinese = prevZh;
    if (m_hwnd || m_controlHwnd) {
        createResources();
    }
    return snap;
}

void ScrollCaptureOverlay::show(RECT captureRect) {
    m_captureRect = captureRect;
    m_dpiScale = scrollOverlayDpiScale(captureRect);
    m_isChinese = tools3000::core::isUiLanguageChinese();

    // 若上一次会话窗口仍在，先彻底重置以重新绑定当前显示器 DPI 资源
    if (m_hwnd || m_controlHwnd) {
        hide();
    }
    if (!initialize()) return;

    applyTransientOverlayShield(true);
    registerTransientHotkeys();

    m_lastThumbScreenRect = {};
    m_hasThumbRect = false;
    m_desiredBarWidth = 0;
    m_desiredBarHeight = 0;
    m_layoutCacheValid = false;
    m_controlBarDirty = true;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_preview.release();
        m_previewDirty = false;
        m_d2dBitmap.Reset();
        m_frameCount = 0;
        m_lastFlashTime = GetTickCount64();
    }

    renderControlBar(true);
    updateControlPlacement(nullptr);

    ShowWindow(m_hwnd, SW_SHOWNA);
    ShowWindow(m_controlHwnd, SW_SHOWNA);
    m_overlayVisibleFlag = true;
    tools3000::core::KeyboardHook::instance().setScrollCaptureOverlayVisible(true);

    SetTimer(m_hwnd, 1, 33, nullptr);
    render();
}

bool ScrollCaptureOverlay::isPointInControlZone(POINT screenPt) const {
    RECT ctrl{};
    {
        std::lock_guard<std::mutex> lock(m_controlRectMutex);
        ctrl = m_controlScreenRect;
    }
    if (ctrl.right <= ctrl.left || ctrl.bottom <= ctrl.top) return false;
    const int pad = static_cast<int>(std::lround(28.0f * m_dpiScale));
    return screenPt.x >= ctrl.left - pad && screenPt.x <= ctrl.right + pad &&
           screenPt.y >= ctrl.top - pad && screenPt.y <= ctrl.bottom + pad;
}

void ScrollCaptureOverlay::updateControlPlacement(const RECT* thumbScreenRect) {
    if (!m_controlHwnd) return;

    const POINT center{(m_captureRect.left + m_captureRect.right) / 2,
                       (m_captureRect.top + m_captureRect.bottom) / 2};
    const HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    const RECT work = tools3000::core::dpi::workArea(mon);

    MONITORINFO mi{sizeof(MONITORINFO)};
    RECT monRect = work;
    if (mon && GetMonitorInfoW(mon, &mi)) {
        monRect = mi.rcMonitor;
    }

    const RECT virtRect{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };

    const int barW = m_desiredBarWidth > 0
        ? m_desiredBarWidth
        : static_cast<int>(std::lround(720.0f * m_dpiScale));
    const int barH = m_desiredBarHeight > 0
        ? m_desiredBarHeight
        : static_cast<int>(std::lround(54.0f * m_dpiScale));
    const int gap  = static_cast<int>(std::lround(12.0f * m_dpiScale));
    const int edge = static_cast<int>(std::lround(10.0f * m_dpiScale));
    const int tightEdge = static_cast<int>(std::lround(4.0f * m_dpiScale));

    auto clampToBounds = [&](const RECT& bounds, int inset, int x, int y) -> RECT {
        const int cx = std::clamp(x, static_cast<int>(bounds.left) + inset,
                                  std::max(static_cast<int>(bounds.left) + inset, static_cast<int>(bounds.right) - barW - inset));
        const int cy = std::clamp(y, static_cast<int>(bounds.top) + inset,
                                  std::max(static_cast<int>(bounds.top) + inset, static_cast<int>(bounds.bottom) - barH - inset));
        return RECT{cx, cy, cx + barW, cy + barH};
    };

    const std::array<RECT, 10> candidates = {
        // 1~5. 当前屏幕工作区外侧停靠（最符合截图工具栏肌肉记忆）
        clampToBounds(work, edge, m_captureRect.right - barW, m_captureRect.bottom + gap),
        clampToBounds(work, edge, m_captureRect.right - barW, m_captureRect.top - barH - gap),
        clampToBounds(work, edge, m_captureRect.left, m_captureRect.bottom + gap),
        clampToBounds(work, edge, m_captureRect.right + gap, m_captureRect.bottom - barH),
        clampToBounds(work, edge, m_captureRect.left - barW - gap, m_captureRect.bottom - barH),
        // 6~7. 最大化窗口选区场景：允许停靠在物理显示器边缘（如任务栏上方/内部），严防侵入选区像素
        clampToBounds(monRect, tightEdge, m_captureRect.right - barW, m_captureRect.bottom + tightEdge),
        clampToBounds(monRect, tightEdge, m_captureRect.right - barW, m_captureRect.top - barH - tightEdge),
        // 8~9. 多屏全屏框选场景：允许停靠在相邻副屏紧贴选区外侧
        clampToBounds(virtRect, edge, m_captureRect.right + gap, m_captureRect.bottom - barH),
        clampToBounds(virtRect, edge, m_captureRect.left - barW - gap, m_captureRect.bottom - barH),
        // 10. 单屏 100% 物理全屏极端兜底：停靠于工作区右下角
        clampToBounds(work, edge, work.right - barW - edge, work.bottom - barH - edge)
    };

    RECT chosen = candidates.back();
    const int pad = static_cast<int>(std::lround(3.0f * m_dpiScale));
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const RECT& c = candidates[i];
        const bool hitsCapture = rectsOverlap(c, m_captureRect, pad);
        const bool hitsThumb = (thumbScreenRect != nullptr) && rectsOverlap(c, *thumbScreenRect, pad);
        if (!hitsCapture && !hitsThumb) {
            chosen = c;
            break;
        }
    }

    RECT current{};
    {
        std::lock_guard<std::mutex> lock(m_controlRectMutex);
        current = m_controlScreenRect;
        m_controlScreenRect = chosen;
    }

    if (current.left != chosen.left || current.top != chosen.top ||
        current.right != chosen.right || current.bottom != chosen.bottom) {
        SetWindowPos(m_controlHwnd, HWND_TOPMOST,
                     chosen.left, chosen.top,
                     chosen.right - chosen.left, chosen.bottom - chosen.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_controlBarDirty = true;
    }
}

void ScrollCaptureOverlay::updatePreview(const cv::Mat& stitched, int frameCount) {
    if (!m_hwnd) return;
    cv::Mat previewBgra;
    if (!stitched.empty()) {
        const int targetWidth = std::max(1, static_cast<int>(std::lround(210.0f * m_dpiScale)));
        const int maxHeight = std::max(
            static_cast<int>(std::lround(140.0f * m_dpiScale)), std::min(
            static_cast<int>(std::lround(500.0f * m_dpiScale)),
            GetSystemMetrics(SM_CYVIRTUALSCREEN) -
                static_cast<int>(std::lround(100.0f * m_dpiScale))));
        const int sourceRows = std::max(1, maxHeight * stitched.cols / targetWidth);
        const int rows = std::min(stitched.rows, sourceRows);
        const cv::Mat recent = stitched(cv::Rect(0, stitched.rows - rows, stitched.cols, rows));
        const double scale = std::min(
            static_cast<double>(targetWidth) / recent.cols,
            static_cast<double>(maxHeight) / recent.rows);
        cv::Mat resized;
        cv::resize(recent, resized, cv::Size(
            std::max(1, static_cast<int>(recent.cols * scale)),
            std::max(1, static_cast<int>(recent.rows * scale))), 0, 0, cv::INTER_AREA);
        cv::cvtColor(resized, previewBgra, cv::COLOR_BGR2BGRA);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_preview = std::move(previewBgra);
        m_frameCount = frameCount;
        m_lastFlashTime = GetTickCount64();
        m_previewDirty = true;
    }
}

void ScrollCaptureOverlay::render() {
    if (!m_hwnd) return;
    if (!m_renderTarget && !createResources()) return;

    syncPauseHotkeyWithRuntimeState();

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    if (!ensureLayeredDib(hdcScreen, width, height, m_overlayDibCache)) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    RECT bindRect = {0, 0, width, height};
    if (FAILED(m_renderTarget->BindDC(m_overlayDibCache.memDc, &bindRect))) {
        handleMainTargetDeviceLost();
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const auto runtimeState = ScrollCapture::instance().runtimeState();
    const uint64_t nowOverlayTick = GetTickCount64();
    const uint64_t tooFastOverlayTick = ScrollCapture::instance().lastScrollTooFastTickMs();
    const bool overlayScrollTooFast =
        (tooFastOverlayTick > 0 && nowOverlayTick >= tooFastOverlayTick && (nowOverlayTick - tooFastOverlayTick) < 1800);
    if (overlayScrollTooFast || runtimeState == ScrollRuntimeState::HoldAtBottom) {
        m_brushBorder->SetColor(D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f)); // 琥珀金预警 / 触底驻留
    } else if (runtimeState == ScrollRuntimeState::PausedManual) {
        m_brushBorder->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 1.0f)); // 手动接管天青蓝
    } else {
        m_brushBorder->SetColor(D2D1::ColorF(0.13f, 0.77f, 0.37f, 1.0f)); // 自动滚动翡翠绿
    }

    const uint64_t elapsed = GetTickCount64() - m_lastFlashTime;
    float flashOpacity = 0.0f;
    if (elapsed < 260) {
        flashOpacity = 1.0f - (static_cast<float>(elapsed) / 260.0f);
    }

    const int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);

    const D2D1_RECT_F cRect = D2D1::RectF(
        static_cast<float>(m_captureRect.left - screenX),
        static_cast<float>(m_captureRect.top - screenY),
        static_cast<float>(m_captureRect.right - screenX),
        static_cast<float>(m_captureRect.bottom - screenY));

    // 外扩绘制指示边框，绝不侵入 m_captureRect 内部像素
    const float borderStroke = 2.5f * m_dpiScale;
    const float borderOutset = borderStroke * 0.5f + 2.0f * m_dpiScale;
    const D2D1_RECT_F borderRect = D2D1::RectF(
        cRect.left - borderOutset,
        cRect.top - borderOutset,
        cRect.right + borderOutset,
        cRect.bottom + borderOutset);
    m_renderTarget->DrawRectangle(&borderRect, m_brushBorder.Get(), borderStroke);

    if (flashOpacity > 0.01f) {
        const float flashStroke = 4.5f * m_dpiScale;
        const float flashOutset = borderOutset + borderStroke * 0.5f + flashStroke * 0.5f;
        const D2D1_RECT_F flashRect = D2D1::RectF(
            cRect.left - flashOutset,
            cRect.top - flashOutset,
            cRect.right + flashOutset,
            cRect.bottom + flashOutset);
        m_brushFlash->SetOpacity(flashOpacity * 0.55f);
        m_renderTarget->DrawRectangle(&flashRect, m_brushFlash.Get(), flashStroke);
    }

    RECT thumbScreenRect{};
    bool hasThumbRect = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_preview.empty() && m_preview.cols > 0) {
            if (m_previewDirty) {
                m_d2dBitmap.Reset();
                m_previewDirty = false;
            }
            if (!m_d2dBitmap) {
                D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                m_renderTarget->CreateBitmap(
                    D2D1::SizeU(m_preview.cols, m_preview.rows),
                    m_preview.data,
                    static_cast<UINT32>(m_preview.step),
                    &props,
                    &m_d2dBitmap);
            }

            if (m_d2dBitmap) {
                const float margin = 12.0f * m_dpiScale;
                const float thumbWidth = 200.0f * m_dpiScale;
                const float scale = thumbWidth / m_preview.cols;
                const float thumbHeight = (std::min)(static_cast<float>(height) - margin * 2.0f, m_preview.rows * scale);

                const float rightCandidateX = cRect.right + 18.0f * m_dpiScale;
                const float leftCandidateX = cRect.left - thumbWidth - 18.0f * m_dpiScale;
                const bool rightFits = (rightCandidateX + thumbWidth + margin <= static_cast<float>(width));
                const bool leftFits = (leftCandidateX >= margin);

                float thumbX = rightFits ? rightCandidateX : (leftFits ? leftCandidateX : rightCandidateX);
                const float thumbY = std::clamp(
                    cRect.top, margin,
                    (std::max)(margin, static_cast<float>(height) - thumbHeight - margin));
                thumbX = std::clamp(
                    thumbX, margin,
                    (std::max)(margin, static_cast<float>(width) - thumbWidth - margin));

                const float padFrame = 6.0f * m_dpiScale;
                const bool overlapsCaptureRect =
                    (thumbX - padFrame < cRect.right && thumbX + thumbWidth + padFrame > cRect.left &&
                     thumbY - padFrame < cRect.bottom && thumbY + thumbHeight + padFrame > cRect.top);

                if (!overlapsCaptureRect) {
                    const D2D1_RECT_F tRect = D2D1::RectF(thumbX, thumbY, thumbX + thumbWidth, thumbY + thumbHeight);
                    const D2D1_ROUNDED_RECT rRect = D2D1::RoundedRect(
                        D2D1::RectF(tRect.left - 6.0f * m_dpiScale,
                                    tRect.top - 6.0f * m_dpiScale,
                                    tRect.right + 6.0f * m_dpiScale,
                                    tRect.bottom + 6.0f * m_dpiScale),
                        9.0f * m_dpiScale, 9.0f * m_dpiScale);
                    m_renderTarget->FillRoundedRectangle(&rRect, m_brushBg.Get());
                    m_renderTarget->DrawBitmap(m_d2dBitmap.Get(), &tRect);
                    m_renderTarget->DrawRoundedRectangle(&rRect, m_brushBorder.Get(), 1.6f * m_dpiScale);

                    thumbScreenRect = RECT{
                        static_cast<LONG>(rRect.rect.left) + screenX,
                        static_cast<LONG>(rRect.rect.top) + screenY,
                        static_cast<LONG>(rRect.rect.right) + screenX,
                        static_cast<LONG>(rRect.rect.bottom) + screenY};
                    hasThumbRect = true;
                }
            }
        }
    }

    const HRESULT endHr = m_renderTarget->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET || endHr == DXGI_ERROR_DEVICE_REMOVED || endHr == DXGI_ERROR_DEVICE_RESET) {
        handleMainTargetDeviceLost();
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    if (SUCCEEDED(endHr)) {
        POINT ptSrc = {0, 0};
        SIZE size = {width, height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(m_hwnd, hdcScreen, nullptr, &size, m_overlayDibCache.memDc, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    ReleaseDC(nullptr, hdcScreen);

    m_lastThumbScreenRect = thumbScreenRect;
    m_hasThumbRect = hasThumbRect;
    updateControlPlacement(hasThumbRect ? &thumbScreenRect : nullptr);
    renderControlBar(false);
}

void ScrollCaptureOverlay::renderControlBar(bool forceRedraw) {
    if (!m_controlHwnd) return;
    if (!m_controlRenderTarget && !createResources()) return;

    const float s = m_dpiScale;
    const auto state = ScrollCapture::instance().runtimeState();
    const bool isPausedOrHold = (state == ScrollRuntimeState::PausedManual || state == ScrollRuntimeState::HoldAtBottom);
    const SIZE dims = ScrollCapture::instance().stitchedDimensions();
    const int frames = ScrollCapture::instance().currentFrameCount();

    D2D1_COLOR_F dotColor = D2D1::ColorF(0.13f, 0.80f, 0.42f, 1.0f);
    int pulseStep = 20;
    std::wstring statusText;
    const uint64_t nowBarTick = GetTickCount64();
    const uint64_t tooFastTick = ScrollCapture::instance().lastScrollTooFastTickMs();
    const bool isScrollTooFast =
        (tooFastTick > 0 && nowBarTick >= tooFastTick && (nowBarTick - tooFastTick) < 1800);
    if (isScrollTooFast) {
        dotColor = D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f);
        statusText = m_isChinese ? L"滚动过快 · 请放慢滚轮" : L"Scroll slower · Overlap lost";
    } else if (state == ScrollRuntimeState::HoldAtBottom) {
        dotColor = D2D1::ColorF(0.98f, 0.65f, 0.10f, 1.0f);
        statusText = m_isChinese ? L"已停滚 · 回车裁切/完成" : L"Stopped · Enter to trim";
    } else if (state == ScrollRuntimeState::PausedManual) {
        dotColor = D2D1::ColorF(0.24f, 0.75f, 0.98f, 1.0f);
        statusText = m_isChinese ? L"已暂停 · 回车裁切/完成" : L"Paused · Enter to trim";
    } else {
        const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(nowBarTick % 1200) / 1200.0f * 6.28318f);
        pulseStep = static_cast<int>(std::lround(pulse * 12.0f));
        dotColor = D2D1::ColorF(0.13f, 0.82f, 0.44f, static_cast<float>(pulseStep) / 12.0f);
        statusText = m_isChinese ? L"正在自动滚动采集" : L"Auto scrolling";
    }

    wchar_t metricsBuf[96]{};
    if (dims.cx > 0 && dims.cy > 0) {
        const wchar_t* fmt = m_isChinese
            ? L"%d 帧 · %ld × %ld px"
            : (frames == 1 ? L"%d frame · %ld × %ld px" : L"%d frames · %ld × %ld px");
        std::swprintf(metricsBuf, 96, fmt, frames, dims.cx, dims.cy);
    } else {
        std::swprintf(metricsBuf, 96, m_isChinese ? L"准备采集中..." : L"Preparing...");
    }
    const std::wstring metricsStr(metricsBuf);

    // 1. 基于当前显示器可用宽度与 DirectWrite 实时测量构建自适应布局（超宽时自动两级紧凑降级）
    const POINT capCenter{(m_captureRect.left + m_captureRect.right) / 2,
                          (m_captureRect.top + m_captureRect.bottom) / 2};
    const HMONITOR mon = MonitorFromPoint(capCenter, MONITOR_DEFAULTTONEAREST);
    const RECT work = tools3000::core::dpi::workArea(mon);
    const int workWidth = static_cast<int>(work.right - work.left);

    const auto snap = rebuildLayout(workWidth, statusText, metricsStr, isPausedOrHold);
    const int desiredW = snap.barWidth;
    const int desiredH = snap.barHeight;

    if (m_desiredBarWidth != desiredW || m_desiredBarHeight != desiredH) {
        m_desiredBarWidth = desiredW;
        m_desiredBarHeight = desiredH;
        updateControlPlacement(m_hasThumbRect ? &m_lastThumbScreenRect : nullptr);
        m_controlBarDirty = true;
    }

    if (!forceRedraw && !m_controlBarDirty &&
        m_lastRenderedState == state &&
        m_lastRenderedWorkWidth == workWidth &&
        m_lastRenderedPulseStep == pulseStep &&
        m_lastRenderedHoveredBtn == m_hoveredBtn &&
        m_lastRenderedPressedBtn == m_pressedBtn &&
        m_lastRenderedStatus == statusText &&
        m_lastRenderedMetrics == metricsStr) {
        return;
    }

    const float leftX = 14.0f * s;
    const float dotR = 4.2f * s;
    const float dotGap = 7.0f * s;
    const float dividerX = snap.dividerX;
    const float padL = 9.5f * s;
    const float iconSize = 15.0f * s;
    const float gapIconLabel = 5.5f * s;
    const float gapLabelKey = 7.5f * s;
    const float gapIconKey = 6.5f * s;

    RECT rc{};
    GetClientRect(m_controlHwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    if (!ensureLayeredDib(hdcScreen, width, height, m_controlDibCache)) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    RECT bindRect = {0, 0, width, height};
    if (FAILED(m_controlRenderTarget->BindDC(m_controlDibCache.memDc, &bindRect))) {
        handleControlTargetDeviceLost();
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    m_controlRenderTarget->BeginDraw();
    m_controlRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float outerPad = 1.5f * s;
    const float radius = 11.5f * s;

    if (m_ctrlDotBrush) {
        m_ctrlDotBrush->SetColor(dotColor);
    }

    const D2D1_ROUNDED_RECT shell = D2D1::RoundedRect(
        D2D1::RectF(outerPad, outerPad, w - outerPad, h - outerPad), radius, radius);
    if (m_ctrlPanelBg) {
        m_controlRenderTarget->FillRoundedRectangle(&shell, m_ctrlPanelBg.Get());
    }
    if (m_ctrlPanelInnerHighlight) {
        const float inset = 1.0f * s;
        const D2D1_ROUNDED_RECT innerShell = D2D1::RoundedRect(
            D2D1::RectF(outerPad + inset, outerPad + inset, w - outerPad - inset, h - outerPad - inset),
            (std::max)(1.0f, radius - inset), (std::max)(1.0f, radius - inset));
        m_controlRenderTarget->DrawRoundedRectangle(&innerShell, m_ctrlPanelInnerHighlight.Get(), 0.9f * s);
    }
    if (m_ctrlPanelBorder) {
        m_controlRenderTarget->DrawRoundedRectangle(&shell, m_ctrlPanelBorder.Get(), std::max(1.0f, 1.0f * s));
    }

    // 左侧状态与实时长图尺寸遥测区（上下双行垂直居中对齐）
    const float topRowTop = 6.0f * s;
    const float topRowBot = h * 0.52f;
    const float botRowTop = h * 0.50f;
    const float botRowBot = h - 5.0f * s;

    if (m_ctrlDotBrush) {
        m_controlRenderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(leftX + dotR, (topRowTop + topRowBot) * 0.5f), dotR, dotR),
            m_ctrlDotBrush.Get());
    }

    const D2D1_RECT_F statusRect = D2D1::RectF(
        leftX + dotR * 2.0f + dotGap, topRowTop,
        (std::max)(leftX + dotR * 2.0f + dotGap, dividerX - 8.0f * s), topRowBot);
    if (m_ctrlTextPrimary) {
        m_controlRenderTarget->DrawTextW(
            statusText.c_str(), static_cast<UINT32>(statusText.size()),
            m_statusFormat.Get(), statusRect, m_ctrlTextPrimary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    const D2D1_RECT_F metricsRect = D2D1::RectF(
        leftX, botRowTop, (std::max)(leftX, dividerX - 8.0f * s), botRowBot);
    if (m_ctrlTextSecondary) {
        m_controlRenderTarget->DrawTextW(
            metricsStr.c_str(), static_cast<UINT32>(metricsStr.size()),
            m_metricsFormat.Get(), metricsRect, m_ctrlTextSecondary.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (m_ctrlDividerBrush) {
        m_controlRenderTarget->DrawLine(
            D2D1::Point2F(dividerX, 10.0f * s), D2D1::Point2F(dividerX, h - 10.0f * s),
            m_ctrlDividerBrush.Get(), (std::max)(1.0f, 1.0f * s));
    }

    // 右侧 5 枚交互式控制按钮：同心圆角 6.0f * s，支持全量与两级紧凑降级零重叠绘制
    const float btnRadius = 6.0f * s;
    const float keyPillH = 20.0f * s;
    static constexpr std::array<ControlButtonId, 5> kBtnIds = {
        ControlButtonId::TogglePause,
        ControlButtonId::Pin,
        ControlButtonId::SaveAs,
        ControlButtonId::Cancel,
        ControlButtonId::Confirm
    };

    for (std::size_t i = 0; i < kBtnIds.size(); ++i) {
        const ControlButtonId btnId = kBtnIds[i];
        const D2D1_RECT_F bRect = snap.buttonRects[i];
        const std::wstring& btnLabel = snap.labels[i];
        const std::wstring& btnKeycap = snap.keycaps[i];
        const float lblMeasuredW = snap.labelMeasuredWidths[i];
        const float keyPillW = snap.keyPillWidths[i];
        const bool isPrimary = (btnId == ControlButtonId::Confirm);
        const bool isDanger = (btnId == ControlButtonId::Cancel);

        const bool hovered = (m_hoveredBtn == btnId);
        const bool pressed = (m_pressedBtn == btnId);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bRect, btnRadius, btnRadius);

        if (isPrimary) {
            ID2D1Brush* bg = (hovered || pressed) ? m_ctrlAccentHoverBg.Get() : m_ctrlAccentBg.Get();
            if (bg) m_controlRenderTarget->FillRoundedRectangle(&rr, bg);
        } else if (pressed && m_ctrlBtnPressBg) {
            m_controlRenderTarget->FillRoundedRectangle(&rr, m_ctrlBtnPressBg.Get());
        } else if (hovered) {
            ID2D1Brush* bg = isDanger ? m_ctrlDangerHoverBg.Get() : m_ctrlBtnHoverBg.Get();
            if (bg) m_controlRenderTarget->FillRoundedRectangle(&rr, bg);
        }

        const float midY = (bRect.top + bRect.bottom) * 0.5f;
        const bool iconOnly = btnLabel.empty() && btnKeycap.empty();
        const float iconLeft = iconOnly
            ? ((bRect.left + bRect.right - iconSize) * 0.5f)
            : (bRect.left + padL);
        const D2D1_RECT_F iconBox = D2D1::RectF(
            iconLeft, midY - iconSize * 0.5f, iconLeft + iconSize, midY + iconSize * 0.5f);

        ID2D1Brush* btnContentBrush = isPrimary
            ? m_ctrlTextOnPrimary.Get()
            : ((isDanger && hovered) ? m_ctrlDangerIconBrush.Get() : m_ctrlTextPrimary.Get());

        if (btnContentBrush) {
            drawControlVectorIcon(
                m_controlRenderTarget.Get(), m_d2dFactory.Get(), m_roundStrokeStyle.Get(),
                static_cast<int>(i), isPausedOrHold, iconBox, btnContentBrush, s);
        }

        float keyLeft = iconBox.right + gapIconKey;
        if (!btnLabel.empty() && btnContentBrush) {
            const float lblLeft = iconBox.right + gapIconLabel;
            const D2D1_RECT_F lblRect = D2D1::RectF(
                lblLeft, bRect.top, lblLeft + lblMeasuredW, bRect.bottom);
            m_controlRenderTarget->DrawTextW(
                btnLabel.c_str(), static_cast<UINT32>(btnLabel.size()),
                m_btnLabelFormat.Get(), lblRect, btnContentBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            keyLeft = lblRect.right + gapLabelKey;
        }

        if (!btnKeycap.empty() && keyPillW > 0.0f) {
            const D2D1_RECT_F keyRect = D2D1::RectF(
                keyLeft, midY - keyPillH * 0.5f, keyLeft + keyPillW, midY + keyPillH * 0.5f);
            const D2D1_ROUNDED_RECT keyPill = D2D1::RoundedRect(keyRect, 4.5f * s, 4.5f * s);
            ID2D1Brush* pillBg = isPrimary ? m_ctrlKeycapPrimaryBg.Get() : m_ctrlKeycapBg.Get();
            ID2D1Brush* pillFg = isPrimary ? m_ctrlTextOnPrimary.Get() : m_ctrlTextSecondary.Get();
            if (pillBg) m_controlRenderTarget->FillRoundedRectangle(&keyPill, pillBg);
            if (pillFg) {
                m_controlRenderTarget->DrawTextW(
                    btnKeycap.c_str(), static_cast<UINT32>(btnKeycap.size()),
                    m_keycapFormat.Get(), keyRect, pillFg,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    const HRESULT endHr = m_controlRenderTarget->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET ||
        endHr == DXGI_ERROR_DEVICE_REMOVED ||
        endHr == DXGI_ERROR_DEVICE_RESET) {
        handleControlTargetDeviceLost();
    } else if (SUCCEEDED(endHr)) {
        POINT ptSrc = {0, 0};
        SIZE size = {width, height};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(m_controlHwnd, hdcScreen, nullptr, &size, m_controlDibCache.memDc, &ptSrc, 0, &blend, ULW_ALPHA);
        m_controlBarDirty = false;
        m_lastRenderedState = state;
        m_lastRenderedWorkWidth = workWidth;
        m_lastRenderedPulseStep = pulseStep;
        m_lastRenderedHoveredBtn = m_hoveredBtn;
        m_lastRenderedPressedBtn = m_pressedBtn;
        m_lastRenderedStatus = statusText;
        m_lastRenderedMetrics = metricsStr;
    }

    ReleaseDC(nullptr, hdcScreen);
}

ScrollCaptureOverlay::ControlButtonId ScrollCaptureOverlay::hitTestControlButton(POINT clientPt) const {
    const float x = static_cast<float>(clientPt.x);
    const float y = static_cast<float>(clientPt.y);
    for (const auto& item : m_buttonRects) {
        if (item.id != ControlButtonId::None &&
            x >= item.rect.left && x <= item.rect.right &&
            y >= item.rect.top && y <= item.rect.bottom) {
            return item.id;
        }
    }
    return ControlButtonId::None;
}

void ScrollCaptureOverlay::executeControlAction(ControlButtonId id) {
    switch (id) {
        case ControlButtonId::TogglePause:
            ScrollCapture::instance().togglePause();
            syncPauseHotkeyWithRuntimeState();
            renderControlBar();
            break;
        case ControlButtonId::Pin:
            ScrollCapture::instance().requestFinish(ScrollFinishAction::Pin);
            break;
        case ControlButtonId::SaveAs:
            ScrollCapture::instance().requestFinish(ScrollFinishAction::SaveAs);
            break;
        case ControlButtonId::Cancel:
            ScrollCapture::instance().requestCancel();
            break;
        case ControlButtonId::Confirm:
            ScrollCapture::instance().requestFinish(ScrollFinishAction::Confirm);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK ScrollCaptureOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& self = instance();
    if (msg == WM_TIMER && wParam == 1) {
        self.render();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ScrollCaptureOverlay::controlWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& self = instance();
    switch (msg) {
        case WM_MOUSEACTIVATE:
            // 核心铁律：点击悬浮控制台绝不抢夺底层目标滚动窗口的焦点
            return MA_NOACTIVATE;

        case WM_SETCURSOR: {
            POINT pt{};
            if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                const auto hit = self.hitTestControlButton(pt);
                SetCursor(LoadCursorW(nullptr, hit != ControlButtonId::None ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!self.m_trackingMouseLeave) {
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                if (TrackMouseEvent(&tme)) {
                    self.m_trackingMouseLeave = true;
                }
            }
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto hit = self.hitTestControlButton(pt);
            if (hit != self.m_hoveredBtn) {
                self.m_hoveredBtn = hit;
                self.renderControlBar();
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            self.m_trackingMouseLeave = false;
            if (self.m_hoveredBtn != ControlButtonId::None || self.m_pressedBtn != ControlButtonId::None) {
                self.m_hoveredBtn = ControlButtonId::None;
                self.m_pressedBtn = ControlButtonId::None;
                self.renderControlBar();
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            self.m_pressedBtn = self.hitTestControlButton(pt);
            self.renderControlBar();
            return 0;
        }

        case WM_LBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const auto releasedOn = self.hitTestControlButton(pt);
            const auto pressed = self.m_pressedBtn;
            self.m_pressedBtn = ControlButtonId::None;
            self.renderControlBar();
            if (pressed != ControlButtonId::None && pressed == releasedOn) {
                self.executeControlAction(releasedOn);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
