// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — Tools3000 程序入口点
//
// 启动流程:
//   1. 单实例检测（避免重复启动）
//   2. COM 初始化
//   3. 崩溃处理器安装
//   4. 日志系统初始化
//   5. 配置管理器初始化
//   6. 创建隐藏消息窗口（消息泵）
//   7. 系统托盘图标创建
//   8. 全局快捷键注册
//   9. 手势引擎启动
//  10. WebView2 设置窗口（按需创建）
//  11. 消息循环
// ─────────────────────────────────────────────────────────────────────────────

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ElevationPolicy.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/MouseHook.h"
#include "core/ipc/MessageBridge.h"
#include "core/stats/StatsManager.h"
#include "core/crash/CrashHandler.h"
#include "core/lua/LuaEngine.h"
#include "core/plugin/PluginManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/update/UpdateChecker.h"
#include "core/utils/ShellContextMenuService.h"
#include "core/ipc/AutoStartPolicy.h"
#include "Tools3000Version.h"
#include "tray/TrayIcon.h"
#include "ui/SettingsWindow.h"
#include "ui/SearchWindow.h"
#include "ui/QuickLookWindow.h"
#include "ui/TrayWindow.h"
#include "ui/ToastOverlay.h"
#include "ui/SpotlightOverlay.h"
#include "ui/WebViewEnvironmentManager.h"
#include "gesture/GestureInputPolicy.h"
#include "core/remote/RemoteMasterEngine.h"

// ── 常量 ─────────────────────────────────────────────────────────────────────
static constexpr const wchar_t* WINDOW_CLASS_NAME = L"Tools3000_MessageWindow";
static constexpr const wchar_t* WINDOW_TITLE      = L"Tools3000MessageWindow";
static constexpr const wchar_t* MUTEX_NAME        = L"Global\\Tools3000_SingleInstance_Mutex";
static constexpr UINT WM_TOOLS3000_SHOW_SETTINGS  = WM_APP + 101;
static constexpr UINT_PTR TIMER_ID_QUICKLOOK_REFRESH = 0x4551;
static constexpr UINT_PTR TIMER_ID_PERFORMANCE_BASELINE = 0x4552;

// ── 前向声明 ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool checkSingleInstance();
bool hasCommandLineFlag(std::wstring_view flag);
std::optional<std::filesystem::path> commandLinePathValue(std::wstring_view prefix);
std::optional<std::wstring> commandLineStringValue(std::wstring_view prefix);
HWND createMessageWindow(HINSTANCE hInstance);
void initializeSubsystems(HWND hwnd, bool preloadSettings);
void shutdownSubsystems();
void showSettingsWindow();
void preloadSettingsWindow(HINSTANCE hInstance);

// ── 全局状态 ─────────────────────────────────────────────────────────────────
static HANDLE g_singleInstanceMutex = nullptr;
static UINT g_wmTaskbarCreated = 0;
static std::optional<std::filesystem::path> g_performanceScenarioOutput;
static std::atomic<bool> g_restartLaunchStarted{false};
static HPOWERNOTIFY g_powerNotify = nullptr;

namespace {

bool writePerformanceBaselineSnapshot(const std::filesystem::path& outputPath) {
    std::error_code error;
    const auto parent = outputPath.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent, error) || error) {
        LOG_ERROR("Performance baseline output directory is unavailable: {}", outputPath.string());
        return false;
    }
    nlohmann::json snapshot = {
        {"schemaVersion", 1},
        {"processId", GetCurrentProcessId()},
        {"metrics", tools3000::core::PerformanceMonitor::instance().getMetricsJson()}
    };
    const auto temporary = outputPath.wstring() + L".partial";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            LOG_ERROR("Cannot create performance baseline snapshot: {}", outputPath.string());
            return false;
        }
        stream << snapshot.dump(2) << '\n';
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, error);
            LOG_ERROR("Cannot write performance baseline snapshot: {}", outputPath.string());
            return false;
        }
    }
    std::filesystem::rename(temporary, outputPath, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        LOG_ERROR("Cannot finalize performance baseline snapshot: {}", outputPath.string());
        return false;
    }
    return true;
}

void releaseSingleInstanceMutex() {
    if (!g_singleInstanceMutex) return;
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
}

HANDLE createSingleInstanceMutex() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;

    DWORD tokenBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    if (tokenBytes == 0) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        SetLastError(error);
        return nullptr;
    }

    std::vector<std::byte> tokenBuffer(tokenBytes);
    if (!GetTokenInformation(token, TokenUser, tokenBuffer.data(), tokenBytes,
                             &tokenBytes)) {
        const DWORD error = GetLastError();
        CloseHandle(token);
        SetLastError(error);
        return nullptr;
    }
    CloseHandle(token);

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText) || !sidText) {
        return nullptr;
    }

    // The explicit user ACE lets a standard-token successor synchronize with
    // a mutex created by the same user's elevated token. SYSTEM and local
    // administrators retain recovery access; unrelated users receive none.
    // An explicit medium integrity label prevents low-integrity processes from
    // acquiring and indefinitely holding the application hand-off barrier.
    const std::wstring sddl =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;" +
        std::wstring(sidText) + L")S:(ML;;NW;;;ME)";
    LocalFree(sidText);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return nullptr;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    // CreateMutexW requests MUTEX_ALL_ACCESS when the object already exists.
    // Request only the rights required by the wait/release protocol so the
    // restricted same-user ACE is sufficient across integrity levels.
    // 生命周期门禁需要和用户已安装、正在运行的实例并行验证待测二进制。
    // 该显式内部参数为本进程生成独立 mutex；正常启动仍严格保持单实例。
    std::wstring mutexName = MUTEX_NAME;
    if (hasCommandLineFlag(L"--lifecycle-test-instance")) {
        mutexName = L"Local\\Tools3000_LifecycleTest_" + std::to_wstring(GetCurrentProcessId());
    }
    HANDLE mutex = CreateMutexExW(
        &security, mutexName.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    SetLastError(error);
    return mutex;
}

std::wstring buildRestartParameters(bool includeWindowPos) {
    std::wstring params = L"--restart-pid=" + std::to_wstring(GetCurrentProcessId());
    if (hasCommandLineFlag(L"--silent")) {
        params += L" --silent";
    }
    if (!includeWindowPos) return params;

    auto& settingsWnd = tools3000::ui::SettingsWindow::instance();
    if (settingsWnd.isVisible() && settingsWnd.hwnd() && IsWindow(settingsWnd.hwnd())) {
        RECT rc{};
        if (GetWindowRect(settingsWnd.hwnd(), &rc)) {
            params += L" --window-pos=" + std::to_wstring(rc.left) + L"," +
                      std::to_wstring(rc.top) + L"," +
                      std::to_wstring(rc.right - rc.left) + L"," +
                      std::to_wstring(rc.bottom - rc.top);
        }
    }
    return params;
}

bool launchElevatedSuccessor(bool includeWindowPos) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    const std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    const std::wstring dirStr = dir.wstring();
    const std::wstring params = buildRestartParameters(includeWindowPos);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params.c_str();
    sei.lpDirectory = dirStr.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

bool launchSameIntegritySuccessor(bool includeWindowPos) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    const std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    const std::wstring dirStr = dir.wstring();

    std::wstring commandLine = L"\"";
    commandLine += exePath;
    commandLine += L"\" ";
    commandLine += buildRestartParameters(includeWindowPos);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, dirStr.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

