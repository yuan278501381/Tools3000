#include "ui/SearchWindow.h"
#include "ui/QuickLookWindow.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/DpiUtils.h"
#include "core/stats/PerformanceMonitor.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewWindowStyle.h"
#include "ui/WebViewSecurity.h"
#include "ui/WebViewSuspend.h"
#include "ui/KeyboardPipeline.h"
#include <WebView2.h>
#include <wrl/event.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <utility>
#include <algorithm>
#include <chrono>
#include "core/utils/WinUtils.h"
#include "core/utils/ThemeUtils.h"
#include "../resource.h"

#pragma comment(lib, "comctl32.lib")

using namespace Microsoft::WRL;

namespace tools3000::ui {

static constexpr const wchar_t* SEARCH_WINDOW_CLASS = L"Tools3000_SearchWindow";

namespace {

LRESULT CALLBACK WebViewResizeSubclassProc(
    HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    if (uMsg == WM_NCHITTEST) {
        HWND parentHwnd = reinterpret_cast<HWND>(dwRefData);
        if (parentHwnd && IsWindow(parentHwnd) && !IsZoomed(parentHwnd) &&
            GetPropW(parentHwnd, L"Tools3000_SearchMaximized") == nullptr) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            RECT rc;
            GetWindowRect(parentHwnd, &rc);
            const float scale = tools3000::core::dpi::scaleForWindow(parentHwnd);
            // 与 SearchWindow::calculateHitTest 完全对齐：所有非客户区边缘和对角拐角向系统声明透明，让宿主窗口响应拉伸
            if (SearchWindow::calculateHitTest(rc, pt, scale) != HTCLIENT) {
                return HTTRANSPARENT;
            }
        }
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

bool isSearchDarkTheme() {
    const std::string theme = tools3000::core::ConfigManager::instance().get<std::string>("/general/theme", "dark");
    if (theme == "light") return false;
    if (theme == "dark") return true;
    return tools3000::core::WinUtils::isSystemDarkMode();
}

COLORREF searchWindowBackdropColor() {
    return isSearchDarkTheme() ? RGB(15, 23, 42) : RGB(248, 250, 252);
}

void applySquareCornersForMaximized(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &noCorners, sizeof(noCorners));
    tools3000::core::WinUtils::clearWindowRegionIfSet(hwnd, false);
}

void hookWebViewChildWindows(HWND parentHwnd) {
    if (!parentHwnd || !IsWindow(parentHwnd)) return;
    EnumChildWindows(parentHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        SetWindowSubclass(child, WebViewResizeSubclassProc, 1001, static_cast<DWORD_PTR>(lParam));
        return TRUE;
    }, reinterpret_cast<LPARAM>(parentHwnd));
}

// 索引服务按需启动，预热 WebView 时并不拉起它。窗口真正呼出才是"用户要搜索"
// 的第一个可靠信号，此时把服务叫醒，启动耗时正好被用户的输入过程盖掉。
void warmUpSearchService() {
    tools3000::core::MessageBridge::instance().handleMessageAsync(
        R"({"id":0,"method":"search.warmup","params":{}})", [](std::string) {});
}

}  // namespace

SearchWindow& SearchWindow::instance() {
    static SearchWindow inst;
    return inst;
}

