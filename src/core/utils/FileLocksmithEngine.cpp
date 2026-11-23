#include "core/utils/FileLocksmithEngine.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace tools3000::core {

namespace {

constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr ULONG kObjectNameInformationClass = 1;
constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

struct SystemHandleTableEntryInfoEx {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SystemHandleInformationEx {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SystemHandleTableEntryInfoEx Handles[1];
};

struct ObjectNameInformation {
    UNICODE_STRING Name;
};

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

using RmStartSessionFn = DWORD(WINAPI*)(DWORD*, DWORD, WCHAR[]);
using RmRegisterResourcesFn = DWORD(WINAPI*)(DWORD, UINT, LPCWSTR[], UINT, RM_UNIQUE_PROCESS[], UINT, LPCWSTR[]);
using RmGetListFn = DWORD(WINAPI*)(DWORD, UINT*, UINT*, RM_PROCESS_INFO[], LPDWORD);
using RmEndSessionFn = DWORD(WINAPI*)(DWORD);

std::wstring normalizeWidePathLower(std::wstring path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        path = L"\\\\" + path.substr(8);
    } else if (path.rfind(L"\\\\?\\", 0) == 0) {
        path = path.substr(4);
    }
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return WinUtils::toLower(std::move(path));
}

std::wstring toNtDevicePathLower(const std::wstring& dosPath) {
    HANDLE h = CreateFileW(
        dosPath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        wchar_t buf[MAX_PATH * 4]{};
        const DWORD len = GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)), VOLUME_NAME_NT);
        CloseHandle(h);
        if (len > 0 && len < std::size(buf)) {
            std::wstring res(buf, len);
            while (res.size() > 1 && res.back() == L'\\') res.pop_back();
            return WinUtils::toLower(std::move(res));
        }
    }

    // 回退 QueryDosDeviceW 映射盘符到 \Device\HarddiskVolumeX
    std::wstring norm = normalizeWidePathLower(dosPath);
    if (norm.size() >= 2 && norm[1] == L':') {
        wchar_t drive[3] = {static_cast<wchar_t>(towupper(norm[0])), L':', L'\0'};
        wchar_t devBuf[MAX_PATH * 2]{};
        if (QueryDosDeviceW(drive, devBuf, static_cast<DWORD>(std::size(devBuf))) > 0) {
            std::wstring nt = WinUtils::toLower(std::wstring(devBuf)) + norm.substr(2);
            while (nt.size() > 1 && nt.back() == L'\\') nt.pop_back();
            return nt;
        }
    }
    return {};
}

bool ntPathMatchesTarget(const std::wstring& candidateLower,
                         const std::wstring& targetNtLower,
                         bool isDirectory) noexcept {
    if (candidateLower.empty() || targetNtLower.empty()) return false;
    if (candidateLower == targetNtLower) return true;
    if (isDirectory && candidateLower.size() > targetNtLower.size() &&
        candidateLower.compare(0, targetNtLower.size(), targetNtLower) == 0 &&
        candidateLower[targetNtLower.size()] == L'\\') {
        return true;
    }
    return false;
}

void mergeProcessLock(std::unordered_map<DWORD, FileLockProcessInfo>& map,
                      DWORD pid,
                      const std::wstring& displayHint,
                      const std::wstring& lockedPath,
                      uintptr_t handleValue,
                      bool fromRm,
                      bool fromHandle) {
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    auto& entry = map[pid];
    if (entry.pid == 0) {
        entry.pid = pid;
        entry.exePath = WinUtils::processPathFromPid(pid);
        entry.exeName = WinUtils::processNameFromPid(pid);
        if (entry.exeName.empty() && !entry.exePath.empty()) {
            entry.exeName = std::filesystem::path(entry.exePath).filename().wstring();
        }
        entry.displayName = !displayHint.empty() ? displayHint : entry.exeName;
        if (entry.displayName.empty()) {
            entry.displayName = L"PID " + std::to_wstring(pid);
        }
        entry.risk = FileLocksmithEngine::classifyProcessRisk(pid, entry.exeName);
    } else if (!displayHint.empty() && (entry.displayName.empty() || entry.displayName == entry.exeName)) {
        entry.displayName = displayHint;
    }

    if (fromRm) entry.viaRestartManager = true;
    if (fromHandle) entry.viaHandleScan = true;
    if (handleValue != 0 &&
        std::find(entry.handleValues.begin(), entry.handleValues.end(), handleValue) == entry.handleValues.end()) {
        entry.handleValues.push_back(handleValue);
    }
    if (!lockedPath.empty() && entry.lockedPaths.size() < 16 &&
        std::find(entry.lockedPaths.begin(), entry.lockedPaths.end(), lockedPath) == entry.lockedPaths.end()) {
        entry.lockedPaths.push_back(lockedPath);
    }
}