bool launchUnelevatedSuccessor(bool includeWindowPos) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    const std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    const std::wstring dirStr = dir.wstring();

    // IPC normally arrives on the WebView UI apartment, but the bridge also
    // supports worker-thread callers. Make the launcher correct in both cases.
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        SetLastError(HRESULT_CODE(comResult));
        return false;
    }

    IShellDispatch2* shell = nullptr;
    const HRESULT createResult = CoCreateInstance(
        CLSID_Shell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
    if (FAILED(createResult) || !shell) {
        if (uninitializeCom) CoUninitialize();
        SetLastError(HRESULT_CODE(createResult));
        return false;
    }

    VARIANT arguments{};
    VARIANT directory{};
    VARIANT operation{};
    VARIANT show{};
    VariantInit(&arguments);
    VariantInit(&directory);
    VariantInit(&operation);
    VariantInit(&show);
    arguments.vt = VT_BSTR;
    arguments.bstrVal = SysAllocString(buildRestartParameters(includeWindowPos).c_str());
    directory.vt = VT_BSTR;
    directory.bstrVal = SysAllocString(dirStr.c_str());
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    const BSTR executable = SysAllocString(exePath);
    const HRESULT launchResult = executable && arguments.bstrVal && directory.bstrVal
        ? shell->ShellExecute(executable, arguments, directory, operation, show)
        : E_OUTOFMEMORY;
    if (executable) SysFreeString(executable);
    VariantClear(&arguments);
    VariantClear(&directory);
    shell->Release();
    if (uninitializeCom) CoUninitialize();
    if (FAILED(launchResult)) SetLastError(HRESULT_CODE(launchResult));
    return SUCCEEDED(launchResult);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// WinMain — 程序入口
// ─────────────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // 提升进程优先级为 HIGH_PRIORITY_CLASS，确保在高负荷系统下鼠标手势与全局热键调度不被饿死
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // 快捷自启动注册/注销独立命令处理（专供安装包或运维脚本原子调用）
    if (hasCommandLineFlag(L"--register-autostart")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        tools3000::core::ConfigManager::instance().initialize(tools3000::core::WinUtils::getConfigDirectory());
        bool ok = tools3000::core::autostart::registerTaskCOM(exePath);
        tools3000::core::ConfigManager::instance().set("/general/autoStart", ok);
        tools3000::core::ConfigManager::instance().shutdown();
        tools3000::core::WinUtils::syncApplicationShortcuts();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (hasCommandLineFlag(L"--unregister-autostart")) {
        tools3000::core::ConfigManager::instance().initialize(tools3000::core::WinUtils::getConfigDirectory());
        bool ok = tools3000::core::autostart::unregisterTaskCOM();
        tools3000::core::ConfigManager::instance().set("/general/autoStart", false);
        tools3000::core::ConfigManager::instance().shutdown();
        return ok ? 0 : 1;
    }
    if (hasCommandLineFlag(L"--update-shortcuts")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        tools3000::core::WinUtils::syncApplicationShortcuts();
        CoUninitialize();
        return 0;
    }

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_wmTaskbarCreated != 0) {
        ChangeWindowMessageFilter(g_wmTaskbarCreated, MSGFLT_ADD);
    }
    ChangeWindowMessageFilter(tools3000::tray::TrayIcon::WM_TRAYICON, MSGFLT_ADD);
    ChangeWindowMessageFilter(WM_TOOLS3000_SHOW_SETTINGS, MSGFLT_ADD);
    const UINT msgShowSettingsEarly = RegisterWindowMessageW(L"Tools3000_ShowSettings_Broadcast");
    if (msgShowSettingsEarly != 0) {
        ChangeWindowMessageFilter(msgShowSettingsEarly, MSGFLT_ADD);
    }
    const auto startupBeganAt = std::chrono::steady_clock::now();

    // ── 0b. 高分屏 (DPI) 感知 ─────────────────────────────────────────────
    tools3000::core::WinUtils::enableHighDpiSupport();

    // ── 1. 单实例检测 ────────────────────────────────────────────────────
    if (!checkSingleInstance()) {
        return 0;
    }

    // ── 2. COM 初始化与 Job Object 生命周期绑定 ──────────────────────────
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"COM 初始化失败", L"Tools3000 错误", MB_OK | MB_ICONERROR);
        releaseSingleInstanceMutex();
        return 1;
    }

    // 显式声明全局 Application User Model ID (AUMID)，确保 Windows 10/11 任务栏与快捷方式精准绑定
    SetCurrentProcessExplicitAppUserModelID(L"Yy1.Tools3000");

    // 同步桌面与开始菜单快捷方式 AUMID 属性，消除旧版快捷方式与图标缓存断层
    tools3000::core::WinUtils::syncApplicationShortcuts();

    // 自动检测并平滑迁移历史老自启动任务计划与注册表 (\Tools3000\ -> \Tools3000\)
    wchar_t runningExePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, runningExePath, MAX_PATH);
    tools3000::core::autostart::migrateLegacyTasksCOM(runningExePath);

    // 绑定当前进程树至 Job Object (带 KILL_ON_JOB_CLOSE 死亡绑定)
    tools3000::core::WinUtils::getProcessJobObject();

    // ── 3. 崩溃处理器 ───────────────────────────────────────────────────
    auto dumpDir = tools3000::core::WinUtils::getAppDataDirectory() / L"crashdumps";
    tools3000::core::CrashHandler::install(dumpDir);

    // ── 4. 预先探测语言设置以使首行日志即刻生效 ──────────────────────────
    std::string startupLanguage = "auto";
    const auto cfgFile = tools3000::core::WinUtils::getConfigDirectory() / L"config.json";
    std::error_code ecCfg;
    if (std::filesystem::exists(cfgFile, ecCfg)) {
        try {
            std::ifstream cf(cfgFile);
            if (cf.is_open()) {
                nlohmann::json root = nlohmann::json::parse(cf, nullptr, false);
                if (!root.is_discarded() && root.contains("general") && root["general"].contains("language")) {
                    startupLanguage = root["general"]["language"].get<std::string>();
                }
            }
        } catch (const std::exception& ex) {
            OutputDebugStringA((std::string("[Tools3000 Startup] Failed to parse startup language: ") + ex.what() + "\n").c_str());
        } catch (...) {
            OutputDebugStringA("[Tools3000 Startup] Unknown exception occurred while parsing startup language\n");
        }
    }
    tools3000::core::Logger::setLanguage(startupLanguage);

    // ── 4b. 日志系统初始化 ──────────────────────────────────────────────
    tools3000::core::LoggerConfig logConfig;
    logConfig.logDir = tools3000::core::WinUtils::getLogDirectory();
    logConfig.enableConsole = false;
    tools3000::core::Logger::initialize(logConfig);

    tools3000::core::TraceId::Scope mainScope;
    LOG_INFO("========================================");
    LOG_INFO("Tools3000 v{} 启动", tools3000::version::String);
    LOG_INFO("Tools3000 process identity: pid={}, elevated={}",
             GetCurrentProcessId(), tools3000::core::WinUtils::isCurrentProcessElevated());
    LOG_INFO("========================================");

    // ── 5a. 性能监控启动 ─────────────────────────────────────────────
    tools3000::core::PerformanceMonitor::instance().start();

    // ── 5. 配置管理器 ───────────────────────────────────────────────────
    if (!tools3000::core::ConfigManager::instance().initialize(
            tools3000::core::WinUtils::getConfigDirectory())) {
        LOG_ERROR("配置管理器初始化失败，应用无法安全启动");
        tools3000::core::PerformanceMonitor::instance().stop();
        tools3000::core::Logger::shutdown();
        releaseSingleInstanceMutex();
        CoUninitialize();
        return 1;
    }

    // 同步用户语言设置与日志保留天数到日志系统 (0 锁动态切换)
    tools3000::core::Logger::setLanguage(
        tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto")
    );
    tools3000::core::Logger::setRetentionDays(
        tools3000::core::ConfigManager::instance().get<uint32_t>("/general/logRetentionDays", 7)
    );

    // ── 5b. 检测并应用安装器生成的初始模块开关 (initial_modules.json) ──
    const auto initialModulesPath = tools3000::core::WinUtils::getExeDirectory() / L"initial_modules.json";
    std::error_code ecInit;
    if (std::filesystem::exists(initialModulesPath, ecInit)) {
        try {
            std::ifstream initFile(initialModulesPath);
            if (initFile.is_open()) {
                nlohmann::json patch = nlohmann::json::parse(initFile, nullptr, false);
                initFile.close();
                if (!patch.is_discarded() && patch.is_object()) {
                    tools3000::core::ConfigManager::instance().mergePatch(patch);
                    LOG_INFO("成功从安装包初始配置同步模块开关并写入当前用户配置");
                }
            }
            std::filesystem::remove(initialModulesPath, ecInit);
        } catch (const std::exception& e) {
            LOG_ERROR("解析或应用 initial_modules.json 失败: {}", e.what());
        }
    }

    const bool elevateSuppressed = hasCommandLineFlag(L"--no-elevate") ||
        commandLinePathValue(L"--performance-baseline-output=").has_value();
    if (tools3000::core::shouldAutoElevateOnStartup(
            tools3000::core::ConfigManager::instance().get<bool>("/general/runAsAdmin", true),
            tools3000::core::WinUtils::isCurrentProcessElevated(),
            elevateSuppressed)) {
        if (launchElevatedSuccessor(false)) {
            LOG_INFO("已按设置拉起管理员实例，当前进程退出");
            tools3000::core::ConfigManager::instance().shutdown();
            tools3000::core::PerformanceMonitor::instance().stop();
            tools3000::core::Logger::shutdown();
            releaseSingleInstanceMutex();
            CoUninitialize();
            return 0;
        }
        LOG_WARN("自动提权被取消或失败，以普通权限继续运行");
    }

    // ── 6. 创建隐藏消息窗口 ─────────────────────────────────────────────
    HWND hwndMessage = createMessageWindow(hInstance);
    if (!hwndMessage) {
        LOG_ERROR("无法创建消息窗口");
        tools3000::core::ConfigManager::instance().shutdown();
        tools3000::core::PerformanceMonitor::instance().stop();
        tools3000::core::Logger::shutdown();
        releaseSingleInstanceMutex();
        CoUninitialize();
        return 1;
    }
    tools3000::core::MainThreadDispatcher::instance().initialize(hwndMessage);
    WTSRegisterSessionNotification(hwndMessage, NOTIFY_FOR_THIS_SESSION);
    g_powerNotify = RegisterSuspendResumeNotification(hwndMessage, DEVICE_NOTIFY_WINDOW_HANDLE);

    // ── 7. 初始化其他子系统 ──────────────────────────────────────────────
    const bool silentStart = hasCommandLineFlag(L"--silent");
    initializeSubsystems(hwndMessage, false);
    tools3000::core::PerformanceMonitor::instance().recordLatency(
        "startup.core",
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startupBeganAt).count());

    // Dedicated benchmark commands never synthesize input into an existing
    // user session. Cold-start exits immediately after normal initialization;
    // search-first-open owns only this freshly launched process and lets the
    // actual SearchWindow show path publish its own hostShow metric first.
    if (const auto output = commandLinePathValue(L"--performance-baseline-output=")) {
        const auto scenario = commandLineStringValue(L"--performance-baseline-scenario=");
        if (!scenario) {
            writePerformanceBaselineSnapshot(*output);
            PostQuitMessage(0);
        } else if (_wcsicmp(scenario->c_str(), L"search-first-open") == 0) {
            g_performanceScenarioOutput = *output;
            tools3000::ui::SearchWindow::instance().show(hInstance);
            // SearchWindow::show records search.hostShow synchronously. Keep
            // the surface visible for one short message-pump turn so its real
            // HWND/WebView creation work is not skipped, then cleanly hide and
            // exit the benchmark-owned host.
            SetTimer(hwndMessage, TIMER_ID_PERFORMANCE_BASELINE, 250, nullptr);
        } else {
            LOG_ERROR("Unknown performance baseline scenario: {}",
                      tools3000::core::WinUtils::wstringToUtf8(*scenario));
            PostQuitMessage(1);
        }
    }

    // 用户主动启动时直接呈现设置；开机自启动使用 --silent 静默驻留托盘。
    if (!silentStart) {
        showSettingsWindow();
        // 仅用户主动启动时反馈成功；登录自启动的 --silent 必须真正安静。
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"Tools3000 已启动"});
    }

    LOG_INFO("程序启动完成，进入消息循环");

    // ── 8. 消息循环 ──────────────────────────────────────────────────────
    MSG msg{};
    int messageResult = 0;
    while ((messageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (messageResult == -1) LOG_ERROR("消息循环失败, error={}", GetLastError());

    // ── 9. 清理与退出 ────────────────────────────────────────────────────
    LOG_INFO("收到退出消息，准备清理");
    shutdownSubsystems();
    releaseSingleInstanceMutex();
    CoUninitialize();
    return messageResult == -1 ? 1 : static_cast<int>(msg.wParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// 内部实现
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct ParsedCommandLine {
    std::vector<std::wstring> args;

    static const ParsedCommandLine& instance() {
        static ParsedCommandLine s_instance = [] {
            ParsedCommandLine res;
            int argc = 0;
            if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
                res.args.reserve(argc);
                for (int i = 0; i < argc; ++i) {
                    res.args.emplace_back(argv[i]);
                }
                LocalFree(argv);
            }
            return res;
        }();
        return s_instance;
    }
};

} // namespace

bool parseWindowPos(int& x, int& y, int& w, int& h) {
    const auto& args = ParsedCommandLine::instance().args;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].rfind(L"--window-pos=", 0) == 0) {
            std::wstring val = args[i].substr(13);
            size_t p1 = val.find(L',');
            size_t p2 = (p1 != std::wstring::npos) ? val.find(L',', p1 + 1) : std::wstring::npos;
            size_t p3 = (p2 != std::wstring::npos) ? val.find(L',', p2 + 1) : std::wstring::npos;
            if (p1 != std::wstring::npos && p2 != std::wstring::npos && p3 != std::wstring::npos) {
                try {
                    x = std::stoi(val.substr(0, p1));
                    y = std::stoi(val.substr(p1 + 1, p2 - p1 - 1));
                    w = std::stoi(val.substr(p2 + 1, p3 - p2 - 1));
                    h = std::stoi(val.substr(p3 + 1));
                    return true;
                } catch (const std::exception& error) {
                    LOG_WARN("忽略无效的 --window-pos 参数: {}", error.what());
                } catch (...) {
                    LOG_WARN("解析 --window-pos 参数时发生未知异常");
                }
            }
            break;
        }
    }
    return false;
}