void SearchWindow::setWindowSize(int baseWidth, int baseHeight, bool forceCenter) {
    baseWidth = (std::clamp)(baseWidth, 500, 2400);
    baseHeight = (std::clamp)(baseHeight, 400, 1600);
    const bool wasMaximized = m_isMaximized.exchange(false);
    tools3000::core::ConfigManager::instance().set<bool>("/search/windowMaximized", false);
    tools3000::core::ConfigManager::instance().set("/search/windowWidth", baseWidth);
    tools3000::core::ConfigManager::instance().set("/search/windowHeight", baseHeight);

    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const DWORD winThread = GetWindowThreadProcessId(m_hwnd, nullptr);
    if (winThread != 0 && winThread != GetCurrentThreadId()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, baseWidth, baseHeight, forceCenter]() {
            setWindowSize(baseWidth, baseHeight, forceCenter);
        });
        return;
    }
    RemovePropW(m_hwnd, L"Tools3000_SearchMaximized");
    if (wasMaximized && m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:searchMaximizedChanged', { detail: false }));",
            nullptr);
    }
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    int scaledWidth = tools3000::core::dpi::scaleMetric(baseWidth, scale);
    int scaledHeight = tools3000::core::dpi::scaleMetric(baseHeight, scale);
    const int margin = tools3000::core::dpi::scaleMetric(SearchWindowStyle::BaseScreenMargin, scale);
    const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
        {scaledWidth, scaledHeight}, workArea, margin);
    scaledWidth = fitted.cx;
    scaledHeight = fitted.cy;

    int x = workArea.left + (workArea.right - workArea.left - scaledWidth) / 2;
    int y = workArea.top + (workArea.bottom - workArea.top - scaledHeight) / 2;

    if (!forceCenter && !wasMaximized) {
        RECT curRect{};
        if (GetWindowRect(m_hwnd, &curRect)) {
            // 保持当前窗口左上角 (x, y) 锚定固定，仅向右下方扩展
            int curX = curRect.left;
            int curY = curRect.top;

            // 检查当前坐标是否有效且在屏幕可视范围内
            if (curX >= workArea.left - 100 && curX < workArea.right - 100 &&
                curY >= workArea.top - 50 && curY < workArea.bottom - 100) {
                x = curX;
                y = curY;

                // 边界智能防溢出：如果向右下拉伸超出屏幕右侧或底部，自动向内微调
                if (x + scaledWidth > workArea.right - margin) {
                    x = (std::max)(static_cast<int>(workArea.left + margin), static_cast<int>(workArea.right - margin - scaledWidth));
                }
                if (y + scaledHeight > workArea.bottom - margin) {
                    y = (std::max)(static_cast<int>(workArea.top + margin), static_cast<int>(workArea.bottom - margin - scaledHeight));
                }
            }
        }
    }

    if (IsZoomed(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    }
    const HWND zInsertAfter = m_isPinned.load() ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(m_hwnd, zInsertAfter, x, y, scaledWidth, scaledHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, scaledWidth, scaledHeight, radius);
    if (m_controller) {
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }
}

void SearchWindow::setMaximized(bool maximized) {
    m_isMaximized.store(maximized);
    tools3000::core::ConfigManager::instance().set<bool>("/search/windowMaximized", maximized);
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const DWORD winThread = GetWindowThreadProcessId(m_hwnd, nullptr);
    if (winThread != 0 && winThread != GetCurrentThreadId()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, maximized]() {
            setMaximized(maximized);
        });
        return;
    }
    if (maximized) {
        SetPropW(m_hwnd, L"Tools3000_SearchMaximized", reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(m_hwnd, L"Tools3000_SearchMaximized");
    }
    if (IsWindowVisible(m_hwnd)) {
        if (IsIconic(m_hwnd)) {
            ShowWindow(m_hwnd, SW_RESTORE);
        }
        ShowWindow(m_hwnd, maximized ? SW_MAXIMIZE : SW_RESTORE);
    } else if (!maximized) {
        updatePlacement();
    }
    if (m_webView) {
        m_webView->ExecuteScript(
            maximized
                ? L"window.dispatchEvent(new CustomEvent('tools3000:searchMaximizedChanged', { detail: true }));"
                : L"window.dispatchEvent(new CustomEvent('tools3000:searchMaximizedChanged', { detail: false }));",
            nullptr);
    }
}

void SearchWindow::toggleMaximized() {
    const bool currentlyZoomed = (m_hwnd && IsWindow(m_hwnd)) ? (IsZoomed(m_hwnd) != FALSE) : m_isMaximized.load();
    setMaximized(!currentlyZoomed);
}

void SearchWindow::setPinned(bool pinned) {
    m_isPinned.store(pinned);
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const DWORD winThread = GetWindowThreadProcessId(m_hwnd, nullptr);
    if (winThread != 0 && winThread != GetCurrentThreadId()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, pinned]() {
            setPinned(pinned);
        });
        return;
    }
    if (pinned) {
        SetPropW(m_hwnd, L"Tools3000_SearchPinned", reinterpret_cast<HANDLE>(1));
    } else {
        RemovePropW(m_hwnd, L"Tools3000_SearchPinned");
    }
    SetWindowPos(m_hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void SearchWindow::dragMove() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    ReleaseCapture();
    PostMessageW(m_hwnd, WM_SYSCOMMAND, 0xF012, 0);
}

