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
#include "core/i18n/UiI18n.h"
#include "core/backup/ConfigBackupService.h"
#include <objbase.h>
#include <psapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <algorithm>
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
#include <zlib.h>

#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ThemeUtils.h"
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
#include "core/utils/FileLocksmithEngine.h"
#include "core/diagnostics/FeedbackDiagnosticService.h"
#include "core/utils/SessionIsolation.h"
#include "core/ipc/AutoStartPolicy.h"
#include "tray/TrayIcon.h"
#include "ui/SettingsWindow.h"
#include "ui/SearchWindow.h"
#include "ui/ClipboardWindow.h"
#include "ui/FileLocksmithWindow.h"
#include "ui/QuickLookWindow.h"
#include "clipboard/ClipboardManager.h"
#include "text_expander/TextExpanderManager.h"
#include "ui/QuickLookNavigation.h"
#include "ui/TrayWindow.h"
#include "ui/ToastOverlay.h"
#include "ui/SpotlightOverlay.h"
#include "ui/WebViewEnvironmentManager.h"
#include "core/remote/RemoteMasterEngine.h"
#include "core/utils/OutOfProcessFileDialog.h"

// ── 常量 ─────────────────────────────────────────────────────────────────────
static constexpr const wchar_t* WINDOW_CLASS_NAME = L"Tools3000_MessageWindow";
static constexpr const wchar_t* WINDOW_TITLE      = L"Tools3000MessageWindow";
static constexpr const wchar_t* MUTEX_NAME        = L"Local\\Tools3000_SingleInstance_Mutex";
static constexpr UINT WM_TOOLS3000_SHOW_SETTINGS    = WM_APP + 101;
static constexpr UINT WM_TOOLS3000_TEST_INJECT_KEY  = WM_APP + 190;
static constexpr UINT_PTR TIMER_ID_QUICKLOOK_REFRESH = 0x4551;
static constexpr UINT_PTR TIMER_ID_PERFORMANCE_BASELINE = 0x4552;
static constexpr UINT_PTR TIMER_ID_FULLSCREEN_GUARD = 0x4553;
static constexpr UINT_PTR TIMER_ID_EXIT_FOR_UPDATE = 0x4554;
static constexpr UINT_PTR TIMER_ID_QUIET_IDLE_CLEANUP = 0x4558;

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
static HWINEVENTHOOK g_fullscreenWinEventHook = nullptr;
static HWINEVENTHOOK g_fullscreenMoveSizeHook = nullptr;
static tools3000::core::SubscriptionId g_hostThemeSubId = 0;
static std::atomic<ULONGLONG> g_lastQuietIdleTrimTick{0};
static uint32_t g_fullscreenGuardTickCounter = 0;

namespace {

void syncForegroundFullscreenState(bool forceRefresh = false) {
    static HWND s_lastValidFg = nullptr;
    HWND fg = GetForegroundWindow();
    if (!fg && s_lastValidFg && IsWindow(s_lastValidFg) &&
        IsWindowVisible(s_lastValidFg) && !IsIconic(s_lastValidFg)) {
        fg = s_lastValidFg;
    } else if (fg) {
        s_lastValidFg = fg;
    } else {
        s_lastValidFg = nullptr;
    }

    // 零分配指纹短路守卫：周期兜底定时器触发时若前台窗口句柄、尺寸与样式均未变更，
    // 跳过 ConfigManager 锁、JSON 查表与外壳进程路径查询，彻底消除闲置态高频唤醒与软缺页。
    struct ForegroundFingerprint {
        HWND hwnd = nullptr;
        RECT rect{};
        LONG_PTR style = 0;
        LONG_PTR exStyle = 0;
        bool iconic = false;
        bool zoomed = false;
        bool valid = false;
    };
    static ForegroundFingerprint s_lastFingerprint{};
    ForegroundFingerprint cur{};
    cur.hwnd = fg;
    cur.valid = true;
    if (fg) {
        GetWindowRect(fg, &cur.rect);
        cur.style = GetWindowLongPtrW(fg, GWL_STYLE);
        cur.exStyle = GetWindowLongPtrW(fg, GWL_EXSTYLE);
        cur.iconic = IsIconic(fg) != FALSE;
        cur.zoomed = IsZoomed(fg) != FALSE;
    }
    if (!forceRefresh && s_lastFingerprint.valid &&
        cur.hwnd == s_lastFingerprint.hwnd &&
        cur.style == s_lastFingerprint.style &&
        cur.exStyle == s_lastFingerprint.exStyle &&
        cur.iconic == s_lastFingerprint.iconic &&
        cur.zoomed == s_lastFingerprint.zoomed &&
        EqualRect(&cur.rect, &s_lastFingerprint.rect)) {
        return;
    }
    s_lastFingerprint = cur;

    const bool gestureBypassEnabled =
        tools3000::core::ConfigManager::instance().get<bool>("/gesture/autoBypassFullscreen", true);
    const bool suspendHotkeys =
        fg && tools3000::core::WinUtils::shouldSuspendHotkeysForWindow(fg);
    const bool suspendGestures =
        gestureBypassEnabled && fg && tools3000::core::WinUtils::shouldSuspendGesturesForWindow(fg);

    auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
    if (hotkeyMgr.isFullscreenSuspended() != suspendHotkeys) {
        hotkeyMgr.setFullscreenSuspended(suspendHotkeys);
    }

    auto& mouseHook = tools3000::core::MouseHook::instance();
    if (mouseHook.isFullscreenSuspended() != suspendGestures) {
        mouseHook.setFullscreenSuspended(suspendGestures);
        if (suspendGestures) {
            tools3000::core::EventBus::instance().publish(tools3000::core::CancelTransientUiEvent{});
        }
    }

    tools3000::ui::SpotlightOverlay::instance().refreshFullscreenBypassState(fg);
}

std::string jsonStringOr(const nlohmann::json& obj, const char* key, std::string fallback = {}) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

int jsonIntOr(const nlohmann::json& obj, const char* key, int fallback = 0) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return fallback;
    return it->get<int>();
}

bool jsonBoolOr(const nlohmann::json& obj, const char* key, bool fallback = false) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

void releaseHostHotkeysAndMessageWindow(HWND& msgHwnd) {
    tools3000::core::LuaEngine::instance().shutdown();
    tools3000::core::HotkeyManager::instance().shutdown();
    if (msgHwnd && IsWindow(msgHwnd)) {
        DestroyWindow(msgHwnd);
    }
    msgHwnd = nullptr;
}

void CALLBACK FullscreenGuardWinEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND /*hwnd*/,
    LONG idObject,
    LONG /*idChild*/,
    DWORD /*dwEventThread*/,
    DWORD /*dwmsEventTime*/) {
    if (idObject != OBJID_WINDOW) {
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND &&
        event != EVENT_SYSTEM_MOVESIZEEND &&
        event != EVENT_SYSTEM_MINIMIZESTART &&
        event != EVENT_SYSTEM_MINIMIZEEND) {
        return;
    }
    if (tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible()) {
        return;
    }
    syncForegroundFullscreenState(true);
}