bool checkSingleInstance() {
    DWORD restartPid = 0;
    const auto& args = ParsedCommandLine::instance().args;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].rfind(L"--restart-pid=", 0) == 0) {
            restartPid = static_cast<DWORD>(_wtoi(args[i].c_str() + 14));
        }
    }

    g_singleInstanceMutex = createSingleInstanceMutex();
    if (g_singleInstanceMutex) {
        // The mutex, rather than an arbitrary PID timeout, is the authoritative
        // hand-off barrier. The old process keeps ownership until every tray,
        // hook, plugin and WebView resource has been shut down. A successor may
        // wait, but it can never overlap and publish a second tray icon.
        constexpr DWORD RestartHandoffTimeoutMs = 30'000;
        const DWORD waitResult = WaitForSingleObject(
            g_singleInstanceMutex, restartPid > 0 ? RestartHandoffTimeoutMs : 0);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            return true;
        }
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        if (restartPid > 0) {
            // Fail closed. Starting anyway would recreate the exact dual-icon,
            // duplicate-hook and hotkey-conflict failure this barrier prevents.
            return false;
        }
    }

    // 允许现有后台实例将窗口置顶于前台
    AllowSetForegroundWindow(ASFW_ANY);

    // 依然被占用时，通知已有实例唤醒并弹出设置窗口
    HWND existing = FindWindowW(WINDOW_CLASS_NAME, nullptr);
    if (existing) {
        DWORD targetPid = 0;
        GetWindowThreadProcessId(existing, &targetPid);
        if (targetPid > 0) {
            AllowSetForegroundWindow(targetPid);
        }
        PostMessageW(existing, WM_TOOLS3000_SHOW_SETTINGS, 0, 0);
    } else {
        HWND settingsWnd = FindWindowW(L"Tools3000_SettingsWindow", nullptr);
        if (settingsWnd) {
            DWORD targetPid = 0;
            GetWindowThreadProcessId(settingsWnd, &targetPid);
            if (targetPid > 0) {
                AllowSetForegroundWindow(targetPid);
            }
            ShowWindow(settingsWnd, IsIconic(settingsWnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(settingsWnd);
        }
        UINT msgShow = RegisterWindowMessageW(L"Tools3000_ShowSettings_Broadcast");
        PostMessageW(HWND_BROADCAST, msgShow, 0, 0);
    }
    return false;
}

bool hasCommandLineFlag(std::wstring_view flag) {
    const auto& args = ParsedCommandLine::instance().args;
    const std::wstring target(flag);
    for (size_t i = 1; i < args.size(); ++i) {
        if (_wcsicmp(args[i].c_str(), target.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::filesystem::path> commandLinePathValue(std::wstring_view prefix) {
    const auto& args = ParsedCommandLine::instance().args;
    for (size_t index = 1; index < args.size(); ++index) {
        std::wstring_view argument(args[index]);
        if (argument.size() >= prefix.size() &&
            _wcsnicmp(argument.data(), prefix.data(), static_cast<int>(prefix.size())) == 0) {
            const auto rawValue = argument.substr(prefix.size());
            if (!rawValue.empty()) return std::filesystem::path(rawValue);
            break;
        }
    }
    return std::nullopt;
}

std::optional<std::wstring> commandLineStringValue(std::wstring_view prefix) {
    const auto& args = ParsedCommandLine::instance().args;
    for (size_t index = 1; index < args.size(); ++index) {
        std::wstring_view argument(args[index]);
        if (argument.size() >= prefix.size() &&
            _wcsnicmp(argument.data(), prefix.data(), static_cast<int>(prefix.size())) == 0) {
            const auto rawValue = argument.substr(prefix.size());
            if (!rawValue.empty()) return std::wstring(rawValue);
            break;
        }
    }
    return std::nullopt;
}

HWND createMessageWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style       = CS_DBLCLKS;
    wcex.lpfnWndProc = MessageWindowProc;
    wcex.hInstance   = hInstance;
    wcex.lpszClassName = WINDOW_CLASS_NAME;
    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );

    if (hwnd) {
        // UIPI (User Interface Privilege Isolation) 防御加固：
        // 当 Tools3000 以管理员权限运行时，普通权限 Explorer 发送的托盘消息和任务栏重建广播默认会被内核阻断。
        // 通过 ChangeWindowMessageFilterEx 显式向 Explorer 放行关键生命周期消息。
        ChangeWindowMessageFilterEx(hwnd, tools3000::tray::TrayIcon::WM_TRAYICON, MSGFLT_ALLOW, nullptr);
        if (g_wmTaskbarCreated != 0) {
            ChangeWindowMessageFilterEx(hwnd, g_wmTaskbarCreated, MSGFLT_ALLOW, nullptr);
        }
        ChangeWindowMessageFilterEx(hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, WM_TOOLS3000_SHOW_SETTINGS, MSGFLT_ALLOW, nullptr);
        const UINT msgShowBroadcast = RegisterWindowMessageW(L"Tools3000_ShowSettings_Broadcast");
        if (msgShowBroadcast != 0) {
            ChangeWindowMessageFilterEx(hwnd, msgShowBroadcast, MSGFLT_ALLOW, nullptr);
        }
    }

    return hwnd;
}

void initializeSubsystems(HWND hwnd, bool preloadSettings) {
    // 1. 托盘图标
    auto& tray = tools3000::tray::TrayIcon::instance();
    tray.create(hwnd);

    tools3000::core::EventBus::instance().subscribe<tools3000::core::ThemeChangedEvent>([](const tools3000::core::ThemeChangedEvent&) {
        tools3000::tray::TrayIcon::instance().refreshThemeIcon();
    });

    tray.onOpenSettings([]() { showSettingsWindow(); });
    tray.onExit([hwnd]() { PostMessageW(hwnd, WM_CLOSE, 0, 0); });

    tray.onScreenshot([]() { tools3000::core::EventBus::instance().publish(tools3000::core::ActionTriggerScreenshotEvent{}); });
    tray.onRecording([]() { tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleRecordingEvent{}); });
    tray.onSearch([]() {
        auto& searchWnd = tools3000::ui::SearchWindow::instance();
        if (searchWnd.isVisible()) searchWnd.hide();
        else searchWnd.show(GetModuleHandleW(nullptr));
    });
    tray.onPauseGesture([]() { tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleGesturePauseEvent{}); });
    tray.onRestartElevated([hwnd]() {
        tools3000::core::ConfigManager::instance().set<bool>("/general/runAsAdmin", true);
        if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) return;
        if (launchElevatedSuccessor(true)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
            g_restartLaunchStarted.store(false, std::memory_order_release);
        }
    });

    // 2. 统计模块
    tools3000::core::StatsManager::instance().initialize();

    // 3. 发现插件。这里只读取元数据与启停配置；实际初始化要等核心服务就绪。
    auto& pm = tools3000::core::PluginManager::instance();
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::filesystem::path pluginDir =
        std::filesystem::path(exePath).parent_path() / L"plugins";
    pm.loadPlugins(pluginDir.string());

    // 4. 全局快捷键注册
    tools3000::core::HotkeyManager::instance().initialize(hwnd);
    if (pm.isEnabledAtLaunch("search")) {
        const tools3000::core::HotkeyDef searchFallback{tools3000::core::ModKey::Alt, VK_SPACE};
        const auto searchText = tools3000::core::ConfigManager::instance().get<std::string>(
            "/hotkeys/Toggle Search", searchFallback.toString());
        const auto searchHotkey = searchText.empty() ? tools3000::core::HotkeyDef{}
            : tools3000::core::HotkeyDef::fromString(searchText).value_or(searchFallback);
        auto toggleSearchSafe = []() {
            auto& searchWnd = tools3000::ui::SearchWindow::instance();
            if (searchWnd.isVisible()) {
                searchWnd.hide();
                return;
            }
            if (tools3000::core::ConfigManager::instance().get<bool>("/search/autoBypassFullscreen", true)) {
                HWND fg = GetForegroundWindow();
                if (fg && tools3000::core::WinUtils::isWindowFullscreen(fg)) {
                    const std::wstring classWide = tools3000::core::WinUtils::getWindowClassName(fg);
                    if (tools3000::gesture::shouldAutoBypassFullscreenGestures(true, tools3000::gesture::isProductivityToolkitClassName(classWide))) {
                        LOG_INFO("前台处于全屏独占应用，自动免打扰跳过搜索窗口呼出: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
                        return;
                    }
                }
            }
            searchWnd.show(GetModuleHandleW(nullptr));
        };

        tools3000::core::HotkeyManager::instance().registerHotkey("Toggle Search", searchHotkey, [toggleSearchSafe]() {
            toggleSearchSafe();
        });

        tools3000::core::MessageBridge::instance().registerHandler(
            "search.toggle", [toggleSearchSafe](const nlohmann::json&) -> nlohmann::json {
                toggleSearchSafe();
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHandler(
            "search.hide", [](const nlohmann::json&) -> nlohmann::json {
                tools3000::ui::SearchWindow::instance().hide();
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHandler(
            "search.getWindowSize", [](const nlohmann::json&) -> nlohmann::json {
                auto [w, h] = tools3000::ui::SearchWindow::instance().getWindowSize();
                return {{"width", w}, {"height", h}};
            });
        tools3000::core::MessageBridge::instance().registerHandler(
            "search.setWindowSize", [](const nlohmann::json& params) -> nlohmann::json {
                if (params.contains("width") && params.contains("height")) {
                    int w = params["width"].get<int>();
                    int h = params["height"].get<int>();
                    bool center = params.value("center", false);
                    tools3000::core::MainThreadDispatcher::instance().post([w, h, center]() {
                        tools3000::ui::SearchWindow::instance().setWindowSize(w, h, center);
                    });
                    return {{"success", true}, {"width", w}, {"height", h}};
                }
                return {{"success", false}, {"error", "missing width or height"}};
            });
    }

    // 注册 Alt+X 划词极速翻译快捷键
    const tools3000::core::HotkeyDef translateFallback{tools3000::core::ModKey::Alt, 'X'};
    const auto translateText = tools3000::core::ConfigManager::instance().get<std::string>(
        "/hotkeys/Translate Selection", translateFallback.toString());
    const auto translateHotkey = translateText.empty() ? tools3000::core::HotkeyDef{}
        : tools3000::core::HotkeyDef::fromString(translateText).value_or(translateFallback);
    tools3000::core::HotkeyManager::instance().registerHotkey("Translate Selection", translateHotkey, []() {
        std::string selected = tools3000::core::WinUtils::captureSelectedText();
        // 移除首尾空白
        selected.erase(0, selected.find_first_not_of(" \t\n\r"));
        selected.erase(selected.find_last_not_of(" \t\n\r") + 1);

        if (selected.empty()) {
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"请先划选文字或复制文本后再按 Alt+X"});
            return;
        }

        std::string encoded;
        for (unsigned char c : selected) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                encoded += buf;
            }
        }
        std::string url = "https://translate.google.com/?sl=auto&tl=zh-CN&op=translate&text=" + encoded;
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{L"已触发划词翻译并打开翻译页面"});
    });

    // 注册核心基础 IPC 处理器
    tools3000::core::MessageBridge::instance().registerBuiltinHandlers();

    // 注册 QuickLook 快速预览 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHandler("quicklook.open", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = params.value("path", "");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        if (!pathUtf8.empty()) {
            ShellExecuteW(nullptr, L"open", tools3000::core::WinUtils::utf8ToWstring(pathUtf8).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            tools3000::ui::QuickLookWindow::instance().hide();
        }
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHandler("quicklook.showInFolder", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = params.value("path", "");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        if (!pathUtf8.empty()) {
            std::wstring cmd = L"/select,\"" + tools3000::core::WinUtils::utf8ToWstring(pathUtf8) + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHandler("quicklook.copyPath", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = params.value("path", "");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        tools3000::core::WinUtils::copyToClipboard(pathUtf8);
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHandler("quicklook.hide", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::QuickLookWindow::instance().hide();
        return {{"success", true}};
    });

    // 注册打开诊断日志目录 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHandler("app.openLogDir", [](const nlohmann::json&) -> nlohmann::json {
        std::filesystem::path logDir = tools3000::core::WinUtils::getLogDirectory();
        std::error_code ec;
        if (!std::filesystem::exists(logDir, ec)) {
            std::filesystem::create_directories(logDir, ec);
        }
        ShellExecuteW(nullptr, L"open", logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return {{"success", true}, {"path", tools3000::core::WinUtils::wstringToUtf8(logDir.wstring())}};
    });

    // 注册导出诊断日志 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHandler("app.exportLogs", [](const nlohmann::json& params) -> nlohmann::json {
        std::optional<std::filesystem::path> savePath;
        const auto supplied = params.value("path", "");
        if (!supplied.empty()) {
            savePath = tools3000::core::WinUtils::utf8ToWstring(supplied);
        } else {
            Microsoft::WRL::ComPtr<IFileDialog> dialog;
            HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
            if (SUCCEEDED(hr) && dialog) {
                static const COMDLG_FILTERSPEC filters[] = {
                    {L"日志文件 (*.log;*.txt)", L"*.log;*.txt"},
                    {L"所有文件 (*.*)", L"*.*"},
                };
                dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
                dialog->SetDefaultExtension(L"log");
                
                auto now = std::chrono::system_clock::now();
                auto in_time_t = std::chrono::system_clock::to_time_t(now);
                std::tm tmBuffer{};
                localtime_s(&tmBuffer, &in_time_t);
                std::wstring verW = tools3000::core::WinUtils::utf8ToWstring(tools3000::version::String);
                wchar_t defaultName[128];
                swprintf_s(defaultName, L"Tools3000_Logs_v%ls_%04d%02d%02d_%02d%02d%02d.log",
                           verW.c_str(),
                           tmBuffer.tm_year + 1900, tmBuffer.tm_mon + 1, tmBuffer.tm_mday,
                           tmBuffer.tm_hour, tmBuffer.tm_min, tmBuffer.tm_sec);

                dialog->SetFileName(defaultName);
                dialog->SetTitle(L"导出 Tools3000 诊断日志");
                
                if (SUCCEEDED(dialog->Show(nullptr))) {
                    Microsoft::WRL::ComPtr<IShellItem> item;
                    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                        PWSTR rawPath = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                            savePath = std::filesystem::path(rawPath);
                            CoTaskMemFree(rawPath);
                        }
                    }
                }
            }
        }

        if (!savePath) {
            return {{"success", false}, {"cancelled", true}};
        }

        // 刷写日志缓存
        if (tools3000::core::Logger::instance()) {
            tools3000::core::Logger::instance()->flush();
        }

        std::filesystem::path target = *savePath;
        std::ofstream out(target, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return {{"success", false}, {"error", "无法创建目标导出文件"}};
        }

        // 写入环境诊断头
        out << "=======================================================\n";
        out << " Tools3000 诊断日志导出报告\n";
        out << " 软件版本: " << tools3000::version::String << "\n";
        out << " 进程 PID: " << GetCurrentProcessId() << "\n";
        out << "=======================================================\n\n";

        // 读取所有当前存在的日志文件并合并
        std::filesystem::path logDir = tools3000::core::WinUtils::getLogDirectory();
        std::error_code ec;
        if (std::filesystem::exists(logDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(logDir, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".log") {
                    out << ">>> File: " << entry.path().filename().string() << " <<<\n";
                    std::ifstream in(entry.path(), std::ios::in | std::ios::binary);
                    if (in.is_open()) {
                        out << in.rdbuf() << "\n\n";
                    }
                }
            }
        }
        out.close();

        // 自动在资源管理器中高亮选中导出的日志
        std::wstring targetStr = target.wstring();
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + targetStr + L"\"").c_str(), nullptr, SW_SHOWNORMAL);

        return {{"success", true}, {"path", tools3000::core::WinUtils::wstringToUtf8(targetStr)}};
    });

    // 注册应用一键热重启 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHandler("app.restart", [hwnd](const nlohmann::json&) -> nlohmann::json {
        if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) {
            return {{"success", true}, {"alreadyRestarting", true}};
        }
        if (!launchSameIntegritySuccessor(true)) {
            const DWORD error = GetLastError();
            g_restartLaunchStarted.store(false, std::memory_order_release);
            return {{"success", false}, {"error", "failed to start successor"}, {"win32Error", error}};
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("app.restartElevated", [hwnd](const nlohmann::json&) -> nlohmann::json {
        if (tools3000::core::WinUtils::isCurrentProcessElevated()) {
            return {{"success", true}, {"alreadyElevated", true}};
        }
        if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) {
            return {{"success", true}, {"alreadyRestarting", true}};
        }
        if (!launchElevatedSuccessor(true)) {
            const DWORD err = GetLastError();
            g_restartLaunchStarted.store(false, std::memory_order_release);
            return {
                {"success", false},
                {"cancelled", err == ERROR_CANCELLED},
                {"error", err == ERROR_CANCELLED ? "uac cancelled" : "failed to start elevated process"}
            };
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("app.restartDemoted", [hwnd](const nlohmann::json&) -> nlohmann::json {
        if (!tools3000::core::WinUtils::isCurrentProcessElevated()) {
            return {{"success", true}, {"alreadyDemoted", true}};
        }
        if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) {
            return {{"success", true}, {"alreadyRestarting", true}};
        }
        if (!launchUnelevatedSuccessor(true)) {
            const DWORD error = GetLastError();
            g_restartLaunchStarted.store(false, std::memory_order_release);
            return {{"success", false}, {"error", "failed to start unelevated successor"},
                    {"win32Error", error}};
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return {{"success", true}};
    });

    // ── 沉浸式标题栏窗口控制 (Seamless Titlebar Window Controls) ────────
    tools3000::core::MessageBridge::instance().registerHandler("window.minimize", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().minimize();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.toggleMaximize", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().toggleMaximize();
        return {{"success", true}, {"isMaximized", tools3000::ui::SettingsWindow::instance().isMaximized()}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.close", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().close();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.isMaximized", [](const nlohmann::json&) -> nlohmann::json {
        return {{"isMaximized", tools3000::ui::SettingsWindow::instance().isMaximized()}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.dragMove", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().dragMove();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.startResize", [](const nlohmann::json& params) -> nlohmann::json {
        std::string edge = params.value("edge", "");
        tools3000::ui::SettingsWindow::instance().startResize(edge);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("window.showSystemMenu", [](const nlohmann::json& params) -> nlohmann::json {
        int screenX = params.value("screenX", -1);
        int screenY = params.value("screenY", -1);
        tools3000::ui::SettingsWindow::instance().showSystemMenu(screenX, screenY);
        return {{"success", true}};
    });

    // 注册托盘菜单 IPC 处理函数
    tools3000::core::MessageBridge::instance().registerHandler("tray.action", [hwnd](const nlohmann::json& params) -> nlohmann::json {
        std::string action = params.value("action", "");
        
        // 执行前先隐藏托盘菜单
        tools3000::ui::TrayWindow::instance().hide();

        if (action == "openSettings") {
            showSettingsWindow();
        } else if (action == "search") {
            auto& searchWnd = tools3000::ui::SearchWindow::instance();
            if (searchWnd.isVisible()) searchWnd.hide();
            else searchWnd.show(GetModuleHandleW(nullptr));
        } else if (action == "screenshot") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionTriggerScreenshotEvent{});
        } else if (action == "recording") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleRecordingEvent{});
        } else if (action == "pauseGesture") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleGesturePauseEvent{});
        } else if (action == "emergencyFlush") {
            tools3000::core::RemoteMasterEngine::instance().flushModifiers();
            return {{"success", true}};
        } else if (action == "restartElevated") {
            tools3000::core::ConfigManager::instance().set<bool>("/general/runAsAdmin", true);
            if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) {
                return {{"success", true}, {"alreadyRestarting", true}};
            }
            if (launchElevatedSuccessor(true)) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else {
                const DWORD error = GetLastError();
                g_restartLaunchStarted.store(false, std::memory_order_release);
                return {{"success", false}, {"error", "failed to start elevated successor"},
                        {"win32Error", error}};
            }
        } else if (action == "restartDemoted") {
            tools3000::core::ConfigManager::instance().set<bool>("/general/runAsAdmin", false);
            if (g_restartLaunchStarted.exchange(true, std::memory_order_acq_rel)) {
                return {{"success", true}, {"alreadyRestarting", true}};
            }
            if (!launchUnelevatedSuccessor(true)) {
                const DWORD error = GetLastError();
                g_restartLaunchStarted.store(false, std::memory_order_release);
                return {{"success", false}, {"error", "failed to start unelevated successor"},
                        {"win32Error", error}};
            }
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (action == "exit") {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
            return {{"success", false}, {"error", "unknown tray action"}};
        }
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("tray.resize", [](const nlohmann::json& params) -> nlohmann::json {
        int width = params.value("width", 0);
        int height = params.value("height", 0);
        if (width > 0 && height > 0) {
            tools3000::ui::TrayWindow::instance().setContentSize(width, height);
            return {{"success", true}};
        }
        return {{"success", false}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("tray.hide", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::TrayWindow::instance().hide();
        return {{"success", true}};
    });

    // 5. 鼠标演示与特效 (Spotlight) Overlay 初始化与 IPC 注册
    tools3000::ui::SpotlightOverlay::instance().initialize(GetModuleHandleW(nullptr));
    tools3000::core::KeyboardHook::instance().setKeyboardActivityCallback([](DWORD vk, WPARAM wp) {
        tools3000::ui::SpotlightOverlay::instance().onKeyboardEvent(vk, wp);
    });

    tools3000::core::EventBus::instance().subscribe<tools3000::core::SpotlightStateChangedEvent>(
        [](const tools3000::core::SpotlightStateChangedEvent& e) {
            auto s = tools3000::ui::SpotlightOverlay::instance().getSettings();
            s.enabled = e.enabled;
            tools3000::ui::SpotlightOverlay::instance().updateSettings(s);
            if (!e.enabled) {
                tools3000::ui::SpotlightOverlay::instance().dismiss();
            }
        }
    );

    tools3000::core::MessageBridge::instance().registerHandler("spotlight.getSettings", [](const nlohmann::json&) -> nlohmann::json {
        auto s = tools3000::ui::SpotlightOverlay::instance().getSettings();
        return {
            {"enabled", s.enabled},
            {"triggerDoubleCtrl", s.triggerDoubleCtrl},
            {"triggerShakeMouse", s.triggerShakeMouse},
            {"autoBypassFullscreen", s.autoBypassFullscreen},
            {"spotlightColor", s.spotlightColor},
            {"spotlightSize", s.spotlightSize},
            {"animationDurationMs", s.animationDurationMs},
            {"holdDurationMs", s.holdDurationMs},
            {"shakeThreshold", s.shakeThreshold},
            {"spotlightAnimStyle", s.spotlightAnimStyle},
            {"clickRippleEnabled", s.clickRippleEnabled},
            {"clickRippleStyle", s.clickRippleStyle},
            {"mouseTrailEnabled", s.mouseTrailEnabled},
            {"mouseTrailStyle", s.mouseTrailStyle},
            {"mouseTrailColorMode", s.mouseTrailColorMode},
            {"leftClickColor", s.leftClickColor},
            {"rightClickColor", s.rightClickColor},
            {"middleClickColor", s.middleClickColor}
        };
    });

    tools3000::core::MessageBridge::instance().registerHandler("spotlight.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
        auto s = tools3000::ui::SpotlightOverlay::instance().getSettings();
        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            s.enabled = params["enabled"].get<bool>();
            tools3000::core::ConfigManager::instance().set<bool>("/spotlight/enabled", s.enabled);
            tools3000::core::ConfigManager::instance().set<bool>("/plugins/spotlight/enabled", s.enabled);
            tools3000::core::MessageBridge::instance().pushEvent("spotlight:stateChanged", {{"enabled", s.enabled}});
            tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "spotlight"}, {"enabled", s.enabled}});
        }
        if (params.contains("triggerDoubleCtrl") && params["triggerDoubleCtrl"].is_boolean()) s.triggerDoubleCtrl = params["triggerDoubleCtrl"].get<bool>();
        if (params.contains("triggerShakeMouse") && params["triggerShakeMouse"].is_boolean()) s.triggerShakeMouse = params["triggerShakeMouse"].get<bool>();
        if (params.contains("autoBypassFullscreen") && params["autoBypassFullscreen"].is_boolean()) s.autoBypassFullscreen = params["autoBypassFullscreen"].get<bool>();
        if (params.contains("spotlightColor") && params["spotlightColor"].is_string()) s.spotlightColor = params["spotlightColor"].get<std::string>();
        if (params.contains("spotlightSize") && params["spotlightSize"].is_number()) s.spotlightSize = params["spotlightSize"].get<int>();
        if (params.contains("animationDurationMs") && params["animationDurationMs"].is_number()) s.animationDurationMs = params["animationDurationMs"].get<int>();
        if (params.contains("holdDurationMs") && params["holdDurationMs"].is_number()) s.holdDurationMs = params["holdDurationMs"].get<int>();
        if (params.contains("shakeThreshold") && params["shakeThreshold"].is_number()) s.shakeThreshold = params["shakeThreshold"].get<int>();
        if (params.contains("spotlightAnimStyle") && params["spotlightAnimStyle"].is_string()) s.spotlightAnimStyle = params["spotlightAnimStyle"].get<std::string>();

        if (params.contains("clickRippleEnabled") && params["clickRippleEnabled"].is_boolean()) s.clickRippleEnabled = params["clickRippleEnabled"].get<bool>();
        if (params.contains("clickRippleStyle") && params["clickRippleStyle"].is_string()) s.clickRippleStyle = params["clickRippleStyle"].get<std::string>();
        if (params.contains("mouseTrailEnabled") && params["mouseTrailEnabled"].is_boolean()) s.mouseTrailEnabled = params["mouseTrailEnabled"].get<bool>();
        if (params.contains("mouseTrailStyle") && params["mouseTrailStyle"].is_string()) s.mouseTrailStyle = params["mouseTrailStyle"].get<std::string>();
        if (params.contains("mouseTrailColorMode") && params["mouseTrailColorMode"].is_string()) s.mouseTrailColorMode = params["mouseTrailColorMode"].get<std::string>();
        if (params.contains("leftClickColor") && params["leftClickColor"].is_string()) s.leftClickColor = params["leftClickColor"].get<std::string>();
        if (params.contains("rightClickColor") && params["rightClickColor"].is_string()) s.rightClickColor = params["rightClickColor"].get<std::string>();
        if (params.contains("middleClickColor") && params["middleClickColor"].is_string()) s.middleClickColor = params["middleClickColor"].get<std::string>();

        tools3000::ui::SpotlightOverlay::instance().updateSettings(s);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("spotlight.trigger", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SpotlightOverlay::instance().trigger();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("spotlight.dismiss", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SpotlightOverlay::instance().dismiss();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("spotlight.resetDefaults", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SpotlightOverlay::instance().resetDefaults();
        return {{"success", true}};
    });

    // 5.5. 远程协助主控单边增强引擎 (沉浸热键直通、修饰键急救冲刷、输入法智能脱敏)
    try {
        tools3000::core::RemoteMasterEngine::instance().initialize();
    } catch (const std::exception& e) {
        LOG_ERROR("[RemoteMaster] Failed to initialize: {}", e.what());
    } catch (...) {
        LOG_ERROR("[RemoteMaster] Failed to initialize: unknown exception");
    }
    tools3000::core::KeyboardHook::instance().setLowLevelKeyInterceptor([](const KBDLLHOOKSTRUCT& data, WPARAM wp) -> bool {
        return tools3000::core::RemoteMasterEngine::instance().onLowLevelKeyboardEvent(data, wp);
    });

    tools3000::core::MessageBridge::instance().registerHandler("remote.getSettings", [](const nlohmann::json&) -> nlohmann::json {
        return tools3000::core::RemoteMasterEngine::instance().getSettings().toJson();
    });

    tools3000::core::MessageBridge::instance().registerHandler("remote.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
        auto s = tools3000::core::RemoteMasterEngine::instance().getSettings();
        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            s.enabled = params["enabled"].get<bool>();
            tools3000::core::ConfigManager::instance().set<bool>("/remote_boost/enabled", s.enabled);
            tools3000::core::ConfigManager::instance().set<bool>("/plugins/remote_boost/enabled", s.enabled);
            tools3000::core::ConfigManager::instance().set<bool>("/plugins/remote/enabled", s.enabled);
            tools3000::core::MessageBridge::instance().pushEvent("remote:stateChanged", {{"enabled", s.enabled}});
            tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "remote_boost"}, {"enabled", s.enabled}});
        }
        if (params.contains("hotkeyTunnelEnabled") && params["hotkeyTunnelEnabled"].is_boolean()) s.hotkeyTunnelEnabled = params["hotkeyTunnelEnabled"].get<bool>();
        if (params.contains("emergencyFlushEnabled") && params["emergencyFlushEnabled"].is_boolean()) s.emergencyFlushEnabled = params["emergencyFlushEnabled"].get<bool>();
        if (params.contains("doubleRightCtrlTrigger") && params["doubleRightCtrlTrigger"].is_boolean()) s.doubleRightCtrlTrigger = params["doubleRightCtrlTrigger"].get<bool>();
        if (params.contains("imeSanitizerEnabled") && params["imeSanitizerEnabled"].is_boolean()) s.imeSanitizerEnabled = params["imeSanitizerEnabled"].get<bool>();
        if (params.contains("emergencyShortcut") && params["emergencyShortcut"].is_string()) s.emergencyShortcut = params["emergencyShortcut"].get<std::string>();
        if (params.contains("targetProcesses") && params["targetProcesses"].is_array()) {
            s.targetProcesses.clear();
            for (const auto& item : params["targetProcesses"]) {
                if (item.is_string()) s.targetProcesses.push_back(item.get<std::string>());
            }
        }
        if (params.contains("targetClasses") && params["targetClasses"].is_array()) {
            s.targetClasses.clear();
            for (const auto& item : params["targetClasses"]) {
                if (item.is_string()) s.targetClasses.push_back(item.get<std::string>());
            }
        }
        tools3000::core::RemoteMasterEngine::instance().updateSettings(s);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("remote.emergencyFlush", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::RemoteMasterEngine::instance().flushModifiers();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("remote.resetDefaults", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::RemoteMasterEngine::instance().resetDefaults();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHandler("remote.getState", [](const nlohmann::json&) -> nlohmann::json {
        return {
            {"isRemoteForeground", tools3000::core::RemoteMasterEngine::instance().isRemoteForeground()},
            {"imeSanitized", tools3000::core::RemoteMasterEngine::instance().isImeSanitized()},
            {"activeProcess", tools3000::core::RemoteMasterEngine::instance().getActiveRemoteProcess()}
        };
    });

    // 6. 键盘与鼠标核心输入总线
    tools3000::core::KeyboardHook::instance().install();
    tools3000::core::MouseHook::instance().install();
    tools3000::core::KeyboardHook::instance().setKeyInterceptor([hwnd](DWORD vkCode, WPARAM wParam) -> bool {
        if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) return false;

        // 1. 空格键 (Space): 在资源管理器或桌面触发 QuickLook 文件预览
        if (vkCode == VK_SPACE) {
            bool hasMods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            if (!hasMods) {
                if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                    tools3000::core::MainThreadDispatcher::instance().post([]() {
                        tools3000::ui::QuickLookWindow::instance().hide();
                    });
                    return true;
                } else {
                    auto sel = tools3000::core::WinUtils::getSelectedExplorerFile();
                    if (sel.has_value()) {
                        std::wstring path = *sel;
                        tools3000::core::MainThreadDispatcher::instance().post([path]() {
                            tools3000::ui::QuickLookWindow::instance().show(path);
                        });
                        return true;
                    }
                }
            }
        }

        // 2. ESC：先收 QuickLook；再通知手势/轮盘取消追踪，但不吞掉按键
        if (vkCode == VK_ESCAPE) {
            if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    tools3000::ui::QuickLookWindow::instance().hide();
                });
                return true;
            }
            tools3000::core::EventBus::instance().publish(tools3000::core::CancelTransientUiEvent{});
        }

        // 3. 方向键联动：在资源管理器中切换选中项时，QuickLook 自动刷新预览内容
        if (vkCode == VK_UP || vkCode == VK_DOWN || vkCode == VK_LEFT || vkCode == VK_RIGHT) {
            if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                // 同一 HWND/ID 的 SetTimer 会重置截止时间，形成无阻塞防抖。
                SetTimer(hwnd, TIMER_ID_QUICKLOOK_REFRESH, 35, nullptr);
            }
        }

        return false;
    });

    // 6. 初始化本次启动获准加载的插件
    pm.initializePlugins();

    // 7. UI Overlay 初始化
    tools3000::ui::ToastOverlay::instance().initialize(GetModuleHandleW(nullptr));

    tools3000::core::EventBus::instance().subscribe<tools3000::core::ShowToastEvent>(
        [](const tools3000::core::ShowToastEvent& ev) {
            const auto message = ev.message;
            tools3000::core::MainThreadDispatcher::instance().post([message]() {
                tools3000::ui::ToastOverlay::instance().showToast(
                    tools3000::core::WinUtils::wstringToUtf8(message));
            });
        }
    );

    // 8. 托盘菜单是常驻入口：启动后立即在隐藏窗口中完成 WebView2、前端
    // chunk 与状态数据预热。这样第一次左/右键打开时直接显示完整菜单，
    // 不把 React Suspense 的“正在加载”暴露给用户。
    tools3000::ui::TrayWindow::instance().preload(GetModuleHandleW(nullptr));

    // 用户主动启动时预热设置页；开机静默驻留不额外预热完整设置页。
    if (preloadSettings) preloadSettingsWindow(GetModuleHandleW(nullptr));

    // 全局搜索窗口与托盘均为核心高频入口：启动后立即在隐藏窗口中预热 WebView2，确保 0ms 秒开
    tools3000::ui::SearchWindow::instance().preload(GetModuleHandleW(nullptr));
    // Debug/console startup still follows the on-demand service contract.
    // Only an explicit request to open Search may count as user activation.
    if (hasCommandLineFlag(L"--show-search")) {
        tools3000::ui::SearchWindow::instance().show(GetModuleHandleW(nullptr));
    }

    // 9. 更新检查严格在后台执行，并由内部频率限制保护启动性能。
    tools3000::core::UpdateChecker::instance().checkAsync(false);
}