void SearchWindow::showSystemMenu(int screenX, int screenY) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    HMENU hMenu = GetSystemMenu(m_hwnd, FALSE);
    if (!hMenu) return;

    const bool zoomed = IsZoomed(m_hwnd) != FALSE;
    EnableMenuItem(hMenu, SC_RESTORE, MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, SC_MOVE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hMenu, SC_SIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hMenu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(hMenu, SC_MAXIMIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);

    POINT pt{};
    if (GetCursorPos(&pt)) {
        screenX = pt.x;
        screenY = pt.y;
    } else if (screenX == -1 || screenY == -1) {
        RECT rc{};
        GetWindowRect(m_hwnd, &rc);
        screenX = rc.left + 24;
        screenY = rc.top + 32;
    }

    SetForegroundWindow(m_hwnd);
    const int cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
        screenX, screenY, 0, m_hwnd, nullptr);
    if (cmd > 0) {
        PostMessageW(m_hwnd, WM_SYSCOMMAND, cmd, 0);
    }
}

void SearchWindow::persistNormalGeometry() {
    if (!m_hwnd || !IsWindow(m_hwnd) || IsIconic(m_hwnd) || IsZoomed(m_hwnd)) return;
    RECT rc{};
    if (!GetWindowRect(m_hwnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const int baseW = static_cast<int>(w / scale);
    const int baseH = static_cast<int>(h / scale);
    if (baseW >= 400 && baseH >= 250) {
        tools3000::core::ConfigManager::instance().set<int>("/search/windowWidth", baseW);
        tools3000::core::ConfigManager::instance().set<int>("/search/windowHeight", baseH);
        tools3000::core::ConfigManager::instance().set<int>("/search/windowX", rc.left);
        tools3000::core::ConfigManager::instance().set<int>("/search/windowY", rc.top);
    }
}

std::pair<int, int> SearchWindow::getWindowSize() const {
    const int w = tools3000::core::ConfigManager::instance().get<int>("/search/windowWidth", SearchWindowStyle::BaseWidth);
    const int h = tools3000::core::ConfigManager::instance().get<int>("/search/windowHeight", SearchWindowStyle::BaseHeight);
    return {w, h};
}

void SearchWindow::focusSearchIfVisible() {
    if (!isVisible()) return;
    if (m_controller) {
        m_controller->put_IsVisible(TRUE);
        if (m_hwnd && IsWindow(m_hwnd)) {
            syncWebViewDpi(m_controller.Get(), m_hwnd);
        }
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:focusSearch'));", nullptr);
    }
}

void SearchWindow::triggerQuickLookForSelection() {
    if (!isVisible()) return;
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:triggerQuickLook'));", nullptr);
    }
}

void SearchWindow::syncQuickLookForSelection() {
    if (!isVisible()) return;
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:syncQuickLook'));", nullptr);
    }
}

void SearchWindow::stepQuickLookSelection(int delta) {
    if (!isVisible() || !m_webView) return;
    const wchar_t* script = (delta < 0)
        ? L"window.dispatchEvent(new CustomEvent('tools3000:stepQuickLook', { detail: -1 }));"
        : L"window.dispatchEvent(new CustomEvent('tools3000:stepQuickLook', { detail: 1 }));";
    m_webView->ExecuteScript(script, nullptr);
}

void SearchWindow::preload(HINSTANCE hInstance) {
    if (m_hwnd && IsWindow(m_hwnd)) return;
    if (!createWindow(hInstance)) {
        LOG_ERROR("SearchWindow: preload createWindow failed");
        return;
    }
    initializeWebView2();
}

void SearchWindow::show(HINSTANCE hInstance) {
    const auto showStarted = std::chrono::steady_clock::now();
    const auto recordShown = [&showStarted]() {
        tools3000::core::PerformanceMonitor::instance().recordLatency(
            "search.hostShow",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - showStarted).count());
    };
    m_showTimeTick = GetTickCount64();
    // 先同步记录“原生窗口被显式唤起”，再异步启动服务。这样查询即使与
    // warmup 并发，也只能在这次用户动作之后补拉服务。
    tools3000::core::MessageBridge::instance().handleMessage(
        R"({"id":0,"method":"search.windowShown","params":{}})");
    warmUpSearchService();

    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, IDT_DEFERRED_SUSPEND);
        const bool wasHidden = !IsWindowVisible(m_hwnd);
        const bool wasMinimized = (IsIconic(m_hwnd) != FALSE);

        if (wasMinimized) {
            // 从任务栏最小化还原：自动保持最小化前的最大化或浮窗几何，不破坏用户分屏与尺寸
            ShowWindow(m_hwnd, SW_RESTORE);
        } else if (wasHidden) {
            updatePlacement();
            const bool wantMax = tools3000::core::ConfigManager::instance().get<bool>(
                "/search/windowMaximized", m_isMaximized.load());
            ShowWindow(m_hwnd, wantMax ? SW_SHOWMAXIMIZED : SW_SHOW);
        } else {
            ShowWindow(m_hwnd, SW_SHOW);
        }

        const HWND zInsertAfter = m_isPinned.load() ? HWND_TOPMOST : HWND_TOP;
        SetWindowPos(m_hwnd, zInsertAfter, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        BringWindowToTop(m_hwnd);
        SetForegroundWindow(m_hwnd);
        SetActiveWindow(m_hwnd);
        SetFocus(m_hwnd);
        m_visible = true;
        m_minimized = false;
        hookWebViewChildWindows(m_hwnd);
        if (m_webView) m_suspendController.resume(m_webView.Get(), "search");
        focusSearchIfVisible();
        recordShown();
        return;
    }

    if (!createWindow(hInstance)) {
        LOG_ERROR("SearchWindow: createWindow failed");
        return;
    }

    initializeWebView2();
    const bool wantMax = tools3000::core::ConfigManager::instance().get<bool>(
        "/search/windowMaximized", m_isMaximized.load());
    ShowWindow(m_hwnd, wantMax ? SW_SHOWMAXIMIZED : SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    m_visible = true;
    m_minimized = false;
    recordShown();
}

