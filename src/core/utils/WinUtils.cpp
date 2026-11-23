// ─────────────────────────────────────────────────────────────────────────────
// WinUtils.cpp — Windows API 常用操作封装实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/WinUtils.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"

#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <exdisp.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <vector>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <system_error>
#include <atomic>
#include <string_view>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "wintrust.lib")

namespace tools3000::core {

namespace {

bool ensureGdiplusStarted() noexcept {
    struct GdiplusProcessGuard {
        ULONG_PTR token = 0;
        bool initialized = false;
        GdiplusProcessGuard() noexcept {
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
                initialized = true;
            }
        }
    };
    static const GdiplusProcessGuard s_guard;
    return s_guard.initialized;
}

struct ScopedStaComGuard {
    bool needUninit = false;
    ScopedStaComGuard() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (hr == S_OK || hr == S_FALSE) {
            needUninit = true;
        }
    }
    ~ScopedStaComGuard() noexcept {
        if (needUninit) {
            CoUninitialize();
        }
    }
    ScopedStaComGuard(const ScopedStaComGuard&) = delete;
    ScopedStaComGuard& operator=(const ScopedStaComGuard&) = delete;
};

}  // namespace

std::filesystem::path WinUtils::getExePath() {
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    while (buffer.size() <= 32768) {
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1 && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return std::filesystem::path(std::wstring(buffer.data(), copied));
        }
        if (buffer.size() >= 32768) {
            return std::filesystem::path(std::wstring(buffer.data(), copied));
        }
        buffer.resize(buffer.size() * 2, L'\0');
    }
    return {};
}

std::filesystem::path WinUtils::getExeDirectory() {
    return getExePath().parent_path();
}

bool WinUtils::isPortableMode() {
    const auto exeDir = getExeDirectory();
    std::error_code ec;
    if (std::filesystem::is_directory(exeDir / L".tools3000", ec)) return true;
    if (std::filesystem::is_directory(exeDir / L"data", ec)) return true;
    if (std::filesystem::is_directory(exeDir / L"portable_data", ec)) return true;
    return false;
}

std::filesystem::path WinUtils::getAppDataDirectory() {
    const auto exeDir = getExeDirectory();
    std::error_code ec;

    // CI/生命周期门禁可为待测进程和它派生的索引进程提供完全隔离的数据根。
    // 只接受绝对路径，避免主进程与服务因工作目录不同而落到两个位置。
    const DWORD overrideLength = GetEnvironmentVariableW(L"TOOLS3000_DATA_ROOT", nullptr, 0);
    if (overrideLength > 1) {
        std::vector<wchar_t> overrideBuffer(overrideLength);
        const DWORD copied = GetEnvironmentVariableW(
            L"TOOLS3000_DATA_ROOT", overrideBuffer.data(), overrideLength);
        if (copied > 0 && copied < overrideLength) {
            std::filesystem::path overridePath(overrideBuffer.data());
            if (overridePath.is_absolute()) {
                std::filesystem::create_directories(overridePath, ec);
                if (!ec) return overridePath;
                ec.clear();
            }
        }
    }
    
    // 1. 便携模式检测：若主程序同级目录下存在 .tools3000 或 data 目录，优先作为数据根目录
    if (std::filesystem::is_directory(exeDir / L".tools3000", ec)) {
        return exeDir / L".tools3000";
    }
    if (std::filesystem::is_directory(exeDir / L"data", ec)) {
        return exeDir / L"data";
    }
    if (std::filesystem::is_directory(exeDir / L"portable_data", ec)) {
        return exeDir / L"portable_data";
    }

    // 2. 标准系统模式：回退到 %LOCALAPPDATA%\Tools3000
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        auto dir = std::filesystem::path(path) / L"Tools3000";
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    return exeDir / L"data";
}

std::filesystem::path WinUtils::getLogDirectory() {
    auto dir = getAppDataDirectory() / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path WinUtils::getConfigDirectory() {
    auto dir = getAppDataDirectory() / L"config";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

namespace {
std::atomic<ULONGLONG> g_lastTrimTick{0};
constexpr ULONGLONG kTrimThrottleIntervalMs = 2000;

inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && _wcsnicmp(a.data(), b.data(), a.size()) == 0;
}
} // namespace

bool WinUtils::shouldTrimProcess(std::wstring_view exeName) {
    if (exeName.empty()) return false;
    size_t lastSlash = exeName.find_last_of(L"\\/");
    if (lastSlash != std::wstring_view::npos) {
        exeName = exeName.substr(lastSlash + 1);
    }

    // 严格排除全盘搜索常驻服务，保障 286MB 核心索引与 SQLite 内存页安全 (零堆内存分配比对)
    if (equalsIgnoreCase(exeName, L"Tools3000_Service.exe")) {
        return false;
    }

    // 白名单仅修剪 WebView2 渲染管线等子进程
    if (equalsIgnoreCase(exeName, L"msedgewebview2.exe") ||
        equalsIgnoreCase(exeName, L"msedge.exe")) {
        return true;
    }

    return false;
}

void WinUtils::resetTrimThrottleForTesting() {
    g_lastTrimTick.store(0, std::memory_order_relaxed);
}

bool WinUtils::trimWorkingSet(bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force) {
        ULONGLONG last = g_lastTrimTick.load(std::memory_order_relaxed);
        do {
            if (last != 0 && (now >= last && (now - last) < kTrimThrottleIntervalMs)) {
                return false;
            }
        } while (!g_lastTrimTick.compare_exchange_weak(
            last, now, std::memory_order_acq_rel, std::memory_order_relaxed));
    } else {
        g_lastTrimTick.store(now, std::memory_order_release);
    }

    // 1. 修剪 Tools3000 宿主主进程自身物理工作集
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));

    // 2. 级联深度修剪所有子进程与孙子进程 (仅限 WebView2 浏览器进程及其派生的 GPU/Network/Renderer 渲染管线)
    const DWORD currentPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        std::vector<PROCESSENTRY32W> entries;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snapshot, &pe)) {
            do {
                entries.push_back(pe);
            } while (Process32NextW(snapshot, &pe));
        }
        CloseHandle(snapshot);

        // BFS 遍历收集以 currentPid 为根的所有符合白名单后代进程 PID
        std::unordered_set<DWORD> descendantPids;
        std::vector<DWORD> queue;
        queue.push_back(currentPid);

        size_t head = 0;
        while (head < queue.size()) {
            DWORD parentPid = queue[head++];
            for (const auto& entry : entries) {
                if (entry.th32ParentProcessID == parentPid &&
                    entry.th32ProcessID != 0 &&
                    entry.th32ProcessID != currentPid) {
                    if (!shouldTrimProcess(entry.szExeFile)) {
                        continue;
                    }
                    if (descendantPids.insert(entry.th32ProcessID).second) {
                        queue.push_back(entry.th32ProcessID);
                    }
                }
            }
        }

        // 对符合白名单的后代进程调用 SetProcessWorkingSetSize(-1, -1)
        for (DWORD pid : descendantPids) {
            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                SetProcessWorkingSetSize(hProc, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
                CloseHandle(hProc);
            }
        }
    }
    return true;
}

std::wstring WinUtils::processNameFromPid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        std::wstring full(path, size);
        size_t slash = full.find_last_of(L"\\/");
        result = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    }
    CloseHandle(proc);
    return result;
}

std::wstring WinUtils::processPathFromPid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[2048] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, path, &size)) {
        result.assign(path, size);
    }
    CloseHandle(proc);
    return result;
}

std::optional<std::wstring> WinUtils::getForegroundProcessName() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return std::nullopt;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name = processNameFromPid(pid);
    if (name.empty()) return std::nullopt;
    return name;
}

std::wstring WinUtils::getWindowClassName(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    return className;
}

std::wstring WinUtils::getWindowTitle(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    wchar_t buffer[512]{};
    DWORD_PTR copied = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETTEXT, std::size(buffer),
                            reinterpret_cast<LPARAM>(buffer),
                            SMTO_ABORTIFHUNG, 50, &copied) != 0 &&
        copied > 0) {
        const size_t len = wcsnlen(buffer, std::min<size_t>(copied, std::size(buffer) - 1));
        return std::wstring(buffer, len);
    }
    const int fallbackLen = InternalGetWindowText(hwnd, buffer, static_cast<int>(std::size(buffer)));
    if (fallbackLen > 0) {
        return std::wstring(buffer, static_cast<size_t>(fallbackLen));
    }
    return {};
}

std::string WinUtils::wstringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring WinUtils::utf8ToWstring(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size);
    return result;
}

std::string WinUtils::pathToUtf8(const std::filesystem::path& path) {
    return wstringToUtf8(path.wstring());
}

std::filesystem::path WinUtils::utf8ToPath(std::string_view utf8Path) {
    if (utf8Path.empty()) return {};
    return std::filesystem::path(utf8ToWstring(std::string(utf8Path)));
}

namespace {

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

using NtQueryInformationProcessFn = LONG(NTAPI*)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

std::wstring normalizeForMatch(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch == L'"' || ch == L'\'') continue;
        if (ch == L'/') ch = L'\\';
        out.push_back(ch);
    }
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    while (!out.empty() && (out.back() == L'\\' || out.back() == L' ')) {
        out.pop_back();
    }
    size_t start = 0;
    while (start < out.size() && out[start] == L' ') {
        ++start;
    }
    if (start > 0) {
        out.erase(0, start);
    }
    return out;
}

std::wstring extractExeBasenameLower(std::wstring_view exeOrPath) {
    std::wstring norm = normalizeForMatch(exeOrPath);
    const size_t slash = norm.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash + 1 < norm.size()) {
        norm = norm.substr(slash + 1);
    }
    return norm;
}

bool isDwmCloakedWindow(HWND hwnd) noexcept {
    DWORD cloaked = 0;
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

bool isEligibleTopLevelAppWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0) {
        return false;
    }
    if (isDwmCloakedWindow(hwnd)) return false;
    return true;
}

void pulseNeutralForegroundUnlock() noexcept {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_F24;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_F24;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

}  // namespace

std::wstring WinUtils::queryProcessCommandLine(DWORD pid) {
    if (pid == 0) return {};
    static const NtQueryInformationProcessFn pNtQuery = []() -> NtQueryInformationProcessFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
        return reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }();
    if (!pNtQuery) return {};

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return {};

    constexpr ULONG kProcessCommandLineInformation = 60;
    ULONG required = 0;
    LONG status = pNtQuery(hProcess, kProcessCommandLineInformation, nullptr, 0, &required);
    if (required < sizeof(NtUnicodeString) || required > 1024 * 1024) {
        CloseHandle(hProcess);
        return {};
    }

    std::vector<BYTE> buffer(required + 64, 0);
    status = pNtQuery(hProcess, kProcessCommandLineInformation,
                      buffer.data(), static_cast<ULONG>(buffer.size()), &required);
    CloseHandle(hProcess);
    if (status < 0) return {};

    const auto* us = reinterpret_cast<const NtUnicodeString*>(buffer.data());
    if (!us->Buffer || us->Length == 0) return {};
    const size_t wcharCount = us->Length / sizeof(wchar_t);
    return std::wstring(us->Buffer, wcharCount);
}

std::wstring WinUtils::extractProfileMatchHintFromArgs(const std::wstring& args) {
    if (args.empty()) return {};
    const std::wstring lower = toLower(args);
    static constexpr std::wstring_view kKeys[] = {
        L"--user-data-dir=",
        L"--profile-directory=",
        L"-profile ",
        L"-p "
    };
    for (const auto key : kKeys) {
        const size_t pos = lower.find(key);
        if (pos == std::wstring::npos) continue;
        size_t valStart = pos + key.size();
        while (valStart < args.size() && args[valStart] == L' ') ++valStart;
        if (valStart >= args.size()) continue;
        size_t valEnd = valStart;
        if (args[valStart] == L'"') {
            ++valStart;
            valEnd = args.find(L'"', valStart);
            if (valEnd == std::wstring::npos) valEnd = args.size();
        } else {
            valEnd = args.find_first_of(L" \t\r\n", valStart);
            if (valEnd == std::wstring::npos) valEnd = args.size();
        }
        if (valEnd > valStart) {
            return args.substr(valStart, valEnd - valStart);
        }
    }
    return {};
}

bool WinUtils::activateWindowForeground(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
    }

    AllowSetForegroundWindow(ASFW_ANY);
    LockSetForegroundWindow(LSFW_UNLOCK);

    if (SetForegroundWindow(hwnd)) {
        BringWindowToTop(hwnd);
        return true;
    }

    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD curThread = GetCurrentThreadId();
    const DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);

    bool attachedFg = false;
    bool attachedTarget = false;
    if (fgThread != 0 && fgThread != curThread) {
        attachedFg = AttachThreadInput(curThread, fgThread, TRUE) != FALSE;
    }
    if (targetThread != 0 && targetThread != curThread && targetThread != fgThread) {
        attachedTarget = AttachThreadInput(curThread, targetThread, TRUE) != FALSE;
    }

    pulseNeutralForegroundUnlock();
    BringWindowToTop(hwnd);
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    BOOL ok = SetForegroundWindow(hwnd);
    if (!ok) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        ok = SetForegroundWindow(hwnd);
    }

    if (attachedTarget) AttachThreadInput(curThread, targetThread, FALSE);
    if (attachedFg) AttachThreadInput(curThread, fgThread, FALSE);
    return ok != FALSE || GetForegroundWindow() == hwnd;
}

HWND WinUtils::findTopLevelWindowByExe(const std::wstring& exeOrPath,
                                       const std::wstring& cmdLineFilter,
                                       const std::wstring& titleFilter) {
    const std::wstring normTargetFull = normalizeForMatch(exeOrPath);
    const std::wstring targetBase = extractExeBasenameLower(exeOrPath);
    if (targetBase.empty()) return nullptr;
    const bool hasFullPath = normTargetFull.find(L'\\') != std::wstring::npos;
    const std::wstring normCmdFilter = normalizeForMatch(cmdLineFilter);
    const std::wstring normTitleFilter = toLower(titleFilter);

    struct SearchCtx {
        std::wstring targetFull;
        std::wstring targetBase;
        bool hasFullPath = false;
        std::wstring cmdFilter;
        std::wstring titleFilter;
        std::unordered_map<DWORD, std::wstring> pidPathCache;
        std::unordered_map<DWORD, std::wstring> pidCmdCache;
        HWND bestExactPathMatch = nullptr;
        HWND bestBaseMatch = nullptr;
    } ctx;
    ctx.targetFull = normTargetFull;
    ctx.targetBase = targetBase;
    ctx.hasFullPath = hasFullPath;
    ctx.cmdFilter = normCmdFilter;
    ctx.titleFilter = normTitleFilter;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<SearchCtx*>(lParam);
            if (!isEligibleTopLevelAppWindow(hwnd)) return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0) return TRUE;

            auto it = c->pidPathCache.find(pid);
            if (it == c->pidPathCache.end()) {
                it = c->pidPathCache.emplace(pid, normalizeForMatch(processPathFromPid(pid))).first;
            }
            const std::wstring& procFull = it->second;
            if (procFull.empty()) return TRUE;

            const std::wstring procBase = extractExeBasenameLower(procFull);
            if (procBase != c->targetBase) return TRUE;

            const bool exactPathMatched = c->hasFullPath && (procFull == c->targetFull);
            if (c->hasFullPath && !exactPathMatched && !c->cmdFilter.empty()) {
                // 指定了完整路径且同时指定命令行时，优先要求同 exe 路径
                return TRUE;
            }

            if (!c->cmdFilter.empty()) {
                auto cmdIt = c->pidCmdCache.find(pid);
                if (cmdIt == c->pidCmdCache.end()) {
                    cmdIt = c->pidCmdCache.emplace(pid, normalizeForMatch(queryProcessCommandLine(pid))).first;
                }
                if (cmdIt->second.find(c->cmdFilter) == std::wstring::npos) {
                    return TRUE;
                }
            }

            if (!c->titleFilter.empty()) {
                const std::wstring titleLow = toLower(getWindowTitle(hwnd));
                if (titleLow.find(c->titleFilter) == std::wstring::npos) {
                    return TRUE;
                }
            }

            if (exactPathMatched) {
                c->bestExactPathMatch = hwnd;
                return FALSE;  // Z-order 首个精确完整路径命中，立即短路结束
            }
            if (!c->bestBaseMatch) {
                c->bestBaseMatch = hwnd;
                if (!c->hasFullPath) {
                    return FALSE;  // 仅按 exe 名匹配时，首个 Z-order 命中即最优
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return ctx.bestExactPathMatch ? ctx.bestExactPathMatch : ctx.bestBaseMatch;
}

WinUtils::WindowActivateOutcome WinUtils::activateOrToggleWindowByExe(
    const std::wstring& exeOrPath,
    bool toggleIfForeground,
    const std::wstring& cmdLineFilter,
    const std::wstring& titleFilter) {
    HWND target = findTopLevelWindowByExe(exeOrPath, cmdLineFilter, titleFilter);
    if (!target) {
        return WindowActivateOutcome::NotFound;
    }
    if (toggleIfForeground && GetForegroundWindow() == target && !IsIconic(target)) {
        ShowWindow(target, SW_MINIMIZE);
        return WindowActivateOutcome::Minimized;
    }
    activateWindowForeground(target);
    return WindowActivateOutcome::Activated;
}

bool WinUtils::focusWindowByTitle(const std::wstring& titleSubstr,
                                  const std::wstring& optionalExeOrPath) {
    if (titleSubstr.empty()) return false;
    const std::wstring titleLow = toLower(titleSubstr);
    const std::wstring exeBaseLow = optionalExeOrPath.empty() ? L"" : extractExeBasenameLower(optionalExeOrPath);

    struct TitleCtx {
        std::wstring titleLow;
        std::wstring exeBaseLow;
        std::unordered_map<DWORD, std::wstring> pidBaseCache;
        HWND found = nullptr;
    } ctx{titleLow, exeBaseLow, {}, nullptr};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<TitleCtx*>(lParam);
            if (!isEligibleTopLevelAppWindow(hwnd)) return TRUE;

            if (!c->exeBaseLow.empty()) {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == 0) return TRUE;
                auto it = c->pidBaseCache.find(pid);
                if (it == c->pidBaseCache.end()) {
                    it = c->pidBaseCache.emplace(pid, extractExeBasenameLower(processPathFromPid(pid))).first;
                }
                if (it->second != c->exeBaseLow) return TRUE;
            }

            const std::wstring title = toLower(getWindowTitle(hwnd));
            if (title.find(c->titleLow) != std::wstring::npos) {
                c->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found) return false;
    return activateWindowForeground(ctx.found);
}

