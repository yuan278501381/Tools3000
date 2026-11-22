// ─────────────────────────────────────────────────────────────────────────────
// ClipboardWindow.cpp — 极速呼出剪贴板历史无边框悬浮面板实现
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/ClipboardWindow.h"
#include "clipboard/ClipboardManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "ui/KeyboardPipeline.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewSecurity.h"
#include "ui/WebViewWindowStyle.h"

#include "../resource.h"

#include <WebView2.h>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <wrl/event.h>

#include <algorithm>
#include <filesystem>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_TRANSIENTWINDOW 3
#endif

namespace tools3000::ui {

namespace {

constexpr const wchar_t* kClipboardWindowClass = L"Tools3000_ClipboardWindow";
constexpr const wchar_t* kClipboardBlobsHost = L"clipboard-blobs.tools3000.local";

void syncClipboardBackdropColor(ICoreWebView2Controller* controller) {
    if (!controller) return;
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
        const auto bg = tools3000::core::NativeThemePalette::isEffectiveDarkMode()
            ? COREWEBVIEW2_COLOR{255, 22, 24, 30}
            : COREWEBVIEW2_COLOR{255, 248, 250, 252};
        c2->put_DefaultBackgroundColor(bg);
    }
}

}  // namespace

ClipboardWindow& ClipboardWindow::instance() {
    static ClipboardWindow inst;
    return inst;
}

ClipboardWindow::~ClipboardWindow() {
    destroy();
}

bool ClipboardWindow::isVisible() const noexcept {
    return m_visible.load(std::memory_order_acquire) && m_hwnd && IsWindowVisible(m_hwnd);
}

void ClipboardWindow::toggle(HINSTANCE hInstance) {
    if (isVisible()) {
        hide();
    } else {
        show(hInstance);
    }
}

void ClipboardWindow::show(HINSTANCE hInstance) {
    if (!hInstance) {
        hInstance = GetModuleHandleW(nullptr);
    }

    const HWND fg = GetForegroundWindow();
    if (fg && fg != m_hwnd && IsWindow(fg)) {
        m_previousForegroundHwnd = fg;
    }

    if (!m_hwnd || !IsWindow(m_hwnd)) {
        if (!createWindow(hInstance)) return;
        initializeWebView2();
    } else {
        KillTimer(m_hwnd, IDT_IDLE_DESTROY);
    }

    applyThemeChrome();
    updatePlacement();
    m_showTimeTick = GetTickCount64();
    m_visible.store(true, std::memory_order_release);

    if (m_controller) {
        m_suspendController.resume(m_webView.Get(), "clipboard");
        m_controller->put_IsVisible(TRUE);
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        m_controller->put_Bounds(rc);
        syncWebViewDpi(m_controller.Get(), m_hwnd);
        syncClipboardBackdropColor(m_controller.Get());
    }

    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    if (m_controller) {
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    notifyShownToWeb();
}

void ClipboardWindow::hide() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    ShowWindow(m_hwnd, SW_HIDE);
    m_visible.store(false, std::memory_order_release);
    if (m_controller) {
        m_controller->put_IsVisible(FALSE);
    }
    if (m_webView) {
        m_suspendController.requestSuspend(m_webView.Get(), "clipboard");
    }
    SetTimer(m_hwnd, IDT_IDLE_DESTROY, IDLE_DESTROY_TIMEOUT_MS, nullptr);
}

void ClipboardWindow::destroy() {
    tools3000::core::MessageBridge::instance().unregisterEventPusher("clipboard_window");
    m_suspendController.abandon();
    ++m_generation;
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, IDT_IDLE_DESTROY);
    }
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
    m_visible.store(false, std::memory_order_release);
    m_webViewReady = false;
    tools3000::core::WinUtils::trimWorkingSet();
}

void ClipboardWindow::pushEventJson(const std::string& envelopeJson) {
    if (envelopeJson.empty()) return;
    const uint64_t generation = m_generation.load();
    tools3000::core::MainThreadDispatcher::instance().post([this, generation, envelopeJson]() {
        if (generation != m_generation.load() || !m_webView || !m_webViewReady) return;
        const std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(envelopeJson);
        m_webView->PostWebMessageAsString(wMsg.c_str());
    });
}

void ClipboardWindow::notifyShownToWeb() {
    if (!m_webView || !m_webViewReady) return;
    m_webView->PostWebMessageAsString(LR"({"type":"event","event":"clipboard:shown","data":{}})");
    notifyWindowStateToWeb();
}