void SearchWindow::minimize() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const DWORD winThread = GetWindowThreadProcessId(m_hwnd, nullptr);
    if (winThread != 0 && winThread != GetCurrentThreadId()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            minimize();
        });
        return;
    }
    if (!IsWindowVisible(m_hwnd)) return;
    ShowWindow(m_hwnd, SW_MINIMIZE);
}

void SearchWindow::hide() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (!m_visible.load() && !IsWindowVisible(m_hwnd)) return;
    persistNormalGeometry();
    ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
    m_minimized = false;
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:searchWindowHidden'));", nullptr);
    }
    // 延迟 25 秒再执行 put_IsVisible(FALSE) + TrySuspend：高频连续呼出搜索时保留 GPU 合成表面，
    // 零黑屏瞬开；超 25 秒闲置后再挂起并由冷路径归还物理内存。
    SetTimer(m_hwnd, IDT_DEFERRED_SUSPEND, DEFERRED_SUSPEND_DELAY_MS, nullptr);
    tools3000::core::MessageBridge::instance().handleMessage(
        R"({"id":0,"method":"search.windowHidden","params":{}})");
}

bool SearchWindow::isVisible() const {
    return m_visible.load() && m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd);
}

bool SearchWindow::isMinimized() const {
    return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd) && (IsIconic(m_hwnd) != FALSE);
}

bool SearchWindow::isForegroundActive() const {
    if (!isVisible()) return false;
    const HWND fg = GetForegroundWindow();
    return fg == m_hwnd || (fg != nullptr && IsChild(m_hwnd, fg));
}

void SearchWindow::destroy() {
    ++m_generation;
    m_suspendController.abandon();
    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webView = nullptr;
    m_environment = nullptr;

    const HWND hwnd = std::exchange(m_hwnd, nullptr);
    if (hwnd && IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    m_visible = false;
    m_minimized = false;
    m_webViewReady = false;
}

bool SearchWindow::createWindow(HINSTANCE hInstance) {
    m_suspendController.reset();
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = SEARCH_WINDOW_CLASS;
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    RegisterClassExW(&wc);

    const bool initialMaximized =
        tools3000::core::ConfigManager::instance().get<bool>("/search/windowMaximized", false);
    m_isMaximized.store(initialMaximized);

    const int baseW = tools3000::core::ConfigManager::instance().get<int>("/search/windowWidth", SearchWindowStyle::BaseWidth);
    const int baseH = tools3000::core::ConfigManager::instance().get<int>("/search/windowHeight", SearchWindowStyle::BaseHeight);
    const int customX = tools3000::core::ConfigManager::instance().get<int>("/search/windowX", -99999);
    const int customY = tools3000::core::ConfigManager::instance().get<int>("/search/windowY", -99999);
    SIZE size{tools3000::core::dpi::scaleMetric(baseW, scale), tools3000::core::dpi::scaleMetric(baseH, scale)};

    const int margin = tools3000::core::dpi::scaleMetric(
        SearchWindowStyle::BaseScreenMargin, scale);
    size = tools3000::core::dpi::fitSizeToWorkArea(size, workArea, margin);
    int x = workArea.left + (workArea.right - workArea.left - size.cx) / 2;
    int y = workArea.top + (workArea.bottom - workArea.top - size.cy) / 2;

    if (customX != -99999 && customY != -99999) {
        if (customX >= workArea.left - 100 && customX + size.cx <= workArea.right + 100 &&
            customY >= workArea.top - 20 && customY + size.cy <= workArea.bottom + 100) {
            x = customX;
            y = customY;
        }
    }

    const bool zh = tools3000::core::isUiLanguageChinese();
    const std::wstring windowTitle = zh ? L"Tools3000 搜索" : L"Tools3000 Search";

    // 原生独立应用窗口样式 (WS_EX_APPWINDOW + WS_OVERLAPPEDWINDOW)：
    // 完整支持任务栏按钮、Alt+Tab、最小化/恢复、Win+方向键 Aero Snap 分屏、
    // 拖动最大化标题栏脱离 (Aero Tear-Off) 与右键系统窗口菜单，同时默认不强行压顶遮挡其他软件。
    m_hwnd = CreateWindowExW(
        NativeWindowExStyle,
        SEARCH_WINDOW_CLASS,
        windowTitle.c_str(),
        NativeWindowStyle,
        x, y, size.cx, size.cy,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!m_hwnd) {
        LOG_ERROR("SearchWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    if (initialMaximized) {
        SetPropW(m_hwnd, L"Tools3000_SearchMaximized", reinterpret_cast<HANDLE>(1));
    }

    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    if (wc.hIcon) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    }
    if (wc.hIconSm) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    }

    // 启用 DWM 全客户区扩展与跨平台通用圆角裁剪，彻底消除系统传统标题栏并全兼容 Win11/Win10/Server
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, size.cx, size.cy, radius);

    return true;
}