std::string WinUtils::getProcessNameFromWindow(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return wstringToUtf8(processNameFromPid(pid));
}

std::wstring WinUtils::getProcessPathFromWindow(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return processPathFromPid(pid);
}

std::string WinUtils::base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    DWORD len = 0;
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    if (len == 0) return "";
    std::string result(len, '\0');
    CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &len);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::string WinUtils::toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return str;
}

std::wstring WinUtils::toLower(std::wstring str) {
    if (!str.empty()) {
        CharLowerBuffW(str.data(), static_cast<DWORD>(str.size()));
    }
    return str;
}

namespace {

std::unordered_map<std::wstring, std::string>& fileTypeIconCache() {
    static std::unordered_map<std::wstring, std::string> s_cache;
    return s_cache;
}

std::mutex& fileTypeIconCacheMutex() {
    static std::mutex s_mutex;
    return s_mutex;
}

bool isDynamicInstanceIconExtension(std::wstring_view normalizedExt) noexcept {
    return normalizedExt == L"exe" ||
           normalizedExt == L"lnk" ||
           normalizedExt == L"ico" ||
           normalizedExt == L"cur" ||
           normalizedExt == L"ani" ||
           normalizedExt == L"scr" ||
           normalizedExt == L"msc" ||
           normalizedExt == L"cpl" ||
           normalizedExt == L"appref-ms" ||
           normalizedExt == L"url";
}

bool startsWithAsciiCaseInsensitiveW(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    return _wcsnicmp(text.data(), prefix.data(), prefix.size()) == 0;
}

std::string encodeBgraBufferToPngDataUri(const BYTE* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0 || !ensureGdiplusStarted()) {
        return {};
    }
    Gdiplus::Bitmap gdiBmp(width, height, width * 4, PixelFormat32bppARGB, const_cast<BYTE*>(pixels));
    if (gdiBmp.GetLastStatus() != Gdiplus::Ok) {
        return {};
    }
    CLSID pngClsid{};
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size > 0) {
        auto pCodecsBuf = std::make_unique<uint8_t[]>(size);
        auto* pCodecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(pCodecsBuf.get());
        if (Gdiplus::GetImageEncoders(num, size, pCodecs) == Gdiplus::Ok) {
            for (UINT j = 0; j < num; ++j) {
                if (wcscmp(pCodecs[j].MimeType, L"image/png") == 0) {
                    pngClsid = pCodecs[j].Clsid;
                    break;
                }
            }
        }
    }
    if (pngClsid == CLSID{}) {
        return {};
    }
    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) || !pStream) {
        return {};
    }
    std::string out;
    if (gdiBmp.Save(pStream, &pngClsid, nullptr) == Gdiplus::Ok) {
        STATSTG statstg{};
        HGLOBAL hMem = nullptr;
        if (SUCCEEDED(pStream->Stat(&statstg, STATFLAG_NONAME)) &&
            statstg.cbSize.QuadPart > 0 &&
            SUCCEEDED(GetHGlobalFromStream(pStream, &hMem)) && hMem) {
            const SIZE_T allocSize = GlobalSize(hMem);
            const SIZE_T pngSize = static_cast<SIZE_T>(statstg.cbSize.QuadPart);
            if (pngSize <= allocSize) {
                const auto* pData = static_cast<const uint8_t*>(GlobalLock(hMem));
                if (pData) {
                    std::vector<uint8_t> pngBuf(pData, pData + pngSize);
                    GlobalUnlock(hMem);
                    out = "data:image/png;base64," + WinUtils::base64Encode(pngBuf);
                }
            }
        }
    }
    pStream->Release();
    return out;
}

std::string extractShellItemImageBase64(const std::wstring& parsingName) {
    if (parsingName.empty()) return {};
    Microsoft::WRL::ComPtr<IShellItem> shellItem;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&shellItem))) ||
        !shellItem) {
        return {};
    }
    Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
    if (FAILED(shellItem.As(&factory)) || !factory) {
        return {};
    }
    HBITMAP hBmp = nullptr;
    const SIZE targetSize{32, 32};
    HRESULT hr = factory->GetImage(targetSize, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hBmp);
    if (FAILED(hr) || !hBmp) {
        hr = factory->GetImage(targetSize, SIIGBF_RESIZETOFIT, &hBmp);
    }
    if (FAILED(hr) || !hBmp) {
        return {};
    }

    std::string out;
    BITMAP bm{};
    if (GetObjectW(hBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight != 0) {
        const int width = bm.bmWidth;
        const int height = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
        HDC hdcScreen = GetDC(nullptr);
        if (hdcScreen) {
            if (GetDIBits(hdcScreen, hBmp, 0, static_cast<UINT>(height),
                          pixels.data(), &bi, DIB_RGB_COLORS) > 0) {
                bool hasAlpha = false;
                for (uint32_t px : pixels) {
                    if ((px & 0xFF000000u) != 0) {
                        hasAlpha = true;
                        break;
                    }
                }
                if (!hasAlpha) {
                    for (uint32_t& px : pixels) {
                        px |= 0xFF000000u;
                    }
                }
                out = encodeBgraBufferToPngDataUri(
                    reinterpret_cast<const BYTE*>(pixels.data()), width, height);
            }
            ReleaseDC(nullptr, hdcScreen);
        }
    }
    DeleteObject(hBmp);
    return out;
}

} // namespace

void WinUtils::clearFileTypeIconCacheForTesting() {
    std::lock_guard<std::mutex> lock(fileTypeIconCacheMutex());
    fileTypeIconCache().clear();
}

size_t WinUtils::getFileTypeIconCacheSizeForTesting() {
    std::lock_guard<std::mutex> lock(fileTypeIconCacheMutex());
    return fileTypeIconCache().size();
}

std::string WinUtils::getFileTypeIconBase64(const std::wstring& extension,
                                            bool isDirectory,
                                            const std::wstring& filePath) {
    std::wstring effectiveFilePath = filePath;
    std::wstring rawExt = extension;
    if (!isDirectory &&
        (rawExt.find(L'\\') != std::wstring::npos || rawExt.find(L'/') != std::wstring::npos)) {
        if (effectiveFilePath.empty()) {
            effectiveFilePath = rawExt;
        }
        rawExt = std::filesystem::path(rawExt).extension().wstring();
    } else if (!isDirectory && rawExt.size() > 1 && rawExt.front() != L'.') {
        const size_t lastDot = rawExt.rfind(L'.');
        if (lastDot != std::wstring::npos && lastDot > 0) {
            rawExt = rawExt.substr(lastDot);
        }
    }

    std::wstring normalizedExt = toLower(rawExt);
    if (normalizedExt.rfind(L".", 0) == 0) {
        normalizedExt = normalizedExt.substr(1);
    }
    if (normalizedExt.empty() && !isDirectory && !effectiveFilePath.empty()) {
        std::wstring extFromPath =
            toLower(std::filesystem::path(effectiveFilePath).extension().wstring());
        if (extFromPath.rfind(L".", 0) == 0) {
            extFromPath = extFromPath.substr(1);
        }
        normalizedExt = std::move(extFromPath);
    }

    const bool isShellUri = startsWithAsciiCaseInsensitiveW(effectiveFilePath, L"shell:");
    const bool hasLocalFilePath =
        !effectiveFilePath.empty() && effectiveFilePath.rfind(L"\\\\", 0) != 0;
    const bool useInstanceIcon =
        !isDirectory && hasLocalFilePath && (isShellUri || isDynamicInstanceIconExtension(normalizedExt));

    std::wstring key;
    if (isDirectory) {
        key = L"::dir::";
    } else if (useInstanceIcon) {
        std::wstring normalizedPath = toLower(effectiveFilePath);
        std::replace(normalizedPath.begin(), normalizedPath.end(), L'/', L'\\');
        key = L"path:" + normalizedPath;
    } else {
        key = L"ext:" + normalizedExt;
    }

    {
        std::lock_guard<std::mutex> lock(fileTypeIconCacheMutex());
        auto& cache = fileTypeIconCache();
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }

    ScopedStaComGuard comGuard;
    SHFILEINFOW sfi{};
    DWORD_PTR hr = 0;

    // 0. UWP / MSIX / shell:AppsFolder 应用：通过 IShellItemImageFactory 直接提取 Windows 原生应用高清图标
    if (isShellUri) {
        std::string shellBase64 = extractShellItemImageBase64(effectiveFilePath);
        if (!shellBase64.empty()) {
            std::lock_guard<std::mutex> lock(fileTypeIconCacheMutex());
            auto& cache = fileTypeIconCache();
            if (cache.size() >= 2048) {
                cache.clear();
            }
            cache[key] = shellBase64;
            return shellBase64;
        }
    }

    // 1. 仅对自带实例级自定义图标的动态后缀（.exe/.lnk/.ico/.cur/.ani/.scr/.msc/.cpl/.appref-ms/.url）
    //    访问真实本地路径提取内嵌图标；其余所有静态后缀与普通目录 100% 走内存虚拟属性零磁盘 I/O 提取。
    if (useInstanceIcon && !isShellUri) {
        const DWORD attrs = GetFileAttributesW(effectiveFilePath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            hr = SHGetFileInfoW(
                effectiveFilePath.c_str(),
                0,
                &sfi,
                sizeof(sfi),
                SHGFI_ICON | SHGFI_LARGEICON
            );
            if (!hr || !sfi.hIcon) {
                hr = SHGetFileInfoW(
                    effectiveFilePath.c_str(),
                    0,
                    &sfi,
                    sizeof(sfi),
                    SHGFI_ICON | SHGFI_SMALLICON
                );
            }
        }
    }

    // 2. 按静态扩展名或目录属性零物理磁盘 I/O 提取 Windows 系统关联高清图标
    if (!hr || !sfi.hIcon) {
        std::wstring fakePath = isDirectory
            ? L"folder"
            : (normalizedExt.empty() ? L"dummy" : (L"dummy." + normalizedExt));
        const DWORD fakeAttrs = isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        hr = SHGetFileInfoW(
            fakePath.c_str(),
            fakeAttrs,
            &sfi,
            sizeof(sfi),
            SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON
        );
        if (!hr || !sfi.hIcon) {
            hr = SHGetFileInfoW(
                fakePath.c_str(),
                fakeAttrs,
                &sfi,
                sizeof(sfi),
                SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_SMALLICON
            );
        }
    }

    if (!hr || !sfi.hIcon) {
        SHGetFileInfoW(
            L"dummy",
            FILE_ATTRIBUTE_NORMAL,
            &sfi,
            sizeof(sfi),
            SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON
        );
    }

    std::string base64;
    if (sfi.hIcon) {
        ICONINFO iconInfo{};
        if (GetIconInfo(sfi.hIcon, &iconInfo)) {
            BITMAP bm{};
            if (GetObjectW(iconInfo.hbmColor ? iconInfo.hbmColor : iconInfo.hbmMask, sizeof(bm), &bm)) {
                int width = bm.bmWidth;
                int height = iconInfo.hbmColor ? bm.bmHeight : (bm.bmHeight / 2);
                if (width <= 0 || height <= 0) {
                    width = GetSystemMetrics(SM_CXSMICON);
                    height = GetSystemMetrics(SM_CYSMICON);
                }

                HDC hdcScreen = GetDC(nullptr);
                HDC hdcMem = CreateCompatibleDC(hdcScreen);

                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = width;
                bi.bmiHeader.biHeight = -height; // Top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;

                void* pBits = nullptr;
                HBITMAP hDIB = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
                if (hDIB && pBits) {
                    HGDIOBJ hOldBmp = SelectObject(hdcMem, hDIB);
                    memset(pBits, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

                    // 使用 Windows 底层权威 DrawIconEx 渲染合成
                    DrawIconEx(hdcMem, 0, 0, sfi.hIcon, width, height, 0, nullptr, DI_NORMAL);

                    uint32_t* pixels = static_cast<uint32_t*>(pBits);
                    const int totalPixels = width * height;
                    bool hasValidAlpha = false;

                    for (int i = 0; i < totalPixels; ++i) {
                        if ((pixels[i] & 0xFF000000u) != 0) {
                            hasValidAlpha = true;
                            break;
                        }
                    }

                    // 如果原图标是传统的 1-bit / 24-bit 掩码图标（Alpha 全为 0），
                    // 通过单次 GetDIBits 批量读取掩码缓冲重建真实 Alpha 通道（消灭 256+ 次逐像素 GetPixel 系统调用）
                    if (!hasValidAlpha && iconInfo.hbmMask) {
                        BITMAP maskBm{};
                        const int maskHeight =
                            (GetObjectW(iconInfo.hbmMask, sizeof(maskBm), &maskBm) && maskBm.bmHeight >= height)
                                ? maskBm.bmHeight
                                : height;
                        BITMAPINFO maskBi{};
                        maskBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        maskBi.bmiHeader.biWidth = width;
                        maskBi.bmiHeader.biHeight = -maskHeight; // Top-down，首部 height 行即 AND 掩码平面
                        maskBi.bmiHeader.biPlanes = 1;
                        maskBi.bmiHeader.biBitCount = 32;
                        maskBi.bmiHeader.biCompression = BI_RGB;

                        std::vector<uint32_t> maskPixels(
                            static_cast<size_t>(width) * static_cast<size_t>(maskHeight), 0u);
                        if (GetDIBits(hdcScreen, iconInfo.hbmMask, 0, static_cast<UINT>(maskHeight),
                                      maskPixels.data(), &maskBi, DIB_RGB_COLORS) > 0) {
                            for (int i = 0; i < totalPixels; ++i) {
                                if ((maskPixels[static_cast<size_t>(i)] & 0x00FFFFFFu) == 0x00FFFFFFu) {
                                    pixels[i] = 0x00000000u; // 掩码白色 = 完全透明
                                } else {
                                    pixels[i] |= 0xFF000000u; // 掩码黑色 = 完全不透明
                                }
                            }
                        }
                    }

                    // 通过进程级单例 GDI+ 导出为 32 位透明 PNG（避免多线程并发 GdiplusStartup/Shutdown 互踩）
                    if (ensureGdiplusStarted()) {
                        Gdiplus::Bitmap gdiBmp(width, height, width * 4, PixelFormat32bppARGB, static_cast<BYTE*>(pBits));
                        if (gdiBmp.GetLastStatus() == Gdiplus::Ok) {
                            CLSID pngClsid{};
                            UINT num = 0, size = 0;
                            Gdiplus::GetImageEncodersSize(&num, &size);
                            if (size > 0) {
                                auto pCodecsBuf = std::make_unique<uint8_t[]>(size);
                                auto* pCodecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(pCodecsBuf.get());
                                if (Gdiplus::GetImageEncoders(num, size, pCodecs) == Gdiplus::Ok) {
                                    for (UINT j = 0; j < num; ++j) {
                                        if (wcscmp(pCodecs[j].MimeType, L"image/png") == 0) {
                                            pngClsid = pCodecs[j].Clsid;
                                            break;
                                        }
                                    }
                                }
                            }

                            if (pngClsid != CLSID{}) {
                                IStream* pStream = nullptr;
                                if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) && pStream) {
                                    if (gdiBmp.Save(pStream, &pngClsid, nullptr) == Gdiplus::Ok) {
                                        STATSTG statstg{};
                                        HGLOBAL hMem = nullptr;
                                        if (SUCCEEDED(pStream->Stat(&statstg, STATFLAG_NONAME)) &&
                                            statstg.cbSize.QuadPart > 0 &&
                                            SUCCEEDED(GetHGlobalFromStream(pStream, &hMem)) && hMem) {
                                            const SIZE_T allocSize = GlobalSize(hMem);
                                            const SIZE_T streamBytes = static_cast<SIZE_T>(
                                                std::min<ULONGLONG>(statstg.cbSize.QuadPart, allocSize));
                                            const void* pData = GlobalLock(hMem);
                                            if (pData && streamBytes > 0) {
                                                std::vector<uint8_t> buffer(
                                                    static_cast<const uint8_t*>(pData),
                                                    static_cast<const uint8_t*>(pData) + streamBytes);
                                                GlobalUnlock(hMem);
                                                base64 = "data:image/png;base64," + base64Encode(buffer);
                                            } else if (pData) {
                                                GlobalUnlock(hMem);
                                            }
                                        }
                                    }
                                    pStream->Release();
                                }
                            }
                        }
                    }

                    SelectObject(hdcMem, hOldBmp);
                    DeleteObject(hDIB);
                }
                DeleteDC(hdcMem);
                ReleaseDC(nullptr, hdcScreen);
            }
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        }
        DestroyIcon(sfi.hIcon);
    }

    {
        std::lock_guard<std::mutex> lock(fileTypeIconCacheMutex());
        auto& cache = fileTypeIconCache();
        if (cache.size() >= 4096) {
            cache.clear();
        }
        cache[key] = base64;
    }
    return base64;
}

