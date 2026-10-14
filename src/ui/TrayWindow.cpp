#include "ui/TrayWindow.h"
#include "tray/TrayIcon.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/DpiUtils.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewWindowStyle.h"
#include "ui/WebViewSecurity.h"
#include "ui/KeyboardPipeline.h"
#include "core/events/MainThreadDispatcher.h"
#include <wrl/event.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <utility>
#include "core/utils/WinUtils.h"

using namespace Microsoft::WRL;

namespace tools3000::ui {

static constexpr const wchar_t* TRAY_WINDOW_CLASS = L"Tools3000_TrayWindow";
static constexpr UINT WM_TRAY_VERIFY_DEACTIVATED = WM_APP + 41;
static constexpr UINT_PTR IDT_TRAY_AUTOHIDE = 9001;

namespace {

SIZE getTrayWindowSize(POINT anchor, int customW = 0, int customH = 0) {
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const LONG margin = tools3000::core::dpi::scaleMetric(TrayWindowStyle::BaseScreenMargin, scale);
    const int maxPhysicalH = std::max<int>(200, static_cast<int>(workArea.bottom - workArea.top - 2 * margin));
    const int effectiveW = customW > 0 ? std::clamp(customW, 200, 340) : TrayWindowStyle::BaseWidth;
    const int effectiveH = customH > 0 ? std::clamp(customH, 100, 760) : TrayWindowStyle::BaseHeight;
    const int physicalW = static_cast<int>(effectiveW * scale);
    const int physicalH = std::min<int>(static_cast<int>(effectiveH * scale), maxPhysicalH);
    return {
        static_cast<LONG>(physicalW),
        static_cast<LONG>(physicalH)
    };
}

POINT trayWindowOrigin(int x, int y, int width, int height) {
    const POINT anchor{x, y};
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const LONG margin = tools3000::core::dpi::scaleMetric(
        TrayWindowStyle::BaseScreenMargin, scale);
    const LONG maxX = std::max(
        workArea.left + margin, workArea.right - static_cast<LONG>(width) - margin);
    const LONG maxY = std::max(
        workArea.top + margin, workArea.bottom - static_cast<LONG>(height) - margin);
    return {
        std::clamp<LONG>(static_cast<LONG>(x - width / 2), workArea.left + margin, maxX),
        std::clamp<LONG>(static_cast<LONG>(y - height - margin), workArea.top + margin, maxY)
    };
}

}  // namespace

TrayWindow& TrayWindow::instance() {
    static TrayWindow inst;
    return inst;
}

void TrayWindow::preload(HINSTANCE hInstance) {
    if (m_hwnd && IsWindow(m_hwnd)) return;
    POINT defaultAnchor{};
    GetCursorPos(&defaultAnchor);
    const RECT work = tools3000::core::dpi::workArea(
        MonitorFromPoint(defaultAnchor, MONITOR_DEFAULTTONEAREST));
    if (defaultAnchor.x < work.left || defaultAnchor.x > work.right ||
        defaultAnchor.y < work.top || defaultAnchor.y > work.bottom) {
        defaultAnchor.x = work.right - 100;
        defaultAnchor.y = work.bottom - 100;
    }
    m_anchor = defaultAnchor;
    if (!createWindow(hInstance, defaultAnchor.x, defaultAnchor.y)) {
        LOG_ERROR("TrayWindow: preload createWindow failed");
        return;
    }
    initializeWebView2();
    ShowWindow(m_hwnd, SW_HIDE);
}

void TrayWindow::show(HINSTANCE hInstance, int x, int y) {
    m_anchor = {x, y};
    m_showTimeTick = GetTickCount64();
    if (m_hwnd && IsWindow(m_hwnd)) {
        updatePlacement();
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        SetActiveWindow(m_hwnd);
        SetFocus(m_hwnd);
        SetTimer(m_hwnd, IDT_TRAY_AUTOHIDE, 80, nullptr);
        m_visible = true;

        if (m_controller) {
            m_controller->put_IsVisible(TRUE);
            m_suspendController.resume(m_controller.Get());
            m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            syncWebViewDpi(m_controller.Get(), m_hwnd);
            if (m_webView) {
                m_webView->ExecuteScript(L"window.dispatchEvent(new CustomEvent('tray:show'));", nullptr);
            }
        }
        return;
    }

    if (!createWindow(hInstance, x, y)) {
        LOG_ERROR("TrayWindow: createWindow failed");
        return;
    }

    initializeWebView2();
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    SetTimer(m_hwnd, IDT_TRAY_AUTOHIDE, 80, nullptr);
    UpdateWindow(m_hwnd);
    m_visible = true;
}

void TrayWindow::hide() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, IDT_TRAY_AUTOHIDE);
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        m_lastHideTimeTick.store(GetTickCount64());
        if (m_controller) {
            m_controller->put_IsVisible(FALSE);
        }
        m_suspendController.requestSuspend(m_controller.Get(), m_webView.Get());
    }
}