void shutdownSubsystems() {
    // Withdraw the user-visible entry point before any potentially blocking
    // plugin teardown. Even if a third-party worker is slow, Explorer cannot
    // retain a clickable-looking icon for an instance already shutting down.
    tools3000::tray::TrayIcon::instance().destroy();
    tools3000::core::UpdateChecker::instance().shutdown();
    // 先关闭所有 WebView 入口，阻止新的 IPC 请求进入插件，再等待并卸载插件。
    tools3000::ui::SettingsWindow::instance().destroy();
    tools3000::ui::SearchWindow::instance().destroy();
    tools3000::ui::QuickLookWindow::instance().destroy();
    tools3000::ui::TrayWindow::instance().destroy();
    tools3000::core::PluginManager::instance().shutdownPlugins();
    tools3000::core::ShellContextMenuService::instance().shutdown();
    tools3000::ui::ToastOverlay::instance().shutdown();
    tools3000::ui::SpotlightOverlay::instance().shutdown();
    tools3000::ui::WebViewEnvironmentManager::instance().shutdown();
    tools3000::core::RemoteMasterEngine::instance().shutdown();
    tools3000::core::KeyboardHook::instance().uninstall();
    tools3000::core::MouseHook::instance().uninstall();
    tools3000::core::WinUtils::emergencyFlushInputState();
    tools3000::core::StatsManager::instance().shutdown();
    tools3000::core::PerformanceMonitor::instance().stop();
    tools3000::core::MainThreadDispatcher::instance().shutdown();
    tools3000::core::ConfigManager::instance().shutdown();
    tools3000::core::Logger::shutdown();
}

