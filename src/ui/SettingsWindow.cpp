// ─────────────────────────────────────────────────────────────────────────────
// SettingsWindow.cpp — WebView2 设置窗口实现
//
// 核心流程:
//   1. 创建 Win32 顶层窗口（可调整大小、有标题栏）
//   2. CreateCoreWebView2EnvironmentWithOptions → 初始化 WebView2 环境
//   3. 环境就绪后 CreateCoreWebView2Controller → 创建 WebView2 控件
//   4. 控件就绪后加载前端 UI + 注册 JS↔C++ 消息桥
//   5. 窗口大小变化时自动调整 WebView2 控件尺寸
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/SettingsWindow.h"
#include "ui/WebViewWindowStyle.h"
#include "../resource.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/ipc/MessageBridge.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/config/ConfigManager.h"
#include "core/stats/PerformanceMonitor.h"
#include "ui/WebViewEnvironmentManager.h"
#include "ui/WebViewDpi.h"
#include "ui/WebViewSecurity.h"
#include "ui/WebViewSuspend.h"
#include "ui/KeyboardPipeline.h"

// WebView2 SDK 头文件
#include <windowsx.h>
#include <dwmapi.h>
#include <WebView2.h>
#include <wrl/event.h>

#include <filesystem>
#include <commctrl.h>
#include <fstream>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <nlohmann/json.hpp>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_MAINWINDOW 2 // Mica
#define DWMSBT_TRANSIENTWINDOW 3 // Acrylic
#endif

using namespace Microsoft::WRL;

namespace tools3000::ui {

namespace {

bool isSettingsDarkTheme() {
    const std::string theme = tools3000::core::ConfigManager::instance().get<std::string>("/general/theme", "dark");
    if (theme == "light") return false;
    if (theme == "dark") return true;
    return tools3000::core::WinUtils::isSystemDarkMode();
}

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
            const int border = tools3000::core::dpi::scaleMetric(8, scale);
            // 当鼠标位于父窗口缩放后边缘范围内时，子窗口声明透明，让宿主窗口响应拉伸
            if (pt.x < rc.left + border || pt.x >= rc.right - border ||
                pt.y < rc.top + border || pt.y >= rc.bottom - border) {
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

} // namespace

static constexpr const wchar_t* SETTINGS_WINDOW_CLASS = L"Tools3000_SettingsWindow";

SettingsWindow& SettingsWindow::instance() {
    static SettingsWindow inst;
    return inst;
}

void SettingsWindow::show(HINSTANCE hInstance) {
    tools3000::core::TraceId::Scope scope;
    m_showRequestedAt = std::chrono::steady_clock::now();
    if (hInstance) m_hInstance = hInstance;
    else if (!m_hInstance) m_hInstance = GetModuleHandleW(nullptr);

    if (m_hwnd && IsWindow(m_hwnd)) {
        // 用户重新激活设置窗口，立即取消 1 分钟闲置销毁倒计时并复用窗口
        KillTimer(m_hwnd, IDT_IDLE_DESTROY);

        const std::wstring windowTitle = tools3000::core::WinUtils::isCurrentProcessElevated()
            ? L"Tools3000 设置 (管理员)"
            : L"Tools3000 设置";
        SetWindowTextW(m_hwnd, windowTitle.c_str());

        if (m_webViewReady) {
            smoothPresent();
        } else {
            m_showWhenReady = true;
            if (m_webView) m_suspendController.resume(m_webView.Get(), "settings");
        }
        
        LOG_INFO("设置窗口已激活（复用已有窗口，已取消闲置销毁定时器）");
        return;
    }

    m_showWhenReady = true;
    if (!createWindow(m_hInstance)) {
        LOG_ERROR("创建设置窗口失败");
        return;
    }

    initializeWebView2();
    LOG_INFO("设置窗口已按需创建，正等待 WebView2 首帧就绪后丝滑呈现");
}

void SettingsWindow::smoothPresent() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else {
        ShowWindow(m_hwnd, SW_SHOW);
    }
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    m_visible = true;