bool TrayWindow::isVisible() const {
    return m_visible.load() && m_hwnd && IsWindowVisible(m_hwnd);
}

void TrayWindow::destroy() {
    m_suspendController.abandon();
    tools3000::core::MessageBridge::instance().unregisterEventPusher("tray");
    ++m_generation;
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
    m_webViewReady = false;
}

bool TrayWindow::createWindow(HINSTANCE hInstance, int x, int y) {
    m_suspendController.reset();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = TRAY_WINDOW_CLASS;
    RegisterClassExW(&wc);

    const SIZE sz = getTrayWindowSize({x, y}, m_contentWidth, m_contentHeight);
    const POINT origin = trayWindowOrigin(x, y, sz.cx, sz.cy);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        TRAY_WINDOW_CLASS,
        L"Tools3000 TrayMenu",
        WS_POPUP | WS_CLIPCHILDREN,
        origin.x, origin.y, sz.cx, sz.cy,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!m_hwnd) return false;

    // 启用 DWM 全客户区扩展 (Sheet of Glass) 与跨平台通用圆角裁剪，彻底消除透明区域黑底
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    const HMONITOR monitor = MonitorFromPoint(m_anchor, MONITOR_DEFAULTTONEAREST);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, sz.cx, sz.cy, radius);

    return true;
}

void TrayWindow::updatePlacement() {
    if (!m_hwnd || m_updatingPlacement) return;
    m_updatingPlacement = true;
    const SIZE size = getTrayWindowSize(m_anchor, m_contentWidth, m_contentHeight);
    const POINT origin = trayWindowOrigin(
        m_anchor.x, m_anchor.y, size.cx, size.cy);
    SetWindowPos(m_hwnd, HWND_TOPMOST, origin.x, origin.y, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const HMONITOR monitor = MonitorFromPoint(m_anchor, MONITOR_DEFAULTTONEAREST);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, size.cx, size.cy, radius);
    if (m_controller) {
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }
    m_updatingPlacement = false;
}