bool WinUtils::copyToClipboard(const std::wstring& wtext, HWND owner) {
    if (wtext.empty()) return false;

    // EmptyClipboard assigns ownership to the HWND passed to OpenClipboard.
    // Passing nullptr leaves the clipboard without an owner and makes the
    // following SetClipboardData call fail, so provide a short-lived
    // message-only window when the caller has no suitable UI window.
    HWND temporaryOwner = nullptr;
    if (!owner || !IsWindow(owner)) {
        temporaryOwner = CreateWindowExW(
            0, L"STATIC", L"Tools3000 Clipboard Owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        owner = temporaryOwner;
    }
    if (!owner) return false;
    struct TemporaryOwnerGuard {
        HWND hwnd;
        ~TemporaryOwnerGuard() { if (hwnd) DestroyWindow(hwnd); }
    } temporaryOwnerGuard{temporaryOwner};

    // 剪贴板可能被其他进程瞬时独占（如剪贴板管理器、Office），进行有界重试
    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) return false;
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } clipboardGuard;

    if (!EmptyClipboard()) return false;
    const size_t byteCount = (wtext.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!hMem) return false;

    void* dest = GlobalLock(hMem);
    if (!dest) {
        GlobalFree(hMem);
        return false;
    }
    memcpy(dest, wtext.c_str(), byteCount);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        return false;
    }
    return true;
}

bool WinUtils::copyToClipboard(const std::string& text, HWND owner) {
    return copyToClipboard(utf8ToWstring(text), owner);
}

bool WinUtils::copyFileToClipboard(const std::wstring& filePath, HWND owner) {
    if (filePath.empty()) return false;
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    size_t charCount = filePath.size() + 2;
    size_t totalBytes = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
    HGLOBAL hGlobal = GlobalAlloc(GHND | GMEM_SHARE, totalBytes);
    if (!hGlobal) return false;

    auto* drop = static_cast<DROPFILES*>(GlobalLock(hGlobal));
    if (!drop) {
        GlobalFree(hGlobal);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
    memcpy(dest, filePath.c_str(), filePath.size() * sizeof(wchar_t));
    dest[filePath.size()] = L'\0';
    dest[filePath.size() + 1] = L'\0';
    GlobalUnlock(hGlobal);

    HWND temporaryOwner = nullptr;
    if (!owner || !IsWindow(owner)) {
        temporaryOwner = CreateWindowExW(
            0, L"STATIC", L"Tools3000 Clipboard Owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        owner = temporaryOwner;
    }
    if (!owner) {
        GlobalFree(hGlobal);
        return false;
    }
    struct TemporaryOwnerGuard {
        HWND hwnd;
        ~TemporaryOwnerGuard() { if (hwnd) DestroyWindow(hwnd); }
    } temporaryOwnerGuard{temporaryOwner};

    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) {
        GlobalFree(hGlobal);
        return false;
    }
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } clipboardGuard;

    if (!EmptyClipboard()) {
        GlobalFree(hGlobal);
        return false;
    }
    if (!SetClipboardData(CF_HDROP, hGlobal)) {
        GlobalFree(hGlobal);
        return false;
    }
    return true;
}

bool WinUtils::copyGifToClipboard(const std::wstring& filePath, HWND owner) {
    if (filePath.empty()) return false;
    DWORD attrs = GetFileAttributesW(filePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;

    // 1. 准备标准 CF_HDROP 拖拽文件路径
    size_t charCount = filePath.size() + 2;
    size_t dropBytes = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
    HGLOBAL hDropGlobal = GlobalAlloc(GHND | GMEM_SHARE, dropBytes);
    if (!hDropGlobal) return false;

    auto* drop = static_cast<DROPFILES*>(GlobalLock(hDropGlobal));
    if (!drop) {
        GlobalFree(hDropGlobal);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(drop) + sizeof(DROPFILES));
    memcpy(dest, filePath.c_str(), filePath.size() * sizeof(wchar_t));
    dest[filePath.size()] = L'\0';
    dest[filePath.size() + 1] = L'\0';
    GlobalUnlock(hDropGlobal);

    // 2. 准备二进制 GIF 格式数据 (供高级 IM / 浏览器等直接读取原生动图流)
    HGLOBAL hGifGlobal = nullptr;
    UINT formatGif = RegisterClipboardFormatW(L"GIF");
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize > 0 && fileSize < 50 * 1024 * 1024) {
            hGifGlobal = GlobalAlloc(GMEM_MOVEABLE, fileSize);
            if (hGifGlobal) {
                void* gifBuf = GlobalLock(hGifGlobal);
                if (gifBuf) {
                    DWORD bytesRead = 0;
                    ReadFile(hFile, gifBuf, fileSize, &bytesRead, nullptr);
                    GlobalUnlock(hGifGlobal);
                } else {
                    GlobalFree(hGifGlobal);
                    hGifGlobal = nullptr;
                }
            }
        }
        CloseHandle(hFile);
    }

    // 3. 准备首帧标准底向上 (Bottom-Up) CF_DIB
    // 微信在 Windows 上粘贴时：检测到包含图像 DIB 且附带指向 .gif 文件的 CF_HDROP，
    // 会直接作为动态 GIF 动图在聊天会话中以可循环播放的富媒体发送，而不是静态图或普通文件。
    HGLOBAL hDibGlobal = nullptr;
    if (ensureGdiplusStarted()) {
        Gdiplus::Bitmap gifBmp(filePath.c_str());
        if (gifBmp.GetLastStatus() == Gdiplus::Ok && gifBmp.GetWidth() > 0 && gifBmp.GetHeight() > 0) {
            int width = static_cast<int>(gifBmp.GetWidth());
            int height = static_cast<int>(gifBmp.GetHeight());
            Gdiplus::Rect gdiRect(0, 0, width, height);
            Gdiplus::BitmapData bmpData{};
            if (gifBmp.LockBits(&gdiRect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &bmpData) == Gdiplus::Ok) {
                BITMAPINFOHEADER bi{};
                bi.biSize = sizeof(bi);
                bi.biWidth = width;
                bi.biHeight = height; // 正数 = 规范底向上 (Bottom-Up DIB)
                bi.biPlanes = 1;
                bi.biBitCount = 24;
                bi.biCompression = BI_RGB;
                int rowBytes = width * 3;
                int stride = (rowBytes + 3) & ~3;
                bi.biSizeImage = stride * height;

                size_t totalBytes = sizeof(BITMAPINFOHEADER) + bi.biSizeImage;
                hDibGlobal = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
                if (hDibGlobal) {
                    auto* pMem = static_cast<uint8_t*>(GlobalLock(hDibGlobal));
                    if (pMem) {
                        memcpy(pMem, &bi, sizeof(bi));
                        uint8_t* pPixels = pMem + sizeof(bi);
                        const auto* srcPtr = static_cast<const uint8_t*>(bmpData.Scan0);
                        for (int y = 0; y < height; ++y) {
                            int srcY = height - 1 - y;
                            memcpy(pPixels + y * stride, srcPtr + srcY * bmpData.Stride, rowBytes);
                        }
                        GlobalUnlock(hDibGlobal);
                    } else {
                        GlobalFree(hDibGlobal);
                        hDibGlobal = nullptr;
                    }
                }
                gifBmp.UnlockBits(&bmpData);
            }
        }
    }

    // 4. 打开剪贴板并同时写入所有复合格式（提供临时 HWND_MESSAGE 宿主确保 EmptyClipboard 持有有效所有者）
    HWND temporaryOwner = nullptr;
    if (!owner || !IsWindow(owner)) {
        temporaryOwner = CreateWindowExW(
            0, L"STATIC", L"Tools3000 Clipboard Owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        owner = temporaryOwner;
    }
    if (!owner) {
        if (hDropGlobal) GlobalFree(hDropGlobal);
        if (hGifGlobal) GlobalFree(hGifGlobal);
        if (hDibGlobal) GlobalFree(hDibGlobal);
        return false;
    }
    struct TemporaryOwnerGuard {
        HWND hwnd;
        ~TemporaryOwnerGuard() { if (hwnd) DestroyWindow(hwnd); }
    } temporaryOwnerGuard{temporaryOwner};

    bool opened = false;
    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        Sleep(10);
    }
    if (!opened) {
        if (hDropGlobal) GlobalFree(hDropGlobal);
        if (hGifGlobal) GlobalFree(hGifGlobal);
        if (hDibGlobal) GlobalFree(hDibGlobal);
        return false;
    }
    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } clipboardGuard;

    if (!EmptyClipboard()) {
        if (hDropGlobal) GlobalFree(hDropGlobal);
        if (hGifGlobal) GlobalFree(hGifGlobal);
        if (hDibGlobal) GlobalFree(hDibGlobal);
        return false;
    }

    bool anySet = false;
    if (hDropGlobal) {
        if (SetClipboardData(CF_HDROP, hDropGlobal)) {
            anySet = true;
        } else {
            GlobalFree(hDropGlobal);
        }
    }
    if (formatGif != 0 && hGifGlobal) {
        if (SetClipboardData(formatGif, hGifGlobal)) {
            anySet = true;
        } else {
            GlobalFree(hGifGlobal);
        }
    } else if (hGifGlobal) {
        GlobalFree(hGifGlobal);
    }
    if (hDibGlobal) {
        if (SetClipboardData(CF_DIB, hDibGlobal)) {
            anySet = true;
        } else {
            GlobalFree(hDibGlobal);
        }
    }

    return anySet;
}

bool WinUtils::showInExplorer(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    std::wstring params = L"/select,\"" + filePath + L"\"";
    HINSTANCE hInst = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(hInst) > 32;
}

void WinUtils::enableHighDpiSupport() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

RECT WinUtils::getVirtualScreenPhysicalBounds() {
    struct MonitorAccum {
        RECT bounds{ 0, 0, 0, 0 };
        bool initialized = false;
    } accum;

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM dwData) -> BOOL {
        MONITORINFO info{};
        info.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoW(hMonitor, &info)) {
            auto* state = reinterpret_cast<MonitorAccum*>(dwData);
            if (!state->initialized) {
                state->bounds = info.rcMonitor;
                state->initialized = true;
            } else {
                state->bounds.left = (std::min)(state->bounds.left, info.rcMonitor.left);
                state->bounds.top = (std::min)(state->bounds.top, info.rcMonitor.top);
                state->bounds.right = (std::max)(state->bounds.right, info.rcMonitor.right);
                state->bounds.bottom = (std::max)(state->bounds.bottom, info.rcMonitor.bottom);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&accum));

    // 如果没有获取到，回退到 GetSystemMetrics
    if (!accum.initialized || accum.bounds.right <= accum.bounds.left ||
        accum.bounds.bottom <= accum.bounds.top) {
        accum.bounds.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        accum.bounds.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        accum.bounds.right = accum.bounds.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        accum.bounds.bottom = accum.bounds.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    return accum.bounds;
}

float WinUtils::getDpiScale(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi / 96.0f;
}

bool WinUtils::allowWindowCapture(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    SetLastError(ERROR_SUCCESS);
    return SetWindowDisplayAffinity(hwnd, WDA_NONE) != FALSE;
}

namespace {

struct WinUnicodeStringView {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

std::wstring queryProcessCommandLineLimited(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};

    static const auto s_ntQueryInfo = []() {
        using NtQueryInfoFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQueryInfoFn>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
    }();

    std::wstring result;
    if (s_ntQueryInfo) {
        constexpr ULONG kProcessCommandLineInformation = 60;
        ULONG retLen = 0;
        s_ntQueryInfo(hProc, kProcessCommandLineInformation, nullptr, 0, &retLen);
        if (retLen >= sizeof(WinUnicodeStringView) && retLen <= 65536) {
            std::vector<std::byte> buf(retLen);
            if (s_ntQueryInfo(hProc, kProcessCommandLineInformation, buf.data(), retLen, &retLen) == 0) {
                const auto* us = reinterpret_cast<const WinUnicodeStringView*>(buf.data());
                if (us->Buffer && us->Length >= sizeof(wchar_t)) {
                    result.assign(us->Buffer, us->Length / sizeof(wchar_t));
                    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
                        return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
                    });
                }
            }
        }
    }
    CloseHandle(hProc);
    return result;
}

bool hasActiveScreenCaptureRemoteSession() {
    DWORD curSessionId = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &curSessionId)) {
        return false;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool activeIncoming = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            const bool isRustDesk = (_wcsicmp(pe.szExeFile, L"rustdesk.exe") == 0);
            const bool isSunlogin = !isRustDesk && (_wcsicmp(pe.szExeFile, L"sunloginclient.exe") == 0);
            const bool isToDesk   = !isRustDesk && !isSunlogin && (_wcsicmp(pe.szExeFile, L"todesk.exe") == 0);
            if (!isRustDesk && !isSunlogin && !isToDesk) {
                continue;
            }
            DWORD procSessionId = 0;
            if (!ProcessIdToSessionId(pe.th32ProcessID, &procSessionId) || procSessionId != curSessionId) {
                continue;
            }
            const std::wstring cmd = queryProcessCommandLineLimited(pe.th32ProcessID);
            if (cmd.empty()) continue;
            // RustDesk 被控端仅在外部远程桌面活跃连入时派生 `--cm` (Connection Manager)；
            // 本机后台待机 (--service/--server/--tray) 或主控连别人时均无 `--cm`，零误判！
            if (isRustDesk && (cmd.find(L"--cm") != std::wstring::npos)) {
                activeIncoming = true;
                break;
            }
            // 向日葵被控端活跃桌面抓屏子进程特征 (`--mod=desktop`)
            if (isSunlogin && (cmd.find(L"--mod=desktop") != std::wstring::npos)) {
                activeIncoming = true;
                break;
            }
            // ToDesk 被控端活跃桌面传输子进程特征
            if (isToDesk && (cmd.find(L"-m work") != std::wstring::npos || cmd.find(L"--desktop") != std::wstring::npos)) {
                activeIncoming = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return activeIncoming;
}

} // namespace

bool WinUtils::isRemoteDesktopSession() {
    if (GetSystemMetrics(SM_REMOTESESSION) != 0 ||
        GetSystemMetrics(SM_REMOTECONTROL) != 0) {
        return true;
    }
    return hasActiveScreenCaptureRemoteSession();
}

bool WinUtils::canSafelyExcludeWindowFromCapture() {
    if (isRemoteDesktopSession()) {
        return false;
    }
    static const DWORD s_osBuildNumber = []() -> DWORD {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return 0;
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (!fn) return 0;
        RTL_OSVERSIONINFOW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);
        if (fn(&osvi) == 0) {
            return osvi.dwBuildNumber;
        }
        return 0;
    }();
    return s_osBuildNumber >= 19041;
}

bool WinUtils::applyCaptureExclusionIfSafe(HWND hwnd, bool wantExclude) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (wantExclude && canSafelyExcludeWindowFromCapture()) {
        if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE) {
            return true;
        }
    }
    SetWindowDisplayAffinity(hwnd, WDA_NONE);
    return false;
}

bool WinUtils::excludeWindowFromCapture(HWND hwnd) {
    // 零隐私防截底层策略 (Zero WDA_EXCLUDEFROMCAPTURE)：
    // 用户明确要求所有界面都支持被截图，绝不开启隐私防截，统一重定向至 allowWindowCapture (WDA_NONE)
    return allowWindowCapture(hwnd);
}

bool WinUtils::isDesktopWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    std::wstring cls = getWindowClassName(hwnd);
    if (cls == L"Progman" || cls == L"WorkerW") return true;
    HWND parent = GetParent(hwnd);
    if (parent) {
        std::wstring pCls = getWindowClassName(parent);
        if (pCls == L"Progman" || pCls == L"WorkerW") return true;
    }
    return false;
}

bool WinUtils::isTaskbarWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    std::wstring cls = getWindowClassName(root);
    return (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd");
}