    if (m_webView) m_suspendController.resume(m_webView.Get(), "settings");
    if (m_controller) {
        m_controller->put_IsVisible(TRUE);
        syncWebViewDpi(m_controller.Get(), m_hwnd);
    }

    if (m_showRequestedAt != std::chrono::steady_clock::time_point{}) {
        tools3000::core::PerformanceMonitor::instance().recordLatency(
            "settings.open",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m_showRequestedAt).count());
        m_showRequestedAt = {};
    }
}

void SettingsWindow::preload(HINSTANCE hInstance) {
    tools3000::core::TraceId::Scope scope;
    if (hInstance) m_hInstance = hInstance;
    else if (!m_hInstance) m_hInstance = GetModuleHandleW(nullptr);

    if (m_hwnd && IsWindow(m_hwnd)) {
        return; // 已创建
    }

    if (createWindow(m_hInstance)) {
        m_initializationStartedAt = std::chrono::steady_clock::now();
        initializeWebView2(); // 初始化 WebView2，但不调用 ShowWindow，实现静默预热
        LOG_INFO("设置窗口后台静默预热完成");
    } else {
        LOG_ERROR("设置窗口后台静默预热失败");
    }
}

void SettingsWindow::hide() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        persistGeometry();
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        
        if (m_controller) {
            m_controller->put_IsVisible(FALSE);
        }

        // 挂起 Chromium 渲染管线以释放 GPU/DOM 显存与工作集
        if (m_webView) m_suspendController.requestSuspend(m_webView.Get(), "settings");

        const bool autoRelease = tools3000::core::ConfigManager::instance().get<bool>(
            "/general/autoReleaseSettingsMemory", true);
        if (autoRelease) {
            // 启动 1 分钟闲置销毁定时器：若用户 60 秒内未再次打开设置，则彻底销毁 WebView2 释放物理内存
            SetTimer(m_hwnd, IDT_IDLE_DESTROY, IDLE_DESTROY_TIMEOUT_MS, nullptr);
            LOG_INFO("设置窗口已隐藏，已启动 1 分钟闲置自动销毁倒计时");
        } else {
            KillTimer(m_hwnd, IDT_IDLE_DESTROY);
            LOG_INFO("设置窗口已隐藏（自动释放内存开关已关闭，保持后台常驻）");
        }
    }
}

bool SettingsWindow::isVisible() const {
    return m_visible.load() && m_hwnd && IsWindowVisible(m_hwnd);
}

void SettingsWindow::destroy() {
    m_suspendController.abandon();
    ++m_generation;
    tools3000::core::MessageBridge::instance().unregisterEventPusher("settings");
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
    m_visible = false;
    m_webViewReady = false;
    tools3000::core::WinUtils::trimWorkingSet();
    LOG_INFO("设置窗口已彻底销毁并释放 WebView2 渲染器");
}

// ─────────────────────────────────────────────────────────────────────────────
// Win32 窗口创建
// ─────────────────────────────────────────────────────────────────────────────

