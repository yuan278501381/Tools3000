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
#include "../resource.h"

#pragma comment(lib, "comctl32.lib")

using namespace Microsoft::WRL;

namespace tools3000::ui {

static constexpr const wchar_t* SEARCH_WINDOW_CLASS = L"Tools3000_SearchWindow";
static constexpr UINT WM_SEARCH_VERIFY_DEACTIVATED = WM_APP + 42;

namespace {

LRESULT CALLBACK WebViewResizeSubclassProc(
    HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass;
    if (uMsg == WM_NCHITTEST) {
        HWND parentHwnd = reinterpret_cast<HWND>(dwRefData);
        if (parentHwnd && IsWindow(parentHwnd) && !IsZoomed(parentHwnd)) {
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

    if (!forceCenter) {
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

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, scaledWidth, scaledHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (m_controller) {
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }
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
    SetWindowPos(m_hwnd, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

std::pair<int, int> SearchWindow::getWindowSize() const {
    const int w = tools3000::core::ConfigManager::instance().get<int>("/search/windowWidth", SearchWindowStyle::BaseWidth);
    const int h = tools3000::core::ConfigManager::instance().get<int>("/search/windowHeight", SearchWindowStyle::BaseHeight);
    return {w, h};
}

void SearchWindow::focusSearchIfVisible() {
    if (!m_visible.load()) return;
    if (m_controller) {
        m_controller->put_IsVisible(TRUE);
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:focusSearch'));", nullptr);
    }
}

void SearchWindow::triggerQuickLookForSelection() {
    if (!m_visible.load()) return;
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:triggerQuickLook'));", nullptr);
    }
}

void SearchWindow::syncQuickLookForSelection() {
    if (!m_visible.load()) return;
    if (m_webView) {
        m_webView->ExecuteScript(
            L"window.dispatchEvent(new CustomEvent('tools3000:syncQuickLook'));", nullptr);
    }
}

void SearchWindow::stepQuickLookSelection(int delta) {
    if (!m_visible.load() || !m_webView) return;
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
        if (m_visible) {
            updatePlacement();
            SetForegroundWindow(m_hwnd);
            SetFocus(m_hwnd);
            focusSearchIfVisible();
            recordShown();
            return;
        }
        updatePlacement();
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        SetFocus(m_hwnd);
        m_visible = true;
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
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    m_visible = true;
    recordShown();
}

void SearchWindow::hide() {
    if (!m_hwnd || !m_visible) return;
    ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
    if (m_controller) m_controller->put_IsVisible(FALSE);
    if (m_webView) m_suspendController.requestSuspend(m_webView.Get(), "search");
    tools3000::core::MessageBridge::instance().handleMessageAsync(
        R"({"id":0,"method":"search.windowHidden","params":{}})", [](std::string) {});
}

bool SearchWindow::isVisible() const {
    return m_visible;
}

void SearchWindow::destroy() {
    m_suspendController.abandon();
    m_webView = nullptr;
    m_controller = nullptr;
    m_environment = nullptr;

    const HWND hwnd = std::exchange(m_hwnd, nullptr);
    if (hwnd && IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    m_visible = false;
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

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        SEARCH_WINDOW_CLASS,
        L"Tools3000 Search",
        WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
        x, y, size.cx, size.cy,
        nullptr, nullptr, hInstance, nullptr
    );
    if (!m_hwnd) {
        LOG_ERROR("SearchWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    if (wc.hIcon) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    }
    if (wc.hIconSm) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    }

    // 启用 DWM 全客户区扩展与跨平台通用圆角裁剪，全兼容 Win11、Win10、Server 2022/2025
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    const int radius = static_cast<int>(12 * scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, size.cx, size.cy, radius);
    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    return true;
}

void SearchWindow::updatePlacement() {
    if (!m_hwnd || m_updatingPlacement) return;
    m_updatingPlacement = true;
    const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
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

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const int radius = static_cast<int>(12 * scale);
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
                            
                            // Enable true alpha transparency
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(m_controller.As(&controller2))) {
                                COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};
                                controller2->put_DefaultBackgroundColor(transparent);
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

                            // 导航完成后标记就绪。后台 preload 不得发送聚焦事件；
                            // 只有可见窗口才应同步查询状态并聚焦输入框。
                            m_webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        if (generation != m_generation.load()) return S_OK;
                                        m_webViewReady = true;
                                        hookWebViewChildWindows(m_hwnd);
                                        focusSearchIfVisible();
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
    switch (uMsg) {
        case WM_ERASEBKGND:
            // 拦截背景擦除，WebView2 DirectComposition 完全接管绘制，消除偶发底衬微闪
            return 1;
        case WM_NCCALCSIZE: {
            if (wParam) {
                return 0; // 消除系统默认边框占用，使 WebView2 客户区占满整个圆角窗口
            }
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
        case WM_NCHITTEST: {
            if (!IsZoomed(hwnd)) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT rc;
                GetWindowRect(hwnd, &rc);
                const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                return calculateHitTest(rc, pt, scale);
            }
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
        }
        case WM_SIZE: {
            const int newW = LOWORD(lParam);
            const int newH = HIWORD(lParam);
            if (newW > 0 && newH > 0) {
                if (IsZoomed(hwnd)) {
                    SetWindowRgn(hwnd, nullptr, TRUE);
                } else {
                    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                    const int radius = static_cast<int>(12 * scale);
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
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                const int w = rc.right - rc.left;
                const int h = rc.bottom - rc.top;
                const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
                const float scale = tools3000::core::dpi::scaleForDpi(dpi);
                const int radius = static_cast<int>(12 * scale);
                tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, w, h, radius);

                int baseW = static_cast<int>(w / scale);
                int baseH = static_cast<int>(h / scale);
                if (baseW >= 400 && baseH >= 250) {
                    tools3000::core::ConfigManager::instance().set<int>("/search/windowWidth", baseW);
                    tools3000::core::ConfigManager::instance().set<int>("/search/windowHeight", baseH);
                    tools3000::core::ConfigManager::instance().set<int>("/search/windowX", rc.left);
                    tools3000::core::ConfigManager::instance().set<int>("/search/windowY", rc.top);
                }
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
            }
            break;
        }
        case WM_DPICHANGED: {
            if (!inst.m_updatingPlacement && lParam) {
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (IsWindowVisible(hwnd)) inst.updatePlacement();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                PostMessageW(hwnd, WM_SEARCH_VERIFY_DEACTIVATED, 0, 0);
            } else {
                inst.focusSearchIfVisible();
            }
            break;
        case WM_SETFOCUS:
            inst.focusSearchIfVisible();
            return 0;
        case WM_CLOSE:
            inst.hide();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                inst.hide();
                return 0;
            }
            break;
        case WM_SEARCH_VERIFY_DEACTIVATED: {
            const bool isPinned = inst.m_isPinned.load() || (GetPropW(hwnd, L"Tools3000_SearchPinned") != nullptr);
            if (isPinned) break; // 启用图钉时，窗口永远显示，绝对不失焦隐藏
            if (!inst.m_visible.load()) break;
            if (inst.isMenuActive()) break;
            if (!inst.m_webViewReady.load()) break; // 渲染就绪前严禁失焦误杀
            const uint64_t elapsed = GetTickCount64() - inst.m_showTimeTick;
            if (elapsed < 350) {
                // 窗口刚打开 350ms 内不因初始焦点抖动或创建子窗口而意外关闭
                break;
            }
            const HWND foreground = GetForegroundWindow();
            const HWND qlHwnd = QuickLookWindow::instance().getHwnd();
            if (qlHwnd && IsWindowVisible(qlHwnd) &&
                (foreground == qlHwnd || (foreground && IsChild(qlHwnd, foreground)))) {
                break;
            }
            if (foreground != hwnd && (!foreground || !IsChild(hwnd, foreground))) {
                inst.hide();
            }
            break;
        }
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