bool writePerformanceBaselineSnapshot(const std::filesystem::path& outputPath) {
    std::error_code error;
    const auto parent = outputPath.parent_path();
    const std::string outputUtf8 = tools3000::core::WinUtils::pathToUtf8(outputPath);
    if (parent.empty() || !std::filesystem::is_directory(parent, error) || error) {
        LOG_ERROR("Performance baseline output directory is unavailable: {}", outputUtf8);
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
            LOG_ERROR("Cannot create performance baseline snapshot: {}", outputUtf8);
            return false;
        }
        stream << snapshot.dump(2) << '\n';
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, error);
            LOG_ERROR("Cannot write performance baseline snapshot: {}", outputUtf8);
            return false;
        }
    }
    std::filesystem::rename(temporary, outputPath, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        LOG_ERROR("Cannot finalize performance baseline snapshot: {}", outputUtf8);
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
    // 会话级单实例互斥体（Local\）：同一桌面会话内保持严格单实例并支持 UAC 提权交接，
    // 在 Windows Server 多管理员并发登录（Console + 多个 RDP 会话）时允许各会话独立并行运行。
    const std::wstring mutexName = tools3000::core::SessionIsolation::buildSingleInstanceMutexName(
        hasCommandLineFlag(L"--lifecycle-test-instance"), GetCurrentProcessId());
    (void)MUTEX_NAME;
    HANDLE mutex = CreateMutexExW(
        &security, mutexName.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    SetLastError(error);
    return mutex;
}

std::wstring buildRestartParameters(bool includeWindowPos) {
    std::wstring params = L"--restart-pid=" + std::to_wstring(GetCurrentProcessId());
    if (hasCommandLineFlag(L"--silent") || hasCommandLineFlag(L"--autostart")) {
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
    const std::filesystem::path exePathObj = tools3000::core::WinUtils::getExePath();
    if (exePathObj.empty()) return false;
    const std::wstring exePath = exePathObj.wstring();
    const std::wstring dirStr = exePathObj.parent_path().wstring();
    const std::wstring params = buildRestartParameters(includeWindowPos);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.c_str();
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
    const std::filesystem::path exePathObj = tools3000::core::WinUtils::getExePath();
    if (exePathObj.empty()) return false;
    const std::wstring exePath = exePathObj.wstring();
    const std::wstring dirStr = exePathObj.parent_path().wstring();

    std::wstring commandLine = L"\"";
    commandLine += exePath;
    commandLine += L"\" ";
    commandLine += buildRestartParameters(includeWindowPos);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, dirStr.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

bool launchUnelevatedSuccessor(bool includeWindowPos) {
    const std::filesystem::path exePathObj = tools3000::core::WinUtils::getExePath();
    if (exePathObj.empty()) return false;
    const std::wstring exePath = exePathObj.wstring();
    const std::wstring dirStr = exePathObj.parent_path().wstring();

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
    const BSTR executable = SysAllocString(exePath.c_str());
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
    // 独立短命重型任务 Worker 子进程模式 (--worker=file-dialog|ocr|studio-export|record-encode|media-probe)
    // 跳过单实例互斥锁、全局钩子、托盘、WebView2 与主日志锁，由 OS 在退场后 100% 物理+虚拟双归零。
    if (const auto workerMode = commandLineStringValue(L"--worker="); workerMode.has_value() && !workerMode->empty()) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

        if (*workerMode == L"file-dialog") {
            // 系统 IFileOpenDialog / IFileSaveDialog 与第三方外壳扩展必须在 STA 套间执行
            const HRESULT staHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            int dlgExitCode = 1;
            if (const auto jobPath = commandLinePathValue(L"--job="); jobPath.has_value()) {
                dlgExitCode = tools3000::core::OutOfProcessFileDialog::runWorkerFromJobFile(*jobPath);
            }
            if (SUCCEEDED(staHr)) {
                CoUninitialize();
            }
            return dlgExitCode;
        }

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        int exitCode = 1;

        wchar_t exePathBuf[MAX_PATH * 4]{};
        const DWORD exeLen = GetModuleFileNameW(nullptr, exePathBuf, static_cast<DWORD>(std::size(exePathBuf)));
        if (exeLen > 0 && exeLen < std::size(exePathBuf)) {
            const std::filesystem::path exeDir = std::filesystem::path(exePathBuf).parent_path();
            const std::filesystem::path pluginDll = exeDir / L"plugins" / L"Plugin_Capture.dll";
            // 确保依赖 DLL（Tools3000Core.dll / OpenCV / FFmpeg 等）从 exeDir 准确解析
            SetDllDirectoryW(exeDir.c_str());
            if (HMODULE hPlugin = LoadLibraryW(pluginDll.c_str())) {
                using RunWorkerFn = int (*)(int, const wchar_t* const*);
                auto* runFn = reinterpret_cast<RunWorkerFn>(
                    GetProcAddress(hPlugin, "Tools3000RunCaptureWorker"));
                if (runFn) {
                    int argc = 0;
                    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
                    if (argv && argc > 0) {
                        std::vector<const wchar_t*> constArgv(static_cast<size_t>(argc));
                        for (int i = 0; i < argc; ++i) constArgv[static_cast<size_t>(i)] = argv[i];
                        exitCode = runFn(argc, constArgv.data());
                        LocalFree(argv);
                    }
                }
                FreeLibrary(hPlugin);
            }
        }
        if (SUCCEEDED(comHr)) {
            CoUninitialize();
        }
        return exitCode;
    }

    // 提升进程优先级为 HIGH_PRIORITY_CLASS，确保在高负荷系统下鼠标手势与全局热键调度不被饿死
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // 独立短命零常驻 File Locksmith 文件占用解锁弹窗 (--unlock "<path>" 或 --unlock=<path>)
    // 跳过单实例互斥锁、全局钩子、托盘与 WebView2，退出即由 OS 100% 归零全部物理与虚拟内存。
    {
        std::wstring unlockTarget;
        if (const auto inlineVal = commandLineStringValue(L"--unlock="); inlineVal.has_value() && !inlineVal->empty()) {
            unlockTarget = *inlineVal;
        } else {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv) {
                for (int i = 1; i + 1 < argc; ++i) {
                    if (_wcsicmp(argv[i], L"--unlock") == 0 && argv[i + 1] && argv[i + 1][0] != L'\0') {
                        unlockTarget = argv[i + 1];
                        break;
                    }
                }
                LocalFree(argv);
            }
        }
        if (!unlockTarget.empty()) {
            return tools3000::ui::runFileLocksmithWindow(hInstance, unlockTarget);
        }
    }

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
        tools3000::core::WinUtils::syncApplicationShortcuts(true);
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
        const bool sysZh = tools3000::core::WinUtils::isSystemLanguageChinese();
        MessageBoxW(
            nullptr,
            sysZh ? L"COM 初始化失败" : L"Failed to initialize COM subsystem.",
            sysZh ? L"Tools3000 错误" : L"Tools3000 Error",
            MB_OK | MB_ICONERROR);
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
    tools3000::core::UiI18n::setConfiguredLanguage(startupLanguage);
    tools3000::core::Logger::setLanguage(startupLanguage);

    // ── 4b. 日志系统初始化 ──────────────────────────────────────────────
    tools3000::core::LoggerConfig logConfig;
    logConfig.logDir = tools3000::core::WinUtils::getLogDirectory();
    logConfig.enableConsole = false;
    tools3000::core::Logger::initialize(logConfig);

    tools3000::core::TraceId::Scope mainScope;
    LOG_INFO("========================================");
    LOG_INFO("Tools3000 v{} 启动", tools3000::version::String);
    LOG_INFO("Tools3000 process identity: pid={}, sessionId={}, elevated={}",
             GetCurrentProcessId(),
             tools3000::core::SessionIsolation::currentSessionId(),
             tools3000::core::WinUtils::isCurrentProcessElevated());
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

    // 同步 Win32 原生弹出菜单 (CreatePopupMenu / uxtheme) 明暗主题模式，消除深色下惨白右键菜单
    tools3000::core::WinUtils::applyNativeMenuTheme();

    // 同步用户语言设置与日志保留天数到原生多语言目录与日志系统 (0 锁动态切换)
    const std::string configuredLanguage =
        tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    tools3000::core::UiI18n::setConfiguredLanguage(configuredLanguage);
    tools3000::core::Logger::setLanguage(configuredLanguage);
    tools3000::core::Logger::setRetentionDays(
        tools3000::core::ConfigManager::instance().get<uint32_t>("/general/logRetentionDays", 7)
    );
    {
        const std::string configuredLogLevel =
            tools3000::core::ConfigManager::instance().get<std::string>("/general/logLevel", "info");
        if (configuredLogLevel == "trace") {
            tools3000::core::Logger::setLevel(spdlog::level::trace);
        } else if (configuredLogLevel == "debug") {
            tools3000::core::Logger::setLevel(spdlog::level::debug);
        } else if (configuredLogLevel == "warn") {
            tools3000::core::Logger::setLevel(spdlog::level::warn);
        } else if (configuredLogLevel == "error") {
            tools3000::core::Logger::setLevel(spdlog::level::err);
        } else {
            tools3000::core::Logger::setLevel(spdlog::level::info);
        }
    }

    // ── 5b. 检测并应用安装器生成的初始模块开关 (initial_modules.json) ──
    const auto initialModulesPath = tools3000::core::WinUtils::getExeDirectory() / L"initial_modules.json";
    std::error_code ecInit;
    if (std::filesystem::exists(initialModulesPath, ecInit)) {
        try {
            std::ifstream initFile(initialModulesPath, std::ios::binary);
            if (initFile.is_open()) {
                const std::string rawContent(
                    (std::istreambuf_iterator<char>(initFile)),
                    std::istreambuf_iterator<char>());
                initFile.close();

                std::error_code ecMeta;
                const auto writeTicks = std::filesystem::last_write_time(initialModulesPath, ecMeta)
                                            .time_since_epoch()
                                            .count();
                const uLong contentCrc = crc32(
                    crc32(0L, Z_NULL, 0),
                    reinterpret_cast<const Bytef*>(rawContent.data()),
                    static_cast<uInt>(rawContent.size()));
                const std::string stamp = std::to_string(writeTicks) + ":" +
                                          std::to_string(rawContent.size()) + ":" +
                                          std::to_string(contentCrc);
                const std::string appliedStamp =
                    tools3000::core::ConfigManager::instance().get<std::string>(
                        "/general/appliedInitialModulesStamp", "");

                if (stamp != appliedStamp) {
                    nlohmann::json patch = nlohmann::json::parse(rawContent, nullptr, false);
                    if (!patch.is_discarded() && patch.is_object()) {
                        const bool picasaEnabledByInstaller =
                            (patch.contains("plugins") && patch["plugins"].is_object() &&
                             patch["plugins"].contains("picasa_viewer") &&
                             patch["plugins"]["picasa_viewer"].is_object() &&
                             patch["plugins"]["picasa_viewer"].value("enabled", false)) ||
                            (patch.contains("picasa_viewer") && patch["picasa_viewer"].is_object() &&
                             patch["picasa_viewer"].value("enabled", false));
                        patch["general"]["appliedInitialModulesStamp"] = stamp;
                        tools3000::core::ConfigManager::instance().mergePatch(patch);
                        tools3000::core::ConfigManager::instance().flush();
                        LOG_INFO("成功从安装包初始配置同步模块开关并写入当前用户配置");

                        if (picasaEnabledByInstaller) {
                            // 若安装器已通过 runasoriginaluser 完成当前用户关联注册，则跳过二次重复调用
                            HKEY hCheckKey = nullptr;
                            const bool alreadyRegistered =
                                (RegOpenKeyExW(
                                     HKEY_CURRENT_USER,
                                     L"Software\\Tools3000\\PicasaViewer\\Capabilities\\FileAssociations",
                                     0, KEY_READ, &hCheckKey) == ERROR_SUCCESS);
                            if (hCheckKey) {
                                RegCloseKey(hCheckKey);
                            }
                            if (!alreadyRegistered) {
                                const auto viewerExe =
                                    tools3000::core::WinUtils::getExeDirectory() / L"Tools3000_Picasa_Viewer.exe";
                                const DWORD attrs = GetFileAttributesW(viewerExe.c_str());
                                const bool safeBinary =
                                    attrs != INVALID_FILE_ATTRIBUTES &&
                                    (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
                                if (safeBinary) {
                                    std::wstring cmdLine = L"\"" + viewerExe.wstring() + L"\" --register-silent";
                                    STARTUPINFOW si{};
                                    si.cb = sizeof(si);
                                    PROCESS_INFORMATION pi{};
                                    if (CreateProcessW(
                                            viewerExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW, nullptr, viewerExe.parent_path().c_str(), &si, &pi)) {
                                        CloseHandle(pi.hThread);
                                        CloseHandle(pi.hProcess);
                                        LOG_INFO("安装器初始配置已启用 Picasa 图片查看器，正在后台自动关联全部支持的图片格式");
                                    } else {
                                        LOG_WARN("安装器初始配置触发 Picasa 图片查看器自动关联失败: error={}",
                                                 GetLastError());
                                    }
                                }
                            }
                        }
                    }
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
    const bool silentStart = hasCommandLineFlag(L"--silent") || hasCommandLineFlag(L"--autostart");
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
            tools3000::core::ShowToastEvent{
                tools3000::core::trWCstr(L"Tools3000 已启动", L"Tools3000 started")});
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
    releaseHostHotkeysAndMessageWindow(hwndMessage);
    releaseSingleInstanceMutex();
    CoUninitialize();

    const int finalExitCode = (messageResult == -1) ? 1 : static_cast<int>(msg.wParam);
    // 防僵尸退场守护：已完成全部配置落盘、云端备份、钩子卸载与日志刷盘，若 CRT 静态析构或底层图形驱动在
    // ExitProcess 阶段偶发锁竞争超时 (>1200ms)，自动强制终结进程，杜绝后台残留单线程僵尸实例。
    std::thread([finalExitCode]() {
        Sleep(1200);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(finalExitCode));
    }).detach();
    return finalExitCode;
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
    g_hostThemeSubId = tools3000::core::EventBus::instance().subscribe<tools3000::core::ThemeChangedEvent>(
        [](const tools3000::core::ThemeChangedEvent& ev) {
            tools3000::core::WinUtils::applyNativeMenuTheme(ev.theme);
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::ClipboardWindow::instance().applyThemeChrome();
            });
        });

    // 1. 托盘图标（内部管理并安全退订 GesturePauseChangedEvent 与 ThemeChangedEvent）
    auto& tray = tools3000::tray::TrayIcon::instance();
    tray.create(hwnd);

    tray.onOpenSettings([]() { showSettingsWindow(); });
    tray.onExit([hwnd]() { PostMessageW(hwnd, WM_CLOSE, 0, 0); });

    tray.onScreenshot([]() { tools3000::core::EventBus::instance().publish(tools3000::core::ActionTriggerScreenshotEvent{}); });
    tray.onRecording([]() { tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleRecordingEvent{}); });
    tray.onSearch([]() {
        auto& searchWnd = tools3000::ui::SearchWindow::instance();
        if (searchWnd.isForegroundActive()) searchWnd.minimize();
        else searchWnd.show(GetModuleHandleW(nullptr));
    });
    tray.onClipboard([]() {
        tools3000::ui::ClipboardWindow::instance().show(GetModuleHandleW(nullptr));
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
    const std::filesystem::path pluginDir =
        tools3000::core::WinUtils::getExeDirectory() / L"plugins";
    pm.loadPlugins(pluginDir);

    // 4. 全局快捷键注册与全屏智能免打扰策略初始化
    tools3000::core::WinUtils::setFullscreenPolicyMode(
        tools3000::core::WinUtils::parseFullscreenPolicyMode(
            tools3000::core::ConfigManager::instance().get<std::string>(
                "/general/fullscreenMode", "smart")));
    tools3000::core::HotkeyManager::instance().initialize(hwnd);
    if (pm.isEnabledAtLaunch("search")) {
        const tools3000::core::HotkeyDef searchFallback{tools3000::core::ModKey::Alt, VK_SPACE};
        const auto searchText = tools3000::core::ConfigManager::instance().get<std::string>(
            "/hotkeys/Toggle Search", searchFallback.toString());
        const auto searchHotkey = searchText.empty() ? tools3000::core::HotkeyDef{}
            : tools3000::core::HotkeyDef::fromString(searchText).value_or(searchFallback);
        auto toggleSearchSafe = []() {
            auto& searchWnd = tools3000::ui::SearchWindow::instance();
            if (searchWnd.isForegroundActive()) {
                // 搜索窗口当前已在最前台获焦：再次按快捷键优雅最小化到任务栏，完整保留搜索结果与滚动状态
                searchWnd.minimize();
                return;
            }
            if (tools3000::core::ConfigManager::instance().get<bool>("/search/autoBypassFullscreen", true)) {
                HWND fg = GetForegroundWindow();
                if (fg && tools3000::core::WinUtils::shouldBypassOverlaysForWindow(fg)) {
                    LOG_INFO("前台处于全屏独占应用，自动免打扰跳过搜索窗口呼出: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
                    return;
                }
            }
            // 无论窗口处于隐藏、最小化在任务栏还是被其他应用遮挡，一律毫秒还原并置前聚焦搜索框
            searchWnd.show(GetModuleHandleW(nullptr));
        };

        tools3000::core::HotkeyManager::instance().registerHotkey("Toggle Search", searchHotkey, [toggleSearchSafe]() {
            toggleSearchSafe();
        });

        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.toggle", [toggleSearchSafe](const nlohmann::json&) -> nlohmann::json {
                toggleSearchSafe();
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.hide", [](const nlohmann::json&) -> nlohmann::json {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    tools3000::ui::SearchWindow::instance().hide();
                });
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.minimize", [](const nlohmann::json&) -> nlohmann::json {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    tools3000::ui::SearchWindow::instance().minimize();
                });
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.showSystemMenu", [](const nlohmann::json& params) -> nlohmann::json {
                const int x = (params.is_object() && params.contains("x") && params["x"].is_number_integer())
                    ? params["x"].get<int>()
                    : -1;
                const int y = (params.is_object() && params.contains("y") && params["y"].is_number_integer())
                    ? params["y"].get<int>()
                    : -1;
                tools3000::core::MainThreadDispatcher::instance().post([x, y]() {
                    tools3000::ui::SearchWindow::instance().showSystemMenu(x, y);
                });
                return {{"success", true}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.getWindowSize", [](const nlohmann::json&) -> nlohmann::json {
                auto [w, h] = tools3000::ui::SearchWindow::instance().getWindowSize();
                return {
                    {"width", w},
                    {"height", h},
                    {"maximized", tools3000::ui::SearchWindow::instance().isMaximized()}
                };
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.getMaximized", [](const nlohmann::json&) -> nlohmann::json {
                return {
                    {"success", true},
                    {"maximized", tools3000::ui::SearchWindow::instance().isMaximized()}
                };
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.setMaximized", [](const nlohmann::json& params) -> nlohmann::json {
                const bool maximized = jsonBoolOr(params, "maximized", false);
                tools3000::core::MainThreadDispatcher::instance().post([maximized]() {
                    tools3000::ui::SearchWindow::instance().setMaximized(maximized);
                });
                return {{"success", true}, {"maximized", maximized}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.toggleMaximized", [](const nlohmann::json&) -> nlohmann::json {
                const bool next = !tools3000::ui::SearchWindow::instance().isMaximized();
                tools3000::core::MainThreadDispatcher::instance().post([next]() {
                    tools3000::ui::SearchWindow::instance().setMaximized(next);
                });
                return {{"success", true}, {"maximized", next}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.setWindowSize", [](const nlohmann::json& params) -> nlohmann::json {
                if (params.is_object() &&
                    params.contains("width") && params["width"].is_number() &&
                    params.contains("height") && params["height"].is_number()) {
                    const int w = params["width"].get<int>();
                    const int h = params["height"].get<int>();
                    const bool center = jsonBoolOr(params, "center", false);
                    tools3000::core::MainThreadDispatcher::instance().post([w, h, center]() {
                        tools3000::ui::SearchWindow::instance().setWindowSize(w, h, center);
                    });
                    return {{"success", true}, {"width", w}, {"height", h}, {"maximized", false}};
                }
                return {{"success", false}, {"error", "missing width or height"}};
            });
        tools3000::core::MessageBridge::instance().registerHostHandler(
            "search.quickLook",
            [](const nlohmann::json& params) -> nlohmann::json {
                const std::string pathUtf8 = jsonStringOr(params, "path");
                if (pathUtf8.empty()) {
                    return {{"success", false}, {"error", "missing path"}};
                }
                const bool toggle = jsonBoolOr(params, "toggle", true);
                const std::wstring wpath = tools3000::ui::normalizePreviewFilePath(
                    tools3000::core::WinUtils::utf8ToWstring(pathUtf8));
                tools3000::core::MainThreadDispatcher::instance().post([wpath, toggle]() {
                    auto& ql = tools3000::ui::QuickLookWindow::instance();
                    const bool sameFile = _wcsicmp(ql.currentFilePath().c_str(), wpath.c_str()) == 0;
                    if (!toggle) {
                        if (ql.isVisible()) {
                            if (!sameFile) {
                                ql.previewFile(wpath);
                            }
                        } else {
                            ql.show(wpath);
                        }
                        return;
                    }
                    if (ql.isVisible() && sameFile) {
                        ql.hide();
                    } else {
                        ql.show(wpath);
                    }
                });
                return {{"success", true}};
            });
    }

    // 注册 Alt+X 划词极速翻译快捷键
    const tools3000::core::HotkeyDef translateFallback{tools3000::core::ModKey::Alt, 'X'};
    const auto translateText = tools3000::core::ConfigManager::instance().get<std::string>(
        "/hotkeys/Translate Selection", translateFallback.toString());
    const auto translateHotkey = translateText.empty() ? tools3000::core::HotkeyDef{}
        : tools3000::core::HotkeyDef::fromString(translateText).value_or(translateFallback);
    tools3000::core::HotkeyManager::instance().registerHotkey("Translate Selection", translateHotkey, []() {
        HWND fg = GetForegroundWindow();
        if (fg && tools3000::core::WinUtils::shouldSuspendHotkeysForWindow(fg)) {
            return;
        }
        std::string selected = tools3000::core::WinUtils::captureSelectedText();
        const auto firstNotSpace = selected.find_first_not_of(" \t\n\r");
        if (firstNotSpace == std::string::npos) {
            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                tools3000::core::trWCstr(L"请先划选文字或复制文本后再按 Alt+X", L"Select or copy text first before pressing Alt+X")});
            return;
        }
        const auto lastNotSpace = selected.find_last_not_of(" \t\n\r");
        selected = selected.substr(firstNotSpace, lastNotSpace - firstNotSpace + 1);

        std::wstring wideSelected = tools3000::core::WinUtils::utf8ToWstring(selected);
        constexpr size_t kMaxTranslateChars = 1200;
        if (wideSelected.size() > kMaxTranslateChars) {
            size_t cutPos = kMaxTranslateChars;
            if (cutPos > 0 && wideSelected[cutPos - 1] >= 0xD800 && wideSelected[cutPos - 1] <= 0xDBFF) {
                --cutPos;
            }
            wideSelected.resize(cutPos);
            selected = tools3000::core::WinUtils::wstringToUtf8(wideSelected);
        }

        size_t cjkCount = 0;
        size_t latinCount = 0;
        for (wchar_t ch : wideSelected) {
            if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF)) {
                ++cjkCount;
            } else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) {
                ++latinCount;
            }
        }
        const char* targetLang = (cjkCount > 0 && cjkCount * 3 >= latinCount) ? "en" : "zh-CN";

        std::string encoded;
        encoded.reserve(selected.size() * 3);
        for (unsigned char c : selected) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += static_cast<char>(c);
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", c);
                encoded += buf;
            }
        }
        std::string url = std::string("https://translate.google.com/?sl=auto&tl=") + targetLang + "&op=translate&text=" + encoded;
        tools3000::core::WinUtils::openUrl(tools3000::core::WinUtils::utf8ToWstring(url));
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstr(L"正在打开划词翻译", L"Opening selection translation")});
    });

    // 注册核心基础 IPC 处理器
    tools3000::core::MessageBridge::instance().registerBuiltinHandlers();

    // 注册 QuickLook 快速预览 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.open", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = jsonStringOr(params, "path");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        if (!pathUtf8.empty()) {
            tools3000::core::WinUtils::openFile(tools3000::core::WinUtils::utf8ToWstring(pathUtf8));
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::QuickLookWindow::instance().hide();
            });
        }
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.showInFolder", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = jsonStringOr(params, "path");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        if (!pathUtf8.empty()) {
            tools3000::core::WinUtils::openFolderAndSelectItem(tools3000::core::WinUtils::utf8ToWstring(pathUtf8));
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::QuickLookWindow::instance().hide();
            });
        }
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.copyPath", [](const nlohmann::json& params) -> nlohmann::json {
        std::string pathUtf8 = jsonStringOr(params, "path");
        if (pathUtf8.empty()) pathUtf8 = tools3000::core::WinUtils::wstringToUtf8(tools3000::ui::QuickLookWindow::instance().currentFilePath());
        tools3000::core::WinUtils::copyToClipboard(pathUtf8);
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.hide", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::MainThreadDispatcher::instance().post([]() {
            tools3000::ui::QuickLookWindow::instance().hide();
        });
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.ready", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::MainThreadDispatcher::instance().post([]() {
            tools3000::ui::QuickLookWindow::instance().notifyFrontendReady();
        });
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.fallbackMedia", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string pathUtf8 = jsonStringOr(params, "path");
        const std::string kind = jsonStringOr(params, "kind", "video");
        tools3000::core::MainThreadDispatcher::instance().post([pathUtf8, kind]() {
            tools3000::ui::QuickLookWindow::instance().requestFallbackMediaAsync(
                tools3000::core::WinUtils::utf8ToWstring(pathUtf8), kind);
        });
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.stepSelection", [](const nlohmann::json& params) -> nlohmann::json {
        const int delta = jsonIntOr(params, "delta", 1) < 0 ? -1 : 1;
        tools3000::core::MainThreadDispatcher::instance().post([delta]() {
            auto& searchWin = tools3000::ui::SearchWindow::instance();
            if (searchWin.isVisible()) {
                searchWin.stepQuickLookSelection(delta);
                return;
            }
            tools3000::ui::QuickLookWindow::instance().stepSiblingAsync(delta);
        });
        return {{"success", true}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("quicklook.startDrag", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::MainThreadDispatcher::instance().post([]() {
            HWND qlHwnd = tools3000::ui::QuickLookWindow::instance().getHwnd();
            if (qlHwnd && IsWindow(qlHwnd)) {
                ReleaseCapture();
                SendMessageW(qlHwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
        });
        return {{"success", true}};
    });

    // 注册打开诊断日志目录 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHostHandler("app.openLogDir", [](const nlohmann::json&) -> nlohmann::json {
        std::filesystem::path logDir = tools3000::core::WinUtils::getLogDirectory();
        std::error_code ec;
        if (!std::filesystem::exists(logDir, ec)) {
            std::filesystem::create_directories(logDir, ec);
        }
        ShellExecuteW(nullptr, L"open", logDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return {{"success", true}, {"path", tools3000::core::WinUtils::wstringToUtf8(logDir.wstring())}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("system.openFileLocksmith", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string pathUtf8 = jsonStringOr(params, "path");
        if (pathUtf8.empty()) {
            return {{"success", false}, {"error", "empty_path"}};
        }
        const bool ok = tools3000::core::FileLocksmithEngine::launchUnlockWindowProcess(
            tools3000::core::WinUtils::utf8ToWstring(pathUtf8), false);
        return {{"success", ok}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("system.getShellMenuConfig", [](const nlohmann::json&) -> nlohmann::json {
        bool shiftOnlyReg = false;
        const bool registered = tools3000::core::FileLocksmithEngine::isExplorerContextMenuRegistered(&shiftOnlyReg);
        auto& cfg = tools3000::core::ConfigManager::instance();
        return {
            {"enabled", registered},
            {"shiftOnly", registered ? shiftOnlyReg : cfg.get<bool>("/general/shellMenuShiftOnly", true)}
        };
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("system.updateShellMenuConfig", [](const nlohmann::json& params) -> nlohmann::json {
        const bool enabled = params.value("enabled", false);
        const bool shiftOnly = params.value("shiftOnly", true);
        auto& cfg = tools3000::core::ConfigManager::instance();
        cfg.set<bool>("/general/shellMenuLocksmithEnabled", enabled);
        cfg.set<bool>("/general/shellMenuShiftOnly", shiftOnly);
        cfg.set<bool>("/plugins/file_locksmith/enabled", enabled);
        const std::wstring verbLabel = tools3000::core::trW(
            L"使用 Tools3000 查看文件占用/解锁",
            L"Inspect File Locks with Tools3000");
        const bool ok = tools3000::core::FileLocksmithEngine::syncExplorerContextMenu(
            enabled, shiftOnly, tools3000::core::WinUtils::getExePath().wstring(), verbLabel);
        if (ok) {
            tools3000::core::MessageBridge::instance().pushEvent("fileLocksmith:stateChanged", {
                {"enabled", enabled},
                {"shiftOnly", shiftOnly}
            });
            tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {
                {"id", "file_locksmith"},
                {"enabled", enabled}
            });
        }
        return {{"success", ok}, {"enabled", enabled}, {"shiftOnly", shiftOnly}};
    });
    tools3000::core::MessageBridge::instance().registerAsyncHostHandler("system.browseFileLocksmithTarget", [](const nlohmann::json& params) -> nlohmann::json {
        const bool pickFolder = params.value("folder", false);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::FileDialogRequest req;
        req.mode = pickFolder ? tools3000::core::FileDialogMode::PickFolder
                              : tools3000::core::FileDialogMode::OpenFile;
        req.title = pickFolder
            ? tools3000::core::trWCstrFor(isZh, L"选择要检测占用的文件夹", L"Select Folder to Inspect Locks")
            : tools3000::core::trWCstrFor(isZh, L"选择要检测占用的文件", L"Select File to Inspect Locks");
        const auto dlgRes = tools3000::core::OutOfProcessFileDialog::show(req);
        if (!dlgRes.accepted || dlgRes.selectedPath.empty()) {
            return {{"success", false}, {"cancelled", true}};
        }
        return {{"success", true}, {"path", tools3000::core::WinUtils::pathToUtf8(dlgRes.selectedPath)}};
    });

    // 方案 C 双轨应用内问题反馈与 10 维隐私脱敏诊断通道（全异步执行，绝不阻塞 UI 消息泵）
    tools3000::core::MessageBridge::instance().markMethodAsync("feedback.preview");
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.preview", [](const nlohmann::json& params) -> nlohmann::json {
        tools3000::core::FeedbackSubmissionInput in;
        in.feedbackType = jsonStringOr(params, "feedbackType", "bug");
        in.title = jsonStringOr(params, "title");
        in.description = jsonStringOr(params, "description");
        in.stepsToReproduce = jsonStringOr(params, "stepsToReproduce");
        in.contact = jsonStringOr(params, "contact");
        in.includeLogs = params.value("includeLogs", true);
        in.screenshotPath = jsonStringOr(params, "screenshotPath");
        in.screenshotDataUri = jsonStringOr(params, "screenshotDataUri");
        const auto preview = tools3000::core::FeedbackDiagnosticService::buildDiagnosticPreview(
            in, tools3000::core::FeedbackDiagnosticService::kDefaultLogLines);
        return {
            {"ok", true},
            {"environment", preview.environment},
            {"redactedMainLog", preview.redactedMainLog},
            {"redactedServiceLog", preview.redactedServiceLog},
            {"redactedLatestCrash", preview.redactedLatestCrash},
            {"markdownReport", preview.markdownReport},
            {"githubIssueUrl", preview.githubIssueUrl},
            {"totalRedactions", preview.totalRedactions},
            {"relayConfigured", preview.relayConfigured}
        };
    });
    tools3000::core::MessageBridge::instance().markMethodAsync("feedback.readClipboardImage");
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.readClipboardImage", [](const nlohmann::json&) -> nlohmann::json {
        const auto att = tools3000::core::FeedbackDiagnosticService::readClipboardImageAsDataUri();
        return {
            {"ok", att.ok},
            {"tooLarge", att.tooLarge},
            {"dataUri", att.dataUri},
            {"fileName", att.fileName},
            {"byteSize", att.byteSize}
        };
    });
    tools3000::core::MessageBridge::instance().markMethodAsync("feedback.submit");
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.submit", [](const nlohmann::json& params) -> nlohmann::json {
        if (tools3000::core::Logger::instance()) {
            tools3000::core::Logger::instance()->flush();
        }
        tools3000::core::FeedbackSubmissionInput in;
        in.feedbackType = jsonStringOr(params, "feedbackType", "bug");
        in.title = jsonStringOr(params, "title");
        in.description = jsonStringOr(params, "description");
        in.stepsToReproduce = jsonStringOr(params, "stepsToReproduce");
        in.contact = jsonStringOr(params, "contact");
        in.includeLogs = params.value("includeLogs", true);
        in.screenshotPath = jsonStringOr(params, "screenshotPath");
        in.screenshotDataUri = jsonStringOr(params, "screenshotDataUri");
        const auto res = tools3000::core::FeedbackDiagnosticService::submitFeedback(in);
        return {
            {"ok", res.ok},
            {"mode", res.mode},
            {"trackingId", res.trackingId},
            {"issueUrl", res.issueUrl},
            {"exportedBundlePath", res.exportedBundlePath},
            {"exportedScreenshotPath", res.exportedScreenshotPath},
            {"copiedToClipboard", res.copiedToClipboard},
            {"markdownReport", res.markdownReport},
            {"errorMessage", res.errorMessage},
            {"totalRedactions", res.totalRedactions}
        };
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.revealBundle", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string pathUtf8 = jsonStringOr(params, "path");
        if (pathUtf8.empty()) return {{"success", false}};
        const bool ok = tools3000::core::WinUtils::openFolderAndSelectItem(
            tools3000::core::WinUtils::utf8ToWstring(pathUtf8));
        return {{"success", ok}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.copyBundleFiles", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string bundleUtf8 = jsonStringOr(params, "bundlePath");
        const std::string shotUtf8 = jsonStringOr(params, "screenshotPath");
        if (bundleUtf8.empty()) return {{"success", false}};
        const bool ok = tools3000::core::FeedbackDiagnosticService::copyDiagnosticFilesToClipboard(
            tools3000::core::WinUtils::utf8ToPath(bundleUtf8),
            shotUtf8.empty() ? std::filesystem::path{} : tools3000::core::WinUtils::utf8ToPath(shotUtf8));
        return {{"success", ok}};
    });
    tools3000::core::MessageBridge::instance().registerHostHandler("feedback.openExternalUrl", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string url = jsonStringOr(params, "url");
        const std::string urlLower = tools3000::core::WinUtils::toLower(url);
        if (urlLower.rfind("https://github.com/yuan278501381/tools3000/", 0) != 0) {
            return {{"success", false}, {"error", "untrusted_url"}};
        }
        // 打开预填 GitHub Issue 页面前自动重刷剪贴板中的诊断包 (.zip + 可选截图)，确保用户过去按一次 Ctrl+V 必中
        const std::string bundleUtf8 = jsonStringOr(params, "bundlePath");
        const std::string shotUtf8 = jsonStringOr(params, "screenshotPath");
        if (!bundleUtf8.empty()) {
            (void)tools3000::core::FeedbackDiagnosticService::copyDiagnosticFilesToClipboard(
                tools3000::core::WinUtils::utf8ToPath(bundleUtf8),
                shotUtf8.empty() ? std::filesystem::path{} : tools3000::core::WinUtils::utf8ToPath(shotUtf8));
        }
        tools3000::core::WinUtils::openUrl(tools3000::core::WinUtils::utf8ToWstring(url));
        return {{"success", true}};
    });

    // 注册导出诊断日志 IPC 处理器（模态保存框 + 磁盘合并解压必须在异步池执行，严禁阻塞 UI 消息泵）
    tools3000::core::MessageBridge::instance().markMethodAsync("app.exportLogs");
    tools3000::core::MessageBridge::instance().registerHostHandler("app.exportLogs", [](const nlohmann::json& params) -> nlohmann::json {
        std::optional<std::filesystem::path> savePath;
        const std::string supplied = jsonStringOr(params, "path");
        if (!supplied.empty()) {
            savePath = tools3000::core::WinUtils::utf8ToWstring(supplied);
        } else {
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

            tools3000::core::FileDialogRequest req;
            req.mode = tools3000::core::FileDialogMode::SaveFile;
            req.title = tools3000::core::trWCstr(L"导出 Tools3000 诊断日志", L"Export Tools3000 Diagnostic Logs");
            req.defaultExtension = L"log";
            req.defaultFileName = defaultName;
            req.filters = {
                {tools3000::core::trWCstr(L"日志文件 (*.log;*.txt)", L"Log Files (*.log;*.txt)"), L"*.log;*.txt"},
                {tools3000::core::trWCstr(L"所有文件 (*.*)", L"All Files (*.*)"), L"*.*"},
            };
            const auto dlgRes = tools3000::core::OutOfProcessFileDialog::show(req);
            if (dlgRes.accepted && !dlgRes.selectedPath.empty()) {
                savePath = dlgRes.selectedPath;
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
            return {
                {"success", false},
                {"error", tools3000::core::trUtf8Cstr("无法创建目标导出文件", "Failed to create target export file")}
            };
        }

        // 写入环境诊断头
        out << "=======================================================\n";
        out << " Tools3000 诊断日志导出报告\n";
        out << " 软件版本: " << tools3000::version::String << "\n";
        out << " 进程 PID: " << GetCurrentProcessId() << "\n";
        out << "=======================================================\n\n";

        // 收集并排序所有当前存在的 .log 与归档 .log.gz 文件（防自吞噬循环 + UTF-8 安全文件名）
        std::filesystem::path logDir = tools3000::core::WinUtils::getLogDirectory();
        std::error_code ec;
        if (std::filesystem::exists(logDir, ec)) {
            struct LogCandidate {
                std::filesystem::path path;
                bool isGzip = false;
            };
            std::vector<LogCandidate> candidates;
            for (const auto& entry : std::filesystem::directory_iterator(logDir, ec)) {
                std::error_code entryEc;
                if (!entry.is_regular_file(entryEc)) {
                    continue;
                }
                const auto& filePath = entry.path();
                std::error_code eqEc;
                if (std::filesystem::equivalent(filePath, target, eqEc) && !eqEc) {
                    continue;
                }
                std::wstring lowerName = filePath.filename().wstring();
                if (!lowerName.empty()) {
                    CharLowerBuffW(lowerName.data(), static_cast<DWORD>(lowerName.size()));
                }
                if (lowerName.ends_with(L".log")) {
                    candidates.push_back({filePath, false});
                } else if (lowerName.ends_with(L".log.gz")) {
                    candidates.push_back({filePath, true});
                }
            }

            std::sort(candidates.begin(), candidates.end(), [](const LogCandidate& a, const LogCandidate& b) {
                return a.path.filename().wstring() < b.path.filename().wstring();
            });

            std::vector<char> chunkBuffer(64 * 1024);
            for (const auto& candidate : candidates) {
                out << ">>> File: "
                    << tools3000::core::WinUtils::wstringToUtf8(candidate.path.filename().wstring())
                    << " <<<\n";
                if (candidate.isGzip) {
                    gzFile gzIn = gzopen_w(candidate.path.c_str(), "rb");
                    if (gzIn) {
                        for (;;) {
                            const int n = gzread(gzIn, chunkBuffer.data(), static_cast<unsigned int>(chunkBuffer.size()));
                            if (n <= 0) {
                                break;
                            }
                            out.write(chunkBuffer.data(), n);
                        }
                        gzclose(gzIn);
                    }
                } else {
                    HANDLE hIn = CreateFileW(
                        candidate.path.c_str(),
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);
                    if (hIn != INVALID_HANDLE_VALUE) {
                        DWORD bytesRead = 0;
                        while (ReadFile(hIn, chunkBuffer.data(), static_cast<DWORD>(chunkBuffer.size()), &bytesRead, nullptr) &&
                               bytesRead > 0) {
                            out.write(chunkBuffer.data(), static_cast<std::streamsize>(bytesRead));
                        }
                        CloseHandle(hIn);
                    }
                }
                out << "\n\n";
            }
        }
        out.close();

        // 自动在资源管理器中高亮选中导出的日志
        std::wstring targetStr = target.wstring();
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + targetStr + L"\"").c_str(), nullptr, SW_SHOWNORMAL);

        return {{"success", true}, {"path", tools3000::core::WinUtils::wstringToUtf8(targetStr)}};
    });

    // 注册应用一键热重启 IPC 处理器
    tools3000::core::MessageBridge::instance().registerHostHandler("app.restart", [hwnd](const nlohmann::json&) -> nlohmann::json {
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

    tools3000::core::MessageBridge::instance().registerHostHandler("app.restartElevated", [hwnd](const nlohmann::json&) -> nlohmann::json {
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

    tools3000::core::MessageBridge::instance().registerHostHandler("app.restartDemoted", [hwnd](const nlohmann::json&) -> nlohmann::json {
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
    tools3000::core::MessageBridge::instance().registerHostHandler("window.minimize", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().minimize();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.toggleMaximize", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().toggleMaximize();
        return {{"success", true}, {"isMaximized", tools3000::ui::SettingsWindow::instance().isMaximized()}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.close", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().close();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.isMaximized", [](const nlohmann::json&) -> nlohmann::json {
        return {{"isMaximized", tools3000::ui::SettingsWindow::instance().isMaximized()}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.dragMove", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SettingsWindow::instance().dragMove();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.startResize", [](const nlohmann::json& params) -> nlohmann::json {
        const std::string edge = jsonStringOr(params, "edge");
        tools3000::ui::SettingsWindow::instance().startResize(edge);
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("window.showSystemMenu", [](const nlohmann::json& params) -> nlohmann::json {
        const int screenX = jsonIntOr(params, "screenX", -1);
        const int screenY = jsonIntOr(params, "screenY", -1);
        tools3000::ui::SettingsWindow::instance().showSystemMenu(screenX, screenY);
        return {{"success", true}};
    });

    // 注册托盘菜单 IPC 处理函数
    tools3000::core::MessageBridge::instance().registerHostHandler("tray.action", [hwnd](const nlohmann::json& params) -> nlohmann::json {
        const std::string action = jsonStringOr(params, "action");
        
        // 执行前先隐藏托盘菜单
        tools3000::ui::TrayWindow::instance().hide();

        if (action == "openSettings") {
            showSettingsWindow();
            const std::string page = jsonStringOr(params, "page");
            if (!page.empty()) {
                tools3000::ui::SettingsWindow::instance().navigateTo(
                    page.front() == '/' ? page : ("/" + page));
            }
        } else if (action == "openAbout") {
            showSettingsWindow();
            tools3000::ui::SettingsWindow::instance().navigateTo("/about");
        } else if (action == "search") {
            auto& searchWnd = tools3000::ui::SearchWindow::instance();
            if (searchWnd.isForegroundActive()) searchWnd.minimize();
            else searchWnd.show(GetModuleHandleW(nullptr));
        } else if (action == "clipboard") {
            tools3000::ui::ClipboardWindow::instance().show(GetModuleHandleW(nullptr));
        } else if (action == "screenshot") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionTriggerScreenshotEvent{});
        } else if (action == "recording") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleRecordingEvent{});
        } else if (action == "pauseGesture") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleGesturePauseEvent{});
        } else if (action == "emergencyFlush") {
            tools3000::core::RemoteMasterEngine::instance().flushModifiers(nullptr, true);
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

    tools3000::core::MessageBridge::instance().registerHostHandler("tray.resize", [](const nlohmann::json& params) -> nlohmann::json {
        const int width = jsonIntOr(params, "width", 0);
        const int height = jsonIntOr(params, "height", 0);
        if (width > 0 && height > 0) {
            tools3000::ui::TrayWindow::instance().setContentSize(width, height);
            return {{"success", true}};
        }
        return {{"success", false}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("tray.hide", [](const nlohmann::json&) -> nlohmann::json {
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

    tools3000::core::MessageBridge::instance().registerHostHandler("spotlight.getSettings", [](const nlohmann::json&) -> nlohmann::json {
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

    tools3000::core::MessageBridge::instance().registerHostHandler("spotlight.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
        auto s = tools3000::ui::SpotlightOverlay::instance().getSettings();
        const bool enabledProvided = params.contains("enabled") && params["enabled"].is_boolean();
        if (enabledProvided) {
            s.enabled = params["enabled"].get<bool>();
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
        if (enabledProvided) {
            tools3000::core::MessageBridge::instance().pushEvent("spotlight:stateChanged", {{"enabled", s.enabled}});
            tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "spotlight"}, {"enabled", s.enabled}});
        }
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("spotlight.trigger", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SpotlightOverlay::instance().trigger();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("spotlight.dismiss", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::ui::SpotlightOverlay::instance().dismiss();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("spotlight.resetDefaults", [](const nlohmann::json&) -> nlohmann::json {
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

    tools3000::core::MessageBridge::instance().registerHostHandler("remote.getSettings", [](const nlohmann::json&) -> nlohmann::json {
        return tools3000::core::RemoteMasterEngine::instance().getSettings().toJson();
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("remote.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
        auto s = tools3000::core::RemoteMasterEngine::instance().getSettings();
        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            s.enabled = params["enabled"].get<bool>();
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

    tools3000::core::MessageBridge::instance().registerHostHandler("remote.emergencyFlush", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::RemoteMasterEngine::instance().flushModifiers(nullptr, true);
        return {{"success", true}};
    });

    // 5.6. 轻量持久化剪贴板历史引擎 (Rust 存储检索内核 + Win32 0% CPU 事件监听 + 极速面板)
    {
        auto& clipMgr = tools3000::clipboard::ClipboardManager::instance();
        clipMgr.initialize(GetModuleHandleW(nullptr));
        clipMgr.setChangeNotifier([](const std::string& envelope) {
            tools3000::ui::ClipboardWindow::instance().pushEventJson(envelope);
        });

        auto registerClipboardHotkey = []() {
            const auto s = tools3000::clipboard::ClipboardManager::instance().getSettings();
            auto& hm = tools3000::core::HotkeyManager::instance();
            hm.unregisterHotkey("Toggle Clipboard");
            if (!s.enabled || s.hotkey.empty()) return;
            if (auto parsed = tools3000::core::HotkeyDef::fromString(s.hotkey)) {
                hm.registerHotkey("Toggle Clipboard", *parsed, []() {
                    if (!tools3000::clipboard::ClipboardManager::instance().isEnabled()) return;
                    tools3000::core::MainThreadDispatcher::instance().post([]() {
                        tools3000::ui::ClipboardWindow::instance().toggle(GetModuleHandleW(nullptr));
                    });
                });
            }
        };
        registerClipboardHotkey();

        tools3000::core::EventBus::instance().subscribe<tools3000::core::ClipboardStateChangedEvent>(
            [registerClipboardHotkey](const tools3000::core::ClipboardStateChangedEvent& e) {
                tools3000::clipboard::ClipboardManager::instance().setEnabled(e.enabled);
                if (!e.enabled) {
                    tools3000::core::MainThreadDispatcher::instance().post([]() {
                        tools3000::ui::ClipboardWindow::instance().hide();
                    });
                }
                registerClipboardHotkey();
            });

        auto& mb = tools3000::core::MessageBridge::instance();
        mb.registerHostHandler("clipboard.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            const auto s = tools3000::clipboard::ClipboardManager::instance().getSettings();
            return {
                {"enabled", s.enabled},
                {"hotkey", s.hotkey},
                {"maxItems", s.maxItems},
                {"maxDays", s.maxDays},
                {"maxImageMb", s.maxImageMb},
                {"autoPaste", s.autoPaste},
                {"plainTextByDefault", s.plainTextByDefault},
                {"captureImages", s.captureImages},
                {"captureFiles", s.captureFiles},
                {"hidePinnedInOtherCategories", s.hidePinnedInOtherCategories},
                {"excludedApps", s.excludedApps}
            };
        });

        mb.registerHostHandler("clipboard.updateSettings", [registerClipboardHotkey](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object()) return {{"success", false}, {"error", "invalid payload"}};
            auto s = tools3000::clipboard::ClipboardManager::instance().getSettings();
            const bool enabledProvided = params.contains("enabled") && params["enabled"].is_boolean();
            if (enabledProvided) s.enabled = params["enabled"].get<bool>();
            if (params.contains("maxItems") && params["maxItems"].is_number_integer()) s.maxItems = params["maxItems"].get<int>();
            if (params.contains("maxDays") && params["maxDays"].is_number_integer()) s.maxDays = params["maxDays"].get<int>();
            if (params.contains("maxImageMb") && params["maxImageMb"].is_number_integer()) s.maxImageMb = params["maxImageMb"].get<int>();
            if (params.contains("autoPaste") && params["autoPaste"].is_boolean()) s.autoPaste = params["autoPaste"].get<bool>();
            if (params.contains("plainTextByDefault") && params["plainTextByDefault"].is_boolean()) s.plainTextByDefault = params["plainTextByDefault"].get<bool>();
            if (params.contains("captureImages") && params["captureImages"].is_boolean()) s.captureImages = params["captureImages"].get<bool>();
            if (params.contains("captureFiles") && params["captureFiles"].is_boolean()) s.captureFiles = params["captureFiles"].get<bool>();
            if (params.contains("hidePinnedInOtherCategories") && params["hidePinnedInOtherCategories"].is_boolean()) {
                s.hidePinnedInOtherCategories = params["hidePinnedInOtherCategories"].get<bool>();
            }
            if (params.contains("excludedApps") && params["excludedApps"].is_string()) s.excludedApps = params["excludedApps"].get<std::string>();

            bool hotkeyChanged = false;
            if (params.contains("hotkey") && params["hotkey"].is_string()) {
                s.hotkey = params["hotkey"].get<std::string>();
                tools3000::core::ConfigManager::instance().set<std::string>("/hotkeys/Toggle Clipboard", s.hotkey);
                hotkeyChanged = true;
            }

            const bool saved = tools3000::clipboard::ClipboardManager::instance().updateSettings(s);
            if (enabledProvided || hotkeyChanged) {
                registerClipboardHotkey();
            }
            if (enabledProvided) {
                if (!s.enabled) {
                    tools3000::core::MainThreadDispatcher::instance().post([]() {
                        tools3000::ui::ClipboardWindow::instance().hide();
                    });
                }
                tools3000::core::MessageBridge::instance().pushEvent("clipboard:stateChanged", {{"enabled", s.enabled}});
                tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "clipboard"}, {"enabled", s.enabled}});
            }
            return {{"success", saved}};
        });

        mb.registerHostHandler("clipboard.query", [](const nlohmann::json& params) -> nlohmann::json {
            const std::string q = params.is_object() ? params.value("query", "") : "";
            const std::string kind = params.is_object() ? params.value("kind", "all") : "all";
            const std::string groupId = params.is_object() ? params.value("groupId", "") : "";
            const uint32_t limit = params.is_object() ? params.value("limit", 80u) : 80u;
            const uint32_t offset = params.is_object() ? params.value("offset", 0u) : 0u;
            const bool excludePinned = params.is_object() && params.contains("excludePinned") && params["excludePinned"].is_boolean()
                ? params["excludePinned"].get<bool>()
                : (kind != "pinned" && tools3000::clipboard::ClipboardManager::instance().getSettings().hidePinnedInOtherCategories);
            return tools3000::clipboard::ClipboardManager::instance().queryItems(q, kind, limit, offset, groupId, excludePinned);
        });

        mb.registerHostHandler("clipboard.getItem", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            auto item = tools3000::clipboard::ClipboardManager::instance().getItem(id);
            return {{"success", item.is_object()}, {"item", std::move(item)}};
        });

        mb.registerHostHandler("clipboard.pasteItem", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            const auto settings = tools3000::clipboard::ClipboardManager::instance().getSettings();
            const bool plainTextOnly = params.is_object()
                ? params.value("plainTextOnly", settings.plainTextByDefault)
                : settings.plainTextByDefault;
            const bool autoPaste = params.is_object()
                ? params.value("autoPaste", settings.autoPaste)
                : settings.autoPaste;
            const bool hideAfter = params.is_object() ? params.value("hideAfter", true) : true;
            const HWND prevHwnd = tools3000::ui::ClipboardWindow::instance().previousForegroundHwnd();
            if (hideAfter) {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    tools3000::ui::ClipboardWindow::instance().hide();
                });
            }
            const bool ok = tools3000::clipboard::ClipboardManager::instance().activateItem(
                id, plainTextOnly, autoPaste, prevHwnd);
            LOG_INFO("Clipboard IPC pasteItem: id={}, plain={}, autoPaste={}, hideAfter={}, hasTarget={}, ok={}",
                     id, plainTextOnly, autoPaste, hideAfter, prevHwnd != nullptr, ok);
            return {{"success", ok}};
        });

        auto extractIdList = [](const nlohmann::json& params) -> std::vector<uint64_t> {
            std::vector<uint64_t> out;
            if (!params.is_object() || !params.contains("ids") || !params["ids"].is_array()) return out;
            out.reserve((std::min<size_t>)(params["ids"].size(), 1000u));
            for (const auto& v : params["ids"]) {
                if (v.is_number_unsigned()) out.push_back(v.get<uint64_t>());
                else if (v.is_number_integer() && v.get<int64_t>() > 0) out.push_back(static_cast<uint64_t>(v.get<int64_t>()));
                if (out.size() >= 1000u) break;
            }
            return out;
        };

        mb.registerHostHandler("clipboard.pasteItemsBatch", [extractIdList](const nlohmann::json& params) -> nlohmann::json {
            const auto ids = extractIdList(params);
            if (ids.empty()) return {{"success", false}, {"error", "empty ids"}};
            const auto settings = tools3000::clipboard::ClipboardManager::instance().getSettings();
            const std::string separator = params.is_object() ? params.value("separator", "\n") : "\n";
            const bool autoPaste = params.is_object() ? params.value("autoPaste", settings.autoPaste) : settings.autoPaste;
            const bool hideAfter = params.is_object() ? params.value("hideAfter", true) : true;
            const HWND prevHwnd = tools3000::ui::ClipboardWindow::instance().previousForegroundHwnd();
            if (hideAfter) {
                tools3000::core::MainThreadDispatcher::instance().post([]() {
                    tools3000::ui::ClipboardWindow::instance().hide();
                });
            }
            const bool ok = tools3000::clipboard::ClipboardManager::instance().activateItemsBatch(
                ids, separator, autoPaste, prevHwnd);
            return {{"success", ok}};
        });

        mb.registerHostHandler("clipboard.togglePin", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            const int res = tools3000::clipboard::ClipboardManager::instance().togglePin(id);
            return {{"success", res >= 0}, {"pinned", res == 1}};
        });

        mb.registerHostHandler("clipboard.deleteItem", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            return {{"success", tools3000::clipboard::ClipboardManager::instance().deleteItem(id)}};
        });

        mb.registerHostHandler("clipboard.deleteItems", [extractIdList](const nlohmann::json& params) -> nlohmann::json {
            const auto ids = extractIdList(params);
            const bool skipPinned = params.is_object() ? params.value("skipPinned", false) : false;
            const uint32_t removed = tools3000::clipboard::ClipboardManager::instance().deleteItems(ids, skipPinned);
            return {{"success", true}, {"removed", removed}};
        });

        mb.registerHostHandler("clipboard.updateItemText", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            const std::string text = params.is_object() ? params.value("text", "") : "";
            const bool ok = tools3000::clipboard::ClipboardManager::instance().updateItemText(id, text);
            return {{"success", ok}};
        });

        mb.registerHostHandler("clipboard.listGroups", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::clipboard::ClipboardManager::instance().listGroups();
        });

        mb.registerHostHandler("clipboard.createGroup", [](const nlohmann::json& params) -> nlohmann::json {
            const std::string name = params.is_object() ? params.value("name", "") : "";
            auto grp = tools3000::clipboard::ClipboardManager::instance().createGroup(name);
            return {{"success", grp.is_object()}, {"group", std::move(grp)}};
        });

        mb.registerHostHandler("clipboard.renameGroup", [](const nlohmann::json& params) -> nlohmann::json {
            const std::string id = params.is_object() ? params.value("id", "") : "";
            const std::string name = params.is_object() ? params.value("name", "") : "";
            return {{"success", tools3000::clipboard::ClipboardManager::instance().renameGroup(id, name)}};
        });

        mb.registerHostHandler("clipboard.deleteGroup", [](const nlohmann::json& params) -> nlohmann::json {
            const std::string id = params.is_object() ? params.value("id", "") : "";
            return {{"success", tools3000::clipboard::ClipboardManager::instance().deleteGroup(id)}};
        });

        mb.registerHostHandler("clipboard.setItemsGroup", [extractIdList](const nlohmann::json& params) -> nlohmann::json {
            const auto ids = extractIdList(params);
            const std::string groupId = params.is_object() ? params.value("groupId", "") : "";
            const uint32_t updated = tools3000::clipboard::ClipboardManager::instance().setItemsGroup(ids, groupId);
            return {{"success", true}, {"updated", updated}};
        });

        mb.registerHostHandler("clipboard.clearHistory", [](const nlohmann::json& params) -> nlohmann::json {
            const bool keepPinned = params.is_object() ? params.value("keepPinned", true) : true;
            const uint32_t removed = tools3000::clipboard::ClipboardManager::instance().clearHistory(keepPinned);
            return {{"success", true}, {"removed", removed}};
        });

        mb.registerHostHandler("clipboard.getStats", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::clipboard::ClipboardManager::instance().getStats();
        });

        mb.registerHostHandler("clipboard.openDataDir", [](const nlohmann::json&) -> nlohmann::json {
            const auto dir = tools3000::clipboard::ClipboardManager::instance().getDataDir();
            if (!dir.empty()) {
                ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return {{"success", true}};
            }
            return {{"success", false}};
        });

        mb.registerHostHandler("clipboard.pinImage", [](const nlohmann::json& params) -> nlohmann::json {
            const uint64_t id = params.is_object() ? params.value("id", static_cast<uint64_t>(0)) : 0;
            const bool autoOcr = params.is_object() ? params.value("autoOcr", false) : false;
            const bool autoTranslate = params.is_object() ? params.value("autoTranslate", false) : false;
            const auto blobPath = tools3000::clipboard::ClipboardManager::instance().resolveImageBlobPath(id);
            std::error_code ec;
            if (blobPath.empty() || !std::filesystem::exists(blobPath, ec)) {
                return {{"success", false}, {"error", "image file not found"}};
            }
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::ClipboardWindow::instance().hide();
            });
            tools3000::core::EventBus::instance().publish(tools3000::core::ActionPinImageFileEvent{
                tools3000::core::WinUtils::wstringToUtf8(blobPath.wstring()),
                autoOcr,
                autoTranslate
            });
            return {{"success", true}};
        });

        mb.registerHostHandler("clipboard.showWindow", [](const nlohmann::json&) -> nlohmann::json {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::ClipboardWindow::instance().show(GetModuleHandleW(nullptr));
            });
            return {{"success", true}};
        });

        mb.registerHostHandler("clipboard.hideWindow", [](const nlohmann::json&) -> nlohmann::json {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::ui::ClipboardWindow::instance().hide();
            });
            return {{"success", true}};
        });
    }

    // 5.7. 系统级文本扩展器 (Text Expander，默认关闭 opt-in 零钩子，支持动态宏与无痕防污染展开)
    {
        tools3000::text_expander::TextExpanderManager::instance().initialize();

        auto& mb = tools3000::core::MessageBridge::instance();
        mb.registerHostHandler("textExpander.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().getSettingsJson();
        });
        mb.registerHostHandler("textExpander.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().updateSettings(params);
        });
        mb.registerHostHandler("textExpander.listRules", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().listRulesJson();
        });
        mb.registerHostHandler("textExpander.upsertRule", [](const nlohmann::json& params) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().upsertRule(params);
        });
        mb.registerHostHandler("textExpander.deleteRule", [](const nlohmann::json& params) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().deleteRule(
                params.is_object() ? params.value("id", std::string{}) : std::string{});
        });
        mb.registerHostHandler("textExpander.toggleRule", [](const nlohmann::json& params) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().toggleRule(
                params.is_object() ? params.value("id", std::string{}) : std::string{},
                params.is_object() ? params.value("enabled", true) : true);
        });
        mb.registerHostHandler("textExpander.importRules", [](const nlohmann::json& params) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().importRules(params);
        });
        mb.registerHostHandler("textExpander.exportRules", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::text_expander::TextExpanderManager::instance().exportRulesJson();
        });
    }

    tools3000::core::MessageBridge::instance().registerHostHandler("remote.resetDefaults", [](const nlohmann::json&) -> nlohmann::json {
        tools3000::core::RemoteMasterEngine::instance().resetDefaults();
        return {{"success", true}};
    });

    tools3000::core::MessageBridge::instance().registerHostHandler("remote.getState", [](const nlohmann::json&) -> nlohmann::json {
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

        // 1. 空格键 (Space / Alt+Space): 在搜索窗口、资源管理器或桌面触发/关闭 QuickLook 文件预览
        //    截图或长截图浮层激活期间（Space 用于平移选区或暂停/继续滚动）严禁跨模块劫持空格键。
        //    严守 1000Hz 钩子红线：通过无锁原子位判断浮层可见态，严禁在 WH_KEYBOARD_LL 内执行 FindWindowW 或跨进程 COM。
        if (vkCode == VK_SPACE) {
            if (!tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible()) {
                const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                     (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
                const bool onlyAlt = altDown && !ctrlDown && !shiftDown && !winDown;
                if (onlyAlt) {
                    if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                        tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                            tools3000::ui::QuickLookWindow::instance().hide();
                        });
                        return true;
                    }
                    // 仅当搜索窗口本身位于最前台获焦时，Alt+Space 才用于预览当前选中结果；
                    // 当用户已切到其他软件（搜索窗在后台或最小化）时，放行 Alt+Space 给全局搜索热键，立即呼出搜索窗口！
                    if (tools3000::ui::SearchWindow::instance().isForegroundActive()) {
                        tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                            tools3000::ui::SearchWindow::instance().triggerQuickLookForSelection();
                        });
                        return true;
                    }
                }
                const bool hasMods = ctrlDown || altDown || shiftDown || winDown;
                if (!hasMods) {
                    auto& ql = tools3000::ui::QuickLookWindow::instance();
                    if (ql.isVisible()) {
                        const HWND fg = GetForegroundWindow();
                        const HWND searchHwnd = tools3000::ui::SearchWindow::instance().getHwnd();
                        const bool searchFocused =
                            searchHwnd && tools3000::ui::SearchWindow::instance().isVisible() &&
                            fg && (fg == searchHwnd || IsChild(searchHwnd, fg));
                        if (searchFocused) {
                            // 焦点位于搜索窗口：放行空格键，供输入框正常键入空格或结果列表按需预览
                            return false;
                        }
                        const HWND qlHwnd = ql.getHwnd();
                        const bool quickLookFocused = fg && qlHwnd && (fg == qlHwnd || IsChild(qlHwnd, fg));
                        if (quickLookFocused && ql.isWebViewReady() && !ql.isPdfPreviewActive()) {
                            // 焦点位于 QuickLook DOM（非跨源 PDF iframe）：交由前端区分 <video>/<audio>/按钮与普通关闭
                            return false;
                        }
                        tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                            tools3000::ui::QuickLookWindow::instance().hide();
                        });
                        return true;
                    }
                    if (tools3000::core::WinUtils::isForegroundExplorerReadyForQuickLook()) {
                        tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                            if (auto sel = tools3000::core::WinUtils::getSelectedExplorerFile(); sel.has_value()) {
                                tools3000::ui::QuickLookWindow::instance().show(*sel);
                            }
                        });
                        return true;
                    }
                }
            }
        }

        // 2. ESC：先收 QuickLook / 非前台置顶剪贴板面板；当剪贴板面板位于前台获焦时，
        //    放行 ESC 给前端执行分级退出（退出内联编辑 -> 退出分组编辑 -> 取消 Ctrl+A 多选 -> 隐藏窗口），
        //    杜绝全局钩子强吞 ESC 导致 Ctrl+A 全选残留锁死双击粘贴。
        if (vkCode == VK_ESCAPE) {
            auto& clipWnd = tools3000::ui::ClipboardWindow::instance();
            if (clipWnd.isVisible() && !clipWnd.isForegroundActive()) {
                tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                    tools3000::ui::ClipboardWindow::instance().hide();
                });
                return true;
            }
            if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                    tools3000::ui::QuickLookWindow::instance().hide();
                });
                return true;
            }
            tools3000::core::MainThreadDispatcher::instance().postDeferred([]() {
                tools3000::core::EventBus::instance().publish(tools3000::core::CancelTransientUiEvent{});
            });
        }

        // 3. 方向键联动：在搜索窗口或资源管理器中切换选中项时，QuickLook 自动刷新预览内容；
        //    当焦点位于 QuickLook 窗口内部时，放行单按方向键用于滚动阅读长文档，仅响应 Alt/Ctrl+Up/Down 切换文件。
        if (vkCode == VK_UP || vkCode == VK_DOWN || vkCode == VK_LEFT || vkCode == VK_RIGHT) {
            if (tools3000::ui::QuickLookWindow::instance().isVisible()) {
                const HWND fg = GetForegroundWindow();
                const HWND qlHwnd = tools3000::ui::QuickLookWindow::instance().getHwnd();
                const bool quickLookFocused = fg && qlHwnd && (fg == qlHwnd || IsChild(qlHwnd, fg));
                if (quickLookFocused) {
                    if (vkCode == VK_UP || vkCode == VK_DOWN) {
                        const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                        const bool winDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                             (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
                        if ((altDown || ctrlDown) && !winDown) {
                            const int delta = (vkCode == VK_UP) ? -1 : 1;
                            tools3000::core::MainThreadDispatcher::instance().postDeferred([delta]() {
                                auto& searchWin = tools3000::ui::SearchWindow::instance();
                                if (searchWin.isVisible()) {
                                    searchWin.stepQuickLookSelection(delta);
                                } else {
                                    tools3000::ui::QuickLookWindow::instance().stepSiblingAsync(delta);
                                }
                            });
                            return true;
                        }
                    }
                    return false;
                }
                const HWND searchHwnd = tools3000::ui::SearchWindow::instance().getHwnd();
                const bool searchFocused =
                    searchHwnd && tools3000::ui::SearchWindow::instance().isVisible() &&
                    fg && (fg == searchHwnd || IsChild(searchHwnd, fg));
                if (searchFocused && (vkCode == VK_LEFT || vkCode == VK_RIGHT)) {
                    // 搜索框左右移动输入光标时不改变选中结果，无需触发预览刷新定时器
                    return false;
                }
                // 同一 HWND/ID 的 SetTimer 会重置截止时间，形成无阻塞防抖。
                SetTimer(hwnd, TIMER_ID_QUICKLOOK_REFRESH, 35, nullptr);
            }
        }

        return false;
    });

    // 6. 初始化本次启动获准加载的插件
    pm.initializePlugins();

    // 6.5. 启动前台窗口全屏/游戏感知守护（精准订阅 FOREGROUND 与 MOVESIZEEND..MINIMIZEEND，
    // 彻底避开 SCROLLINGSTART / CAPTURESTART / MENUSTART 等高频系统噪声污染 UI 主线程消息队列）
    g_fullscreenWinEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        FullscreenGuardWinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_fullscreenMoveSizeHook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZEEND,
        EVENT_SYSTEM_MINIMIZEEND,
        nullptr,
        FullscreenGuardWinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(hwnd, TIMER_ID_FULLSCREEN_GUARD, 2000, nullptr);
    syncForegroundFullscreenState(true);

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
    tools3000::core::UpdateChecker::instance().setExitForUpdateCallback([hwnd]() {
        tools3000::core::MainThreadDispatcher::instance().post([hwnd]() {
            if (hwnd && IsWindow(hwnd)) {
                SetTimer(hwnd, TIMER_ID_EXIT_FOR_UPDATE, 180, nullptr);
            }
        });
    });
    tools3000::core::UpdateChecker::instance().checkAsync(false);

    // 重新锚定宿主崩溃处理器，防止 WebView2 Crashpad 在预热期间覆盖全局异常过滤器
    tools3000::core::CrashHandler::reinstallFilter();

    // 10. 绑定零额外线程的静默冷态防抖回收触发器，并在启动预热落地后预约首次冷态紧缩（清除启动期 ~35MB 一次性初始化脏页）
    tools3000::core::WinUtils::setQuietIdleTrimTrigger([hwnd](uint32_t delayMs) {
        tools3000::core::MainThreadDispatcher::instance().post([hwnd, delayMs]() {
            if (hwnd && IsWindow(hwnd)) {
                SetTimer(hwnd, TIMER_ID_QUIET_IDLE_CLEANUP, delayMs, nullptr);
            }
        });
    });
    tools3000::core::WinUtils::scheduleQuietIdleTrim(5500);
}