bool SettingsWindow::createWindow(HINSTANCE hInstance) {
    m_suspendController.reset();

    if (!m_config.hasCustomPlacement) {
        applyPersistedPlacementIfAny();
    }

    const POINT placementOrigin{m_config.hasCustomPlacement ? m_config.posX : 0,
                                m_config.hasCustomPlacement ? m_config.posY : 0};
    const HMONITOR monitor = m_config.hasCustomPlacement
        ? MonitorFromPoint(placementOrigin, MONITOR_DEFAULTTONEAREST)
        : tools3000::core::dpi::activeMonitor();
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
    const float scale = tools3000::core::dpi::scaleForDpi(dpi);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = SETTINGS_WINDOW_CLASS;
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    if (!wc.hIcon) wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR | LR_SHARED);
    RegisterClassExW(&wc);

    SIZE targetSize = (m_config.width == SettingsWindowStyle::BaseWidth &&
                       m_config.height == SettingsWindowStyle::BaseHeight)
        ? SettingsWindowStyle::windowSizeForDpi(dpi)
        : SIZE{tools3000::core::dpi::scaleMetric(m_config.width, scale),
               tools3000::core::dpi::scaleMetric(m_config.height, scale)};

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    const int margin = tools3000::core::dpi::scaleMetric(SettingsWindowStyle::BaseScreenMargin, scale);
    if (m_config.hasCustomPlacement) {
        targetSize.cx = (std::max)(400, m_config.width);
        targetSize.cy = (std::max)(300, m_config.height);
        const RECT proposed{
            m_config.posX,
            m_config.posY,
            m_config.posX + targetSize.cx,
            m_config.posY + targetSize.cy};
        const RECT clamped = tools3000::core::dpi::clampWindowToWorkArea(proposed, work);
        x = clamped.left;
        y = clamped.top;
        targetSize.cx = clamped.right - clamped.left;
        targetSize.cy = clamped.bottom - clamped.top;
    } else {
        targetSize = tools3000::core::dpi::fitSizeToWorkArea(targetSize, work, margin);
        x = m_config.startCentered
            ? work.left + (work.right - work.left - targetSize.cx) / 2
            : CW_USEDEFAULT;
        y = m_config.startCentered
            ? work.top + (work.bottom - work.top - targetSize.cy) / 2
            : CW_USEDEFAULT;
    }

    const std::wstring windowTitle = tools3000::core::WinUtils::isCurrentProcessElevated()
        ? L"Tools3000 设置 (管理员)"
        : L"Tools3000 设置";

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        SETTINGS_WINDOW_CLASS,
        windowTitle.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, targetSize.cx, targetSize.cy,
        nullptr, nullptr, hInstance, this  // 传递 this 指针
    );

    if (!m_hwnd) {
        LOG_ERROR("CreateWindowExW 失败, error={}", GetLastError());
        return false;
    }

    // 启用 DWM 全客户区扩展与跨平台通用圆角裁剪，全兼容 Win11、Win10、Server 2022/2025
    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    // 强制通知系统 Frame 已变更，立即移除系统黑色标题栏
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    const int radius = static_cast<int>(12 * scale);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(m_hwnd, targetSize.cx, targetSize.cy, radius);
    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (wc.hIcon) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    }
    if (wc.hIconSm) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);
    }

    LOG_DEBUG("Win32 设置窗口已创建, size={}x{} on DPI {}", targetSize.cx, targetSize.cy, dpi);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// WebView2 初始化
// ─────────────────────────────────────────────────────────────────────────────

void SettingsWindow::initializeWebView2() {
    if (m_initializationStartedAt == std::chrono::steady_clock::time_point{}) {
        m_initializationStartedAt = std::chrono::steady_clock::now();
    }
    const uint64_t generation = ++m_generation;
    WebViewEnvironmentManager::instance().acquire(
        [this, generation](HRESULT result, ICoreWebView2Environment* environment) {
            // destroy() invalidates the generation before closing WebView2.
            // An E_ABORT callback after that is expected cancellation, not a
            // startup failure worth alarming the user or polluting telemetry.
            if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                return;
            }
            if (FAILED(result) || !environment) {
                LOG_ERROR("WebView2 环境获取失败, hr=0x{:08X}", static_cast<unsigned>(result));
                return;
            }

            m_environment = environment;
            const HRESULT controllerResult = environment->CreateCoreWebView2Controller(
                m_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, generation](HRESULT readyResult,
                                       ICoreWebView2Controller* controller) -> HRESULT {
                        if (generation != m_generation.load() || !m_hwnd || !IsWindow(m_hwnd)) {
                            if (controller) controller->Close();
                            LOG_DEBUG("WebView2 控件创建已因窗口销毁而取消");
                            return S_OK;
                        }
                        if (FAILED(readyResult) || !controller) {
                            LOG_ERROR("WebView2 控件创建失败, hr=0x{:08X}",
                                      static_cast<unsigned>(readyResult));
                            return readyResult;
                        }
                        m_controller = controller;
                        const HRESULT hrGetWeb = controller->get_CoreWebView2(&m_webView);
                        if (FAILED(hrGetWeb) || !m_webView) {
                            LOG_WARN("SettingsWindow: get_CoreWebView2 失败, hr=0x{:08X}", static_cast<unsigned>(hrGetWeb));
                            return hrGetWeb;
                        }
                        onWebView2Ready();
                        return S_OK;
                    }).Get());
            if (FAILED(controllerResult)) {
                LOG_ERROR("WebView2 控件创建请求失败, hr=0x{:08X}",
                          static_cast<unsigned>(controllerResult));
            }
        });
}