void SearchWindow::updatePlacement() {
    if (!m_hwnd || m_updatingPlacement || IsIconic(m_hwnd) || IsZoomed(m_hwnd)) return;
    m_updatingPlacement = true;
    const HMONITOR monitor = IsWindowVisible(m_hwnd)
        ? MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST)
        : tools3000::core::dpi::activeMonitor();
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    const int baseW = tools3000::core::ConfigManager::instance().get<int>("/search/windowWidth", SearchWindowStyle::BaseWidth);
    const int baseH = tools3000::core::ConfigManager::instance().get<int>("/search/windowHeight", SearchWindowStyle::BaseHeight);
    const int customX = tools3000::core::ConfigManager::instance().get<int>("/search/windowX", -99999);
    const int customY = tools3000::core::ConfigManager::instance().get<int>("/search/windowY", -99999);
    SIZE size{tools3000::core::dpi::scaleMetric(baseW, scale), tools3000::core::dpi::scaleMetric(baseH, scale)};

    const int margin = tools3000::core::dpi::scaleMetric(
        SearchWindowStyle::BaseScreenMargin, scale);
    size = tools3000::core::dpi::fitSizeToWorkArea(size, workArea, margin);
    int x = workArea.left + (workArea.right - workArea.left - size.cx) / 2;
    int y = workArea.top + (workArea.bottom - workArea.top - size.cy) / 2;

    if (customX != -99999 && customY != -99999) {
        if (customX >= workArea.left - 100 && customX + size.cx <= workArea.right + 100 &&
            customY >= workArea.top - 20 && customY + size.cy <= workArea.bottom + 100) {
            x = customX;
            y = customY;
        }
    }

    const HWND zInsertAfter = m_isPinned.load() ? HWND_TOPMOST : HWND_NOTOPMOST;
    SetWindowPos(m_hwnd, zInsertAfter, x, y, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, size.cx, size.cy, radius);
    if (m_controller) {
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }
    m_updatingPlacement = false;
}