bool WinUtils::isWindowVisibleOnCurrentDesktop(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root || !IsWindow(root)) {
        root = hwnd;
    }
    if (!IsWindowVisible(root) || IsIconic(root)) {
        return false;
    }

    // 第一防线：DWM Cloaked 校验（Windows 10/11 切换虚拟桌面时，非当前桌面的窗口会被标记 DWM_CLOAKED_SHELL；
    // 挂起的 UWP / Modern 应用窗口会被标记 DWM_CLOAKED_APP，IsWindowVisible 仍为 TRUE，必须用 DWMWA_CLOAKED 剔除）
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return false;
    }
    if (root != hwnd) {
        DWORD childCloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &childCloaked, sizeof(childCloaked))) && childCloaked != 0) {
            return false;
        }
    }

    // 第二防线：过滤完全不可见的零透明度分层幽灵窗
    const LONG_PTR exStyle = GetWindowLongPtrW(root, GWL_EXSTYLE);
    if ((exStyle & WS_EX_LAYERED) != 0) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(root, nullptr, &alpha, &flags) &&
            (flags & LWA_ALPHA) != 0 && alpha == 0) {
            return false;
        }
    }

    // 第三防线：官方 COM IVirtualDesktopManager 跨虚拟桌面权威校验（线程局部复用，零每帧创建开销）
    struct ThreadLocalVdm {
        Microsoft::WRL::ComPtr<IVirtualDesktopManager> mgr;
        bool attempted = false;
        bool ownCom = false;
        ~ThreadLocalVdm() {
            mgr.Reset();
            if (ownCom) {
                CoUninitialize();
            }
        }
        IVirtualDesktopManager* get() noexcept {
            if (!attempted) {
                attempted = true;
                HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                if (coHr == S_OK || coHr == S_FALSE) {
                    ownCom = true;
                }
                if (SUCCEEDED(coHr) || coHr == RPC_E_CHANGED_MODE) {
                    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(mgr.GetAddressOf()));
                }
            }
            return mgr.Get();
        }
    };
    thread_local ThreadLocalVdm tlsVdm;
    if (IVirtualDesktopManager* vdm = tlsVdm.get()) {
        BOOL onCurrent = TRUE;
        const HRESULT hr = vdm->IsWindowOnCurrentVirtualDesktop(root, &onCurrent);
        if (SUCCEEDED(hr) && !onCurrent) {
            return false;
        }
    }

    return true;
}

bool WinUtils::isSystemLanguageChinese() {
    LANGID langId = GetUserDefaultUILanguage();
    return PRIMARYLANGID(langId) == LANG_CHINESE;
}

bool WinUtils::isWindowFullscreen(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    // 排除桌面、Shell 窗口、隐藏窗口、最小化窗口与鼠标穿透层
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow() ||
        !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((exStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) {
        return false;
    }

    std::wstring className = getWindowClassName(hwnd);
    if (className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd") {
        return false;
    }

    // 获取窗口所在的显示器
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!hMon) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    // 允许 2px 容差，兼容 DWM Focus Assist 避让缩进 (vw-1, vh-1) 与高分屏 DPI 舍入误差
    constexpr LONG kFullscreenTolerancePx = 2;

    // 若具有标准标题栏（WS_CAPTION），常规最大化窗口 (IsZoomed) 非全屏；
    // 但部分 DXGI / SDL2 全屏游戏保留了 WS_CAPTION 样式位且未处于最大化状态，其客户区覆盖整个物理显示器
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) {
        if (IsZoomed(hwnd)) {
            return false;
        }
        RECT rcClient{};
        if (!GetClientRect(hwnd, &rcClient)) return false;
        POINT ptTopLeft{rcClient.left, rcClient.top};
        POINT ptBottomRight{rcClient.right, rcClient.bottom};
        if (!ClientToScreen(hwnd, &ptTopLeft) || !ClientToScreen(hwnd, &ptBottomRight)) {
            return false;
        }
        return (ptTopLeft.x <= mi.rcMonitor.left + kFullscreenTolerancePx &&
                ptTopLeft.y <= mi.rcMonitor.top + kFullscreenTolerancePx &&
                ptBottomRight.x >= mi.rcMonitor.right - kFullscreenTolerancePx &&
                ptBottomRight.y >= mi.rcMonitor.bottom - kFullscreenTolerancePx);
    }

    RECT rcWindow{};
    if (!GetWindowRect(hwnd, &rcWindow)) return false;

    // 判定无边框窗口是否覆盖整个物理显示器区域（含 2px 容差）
    return (rcWindow.left <= mi.rcMonitor.left + kFullscreenTolerancePx &&
            rcWindow.top <= mi.rcMonitor.top + kFullscreenTolerancePx &&
            rcWindow.right >= mi.rcMonitor.right - kFullscreenTolerancePx &&
            rcWindow.bottom >= mi.rcMonitor.bottom - kFullscreenTolerancePx);
}

namespace {

std::atomic<int> g_fullscreenPolicyMode{static_cast<int>(FullscreenPolicyMode::Smart)};

inline bool classNameStartsWith(std::wstring_view cls, std::wstring_view prefix) noexcept {
    return cls.size() >= prefix.size() && cls.substr(0, prefix.size()) == prefix;
}

inline bool isTabbedBrowserClassName(std::wstring_view cls) noexcept {
    return classNameStartsWith(cls, L"Chrome_WidgetWin") ||
           cls == L"MozillaWindowClass" ||
           cls == L"CabinetWClass";
}

inline bool isWebBrowserClassName(std::wstring_view cls) noexcept {
    return classNameStartsWith(cls, L"Chrome_WidgetWin") ||
           cls == L"MozillaWindowClass";
}

inline bool isTools3000UiClassName(std::wstring_view cls) noexcept {
    constexpr std::wstring_view kPrefix = L"Tools3000_";
    return cls.size() >= kPrefix.size() && cls.substr(0, kPrefix.size()) == kPrefix;
}

inline wchar_t asciiLowerWide(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
    return ch;
}

inline char asciiLowerChar(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') return static_cast<char>(ch - 'A' + 'a');
    return ch;
}

inline bool equalsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerWide(a[i]) != asciiLowerWide(b[i])) return false;
    }
    return true;
}

inline bool equalsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
    }
    return true;
}

inline bool endsWithIgnoreCaseAscii(std::wstring_view str, std::wstring_view suffix) noexcept {
    if (str.size() < suffix.size()) return false;
    return equalsIgnoreCaseAscii(str.substr(str.size() - suffix.size()), suffix);
}

inline bool containsIgnoreCaseNormalizedPath(std::wstring_view path, std::wstring_view needle) noexcept {
    if (needle.empty() || path.size() < needle.size()) return false;
    const size_t limit = path.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            wchar_t pc = asciiLowerWide(path[i + j]);
            if (pc == L'/') pc = L'\\';
            wchar_t nc = asciiLowerWide(needle[j]);
            if (nc == L'/') nc = L'\\';
            if (pc != nc) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

inline std::wstring_view extractFileNameView(std::wstring_view pathOrName) noexcept {
    size_t pos = pathOrName.find_last_of(L"\\/");
    return (pos == std::wstring_view::npos) ? pathOrName : pathOrName.substr(pos + 1);
}

inline bool isWebBrowserProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kBrowsers[] = {
        L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"vivaldi.exe", L"arc.exe", L"waterfox.exe",
        L"librewolf.exe", L"zen.exe", L"thorium.exe", L"chromium.exe",
        L"qqbrowser.exe", L"360chrome.exe", L"360se.exe", L"sogouexplorer.exe",
        L"2345explorer.exe", L"liebao.exe", L"maxthon.exe", L"centbrowser.exe",
        L"whale.exe", L"yandex.exe"
    };
    for (const auto& b : kBrowsers) {
        if (equalsIgnoreCaseAscii(procFile, b)) return true;
    }
    return false;
}

inline bool isElectronOrIdeProductivityProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kProductivityApps[] = {
        L"code.exe", L"cursor.exe", L"windsurf.exe", L"devenv.exe",
        L"idea64.exe", L"clion64.exe", L"pycharm64.exe", L"webstorm64.exe",
        L"goland64.exe", L"rider64.exe", L"rustrover64.exe", L"phpstorm64.exe",
        L"datagrip64.exe", L"studio64.exe", L"sublime_text.exe", L"notepad++.exe",
        L"obsidian.exe", L"notion.exe", L"logseq.exe", L"typora.exe",
        L"feishu.exe", L"lark.exe", L"dingtalk.exe", L"wxwork.exe",
        L"wechat.exe", L"weixin.exe", L"qq.exe", L"tim.exe",
        L"slack.exe", L"teams.exe", L"zoom.exe", L"wemeetapp.exe",
        L"winword.exe", L"excel.exe", L"onenote.exe", L"outlook.exe",
        L"wps.exe", L"et.exe", L"photoshop.exe", L"illustrator.exe",
        L"acad.exe", L"blender.exe", L"unity.exe", L"unrealeditor.exe",
        L"ue4editor.exe", L"godot.exe", L"scrcpy.exe", L"explorer.exe", L"tools3000.exe"
    };
    for (const auto& app : kProductivityApps) {
        if (equalsIgnoreCaseAscii(procFile, app)) return true;
    }
    return false;
}

inline constexpr std::wstring_view kCanonicalRemoteProcessesW[] = {
    L"ToDesk.exe",
    L"SunloginClient.exe",
    L"AnyDesk.exe",
    L"RustDesk.exe",
    L"mstsc.exe",
    L"msrdc.exe",
    L"msrdcw.exe",
    L"TeamViewer.exe",
    L"vncviewer.exe",
    L"tv_w32.exe",
    L"parsec.exe",
    L"parsecd.exe",
    L"Splashtop.exe",
    L"UltraViewer_Desktop.exe",
    L"vmware.exe",
    L"vmware-vmx.exe",
    L"vmware-remotemks.exe",
    L"VirtualBoxVM.exe",
    L"vmconnect.exe",
    L"uuremote.exe",
    L"GameViewer.exe",
    L"Moonlight.exe"
};

inline constexpr std::wstring_view kCanonicalRemoteClassesW[] = {
    L"TscShellContainerClass",
    L"TSSHELLWND",
    L"IHWindowClass",
    L"OPContainerClass",
    L"SunloginMain",
    L"SunloginRemote",
    L"ToDesk_Main",
    L"ToDesk_Remote",
    L"anydesk",
    L"RustDesk",
    L"TeamViewer"
};

inline bool isRemoteDesktopOrVmProcessOrClass(std::wstring_view processName,
                                              std::wstring_view className) noexcept {
    const std::wstring_view procFile = extractFileNameView(processName);
    if (!procFile.empty()) {
        for (const auto& rp : kCanonicalRemoteProcessesW) {
            if (equalsIgnoreCaseAscii(procFile, rp)) return true;
        }
    }
    if (!className.empty()) {
        for (const auto& rc : kCanonicalRemoteClassesW) {
            if (equalsIgnoreCaseAscii(className, rc)) return true;
        }
    }
    return false;
}

inline bool isNonGameLauncherOrUtilityProcessName(std::wstring_view procFile) noexcept {
    if (isWebBrowserProcessName(procFile) ||
        isElectronOrIdeProductivityProcessName(procFile) ||
        isRemoteDesktopOrVmProcessOrClass(procFile, L"")) {
        return true;
    }
    static constexpr std::wstring_view kLaunchersAndUtilities[] = {
        L"steam.exe", L"steamwebhelper.exe", L"steamservice.exe", L"gameoverlayui.exe",
        L"epicgameslauncher.exe", L"epicwebhelper.exe", L"unrealcefsubp.exe",
        L"wegame.exe", L"tgp_daemon.exe", L"tgp_browser.exe",
        L"battle.net.exe", L"agent.exe", L"galaxyclient.exe", L"galaxyclienthelper.exe",
        L"eaapp.exe", L"eadesktop.exe", L"eabackgroundservice.exe",
        L"upc.exe", L"ubisoftconnect.exe", L"uplaywebcore.exe",
        L"riotclientservices.exe", L"riotclientux.exe", L"riotclientuxrender.exe",
        L"leagueclient.exe", L"leagueclientux.exe", L"leagueclientuxrender.exe",
        L"wallpaper32.exe", L"wallpaper64.exe", L"ui32.exe", L"webwallpaper32.exe",
        L"losslessscaling.exe", L"obs64.exe", L"obs32.exe",
        L"discord.exe", L"kook.exe", L"yy.exe", L"teamspeak3.exe",
        L"msi afterburner.exe", L"rtss.exe", L"playnite.desktopapp.exe", L"playnite.fullscreenapp.exe"
    };
    for (const auto& util : kLaunchersAndUtilities) {
        if (equalsIgnoreCaseAscii(procFile, util)) return true;
    }
    return false;
}

inline bool isExplicitCompetitiveOr3DGameProcessName(std::wstring_view procFile) noexcept {
    static constexpr std::wstring_view kExplicitGames[] = {
        L"dota2.exe", L"cs2.exe", L"csgo.exe", L"hl2.exe", L"deadlock.exe", L"project8.exe",
        L"league of legends.exe", L"valorant.exe", L"valorant-win64-shipping.exe",
        L"overwatch.exe", L"wow.exe", L"wowclassic.exe", L"diablo iv.exe", L"diablo iii64.exe",
        L"hearthstone.exe", L"sc2_x64.exe", L"sc2.exe", L"war3.exe",
        L"r5apex.exe", L"tslgame.exe", L"narakabladepoint.exe",
        L"genshinimpact.exe", L"yuanshen.exe", L"starrail.exe", L"zenlesszonezero.exe", L"bh3.exe",
        L"b1-win64-shipping.exe", L"eldenring.exe", L"sekiro.exe", L"darksoulsiii.exe", L"armoredcore6.exe",
        L"cyberpunk2077.exe", L"witcher3.exe", L"bg3.exe", L"bg3_dx11.exe",
        L"rdr2.exe", L"gtav.exe", L"gta5.exe", L"playgtav.exe",
        L"monsterhunterworld.exe", L"monsterhunterrise.exe", L"monsterhunterwilds.exe",
        L"re4.exe", L"re8.exe", L"streetfighter6.exe",
        L"pathofexile_x64.exe", L"pathofexile.exe", L"pathofexilesteam.exe", L"pathofexile2.exe",
        L"destiny2.exe", L"warframe.x64.exe", L"helldivers2.exe",
        L"palworld-win64-shipping.exe", L"client-win64-shipping.exe", L"deltaforceclient-win64-shipping.exe",
        L"fc24.exe", L"fc25.exe", L"nba2k24.exe", L"nba2k25.exe",
        L"forzahorizon5.exe", L"forzahorizon4.exe", L"flightsimulator.exe",
        L"minecraft.windows.exe", L"terraria.exe", L"stardew valley.exe",
        L"factorio.exe", L"rimworldwin64.exe", L"dontstarve_steam_x64.exe",
        L"hollow_knight.exe", L"hades.exe", L"hades2.exe", L"celeste.exe",
        L"balatro.exe", L"slaythespire.exe", L"deadbydaylight-win64-shipping.exe",
        L"rainbowsix.exe", L"rainbowsix_vulkan.exe", L"bf2042.exe", L"bfv.exe", L"bf1.exe",
        L"cod.exe", L"ModernWarfare.exe", L"BlackOpsColdWar.exe"
    };
    for (const auto& game : kExplicitGames) {
        if (equalsIgnoreCaseAscii(procFile, game)) return true;
    }
    if (endsWithIgnoreCaseAscii(procFile, L"-win64-shipping.exe") ||
        endsWithIgnoreCaseAscii(procFile, L"-win32-shipping.exe")) {
        return true;
    }
    return false;
}

inline bool isKnownGameEngineWindowClass(std::wstring_view cls) noexcept {
    if (cls.empty()) return false;
    static constexpr std::wstring_view kGameClasses[] = {
        L"Valve001", L"Source2WndClass", L"SDL_app",
        L"UnityWndClass", L"UnrealWindow", L"RiotWindowClass",
        L"CryENGINE", L"GLFW30", L"LWJGL",
        L"D3DWindow", L"Direct3DWindowClass", L"D3D11_GameWindow", L"D3D12_GameWindow",
        L"grcWindow", L"CreationEngine", L"REEngine", L"AnvilNext", L"Prism3D"
    };
    for (const auto& gc : kGameClasses) {
        if (equalsIgnoreCaseAscii(cls, gc)) return true;
    }
    return false;
}

