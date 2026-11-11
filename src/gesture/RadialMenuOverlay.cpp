#include "gesture/RadialMenuOverlay.h"
#include "gesture/RadialMenuStyle.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/GestureAction.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include <cmath>
#include <algorithm>
#include <charconv>
#include <unordered_map>

namespace tools3000::gesture {

static const wchar_t* CLASS_NAME = L"Tools3000_RadialMenu";
const UINT_PTR TIMER_ID_ANIMATION = 1001;
constexpr float PI_F = 3.14159265358979323846f;

// 缓动函数
static float easeOutBack(float x) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float delta = x - 1.0f;
    return 1.0f + c3 * delta * delta * delta + c1 * delta * delta;
}

RadialMenuOverlay& RadialMenuOverlay::instance() {
    static RadialMenuOverlay inst;
    return inst;
}

RadialMenuOverlay::RadialMenuOverlay() {
    registerWindowClass();
}

RadialMenuOverlay::~RadialMenuOverlay() {
    if (m_timerId && m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, TIMER_ID_ANIMATION);
        m_timerId = 0;
    }
    m_visible.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        discardResourcesLocked();
        deleteBitmapLocked();
        m_d2dFactory.Reset();
        m_dwriteFactory.Reset();
    }
    if (m_hwnd && IsWindow(m_hwnd) &&
        GetWindowThreadProcessId(m_hwnd, nullptr) == GetCurrentThreadId()) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
    if (m_helperOwnerHwnd && IsWindow(m_helperOwnerHwnd) &&
        GetWindowThreadProcessId(m_helperOwnerHwnd, nullptr) == GetCurrentThreadId()) {
        DestroyWindow(m_helperOwnerHwnd);
    }
    m_helperOwnerHwnd = nullptr;
}

POINT RadialMenuOverlay::centerPoint() const noexcept {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_centerPt;
}

void RadialMenuOverlay::registerWindowClass() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

void RadialMenuOverlay::setItems(const std::vector<RadialMenuItem>& items) {
    size_t prevCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        prevCount = m_items.size();
        m_items = items;
        m_itemCount.store(items.size(), std::memory_order_release);
    }

    auto syncUiState = [this, newCount = items.size(), countChanged = (prevCount != items.size())]() {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_hoverRadii.assign(newCount, 0.0f);
        if (countChanged) {
            // 扇区密度改变时重建字号与表面规格
            discardResourcesLocked();
            deleteBitmapLocked();
        }
    };

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post(std::move(syncUiState));
    } else {
        syncUiState();
    }
}

std::vector<RadialMenuItem> RadialMenuOverlay::getItems() const {
    std::lock_guard<std::mutex> lock(m_itemsMutex);
    return m_items;
}