void showSettingsWindow() {
    tools3000::tray::TrayIcon::instance().ensureCreated();
    auto& settingsWnd = tools3000::ui::SettingsWindow::instance();
    tools3000::ui::SettingsWindowConfig config;
    int wx = -1, wy = -1, ww = -1, wh = -1;
    if (parseWindowPos(wx, wy, ww, wh) && ww > 0 && wh > 0) {
        config.posX = wx;
        config.posY = wy;
        config.width = ww;
        config.height = wh;
        config.hasCustomPlacement = true;
        config.startCentered = false;
    }
    config.devServerUrl = "http://localhost:5173";
    settingsWnd.setConfig(config);
    settingsWnd.show(GetModuleHandleW(nullptr));
}

void preloadSettingsWindow(HINSTANCE hInstance) {
    auto& settingsWnd = tools3000::ui::SettingsWindow::instance();
    tools3000::ui::SettingsWindowConfig config;
    int wx = -1, wy = -1, ww = -1, wh = -1;
    if (parseWindowPos(wx, wy, ww, wh) && ww > 0 && wh > 0) {
        config.posX = wx;
        config.posY = wy;
        config.width = ww;
        config.height = wh;
        config.hasCustomPlacement = true;
        config.startCentered = false;
    }
    config.devServerUrl = "http://localhost:5173";
    settingsWnd.setConfig(config);
    settingsWnd.preload(hInstance);
}