inline bool isGameLibraryExecutablePath(std::wstring_view exePath) noexcept {
    if (exePath.empty()) return false;
    static constexpr std::wstring_view kGameLibraryMarkers[] = {
        L"\\steamapps\\common\\",
        L"\\epic games\\",
        L"\\wegameapps\\",
        L"\\riot games\\",
        L"\\gog galaxy\\games\\",
        L"\\xboxgames\\",
        L"\\ea games\\",
        L"\\origin games\\",
        L"\\ubisoft game launcher\\games\\",
        L"\\battle.net\\games\\"
    };
    for (const auto& marker : kGameLibraryMarkers) {
        if (containsIgnoreCaseNormalizedPath(exePath, marker)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool WinUtils::isProductivityToolkitClassName(std::wstring_view cls) noexcept {
    if (isTabbedBrowserClassName(cls) || isTools3000UiClassName(cls)) return true;
    if (cls == L"OrpheusBrowserHost" || cls == L"CefBrowserWindow" ||
        cls == L"ApplicationFrameWindow") {
        return true;
    }
    return classNameStartsWith(cls, L"Qt");
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, const wchar_t* cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls ? cls : L"");
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, std::wstring_view cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls);
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, const std::wstring& cls) noexcept {
    return isFullscreen && !isProductivityToolkitClassName(cls);
}

bool WinUtils::shouldBypassFullscreenInteractions(bool isFullscreen, bool isProductivityClass) noexcept {
    return isFullscreen && !isProductivityClass;
}

bool WinUtils::shouldBypassFullscreenInteractions(HWND hwnd) {
    if (!hwnd || !isWindowFullscreen(hwnd)) {
        return false;
    }
    const std::wstring classWide = getWindowClassName(hwnd);
    return shouldBypassFullscreenInteractions(true, classWide);
}

FullscreenPolicyMode WinUtils::parseFullscreenPolicyMode(std::string_view modeStr) noexcept {
    if (equalsIgnoreCaseAscii(modeStr, "strict_all") ||
        equalsIgnoreCaseAscii(modeStr, "strict") ||
        equalsIgnoreCaseAscii(modeStr, "all")) {
        return FullscreenPolicyMode::StrictAll;
    }
    if (equalsIgnoreCaseAscii(modeStr, "gesture_only") ||
        equalsIgnoreCaseAscii(modeStr, "gesture")) {
        return FullscreenPolicyMode::GestureOnly;
    }
    if (equalsIgnoreCaseAscii(modeStr, "off") ||
        equalsIgnoreCaseAscii(modeStr, "disabled") ||
        equalsIgnoreCaseAscii(modeStr, "none") ||
        equalsIgnoreCaseAscii(modeStr, "false")) {
        return FullscreenPolicyMode::Off;
    }
    return FullscreenPolicyMode::Smart;
}

const char* WinUtils::fullscreenPolicyModeToString(FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::StrictAll:   return "strict_all";
        case FullscreenPolicyMode::GestureOnly: return "gesture_only";
        case FullscreenPolicyMode::Off:         return "off";
        case FullscreenPolicyMode::Smart:
        default:                                return "smart";
    }
}

void WinUtils::setFullscreenPolicyMode(FullscreenPolicyMode mode) noexcept {
    g_fullscreenPolicyMode.store(static_cast<int>(mode), std::memory_order_release);
}

FullscreenPolicyMode WinUtils::getFullscreenPolicyMode() noexcept {
    return static_cast<FullscreenPolicyMode>(g_fullscreenPolicyMode.load(std::memory_order_acquire));
}

std::vector<std::string> WinUtils::defaultRemoteTargetProcesses() {
    std::vector<std::string> out;
    out.reserve(std::size(kCanonicalRemoteProcessesW));
    for (const auto& w : kCanonicalRemoteProcessesW) {
        out.push_back(wstringToUtf8(std::wstring(w)));
    }
    return out;
}

std::vector<std::string> WinUtils::defaultRemoteTargetClasses() {
    std::vector<std::string> out;
    out.reserve(std::size(kCanonicalRemoteClassesW));
    for (const auto& w : kCanonicalRemoteClassesW) {
        out.push_back(wstringToUtf8(std::wstring(w)));
    }
    return out;
}

bool WinUtils::isDefaultRemoteDesktopOrVmProcessOrClass(std::wstring_view processName,
                                                        std::wstring_view className) noexcept {
    return isRemoteDesktopOrVmProcessOrClass(processName, className);
}

bool WinUtils::isMediaPlayerProcessOrClass(std::wstring_view processName,
                                           std::wstring_view className) noexcept {
    const std::wstring_view procFile = extractFileNameView(processName);
    if (!procFile.empty()) {
        static constexpr std::wstring_view kMediaProcesses[] = {
            L"potplayermini64.exe", L"potplayermini.exe", L"potplayer64.exe", L"potplayer.exe",
            L"vlc.exe", L"mpv.exe", L"mpvnet.exe", L"ffplay.exe",
            L"mpc-hc.exe", L"mpc-hc64.exe", L"mpc-be.exe", L"mpc-be64.exe",
            L"kmplayer.exe", L"kmplayer64.exe", L"smplayer.exe", L"gom.exe", L"gom64.exe",
            L"wmplayer.exe", L"microsoft.media.player.exe", L"video.ui.exe",
            L"bilibili.exe", L"qiyiclient.exe", L"qqlive.exe", L"youkudesktop.exe",
            L"douyin.exe", L"baofeng.exe", L"xmp.exe", L"qqplayer.exe",
            L"kodi.exe", L"plex.exe", L"plexhtpc.exe", L"jellyfinmediaplayer.exe", L"embytheater.exe",
            L"cloudmusic.exe", L"qqmusic.exe", L"kugou.exe", L"kuwomusic.exe",
            L"spotify.exe", L"foobar2000.exe", L"aimp.exe", L"musicbee.exe",
            L"powerpnt.exe", L"wpp.exe"
        };
        for (const auto& mp : kMediaProcesses) {
            if (equalsIgnoreCaseAscii(procFile, mp)) return true;
        }
    }

    if (!className.empty()) {
        static constexpr std::wstring_view kMediaClasses[] = {
            L"PotPlayer", L"PotPlayer64", L"MediaPlayerClassicW",
            L"mpv", L"VLC DirectX", L"WMPlayerApp", L"screenClass"
        };
        for (const auto& mc : kMediaClasses) {
            if (equalsIgnoreCaseAscii(className, mc)) return true;
        }
    }
    return false;
}

bool WinUtils::isKnownGameProcessOrClass(std::wstring_view processName,
                                         std::wstring_view className,
                                         std::wstring_view exePath) noexcept {
    if (isTools3000UiClassName(className) ||
        className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        className == L"CabinetWClass" || className == L"ExploreWClass") {
        return false;
    }

    if (isMediaPlayerProcessOrClass(processName, className) ||
        isRemoteDesktopOrVmProcessOrClass(processName, className)) {
        return false;
    }

    const std::wstring_view procFile = extractFileNameView(
        !processName.empty() ? processName : exePath);

    if (!procFile.empty()) {
        if (isExplicitCompetitiveOr3DGameProcessName(procFile)) {
            return true;
        }
        if (isNonGameLauncherOrUtilityProcessName(procFile)) {
            return false;
        }
    }

    if (isKnownGameEngineWindowClass(className)) {
        return true;
    }

    if (isGameLibraryExecutablePath(exePath)) {
        return true;
    }

    return false;
}

FullscreenScene WinUtils::classifyFullscreenScene(bool isFullscreen,
                                                  std::wstring_view processName,
                                                  std::wstring_view className,
                                                  std::wstring_view exePath) noexcept {
    if (!isFullscreen) {
        return FullscreenScene::None;
    }

    // 1. Tools3000 自身界面、桌面背景、任务栏与资源管理器全屏：归为生产力场景，绝不屏蔽热键与截图
    if (isTools3000UiClassName(className) ||
        className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        className == L"CabinetWClass" || className == L"ExploreWClass") {
        return FullscreenScene::Productivity;
    }

    const std::wstring_view procFile = extractFileNameView(
        !processName.empty() ? processName : exePath);

    // 2. 专用影音播放器、幻灯片放映与远程桌面/虚拟机全屏会话
    //    (PotPlayer / VLC / MPV / Bilibili / PowerPoint / MSTSC / ToDesk / 向日葵 / VMware 等)
    if (isMediaPlayerProcessOrClass(procFile, className) ||
        isRemoteDesktopOrVmProcessOrClass(procFile, className)) {
        return FullscreenScene::MediaOrBrowser;
    }

    // 3. 已知游戏进程、游戏引擎窗口类名 (SDL_app / Valve001 / UnityWndClass / UnrealWindow) 或 Steam/Epic 游戏库目录
    if (isKnownGameProcessOrClass(procFile, className, exePath)) {
        return FullscreenScene::Game;
    }

    // 4. 生产力 IDE / 办公协作客户端 (VS Code / Cursor / Visual Studio / JetBrains / 微信 / 飞书 / scrcpy 等)
    if (!procFile.empty() && isElectronOrIdeProductivityProcessName(procFile)) {
        return FullscreenScene::Productivity;
    }

    // 5. 网页浏览器全屏 (Chrome / Edge / Firefox / Brave 等观看网页全屏电影或 F11 全屏浏览)
    if ((!procFile.empty() && isWebBrowserProcessName(procFile)) ||
        (procFile.empty() && isWebBrowserClassName(className))) {
        return FullscreenScene::MediaOrBrowser;
    }

    // 6. 其它生产力工具类框架窗口 (Qt / CEF / UWP ApplicationFrameWindow 等)
    if (isProductivityToolkitClassName(className)) {
        return FullscreenScene::Productivity;
    }

    // 7. 其余未知的全屏独占非生产力窗口（如自研引擎全屏 3D 游戏）默认按游戏场景保护
    return FullscreenScene::Game;
}

FullscreenScene WinUtils::classifyWindowScene(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FullscreenScene::None;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;

    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow() ||
        !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return FullscreenScene::None;
    }

    const std::wstring className = getWindowClassName(hwnd);
    if (className == L"Progman" || className == L"WorkerW" ||
        className == L"Shell_TrayWnd" || className == L"Shell_SecondaryTrayWnd" ||
        isTools3000UiClassName(className)) {
        return FullscreenScene::None;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring procName = processNameFromPid(pid);
    const std::wstring procPath = processPathFromPid(pid);

    if (isWindowFullscreen(hwnd)) {
        return classifyFullscreenScene(true, procName, className, procPath);
    }

    // 针对明确已知的大型电竞/3D 游戏进程或安装于游戏库目录下的游戏，
    // 即便玩家采用带边框窗口化运行，只要该游戏窗口处于前台活动态且客户区尺寸 >= 640x480，
    // 同样识别为 Game 场景，自动让出 F1 等游戏高频按键并停用鼠标手势；
    // 注意：非全屏状态下不单凭通用引擎类名（如 SDL_app / GLFW30）判定，避免误伤 scrcpy 或桌面开发工具。
    const std::wstring_view procFile = extractFileNameView(
        !procName.empty() ? std::wstring_view(procName) : std::wstring_view(procPath));
    if (!isMediaPlayerProcessOrClass(procFile, className) &&
        !isRemoteDesktopOrVmProcessOrClass(procFile, className) &&
        !isNonGameLauncherOrUtilityProcessName(procFile) &&
        (isExplicitCompetitiveOr3DGameProcessName(procFile) ||
         isGameLibraryExecutablePath(procPath))) {
        RECT rcClient{};
        if (GetClientRect(hwnd, &rcClient)) {
            const int w = rcClient.right - rcClient.left;
            const int h = rcClient.bottom - rcClient.top;
            if (w >= 640 && h >= 480) {
                return FullscreenScene::Game;
            }
        }
    }

    return FullscreenScene::None;
}

bool WinUtils::shouldSuspendHotkeysForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::Off:
        case FullscreenPolicyMode::GestureOnly:
            return false;
        case FullscreenPolicyMode::StrictAll:
            return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
        case FullscreenPolicyMode::Smart:
        default:
            // 智能模式：游戏内卸下 F1 等全局快捷键；全屏看电影/视频时保留截图、录屏、OCR 等快捷键可用
            return scene == FullscreenScene::Game;
    }
}

bool WinUtils::shouldSuspendGesturesForScene(FullscreenScene scene,
                                             FullscreenPolicyMode mode,
                                             std::wstring_view className) noexcept {
    if (mode == FullscreenPolicyMode::Off) {
        return false;
    }
    if (scene == FullscreenScene::Game) {
        return true;
    }
    if (scene == FullscreenScene::MediaOrBrowser) {
        if (mode == FullscreenPolicyMode::StrictAll) {
            return true;
        }
        // Smart / GestureOnly 模式下：独立影音播放器全屏停用手势（保留原生右键菜单），浏览器全屏保留网页手势
        return !isTabbedBrowserClassName(className);
    }
    return false;
}

bool WinUtils::shouldBypassCaptureForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    switch (mode) {
        case FullscreenPolicyMode::Off:
        case FullscreenPolicyMode::GestureOnly:
            return false;
        case FullscreenPolicyMode::StrictAll:
            return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
        case FullscreenPolicyMode::Smart:
        default:
            // 智能模式：游戏全屏禁用截图防误触（F1 归还游戏），全屏看电影/视频时允许正常截图与录屏
            return scene == FullscreenScene::Game;
    }
}

bool WinUtils::shouldBypassOverlaysForScene(FullscreenScene scene, FullscreenPolicyMode mode) noexcept {
    if (mode == FullscreenPolicyMode::Off) {
        return false;
    }
    // 无论是打游戏还是全屏看电影，均自动静默屏幕四角触发角、按键回显与鼠标聚光灯水波纹
    return scene == FullscreenScene::Game || scene == FullscreenScene::MediaOrBrowser;
}

bool WinUtils::shouldSuspendHotkeysForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldSuspendHotkeysForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::shouldSuspendGesturesForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off || !hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root) hwnd = root;
    const FullscreenScene scene = classifyWindowScene(hwnd);
    const std::wstring className = getWindowClassName(hwnd);
    return shouldSuspendGesturesForScene(scene, mode, className);
}

bool WinUtils::shouldBypassCaptureForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldBypassCaptureForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::shouldBypassOverlaysForWindow(HWND hwnd, FullscreenPolicyMode mode) {
    if (mode == FullscreenPolicyMode::Off) return false;
    return shouldBypassOverlaysForScene(classifyWindowScene(hwnd), mode);
}

bool WinUtils::queryProcessElevated(HANDLE process) {
    if (!process) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD ret = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

bool WinUtils::isCurrentProcessElevated() {
    return queryProcessElevated(GetCurrentProcess());
}

WinUtils::WindowProcessQuery WinUtils::queryWindowProcessAccess(HWND hwnd) {
    WindowProcessQuery q;
    if (!hwnd || !IsWindow(hwnd)) return q;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return q;

    HANDLE limited = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    q.queryLimitedOk = limited != nullptr;

    HANDLE full = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    q.queryInformationOk = full != nullptr;

    HANDLE tokenSource = full ? full : limited;
    if (tokenSource) {
        HANDLE token = nullptr;
        if (OpenProcessToken(tokenSource, TOKEN_QUERY, &token)) {
            q.tokenQueryOk = true;
            TOKEN_ELEVATION elev{};
            DWORD ret = 0;
            if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &ret)) {
                q.tokenElevated = elev.TokenIsElevated != 0;
            }
            CloseHandle(token);
        }
    }
    if (full) CloseHandle(full);
    if (limited) CloseHandle(limited);
    return q;
}

bool WinUtils::isWindowProcessElevated(HWND hwnd) {
    return queryWindowProcessAccess(hwnd).tokenElevated;
}

bool WinUtils::isWindowHigherIntegrity(HWND hwnd) {
    const auto q = queryWindowProcessAccess(hwnd);
    if (!q.queryLimitedOk) return false;
    if (!q.queryInformationOk || !q.tokenQueryOk) {
        return !isCurrentProcessElevated();
    }
    return q.tokenElevated && !isCurrentProcessElevated();
}

namespace {

bool checkExplorerShellViewFocus(HWND foregroundWnd, bool& outIsExplorer, bool& outIsDesktop) noexcept {
    outIsExplorer = false;
    outIsDesktop = false;
    if (!foregroundWnd) return false;

    wchar_t className[256] = {0};
    if (GetClassNameW(foregroundWnd, className, 256) <= 0) return false;
    std::wstring_view cls(className);

    outIsExplorer = (cls == L"CabinetWClass" || cls == L"ExploreWClass");
    outIsDesktop = (cls == L"Progman" || cls == L"WorkerW");
    if (!outIsExplorer && !outIsDesktop) {
        return false;
    }

    // 检查焦点控件是否处于重命名编辑状态或搜索框/地址栏输入状态，防止打字时按空格误触发预览
    DWORD threadId = GetWindowThreadProcessId(foregroundWnd, nullptr);
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus) {
        wchar_t focusClass[256] = {0};
        GetClassNameW(gti.hwndFocus, focusClass, 256);
        if (_wcsicmp(focusClass, L"Edit") == 0 ||
            _wcsicmp(focusClass, L"RichEditD2DPT") == 0 ||
            _wcsicmp(focusClass, L"RichEdit20W") == 0) {
            return false;
        }
        if (outIsExplorer && gti.hwndFocus != foregroundWnd &&
            _wcsicmp(focusClass, L"ShellTabWindowClass") != 0) {
            bool insideShellView = false;
            for (HWND cur = gti.hwndFocus; cur && cur != foregroundWnd; cur = GetParent(cur)) {
                wchar_t parentCls[128] = {0};
                if (GetClassNameW(cur, parentCls, 128) > 0 &&
                    _wcsicmp(parentCls, L"SHELLDLL_DefView") == 0) {
                    insideShellView = true;
                    break;
                }
            }
            if (!insideShellView) {
                return false;
            }
        }
    }
    return true;
}