void scanViaRestartManager(const std::wstring& targetPath,
                           std::unordered_map<DWORD, FileLockProcessInfo>& outMap) {
    HMODULE hRm = LoadLibraryW(L"rstrtmgr.dll");
    if (!hRm) return;
    struct ModGuard {
        HMODULE m;
        ~ModGuard() { FreeLibrary(m); }
    } modGuard{hRm};

    auto* pStart = reinterpret_cast<RmStartSessionFn>(GetProcAddress(hRm, "RmStartSession"));
    auto* pRegister = reinterpret_cast<RmRegisterResourcesFn>(GetProcAddress(hRm, "RmRegisterResources"));
    auto* pGetList = reinterpret_cast<RmGetListFn>(GetProcAddress(hRm, "RmGetList"));
    auto* pEnd = reinterpret_cast<RmEndSessionFn>(GetProcAddress(hRm, "RmEndSession"));
    if (!pStart || !pRegister || !pGetList || !pEnd) return;

    DWORD session = 0;
    WCHAR sessionKey[CCH_RM_SESSION_KEY + 1]{};
    if (pStart(&session, 0, sessionKey) != ERROR_SUCCESS) return;
    struct SessGuard {
        RmEndSessionFn endFn;
        DWORD s;
        ~SessGuard() { endFn(s); }
    } sessGuard{pEnd, session};

    LPCWSTR files[1] = {targetPath.c_str()};
    if (pRegister(session, 1, files, 0, nullptr, 0, nullptr) != ERROR_SUCCESS) return;

    UINT needed = 0;
    UINT count = 0;
    DWORD reason = 0;
    DWORD err = pGetList(session, &needed, &count, nullptr, &reason);
    if (err != ERROR_MORE_DATA || needed == 0) return;

    std::vector<RM_PROCESS_INFO> infos(needed + 4);
    count = static_cast<UINT>(infos.size());
    err = pGetList(session, &needed, &count, infos.data(), &reason);
    if (err != ERROR_SUCCESS) return;

    for (UINT i = 0; i < count; ++i) {
        const auto& info = infos[i];
        std::wstring appName = info.strAppName;
        if (appName.empty() && info.strServiceShortName[0] != L'\0') {
            appName = info.strServiceShortName;
        }
        mergeProcessLock(outMap, info.Process.dwProcessId, appName, targetPath, 0, true, false);
    }
}

void scanLoadedModules(const std::wstring& targetDosLower,
                       bool isDirectory,
                       std::unordered_map<DWORD, FileLockProcessInfo>& outMap) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const DWORD selfPid = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == 4 || pid == selfPid) continue;
            HANDLE modSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (modSnap == INVALID_HANDLE_VALUE) continue;
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(modSnap, &me)) {
                do {
                    const std::wstring modLow = normalizeWidePathLower(me.szExePath);
                    if (modLow == targetDosLower ||
                        (isDirectory && modLow.size() > targetDosLower.size() &&
                         modLow.compare(0, targetDosLower.size(), targetDosLower) == 0 &&
                         modLow[targetDosLower.size()] == L'\\')) {
                        mergeProcessLock(outMap, pid, pe.szExeFile, me.szExePath, 0, false, true);
                        break;
                    }
                } while (Module32NextW(modSnap, &me));
            }
            CloseHandle(modSnap);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// 单句柄安全探测工作器：使用独立线程 + CancelSynchronousIo 彻底免疫匿名管道内核挂死