LRESULT CALLBACK MessageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == tools3000::core::MainThreadDispatcher::MessageId) {
        tools3000::core::MainThreadDispatcher::instance().drain();
        return 0;
    }
    if (msg == (WM_USER + 100)) {
        tools3000::tray::TrayIcon::instance().handleMessage(wParam, lParam);
        return 0;
    }
    if (msg == WM_COMMAND) {
        EndMenu();
        tools3000::tray::TrayIcon::instance().fireCallback(static_cast<tools3000::tray::TrayMenuId>(LOWORD(wParam)));
        return 0;
    }

    if (msg == WM_TIMER) {
        if (wParam == TIMER_ID_PERFORMANCE_BASELINE) {
            KillTimer(hwnd, TIMER_ID_PERFORMANCE_BASELINE);
            tools3000::ui::SearchWindow::instance().hide();
            const bool wrote = g_performanceScenarioOutput &&
                writePerformanceBaselineSnapshot(*g_performanceScenarioOutput);
            g_performanceScenarioOutput.reset();
            PostQuitMessage(wrote ? 0 : 1);
            return 0;
        }
        if (wParam == TIMER_ID_QUICKLOOK_REFRESH) {
            KillTimer(hwnd, TIMER_ID_QUICKLOOK_REFRESH);
            auto selected = tools3000::core::WinUtils::getSelectedExplorerFile();
            auto& quickLook = tools3000::ui::QuickLookWindow::instance();
            if (selected.has_value() && quickLook.isVisible() &&
                *selected != quickLook.currentFilePath()) {
                quickLook.previewFile(*selected);
            }
            return 0;
        }
        if (wParam == tools3000::tray::TrayIcon::TIMER_ID_TRAY_RETRY) {
            tools3000::tray::TrayIcon::instance().ensureCreated(hwnd);
            return 0;
        }
    }

    if (msg == WM_HOTKEY) {
        tools3000::core::HotkeyManager::instance().handleHotkeyMessage(wParam);
        return 0;
    }

    static UINT msgShowBroadcast = RegisterWindowMessageW(L"Tools3000_ShowSettings_Broadcast");
    if (msg == msgShowBroadcast || msg == WM_TOOLS3000_SHOW_SETTINGS) {
        tools3000::tray::TrayIcon::instance().ensureCreated(hwnd);
        showSettingsWindow();
        return 0;
    }

    if (g_wmTaskbarCreated != 0 && msg == g_wmTaskbarCreated) {
        LOG_INFO("检测到系统任务栏重建 (TaskbarCreated)，重新注册托盘图标");
        tools3000::tray::TrayIcon::instance().recreate();
        return 0;
    }

    if (msg == WM_SETTINGCHANGE || msg == WM_THEMECHANGED) {
        tools3000::tray::TrayIcon::instance().refreshThemeIcon();
        return 0;
    }

    if (msg == WM_WTSSESSION_CHANGE) {
        tools3000::core::EventBus::instance().publish(tools3000::core::CancelTransientUiEvent{});
        tools3000::core::EventBus::instance().publish(tools3000::core::SystemSessionChangedEvent{static_cast<uint32_t>(wParam)});
        if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF) {
            LOG_INFO("检测到 Windows 系统会话锁屏或注销 (0x{:X})，主动挂起渲染并释放物理内存", wParam);
            tools3000::ui::SpotlightOverlay::instance().dismiss();
            tools3000::core::WinUtils::trimWorkingSet();
        } else if (wParam == WTS_SESSION_UNLOCK) {
            LOG_INFO("检测到 Windows 系统会话解锁，恢复正常工作状态");
        }
        return 0;
    }

    if (msg == WM_POWERBROADCAST) {
        tools3000::core::EventBus::instance().publish(tools3000::core::CancelTransientUiEvent{});
        tools3000::core::EventBus::instance().publish(tools3000::core::SystemPowerChangedEvent{static_cast<uint32_t>(wParam)});
        if (wParam == PBT_APMSUSPEND) {
            LOG_INFO("检测到系统进入休眠或睡眠 (0x{:X})，主动挂起渲染并释放物理内存", wParam);
            tools3000::ui::SpotlightOverlay::instance().dismiss();
            tools3000::core::WinUtils::trimWorkingSet();
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            LOG_INFO("检测到系统从休眠或睡眠中唤醒 (0x{:X})，防御性重置输入拦截状态", wParam);
        }
        return TRUE;
    }

    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        EndMenu();
        if (g_powerNotify) {
            UnregisterSuspendResumeNotification(g_powerNotify);
            g_powerNotify = nullptr;
        }
        WTSUnRegisterSessionNotification(hwnd);
        tools3000::core::WinUtils::emergencyFlushInputState();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