void shutdownSubsystems() {
    tools3000::core::WinUtils::setQuietIdleTrimTrigger(nullptr);
    if (g_hostThemeSubId != 0) {
        tools3000::core::EventBus::instance().unsubscribeAndWait(g_hostThemeSubId);
        g_hostThemeSubId = 0;
    }
    if (g_fullscreenWinEventHook) {
        UnhookWinEvent(g_fullscreenWinEventHook);
        g_fullscreenWinEventHook = nullptr;
    }
    if (g_fullscreenMoveSizeHook) {
        UnhookWinEvent(g_fullscreenMoveSizeHook);
        g_fullscreenMoveSizeHook = nullptr;
    }
    tools3000::core::HotkeyManager::instance().setFullscreenSuspended(false);
    tools3000::core::MouseHook::instance().setFullscreenSuspended(false);
    // Withdraw the user-visible entry point before any potentially blocking
    // plugin teardown. Even if a third-party worker is slow, Explorer cannot
    // retain a clickable-looking icon for an instance already shutting down.
    tools3000::tray::TrayIcon::instance().destroy();
    tools3000::core::UpdateChecker::instance().shutdown();
    // 先关闭所有 WebView 入口并卸下全局输入钩子，阻止新的 IPC 请求或高频输入回调进入即将卸载的插件 DLL。
    tools3000::ui::SettingsWindow::instance().destroy();
    tools3000::ui::SearchWindow::instance().destroy();
    tools3000::ui::ClipboardWindow::instance().destroy();
    tools3000::ui::QuickLookWindow::instance().destroy();
    tools3000::ui::TrayWindow::instance().destroy();
    tools3000::text_expander::TextExpanderManager::instance().shutdown();
    tools3000::clipboard::ClipboardManager::instance().shutdown();
    tools3000::core::KeyboardHook::instance().uninstall();
    tools3000::core::MouseHook::instance().uninstall();
    tools3000::core::LuaEngine::instance().shutdown();
    tools3000::core::PluginManager::instance().shutdownPlugins();
    tools3000::core::HotkeyManager::instance().shutdown();
    tools3000::core::ShellContextMenuService::instance().shutdown();
    tools3000::ui::ToastOverlay::instance().shutdown();
    tools3000::ui::SpotlightOverlay::instance().shutdown();
    tools3000::ui::WebViewEnvironmentManager::instance().shutdown();
    tools3000::core::RemoteMasterEngine::instance().shutdown();
    tools3000::core::WinUtils::emergencyFlushInputState();
    tools3000::core::StatsManager::instance().shutdown();
    tools3000::core::backup::ConfigBackupService::instance().runAutoBackupOnExitIfEnabled();
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

static bool isAnyHostSurfaceActive() noexcept {
    const bool mousePressed =
        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0);
    if (mousePressed) return true;
    return tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible() ||
           tools3000::core::KeyboardHook::instance().isRecordingMode() ||
           tools3000::ui::SpotlightOverlay::instance().isActive() ||
           tools3000::ui::SettingsWindow::instance().isVisible() ||
           tools3000::ui::SearchWindow::instance().isVisible() ||
           tools3000::ui::ClipboardWindow::instance().isVisible() ||
           tools3000::ui::TrayWindow::instance().isVisible() ||
           tools3000::ui::QuickLookWindow::instance().isVisible();
}