class SafeHandleNameProber {
public:
    explicit SafeHandleNameProber(NtQueryObjectFn queryObjFn) : m_queryObj(queryObjFn) {
        m_worker = std::thread([this]() { workerLoop(); });
    }

    ~SafeHandleNameProber() {
        {
            std::lock_guard lock(m_mtx);
            m_stop = true;
            m_hasTask = false;
        }
        m_cv.notify_one();
        if (m_worker.joinable()) {
            CancelSynchronousIo(m_worker.native_handle());
            m_worker.join();
        }
    }

    bool probeDiskHandleNtName(HANDLE dupHandle, uint32_t timeoutMs, std::wstring& outNtName) {
        outNtName.clear();
        if (!dupHandle || dupHandle == INVALID_HANDLE_VALUE || !m_queryObj) return false;

        std::unique_lock lock(m_mtx);
        m_handle = dupHandle;
        m_resultOk = false;
        m_resultName.clear();
        m_done = false;
        m_hasTask = true;
        m_cv.notify_one();

        const bool finished = m_doneCv.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), [this]() { return m_done; });
        if (!finished) {
            CancelSynchronousIo(m_worker.native_handle());
            m_doneCv.wait_for(lock, std::chrono::milliseconds(150), [this]() { return m_done; });
            return false;
        }
        if (m_resultOk) {
            outNtName = std::move(m_resultName);
            return true;
        }
        return false;
    }

private:
    void workerLoop() {
        std::vector<BYTE> buf(8192);
        while (true) {
            HANDLE target = nullptr;
            {
                std::unique_lock lock(m_mtx);
                m_cv.wait(lock, [this]() { return m_stop || m_hasTask; });
                if (m_stop) break;
                target = m_handle;
                m_hasTask = false;
            }

            bool ok = false;
            std::wstring name;
            // GetFileType(FILE_TYPE_DISK) 快速过滤管道/控制台/套接字，仅磁盘文件句柄才调 NtQueryObject
            if (GetFileType(target) == FILE_TYPE_DISK) {
                ULONG retLen = 0;
                const NTSTATUS st = m_queryObj(
                    target, kObjectNameInformationClass, buf.data(), static_cast<ULONG>(buf.size()), &retLen);
                if (st >= 0) {
                    const auto* oni = reinterpret_cast<const ObjectNameInformation*>(buf.data());
                    if (oni->Name.Buffer && oni->Name.Length > 0) {
                        name.assign(oni->Name.Buffer, oni->Name.Length / sizeof(wchar_t));
                        ok = true;
                    }
                }
            }

            {
                std::lock_guard lock(m_mtx);
                m_resultOk = ok;
                m_resultName = std::move(name);
                m_done = true;
            }
            m_doneCv.notify_one();
        }
    }

    NtQueryObjectFn m_queryObj = nullptr;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::condition_variable m_doneCv;
    HANDLE m_handle = nullptr;
    bool m_stop = false;
    bool m_hasTask = false;
    bool m_done = false;
    bool m_resultOk = false;
    std::wstring m_resultName;
    std::thread m_worker;
};

