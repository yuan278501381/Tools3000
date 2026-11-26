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
// 的第一个可靠信号，此时把服务叫醒，启动耗时正好被用户的输入过程盖掉；
// 若用户将默认筛选分类设为纯离线模式（app / bookmark），则跳过提前唤醒，省去 100% 后台索引服务开销。
void warmUpSearchService() {
    const std::string defaultCategory =
        tools3000::core::ConfigManager::instance().get<std::string>("/search/defaultCategory", "all");
    if (!SearchWindow::shouldWarmUpSearchServiceOnShow(defaultCategory)) {
        return;
    }
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
    setMenuActive(true);
    const int cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
        screenX, screenY, 0, m_hwnd, nullptr);
    setMenuActive(false);
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

void SearchWindow::presentWebViewSurface(bool forceCompositorKick) {
    if (!m_hwnd || !IsWindow(m_hwnd) || !m_controller) return;
    if (m_webView) {
        m_suspendController.resume(m_webView.Get(), "search");
    }
    const bool needKick = shouldTriggerCompositorSwapKick(forceCompositorKick, m_wasSuspended);
    m_wasSuspended = false;

    RECT rc{};
    const bool hasRect = (GetClientRect(m_hwnd, &rc) != FALSE) && (rc.right > rc.left) && (rc.bottom > rc.top);
    if (needKick) {
        // 仅在 TrySuspend 挂起恢复或显式强刷时执行 1px 脏区脉冲，避免在崭新首帧上砸毁 SwapChain 产生纯黑闪帧
        m_controller->put_IsVisible(FALSE);
        if (hasRect && (rc.bottom - rc.top) > 2) {
            RECT pulseRc = rc;
            pulseRc.bottom -= 1;
            m_controller->put_Bounds(pulseRc);
        }
    }
    if (hasRect) {
        m_controller->put_Bounds(rc);
    }
    syncWebViewDpi(m_controller.Get(), m_hwnd);
    m_controller->put_IsVisible(TRUE);
    m_controller->NotifyParentWindowPositionChanged();
    m_everPresentedVisible = true;
    SetTimer(m_hwnd, IDT_POST_SHOW_REPAINT_VERIFY, POST_SHOW_REPAINT_VERIFY_MS, nullptr);
}

void SearchWindow::focusSearchIfVisible() {
    if (!isVisible()) return;
    if (!m_webViewReady.load(std::memory_order_acquire)) {
        if (m_hwnd && IsWindow(m_hwnd)) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }
    presentWebViewSurface(false);
    if (m_controller) {
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:focusSearch'));", nullptr);
    }
}

void SearchWindow::markFirstPaintReady() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        const DWORD winThread = GetWindowThreadProcessId(m_hwnd, nullptr);
        if (winThread != 0 && winThread != GetCurrentThreadId()) {
            tools3000::core::MainThreadDispatcher::instance().post([this]() {
                markFirstPaintReady();
            });
            return;
        }
    }
    completeFirstPaintReady();
}

bool SearchWindow::setWindowCloaked(bool cloaked, bool startUncloakTimer) {
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        m_cloakedForFirstPaint = false;
        return false;
    }
    if (!cloaked) {
        KillTimer(m_hwnd, IDT_FIRST_PAINT_UNCLOAK);
        if (!m_cloakedForFirstPaint) return true;
        const BOOL val = FALSE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &val, sizeof(val));
        m_cloakedForFirstPaint = false;
        return true;
    }
    const BOOL val = TRUE;
    const HRESULT hr = DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &val, sizeof(val));
    if (SUCCEEDED(hr)) {
        m_cloakedForFirstPaint = true;
        if (startUncloakTimer) {
            SetTimer(m_hwnd, IDT_FIRST_PAINT_UNCLOAK, FIRST_PAINT_UNCLOAK_TIMEOUT_MS, nullptr);
        } else {
            KillTimer(m_hwnd, IDT_FIRST_PAINT_UNCLOAK);
        }
        return true;
    }
    m_cloakedForFirstPaint = false;
    return false;
}