void SearchWindow::initializeWebView2() {
    const uint64_t generation = ++m_generation;
    WebViewEnvironmentManager::instance().acquire(
            [this, generation](HRESULT result, ICoreWebView2Environment* env) {
                if (FAILED(result) || !env) {
                    LOG_ERROR("SearchWindow: shared environment unavailable.");
                    return;
                }
                if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                    return;
                }
                m_environment = env;
                const HRESULT controllerResult = m_environment->CreateCoreWebView2Controller(
                    m_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, generation](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) return E_FAIL;
                            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                                controller->Close();
                                return E_ABORT;
                            }
                            m_controller = controller;
                            const HRESULT hrGetWeb = m_controller->get_CoreWebView2(&m_webView);
                            if (FAILED(hrGetWeb) || !m_webView) {
                                LOG_WARN("SearchWindow: get_CoreWebView2 失败: HRESULT=0x{:08X}", static_cast<unsigned>(hrGetWeb));
                                return E_FAIL;
                            }
                            m_controller->put_IsVisible(m_visible.load() ? TRUE : FALSE);

                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            const HRESULT hrGetSettings = m_webView->get_Settings(&settings);
                            if (SUCCEEDED(hrGetSettings) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);

                                Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                }
                            }
                            
                            // 使用与当前主题一致的不透明实色背景，彻底杜绝最大化或 TrySuspend 唤醒空窗期透明露底
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(m_controller.As(&controller2))) {
                                const COLORREF bg = searchWindowBackdropColor();
                                COREWEBVIEW2_COLOR solidBg = {
                                    255, GetRValue(bg), GetGValue(bg), GetBValue(bg)
                                };
                                controller2->put_DefaultBackgroundColor(solidBg);
                            }

                            RECT bounds;
                            GetClientRect(m_hwnd, &bounds);
                            m_controller->put_Bounds(bounds);
                            syncWebViewDpi(m_controller.Get(), m_hwnd);
                            if (m_visible.load()) {
                                m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                            }

                            // ── 本地打包模式: 设置虚拟主机映射 ──────────────────────────────────
                            auto uiFolder = (tools3000::core::WinUtils::getExeDirectory() / L"ui").wstring();
                            std::error_code ec;
                            if (std::filesystem::exists(uiFolder, ec)) {
                                Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
                                if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
                                    webView3->SetVirtualHostNameToFolderMapping(
                                        L"tools3000.local", uiFolder.c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                }
                            }

                            web_security::applyNavigationPolicy(m_webView.Get());
                            KeyboardPipeline::applyWebKeyboardPolicy(m_controller.Get());

                            // ── 动态获取 UI 基础地址 ──────────────────────────────────────────
                            auto exeDir = tools3000::core::WinUtils::getExeDirectory();
                            auto indexPath = exeDir / L"ui" / L"index.html";
                            std::wstring baseUrl;
                            if (std::filesystem::exists(indexPath, ec)) {
                                baseUrl = L"https://tools3000.local/index.html";
                            } else {
#ifdef _DEBUG
                                auto devUrlPath = exeDir.parent_path().parent_path().parent_path() / L"ui" / L".dev-server-url";
                                if (std::filesystem::exists(devUrlPath, ec)) {
                                    std::ifstream file(devUrlPath);
                                    std::string url;
                                    if (std::getline(file, url) && !url.empty()) {
                                        baseUrl = tools3000::core::WinUtils::utf8ToWstring(url);
                                    }
                                }
                                if (baseUrl.empty()) baseUrl = L"http://localhost:5173";
#else
                                LOG_ERROR("SearchWindow: 打包 UI 缺失，已拒绝连接开发服务器");
                                baseUrl = L"https://tools3000.local/index.html";
#endif
                            }

                            // Load the search URL
                            std::wstring targetUrl = baseUrl + L"#/search";
                            // Setup JS bridge before Navigate so initial document creation always receives the listener
                            m_webView->AddScriptToExecuteOnDocumentCreated(
                                L"window.chrome.webview.addEventListener('message', event => {"
                                L"  const msg = event.data;"
                                L"  if (window.tools3000Bridge && window.tools3000Bridge.onMessage) {"
                                L"      window.tools3000Bridge.onMessage(msg);"
                                L"  }"
                                L"});", nullptr);

                            m_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this, generation](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        try {
                                            if (!web_security::isTrustedMessageSource(args)) return S_OK;
                                            if (generation != m_generation.load()) return S_OK;
                                            PWSTR messageRaw = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&messageRaw)) && messageRaw) {
                                                 const std::string request = tools3000::core::WinUtils::wstringToUtf8(messageRaw);
                                                 CoTaskMemFree(messageRaw);
                                                 if (!web_security::isBridgeMethodAllowed(
                                                         request, web_security::Surface::Search)) return S_OK;
                                                // 搜索类请求要跨进程等待索引服务。同步执行会冻结 WebView2 的
                                                // UI 线程，让搜索框在整个等待期间无法接收键盘输入，因此改为
                                                // 在线程池中处理，完成后再回到 UI 线程投递响应。
                                                tools3000::core::MessageBridge::instance().handleMessageAsync(
                                                    request,
                                                    [this, generation](std::string response) {
                                                        tools3000::core::MainThreadDispatcher::instance().post(
                                                            [this, generation, response = std::move(response)]() {
                                                                if (generation != m_generation.load() || !m_webView) return;
                                                                const std::wstring wideResponse =
                                                                    tools3000::core::WinUtils::utf8ToWstring(response);
                                                                m_webView->PostWebMessageAsString(wideResponse.c_str());
                                                            });
                                                    });
                                            }
                                        } catch (const std::exception& e) {
                                            LOG_ERROR("SearchWindow bridge error: {}", e.what());
                                        } catch (...) {
                                            LOG_ERROR("SearchWindow bridge unknown error");
                                        }
                                        return S_OK;
                                    }
                                ).Get(), nullptr);

                            // 导航完成后标记就绪。后台 preload 不得发送聚焦事件，且应立即挂起
                            // Chromium renderer 以确保隐藏态零 CPU；只有可见窗口才同步聚焦输入框。
                            m_webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        if (generation != m_generation.load()) return S_OK;
                                        m_webViewReady = true;
                                        hookWebViewChildWindows(m_hwnd);
                                        if (m_visible.load()) {
                                            focusSearchIfVisible();
                                        } else if (m_controller && m_webView) {
                                            m_controller->put_IsVisible(FALSE);
                                            m_suspendController.requestSuspend(m_webView.Get(), "search");
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            m_webView->Navigate(targetUrl.c_str());
                            hookWebViewChildWindows(m_hwnd);
                            return S_OK;
                        }
                    ).Get());
                if (FAILED(controllerResult)) {
                    LOG_ERROR("SearchWindow: Create controller request failed, hr=0x{:08X}",
                              static_cast<unsigned>(controllerResult));
                }
            });
}