void SettingsWindow::onWebView2Ready() {
    if (!m_webView || !m_controller) return;

    const uint64_t generation = m_generation.load();
    tools3000::core::TraceId::Scope scope;
    LOG_INFO("WebView2 控件就绪");

    // ── 调整控件尺寸以填满窗口并确保可见 ─────────────────────────────────
    if (m_visible.load()) {
        m_controller->put_IsVisible(TRUE);
    } else {
        m_controller->put_IsVisible(FALSE);
    }
    
    RECT bounds;
    GetClientRect(m_hwnd, &bounds);
    m_controller->put_Bounds(bounds);
    syncWebViewDpi(m_controller.Get(), m_hwnd);

    // ── 配置 WebView2 设置 ──────────────────────────────────────────────
    ComPtr<ICoreWebView2Settings> settings;
    const HRESULT hrGetSettings = m_webView->get_Settings(&settings);

    if (SUCCEEDED(hrGetSettings) && settings) {
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);

        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3)) && settings3) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }

#ifdef _DEBUG
        settings->put_AreDevToolsEnabled(m_config.devToolsEnabled ? TRUE : FALSE);
#else
        settings->put_AreDevToolsEnabled(FALSE);
#endif
    } else if (FAILED(hrGetSettings)) {
        LOG_WARN("SettingsWindow: get_Settings 失败, hr=0x{:08X}", static_cast<unsigned>(hrGetSettings));
    }

    // Serve packaged UI from a constrained virtual HTTPS origin instead of
    // granting broad file:// access to the browser process.
    const auto uiFolder = (tools3000::core::WinUtils::getExeDirectory() / L"ui").wstring();
    std::error_code mappingError;
    if (std::filesystem::exists(uiFolder, mappingError)) {
        ComPtr<ICoreWebView2_3> webView3;
        if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
            webView3->SetVirtualHostNameToFolderMapping(
                L"tools3000.local", uiFolder.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        }
    }

    // ── 设置 WebView2 透明背景，使得底层 Mica 材质透出 ──────────────────
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(m_controller.As(&controller2))) {
        COREWEBVIEW2_COLOR color = { 0, 0, 0, 0 }; // 完全透明
        controller2->put_DefaultBackgroundColor(color);
    }

    web_security::applyNavigationPolicy(m_webView.Get());
    KeyboardPipeline::applyWebKeyboardPolicy(m_controller.Get(), m_config.devToolsEnabled);

    // ── 注册 JS → C++ 消息监听 ─────────────────────────────────────────
    m_webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this, generation](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (!web_security::isTrustedMessageSource(args)) return S_OK;
                if (generation != m_generation.load()) return S_OK;
                // 关键: C++ 异常绝不能逃逸回 WebView2 的 native 调用栈, 否则进程崩溃。
                try {
                    LPWSTR messageRaw = nullptr;
                    args->TryGetWebMessageAsString(&messageRaw);

                    if (messageRaw) {
                        std::string message = tools3000::core::WinUtils::wstringToUtf8(messageRaw);
                        CoTaskMemFree(messageRaw);

                        if (!web_security::isBridgeMethodAllowed(
                                message, web_security::Surface::Settings)) return S_OK;

                        tools3000::core::MessageBridge::instance().handleMessageAsync(
                            message,
                            [this, generation](std::string response) {
                                tools3000::core::MainThreadDispatcher::instance().post(
                                    [this, generation, response = std::move(response)]() {
                                        if (generation != m_generation.load() || !m_webView) return;
                                        std::wstring wResponse = tools3000::core::WinUtils::utf8ToWstring(response);
                                        m_webView->PostWebMessageAsString(wResponse.c_str());
                                    });
                            });
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("WebMessageReceived 处理异常: {}", e.what());
                } catch (...) {
                    LOG_ERROR("WebMessageReceived 处理未知异常");
                }
                return S_OK;
            }
        ).Get(),
        nullptr
    );

    // ── 设置事件推送器（C++ → JS）───────────────────────────────────────
    tools3000::core::MessageBridge::instance().registerEventPusher(
        "settings",
        [this](const std::string& eventName, const nlohmann::json& data) {
            tools3000::core::MainThreadDispatcher::instance().post([this, eventName, data]() {
                if (!m_webView || !m_webViewReady) return;
                const nlohmann::json envelope = {
                    {"type", "event"}, {"event", eventName}, {"data", data}
                };
                const auto msg = tools3000::core::WinUtils::utf8ToWstring(envelope.dump());
                m_webView->PostWebMessageAsString(msg.c_str());
            });
        }
    );

    // ── 加载前端 UI ─────────────────────────────────────────────────────
    std::string entryUrl = getUIEntryUrl();
    std::wstring wUrl = tools3000::core::WinUtils::utf8ToWstring(entryUrl);
    
    // 监听导航失败事件，记录详细错误，方便诊断白屏
    EventRegistrationToken token;
    m_webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this, entryUrl](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL success;
                args->get_IsSuccess(&success);
                if (success) {
                    m_webViewReady = true;
                    if (m_showWhenReady) {
                        m_showWhenReady = false;
                        smoothPresent();
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (m_initializationStartedAt != std::chrono::steady_clock::time_point{}) {
                        tools3000::core::PerformanceMonitor::instance().recordLatency(
                            "settings.navigation",
                            std::chrono::duration<double, std::milli>(
                                now - m_initializationStartedAt).count());
                        m_initializationStartedAt = {};
                    }
                    if (m_showRequestedAt != std::chrono::steady_clock::time_point{}) {
                        tools3000::core::PerformanceMonitor::instance().recordLatency(
                            "settings.open",
                            std::chrono::duration<double, std::milli>(
                                now - m_showRequestedAt).count());
                        m_showRequestedAt = {};
                    }
                    LOG_INFO("WebView2 导航成功: {}", entryUrl);
                } else {
                    COREWEBVIEW2_WEB_ERROR_STATUS status;
                    args->get_WebErrorStatus(&status);
                    LOG_ERROR("WebView2 导航失败, status={}", static_cast<int>(status));
                    m_webViewReady = true;
                    if (m_showWhenReady) {
                        m_showWhenReady = false;
                        smoothPresent();
                    }
                }
                return S_OK;
            }
        ).Get(), &token);

    m_webView->Navigate(wUrl.c_str());
    hookWebViewChildWindows(m_hwnd);

    LOG_INFO("WebView2 正在加载前端 UI: {}", entryUrl);
}