void SearchWindow::startBackgroundFirstPaintWarmup() {
    if (!m_hwnd || !IsWindow(m_hwnd) || !m_controller) return;
    if (!shouldStartBackgroundFirstPaintWarmup(
            m_visible.load(std::memory_order_acquire),
            m_webViewReady.load(std::memory_order_acquire),
            m_uiFirstPaintReady.load(std::memory_order_acquire),
            m_preloadWarmingFrame)) {
        return;
    }
    // 必须先成功开启 DWMWA_CLOAK 完全隐身才允许在后台推进真实 GPU 首帧，防止非 DWM 环境闪烁
    if (!setWindowCloaked(true, /*startUncloakTimer=*/false)) {
        LOG_DEBUG("SearchWindow: DWMWA_CLOAK unavailable, skipping silent background first-paint warmup");
        return;
    }
    // 临时切换为 TOOLWINDOW + NOACTIVATE：任务栏与 Alt+Tab 零闪烁、绝不抢占当前前台焦点
    const LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    const LONG_PTR warmupEx = (exStyle & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                              static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, warmupEx);
    m_preloadWarmingFrame = true;

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    RECT rc{};
    if (GetClientRect(m_hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
        m_controller->put_Bounds(rc);
    }
    syncWebViewDpi(m_controller.Get(), m_hwnd);
    m_controller->put_IsVisible(TRUE);
    m_controller->NotifyParentWindowPositionChanged();
    SetTimer(m_hwnd, IDT_PRELOAD_WARMUP_TIMEOUT, PRELOAD_WARMUP_TIMEOUT_MS, nullptr);
    LOG_INFO("SearchWindow: silent background GPU first-paint warmup started (elapsedSinceInit={}ms)",
             m_initStartTick > 0 ? (GetTickCount64() - m_initStartTick) : 0ULL);
}

void SearchWindow::finishBackgroundFirstPaintWarmup(bool reachedFirstPaint, const char* reason) {
    if (!m_preloadWarmingFrame) return;
    m_preloadWarmingFrame = false;
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    KillTimer(m_hwnd, IDT_PRELOAD_WARMUP_TIMEOUT);

    // 趁 DWMWA_CLOAK 仍为 TRUE 时先 SW_HIDE 再还原 WS_EX_APPWINDOW，确保任务栏和屏幕全程零闪烁
    ShowWindow(m_hwnd, SW_HIDE);
    const LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    const LONG_PTR restoredEx = (exStyle & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) |
                                static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, restoredEx);
    setWindowCloaked(false);

    if (reachedFirstPaint) {
        m_everPresentedVisible = true;
        if (m_controller) {
            m_controller->put_IsVisible(FALSE);
        }
        SetTimer(m_hwnd, IDT_DEFERRED_SUSPEND, DEFERRED_SUSPEND_DELAY_MS, nullptr);
    } else if (m_controller) {
        m_controller->put_IsVisible(FALSE);
    }
    LOG_INFO("SearchWindow: silent background GPU warmup finished (reachedFirstPaint={}, reason={}, elapsedSinceInit={}ms)",
             reachedFirstPaint, reason ? reason : "unknown",
             m_initStartTick > 0 ? (GetTickCount64() - m_initStartTick) : 0ULL);
}