void scanViaSystemHandles(const std::wstring& targetPath,
                          const std::wstring& targetNtLower,
                          bool isDirectory,
                          uint32_t budgetMs,
                          std::unordered_map<DWORD, FileLockProcessInfo>& outMap,
                          uint32_t& outInaccessibleProcesses) {
    if (targetNtLower.empty()) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto* pQuerySys = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    auto* pQueryObj = reinterpret_cast<NtQueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"));
    if (!pQuerySys || !pQueryObj) return;

    // 打开一个普通哨兵文件，精确标定当前内核的 File 对象 ObjectTypeIndex（直接剪枝 >95% 非文件句柄）
    HANDLE sentinel = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sentinel == INVALID_HANDLE_VALUE) return;
    struct SentinelGuard {
        HANDLE h;
        ~SentinelGuard() { CloseHandle(h); }
    } sentinelGuard{sentinel};

    ULONG bufSize = 4u * 1024u * 1024u;
    std::vector<BYTE> buffer;
    NTSTATUS status = kStatusInfoLengthMismatch;
    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.resize(bufSize);
        ULONG needed = 0;
        status = pQuerySys(kSystemExtendedHandleInformation, buffer.data(), bufSize, &needed);
        if (status == kStatusInfoLengthMismatch) {
            bufSize = (std::max)(needed + 512u * 1024u, bufSize * 2u);
            if (bufSize > 128u * 1024u * 1024u) return;
            continue;
        }
        break;
    }
    if (status < 0) return;

    const auto* info = reinterpret_cast<const SystemHandleInformationEx*>(buffer.data());
    const DWORD selfPid = GetCurrentProcessId();
    const ULONG_PTR sentinelVal = reinterpret_cast<ULONG_PTR>(sentinel);

    USHORT fileTypeIndex = 0;
    bool foundFileTypeIndex = false;
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& e = info->Handles[i];
        if (e.UniqueProcessId == selfPid && e.HandleValue == sentinelVal) {
            fileTypeIndex = e.ObjectTypeIndex;
            foundFileTypeIndex = true;
            break;
        }
    }
    if (!foundFileTypeIndex) return;

    // 按 PID 分组，每个进程只 OpenProcess 一次
    std::unordered_map<DWORD, std::vector<ULONG_PTR>> handlesByPid;
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& e = info->Handles[i];
        if (e.ObjectTypeIndex != fileTypeIndex) continue;
        const DWORD pid = static_cast<DWORD>(e.UniqueProcessId);
        if (pid == 0 || pid == 4 || pid == selfPid) continue;
        // 已知易阻塞的纯同步管道权限掩码直接跳过
        if (e.GrantedAccess == 0x0012019Fu || e.GrantedAccess == 0x001A019Fu ||
            e.GrantedAccess == 0x00120189u || e.GrantedAccess == 0x00100000u) {
            continue;
        }
        handlesByPid[pid].push_back(e.HandleValue);
    }

    SafeHandleNameProber prober(pQueryObj);
    const ULONGLONG deadline = GetTickCount64() + budgetMs;

    for (const auto& [pid, hvals] : handlesByPid) {
        if (GetTickCount64() >= deadline) break;
        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) {
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                ++outInaccessibleProcesses;
            }
            continue;
        }

        for (ULONG_PTR hv : hvals) {
            if (GetTickCount64() >= deadline) break;
            HANDLE dup = nullptr;
            if (!DuplicateHandle(hProc, reinterpret_cast<HANDLE>(hv),
                                 GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                continue;
            }
            std::wstring ntName;
            const bool gotName = prober.probeDiskHandleNtName(dup, 35, ntName);
            CloseHandle(dup);
            if (!gotName || ntName.empty()) continue;

            std::wstring ntLow = WinUtils::toLower(ntName);
            while (ntLow.size() > 1 && ntLow.back() == L'\\') ntLow.pop_back();
            if (ntPathMatchesTarget(ntLow, targetNtLower, isDirectory)) {
                mergeProcessLock(outMap, pid, L"", targetPath, static_cast<uintptr_t>(hv), false, true);
            }
        }
        CloseHandle(hProc);
    }
}

constexpr const wchar_t* kFileMenuRegKey = L"Software\\Classes\\*\\shell\\Tools3000.FileLocksmith";
constexpr const wchar_t* kDirMenuRegKey = L"Software\\Classes\\Directory\\shell\\Tools3000.FileLocksmith";