std::string SettingsWindow::getUIEntryUrl() const {
    auto exeDir = tools3000::core::WinUtils::getExeDirectory();
    auto indexPath = exeDir / L"ui" / L"index.html";

    // 1. 优先使用本地单文件包，通过受限虚拟 HTTPS 源离线加载。
    std::error_code ec;
    if (std::filesystem::exists(indexPath, ec)) {
        return "https://tools3000.local/index.html";
    }

#ifndef _DEBUG
    // Release 构建必须失败关闭。绝不能因为安装文件损坏而连接可能被其他本机进程
    // 占用的开发端口，同时向该页面暴露原生消息桥。
    LOG_ERROR("打包 UI 缺失: {}", tools3000::core::WinUtils::wstringToUtf8(indexPath.wstring()));
    return "https://tools3000.local/index.html";
#else

    // 2. 如果本地不存在，说明是在 C++ 开发模式下运行。尝试读取 Vite 动态端口文件
    auto devUrlPath = exeDir.parent_path().parent_path().parent_path() / L"ui" / L".dev-server-url";
    if (std::filesystem::exists(devUrlPath, ec)) {
        try {
            std::ifstream file(devUrlPath);
            std::string url;
            if (std::getline(file, url) && !url.empty()) {
                LOG_INFO("成功读取到动态开发服务器地址: {}", url);
                return url;
            }
        } catch (const std::exception& ex) {
            LOG_WARN("读取开发服务器动态端口文件异常: {}", ex.what());
        } catch (...) {
            LOG_WARN("读取开发服务器动态端口文件发生未知异常");
        }
    }

    // 3. 配置中的备用开发地址 (如果有的话，主要防呆)
    if (!m_config.devServerUrl.empty() && m_config.devServerUrl != "http://localhost:5173") {
        return m_config.devServerUrl;
    }

    // 4. 终极降级: 使用开发服务器默认地址
    LOG_WARN("未找到本地 UI 文件及动态端口文件, 尝试连接默认开发服务器 http://localhost:5173");
    return "http://localhost:5173";
#endif
}