void SearchWindow::completeFirstPaintReady() {
    if (!m_hwnd || !IsWindow(m_hwnd) || !m_controller) return;
    KillTimer(m_hwnd, IDT_FIRST_PAINT_FALLBACK);
    const bool wasCloaked = m_cloakedForFirstPaint;
    const bool wasPreloadWarming = m_preloadWarmingFrame;
    m_uiFirstPaintReady.store(true, std::memory_order_release);
    setWebViewStatus(WebViewStatus::Ready, S_OK);
    if (wasPreloadWarming && !m_visible.load(std::memory_order_acquire)) {
        finishBackgroundFirstPaintWarmup(true, "firstPaintReady");
        return;
    }
    setWindowCloaked(false);
    if (m_visible.load(std::memory_order_acquire)) {
        KillTimer(m_hwnd, IDT_DEFERRED_SUSPEND);
        const uint64_t showElapsed = (m_showTimeTick > 0) ? (GetTickCount64() - m_showTimeTick) : 0ULL;
        LOG_INFO("SearchWindow: firstPaintReady committed (wasCloaked={}, showToPaintMs={})",
                 wasCloaked, showElapsed);
        if (wasCloaked) {
            // 原子揭幕瞬间轻推一次焦点，确保从 Cloak 释放后输入框光标 100% 就位
            m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        } else {
            focusSearchIfVisible();
        }
    } else if (m_everPresentedVisible) {
        // 仅在窗口至少完整向用户渲染过一次真实帧后才进入延迟挂起，
        // 杜绝后台 preload 在 0 帧隐藏态即被 TrySuspend 冻死导致首次 Alt+Space 呼出黑屏。
        m_controller->put_IsVisible(FALSE);
        SetTimer(m_hwnd, IDT_DEFERRED_SUSPEND, DEFERRED_SUSPEND_DELAY_MS, nullptr);
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
    // 先同步记录“原生窗口被显式唤起”，再决定是否立即唤醒搜索服务。
    // 若 WebView2 已热就绪，0ms 立即唤醒服务；若 WebView2 尚在冷启动（例如 Win10 LTSC / 机械盘），
    // 先把磁盘 I/O 让给 WebView2 首屏加载，待导航完成（或 1.5s 兜底定时器）后再唤醒索引服务，
    // 避免两者在机械盘上互掐造成长时间黑屏。
    tools3000::core::MessageBridge::instance().handleMessage(
        R"({"id":0,"method":"search.windowShown","params":{}})");
    if (m_webViewReady.load(std::memory_order_acquire)) {
        m_pendingServiceWarmup.store(false, std::memory_order_release);
        warmUpSearchService();
    } else {
        m_pendingServiceWarmup.store(true, std::memory_order_release);
    }

    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, IDT_DEFERRED_SUSPEND);
        KillTimer(m_hwnd, IDT_DEFERRED_BLUR_HIDE);
        // 若后台无感首帧预热正在进行中，用户恰好按快捷键唤起：立即无缝接管并恢复 WS_EX_APPWINDOW 任务栏样式
        if (m_preloadWarmingFrame) {
            KillTimer(m_hwnd, IDT_PRELOAD_WARMUP_TIMEOUT);
            m_preloadWarmingFrame = false;
            ShowWindow(m_hwnd, SW_HIDE);
            const LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
            const LONG_PTR restoredEx = (exStyle & ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) |
                                        static_cast<LONG_PTR>(WS_EX_APPWINDOW);
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, restoredEx);
            LOG_INFO("SearchWindow: user show() seamlessly took over in-flight background warmup");
        }
        // 若此前 WebView2 初始化曾失败或渲染器崩溃且当前未在初始化，立即自动重试恢复，杜绝永久黑屏死胡同
        if (!m_webView && !m_webViewInitializing.load(std::memory_order_acquire)) {
            m_autoRecoverAttempted.store(false, std::memory_order_release);
            initializeWebView2();
        }
        if (m_pendingServiceWarmup.load(std::memory_order_acquire)) {
            SetTimer(m_hwnd, IDT_COLD_WARMUP_FALLBACK, COLD_WARMUP_FALLBACK_DELAY_MS, nullptr);
        }
        const bool wasHidden = !IsWindowVisible(m_hwnd);
        const bool wasMinimized = (IsIconic(m_hwnd) != FALSE);
        const bool webReady = m_webViewReady.load(std::memory_order_acquire);
        const bool firstPaintDone = m_uiFirstPaintReady.load(std::memory_order_acquire);
        const bool wasSuspendedBeforeShow = m_wasSuspended;
        const bool needCloak = shouldCloakOnShow(webReady, firstPaintDone, wasSuspendedBeforeShow, wasMinimized);
        if (needCloak) {
            // 原生 DWMWA_CLOAK 隐身热身（配合 --disable-backgrounding-occluded-windows 不触发 Chromium 遮挡节流），
            // 在 ShowWindow 之前屏蔽未提交帧输出，待前端双 rAF 确认首帧/唤醒帧完成后原子揭幕。
            setWindowCloaked(true, /*startUncloakTimer=*/true);
        }

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
        if (!webReady) {
            setWindowCloaked(false);
            if (m_controller) m_controller->put_IsVisible(FALSE);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        } else {
            // 仅在真正挂起或最小化恢复时触发合成器强制刷新脉冲，严禁砸毁刚预热好的崭新 SwapChain
            presentWebViewSurface(wasSuspendedBeforeShow || wasMinimized);
            if (needCloak && firstPaintDone && m_webView) {
                // 已完成首帧但经历过挂起/最小化：请求前端在重绘 1 帧 rAF 后触发原子揭幕
                m_webView->ExecuteScript(
                    L"window.dispatchEvent(new CustomEvent('tools3000:searchResumeFrame'));", nullptr);
            }
        }
        LOG_INFO("SearchWindow: show() completed (webReady={}, firstPaintDone={}, wasSuspended={}, needCloak={})",
                 webReady, firstPaintDone, wasSuspendedBeforeShow, needCloak);
        focusSearchIfVisible();
        recordShown();
        return;
    }

    if (!createWindow(hInstance)) {
        LOG_ERROR("SearchWindow: createWindow failed");
        return;
    }

    m_autoRecoverAttempted.store(false, std::memory_order_release);
    initializeWebView2();
    if (m_pendingServiceWarmup.load(std::memory_order_acquire)) {
        SetTimer(m_hwnd, IDT_COLD_WARMUP_FALLBACK, COLD_WARMUP_FALLBACK_DELAY_MS, nullptr);
    }
    const bool wantMax = tools3000::core::ConfigManager::instance().get<bool>(
        "/search/windowMaximized", m_isMaximized.load());
    ShowWindow(m_hwnd, wantMax ? SW_SHOWMAXIMIZED : SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    m_visible = true;
    m_minimized = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
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
    setWindowCloaked(false);
    ShowWindow(m_hwnd, SW_MINIMIZE);
}