bool writeSingleShellVerb(const wchar_t* subKeyPath,
                          bool shiftOnlyExtended,
                          const std::wstring& exePath,
                          const std::wstring& menuLabel) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    auto setSz = [](HKEY k, const wchar_t* name, const std::wstring& val) -> bool {
        const DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
        return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), bytes) == ERROR_SUCCESS;
    };

    const std::wstring label = menuLabel.empty() ? L"Tools3000: Inspect File Locks" : menuLabel;
    const std::wstring iconVal = L"\"" + exePath + L"\",0";
    bool ok = setSz(hKey, L"MUIVerb", label) &&
              setSz(hKey, L"Icon", iconVal) &&
              setSz(hKey, L"MultiSelectModel", L"Single");

    if (shiftOnlyExtended) {
        ok = ok && setSz(hKey, L"Extended", L"");
    } else {
        RegDeleteValueW(hKey, L"Extended");
    }

    HKEY hCmd = nullptr;
    if (ok && RegCreateKeyExW(hKey, L"command", 0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_SET_VALUE, nullptr, &hCmd, nullptr) == ERROR_SUCCESS) {
        const std::wstring cmd = L"\"" + exePath + L"\" --unlock \"%1\"";
        ok = setSz(hCmd, nullptr, cmd);
        RegCloseKey(hCmd);
    } else {
        ok = false;
    }

    RegCloseKey(hKey);
    return ok;
}

}  // namespace

LockProcessRisk FileLocksmithEngine::classifyProcessRisk(DWORD pid, const std::wstring& exeName) noexcept {
    if (pid == 0 || pid == 4) return LockProcessRisk::CriticalSystem;
    const std::wstring low = WinUtils::toLower(exeName);
    if (low == L"explorer.exe") return LockProcessRisk::Explorer;

    static constexpr std::wstring_view kCritical[] = {
        L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"lsm.exe", L"fontdrvhost.exe",
        L"dwm.exe", L"memory compression", L"registry", L"ntoskrnl.exe"
    };
    for (const auto& name : kCritical) {
        if (low == name) return LockProcessRisk::CriticalSystem;
    }
    return LockProcessRisk::Normal;
}

FileLockScanResult FileLocksmithEngine::scanTargetLocks(const std::wstring& targetPath,
                                                        uint32_t totalBudgetMs) {
    const auto started = std::chrono::steady_clock::now();
    FileLockScanResult result;
    result.targetPath = targetPath;
    result.currentProcessElevated = WinUtils::isCurrentProcessElevated();
    if (targetPath.empty()) return result;

    std::error_code ec;
    const std::filesystem::path p(targetPath);
    if (!std::filesystem::exists(p, ec)) {
        return result;
    }
    result.ok = true;
    result.isDirectory = std::filesystem::is_directory(p, ec);

    std::unordered_map<DWORD, FileLockProcessInfo> procMap;

    // 阶段 1：Restart Manager 毫秒级扫描（仅针对普通文件，目录由阶段 2 深度扫描接管）
    if (!result.isDirectory) {
        scanViaRestartManager(targetPath, procMap);
    }

    // 阶段 2：已加载模块映射 + 深度内核句柄探测（带独立线程 CancelSynchronousIo 护栏）
    const std::wstring targetDosLower = normalizeWidePathLower(targetPath);
    scanLoadedModules(targetDosLower, result.isDirectory, procMap);

    const std::wstring targetNtLower = toNtDevicePathLower(targetPath);
    scanViaSystemHandles(targetPath, targetNtLower, result.isDirectory, totalBudgetMs,
                         procMap, result.inaccessibleProcessCount);

    result.processes.reserve(procMap.size());
    for (auto& [pid, info] : procMap) {
        result.processes.push_back(std::move(info));
    }

    // 按风险分级（普通进程在前 -> Explorer -> 系统关键进程）与名称稳定排序
    std::sort(result.processes.begin(), result.processes.end(),
              [](const FileLockProcessInfo& a, const FileLockProcessInfo& b) {
                  if (a.risk != b.risk) return static_cast<int>(a.risk) < static_cast<int>(b.risk);
                  return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
              });

    result.elapsedMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}