static void cleanupQuietIdleWorkingSet(HWND hwnd) {
    // 活跃交互与 WebView2 初始化守卫：若截图/录屏选区遮罩可见、聚光灯活跃、鼠标键按下，
    // 或共享 WebView2 环境尚在异步初始化中，顺延至静默期再回收，严禁并发干扰 COM 初始化。
    const bool mousePressed =
        ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0);
    const bool overlayBusy =
        tools3000::core::KeyboardHook::instance().isAnyCaptureOverlayVisible() ||
        tools3000::core::KeyboardHook::instance().isRecordingMode() ||
        tools3000::ui::SpotlightOverlay::instance().isActive() ||
        tools3000::ui::WebViewEnvironmentManager::instance().isInitializing();
    if (mousePressed || overlayBusy) {
        if (hwnd && IsWindow(hwnd)) {
            SetTimer(hwnd, TIMER_ID_QUIET_IDLE_CLEANUP, 3000, nullptr);
        }
        return;
    }

    // 严禁传入 dwUnloadDelay = 0：10 秒安全宽限期可杜绝慢速虚拟机上异步 COM 工厂被提前强拆。
    CoFreeUnusedLibrariesEx(10000, 0);
    tools3000::core::WinUtils::trimWorkingSet(true);
    g_lastQuietIdleTrimTick.store(GetTickCount64(), std::memory_order_release);
}