void SettingsWindow::applyPersistedPlacementIfAny() {
    auto& cfg = tools3000::core::ConfigManager::instance();
    if (!cfg.has("/ui/settingsWindow/width") || !cfg.has("/ui/settingsWindow/height")) {
        return;
    }

    const int savedX = cfg.get<int>("/ui/settingsWindow/x", 0);
    const int savedY = cfg.get<int>("/ui/settingsWindow/y", 0);
    const int savedW = cfg.get<int>("/ui/settingsWindow/width", SettingsWindowStyle::BaseWidth);
    const int savedH = cfg.get<int>("/ui/settingsWindow/height", SettingsWindowStyle::BaseHeight);
    const int savedDpi = cfg.get<int>("/ui/settingsWindow/dpi", static_cast<int>(tools3000::core::dpi::DefaultDpi));

    const POINT origin{savedX, savedY};
    const unsigned targetDpi = tools3000::core::dpi::effectiveDpiForMonitor(
        MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST));
    const float ratio = tools3000::core::dpi::scaleForDpi(targetDpi) /
                        tools3000::core::dpi::scaleForDpi(static_cast<unsigned>(
                            (std::max)(1, savedDpi)));

    // 若历史记录保存的尺寸超过或接近全屏，平滑回退为标准适中尺寸
    const int scaledW = static_cast<int>(std::lround(savedW * ratio));
    const int scaledH = static_cast<int>(std::lround(savedH * ratio));
    const RECT workArea = tools3000::core::dpi::workArea(MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST));
    const int maxAcceptableW = static_cast<int>((workArea.right - workArea.left) * 0.88f);
    const int maxAcceptableH = static_cast<int>((workArea.bottom - workArea.top) * 0.88f);

    if (scaledW > maxAcceptableW || scaledH > maxAcceptableH) {
        m_config.width = SettingsWindowStyle::BaseWidth;
        m_config.height = SettingsWindowStyle::BaseHeight;
        m_config.hasCustomPlacement = false;
        m_config.startCentered = true;
        return;
    }

    m_config.posX = savedX;
    m_config.posY = savedY;
    m_config.width = (std::max)(400, scaledW);
    m_config.height = (std::max)(300, scaledH);
    m_config.hasCustomPlacement = true;
    m_config.startCentered = false;
}

void SettingsWindow::persistGeometry() {
    if (!m_hwnd || !IsWindow(m_hwnd) || IsIconic(m_hwnd)) return;

    RECT rc{};
    if (!GetWindowRect(m_hwnd, &rc)) return;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    tools3000::core::ConfigManager::instance().mergePatch({
        {"ui", {
            {"settingsWindow", {
                {"x", rc.left},
                {"y", rc.top},
                {"width", width},
                {"height", height},
                {"dpi", static_cast<int>(tools3000::core::dpi::effectiveDpiForWindow(m_hwnd))}
            }}
        }}
    }, "/ui/settingsWindow");
}

void SettingsWindow::navigateTo(const std::string& path) {
    if (!tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, path]() { navigateTo(path); });
        return;
    }
    if (!m_webView || !m_webViewReady) return;

    // 通过 JS 路由导航（采用 JSON 字符串序列化安全转义引号、反斜杠与控制字符）
    std::string script = "window.location.hash = " + nlohmann::json(path).dump() + ";";
    std::wstring wScript = tools3000::core::WinUtils::utf8ToWstring(script);
    m_webView->ExecuteScript(wScript.c_str(), nullptr);
}