void SearchWindow::hide() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    KillTimer(m_hwnd, IDT_DEFERRED_BLUR_HIDE);
    KillTimer(m_hwnd, IDT_POST_SHOW_REPAINT_VERIFY);
    setWindowCloaked(false);
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
    m_wasSuspended = false;
    m_everPresentedVisible = false;
    m_preloadWarmingFrame = false;
    if (m_hwnd && IsWindow(m_hwnd)) {
        setWindowCloaked(false);
        KillTimer(m_hwnd, IDT_POST_SHOW_REPAINT_VERIFY);
        KillTimer(m_hwnd, IDT_DEFERRED_BLUR_HIDE);
        KillTimer(m_hwnd, IDT_FIRST_PAINT_FALLBACK);
        KillTimer(m_hwnd, IDT_PRELOAD_WARMUP_TIMEOUT);
    }
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
    m_uiFirstPaintReady = false;
    m_webViewInitializing = false;
    m_pendingServiceWarmup = false;
    m_webViewStatus.store(static_cast<uint8_t>(WebViewStatus::Loading), std::memory_order_release);
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

void SearchWindow::setWebViewStatus(WebViewStatus status, HRESULT hr) {
    m_webViewStatus.store(static_cast<uint8_t>(status), std::memory_order_release);
    if (FAILED(hr)) {
        m_lastWebViewErrorHr.store(static_cast<uint32_t>(hr), std::memory_order_release);
    } else if (status == WebViewStatus::Ready || status == WebViewStatus::Loading) {
        m_lastWebViewErrorHr.store(0u, std::memory_order_release);
    }
    if (status == WebViewStatus::MissingRuntime ||
        status == WebViewStatus::InitFailed ||
        status == WebViewStatus::RenderCrashed) {
        // 异常/降级提示面必须立即可见，绝不允许停留在 DWM Cloak 隐身态
        setWindowCloaked(false);
    }
    if (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void SearchWindow::paintStatusBackdrop(HDC hdc, const RECT& clientRect) const {
    const bool dark = isSearchDarkTheme();
    const COLORREF bg = searchWindowBackdropColor();
    HBRUSH hBrush = CreateSolidBrush(bg);
    if (hBrush) {
        FillRect(hdc, &clientRect, hBrush);
        DeleteObject(hBrush);
    }

    const auto status = static_cast<WebViewStatus>(m_webViewStatus.load(std::memory_order_acquire));
    if (status == WebViewStatus::Ready) {
        return;
    }

    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width < 120 || height < 80) return;

    const float scale = m_hwnd ? tools3000::core::dpi::scaleForWindow(m_hwnd) : 1.0f;
    const bool zh = tools3000::core::isUiLanguageChinese();
    const COLORREF titleColor = dark ? RGB(241, 245, 249) : RGB(15, 23, 42);
    const COLORREF subColor = dark ? RGB(148, 163, 184) : RGB(71, 85, 105);
    const COLORREF accentColor = dark ? RGB(96, 165, 250) : RGB(37, 99, 235);

    // Loading 状态绘制与真实搜索界面 1:1 同色系的原生圆角搜索框骨架轮廓，
    // 即使在机械盘冷启瞬间用户手速极快抢先呼出，也绝对不会看到冷硬黑框。
    if (status == WebViewStatus::Loading && width >= 260 && height >= 160) {
        const int padTop = tools3000::core::dpi::scaleMetric(16, scale);
        const int padSide = tools3000::core::dpi::scaleMetric(18, scale);
        const int barH = tools3000::core::dpi::scaleMetric(44, scale);
        const int corner = tools3000::core::dpi::scaleMetric(10, scale);
        const COLORREF barBg = dark ? RGB(30, 41, 59) : RGB(255, 255, 255);
        const COLORREF barBorder = dark ? RGB(51, 65, 85) : RGB(203, 213, 225);
        HPEN borderPen = CreatePen(PS_SOLID, 1, barBorder);
        HBRUSH barBrush = CreateSolidBrush(barBg);
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, barBrush);
        RoundRect(hdc,
                  clientRect.left + padSide,
                  clientRect.top + padTop,
                  clientRect.right - padSide,
                  clientRect.top + padTop + barH,
                  corner, corner);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        if (borderPen) DeleteObject(borderPen);
        if (barBrush) DeleteObject(barBrush);
    }

    std::wstring titleText;
    std::wstring descText;
    std::wstring actionText;
    const uint32_t errHr = m_lastWebViewErrorHr.load(std::memory_order_acquire);

    switch (status) {
        case WebViewStatus::MissingRuntime:
            titleText = zh ? L"缺少 Microsoft Edge WebView2 运行时" : L"Microsoft Edge WebView2 Runtime Required";
            descText = zh
                ? L"当前 Windows 系统（常见于 Windows 10 LTSC 精简版）未预装 WebView2 运行时，因此搜索界面暂时无法渲染。"
                : L"WebView2 Runtime is not installed on this Windows system (common on Windows 10 LTSC), so the Search UI cannot render yet.";
            actionText = zh
                ? L"按 Enter 或点击窗口打开微软官方下载页安装  |  按 Esc 关闭"
                : L"Press Enter or click here to open Microsoft's download page  |  Esc to close";
            break;
        case WebViewStatus::RenderCrashed:
        case WebViewStatus::InitFailed: {
            wchar_t hexBuf[24]{};
            if (errHr != 0) {
                swprintf_s(hexBuf, L" (0x%08X)", errHr);
            }
            titleText = (zh ? L"搜索界面加载受阻" : L"Search Interface Failed to Load") + std::wstring(hexBuf);
            descText = zh
                ? L"WebView2 渲染进程启动超时或异常中止（机械硬盘首次冷启动或驱动兼容问题可能引发）。"
                : L"The WebView2 renderer failed to initialize or stopped unexpectedly (slow HDD cold start or GPU driver issue).";
            actionText = zh
                ? L"按 R 或点击窗口立即重新加载  |  按 Esc 关闭"
                : L"Press R or click here to retry  |  Esc to close";
            break;
        }
        case WebViewStatus::Loading:
        default:
            titleText = zh ? L"正在启动 Tools3000 搜索界面..." : L"Starting Tools3000 Search...";
            descText = zh
                ? L"首次冷启动正在初始化 WebView2 视图，机械硬盘设备可能需要数秒，请稍候。"
                : L"Initializing WebView2 search view for the first time (may take a few seconds on HDD).";
            actionText = zh ? L"按 Esc 隐藏窗口" : L"Press Esc to hide";
            break;
    }

    SetBkMode(hdc, TRANSPARENT);
    const int titleFontPx = -tools3000::core::dpi::scaleMetric(18, scale);
    const int bodyFontPx = -tools3000::core::dpi::scaleMetric(13, scale);
    HFONT titleFont = CreateFontW(titleFontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    HFONT bodyFont = CreateFontW(bodyFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

    const int padX = tools3000::core::dpi::scaleMetric(40, scale);
    const int centerY = clientRect.top + height / 2;
    const int lineGap = tools3000::core::dpi::scaleMetric(12, scale);

    HGDIOBJ oldFont = SelectObject(hdc, titleFont ? titleFont : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(hdc, titleColor);
    RECT titleRc{clientRect.left + padX, centerY - tools3000::core::dpi::scaleMetric(54, scale),
                 clientRect.right - padX, centerY - tools3000::core::dpi::scaleMetric(14, scale)};
    DrawTextW(hdc, titleText.c_str(), -1, &titleRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, bodyFont ? bodyFont : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(hdc, subColor);
    RECT descRc{clientRect.left + padX, centerY - tools3000::core::dpi::scaleMetric(8, scale),
                clientRect.right - padX, centerY + tools3000::core::dpi::scaleMetric(42, scale)};
    DrawTextW(hdc, descText.c_str(), -1, &descRc, DT_CENTER | DT_WORDBREAK);

    SetTextColor(hdc, status == WebViewStatus::Loading ? subColor : accentColor);
    RECT actRc{clientRect.left + padX, descRc.bottom + lineGap,
               clientRect.right - padX, descRc.bottom + lineGap + tools3000::core::dpi::scaleMetric(28, scale)};
    DrawTextW(hdc, actionText.c_str(), -1, &actRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    if (titleFont) DeleteObject(titleFont);
    if (bodyFont) DeleteObject(bodyFont);
}

void SearchWindow::handleStatusSurfaceAction() {
    const auto status = static_cast<WebViewStatus>(m_webViewStatus.load(std::memory_order_acquire));
    if (status == WebViewStatus::MissingRuntime) {
        tools3000::core::WinUtils::openUrl(WebViewEnvironmentManager::evergreenDownloadUrl());
    } else if (status == WebViewStatus::InitFailed || status == WebViewStatus::RenderCrashed) {
        if (!m_webViewInitializing.load(std::memory_order_acquire)) {
            if (m_controller) {
                m_controller->Close();
                m_controller = nullptr;
            }
            m_webView = nullptr;
            m_webViewReady = false;
            m_uiFirstPaintReady = false;
            initializeWebView2();
        }
    }
}

void SearchWindow::initializeWebView2() {
    const uint64_t generation = ++m_generation;
    m_initStartTick = GetTickCount64();
    m_preloadWarmingFrame = false;
    m_webViewReady.store(false, std::memory_order_release);
    m_uiFirstPaintReady.store(false, std::memory_order_release);
    m_webViewInitializing.store(true, std::memory_order_release);
    if (!WebViewEnvironmentManager::isRuntimeInstalled()) {
        LOG_ERROR("SearchWindow: WebView2 Runtime is missing on this machine");
        m_webViewInitializing.store(false, std::memory_order_release);
        setWebViewStatus(WebViewStatus::MissingRuntime, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        return;
    }
    setWebViewStatus(WebViewStatus::Loading, S_OK);
    WebViewEnvironmentManager::instance().acquire(
            [this, generation](HRESULT result, ICoreWebView2Environment* env) {
                if (FAILED(result) || !env) {
                    LOG_ERROR("SearchWindow: shared environment unavailable, hr=0x{:08X}",
                              static_cast<unsigned>(result));
                    if (generation == m_generation.load()) {
                        m_webViewInitializing.store(false, std::memory_order_release);
                        setWebViewStatus(
                            WebViewEnvironmentManager::isRuntimeInstalled()
                                ? WebViewStatus::InitFailed
                                : WebViewStatus::MissingRuntime,
                            result);
                    }
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
                            if (FAILED(res) || !controller) {
                                LOG_ERROR("SearchWindow: CreateCoreWebView2Controller completed with error hr=0x{:08X}",
                                          static_cast<unsigned>(res));
                                if (generation == m_generation.load()) {
                                    m_webViewInitializing.store(false, std::memory_order_release);
                                    setWebViewStatus(WebViewStatus::InitFailed, res);
                                }
                                return E_FAIL;
                            }
                            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                                controller->Close();
                                return E_ABORT;
                            }
                            m_controller = controller;
                            const HRESULT hrGetWeb = m_controller->get_CoreWebView2(&m_webView);
                            if (FAILED(hrGetWeb) || !m_webView) {
                                LOG_WARN("SearchWindow: get_CoreWebView2 失败: HRESULT=0x{:08X}", static_cast<unsigned>(hrGetWeb));
                                m_webViewInitializing.store(false, std::memory_order_release);
                                setWebViewStatus(WebViewStatus::InitFailed, hrGetWeb);
                                return E_FAIL;
                            }
                            // 首帧未提交前保持 WebView2 控制器隐藏，持续展示宿主原生加载提示，
                            // 彻底杜绝机械盘冷启期间提前用纯 #0F172A 空画布遮挡产生“黑屏”观感。
                            m_controller->put_IsVisible(FALSE);

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

                            // 监听 WebView2 渲染器/GPU 进程崩溃：自动尝试一次自愈重载，杜绝永久黑屏死窗
                            m_webView->add_ProcessFailed(
                                Callback<ICoreWebView2ProcessFailedEventHandler>(
                                    [this, generation](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                        if (generation != m_generation.load()) return S_OK;
                                        COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                        if (args) args->get_ProcessFailedKind(&kind);
                                        LOG_ERROR("SearchWindow: WebView2 process failed, kind={}", static_cast<int>(kind));
                                        m_webViewReady.store(false, std::memory_order_release);
                                        m_uiFirstPaintReady.store(false, std::memory_order_release);
                                        m_webViewInitializing.store(false, std::memory_order_release);
                                        if (m_controller) m_controller->put_IsVisible(FALSE);
                                        if (!m_autoRecoverAttempted.exchange(true, std::memory_order_acq_rel) && m_webView) {
                                            setWebViewStatus(WebViewStatus::Loading, S_OK);
                                            m_webView->Reload();
                                        } else {
                                            if (m_controller) m_controller->put_IsVisible(FALSE);
                                            setWebViewStatus(WebViewStatus::RenderCrashed, E_FAIL);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

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
                                    [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* navArgs) -> HRESULT {
                                        if (generation != m_generation.load()) return S_OK;
                                        m_webViewInitializing.store(false, std::memory_order_release);
                                        BOOL isSuccess = TRUE;
                                        COREWEBVIEW2_WEB_ERROR_STATUS webErr = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                        if (navArgs) {
                                            navArgs->get_IsSuccess(&isSuccess);
                                            navArgs->get_WebErrorStatus(&webErr);
                                        }
                                        if (!isSuccess && webErr != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                                            LOG_ERROR("SearchWindow: WebView2 navigation failed, webErrorStatus={}",
                                                      static_cast<int>(webErr));
                                            if (m_controller) m_controller->put_IsVisible(FALSE);
                                            setWebViewStatus(WebViewStatus::InitFailed, E_FAIL);
                                            return S_OK;
                                        }
                                        m_webViewReady = true;
                                        hookWebViewChildWindows(m_hwnd);
                                        const uint64_t navElapsed =
                                            (m_initStartTick > 0) ? (GetTickCount64() - m_initStartTick) : 0ULL;
                                        LOG_INFO("SearchWindow: NavigationCompleted (navMs={}, visible={}, firstPaintReady={})",
                                                 navElapsed,
                                                 m_visible.load(std::memory_order_acquire),
                                                 m_uiFirstPaintReady.load(std::memory_order_acquire));
                                        if (m_pendingServiceWarmup.exchange(false, std::memory_order_acq_rel)) {
                                            if (m_hwnd && IsWindow(m_hwnd)) {
                                                KillTimer(m_hwnd, IDT_COLD_WARMUP_FALLBACK);
                                            }
                                            warmUpSearchService();
                                        }
                                        if (m_visible.load(std::memory_order_acquire)) {
                                            // 用户已在冷启中途打开窗口：无需砸毁新建 SwapChain，直接平滑呈现并等待首帧握手
                                            setWebViewStatus(WebViewStatus::Ready, S_OK);
                                            presentWebViewSurface(false);
                                            focusSearchIfVisible();
                                            if (!m_uiFirstPaintReady.load(std::memory_order_acquire) &&
                                                m_hwnd && IsWindow(m_hwnd)) {
                                                SetTimer(m_hwnd, IDT_FIRST_PAINT_FALLBACK, FIRST_PAINT_FALLBACK_DELAY_MS, nullptr);
                                            }
                                        } else {
                                            // 后台静默 preload 完成：触发无感 DWM Cloak 真实 GPU 首帧预热，
                                            // 让显存在用户首次 Alt+Space 之前就持有真实完整画面，根除 Win10 LTSC/机械盘首次黑屏。
                                            startBackgroundFirstPaintWarmup();
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
                    if (generation == m_generation.load()) {
                        m_webViewInitializing.store(false, std::memory_order_release);
                        setWebViewStatus(WebViewStatus::InitFailed, controllerResult);
                    }
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
            RECT clientRc{};
            GetClientRect(hwnd, &clientRc);
            inst.paintStatusBackdrop(hdc, clientRc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!inst.m_webViewReady.load(std::memory_order_acquire)) {
                inst.handleStatusSurfaceAction();
                return 0;
            }
            break;
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
                    inst.m_wasSuspended = true;
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
                if (IsWindowVisible(hwnd) && inst.m_webViewReady.load(std::memory_order_acquire)) {
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
            // 独立桌面窗口模式默认失焦保留在后台；当用户显式开启 /search/hideOnBlur 时，
            // 通过 60ms 延迟确认并严格避让置顶态、QuickLook 预览、右键/系统菜单与子对话框后再自动收起。
            const bool active = (LOWORD(wParam) != WA_INACTIVE);
            if (active) {
                KillTimer(hwnd, IDT_DEFERRED_BLUR_HIDE);
            }
            if (active && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                if (inst.m_webView) {
                    inst.m_suspendController.resume(inst.m_webView.Get(), "search");
                }
                if (inst.m_controller && inst.m_uiFirstPaintReady.load(std::memory_order_acquire)) {
                    inst.m_controller->put_IsVisible(TRUE);
                }
            } else if (!active && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                const bool hideOnBlur = tools3000::core::ConfigManager::instance().get<bool>(
                    "/search/hideOnBlur", false);
                if (hideOnBlur && !inst.m_isPinned.load() && !inst.isMenuActive()) {
                    SetTimer(hwnd, IDT_DEFERRED_BLUR_HIDE, DEFERRED_BLUR_HIDE_DELAY_MS, nullptr);
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
            if (wParam == IDT_DEFERRED_BLUR_HIDE) {
                KillTimer(hwnd, IDT_DEFERRED_BLUR_HIDE);
                const HWND fg = GetForegroundWindow();
                const HWND quickLookHwnd = QuickLookWindow::instance().getHwnd();
                bool fgOwnedBySearchOrMenu = false;
                if (fg) {
                    if (fg == hwnd || IsChild(hwnd, fg) || GetWindow(fg, GW_OWNER) == hwnd ||
                        (quickLookHwnd && (fg == quickLookHwnd || IsChild(quickLookHwnd, fg)))) {
                        fgOwnedBySearchOrMenu = true;
                    } else {
                        wchar_t clsName[64]{};
                        if (GetClassNameW(fg, clsName, 64) > 0 && wcscmp(clsName, L"#32768") == 0) {
                            fgOwnedBySearchOrMenu = true;
                        }
                    }
                }
                const uint64_t nowTick = GetTickCount64();
                BlurHideContext ctx{};
                ctx.hideOnBlurEnabled = tools3000::core::ConfigManager::instance().get<bool>(
                    "/search/hideOnBlur", false);
                ctx.visible = inst.isVisible();
                ctx.minimized = inst.isMinimized();
                ctx.foregroundActive = inst.isForegroundActive();
                ctx.pinned = inst.m_isPinned.load();
                ctx.menuOrDragActive = inst.isMenuActive();
                ctx.quickLookVisible = QuickLookWindow::instance().isVisible();
                ctx.foregroundOwnedBySearchOrMenu = fgOwnedBySearchOrMenu;
                ctx.elapsedSinceShowMs = (nowTick >= inst.m_showTimeTick)
                    ? (nowTick - inst.m_showTimeTick)
                    : 0;
                if (shouldAutoHideOnBlur(ctx)) {
                    inst.hide();
                }
                return 0;
            }
            if (wParam == IDT_COLD_WARMUP_FALLBACK) {
                KillTimer(hwnd, IDT_COLD_WARMUP_FALLBACK);
                if (inst.m_pendingServiceWarmup.exchange(false, std::memory_order_acq_rel)) {
                    warmUpSearchService();
                }
                return 0;
            }
            if (wParam == IDT_POST_SHOW_REPAINT_VERIFY) {
                KillTimer(hwnd, IDT_POST_SHOW_REPAINT_VERIFY);
                if (inst.isVisible() && inst.m_controller && inst.m_webViewReady.load(std::memory_order_acquire)) {
                    RECT verifyRc{};
                    if (GetClientRect(hwnd, &verifyRc) && verifyRc.right > verifyRc.left && verifyRc.bottom > verifyRc.top) {
                        inst.m_controller->put_Bounds(verifyRc);
                    }
                    inst.m_controller->put_IsVisible(TRUE);
                    inst.m_controller->NotifyParentWindowPositionChanged();
                    if (inst.m_webView) {
                        inst.m_webView->ExecuteScript(
                            L"requestAnimationFrame(() => { void document.documentElement.offsetHeight; });",
                            nullptr);
                    }
                }
                return 0;
            }
            if (wParam == IDT_FIRST_PAINT_UNCLOAK) {
                KillTimer(hwnd, IDT_FIRST_PAINT_UNCLOAK);
                LOG_WARN("SearchWindow: first-paint cloak timeout ({}ms) reached; forcing uncloak",
                         FIRST_PAINT_UNCLOAK_TIMEOUT_MS);
                inst.setWindowCloaked(false);
                if (inst.isVisible() && inst.m_controller) {
                    inst.m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
                return 0;
            }
            if (wParam == IDT_PRELOAD_WARMUP_TIMEOUT) {
                KillTimer(hwnd, IDT_PRELOAD_WARMUP_TIMEOUT);
                inst.finishBackgroundFirstPaintWarmup(false, "timeout");
                return 0;
            }
            if (wParam == IDT_FIRST_PAINT_FALLBACK) {
                KillTimer(hwnd, IDT_FIRST_PAINT_FALLBACK);
                inst.completeFirstPaintReady();
                return 0;
            }
            if (wParam == IDT_DEFERRED_SUSPEND && !inst.m_visible.load()) {
                KillTimer(hwnd, IDT_DEFERRED_SUSPEND);
                if (inst.m_controller) inst.m_controller->put_IsVisible(FALSE);
                if (inst.m_webView && inst.m_everPresentedVisible) {
                    inst.m_wasSuspended = true;
                    inst.m_suspendController.requestSuspend(inst.m_webView.Get(), "search");
                }
                tools3000::core::WinUtils::scheduleQuietIdleTrim(2500);
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
            if (!inst.m_webViewReady.load(std::memory_order_acquire) &&
                (wParam == VK_RETURN || wParam == 'R')) {
                inst.handleStatusSurfaceAction();
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