void ClipboardWindow::notifyWindowStateToWeb() {
    if (!m_webView || !m_webViewReady) return;
    json payload = {
        {"type", "event"},
        {"event", "clipboard:windowState"},
        {"data", {{"pinned", m_pinned}, {"maximized", m_maximized}}}
    };
    const std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(payload.dump());
    m_webView->PostWebMessageAsString(wMsg.c_str());
}

void ClipboardWindow::setPinned(bool pinned) {
    m_pinned = pinned;
    if (m_hwnd && IsWindow(m_hwnd)) {
        SetWindowPos(
            m_hwnd,
            m_pinned ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    notifyWindowStateToWeb();
}

void ClipboardWindow::toggleMaximize() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (IsZoomed(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else {
        // 最大化前先记住普通态位置尺寸，供还原与下次呼出复用
        RECT rc{};
        if (GetWindowRect(m_hwnd, &rc) && (rc.right - rc.left) > 200 && (rc.bottom - rc.top) > 200) {
            m_customBounds = rc;
            m_hasCustomBounds = true;
        }
        ShowWindow(m_hwnd, SW_MAXIMIZE);
    }
}

void ClipboardWindow::applyThemeChrome() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const BOOL dark = tools3000::core::NativeThemePalette::isEffectiveDarkMode() ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    syncClipboardBackdropColor(m_controller.Get());
}

void ClipboardWindow::updatePlacement() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const HWND zInsertAfter = m_pinned ? HWND_TOPMOST : HWND_TOP;
    if (IsZoomed(m_hwnd)) {
        SetWindowPos(m_hwnd, zInsertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (m_hasCustomBounds) {
        const POINT center{
            (m_customBounds.left + m_customBounds.right) / 2,
            (m_customBounds.top + m_customBounds.bottom) / 2
        };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        if (!mon) mon = tools3000::core::dpi::activeMonitor();
        const RECT work = tools3000::core::dpi::workArea(mon);
        const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(mon);
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        const int minW = tools3000::core::dpi::scaleMetric(560, scale);
        const int minH = tools3000::core::dpi::scaleMetric(420, scale);
        const int workW = (std::max)(minW, static_cast<int>(work.right - work.left));
        const int workH = (std::max)(minH, static_cast<int>(work.bottom - work.top));

        const int targetW = std::clamp(static_cast<int>(m_customBounds.right - m_customBounds.left), minW, workW);
        const int targetH = std::clamp(static_cast<int>(m_customBounds.bottom - m_customBounds.top), minH, workH);
        const int x = std::clamp(static_cast<int>(m_customBounds.left), static_cast<int>(work.left), static_cast<int>(work.right - targetW));
        const int y = std::clamp(static_cast<int>(m_customBounds.top), static_cast<int>(work.top), static_cast<int>(work.bottom - targetH));

        SetWindowPos(m_hwnd, zInsertAfter, x, y, targetW, targetH, SWP_NOACTIVATE);
        const int radius = tools3000::core::dpi::scaleMetric(12, scale);
        tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetW, targetH, radius);
        return;
    }

    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    int targetW = tools3000::core::dpi::scaleMetric(760, scale);
    int targetH = tools3000::core::dpi::scaleMetric(560, scale);
    const int maxW = static_cast<int>(work.right - work.left) - 32;
    const int maxH = static_cast<int>(work.bottom - work.top) - 32;
    targetW = (std::min)(targetW, maxW);
    targetH = (std::min)(targetH, maxH);

    // 居中略偏上黄金视区，兼顾多屏与混合 DPI 工作区边界
    const int x = work.left + (work.right - work.left - targetW) / 2;
    const int y = work.top + static_cast<int>((work.bottom - work.top - targetH) * 0.38);

    SetWindowPos(m_hwnd, zInsertAfter, x, y, targetW, targetH, SWP_NOACTIVATE);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetW, targetH, radius);
}

bool ClipboardWindow::createWindow(HINSTANCE hInstance) {
    m_suspendController.reset();
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &ClipboardWindow::windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClipboardWindowClass;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
        LR_DEFAULTCOLOR | LR_SHARED));
    RegisterClassExW(&wc);

    const int targetW = tools3000::core::dpi::scaleMetric(760, scale);
    const int targetH = tools3000::core::dpi::scaleMetric(560, scale);
    const int x = work.left + (work.right - work.left - targetW) / 2;
    const int y = work.top + static_cast<int>((work.bottom - work.top - targetH) * 0.38);

    // WS_POPUP + WS_THICKFRAME + WS_MAXIMIZEBOX（无 WS_CAPTION）：完整支持原生 8 向缩放与最大化贴边，
    // 同时从内核层面彻底杜绝 Win11 DWM 旧式 Win7/Win98 标题栏降级闪现。
    const DWORD exStyle = WS_EX_TOOLWINDOW | (m_pinned ? WS_EX_TOPMOST : 0u);
    m_hwnd = CreateWindowExW(
        exStyle,
        kClipboardWindowClass,
        L"Tools3000 Clipboard",
        WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, targetW, targetH,
        nullptr, nullptr, hInstance, this);

    if (!m_hwnd) {
        LOG_ERROR("ClipboardWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    applyThemeChrome();
    DWORD backdrop = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetW, targetH, radius);
    return true;
}