void RadialMenuOverlay::show(POINT centerPt) {
    const size_t itemCount = m_itemCount.load(std::memory_order_acquire);
    if (itemCount == 0) return;

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, centerPt]() {
            show(centerPt);
        });
        return;
    }

    const size_t currentItemCount = m_itemCount.load(std::memory_order_acquire);
    if (currentItemCount == 0) return;

    const HMONITOR hMon = tools3000::core::dpi::monitorAtPoint(centerPt);
    const float newScale = tools3000::core::dpi::scaleAtPoint(centerPt);
    const auto metrics = RadialMenuStyle::metricsForDpi(
        tools3000::core::dpi::effectiveDpiForMonitor(hMon),
        currentItemCount);
    const int newWindowSize = metrics.windowSize;

    // 计算显示器工作区并钳制窗口原点，防止靠近屏幕四边弹出时轮盘被裁剪
    RECT workArea = {
        0, 0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)};
    if (hMon) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
            workArea = mi.rcWork;
        }
    }
    const int edgeMargin = tools3000::core::dpi::scaleMetric(RadialMenuStyle::BaseScreenEdgeMargin, newScale);
    const POINT origin = RadialMenuStyle::clampWindowOrigin(centerPt, newWindowSize, workArea, edgeMargin);
    m_windowLeft.store(origin.x, std::memory_order_release);
    m_windowTop.store(origin.y, std::memory_order_release);

    const HWND initialFg = GetForegroundWindow();
    const DWORD nowTick = GetTickCount();
    m_sawButtonDown.store(false, std::memory_order_release);
    m_hoverIndex.store(-1, std::memory_order_release);
    m_showStartTime.store(nowTick, std::memory_order_release);
    m_radiusOuter.store(metrics.outerRadius, std::memory_order_release);
    m_radiusInner.store(metrics.innerRadius, std::memory_order_release);

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        const float prevScale = m_dpiScale.load(std::memory_order_relaxed);
        if (std::abs(newScale - prevScale) >= 0.01f ||
            newWindowSize != m_windowSize.load(std::memory_order_relaxed)) {
            discardResourcesLocked();
            deleteBitmapLocked();
            m_dpiScale.store(newScale, std::memory_order_release);
            m_windowSize.store(newWindowSize, std::memory_order_release);
        }
        m_centerPt = centerPt;
        m_initialForegroundHwnd = initialFg;
        m_popScale = 0.25f;
        m_lastTickTime = nowTick;
        m_hoverRadii.assign(currentItemCount, 0.0f);
    }

    if (!m_hwnd) {
        HINSTANCE hInst = GetModuleHandle(nullptr);
        m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(hInst, L"Tools3000_RadialMenuHelperOwner");

        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            CLASS_NAME, L"RadialMenu",
            WS_POPUP,
            origin.x, origin.y,
            newWindowSize, newWindowSize,
            m_helperOwnerHwnd, nullptr, hInst, this
        );
        if (m_hwnd) {
            tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(m_hwnd, false);
            SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
        }
    }

    bool resourcesReady = false;
    if (m_hwnd) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        resourcesReady = createResourcesLocked();
    }
    if (!m_hwnd || !resourcesReady) {
        LOG_ERROR("手势轮盘高 DPI 渲染资源创建失败");
        return;
    }

    // 同步初始悬停状态（若靠近屏幕边缘钳制后鼠标已位于某扇区内，立即点亮对应扇区）
    POINT curPt{};
    if (GetCursorPos(&curPt)) {
        const POINT localPt{curPt.x - origin.x, curPt.y - origin.y};
        m_hoverIndex.store(hitTest(localPt, /*clampOuterRing=*/true), std::memory_order_release);
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST,
                 origin.x, origin.y,
                 newWindowSize, newWindowSize,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    m_visible.store(true, std::memory_order_release);
    m_timerId = SetTimer(m_hwnd, TIMER_ID_ANIMATION, 16, nullptr); // ~60 FPS

    render();
}

void RadialMenuOverlay::hide() {
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            hide();
        });
        return;
    }

    if (m_visible.exchange(false, std::memory_order_acq_rel)) {
        if (m_timerId && m_hwnd) {
            KillTimer(m_hwnd, TIMER_ID_ANIMATION);
            m_timerId = 0;
        }
        if (m_hwnd && GetCapture() == m_hwnd) {
            ReleaseCapture();
        }
        if (m_hwnd) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_hoverIndex.store(-1, std::memory_order_release);
        m_sawButtonDown.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_initialForegroundHwnd = nullptr;
            discardResourcesLocked();
            deleteBitmapLocked();
        }
    }
}

void RadialMenuOverlay::reloadThemeColors() {
    if (!m_visible.load(std::memory_order_acquire)) {
        return;
    }
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            if (m_visible.load(std::memory_order_acquire)) {
                render();
            }
        });
        return;
    }
    render();
}

void RadialMenuOverlay::shutdown() {
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            shutdown();
        });
        return;
    }

    hide();

    HWND hwndToDestroy = m_hwnd;
    HWND helperToDestroy = m_helperOwnerHwnd;
    if (m_timerId && hwndToDestroy && IsWindow(hwndToDestroy)) {
        KillTimer(hwndToDestroy, TIMER_ID_ANIMATION);
    }
    m_timerId = 0;
    m_hwnd = nullptr;
    m_helperOwnerHwnd = nullptr;

    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        m_initialForegroundHwnd = nullptr;
        m_hoverRadii.clear();
        discardResourcesLocked();
        deleteBitmapLocked();
        m_d2dFactory.Reset();
        m_dwriteFactory.Reset();
    }

    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        if (GetWindowThreadProcessId(hwndToDestroy, nullptr) == GetCurrentThreadId()) {
            DestroyWindow(hwndToDestroy);
        } else {
            PostMessageW(hwndToDestroy, WM_CLOSE, 0, 0);
        }
    }
    if (helperToDestroy && IsWindow(helperToDestroy)) {
        if (GetWindowThreadProcessId(helperToDestroy, nullptr) == GetCurrentThreadId()) {
            DestroyWindow(helperToDestroy);
        } else {
            PostMessageW(helperToDestroy, WM_CLOSE, 0, 0);
        }
    }
}