bool FileLocksmithEngine::terminateProcessSafely(DWORD pid, std::string& outError) {
    outError.clear();
    if (pid == 0 || pid == 4 || pid == GetCurrentProcessId()) {
        outError = "protected_process";
        return false;
    }
    const std::wstring exeName = WinUtils::processNameFromPid(pid);
    if (classifyProcessRisk(pid, exeName) == LockProcessRisk::CriticalSystem) {
        outError = "critical_system_process";
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!hProc) {
        outError = (GetLastError() == ERROR_ACCESS_DENIED) ? "access_denied" : "open_failed";
        return false;
    }

    // 先温和发 WM_CLOSE 给顶层窗口给应用保存机会
    struct CloseCtx { DWORD pid; } ctx{pid};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            const auto* c = reinterpret_cast<const CloseCtx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == c->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (WaitForSingleObject(hProc, 450) == WAIT_OBJECT_0) {
        CloseHandle(hProc);
        return true;
    }

    const BOOL terminated = TerminateProcess(hProc, 1);
    if (terminated) {
        WaitForSingleObject(hProc, 1500);
        CloseHandle(hProc);
        return true;
    }
    const DWORD err = GetLastError();
    CloseHandle(hProc);
    outError = (err == ERROR_ACCESS_DENIED) ? "access_denied" : "terminate_failed";
    return false;
}

bool FileLocksmithEngine::restartExplorerSafely(std::string& outError) {
    outError.clear();
    DWORD explorerPid = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        GetWindowThreadProcessId(tray, &explorerPid);
    }
    if (explorerPid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, explorerPid);
        if (hProc) {
            TerminateProcess(hProc, 0);
            WaitForSingleObject(hProc, 2000);
            CloseHandle(hProc);
        }
    }
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::wstring explorerExe = std::wstring(winDir) + L"\\explorer.exe";
    HINSTANCE hi = ShellExecuteW(nullptr, L"open", explorerExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(hi) <= 32) {
        outError = "restart_explorer_failed";
        return false;
    }
    return true;
}

bool FileLocksmithEngine::syncExplorerContextMenu(bool enabled,
                                                  bool shiftOnlyExtended,
                                                  const std::wstring& exePath,
                                                  const std::wstring& menuLabel) {
    bool ok = true;
    if (!enabled) {
        const LSTATUS s1 = RegDeleteTreeW(HKEY_CURRENT_USER, kFileMenuRegKey);
        const LSTATUS s2 = RegDeleteTreeW(HKEY_CURRENT_USER, kDirMenuRegKey);
        ok = (s1 == ERROR_SUCCESS || s1 == ERROR_FILE_NOT_FOUND) &&
             (s2 == ERROR_SUCCESS || s2 == ERROR_FILE_NOT_FOUND);
    } else {
        const std::wstring resolvedExe = exePath.empty() ? WinUtils::getExePath().wstring() : exePath;
        if (resolvedExe.empty()) return false;
        ok = writeSingleShellVerb(kFileMenuRegKey, shiftOnlyExtended, resolvedExe, menuLabel) &&
             writeSingleShellVerb(kDirMenuRegKey, shiftOnlyExtended, resolvedExe, menuLabel);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool FileLocksmithEngine::isExplorerContextMenuRegistered(bool* outShiftOnly) {
    if (outShiftOnly) *outShiftOnly = false;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kFileMenuRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    if (outShiftOnly) {
        DWORD type = 0;
        const LSTATUS st = RegQueryValueExW(hKey, L"Extended", nullptr, &type, nullptr, nullptr);
        *outShiftOnly = (st == ERROR_SUCCESS);
    }
    RegCloseKey(hKey);
    return true;
}

bool FileLocksmithEngine::launchUnlockWindowProcess(const std::wstring& targetPath, bool requestElevation) {
    if (targetPath.empty()) return false;
    const std::filesystem::path exePath = WinUtils::getExePath();
    if (exePath.empty()) return false;
    const std::wstring args = L"--unlock \"" + targetPath + L"\"";

    if (requestElevation) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = exePath.wstring().c_str();
        sei.lpParameters = args.c_str();
        sei.lpDirectory = exePath.parent_path().wstring().c_str();
        sei.nShow = SW_SHOWNORMAL;
        return ShellExecuteExW(&sei) != FALSE;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\" " + args;
    if (!CreateProcessW(
            exePath.wstring().c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            exePath.parent_path().wstring().c_str(),
            &si,
            &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace tools3000::core