void ClipboardWindow::initializeWebView2() {
    const uint64_t generation = ++m_generation;
    WebViewEnvironmentManager::instance().acquire(
        [this, generation](HRESULT hr, ICoreWebView2Environment* env) {
            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) return;
            if (FAILED(hr) || !env) {
                LOG_ERROR("ClipboardWindow: acquire WebView2 environment failed");
                return;
            }
            m_environment = env;
            env->CreateCoreWebView2Controller(
                m_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, generation](HRESULT readyHr, ICoreWebView2Controller* controller) -> HRESULT {
                        if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                            if (controller) controller->Close();
                            return S_OK;
                        }
                        if (FAILED(readyHr) || !controller) {
                            LOG_ERROR("ClipboardWindow: CreateCoreWebView2Controller failed");
                            return readyHr;
                        }

                        m_controller = controller;
                        controller->get_CoreWebView2(&m_webView);
                        m_controller->put_IsVisible(m_visible.load() ? TRUE : FALSE);

                        RECT bounds{};
                        GetClientRect(m_hwnd, &bounds);
                        m_controller->put_Bounds(bounds);
                        syncWebViewDpi(m_controller.Get(), m_hwnd);
                        syncClipboardBackdropColor(m_controller.Get());

                        ComPtr<ICoreWebView2Settings> settings;
                        m_webView->get_Settings(&settings);
                        if (settings) {
                            settings->put_IsScriptEnabled(TRUE);
                            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(FALSE);
                            ComPtr<ICoreWebView2Settings3> s3;
                            if (SUCCEEDED(settings.As(&s3)) && s3) {
                                s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                            }
                        }

                        const std::wstring uiFolder =
                            (tools3000::core::WinUtils::getExeDirectory() / L"ui").wstring();
                        const std::wstring blobsFolder =
                            tools3000::clipboard::ClipboardManager::instance().getBlobsDir().wstring();

                        ComPtr<ICoreWebView2_3> webView3;
                        if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
                            webView3->SetVirtualHostNameToFolderMapping(
                                L"tools3000.local", uiFolder.c_str(),
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            if (!blobsFolder.empty()) {
                                webView3->SetVirtualHostNameToFolderMapping(
                                    kClipboardBlobsHost, blobsFolder.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }
                        }

                        web_security::applyNavigationPolicy(m_webView.Get());
                        KeyboardPipeline::applyWebKeyboardPolicy(m_controller.Get());

                        m_webView->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [this, generation](ICoreWebView2* sender,
                                                   ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    if (!web_security::isTrustedMessageSource(args)) return S_OK;
                                    try {
                                        LPWSTR raw = nullptr;
                                        args->TryGetWebMessageAsString(&raw);
                                        if (!raw) return S_OK;
                                        const std::string msg = tools3000::core::WinUtils::wstringToUtf8(raw);
                                        CoTaskMemFree(raw);
                                        if (!web_security::isBridgeMethodAllowed(msg, web_security::Surface::Clipboard)) {
                                            return S_OK;
                                        }
                                        auto parsed = json::parse(msg, nullptr, false);
                                        if (parsed.is_object()) {
                                            const std::string method = parsed.value("method", "");
                                            const auto sendReply = [&](const json& resultObj) {
                                                json reply = {
                                                    {"id", parsed.contains("id") ? parsed["id"] : json(nullptr)},
                                                    {"result", resultObj}
                                                };
                                                const std::wstring wReply = tools3000::core::WinUtils::utf8ToWstring(reply.dump());
                                                sender->PostWebMessageAsString(wReply.c_str());
                                            };
                                            if (method == "clipboard.startDrag") {
                                                if (generation == m_generation.load() && m_hwnd && IsWindow(m_hwnd)) {
                                                    ReleaseCapture();
                                                    SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                                }
                                                sendReply({{"success", true}});
                                                return S_OK;
                                            }
                                            if (method == "clipboard.startResize") {
                                                if (generation == m_generation.load() && m_hwnd && IsWindow(m_hwnd) && !IsZoomed(m_hwnd)) {
                                                    const auto params = parsed.value("params", json::object());
                                                    const std::string edge = params.value("edge", "");
                                                    WPARAM ht = HTNOWHERE;
                                                    if (edge == "n") ht = HTTOP;
                                                    else if (edge == "s") ht = HTBOTTOM;
                                                    else if (edge == "e") ht = HTRIGHT;
                                                    else if (edge == "w") ht = HTLEFT;
                                                    else if (edge == "ne") ht = HTTOPRIGHT;
                                                    else if (edge == "nw") ht = HTTOPLEFT;
                                                    else if (edge == "se") ht = HTBOTTOMRIGHT;
                                                    else if (edge == "sw") ht = HTBOTTOMLEFT;
                                                    if (ht != HTNOWHERE) {
                                                        ReleaseCapture();
                                                        SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, ht, 0);
                                                    }
                                                }
                                                sendReply({{"success", true}});
                                                return S_OK;
                                            }
                                            if (method == "clipboard.setWindowPinned") {
                                                if (generation == m_generation.load()) {
                                                    const auto params = parsed.value("params", json::object());
                                                    const bool nextPin = params.contains("pinned") && params["pinned"].is_boolean()
                                                        ? params["pinned"].get<bool>()
                                                        : !m_pinned;
                                                    setPinned(nextPin);
                                                }
                                                sendReply({{"success", true}, {"pinned", m_pinned}, {"maximized", m_maximized}});
                                                return S_OK;
                                            }
                                            if (method == "clipboard.toggleMaximize") {
                                                if (generation == m_generation.load()) {
                                                    toggleMaximize();
                                                }
                                                sendReply({{"success", true}, {"pinned", m_pinned}, {"maximized", m_maximized}});
                                                return S_OK;
                                            }
                                            if (method == "clipboard.getWindowState") {
                                                sendReply({{"success", true}, {"pinned", m_pinned}, {"maximized", m_maximized}});
                                                return S_OK;
                                            }
                                        }
                                        const std::string resp = tools3000::core::MessageBridge::instance().handleMessage(msg);
                                        const std::wstring wResp = tools3000::core::WinUtils::utf8ToWstring(resp);
                                        sender->PostWebMessageAsString(wResp.c_str());
                                    } catch (const std::exception& e) {
                                        LOG_WARN("ClipboardWindow: WebMessage error: {}", e.what());
                                    } catch (...) {
                                    }
                                    return S_OK;
                                }).Get(),
                            nullptr);

                        m_webView->add_NavigationCompleted(
                            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                    if (generation != m_generation.load()) return S_OK;
                                    BOOL ok = FALSE;
                                    if (args) args->get_IsSuccess(&ok);
                                    if (ok) {
                                        m_webViewReady = true;
                                        if (m_visible.load()) {
                                            notifyShownToWeb();
                                            if (m_controller) {
                                                m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                                            }
                                        }
                                    }
                                    return S_OK;
                                }).Get(),
                            nullptr);

                        m_webView->Navigate(L"https://tools3000.local/index.html?clipboard=1");
                        tools3000::core::MessageBridge::instance().registerEventPusher(
                            "clipboard_window",
                            [this, generation](const std::string& eventName, const json& data) {
                                if (eventName != "appearance:changed" &&
                                    eventName != "clipboard:stateChanged" &&
                                    eventName != "clipboard:updated") {
                                    return;
                                }
                                json envelope = {
                                    {"type", "event"},
                                    {"event", eventName},
                                    {"data", data}
                                };
                                const std::string payload = envelope.dump();
                                const bool isAppearance = (eventName == "appearance:changed");
                                tools3000::core::MainThreadDispatcher::instance().post(
                                    [this, generation, payload, isAppearance]() {
                                        if (generation != m_generation.load()) return;
                                        if (isAppearance) {
                                            applyThemeChrome();
                                        }
                                        if (m_webView && m_webViewReady) {
                                            const std::wstring wMsg =
                                                tools3000::core::WinUtils::utf8ToWstring(payload);
                                            m_webView->PostWebMessageAsString(wMsg.c_str());
                                        }
                                    });
                            });
                        return S_OK;
                    }).Get());
        });
}