bool RadialMenuOverlay::isPointInsideRing(POINT screenPt) const noexcept {
    if (!isVisible()) return false;
    const int winSize = m_windowSize.load(std::memory_order_relaxed);
    const int left = m_windowLeft.load(std::memory_order_acquire);
    const int top = m_windowTop.load(std::memory_order_acquire);
    const float cx = static_cast<float>(left) + winSize * 0.5f;
    const float cy = static_cast<float>(top) + winSize * 0.5f;
    const float dx = static_cast<float>(screenPt.x) - cx;
    const float dy = static_cast<float>(screenPt.y) - cy;
    const float scale = m_dpiScale.load(std::memory_order_acquire);
    const float maxR = static_cast<float>(m_radiusOuter.load(std::memory_order_relaxed)) + 18.0f * scale;
    return (dx * dx + dy * dy) <= (maxR * maxR);
}

bool RadialMenuOverlay::handleGlobalMouseMessage(WPARAM message, POINT screenPt) noexcept {
    if (!isVisible()) return false;

    const int left = m_windowLeft.load(std::memory_order_acquire);
    const int top = m_windowTop.load(std::memory_order_acquire);
    const POINT clientPt{screenPt.x - left, screenPt.y - top};

    switch (message) {
        case WM_MOUSEMOVE: {
            if (m_hwnd && IsWindow(m_hwnd)) {
                PostMessageW(m_hwnd, WM_MOUSEMOVE, 0,
                             MAKELPARAM(static_cast<short>(clientPt.x), static_cast<short>(clientPt.y)));
            }
            return false;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            if (!isPointInsideRing(screenPt)) {
                // 点击轮盘圆环外部：自动收起轮盘，不吞掉外部点击
                if (m_hwnd && IsWindow(m_hwnd) &&
                    GetCurrentThreadId() != GetWindowThreadProcessId(m_hwnd, nullptr) &&
                    tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                    tools3000::core::MainThreadDispatcher::instance().post([this]() { hide(); });
                } else {
                    hide();
                }
                return false;
            }
            m_sawButtonDown.store(true, std::memory_order_release);
            if (m_hwnd && IsWindow(m_hwnd)) {
                PostMessageW(m_hwnd, static_cast<UINT>(message), 0,
                             MAKELPARAM(static_cast<short>(clientPt.x), static_cast<short>(clientPt.y)));
            }
            return true;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            const int sector = hitTest(clientPt, /*clampOuterRing=*/false);
            const DWORD elapsedSinceShow = GetTickCount() - m_showStartTime.load(std::memory_order_acquire);
            const bool sawDown = m_sawButtonDown.load(std::memory_order_acquire);
            // 若在轮盘内按下了鼠标，或者按住呼出后已划入某个扇区松手，或者呼出超过 180ms 后在中心取消区松手：确认或取消收起
            if (sawDown || (isPointInsideRing(screenPt) && (sector >= 0 || elapsedSinceShow >= 180)) ||
                (!isPointInsideRing(screenPt) && sector >= 0 && elapsedSinceShow >= 120)) {
                if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                    tools3000::core::MainThreadDispatcher::instance().post([this, clientPt]() {
                        confirmSelectionAtClientPoint(clientPt);
                    });
                } else {
                    confirmSelectionAtClientPoint(clientPt);
                }
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

bool RadialMenuOverlay::createResourcesLocked() {
    if (!m_d2dFactory) {
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                &options, reinterpret_cast<void**>(m_d2dFactory.GetAddressOf())))) {
            return false;
        }
    }
    if (!m_dwriteFactory && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
        return false;
    }

    const int winSize = m_windowSize.load(std::memory_order_relaxed);
    const float scale = m_dpiScale.load(std::memory_order_acquire);

    if (!m_renderTarget) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT
        );
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget)) ||
            !m_renderTarget) {
            return false;
        }
        m_renderTarget->SetDpi(96.0f, 96.0f);

        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.1f, 0.13f, 0.88f), &m_bgBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.45f, 0.9f, 0.96f), &m_hoverBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_textBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), &m_borderBrush);

        const float fontSize = RadialMenuStyle::fontSizeForItems(
            scale, m_itemCount.load(std::memory_order_acquire));
        const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
        const wchar_t* localeName = tools3000::core::isUiLanguageChinese() ? L"zh-CN" : L"en-US";
        if (FAILED(m_dwriteFactory->CreateTextFormat(
            fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize, localeName, &m_textFormat
        )) || !m_textFormat) return false;
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        m_ellipsisTrimmingSign.Reset();
        if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(
                m_textFormat.Get(), &m_ellipsisTrimmingSign)) && m_ellipsisTrimmingSign) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            m_textFormat->SetTrimming(&trimming, m_ellipsisTrimmingSign.Get());
        }
    }

    if (!m_hBitmap) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = winSize;
        bmi.bmiHeader.biHeight = -winSize;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    }
    return m_renderTarget && m_bgBrush && m_hoverBrush && m_textBrush &&
           m_borderBrush && m_textFormat && m_hBitmap && m_bits;
}