LRESULT CALLBACK SearchWindow::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    auto& inst = SearchWindow::instance();
    LRESULT ncResult = 0;
    if (tools3000::core::WinUtils::handleFramelessNonClientMessage(hwnd, uMsg, wParam, lParam, ncResult)) {
        return ncResult;
    }
    switch (uMsg) {
        case WM_ERASEBKGND:
            // 拦截背景擦除，由 WM_PAINT 与 WebView2 接管绘制，消除底衬微闪
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            // 填充当前主题底色，防止最大化或后台休眠唤醒首帧到达前露底或残留旧帧
            HBRUSH hBrush = CreateSolidBrush(searchWindowBackdropColor());
            if (hBrush) {
                FillRect(hdc, &ps.rcPaint, hBrush);
                DeleteObject(hBrush);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCCALCSIZE: {
            if (wParam) {
                if (IsZoomed(hwnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hMon, &mi)) {
                        params->rgrc[0] = mi.rcWork;
                    }
                }
            }
            // 无论 wParam 为 TRUE 或 FALSE 均保持全客户区，彻底消除系统传统标题栏与最大化 8px 溢出边框
            return 0;
        }
        case WM_NCHITTEST: {
            if (!IsZoomed(hwnd)) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT rc;
                GetWindowRect(hwnd, &rc);
                const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                return calculateHitTest(rc, pt, scale);
            }
            return HTCLIENT;
        }
        case WM_MOVE: {
            if (inst.m_controller) {
                inst.m_controller->NotifyParentWindowPositionChanged();
            }
            break;
        }
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                inst.m_minimized.store(true);
                if (inst.m_webView) {
                    inst.m_webView->ExecuteScript(
                        L"window.dispatchEvent(new CustomEvent('tools3000:searchWindowHidden'));", nullptr);
                }
                tools3000::core::MessageBridge::instance().handleMessage(
                    R"({"id":0,"method":"search.cancel","params":{}})");
                if (inst.m_controller) {
                    inst.m_controller->put_IsVisible(FALSE);
                }
                if (inst.m_webView) {
                    // 最小化到任务栏期间冻结渲染器零 CPU，同时完整保留当前搜索结果与滚动位置
                    inst.m_suspendController.requestSuspend(inst.m_webView.Get(), "search");
                }
                return 0;
            }

            const bool restoredFromMinimize = inst.m_minimized.exchange(false);
            if (restoredFromMinimize && inst.m_webView) {
                inst.m_suspendController.resume(inst.m_webView.Get(), "search");
            }

            const bool maxState = (wParam == SIZE_MAXIMIZED || IsZoomed(hwnd));
            const bool prevMax = inst.m_isMaximized.exchange(maxState);
            if (maxState) {
                SetPropW(hwnd, L"Tools3000_SearchMaximized", reinterpret_cast<HANDLE>(1));
            } else {
                RemovePropW(hwnd, L"Tools3000_SearchMaximized");
            }
            if (IsWindowVisible(hwnd) && prevMax != maxState) {
                tools3000::core::ConfigManager::instance().set<bool>("/search/windowMaximized", maxState);
            }
            if (inst.m_webView && (prevMax != maxState || restoredFromMinimize)) {
                inst.m_webView->ExecuteScript(
                    maxState
                        ? L"window.dispatchEvent(new CustomEvent('tools3000:searchMaximizedChanged', { detail: true }));"
                        : L"window.dispatchEvent(new CustomEvent('tools3000:searchMaximizedChanged', { detail: false }));",
                    nullptr);
            }

            const int newW = LOWORD(lParam);
            const int newH = HIWORD(lParam);
            if (newW > 0 && newH > 0) {
                if (maxState) {
                    applySquareCornersForMaximized(hwnd);
                } else {
                    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
                    tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, newW, newH, radius);
                }
            }
            hookWebViewChildWindows(hwnd);
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
                if (IsWindowVisible(hwnd)) {
                    inst.m_controller->put_IsVisible(TRUE);
                }
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            LPMINMAXINFO mmi = reinterpret_cast<LPMINMAXINFO>(lParam);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
            const float scale = tools3000::core::dpi::scaleForDpi(dpi);
            mmi->ptMinTrackSize.x = static_cast<LONG>(480 * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(320 * scale);
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            inst.m_inSizeMove.store(true);
            break;
        case WM_EXITSIZEMOVE: {
            inst.m_inSizeMove.store(false);
            if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
                RECT rc;
                if (GetWindowRect(hwnd, &rc)) {
                    const int w = rc.right - rc.left;
                    const int h = rc.bottom - rc.top;
                    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
                    const float scale = tools3000::core::dpi::scaleForDpi(dpi);
                    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
                    tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, w, h, radius);
                }
                inst.persistNormalGeometry();
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
            }
            break;
        }
        case WM_DPICHANGED: {
            if (IsZoomed(hwnd)) {
                if (inst.m_controller) {
                    syncWebViewDpi(inst.m_controller.Get(), hwnd);
                }
                return 0;
            }
            if (!inst.m_updatingPlacement && lParam) {
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RECT curRc{};
            if (GetWindowRect(hwnd, &curRc)) {
                const int curW = curRc.right - curRc.left;
                const int curH = curRc.bottom - curRc.top;
                if (curW > 0 && curH > 0) {
                    const unsigned newDpi = HIWORD(wParam) > 0
                        ? static_cast<unsigned>(HIWORD(wParam))
                        : tools3000::core::dpi::effectiveDpiForWindow(hwnd);
                    const float scale = tools3000::core::dpi::scaleForDpi(newDpi);
                    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
                    tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, curW, curH, radius);
                }
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
                inst.updatePlacement();
            }
            return 0;
        case WM_ACTIVATE: {
            // 纯独立桌面窗口模式：失焦时停留在后台和任务栏，不再自动隐藏；切回前台时抢跑唤醒渲染器、恢复控制器可见与焦点同步，
            // 并通知前端感知前后台切换以对重型全盘内容扫描执行失焦软暂停，绝不拖慢前台其他应用或 OCR。
            const bool active = (LOWORD(wParam) != WA_INACTIVE);
            if (active && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                if (inst.m_webView) {
                    inst.m_suspendController.resume(inst.m_webView.Get(), "search");
                }
                if (inst.m_controller) {
                    inst.m_controller->put_IsVisible(TRUE);
                }
            }
            if (inst.m_webView) {
                inst.m_webView->ExecuteScript(
                    active
                        ? L"window.dispatchEvent(new CustomEvent('tools3000:searchWindowActiveChanged', { detail: true }));"
                        : L"window.dispatchEvent(new CustomEvent('tools3000:searchWindowActiveChanged', { detail: false }));",
                    nullptr);
            }
            break;
        }
        case WM_SETFOCUS:
            if (inst.m_controller && inst.isVisible()) {
                inst.m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_TIMER: {
            if (wParam == IDT_DEFERRED_SUSPEND && !inst.m_visible.load()) {
                KillTimer(hwnd, IDT_DEFERRED_SUSPEND);
                if (inst.m_controller) inst.m_controller->put_IsVisible(FALSE);
                if (inst.m_webView) inst.m_suspendController.requestSuspend(inst.m_webView.Get(), "search");
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            inst.hide();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                inst.hide();
                return 0;
            }
            break;
        case WM_SYSCOMMAND:
        case WM_HELP: {
            if (KeyboardPipeline::filterWindowMessage(hwnd, uMsg, wParam, lParam)) {
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            if (inst.m_hwnd == hwnd) inst.m_hwnd = nullptr;
            inst.destroy();
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace tools3000::ui