LRESULT CALLBACK ClipboardWindow::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    ClipboardWindow* self = nullptr;
    if (uMsg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ClipboardWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ClipboardWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (KeyboardPipeline::filterWindowMessage(hwnd, uMsg, wParam, lParam)) {
        return 0;
    }

    LRESULT ncRes = 0;
    if (tools3000::core::WinUtils::handleFramelessNonClientMessage(hwnd, uMsg, wParam, lParam, ncRes)) {
        return ncRes;
    }

    if (self) {
        switch (uMsg) {
            case WM_NCCALCSIZE: {
                if (wParam == TRUE && IsZoomed(hwnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    if (params) {
                        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi{sizeof(mi)};
                        if (mon && GetMonitorInfoW(mon, &mi)) {
                            params->rgrc[0] = mi.rcWork;
                        }
                    }
                }
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
                if (mmi) {
                    const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                    mmi->ptMinTrackSize.x = tools3000::core::dpi::scaleMetric(560, scale);
                    mmi->ptMinTrackSize.y = tools3000::core::dpi::scaleMetric(420, scale);
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{sizeof(mi)};
                    if (mon && GetMonitorInfoW(mon, &mi)) {
                        mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                        mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                        mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                        mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                    }
                }
                return 0;
            }
            case WM_ENTERSIZEMOVE:
                self->m_inSizeMove = true;
                return 0;
            case WM_EXITSIZEMOVE: {
                self->m_inSizeMove = false;
                if (!IsZoomed(hwnd)) {
                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc) && (rc.right - rc.left) > 200 && (rc.bottom - rc.top) > 200) {
                        self->m_customBounds = rc;
                        self->m_hasCustomBounds = true;
                    }
                }
                return 0;
            }
            case WM_SIZE: {
                const int w = LOWORD(lParam);
                const int h = HIWORD(lParam);
                const bool nowMaximized = (wParam == SIZE_MAXIMIZED);
                const bool maxChanged = (nowMaximized != self->m_maximized);
                self->m_maximized = nowMaximized;
                if (w > 0 && h > 0) {
                    if (nowMaximized) {
                        tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, w, h, 0);
                    } else {
                        const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                        tools3000::core::WinUtils::applyUniversalRoundedCorners(
                            hwnd, w, h, tools3000::core::dpi::scaleMetric(12, scale));
                    }
                    if (self->m_controller) {
                        RECT rc{0, 0, w, h};
                        self->m_controller->put_Bounds(rc);
                    }
                }
                if (maxChanged) {
                    self->notifyWindowStateToWeb();
                }
                return 0;
            }
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                if (suggested) {
                    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                                 suggested->right - suggested->left, suggested->bottom - suggested->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                if (self->m_controller) {
                    syncWebViewDpi(self->m_controller.Get(), hwnd);
                }
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                const COLORREF bg = tools3000::core::NativeThemePalette::isEffectiveDarkMode()
                    ? RGB(22, 24, 30) : RGB(248, 250, 252);
                HBRUSH brush = CreateSolidBrush(bg);
                FillRect(hdc, &ps.rcPaint, brush);
                DeleteObject(brush);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_ACTIVATE: {
                if (LOWORD(wParam) == WA_INACTIVE && self->m_visible.load() &&
                    !self->m_pinned && !self->m_inSizeMove) {
                    if (GetTickCount64() - self->m_showTimeTick > 200) {
                        PostMessageW(hwnd, WM_CLIPBOARD_VERIFY_DEACTIVATED, 0, 0);
                    }
                }
                return 0;
            }
            case WM_CLIPBOARD_VERIFY_DEACTIVATED: {
                if (self->m_visible.load() && !self->m_pinned && !self->m_inSizeMove) {
                    const HWND fg = GetForegroundWindow();
                    if (fg != hwnd && (!fg || !IsChild(hwnd, fg))) {
                        self->hide();
                    }
                }
                return 0;
            }
            case WM_TIMER: {
                if (wParam == IDT_IDLE_DESTROY && !self->m_visible.load()) {
                    KillTimer(hwnd, IDT_IDLE_DESTROY);
                    self->destroy();
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
                self->hide();
                return 0;
            default:
                break;
        }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

}  // namespace tools3000::ui