HWND findActiveExplorerTabHwnd(HWND explorerRoot) noexcept {
    if (!explorerRoot || !IsWindow(explorerRoot)) return nullptr;

    // 1. 优先沿当前焦点控件向上回溯所属 ShellTabWindowClass（Win11 23H2/24H2 多标签页最精准）
    DWORD tid = GetWindowThreadProcessId(explorerRoot, nullptr);
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (tid != 0 && GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) {
        for (HWND cur = gti.hwndFocus; cur && cur != explorerRoot; cur = GetParent(cur)) {
            wchar_t cls[128]{};
            if (GetClassNameW(cur, cls, 128) > 0 && _wcsicmp(cls, L"ShellTabWindowClass") == 0) {
                return cur;
            }
        }
    }

    // 2. 次选遍历直接子窗口寻找可见的 ShellTabWindowClass（避免命中后台隐藏标签页）
    HWND fallbackFirst = nullptr;
    for (HWND tab = FindWindowExW(explorerRoot, nullptr, L"ShellTabWindowClass", nullptr);
         tab != nullptr;
         tab = FindWindowExW(explorerRoot, tab, L"ShellTabWindowClass", nullptr)) {
        if (!fallbackFirst) fallbackFirst = tab;
        if (IsWindowVisible(tab)) {
            return tab;
        }
    }
    return fallbackFirst;
}

std::optional<std::wstring> extractSelectedPathFromFolderView(IFolderView* fv) {
    if (!fv) return std::nullopt;

    auto resolveShellItemPath = [](IShellItem* item) -> std::optional<std::wstring> {
        if (!item) return std::nullopt;
        PWSTR pszPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) || !pszPath) {
            return std::nullopt;
        }
        std::wstring resolved(pszPath);
        CoTaskMemFree(pszPath);
        if (resolved.empty()) return std::nullopt;
        const bool isUnc = resolved.size() >= 2 && resolved[0] == L'\\' && resolved[1] == L'\\';
        if (isUnc) return resolved;
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec)) {
            return resolved;
        }
        return std::nullopt;
    };

    // 1. 首选 IFolderView2::GetSelection：原生支持 Win11 图库(Gallery)/主页(Home)/库(Libraries)/搜索结果及超长路径
    Microsoft::WRL::ComPtr<IFolderView2> fv2;
    if (SUCCEEDED(fv->QueryInterface(IID_PPV_ARGS(&fv2))) && fv2) {
        Microsoft::WRL::ComPtr<IShellItemArray> items;
        if (SUCCEEDED(fv2->GetSelection(FALSE, &items)) && items) {
            DWORD selCount = 0;
            if (SUCCEEDED(items->GetCount(&selCount)) && selCount > 0) {
                Microsoft::WRL::ComPtr<IShellItem> firstItem;
                if (SUCCEEDED(items->GetItemAt(0, &firstItem)) && firstItem) {
                    if (auto path = resolveShellItemPath(firstItem.Get())) {
                        return path;
                    }
                }
            }
        }
    }

    // 2. 次选 IFolderView::Items(SVGIO_SELECTION, IShellItemArray)
    {
        Microsoft::WRL::ComPtr<IShellItemArray> items;
        if (SUCCEEDED(fv->Items(SVGIO_SELECTION, IID_PPV_ARGS(&items))) && items) {
            DWORD selCount = 0;
            if (SUCCEEDED(items->GetCount(&selCount)) && selCount > 0) {
                Microsoft::WRL::ComPtr<IShellItem> firstItem;
                if (SUCCEEDED(items->GetItemAt(0, &firstItem)) && firstItem) {
                    if (auto path = resolveShellItemPath(firstItem.Get())) {
                        return path;
                    }
                }
            }
        }
    }

    // 3. 兜底仅在存在显式选中数 (>0) 时结合焦点项解析，防止点击空白取消选中后误预览首项
    int selectedCount = 0;
    if (FAILED(fv->ItemCount(SVGIO_SELECTION, &selectedCount)) || selectedCount <= 0) {
        return std::nullopt;
    }

    int focusIndex = -1;
    if (FAILED(fv->GetFocusedItem(&focusIndex)) || focusIndex < 0) {
        if (FAILED(fv->GetSelectionMarkedItem(&focusIndex)) || focusIndex < 0) {
            return std::nullopt;
        }
    }

    Microsoft::WRL::ComPtr<IShellFolder> shellFolder;
    if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&shellFolder))) && shellFolder) {
        PITEMID_CHILD pidlItem = nullptr;
        if (SUCCEEDED(fv->Item(focusIndex, &pidlItem)) && pidlItem) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            const HRESULT itemHr = SHCreateItemWithParent(
                nullptr, shellFolder.Get(), pidlItem, IID_PPV_ARGS(&item));
            CoTaskMemFree(pidlItem);
            if (SUCCEEDED(itemHr) && item) {
                if (auto path = resolveShellItemPath(item.Get())) {
                    return path;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool WinUtils::isForegroundExplorerReadyForQuickLook() {
    bool isExplorer = false;
    bool isDesktop = false;
    return checkExplorerShellViewFocus(GetForegroundWindow(), isExplorer, isDesktop);
}

std::optional<std::wstring> WinUtils::getSelectedExplorerFile() {
    HWND foregroundWnd = GetForegroundWindow();
    bool isExplorer = false;
    bool isDesktop = false;
    if (!checkExplorerShellViewFocus(foregroundWnd, isExplorer, isDesktop)) {
        return std::nullopt;
    }

    struct ScopedComApartment {
        HRESULT hr = E_FAIL;
        ScopedComApartment() noexcept {
            hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        }
        ~ScopedComApartment() noexcept {
            if (SUCCEEDED(hr)) {
                CoUninitialize();
            }
        }
    } comGuard;

    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr) || !shellWindows) return std::nullopt;

    // 桌面窗口专属路径：IShellWindows::Item() 不包含桌面，必须走 FindWindowSW(SWC_DESKTOP)
    if (isDesktop) {
        VARIANT vEmpty;
        VariantInit(&vEmpty);
        long lhwnd = 0;
        Microsoft::WRL::ComPtr<IDispatch> desktopDisp;
        hr = shellWindows->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &lhwnd, SWFO_NEEDDISPATCH, &desktopDisp);
        if (SUCCEEDED(hr) && desktopDisp) {
            Microsoft::WRL::ComPtr<IServiceProvider> sp;
            Microsoft::WRL::ComPtr<IShellBrowser> sb;
            Microsoft::WRL::ComPtr<IShellView> sv;
            Microsoft::WRL::ComPtr<IFolderView> fv;
            if (SUCCEEDED(desktopDisp.As(&sp)) && sp &&
                SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) && sb &&
                SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv &&
                SUCCEEDED(sv.As(&fv)) && fv) {
                return extractSelectedPathFromFolderView(fv.Get());
            }
        }
        return std::nullopt;
    }

    const HWND activeTab = findActiveExplorerTabHwnd(foregroundWnd);

    long count = 0;
    shellWindows->get_Count(&count);

    std::optional<std::wstring> fallbackSameWindowResult;
    for (long i = 0; i < count; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        Microsoft::WRL::ComPtr<IDispatch> disp;
        HRESULT itemHr = shellWindows->Item(vi, &disp);
        VariantClear(&vi);
        if (FAILED(itemHr) || !disp) continue;

        Microsoft::WRL::ComPtr<IWebBrowserApp> app;
        if (FAILED(disp.As(&app)) || !app) continue;

        HWND hwnd = nullptr;
        app->get_HWND(reinterpret_cast<SHANDLE_PTR*>(&hwnd));
        if (!hwnd) continue;

        if (hwnd != foregroundWnd && !IsChild(hwnd, foregroundWnd) && !IsChild(foregroundWnd, hwnd)) {
            continue;
        }

        Microsoft::WRL::ComPtr<IServiceProvider> sp;
        if (FAILED(disp.As(&sp)) || !sp) continue;

        Microsoft::WRL::ComPtr<IShellBrowser> sb;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&sb))) || !sb) continue;

        HWND browserHwnd = nullptr;
        (void)sb->GetWindow(&browserHwnd);
        const bool exactTabMatch = (!activeTab) || (browserHwnd == activeTab);
        const bool sameTopWindow = (browserHwnd == foregroundWnd || !browserHwnd);
        if (!exactTabMatch && !sameTopWindow) {
            continue;
        }

        Microsoft::WRL::ComPtr<IShellView> sv;
        if (FAILED(sb->QueryActiveShellView(&sv)) || !sv) continue;

        Microsoft::WRL::ComPtr<IFolderView> fv;
        if (FAILED(sv.As(&fv)) || !fv) continue;

        auto selected = extractSelectedPathFromFolderView(fv.Get());
        if (selected.has_value()) {
            if (exactTabMatch) {
                return selected;
            }
            if (!fallbackSameWindowResult.has_value()) {
                fallbackSameWindowResult = std::move(selected);
            }
        }
    }

    return fallbackSameWindowResult;
}

std::string WinUtils::captureSelectedText() {
    // 仅快照当前剪贴板序列号，绝不预读或回退返回剪贴板中可能存在的密码/隐私内容
    const DWORD seqBefore = GetClipboardSequenceNumber();

    std::vector<INPUT> inputs;
    inputs.reserve(12);
    auto appendKey = [&inputs](WORD vk, DWORD flags) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = flags;
        inputs.push_back(in);
    };

    // 若用户通过组合热键（如 Ctrl+Alt+C / Alt+Shift+*）触发划词，先抬起冲突修饰键，确保目标窗口收到纯净 Ctrl+C。
    // 仅当 Alt 实际处于按下态时，才先行注入无害掩护键 VK_F24 脉冲再释放 Alt，杜绝误激活菜单栏 (SC_KEYMENU) 吞掉 Ctrl+C。
    const bool lAltDown = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
    const bool rAltDown = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    const bool altDown = lAltDown || rAltDown || ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    if (altDown) {
        appendKey(VK_F24, 0);
        appendKey(VK_F24, KEYEVENTF_KEYUP);
        if (lAltDown) appendKey(VK_LMENU, KEYEVENTF_KEYUP);
        if (rAltDown) appendKey(VK_RMENU, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY);
        appendKey(VK_MENU, KEYEVENTF_KEYUP);
    }
    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) appendKey(VK_LSHIFT, KEYEVENTF_KEYUP);
    if ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0) appendKey(VK_RSHIFT, KEYEVENTF_KEYUP);
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) appendKey(VK_SHIFT, KEYEVENTF_KEYUP);
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) appendKey(VK_LWIN, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY);
    if ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) appendKey(VK_RWIN, KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY);

    const bool ctrlAlreadyDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!ctrlAlreadyDown) {
        appendKey(VK_CONTROL, 0);
    }
    appendKey('C', 0);
    appendKey('C', KEYEVENTF_KEYUP);
    if (!ctrlAlreadyDown) {
        appendKey(VK_CONTROL, KEYEVENTF_KEYUP);
    }

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

    // 轮询剪贴板序列号更新（最快 5ms 即刻返回，最长等待 80ms 兼容 Electron/Word）
    bool updated = false;
    for (int step = 0; step < 16; ++step) {
        Sleep(5);
        if (GetClipboardSequenceNumber() != seqBefore) {
            updated = true;
            break;
        }
    }
    if (!updated) {
        return {};
    }

    for (int retry = 0; retry < 5; ++retry) {
        if (OpenClipboard(nullptr)) {
            struct ClipboardGuard {
                ~ClipboardGuard() { CloseClipboard(); }
            } guard;
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
                const SIZE_T maxChars = GlobalSize(hData) / sizeof(wchar_t);
                auto* pszText = (maxChars > 0) ? static_cast<const wchar_t*>(GlobalLock(hData)) : nullptr;
                if (pszText) {
                    const size_t safeLen = wcsnlen(pszText, maxChars);
                    std::wstring safeWide(pszText, safeLen);
                    GlobalUnlock(hData);
                    return wstringToUtf8(safeWide);
                }
            }
            return {};
        }
        Sleep(5);
    }

    return {};
}

std::vector<WinUtils::SystemDriveInfo> WinUtils::getSystemDrives() {
    std::vector<SystemDriveInfo> drives;
    DWORD driveMask = GetLogicalDrives();
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if (!(driveMask & (1u << (letter - 'A')))) continue;

        SystemDriveInfo info;
        info.letter = letter;
        info.path = std::wstring{static_cast<wchar_t>(letter), L':', L'\\'};

        UINT driveType = GetDriveTypeW(info.path.c_str());
        switch (driveType) {
            case DRIVE_FIXED:     info.typeStr = L"fixed"; break;
            case DRIVE_REMOTE:    info.typeStr = L"remote"; break;
            case DRIVE_REMOVABLE: info.typeStr = L"removable"; break;
            case DRIVE_CDROM:     info.typeStr = L"cdrom"; break;
            case DRIVE_RAMDISK:   info.typeStr = L"ramdisk"; break;
            default:              info.typeStr = L"unknown"; break;
        }

        wchar_t volumeName[MAX_PATH + 1] = {};
        wchar_t fileSystemName[MAX_PATH + 1] = {};
        DWORD serialNumber = 0, maxComponentLen = 0, flags = 0;
        if (GetVolumeInformationW(info.path.c_str(), volumeName, MAX_PATH + 1,
                                 &serialNumber, &maxComponentLen, &flags,
                                 fileSystemName, MAX_PATH + 1)) {
            info.volumeLabel = volumeName;
            info.fileSystem = fileSystemName;
        }

        ULARGE_INTEGER freeBytesAvailable, totalNumberOfBytes, totalNumberOfFreeBytes;
        if (GetDiskFreeSpaceExW(info.path.c_str(), &freeBytesAvailable, &totalNumberOfBytes, &totalNumberOfFreeBytes)) {
            info.totalBytes = totalNumberOfBytes.QuadPart;
            info.freeBytes = totalNumberOfFreeBytes.QuadPart;
        }

        drives.push_back(std::move(info));
    }
    return drives;
}