static void maybeRunIdleMemoryWatermarkTrim(HWND hwnd) {
    // 温和静默水位守卫：仅当全部本应用界面/浮层均已隐藏、距上次静默回收满 60 秒、
    // 且主进程私有工作集自然漂移超过 14 MB（或总工作集超 24 MB）时执行单次回收，
    // 既根治后台长时间飘在 30~40 MB，又彻底杜绝高频反复修剪引发的软缺页锯齿。
    constexpr ULONGLONG kWatermarkCooldownMs = 60'000;
    constexpr SIZE_T kPrivateWsWatermarkBytes = 14ull * 1024ull * 1024ull;
    constexpr SIZE_T kTotalWsWatermarkBytes = 24ull * 1024ull * 1024ull;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastTrim = g_lastQuietIdleTrimTick.load(std::memory_order_acquire);
    if (lastTrim != 0 && now >= lastTrim && (now - lastTrim) < kWatermarkCooldownMs) {
        return;
    }
    if (tools3000::ui::WebViewEnvironmentManager::instance().isInitializing() ||
        isAnyHostSurfaceActive()) {
        return;
    }

    struct ProcessMemoryCountersEx2Abi {
        DWORD cb;
        DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize;
        SIZE_T WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage;
        SIZE_T QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage;
        SIZE_T QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage;
        SIZE_T PeakPagefileUsage;
        SIZE_T PrivateUsage;
        SIZE_T PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    };

    bool overWatermark = false;
    ProcessMemoryCountersEx2Abi pmc2{};
    pmc2.cb = sizeof(pmc2);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc2),
                             pmc2.cb)) {
        overWatermark = (pmc2.PrivateWorkingSetSize >= kPrivateWsWatermarkBytes) ||
                        (pmc2.WorkingSetSize >= kTotalWsWatermarkBytes);
    } else {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc),
                                 pmc.cb)) {
            overWatermark = (pmc.WorkingSetSize >= kTotalWsWatermarkBytes);
        }
    }

    if (overWatermark) {
        cleanupQuietIdleWorkingSet(hwnd);
    }
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
            auto& quickLook = tools3000::ui::QuickLookWindow::instance();
            if (!quickLook.isVisible()) return 0;
            if (tools3000::ui::SearchWindow::instance().isVisible()) {
                tools3000::ui::SearchWindow::instance().syncQuickLookForSelection();
                return 0;
            }
            auto selected = tools3000::core::WinUtils::getSelectedExplorerFile();
            if (selected.has_value()) {
                const std::wstring normSelected = tools3000::ui::normalizePreviewFilePath(*selected);
                if (!normSelected.empty() && _wcsicmp(normSelected.c_str(), quickLook.currentFilePath().c_str()) != 0) {
                    quickLook.previewFile(normSelected);
                }
            }
            return 0;
        }
        if (wParam == TIMER_ID_FULLSCREEN_GUARD) {
            syncForegroundFullscreenState(false);
            if ((++g_fullscreenGuardTickCounter % 5u) == 0u) {
                maybeRunIdleMemoryWatermarkTrim(hwnd);
            }
            return 0;
        }
        if (wParam == TIMER_ID_QUIET_IDLE_CLEANUP) {
            KillTimer(hwnd, TIMER_ID_QUIET_IDLE_CLEANUP);
            cleanupQuietIdleWorkingSet(hwnd);
            return 0;
        }
        if (wParam == TIMER_ID_EXIT_FOR_UPDATE) {
            KillTimer(hwnd, TIMER_ID_EXIT_FOR_UPDATE);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
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

    if (msg == WM_TOOLS3000_TEST_INJECT_KEY) {
        const DWORD vkCode = static_cast<DWORD>(LOWORD(wParam));
        const bool isKeyUp = (HIWORD(wParam) != 0);
        const UINT scanCode = static_cast<UINT>(LOWORD(lParam));
        return tools3000::core::KeyboardHook::instance().injectTestKeyEvent(vkCode, scanCode, isKeyUp) ? 1 : 0;
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
        tools3000::core::WinUtils::applyNativeMenuTheme();
        tools3000::tray::TrayIcon::instance().refreshThemeIcon();
        auto& cfg = tools3000::core::ConfigManager::instance();
        const std::string configuredTheme = cfg.get<std::string>("/general/theme", "system");
        if (configuredTheme == "system") {
            tools3000::core::EventBus::instance().publish(tools3000::core::ThemeChangedEvent{
                configuredTheme,
                cfg.get<std::string>("/general/accentColor", "blue"),
                cfg.get<std::string>("/general/trayIconTheme", "system")
            });
        }
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
        KillTimer(hwnd, TIMER_ID_FULLSCREEN_GUARD);
        if (g_fullscreenWinEventHook) {
            UnhookWinEvent(g_fullscreenWinEventHook);
            g_fullscreenWinEventHook = nullptr;
        }
        if (g_powerNotify) {
            UnregisterSuspendResumeNotification(g_powerNotify);
            g_powerNotify = nullptr;
        }
        WTSUnRegisterSessionNotification(hwnd);
        // 幂等预释放全局系统热键与 Lua 热键，防止重启交接期间旧消息窗口继续占用 RegisterHotKey
        tools3000::core::LuaEngine::instance().shutdown();
        tools3000::core::HotkeyManager::instance().shutdown();
        tools3000::core::WinUtils::emergencyFlushInputState();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