void SettingsWindow::pushEventToFrontend(const std::string& eventName, const std::string& dataJson) {
    if (!tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post(
            [this, eventName, dataJson]() { pushEventToFrontend(eventName, dataJson); });
        return;
    }
    if (!m_webView || !m_webViewReady) return;

    std::string envelope = R"({"type":"event","event":")" + eventName + R"(","data":)" + dataJson + "}";
    std::wstring wMsg = tools3000::core::WinUtils::utf8ToWstring(envelope);
    m_webView->PostWebMessageAsString(wMsg.c_str());
}

void SettingsWindow::minimize() {
    if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
}

void SettingsWindow::toggleMaximize() {
    if (!m_hwnd) return;
    if (IsZoomed(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else {
        ShowWindow(m_hwnd, SW_MAXIMIZE);
    }
}

void SettingsWindow::close() {
    if (m_hwnd) SendMessageW(m_hwnd, WM_CLOSE, 0, 0);
}

void SettingsWindow::dragMove() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    ReleaseCapture();
    PostMessageW(m_hwnd, WM_SYSCOMMAND, 0xF012, 0);
}

void SettingsWindow::showSystemMenu(int screenX, int screenY) {
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
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_TOPALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, m_hwnd, nullptr);
    if (cmd > 0) {
        PostMessageW(m_hwnd, WM_SYSCOMMAND, cmd, 0);
    }
}

void SettingsWindow::startResize(const std::string& edge) {
    if (!m_hwnd || !IsWindow(m_hwnd) || IsZoomed(m_hwnd)) return;
    ReleaseCapture();
    WPARAM scSizeParam = 0;
    if (edge == "left") scSizeParam = 0xF001;          // SC_SIZE + WMSZ_LEFT
    else if (edge == "right") scSizeParam = 0xF002;    // SC_SIZE + WMSZ_RIGHT
    else if (edge == "top") scSizeParam = 0xF003;      // SC_SIZE + WMSZ_TOP
    else if (edge == "top_left") scSizeParam = 0xF004; // SC_SIZE + WMSZ_TOPLEFT
    else if (edge == "top_right") scSizeParam = 0xF005;// SC_SIZE + WMSZ_TOPRIGHT
    else if (edge == "bottom") scSizeParam = 0xF006;   // SC_SIZE + WMSZ_BOTTOM
    else if (edge == "bottom_left") scSizeParam = 0xF007;// SC_SIZE + WMSZ_BOTTOMLEFT
    else if (edge == "bottom_right") scSizeParam = 0xF008;// SC_SIZE + WMSZ_BOTTOMRIGHT
    else return;

    PostMessageW(m_hwnd, WM_SYSCOMMAND, scSizeParam, 0);
}