bool WinUtils::openFolderAndSelectItem(const std::wstring& filePath) {
    if (filePath.empty()) return false;

    struct LocateTaskState {
        std::wstring targetPath;
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        bool success = false;
    };

    auto state = std::make_shared<LocateTaskState>();
    state->targetPath = filePath;

    // 全部磁盘属性探测、COM STA 定位及三级降级均封装在共享堆状态的独立 STA 线程内执行，
    // 禁止在调用方为 UI 主线程时无限同步阻塞，且超时 detach 后零栈引用悬垂风险。
    std::thread staThread([state]() {
        const std::wstring& path = state->targetPath;
        bool located = false;

        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("WinUtils: openFolder target no longer exists: {}", wstringToUtf8(path));
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            const auto result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result > 32) {
                located = true;
            } else {
                LOG_WARN("WinUtils: failed to open directory, shellCode={}, path={}",
                         result, wstringToUtf8(path));
            }
        } else {
            HRESULT parseResult = E_FAIL;
            HRESULT selectResult = E_FAIL;
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            PIDLIST_ABSOLUTE itemId = nullptr;
            parseResult = SHParseDisplayName(path.c_str(), nullptr, &itemId, 0, nullptr);
            if (SUCCEEDED(parseResult) && itemId) {
                selectResult = SHOpenFolderAndSelectItems(itemId, 0, nullptr, 0);
                CoTaskMemFree(itemId);
                if (SUCCEEDED(selectResult)) {
                    located = true;
                }
            }
            if (SUCCEEDED(comResult)) CoUninitialize();

            if (!located) {
                const std::wstring arguments = L"/select,\"" + path + L"\"";
                const auto explorerResult = reinterpret_cast<INT_PTR>(
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
                if (explorerResult > 32) {
                    located = true;
                } else {
                    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
                    if (!parent.empty()) {
                        const auto parentResult = reinterpret_cast<INT_PTR>(
                            ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                        if (parentResult > 32) {
                            located = true;
                        }
                    }
                    if (!located) {
                        LOG_WARN("WinUtils: failed to locate item, parseHr=0x{:08X}, selectHr=0x{:08X}, shellCode={}, path={}",
                                 static_cast<unsigned>(parseResult), static_cast<unsigned>(selectResult), explorerResult,
                                 wstringToUtf8(path));
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->success = located;
            state->finished = true;
        }
        state->cv.notify_all();
    });

    const bool onUiThread = MainThreadDispatcher::instance().isOwnerThread();
    const auto waitBudget = onUiThread
        ? std::chrono::milliseconds(180)
        : std::chrono::milliseconds(2500);

    bool completedInTime = false;
    bool ok = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        completedInTime = state->cv.wait_for(lock, waitBudget, [&state]() { return state->finished; });
        if (completedInTime) {
            ok = state->success;
        }
    }

    if (completedInTime) {
        if (staThread.joinable()) staThread.join();
        return ok;
    }

    // 慢速机械盘唤醒或离线 SMB 网络盘超时：立即脱离等待，由后台 STA 线程完成剩余定位，严禁冻结消息泵
    if (staThread.joinable()) {
        staThread.detach();
    }
    LOG_WARN("WinUtils: openFolderAndSelectItem exceeded {}ms wait budget on {} thread; detached background STA worker for {}",
             waitBudget.count(), onUiThread ? "UI" : "worker", wstringToUtf8(filePath));
    return true;
}

namespace {

// 微软官方桌面 Explorer 降权启动协议（Raymond Chen / PowerToys Run 标配）：
// 当 Tools3000 以管理员 (High IL) 运行时，直接 ShellExecuteW 拉起 Chrome/Edge/桌面应用会让子进程继承
// 管理员令牌与 Job Object，引发已有普通权限 Chrome 单实例死锁、沙箱异常或拖放失效。
// 委托给桌面 explorer.exe (Medium IL) 的 IShellDispatch2::ShellExecute 可彻底避免高权限传染与阻塞。
bool tryShellExecuteViaDesktopExplorer(const std::wstring& target,
                                       const std::wstring& args,
                                       const std::wstring& workingDir,
                                       const std::wstring& verb,
                                       int showCmd) noexcept {
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&shellWindows))) || !shellWindows) {
        return false;
    }

    VARIANT vEmpty{};
    VariantInit(&vEmpty);
    long hwndDesktop = 0;
    Microsoft::WRL::ComPtr<IDispatch> dispDesktop;
    if (FAILED(shellWindows->FindWindowSW(&vEmpty, &vEmpty, SWC_DESKTOP, &hwndDesktop,
                                          SWFO_NEEDDISPATCH, &dispDesktop)) || !dispDesktop) {
        return false;
    }

    Microsoft::WRL::ComPtr<IServiceProvider> serviceProvider;
    if (FAILED(dispDesktop.As(&serviceProvider)) || !serviceProvider) {
        return false;
    }

    Microsoft::WRL::ComPtr<IShellBrowser> shellBrowser;
    if (FAILED(serviceProvider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))) ||
        !shellBrowser) {
        return false;
    }

    Microsoft::WRL::ComPtr<IShellView> shellView;
    if (FAILED(shellBrowser->QueryActiveShellView(&shellView)) || !shellView) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDispatch> dispView;
    if (FAILED(shellView->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&dispView))) || !dispView) {
        return false;
    }

    Microsoft::WRL::ComPtr<IShellFolderViewDual> folderView;
    if (FAILED(dispView.As(&folderView)) || !folderView) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDispatch> dispApp;
    if (FAILED(folderView->get_Application(&dispApp)) || !dispApp) {
        return false;
    }

    Microsoft::WRL::ComPtr<IShellDispatch2> shellDispatch;
    if (FAILED(dispApp.As(&shellDispatch)) || !shellDispatch) {
        return false;
    }

    BSTR bstrFile = SysAllocString(target.c_str());
    if (!bstrFile) return false;

    VARIANT vArgs{};
    VARIANT vDir{};
    VARIANT vVerb{};
    VARIANT vShow{};
    VariantInit(&vArgs);
    VariantInit(&vDir);
    VariantInit(&vVerb);
    VariantInit(&vShow);

    if (!args.empty()) {
        vArgs.vt = VT_BSTR;
        vArgs.bstrVal = SysAllocString(args.c_str());
    }
    if (!workingDir.empty()) {
        vDir.vt = VT_BSTR;
        vDir.bstrVal = SysAllocString(workingDir.c_str());
    }
    if (!verb.empty()) {
        vVerb.vt = VT_BSTR;
        vVerb.bstrVal = SysAllocString(verb.c_str());
    }
    vShow.vt = VT_I4;
    vShow.lVal = showCmd;

    const HRESULT hr = shellDispatch->ShellExecute(bstrFile, vArgs, vDir, vVerb, vShow);

    SysFreeString(bstrFile);
    VariantClear(&vArgs);
    VariantClear(&vDir);
    VariantClear(&vVerb);
    VariantClear(&vShow);
    return SUCCEEDED(hr);
}

bool launchShellTargetBoundedSta(const std::wstring& target,
                                 const std::wstring& args,
                                 const std::wstring& workingDir,
                                 const std::wstring& verb,
                                 bool preferUnelevated,
                                 bool suppressUiErrors) {
    struct LaunchState {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        bool success = false;
        DWORD lastError = ERROR_SUCCESS;
    };

    auto state = std::make_shared<LaunchState>();
    const bool useExplorerDeElevation = preferUnelevated && WinUtils::isCurrentProcessElevated();

    std::thread staThread([state, target, args, workingDir, verb, useExplorerDeElevation, suppressUiErrors]() {
        ScopedStaComGuard comGuard;
        bool launched = false;
        DWORD err = ERROR_SUCCESS;

        if (useExplorerDeElevation) {
            launched = tryShellExecuteViaDesktopExplorer(target, args, workingDir, verb, SW_SHOWNORMAL);
        }

        if (!launched) {
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_ASYNCOK | (suppressUiErrors ? SEE_MASK_FLAG_NO_UI : 0u);
            sei.lpVerb = verb.empty() ? nullptr : verb.c_str();
            sei.lpFile = target.c_str();
            sei.lpParameters = args.empty() ? nullptr : args.c_str();
            sei.lpDirectory = workingDir.empty() ? nullptr : workingDir.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (ShellExecuteExW(&sei) != FALSE) {
                launched = true;
            } else {
                err = GetLastError();
            }
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->success = launched;
            state->lastError = err;
            state->finished = true;
        }
        state->cv.notify_all();
    });

    const bool onUiThread = MainThreadDispatcher::instance().isOwnerThread();
    const auto waitBudget = onUiThread
        ? std::chrono::milliseconds(180)
        : std::chrono::milliseconds(1500);

    bool completedInTime = false;
    bool ok = false;
    DWORD err = ERROR_SUCCESS;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        completedInTime = state->cv.wait_for(lock, waitBudget, [&state]() { return state->finished; });
        if (completedInTime) {
            ok = state->success;
            err = state->lastError;
        }
    }

    if (completedInTime) {
        if (staThread.joinable()) staThread.join();
        if (!ok) {
            LOG_WARN("WinUtils: shell launch failed, error={}, target={}",
                     err, WinUtils::wstringToUtf8(target));
        }
        return ok;
    }

    if (staThread.joinable()) {
        staThread.detach();
    }
    LOG_WARN("WinUtils: shell launch exceeded {}ms wait budget on {} thread; detached STA worker for {}",
             waitBudget.count(), onUiThread ? "UI" : "worker", WinUtils::wstringToUtf8(target));
    return true;
}

}  // namespace

bool WinUtils::openProgram(const std::wstring& filePath,
                           const std::wstring& args,
                           const std::wstring& workingDir) {
    if (filePath.empty()) return false;
    const auto first = filePath.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = filePath.find_last_not_of(L" \t\r\n");
    const std::wstring trimmed = filePath.substr(first, last - first + 1);

    if (startsWithAsciiCaseInsensitiveW(trimmed, L"http://") ||
        startsWithAsciiCaseInsensitiveW(trimmed, L"https://")) {
        return openUrl(trimmed);
    }

    const bool isShellUri = startsWithAsciiCaseInsensitiveW(trimmed, L"shell:");
    if (!isShellUri) {
        // 仅对含路径分隔符或盘符的显式本地路径做存在性预检；裸程序名（如 notepad.exe）交由 Shell/PATH 解析
        const bool looksExplicitPath =
            trimmed.find(L'\\') != std::wstring::npos ||
            trimmed.find(L'/') != std::wstring::npos ||
            (trimmed.size() >= 2 && trimmed[1] == L':');
        if (looksExplicitPath && GetFileAttributesW(trimmed.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("WinUtils: openProgram target no longer exists: {}", wstringToUtf8(trimmed));
            return false;
        }
    }

    std::wstring effectiveDir = workingDir;
    if (effectiveDir.empty() && !isShellUri) {
        std::error_code ec;
        const std::filesystem::path p(trimmed);
        if (!std::filesystem::is_directory(p, ec) && p.has_parent_path()) {
            const auto parent = p.parent_path();
            if (!parent.empty() && std::filesystem::is_directory(parent, ec)) {
                effectiveDir = parent.wstring();
            }
        }
    }

    return launchShellTargetBoundedSta(
        trimmed,
        args,
        effectiveDir,
        isShellUri ? L"" : L"open",
        /*preferUnelevated=*/true,
        /*suppressUiErrors=*/true);
}

bool WinUtils::openFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const auto first = filePath.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = filePath.find_last_not_of(L" \t\r\n");
    const std::wstring trimmed = filePath.substr(first, last - first + 1);

    if (startsWithAsciiCaseInsensitiveW(trimmed, L"http://") ||
        startsWithAsciiCaseInsensitiveW(trimmed, L"https://")) {
        return openUrl(trimmed);
    }
    if (!startsWithAsciiCaseInsensitiveW(trimmed, L"shell:") &&
        GetFileAttributesW(trimmed.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_WARN("WinUtils: openFile target no longer exists: {}", wstringToUtf8(trimmed));
        return false;
    }
    return openProgram(trimmed);
}

bool WinUtils::openUrl(const std::wstring& url) {
    if (url.empty()) return false;
    const auto first = url.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = url.find_last_not_of(L" \t\r\n");
    const std::wstring trimmed = url.substr(first, last - first + 1);

    if (!startsWithAsciiCaseInsensitiveW(trimmed, L"http://") &&
        !startsWithAsciiCaseInsensitiveW(trimmed, L"https://")) {
        LOG_WARN("WinUtils: openUrl rejected non-http(s) scheme: {}", wstringToUtf8(trimmed));
        return false;
    }
    if (trimmed == L"http://" || trimmed == L"https://") {
        LOG_WARN("WinUtils: openUrl rejected incomplete scheme: {}", wstringToUtf8(trimmed));
        return false;
    }
    return launchShellTargetBoundedSta(
        trimmed,
        L"",
        L"",
        L"open",
        /*preferUnelevated=*/true,
        /*suppressUiErrors=*/true);
}

bool WinUtils::openWithNotepad(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    const std::wstring args = L"\"" + filePath + L"\"";
    return launchShellTargetBoundedSta(
        L"notepad.exe",
        args,
        L"",
        L"open",
        /*preferUnelevated=*/true,
        /*suppressUiErrors=*/true);
}

bool WinUtils::openFileAsAdmin(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    return launchShellTargetBoundedSta(
        filePath,
        L"",
        L"",
        L"runas",
        /*preferUnelevated=*/false,
        /*suppressUiErrors=*/false);
}

bool WinUtils::showFileProperties(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    if (GetFileAttributesW(filePath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const HRESULT oleResult = OleInitialize(nullptr);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"properties";
    info.lpFile = filePath.c_str();
    info.nShow = SW_SHOWNORMAL;
    const bool success = ShellExecuteExW(&info) != FALSE;
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (oleResult == S_OK || oleResult == S_FALSE) OleUninitialize();
    if (!success) {
        LOG_WARN("WinUtils: show properties failed, error={}, path={}", error, wstringToUtf8(filePath));
    }
    return success;
}

bool WinUtils::isSystemTaskbarDark() {
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    // 读取 Windows 10 (1903+) / Windows 11 系统任务栏主题设置 (0: Dark, 1: Light)
    LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0); // 0 为深色任务栏，1 为浅色任务栏
    }

    // 若不存在，尝试读取应用级主题 AppsUseLightTheme 回退
    res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0);
    }

    // 默认回退深色
    return true;
}

bool WinUtils::isSystemDarkMode() {
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &data,
        &dataSize
    );
    if (res == ERROR_SUCCESS) {
        return (data == 0);
    }
    return isSystemTaskbarDark();
}

void WinUtils::applyNativeMenuTheme(std::string_view themeMode) {
    std::string resolvedMode(themeMode);
    if (resolvedMode.empty()) {
        resolvedMode = ConfigManager::instance().get<std::string>("/general/theme", "system");
    }

    // uxtheme.dll undocumented PreferredAppMode enum (Windows 10 1903+ / Windows 11):
    // Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3
    int preferredAppMode = 1; // AllowDark
    if (resolvedMode == "dark") {
        preferredAppMode = 2; // ForceDark
    } else if (resolvedMode == "light") {
        preferredAppMode = 3; // ForceLight
    }

    using PFN_SetPreferredAppMode = int(WINAPI*)(int);
    using PFN_FlushMenuThemes = void(WINAPI*)();

    static PFN_SetPreferredAppMode s_setPreferredAppMode = nullptr;
    static PFN_FlushMenuThemes s_flushMenuThemes = nullptr;
    static std::once_flag s_uxthemeResolveOnce;
    std::call_once(s_uxthemeResolveOnce, []() {
        HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!hUxtheme) {
            hUxtheme = GetModuleHandleW(L"uxtheme.dll");
        }
        if (hUxtheme) {
            s_setPreferredAppMode = reinterpret_cast<PFN_SetPreferredAppMode>(
                GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
            s_flushMenuThemes = reinterpret_cast<PFN_FlushMenuThemes>(
                GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136)));
        }
    });

    if (s_setPreferredAppMode) {
        s_setPreferredAppMode(preferredAppMode);
    }
    if (s_flushMenuThemes) {
        s_flushMenuThemes();
    }
}

HANDLE WinUtils::getProcessJobObject() {
    static HANDLE s_job = []() -> HANDLE {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return nullptr;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            CloseHandle(job);
            return nullptr;
        }
        // 将当前主进程绑定到 Job Object
        AssignProcessToJobObject(job, GetCurrentProcess());
        return job;
    }();
    return s_job;
}

bool WinUtils::assignProcessToCurrentJob(HANDLE hProcess) {
    if (!hProcess) return false;
    HANDLE job = getProcessJobObject();
    if (!job) return false;
    return AssignProcessToJobObject(job, hProcess) != FALSE;
}

bool WinUtils::flushFileToPhysicalDisk(HANDLE hFile) {
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return false;
    return FlushFileBuffers(hFile) != FALSE;
}

bool WinUtils::atomicWriteFileWithFlush(const std::wstring& targetPath, const std::string& data) {
    return tools3000::common::atomicWriteFileWithFlush(targetPath, data);
}

HANDLE WinUtils::createLowMemoryNotification() {
    return CreateMemoryResourceNotification(LowMemoryResourceNotification);
}

HWND WinUtils::createOverlayHelperOwner(HINSTANCE hInstance, const wchar_t* name) {
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC",
        name ? name : L"Tools3000_OverlayHelperOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );
    if (hwnd) {
        SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    }
    return hwnd;
}

void WinUtils::applyTaskbarSafeOverlayStyle(HWND hwnd, bool excludeFromCapture) {
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    constexpr LONG_PTR required = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                  WS_EX_TOOLWINDOW;
    exStyle = (exStyle | required) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    // 显式标记 NonRudeHWND 属性，阻止 explorer.exe (CRudeAppManager) 将全屏覆盖层误判为全屏独占应用并压低任务栏 Z 序
    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &noCorners, sizeof(noCorners));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions, sizeof(disableTransitions));
    (void)excludeFromCapture;
    allowWindowCapture(hwnd);
}

bool WinUtils::isWindows11OrNewer() noexcept {
    static const bool cached = []() noexcept {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0) {
                    return info.dwMajorVersion > 10 ||
                           (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
                }
            }
        }
        return false;
    }();
    return cached;
}

bool WinUtils::isWindows1122H2OrNewer() noexcept {
    static const bool cached = []() noexcept {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0) {
                    return info.dwMajorVersion > 10 ||
                           (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22621);
                }
            }
        }
        return false;
    }();
    return cached;
}

void WinUtils::applyImmersiveDarkMode(HWND hwnd, bool dark) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmwaUseImmersiveDarkModeBefore20H1 = 19;
    const BOOL flag = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &flag, sizeof(flag)))) {
        (void)DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkModeBefore20H1, &flag, sizeof(flag));
    }
}

void WinUtils::applyWin11SystemBackdrop(HWND hwnd, DWORD backdropType) noexcept {
    if (!hwnd || !IsWindow(hwnd) || !isWindows1122H2OrNewer()) return;
    constexpr DWORD kDwmwaSystemBackdropType = 38;
    (void)DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdropType, sizeof(backdropType));
}

bool WinUtils::shouldUseRegionFallbackForRoundedCorners(
    DWORD windowStyle,
    bool isWin11,
    bool dwmCompositionEnabled,
    HRESULT dwmCornerHr) noexcept {
    const bool hasCaptionFrame = (windowStyle & WS_CAPTION) == WS_CAPTION;
    if (hasCaptionFrame && isWin11 && dwmCompositionEnabled && SUCCEEDED(dwmCornerHr)) {
        return false;
    }
    return true;
}

void WinUtils::clearWindowRegionIfSet(HWND hwnd, bool redraw) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    RECT rgnBox{};
    if (GetWindowRgnBox(hwnd, &rgnBox) != ERROR) {
        SetWindowRgn(hwnd, nullptr, redraw ? TRUE : FALSE);
    }
}