void TrayWindow::setContentSize(int width, int height) {
    if (!m_hwnd || !IsWindow(m_hwnd) || width <= 0 || height <= 0) return;
    const int clampedW = std::clamp(width, 200, 340);
    const int clampedH = std::clamp(height, 100, 760);
    if (m_contentWidth == clampedW && m_contentHeight == clampedH) return;
    m_contentWidth = clampedW;
    m_contentHeight = clampedH;
    const HMONITOR monitor = MonitorFromPoint(m_anchor, MONITOR_DEFAULTTONEAREST);
    const RECT workArea = tools3000::core::dpi::workArea(monitor);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const LONG margin = tools3000::core::dpi::scaleMetric(TrayWindowStyle::BaseScreenMargin, scale);
    const int maxPhysicalH = std::max<int>(200, static_cast<int>(workArea.bottom - workArea.top - 2 * margin));
    const int scaledW = static_cast<int>(clampedW * scale);
    const int scaledH = std::min<int>(static_cast<int>(clampedH * scale), maxPhysicalH);

    m_updatingPlacement = true;
    const POINT origin = trayWindowOrigin(m_anchor.x, m_anchor.y, scaledW, scaledH);
    SetWindowPos(m_hwnd, HWND_TOPMOST, origin.x, origin.y, scaledW, scaledH,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const int radius = tools3000::core::dpi::scaleMetric(12, scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, scaledW, scaledH, radius);
    if (m_controller) {
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }
    m_updatingPlacement = false;
}

void TrayWindow::initializeWebView2() {
    const uint64_t generation = ++m_generation;
    WebViewEnvironmentManager::instance().acquire(
            [this, generation](HRESULT result, ICoreWebView2Environment* env) {
                if (FAILED(result) || !env) {
                    LOG_ERROR("TrayWindow: shared environment unavailable.");
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
                                LOG_WARN("TrayWindow: CreateCoreWebView2Controller 失败, res=0x{:08X}", static_cast<unsigned>(res));
                                return E_FAIL;
                            }
                            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                                if (controller) controller->Close();
                                return E_ABORT;
                            }
                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webView);
                            
                            if (m_visible.load()) {
                                m_controller->put_IsVisible(TRUE);
                                m_suspendController.resume(m_controller.Get());
                            } else {
                                m_controller->put_IsVisible(FALSE);
                                m_suspendController.requestSuspend(m_controller.Get(), m_webView.Get());
                            }
                            
                            // 开启真正完全透明背景 (ARGB = 0x00000000)
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(m_controller.As(&controller2))) {
                                COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};
                                controller2->put_DefaultBackgroundColor(transparent);
                            }

                            RECT bounds;
                            GetClientRect(m_hwnd, &bounds);
                            m_controller->put_Bounds(bounds);
                            syncWebViewDpi(m_controller.Get(), m_hwnd);

                            // 取消右键菜单和状态栏
                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            m_webView->get_Settings(&settings);
                            if (settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);

                                Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                }
                            }

                            web_security::applyNavigationPolicy(m_webView.Get());

                            // 加载 Tray URL
                            auto exeDir = tools3000::core::WinUtils::getExeDirectory();
                            auto indexPath = exeDir / L"ui" / L"index.html";
                            std::wstring baseUrl;
                            std::error_code ec;
                            if (std::filesystem::exists(indexPath, ec)) {
                                Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
                                if (SUCCEEDED(m_webView->QueryInterface(IID_PPV_ARGS(&webView3))) && webView3) {
                                    webView3->SetVirtualHostNameToFolderMapping(
                                        L"tools3000.local", (exeDir / L"ui").c_str(),
                                        COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                                }
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
                                LOG_ERROR("TrayWindow: 打包 UI 缺失，已拒绝连接开发服务器");
                                baseUrl = L"https://tools3000.local/index.html";
#endif
                            }

                            std::wstring targetUrl = baseUrl;
                            if (baseUrl.find(L"index.html") != std::wstring::npos) {
                                targetUrl += L"?tray=1";
                            } else {
                                targetUrl += L"/?tray=1";
                            }
                            
                            // 注册 JS bridge（必须在 Navigate 前完成，确保首帧 DocumentCreated 必注入）
                            m_webView->AddScriptToExecuteOnDocumentCreated(
                                L"window.chrome.webview.addEventListener('message', event => {"
                                L"  const msg = event.data;"
                                L"  if (window.tools3000Bridge && window.tools3000Bridge.onMessage) {"
                                L"      window.tools3000Bridge.onMessage(msg);"
                                L"  }"
                                L"});", nullptr);

                            m_webView->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this, generation](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        try {
                                            if (!web_security::isTrustedMessageSource(args)) return S_OK;
                                            if (generation != m_generation.load()) return S_OK;
                                            PWSTR messageRaw = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&messageRaw)) && messageRaw) {
                                                 const std::string request = tools3000::core::WinUtils::wstringToUtf8(messageRaw);
                                                 CoTaskMemFree(messageRaw);
                                                 if (!web_security::isBridgeMethodAllowed(
                                                         request, web_security::Surface::Tray)) return S_OK;
                                                 const std::string response =
                                                    tools3000::core::MessageBridge::instance().handleMessage(request);
                                                const std::wstring wideResponse =
                                                    tools3000::core::WinUtils::utf8ToWstring(response);
                                                sender->PostWebMessageAsString(wideResponse.c_str());
                                            }
                                        } catch (const std::exception& e) {
                                            LOG_ERROR("TrayWindow bridge error: {}", e.what());
                                        } catch (...) {
                                            LOG_ERROR("TrayWindow bridge unknown error");
                                        }
                                        return S_OK;
                                    }
                                ).Get(), nullptr);

                            KeyboardPipeline::applyWebKeyboardPolicy(m_controller.Get(), false);
                            tools3000::core::MessageBridge::instance().registerEventPusher("tray",
                                [this](const std::string& eventName, const nlohmann::json& data) {
                                    tools3000::core::MainThreadDispatcher::instance().post([this, eventName, data]() {
                                        if (!m_webView || !m_webViewReady.load()) return;
                                        const nlohmann::json envelope = {
                                            {"type", "event"}, {"event", eventName}, {"data", data}
                                        };
                                        const auto msg = tools3000::core::WinUtils::utf8ToWstring(envelope.dump());
                                        m_webView->PostWebMessageAsString(msg.c_str());
                                    });
                                }
                            );

                            EventRegistrationToken token;
                            m_webView->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this, generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                        if (generation != m_generation.load()) return S_OK;
                                        BOOL success = FALSE;
                                        args->get_IsSuccess(&success);
                                        if (!success) {
                                            COREWEBVIEW2_WEB_ERROR_STATUS status;
                                            args->get_WebErrorStatus(&status);
                                            LOG_ERROR("TrayWindow: WebView2 导航失败, status={}", static_cast<int>(status));
                                        }
                                        m_webViewReady = true;
                                        return S_OK;
                                    }
                                ).Get(), &token);

                            m_webView->Navigate(targetUrl.c_str());
                            LOG_INFO("TrayWindow: WebView2 渲染环境已就绪");
                            return S_OK;
                        }
                    ).Get());
                if (FAILED(controllerResult)) {
                    LOG_ERROR("TrayWindow: Create controller request failed, hr=0x{:08X}",
                              static_cast<unsigned>(controllerResult));
                }
            });
}