bool SettingsWindow::isMaximized() const {
    return m_hwnd && (IsZoomed(m_hwnd) != FALSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK SettingsWindow::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_TIMER: {
            if (wParam == IDT_IDLE_DESTROY) {
                KillTimer(hwnd, IDT_IDLE_DESTROY);
                const bool autoRelease = tools3000::core::ConfigManager::instance().get<bool>(
                    "/general/autoReleaseSettingsMemory", true);
                if (autoRelease && self && !self->isVisible()) {
                    LOG_INFO("设置窗口已闲置 1 分钟，自动销毁 Win32 窗口并释放 WebView2 渲染进程物理内存");
                    self->destroy();
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
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
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_NCHITTEST: {
            if (!IsZoomed(hwnd)) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                RECT rc;
                GetWindowRect(hwnd, &rc);
                const float scale = tools3000::core::dpi::scaleForWindow(hwnd);
                const int border = tools3000::core::dpi::scaleMetric(8, scale);

                if (pt.y < rc.top + border && pt.x < rc.left + border) return HTTOPLEFT;
                if (pt.y < rc.top + border && pt.x >= rc.right - border) return HTTOPRIGHT;
                if (pt.y >= rc.bottom - border && pt.x < rc.left + border) return HTBOTTOMLEFT;
                if (pt.y >= rc.bottom - border && pt.x >= rc.right - border) return HTBOTTOMRIGHT;
                if (pt.y < rc.top + border) return HTTOP;
                if (pt.y >= rc.bottom - border) return HTBOTTOM;
                if (pt.x < rc.left + border) return HTLEFT;
                if (pt.x >= rc.right - border) return HTRIGHT;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_SIZE: {
            if (self) {
                const bool maxState = (wParam == SIZE_MAXIMIZED || IsZoomed(hwnd));
                nlohmann::json data = {{"isMaximized", maxState}};
                self->pushEventToFrontend("window:maximizedChanged", data.dump());

                const int newW = LOWORD(lParam);
                const int newH = HIWORD(lParam);
                if (newW > 0 && newH > 0) {
                    if (maxState) {
                        SetWindowRgn(hwnd, nullptr, TRUE);
                    } else {
                        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                        const int radius = static_cast<int>(12 * scale);
                        tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, newW, newH, radius);
                    }
                }

                if (self->m_controller) {
                    syncWebViewDpi(self->m_controller.Get(), hwnd);
                    hookWebViewChildWindows(hwnd);
                    if (IsWindowVisible(hwnd)) {
                        self->m_controller->put_IsVisible(TRUE);
                    }
                }
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1; // 阻止 Win32 默认黑白背景擦除引发的闪烁

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // 按当前深浅色主题自适应填充背景底色，彻底消除浅色模式拖拽拉伸闪黑
            const COLORREF bgColor = isSettingsDarkTheme() ? RGB(15, 23, 42) : RGB(248, 250, 252);
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            if (hBrush) {
                FillRect(hdc, &ps.rcPaint, hBrush);
                DeleteObject(hBrush);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE: {
            if (self) self->persistGeometry();
            if (!tools3000::core::ConfigManager::instance().get<bool>(
                    "/general/minimizeToTray", true)) {
                PostQuitMessage(0);
            } else if (self) self->hide();
            return 0;  // 不调用 DestroyWindow
        }

        case WM_EXITSIZEMOVE: {
            if (self) {
                self->persistGeometry();
                if (!IsZoomed(hwnd)) {
                    RECT rc;
                    if (GetWindowRect(hwnd, &rc)) {
                        const int w = rc.right - rc.left;
                        const int h = rc.bottom - rc.top;
                        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                        const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
                        const int radius = static_cast<int>(12 * scale);
                        tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, w, h, radius);
                    }
                }
            }
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            const unsigned newDpi = HIWORD(wParam);
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            HICON hNewIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                GetSystemMetricsForDpi(SM_CXICON, newDpi), GetSystemMetricsForDpi(SM_CYICON, newDpi), LR_DEFAULTCOLOR | LR_SHARED);
            HICON hNewIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                GetSystemMetricsForDpi(SM_CXSMICON, newDpi), GetSystemMetricsForDpi(SM_CYSMICON, newDpi), LR_DEFAULTCOLOR | LR_SHARED);
            if (hNewIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hNewIcon);
            }
            if (hNewIconSm) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hNewIconSm);
            }
            if (self && self->m_controller) {
                syncWebViewDpi(self->m_controller.Get(), hwnd);
            }
            if (self) self->persistGeometry();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            const unsigned dpi = tools3000::core::dpi::effectiveDpiForWindow(hwnd);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            const RECT work = tools3000::core::dpi::workArea(monitor);
            const int workW = (std::max)(1, static_cast<int>(work.right - work.left));
            const int workH = (std::max)(1, static_cast<int>(work.bottom - work.top));

            const SIZE minSize = SettingsWindowStyle::minWindowSizeForDpi(dpi);
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = (std::min)(minSize.cx, static_cast<LONG>(workW * 0.9f));
            mmi->ptMinTrackSize.y = (std::min)(minSize.cy, static_cast<LONG>(workH * 0.9f));
            return 0;
        }

        case WM_SYSCOMMAND:
        case WM_HELP: {
            if (KeyboardPipeline::filterWindowMessage(hwnd, msg, wParam, lParam)) {
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace tools3000::ui