bool WinUtils::handleFramelessNonClientMessage(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT& outResult) noexcept {
    (void)wParam;
    (void)lParam;
    if (!hwnd || !IsWindow(hwnd)) return false;

    switch (msg) {
        case WM_NCACTIVATE:
            // 最小化态交由系统默认维护；非最小化态直接返回 TRUE 允许激活/失焦，
            // 彻底阻断 DefWindowProcW 与 UxTheme 在非客户区绘制 Win7/Win98 旧式标题栏。
            if (IsIconic(hwnd)) {
                return false;
            }
            outResult = TRUE;
            return true;
        case WM_NCPAINT: {
            if (IsIconic(hwnd)) {
                return false;
            }
            // 关键 Windows DWM 铁律：当 DWMWA_NCRENDERING_ENABLED 为 TRUE 时（如 Win11 纯硬件圆角态），
            // 若截断 WM_NCPAINT 不交给 DefWindowProcW，内核会判定应用自绘非客户区并永久将 DWM 非客户区渲染
            // 降级关闭（从 1 -> 0），进而触发 uxtheme 强行附加 Win7 Basic 3px 顶角 RGN。
            // 此时 WM_NCCALCSIZE 已将非客户区归零，放行 DefWindowProcW 既零旧标题栏残留又完整保留原生 DWM 阴影；
            // 仅在非 DWM 合成或 Win10/Server RGN 降级态（ncEnabled == FALSE）时拦截返回 0 封死 GDI 旧框。
            BOOL ncEnabled = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(
                    hwnd, DWMWA_NCRENDERING_ENABLED, &ncEnabled, sizeof(ncEnabled))) &&
                ncEnabled != FALSE) {
                return false;
            }
            outResult = 0;
            return true;
        }
        case WM_DWMCOMPOSITIONCHANGED: {
            MARGINS margins = {1, 1, 1, 1};
            DwmExtendFrameIntoClientArea(hwnd, &margins);
            outResult = 0;
            return true;
        }
        default:
            return false;
    }
}

void WinUtils::applyUniversalRoundedCorners(HWND hwnd, int width, int height, int radius) {
    if (!hwnd || !IsWindow(hwnd) || width <= 0 || height <= 0) return;

    // radius <= 0 表示显式要求直角态（例如最大化铺满屏幕）：必须主动切换 DWMWCP_DONOTROUND
    // 并清除此前普通态遗留的 SetWindowRgn 区域，防止全屏窗口被旧的小尺寸 HRGN 截断！
    if (radius <= 0) {
        const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &noCorners, sizeof(noCorners));
        clearWindowRegionIfSet(hwnd, true);
        return;
    }

    // 1. Windows 11 DWM 硬件级超平滑圆角首选
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    const HRESULT dwmHr = DwmSetWindowAttribute(
        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    BOOL dwmEnabled = FALSE;
    const bool compositionEnabled =
        SUCCEEDED(DwmIsCompositionEnabled(&dwmEnabled)) && (dwmEnabled != FALSE);
    const DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));

    if (!shouldUseRegionFallbackForRoundedCorners(
            style, isWindows11OrNewer(), compositionEnabled, dwmHr)) {
        // Win11 下带 WS_CAPTION 的标准无边框应用窗口由 DWM 硬件圆角与原生高斯阴影全权托管，
        // 严禁叠加 SetWindowRgn 触发系统降级绘制 Win7/Win98 旧式标题栏。
        clearWindowRegionIfSet(hwnd, false);
        return;
    }

    // 2. Win32 硬件级 RGN 裁剪兜底 (确保在 Win10、Windows Server 2019/2022/2025、RDP 与自定义大圆角浮层下 100% 绝对圆角)
    const HRGN hRgn = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (hRgn) {
        const BOOL redraw = ((style & WS_CAPTION) == WS_CAPTION) ? FALSE : TRUE;
        SetWindowRgn(hwnd, hRgn, redraw);
        // SetWindowRgn 成功后系统接管 hRgn 句柄的所有权，无需手动 DeleteObject
    }
}

bool WinUtils::setWindowAppUserModelId(HWND hwnd, const std::wstring& aumid) {
    if (!hwnd || !IsWindow(hwnd) || aumid.empty()) return false;
    IPropertyStore* pps = nullptr;
    HRESULT hr = SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&pps));
    if (SUCCEEDED(hr) && pps) {
        PROPVARIANT pv;
        InitPropVariantFromString(aumid.c_str(), &pv);
        hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
        if (SUCCEEDED(hr)) {
            pps->Commit();
        }
        PropVariantClear(&pv);
        pps->Release();
        return SUCCEEDED(hr);
    }
    return false;
}

bool WinUtils::setShortcutAppUserModelId(const std::wstring& shortcutPath, const std::wstring& aumid) {
    if (shortcutPath.empty() || aumid.empty()) return false;
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (FAILED(hr) || !psl) return false;

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
    if (SUCCEEDED(hr) && ppf) {
        hr = ppf->Load(shortcutPath.c_str(), STGM_READWRITE);
        if (SUCCEEDED(hr)) {
            IPropertyStore* pps = nullptr;
            hr = psl->QueryInterface(IID_PPV_ARGS(&pps));
            if (SUCCEEDED(hr) && pps) {
                PROPVARIANT pv;
                InitPropVariantFromString(aumid.c_str(), &pv);
                hr = pps->SetValue(PKEY_AppUserModel_ID, pv);
                if (SUCCEEDED(hr)) {
                    pps->Commit();
                }
                PropVariantClear(&pv);
                pps->Release();
                if (SUCCEEDED(hr)) {
                    hr = ppf->Save(shortcutPath.c_str(), TRUE);
                }
            }
        }
        ppf->Release();
    }
    psl->Release();
    return SUCCEEDED(hr);
}

bool WinUtils::syncSingleShortcutMetadata(const std::wstring& shortcutPath, const std::wstring& aumid, bool forceRefresh) {
    if (shortcutPath.empty() || aumid.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(shortcutPath, ec) || ec) return false;

    Microsoft::WRL::ComPtr<IShellLinkW> pslRead;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pslRead))) || !pslRead) {
        return false;
    }
    Microsoft::WRL::ComPtr<IPersistFile> ppfRead;
    if (FAILED(pslRead.As(&ppfRead)) || !ppfRead) {
        return false;
    }
    if (FAILED(ppfRead->Load(shortcutPath.c_str(), STGM_READ))) {
        return false;
    }

    wchar_t targetBuf[MAX_PATH]{};
    pslRead->GetPath(targetBuf, MAX_PATH, nullptr, 0);
    const std::wstring targetPath(targetBuf);

    wchar_t iconBuf[MAX_PATH]{};
    int iconIdx = 0;
    pslRead->GetIconLocation(iconBuf, MAX_PATH, &iconIdx);

    std::wstring existingAumid;
    Microsoft::WRL::ComPtr<IPropertyStore> ppsRead;
    if (SUCCEEDED(pslRead.As(&ppsRead)) && ppsRead) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ppsRead->GetValue(PKEY_AppUserModel_ID, &pv))) {
            if ((pv.vt == VT_LPWSTR || pv.vt == VT_BSTR) && pv.pwszVal != nullptr) {
                existingAumid = pv.pwszVal;
            }
        }
        PropVariantClear(&pv);
    }

    // 图标定位严格锚定该快捷方式自身的有效 TargetPath，严禁开发构建目录 (deploy_dist) 或临时测试目录跨路径篡改已安装快捷方式
    std::wstring desiredIconPath;
    if (!targetPath.empty() && std::filesystem::exists(targetPath, ec) && !ec) {
        desiredIconPath = targetPath;
    }

    const bool needAumid = (existingAumid != aumid);
    const bool needIcon = !desiredIconPath.empty() &&
        (forceRefresh || iconIdx != 0 || _wcsicmp(iconBuf, desiredIconPath.c_str()) != 0);

    if (!needAumid && !needIcon) {
        return false;
    }

    // 释放只读句柄后再以读写模式打开并原子落盘
    ppsRead.Reset();
    ppfRead.Reset();
    pslRead.Reset();

    Microsoft::WRL::ComPtr<IShellLinkW> pslWrite;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pslWrite))) || !pslWrite) {
        return false;
    }
    Microsoft::WRL::ComPtr<IPersistFile> ppfWrite;
    if (FAILED(pslWrite.As(&ppfWrite)) || !ppfWrite) {
        return false;
    }
    if (FAILED(ppfWrite->Load(shortcutPath.c_str(), STGM_READWRITE))) {
        return false;
    }

    if (needAumid) {
        Microsoft::WRL::ComPtr<IPropertyStore> ppsWrite;
        if (SUCCEEDED(pslWrite.As(&ppsWrite)) && ppsWrite) {
            PROPVARIANT pv;
            if (SUCCEEDED(InitPropVariantFromString(aumid.c_str(), &pv))) {
                if (SUCCEEDED(ppsWrite->SetValue(PKEY_AppUserModel_ID, pv))) {
                    ppsWrite->Commit();
                }
                PropVariantClear(&pv);
            }
        }
    }

    if (needIcon) {
        pslWrite->SetIconLocation(desiredIconPath.c_str(), 0);
    }

    if (FAILED(ppfWrite->Save(shortcutPath.c_str(), TRUE))) {
        return false;
    }

    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, shortcutPath.c_str(), nullptr);
    return true;
}

void WinUtils::syncApplicationShortcuts(bool forceRefresh) {
    // 便携版严格保持零宿主快捷方式副作用，防止临时解压目录污染任务栏/开始菜单图标定位
    if (!forceRefresh && isPortableMode()) {
        return;
    }

    const std::wstring aumid = L"Yy1.Tools3000";
    bool anyUpdated = false;

    // 1. 获取用户及公共目录下的开始菜单与桌面路径
    const int csidls[] = {
        CSIDL_PROGRAMS,
        CSIDL_COMMON_PROGRAMS,
        CSIDL_DESKTOPDIRECTORY,
        CSIDL_COMMON_DESKTOPDIRECTORY
    };

    for (int csidl : csidls) {
        wchar_t folderPath[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, folderPath))) {
            const std::filesystem::path baseDir(folderPath);

            std::filesystem::path toolsLnk = (csidl == CSIDL_PROGRAMS || csidl == CSIDL_COMMON_PROGRAMS)
                ? (baseDir / L"Tools3000" / L"Tools3000.lnk")
                : (baseDir / L"Tools3000.lnk");

            std::error_code ec;
            if (std::filesystem::exists(toolsLnk, ec) && !ec) {
                if (syncSingleShortcutMetadata(toolsLnk.wstring(), aumid, forceRefresh)) {
                    anyUpdated = true;
                }
            }
        }
    }

    // 2. 同步刷新用户已固定在任务栏上的 Tools3000.lnk 快捷方式图标
    wchar_t appDataPath[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appDataPath))) {
        const std::filesystem::path pinnedTaskBarLnk =
            std::filesystem::path(appDataPath) / L"Microsoft" / L"Internet Explorer" /
            L"Quick Launch" / L"User Pinned" / L"TaskBar" / L"Tools3000.lnk";
        std::error_code ec;
        if (std::filesystem::exists(pinnedTaskBarLnk, ec) && !ec) {
            if (syncSingleShortcutMetadata(pinnedTaskBarLnk.wstring(), aumid, forceRefresh)) {
                anyUpdated = true;
            }
        }
    }

    // 仅在实际修复了快捷方式或显式强制刷新 (--update-shortcuts) 时通知 Shell 重刷图标缓存，消除冷启动桌面闪烁
    if (anyUpdated || forceRefresh) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

void WinUtils::emergencyFlushInputState() noexcept {
    // 1. 如果当前进程任意窗口持有鼠标捕获，立即强制释放
    if (GetCapture() != nullptr) {
        ReleaseCapture();
    }
    // 2. 解除可能存在的光标区域限制
    ClipCursor(nullptr);

    // 3. 释放可能被卡住的修饰键 (Win, Ctrl, Shift, Alt)
    std::vector<INPUT> inputs;
    inputs.reserve(10);

    auto addKeyUp = [&](WORD vk, bool extended = false) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inp.ki.dwFlags = KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(inp);
    };

    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) addKeyUp(VK_LWIN, true);
    if ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) addKeyUp(VK_RWIN, true);
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) addKeyUp(VK_CONTROL, false);
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) addKeyUp(VK_SHIFT, false);

    // 针对 Alt 键 (VK_MENU)：若处于逻辑按下态，绝不能单独发裸 Alt KeyUp，
    // 否则 Windows 会立即判定为单按 Alt 并触发前台窗口 (尤其是 Explorer) 激活系统菜单栏 SC_KEYMENU，
    // 从而锁死文件拖拽和 OLE DragDrop。
    // 正确方案：先行注入中立无害按键脉冲 (VK_F24 Down + Up)，彻底消除 SC_KEYMENU 判定，再安全释放 Alt。
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        INPUT neutralDown{};
        neutralDown.type = INPUT_KEYBOARD;
        neutralDown.ki.wVk = VK_F24;
        inputs.push_back(neutralDown);

        INPUT neutralUp{};
        neutralUp.type = INPUT_KEYBOARD;
        neutralUp.ki.wVk = VK_F24;
        neutralUp.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(neutralUp);

        addKeyUp(VK_MENU, false);
    }

    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

bool WinUtils::isProcessElevated() {
    return isCurrentProcessElevated();
}

bool WinUtils::hasValidAuthenticodeSignature(const std::filesystem::path& filePath) {
    if (filePath.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec) || ec) return false;

    const std::wstring widePath = filePath.wstring();
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = widePath.c_str();

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    const LONG status = WinVerifyTrust(
        static_cast<HWND>(INVALID_HANDLE_VALUE), &policyGuid, &trustData);

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policyGuid, &trustData);

    return status == ERROR_SUCCESS;
}

namespace {

size_t utf8ValidSequenceLength(std::string_view input, size_t pos) noexcept {
    if (pos >= input.size()) return 0;
    const unsigned char b0 = static_cast<unsigned char>(input[pos]);
    if (b0 < 0x80u) return 1;
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (pos + 1 < input.size()) {
            const unsigned char b1 = static_cast<unsigned char>(input[pos + 1]);
            if ((b1 & 0xC0u) == 0x80u) return 2;
        }
        return 0;
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (pos + 2 < input.size()) {
            const unsigned char b1 = static_cast<unsigned char>(input[pos + 1]);
            const unsigned char b2 = static_cast<unsigned char>(input[pos + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                // 排除 overlong (E0 80..9F) 与 UTF-16 代理项 (ED A0..BF)
                if (b0 == 0xE0u && b1 < 0xA0u) return 0;
                if (b0 == 0xEDu && b1 >= 0xA0u) return 0;
                return 3;
            }
        }
        return 0;
    }
    if (b0 >= 0xF0u && b0 <= 0xF4u) {
        if (pos + 3 < input.size()) {
            const unsigned char b1 = static_cast<unsigned char>(input[pos + 1]);
            const unsigned char b2 = static_cast<unsigned char>(input[pos + 2]);
            const unsigned char b3 = static_cast<unsigned char>(input[pos + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                // 排除 overlong (F0 80..8F) 与超出 U+10FFFF (F4 90..BF)
                if (b0 == 0xF0u && b1 < 0x90u) return 0;
                if (b0 == 0xF4u && b1 >= 0x90u) return 0;
                return 4;
            }
        }
        return 0;
    }
    return 0;
}

}  // namespace

std::string WinUtils::sanitizeUtf8(std::string_view input) {
    if (input.empty()) return {};
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        const size_t seqLen = utf8ValidSequenceLength(input, i);
        if (seqLen > 0) {
            out.append(input.data() + i, seqLen);
            i += seqLen;
        } else {
            out.append("\xEF\xBF\xBD", 3);
            ++i;
        }
    }
    return out;
}

std::string WinUtils::truncateUtf8Head(std::string_view input, size_t maxBytes) {
    if (input.empty() || maxBytes == 0) return {};
    std::string out;
    out.reserve((std::min)(input.size(), maxBytes));
    size_t i = 0;
    while (i < input.size()) {
        const size_t seqLen = utf8ValidSequenceLength(input, i);
        if (seqLen > 0) {
            if (out.size() + seqLen > maxBytes) break;
            out.append(input.data() + i, seqLen);
            i += seqLen;
        } else {
            if (out.size() + 3 > maxBytes) break;
            out.append("\xEF\xBF\xBD", 3);
            ++i;
        }
    }
    return out;
}

std::string WinUtils::truncateUtf8Tail(std::string_view input, size_t maxBytes, bool alignToLineStart) {
    if (input.empty() || maxBytes == 0) return {};
    if (input.size() <= maxBytes) {
        return sanitizeUtf8(input);
    }
    size_t start = input.size() - maxBytes;
    // 跳过落在多字节序列中间的 UTF-8 续字节 (10xxxxxx)
    while (start < input.size() &&
           (static_cast<unsigned char>(input[start]) & 0xC0u) == 0x80u) {
        ++start;
    }
    if (alignToLineStart && start < input.size()) {
        const size_t nl = input.find('\n', start);
        if (nl != std::string_view::npos && nl + 1 < input.size()) {
            start = nl + 1;
        }
    }
    return sanitizeUtf8(input.substr(start));
}

}  // namespace tools3000::core