LRESULT CALLBACK TrayWindow::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (KeyboardPipeline::filterWindowMessage(hwnd, uMsg, wParam, lParam)) {
        return 0;
    }
    auto& inst = TrayWindow::instance();
    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_NCCALCSIZE:
            return 0; // 全阶段消除非客户区边框占用，使 WebView2 客户区 100% 贴合圆角窗口
        case WM_SIZE: {
            const int newW = LOWORD(lParam);
            const int newH = HIWORD(lParam);
            if (newW > 0 && newH > 0) {
                const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                const int radius = tools3000::core::dpi::scaleMetric(12, scale);
                tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, newW, newH, radius);
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
                if (IsWindowVisible(hwnd)) {
                    inst.m_controller->put_IsVisible(TRUE);
                }
            }
            break;
        }
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            if (!inst.m_updatingPlacement && IsWindowVisible(hwnd)) {
                inst.updatePlacement();
            }
            if (inst.m_controller) {
                syncWebViewDpi(inst.m_controller.Get(), hwnd);
            }
            return 0;
        case WM_ACTIVATEAPP:
            if (!inst.m_webViewReady.load()) break; // 渲染就绪前严禁失焦误杀
            if (wParam == FALSE && inst.m_visible.load()) {
                const uint64_t elapsed = GetTickCount64() - inst.m_showTimeTick;
                if (elapsed < 300) break; // 初始渲染与点击释放防抖 (300ms)
                if (tools3000::tray::TrayIcon::instance().isCursorOnTrayIcon()) {
                    break;
                }
                const HWND foreground = GetForegroundWindow();
                if (foreground == hwnd || (foreground && IsChild(hwnd, foreground))) {
                    break;
                }
                if (isTaskbarOrOverflowWindow(foreground)) {
                    break;
                }
                inst.hide();
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                if (!inst.m_webViewReady.load()) break; // 渲染就绪前严禁失焦误杀
                if (inst.m_visible.load()) {
                    const uint64_t elapsed = GetTickCount64() - inst.m_showTimeTick;
                    if (elapsed < 300) break; // 初始渲染与点击释放防抖 (300ms)
                    if (tools3000::tray::TrayIcon::instance().isCursorOnTrayIcon()) {
                        break;
                    }
                    const HWND foreground = GetForegroundWindow();
                    if (foreground == hwnd || (foreground && IsChild(hwnd, foreground))) {
                        break;
                    }
                    if (isTaskbarOrOverflowWindow(foreground)) {
                        break;
                    }
                    inst.hide();
                }
            } else {
                if (inst.m_controller) {
                    inst.m_controller->put_IsVisible(TRUE);
                    inst.m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                }
            }
            break;
        case WM_KILLFOCUS:
            if (!inst.m_webViewReady.load()) break; // 渲染就绪前严禁失焦误杀
            if (inst.m_visible.load()) {
                HWND newFocus = reinterpret_cast<HWND>(wParam);
                if (newFocus != hwnd && (!newFocus || !IsChild(hwnd, newFocus))) {
                    const uint64_t elapsed = GetTickCount64() - inst.m_showTimeTick;
                    if (elapsed < 300) break; // 初始渲染与点击释放防抖 (300ms)
                    if (tools3000::tray::TrayIcon::instance().isCursorOnTrayIcon()) {
                        break;
                    }
                    if (isTaskbarOrOverflowWindow(newFocus)) {
                        break;
                    }
                    const HWND foreground = GetForegroundWindow();
                    if (foreground == hwnd || (foreground && IsChild(hwnd, foreground))) {
                        break;
                    }
                    if (isTaskbarOrOverflowWindow(foreground)) {
                        break;
                    }
                    inst.hide();
                }
            }
            break;
        case WM_TIMER: {
            if (wParam == IDT_TRAY_AUTOHIDE) {
                if (!inst.m_visible.load()) {
                    KillTimer(hwnd, IDT_TRAY_AUTOHIDE);
                    break;
                }
                const uint64_t elapsed = GetTickCount64() - inst.m_showTimeTick;
                if (elapsed < 300) {
                    // 初始创建与托盘图标点击释放容差期 (300ms)
                    break;
                }

                // 1. 外部鼠标点击判定：若用户在托盘卡片外部按下了鼠标左键或右键，收起
                const bool lButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                const bool rButtonDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                if (lButtonDown || rButtonDown) {
                    POINT curPt{};
                    GetCursorPos(&curPt);
                    RECT winRc{};
                    GetWindowRect(hwnd, &winRc);
                    if (!PtInRect(&winRc, curPt)) {
                        HWND ptWnd = WindowFromPoint(curPt);
                        if (tools3000::tray::TrayIcon::instance().isCursorOnTrayIcon() ||
                            isTaskbarOrOverflowWindow(ptWnd)) {
                            // 点击在任务栏或托盘图标区域时，交由托盘原生点击消息自主处理 Toggle 收起
                            break;
                        }
                        inst.hide();
                        KillTimer(hwnd, IDT_TRAY_AUTOHIDE);
                        break;
                    }
                }

                // 2. 外部前台切换判定：若前台窗口不是本窗口且不是本窗口的子控件，立即收起
                const HWND foreground = GetForegroundWindow();
                if (foreground && foreground != hwnd && !IsChild(hwnd, foreground)) {
                    if (tools3000::tray::TrayIcon::instance().isCursorOnTrayIcon() ||
                        isTaskbarOrOverflowWindow(foreground)) {
                        break;
                    }
                    inst.hide();
                    KillTimer(hwnd, IDT_TRAY_AUTOHIDE);
                    break;
                }
            }
            break;
        }
        case WM_DESTROY:
            KillTimer(hwnd, IDT_TRAY_AUTOHIDE);
            if (inst.m_hwnd == hwnd) inst.m_hwnd = nullptr;
            inst.destroy();
            break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

} // namespace tools3000::ui