void RadialMenuOverlay::discardResourcesLocked() {
    m_renderTarget.Reset();
    m_bgBrush.Reset();
    m_hoverBrush.Reset();
    m_textBrush.Reset();
    m_borderBrush.Reset();
    m_ellipsisTrimmingSign.Reset();
    m_textFormat.Reset();
}

void RadialMenuOverlay::deleteBitmapLocked() noexcept {
    if (m_hBitmap) {
        DeleteObject(m_hBitmap);
        m_hBitmap = nullptr;
        m_bits = nullptr;
    }
}

void RadialMenuOverlay::render() {
    if (!m_hwnd) return;

    // 先在锁外获取菜单项快照，严格避免 m_stateMutex 与 m_itemsMutex 嵌套加锁
    const std::vector<RadialMenuItem> itemsSnapshot = getItems();

    std::lock_guard<std::mutex> stateLock(m_stateMutex);
    if (!m_hwnd) return;
    if (!createResourcesLocked()) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }
    HGDIOBJ hOldBmp = SelectObject(hdcMem, m_hBitmap);

    const int winSize = m_windowSize.load(std::memory_order_relaxed);
    const int baseOuterRadius = m_radiusOuter.load(std::memory_order_relaxed);
    const int innerRadiusPx = m_radiusInner.load(std::memory_order_relaxed);
    const int hoverIdx = m_hoverIndex.load(std::memory_order_acquire);
    const float scale = m_dpiScale.load(std::memory_order_acquire);
    const float popScale = m_popScale;

    RECT rc = {0, 0, winSize, winSize};
    m_renderTarget->BindDC(hdcMem, &rc);
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    const auto accent = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    if (m_bgBrush) {
        m_bgBrush->SetColor(isDark
            ? D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.88f)
            : D2D1::ColorF(0.97f, 0.98f, 0.99f, 0.94f));
    }
    if (m_hoverBrush) {
        m_hoverBrush->SetColor(D2D1::ColorF(accent.r, accent.g, accent.b, 0.96f));
    }
    if (m_textBrush) {
        m_textBrush->SetColor(isDark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
            : D2D1::ColorF(0.09f, 0.11f, 0.16f, 0.98f));
    }
    if (m_borderBrush) {
        m_borderBrush->SetColor(isDark
            ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f)
            : D2D1::ColorF(0.10f, 0.12f, 0.18f, 0.16f));
    }

    // 缩放矩阵 (出场动画)
    float cx = winSize / 2.0f;
    float cy = winSize / 2.0f;
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Scale(popScale, popScale, D2D1::Point2F(cx, cy)));

    // 中心取消区域反馈（始终绘制非零 Alpha 底衬保证中心点击不穿透，悬停在中心时高亮淡红取消提示）
    {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> centerBrush;
        const D2D1_COLOR_F centerColor = (hoverIdx == -1)
            ? D2D1::ColorF(0.94f, 0.32f, 0.32f, 0.58f)
            : (isDark
                ? D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.72f)
                : D2D1::ColorF(0.94f, 0.96f, 0.99f, 0.86f));
        if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(centerColor, &centerBrush)) && centerBrush) {
            const float innerRadius = static_cast<float>(innerRadiusPx);
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(cx, cy), innerRadius, innerRadius),
                centerBrush.Get());
            m_renderTarget->DrawEllipse(
                D2D1::Ellipse(D2D1::Point2F(cx, cy), innerRadius, innerRadius),
                m_borderBrush.Get(), 1.2f * scale);
        }
    }

    // 绘制轮盘扇区与自适应无重叠单行文本（两遍分层绘制：先绘制常规扇区，最后在顶层绘制悬停扇区完整闭合亮框）
    const int numItems = static_cast<int>(itemsSnapshot.size());
    if (numItems > 0) {
        const float angleStep = 360.0f / static_cast<float>(numItems);
        const float offsetAngle = -90.0f - (angleStep / 2.0f);
        const float innerRadius = static_cast<float>(innerRadiusPx);
        const D2D1_ARC_SIZE arcSize = (angleStep > 180.0f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

        auto buildSectorGeometry = [&](int idx, float& outOuterR) -> Microsoft::WRL::ComPtr<ID2D1PathGeometry> {
            const float startAngle = offsetAngle + idx * angleStep;
            const float endAngle = startAngle + angleStep;
            const float startRad = startAngle * PI_F / 180.0f;
            const float endRad = endAngle * PI_F / 180.0f;
            const float hoverExtra = (idx < static_cast<int>(m_hoverRadii.size())) ? m_hoverRadii[idx] : 0.0f;
            outOuterR = static_cast<float>(baseOuterRadius) + hoverExtra;

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
            if (FAILED(m_d2dFactory->CreatePathGeometry(&geometry)) || !geometry) return nullptr;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geometry->Open(&sink)) || !sink) return nullptr;

            const D2D1_POINT_2F ptInnerStart = D2D1::Point2F(cx + innerRadius * std::cos(startRad), cy + innerRadius * std::sin(startRad));
            const D2D1_POINT_2F ptOuterStart = D2D1::Point2F(cx + outOuterR * std::cos(startRad), cy + outOuterR * std::sin(startRad));
            const D2D1_POINT_2F ptOuterEnd = D2D1::Point2F(cx + outOuterR * std::cos(endRad), cy + outOuterR * std::sin(endRad));
            const D2D1_POINT_2F ptInnerEnd = D2D1::Point2F(cx + innerRadius * std::cos(endRad), cy + innerRadius * std::sin(endRad));

            sink->BeginFigure(ptInnerStart, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(ptOuterStart);
            sink->AddArc(D2D1::ArcSegment(ptOuterEnd, D2D1::SizeF(outOuterR, outOuterR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSize));
            sink->AddLine(ptInnerEnd);
            sink->AddArc(D2D1::ArcSegment(ptInnerStart, D2D1::SizeF(innerRadius, innerRadius), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSize));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) return nullptr;
            return geometry;
        };

        // 第一遍：绘制所有非悬停常规扇区
        for (int i = 0; i < numItems; ++i) {
            if (i == hoverIdx) continue;
            float outerR = 0.0f;
            auto geometry = buildSectorGeometry(i, outerR);
            if (!geometry) continue;
            m_renderTarget->FillGeometry(geometry.Get(), m_bgBrush.Get());
            m_renderTarget->DrawGeometry(geometry.Get(), m_borderBrush.Get(), 1.5f * scale);
        }

        // 第二遍：在最顶层绘制悬停放大扇区及完整闭合高亮亮框，防止顺时针相邻扇区压盖切边
        if (hoverIdx >= 0 && hoverIdx < numItems) {
            float hoverOuterR = 0.0f;
            auto hoverGeometry = buildSectorGeometry(hoverIdx, hoverOuterR);
            if (hoverGeometry) {
                m_renderTarget->FillGeometry(hoverGeometry.Get(), m_hoverBrush.Get());
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverBorderBrush;
                if (SUCCEEDED(m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(0.75f, 0.90f, 1.0f, 0.92f), &hoverBorderBrush)) && hoverBorderBrush) {
                    m_renderTarget->DrawGeometry(hoverGeometry.Get(), hoverBorderBrush.Get(), 2.0f * scale);
                } else {
                    m_renderTarget->DrawGeometry(hoverGeometry.Get(), m_borderBrush.Get(), 2.0f * scale);
                }
            }
        }

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hoverTextBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &hoverTextBrush);

        // 第三遍：在所有扇区几何之上绘制单行省略号裁剪文本
        for (int i = 0; i < numItems; ++i) {
            ID2D1Brush* activeTextBrush = (i == hoverIdx && hoverTextBrush)
                ? static_cast<ID2D1Brush*>(hoverTextBrush.Get())
                : static_cast<ID2D1Brush*>(m_textBrush.Get());
            const float hoverExtra = (i < static_cast<int>(m_hoverRadii.size())) ? m_hoverRadii[i] : 0.0f;
            const float outerR = static_cast<float>(baseOuterRadius) + hoverExtra;
            const D2D1_RECT_F textRect = RadialMenuStyle::computeSectorTextBox(
                cx, cy, innerRadius, outerR, i, numItems, scale);

            const std::wstring wLabel = tools3000::core::WinUtils::utf8ToWstring(itemsSnapshot[i].label);
            const float boxW = (std::max)(1.0f, textRect.right - textRect.left);
            const float boxH = (std::max)(1.0f, textRect.bottom - textRect.top);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
            if (m_dwriteFactory &&
                SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    wLabel.c_str(),
                    static_cast<UINT32>(wLabel.length()),
                    m_textFormat.Get(),
                    boxW,
                    boxH,
                    &textLayout)) &&
                textLayout) {
                textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                if (m_ellipsisTrimmingSign) {
                    DWRITE_TRIMMING trimming{};
                    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                    textLayout->SetTrimming(&trimming, m_ellipsisTrimmingSign.Get());
                }
                m_renderTarget->DrawTextLayout(
                    D2D1::Point2F(textRect.left, textRect.top),
                    textLayout.Get(),
                    activeTextBrush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            } else {
                m_renderTarget->DrawTextW(
                    wLabel.c_str(),
                    static_cast<UINT32>(wLabel.length()),
                    m_textFormat.Get(),
                    textRect,
                    activeTextBrush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    if (FAILED(m_renderTarget->EndDraw())) {
        discardResourcesLocked();
        SelectObject(hdcMem, hOldBmp);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    updateLayeredWindowLocked(hdcScreen, hdcMem, popScale);

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void RadialMenuOverlay::updateLayeredWindowLocked(HDC hdcScreen, HDC hdcMem, float popScale) {
    if (!m_hwnd || !hdcScreen || !hdcMem || !m_hBitmap) return;
    const int winSize = m_windowSize.load(std::memory_order_relaxed);
    POINT ptSrc = {0, 0};
    POINT ptDst = {
        m_windowLeft.load(std::memory_order_acquire),
        m_windowTop.load(std::memory_order_acquire)};
    SIZE size = {winSize, winSize};

    BYTE alpha = static_cast<BYTE>(std::clamp(popScale * 255.0f, 0.0f, 255.0f));
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};

    UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
}

int RadialMenuOverlay::hitTest(POINT pt, bool clampOuterRing) const noexcept {
    const size_t rawCount = m_itemCount.load(std::memory_order_acquire);
    if (rawCount == 0) return -1;

    const int winSize = m_windowSize.load(std::memory_order_relaxed);
    const float dx = static_cast<float>(pt.x) - (winSize / 2.0f);
    const float dy = static_cast<float>(pt.y) - (winSize / 2.0f);
    const float dist = std::sqrt(dx * dx + dy * dy);

    // 内圆中心区视为取消区
    if (dist <= static_cast<float>(m_radiusInner.load(std::memory_order_acquire))) {
        return -1;
    }

    if (clampOuterRing) {
        const float scale = m_dpiScale.load(std::memory_order_acquire);
        const float maxHoverRadius = static_cast<float>(m_radiusOuter.load(std::memory_order_relaxed)) + 28.0f * scale;
        if (dist > maxHoverRadius) {
            return -1;
        }
    }

    const float angle = std::atan2(dy, dx) * 180.0f / PI_F; // -180 ~ 180
    const int numItems = static_cast<int>(rawCount);
    const float angleStep = 360.0f / static_cast<float>(numItems);
    const float offsetAngle = -90.0f - (angleStep / 2.0f);

    const float normAngle = std::fmod(angle - offsetAngle + 360.0f * 2.0f, 360.0f);
    const int index = static_cast<int>(normAngle / angleStep) % numItems;
    return index;
}

void RadialMenuOverlay::confirmSelectionAtClientPoint(POINT clientPt) {
    if (!isVisible()) return;
    const int finalIndex = hitTest(clientPt, /*clampOuterRing=*/false);
    hide();
    if (finalIndex != -1) {
        executeAction(finalIndex);
    }
}

void RadialMenuOverlay::executeAction(int index) {
    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        if (index < 0 || index >= static_cast<int>(m_items.size())) {
            return;
        }
        cmd = m_items[index].command;
    }
    LOG_INFO("RadialMenu: Execute command '{}'", cmd);

    // 兼容早期的可读字符串，并以数值索引作为当前稳定格式。
    static const std::unordered_map<std::string, BuiltinCommand> legacyCommands = {
        {"capture", BuiltinCommand::TakeScreenshot},
        {"search", BuiltinCommand::ToggleSearch},
        {"lock", BuiltinCommand::LockScreen},
        {"pin", BuiltinCommand::PasteAsPin},
        {"record", BuiltinCommand::StartRecording},
    };
    if (const auto legacy = legacyCommands.find(cmd); legacy != legacyCommands.end()) {
        BuiltinCommandDispatcher::instance().execute(legacy->second);
        return;
    }
    int commandIndex = -1;
    const auto [end, error] = std::from_chars(
        cmd.data(), cmd.data() + cmd.size(), commandIndex);
    if (error == std::errc{} && end == cmd.data() + cmd.size() &&
        isValidBuiltinCommandIndex(commandIndex)) {
        BuiltinCommandDispatcher::instance().execute(
            static_cast<BuiltinCommand>(commandIndex));
        return;
    }

    // 仅为旧配置保留结构化 IPC 消息兼容；普通文本不再交给 JSON 解析器。
    if (!cmd.empty() && cmd.front() == '{') {
        tools3000::core::MessageBridge::instance().handleMessage(cmd);
    } else {
        LOG_WARN("RadialMenu: 忽略未知命令 '{}'", cmd);
    }
}

LRESULT CALLBACK RadialMenuOverlay::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    RadialMenuOverlay* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<RadialMenuOverlay*>(pCreate->lpCreateParams);
        if (pThis) {
            pThis->m_hwnd = hwnd;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<RadialMenuOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handleMessage(uMsg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT RadialMenuOverlay::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_TIMER: {
            if (wParam == TIMER_ID_ANIMATION && isVisible()) {
                // Esc 按下或前台窗口切换时自动收起轮盘
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                    hide();
                    return 0;
                }
                const HWND curFg = GetForegroundWindow();
                HWND initialFg = nullptr;
                {
                    std::lock_guard<std::mutex> stateLock(m_stateMutex);
                    initialFg = m_initialForegroundHwnd;
                }
                if (initialFg && curFg &&
                    curFg != initialFg &&
                    curFg != m_hwnd && curFg != m_helperOwnerHwnd) {
                    hide();
                    return 0;
                }

                bool needsRender = false;
                const DWORD now = GetTickCount();

                // 实时同步物理光标悬停扇区（即使未持有鼠标捕获也 100% 跟手）
                POINT screenPt{};
                if (GetCursorPos(&screenPt)) {
                    const POINT localPt{
                        screenPt.x - m_windowLeft.load(std::memory_order_acquire),
                        screenPt.y - m_windowTop.load(std::memory_order_acquire)};
                    const bool anyBtnHeld =
                        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
                        ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
                        ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
                    const int polledHover = hitTest(localPt, /*clampOuterRing=*/!anyBtnHeld);
                    if (polledHover != m_hoverIndex.exchange(polledHover, std::memory_order_acq_rel)) {
                        needsRender = true;
                    }
                }

                const int hoverIdx = m_hoverIndex.load(std::memory_order_acquire);
                const float scale = m_dpiScale.load(std::memory_order_acquire);
                const DWORD showStart = m_showStartTime.load(std::memory_order_acquire);
                const size_t itemCount = m_itemCount.load(std::memory_order_acquire);

                {
                    std::lock_guard<std::mutex> stateLock(m_stateMutex);
                    const float dt = (std::min)(0.05f, (now - m_lastTickTime) / 1000.0f);
                    m_lastTickTime = now;

                    // 1. 出场动画 (0.25 -> 1.0)
                    const float elapsedShow = (now - showStart) / 1000.0f;
                    if (elapsedShow < 0.2f) {
                        const float progress = std::clamp(elapsedShow / 0.2f, 0.0f, 1.0f);
                        m_popScale = easeOutBack(progress);
                        needsRender = true;
                    } else if (m_popScale != 1.0f) {
                        m_popScale = 1.0f;
                        needsRender = true;
                    }

                    // 2. 悬停动画 (半径伸缩)
                    if (m_hoverRadii.size() != itemCount) {
                        m_hoverRadii.assign(itemCount, 0.0f);
                    }
                    for (size_t i = 0; i < itemCount; ++i) {
                        const float targetRadii = (static_cast<int>(i) == hoverIdx)
                            ? 15.0f * scale : 0.0f;
                        if (std::abs(m_hoverRadii[i] - targetRadii) > 0.5f * scale) {
                            m_hoverRadii[i] += (targetRadii - m_hoverRadii[i]) * (std::min)(1.0f, 15.0f * dt);
                            needsRender = true;
                        } else if (m_hoverRadii[i] != targetRadii) {
                            m_hoverRadii[i] = targetRadii;
                            needsRender = true;
                        }
                    }
                }

                if (needsRender) {
                    render();
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt;
            pt.x = static_cast<short>(LOWORD(lParam));
            pt.y = static_cast<short>(HIWORD(lParam));
            const int newHover = hitTest(pt, /*clampOuterRing=*/false);
            if (newHover != m_hoverIndex.exchange(newHover, std::memory_order_acq_rel)) {
                render();
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            POINT pt;
            pt.x = static_cast<short>(LOWORD(lParam));
            pt.y = static_cast<short>(HIWORD(lParam));
            const POINT screenPt{
                pt.x + m_windowLeft.load(std::memory_order_acquire),
                pt.y + m_windowTop.load(std::memory_order_acquire)};
            if (!isPointInsideRing(screenPt)) {
                hide();
                return 0;
            }
            m_sawButtonDown.store(true, std::memory_order_release);
            const int newHover = hitTest(pt, /*clampOuterRing=*/false);
            if (newHover != m_hoverIndex.exchange(newHover, std::memory_order_acq_rel)) {
                render();
            }
            return 0;
        }
        case WM_MBUTTONUP:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            int finalIndex = m_hoverIndex.load(std::memory_order_acquire);
            if (lParam != 0) {
                POINT pt;
                pt.x = static_cast<short>(LOWORD(lParam));
                pt.y = static_cast<short>(HIWORD(lParam));
                finalIndex = hitTest(pt, /*clampOuterRing=*/false);
            }
            hide();
            if (finalIndex != -1) {
                executeAction(finalIndex);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                hide();
                return 0;
            }
            break;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE) {
                hide();
                return 0;
            }
            break;
        }
        case WM_KILLFOCUS: {
            hide();
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (lParam != 0 && reinterpret_cast<HWND>(lParam) != m_hwnd) {
                hide();
            }
            return 0;
        }
    }
    return DefWindowProcW(m_hwnd, uMsg, wParam, lParam);
}

} // namespace tools3000::gesture
