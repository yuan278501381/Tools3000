#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureWorkerCommon.h — 重型媒体/AI 短命 Worker 子进程隔离公共原语 (SSOT)
//
// 职责:
//   1. 统一定义 Worker 模式标识、CLI 参数、管道/共享内存命名规则与超时常量
//   2. 提供零泄漏 ScopedHandle、可靠管道精确字节流读写与安全子进程拉起封装
//   3. 提供父进程死亡内核看门狗 (ParentProcessWatchdog)，配合宿主 Job Object 双重杜绝僵尸进程
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CAPTUREWORKERCOMMON_H
#define TOOLS3000_CAPTURE_CAPTUREWORKERCOMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tools3000::worker {

namespace Constants {
    inline constexpr const char* kWorkerEntryExportName = "Tools3000RunCaptureWorker";
    inline constexpr const wchar_t* kFlagWorkerMode     = L"--worker=";
    inline constexpr const wchar_t* kFlagPipeName       = L"--pipe=";
    inline constexpr const wchar_t* kFlagParentPid      = L"--parent-pid=";
    inline constexpr const wchar_t* kFlagIdleSeconds    = L"--idle-sec=";
    inline constexpr const wchar_t* kFlagJobFile        = L"--job=";
    inline constexpr const wchar_t* kFlagCancelEvent    = L"--cancel-event=";
    inline constexpr const wchar_t* kFlagSafeCpuMode    = L"--safe-cpu=";

    inline constexpr const wchar_t* kModeOcr            = L"ocr";
    inline constexpr const wchar_t* kModeStudioExport   = L"studio-export";
    inline constexpr const wchar_t* kModeRecordEncode   = L"record-encode";
    inline constexpr const wchar_t* kModeMediaProbe     = L"media-probe";

    inline constexpr DWORD kPipeBufferBytes             = 256 * 1024;
    inline constexpr DWORD kPipeConnectTimeoutMs        = 15000;
    inline constexpr DWORD kWorkerGracefulExitWaitMs    = 4000;
    inline constexpr int   kDefaultOcrWorkerSoftTrimSeconds = 60;   // 阶段 1: 闲置 60s 执行软修剪 (DXGI Trim + 工作集收缩)，保留已编译 DirectML Session
    inline constexpr int   kDefaultOcrWorkerIdleSeconds     = 900;  // 阶段 2: 闲置 15 分钟仍无请求则优雅退出，物理+虚拟内存 100% 归零
    inline constexpr uint32_t kMaxIpcStringBytes        = 16u * 1024u * 1024u;
} // namespace Constants

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : m_handle(normalize(handle)) {}
    ~ScopedHandle() noexcept { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_handle; }
    explicit operator bool() const noexcept { return m_handle != nullptr; }

    HANDLE release() noexcept {
        HANDLE h = m_handle;
        m_handle = nullptr;
        return h;
    }

    void reset(HANDLE handle = nullptr) noexcept {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
        m_handle = normalize(handle);
    }

private:
    static HANDLE normalize(HANDLE h) noexcept {
        return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
    }
    HANDLE m_handle = nullptr;
};

class ScopedMapView {
public:
    ScopedMapView() noexcept = default;
    explicit ScopedMapView(void* ptr) noexcept : m_ptr(ptr) {}
    ~ScopedMapView() noexcept { reset(); }

    ScopedMapView(const ScopedMapView&) = delete;
    ScopedMapView& operator=(const ScopedMapView&) = delete;

    ScopedMapView(ScopedMapView&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }
    ScopedMapView& operator=(ScopedMapView&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void* get() const noexcept { return m_ptr; }
    uint8_t* bytes() const noexcept { return static_cast<uint8_t*>(m_ptr); }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset(void* ptr = nullptr) noexcept {
        if (m_ptr != nullptr) {
            UnmapViewOfFile(m_ptr);
        }
        m_ptr = ptr;
    }

private:
    void* m_ptr = nullptr;
};

inline std::atomic<bool>& runningInsideWorkerFlag() noexcept {
    static std::atomic<bool> s_inWorker{false};
    return s_inWorker;
}

inline bool isRunningInsideWorkerProcess() noexcept {
    return runningInsideWorkerFlag().load(std::memory_order_acquire);
}

inline void markRunningInsideWorkerProcess() noexcept {
    runningInsideWorkerFlag().store(true, std::memory_order_release);
}

inline std::wstring makeUniqueSessionSuffix() {
    static std::atomic<uint64_t> s_seq{1};
    const DWORD pid = GetCurrentProcessId();
    const uint64_t tick = GetTickCount64();
    const uint64_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    return std::to_wstring(pid) + L"_" + std::to_wstring(tick) + L"_" + std::to_wstring(seq);
}

inline std::wstring makeLocalPipeName(std::wstring_view tag) {
    std::wstring name = L"\\\\.\\pipe\\Tools3000_";
    name.append(tag);
    name.push_back(L'_');
    name.append(makeUniqueSessionSuffix());
    return name;
}

inline std::wstring makeLocalObjectName(std::wstring_view tag) {
    std::wstring name = L"Local\\Tools3000_";
    name.append(tag);
    name.push_back(L'_');
    name.append(makeUniqueSessionSuffix());
    return name;
}

inline bool envFlagEnabled(const wchar_t* name) noexcept {
    wchar_t buf[16]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return len > 0 && (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y');
}

inline std::wstring toLowerWideAscii(std::wstring s) {
    for (wchar_t& ch : s) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return s;
}

/// 定位宿主 Tools3000.exe 路径：
/// - 生产环境运行于 Tools3000.exe 内部时直接返回自身路径
/// - 单测环境 (Tools3000Tests.exe) 默认返回空路径（保持进程内快跑），显式注入 overrideExe 时使用指定路径
inline std::filesystem::path resolveHostWorkerExecutablePath(
    const std::filesystem::path& overrideExe = {}) {
    std::error_code ec;
    if (!overrideExe.empty()) {
        if (std::filesystem::is_regular_file(overrideExe, ec) && !ec) {
            return overrideExe;
        }
        return {};
    }
    wchar_t modulePath[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (len == 0 || len >= std::size(modulePath)) {
        return {};
    }
    const std::filesystem::path selfPath(modulePath);
    if (toLowerWideAscii(selfPath.filename().wstring()) == L"tools3000.exe") {
        return selfPath;
    }
    return {};
}

inline std::wstring quoteWindowsArg(std::wstring_view arg) {
    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes > 0) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out.push_back(ch);
        }
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
    return out;
}

inline bool launchHiddenWorkerProcess(
    const std::filesystem::path& exePath,
    const std::vector<std::wstring>& args,
    ScopedHandle& outProcessHandle,
    DWORD* outPid = nullptr,
    DWORD priorityClass = NORMAL_PRIORITY_CLASS) {
    outProcessHandle.reset();
    if (outPid) *outPid = 0;
    if (exePath.empty()) return false;

    std::wstring cmdLine = quoteWindowsArg(exePath.wstring());
    for (const auto& arg : args) {
        cmdLine.push_back(L' ');
        cmdLine.append(quoteWindowsArg(arg));
    }

    // 前台交互型 --worker=ocr 子进程自动提升至 ABOVE_NORMAL_PRIORITY_CLASS，
    // 确保前台识图在后台扫描/编译等高负载场景下依然获得优先调度
    DWORD effectivePriority = priorityClass;
    if (effectivePriority == NORMAL_PRIORITY_CLASS) {
        const std::wstring ocrModeFlag = std::wstring(Constants::kFlagWorkerMode) + Constants::kModeOcr;
        for (const auto& arg : args) {
            if (arg == ocrModeFlag) {
                effectivePriority = ABOVE_NORMAL_PRIORITY_CLASS;
                break;
            }
        }
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    const std::wstring exeWide = exePath.wstring();
    const std::wstring workDir = exePath.parent_path().wstring();
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(
        exeWide.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | effectivePriority,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si,
        &pi);
    if (!ok) {
        return false;
    }
    CloseHandle(pi.hThread);
    outProcessHandle.reset(pi.hProcess);
    if (outPid) *outPid = pi.dwProcessId;
    return true;
}

/// 带超时与子进程存活监测的异步命名管道服务端连接等待
inline bool waitForPipeClientWithTimeout(
    HANDLE pipeServer,
    HANDLE childProcess,
    DWORD timeoutMs) {
    if (!pipeServer || pipeServer == INVALID_HANDLE_VALUE) return false;
    ScopedHandle evt(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!evt) return false;

    OVERLAPPED ov{};
    ov.hEvent = evt.get();
    const BOOL immediate = ConnectNamedPipe(pipeServer, &ov);
    if (immediate) {
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (err != ERROR_IO_PENDING) {
        return false;
    }

    HANDLE waitHandles[2] = {evt.get(), childProcess};
    const DWORD count = (childProcess != nullptr) ? 2u : 1u;
    const DWORD waitRes = WaitForMultipleObjects(count, waitHandles, FALSE, timeoutMs);
    if (waitRes == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        return GetOverlappedResult(pipeServer, &ov, &transferred, FALSE) != FALSE;
    }
    CancelIoEx(pipeServer, &ov);
    return false;
}

/// 命名管道客户端带退避重试连接
inline ScopedHandle connectPipeClientWithRetry(const std::wstring& pipeName, DWORD timeoutMs) {
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (true) {
        HANDLE h = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            return ScopedHandle(h);
        }
        const DWORD err = GetLastError();
        if (GetTickCount64() >= deadline) {
            return {};
        }
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), 100);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            Sleep(10);
        } else {
            return {};
        }
    }
}

/// 同步管道精确写满全部字节（管道须为阻塞模式，或通过局部 OVERLAPPED 兼容 FILE_FLAG_OVERLAPPED）
inline bool writeExact(HANDLE handle, const void* data, size_t size, bool overlappedHandle = false) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    ScopedHandle evt;
    if (overlappedHandle) {
        evt.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!evt) return false;
    }
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(remaining, 64ULL * 1024ULL * 1024ULL));
        DWORD written = 0;
        if (overlappedHandle) {
            ResetEvent(evt.get());
            OVERLAPPED ov{};
            ov.hEvent = evt.get();
            if (!WriteFile(handle, ptr, chunk, &written, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (!GetOverlappedResult(handle, &ov, &written, TRUE)) return false;
            }
        } else {
            if (!WriteFile(handle, ptr, chunk, &written, nullptr)) {
                return false;
            }
        }
        if (written == 0) return false;
        ptr += written;
        remaining -= written;
    }
    return true;
}

/// 命名管道读取结果枚举（支持精确区分空窗超时与管道断开/父进程退出，支撑两阶段保活）
enum class PipeReadStatus {
    Ok = 0,
    Timeout,
    Aborted,
    ErrorOrEof,
};

/// 同步管道精确读满全部字节并返回精细状态（仅 overlappedHandle=true 时支持超时与外部中断事件）
inline PipeReadStatus readExactWithStatus(
    HANDLE handle,
    void* data,
    size_t size,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr) {
    if (!handle || handle == INVALID_HANDLE_VALUE) return PipeReadStatus::ErrorOrEof;
    auto* ptr = static_cast<uint8_t*>(data);
    size_t remaining = size;
    ScopedHandle evt;
    if (overlappedHandle) {
        evt.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!evt) return PipeReadStatus::ErrorOrEof;
    }
    const uint64_t deadline = (timeoutMs == INFINITE) ? 0ULL : (GetTickCount64() + timeoutMs);
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(remaining, 64ULL * 1024ULL * 1024ULL));
        DWORD got = 0;
        if (overlappedHandle) {
            ResetEvent(evt.get());
            OVERLAPPED ov{};
            ov.hEvent = evt.get();
            if (!ReadFile(handle, ptr, chunk, &got, &ov)) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) return PipeReadStatus::ErrorOrEof;
                DWORD waitMs = INFINITE;
                if (timeoutMs != INFINITE) {
                    const uint64_t now = GetTickCount64();
                    if (now >= deadline) {
                        CancelIoEx(handle, &ov);
                        DWORD cancelledGot = 0;
                        const BOOL completed = GetOverlappedResult(handle, &ov, &cancelledGot, TRUE);
                        if (completed && cancelledGot > 0) {
                            ptr += cancelledGot;
                            remaining -= cancelledGot;
                            continue;
                        }
                        return (remaining == size) ? PipeReadStatus::Timeout : PipeReadStatus::ErrorOrEof;
                    }
                    waitMs = static_cast<DWORD>(deadline - now);
                }
                HANDLE waitHandles[2] = {evt.get(), abortEvent};
                const DWORD count = (abortEvent != nullptr) ? 2u : 1u;
                const DWORD waitRes = WaitForMultipleObjects(count, waitHandles, FALSE, waitMs);
                if (waitRes != WAIT_OBJECT_0) {
                    CancelIoEx(handle, &ov);
                    // 严格同步等待内核取消落地，防止栈上 OVERLAPPED 析构悬空；
                    // 若临界微秒数据恰已抵达，则平滑续读避免丢包错位
                    DWORD cancelledGot = 0;
                    const BOOL completed = GetOverlappedResult(handle, &ov, &cancelledGot, TRUE);
                    if (waitRes == WAIT_TIMEOUT) {
                        if (completed && cancelledGot > 0) {
                            ptr += cancelledGot;
                            remaining -= cancelledGot;
                            continue;
                        }
                        return (remaining == size) ? PipeReadStatus::Timeout : PipeReadStatus::ErrorOrEof;
                    }
                    if (waitRes == WAIT_OBJECT_0 + 1) {
                        return PipeReadStatus::Aborted;
                    }
                    return PipeReadStatus::ErrorOrEof;
                }
                if (!GetOverlappedResult(handle, &ov, &got, FALSE)) return PipeReadStatus::ErrorOrEof;
            }
        } else {
            if (!ReadFile(handle, ptr, chunk, &got, nullptr)) {
                return PipeReadStatus::ErrorOrEof;
            }
        }
        if (got == 0) return PipeReadStatus::ErrorOrEof;
        ptr += got;
        remaining -= got;
    }
    return PipeReadStatus::Ok;
}

/// 同步管道精确读满全部字节（支持可选读取超时与外部中断事件，仅 overlappedHandle=true 时生效）
inline bool readExact(
    HANDLE handle,
    void* data,
    size_t size,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr) {
    return readExactWithStatus(handle, data, size, overlappedHandle, timeoutMs, abortEvent) == PipeReadStatus::Ok;
}

template <typename T>
inline bool writePod(HANDLE handle, const T& value, bool overlappedHandle = false) {
    static_assert(std::is_trivially_copyable_v<T>);
    return writeExact(handle, &value, sizeof(T), overlappedHandle);
}

template <typename T>
inline PipeReadStatus readPodWithStatus(
    HANDLE handle,
    T& value,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr) {
    static_assert(std::is_trivially_copyable_v<T>);
    return readExactWithStatus(handle, &value, sizeof(T), overlappedHandle, timeoutMs, abortEvent);
}

template <typename T>
inline bool readPod(
    HANDLE handle,
    T& value,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr) {
    static_assert(std::is_trivially_copyable_v<T>);
    return readExact(handle, &value, sizeof(T), overlappedHandle, timeoutMs, abortEvent);
}

inline bool writeString(HANDLE handle, std::string_view text, bool overlappedHandle = false) {
    if (text.size() > Constants::kMaxIpcStringBytes) return false;
    const uint32_t len = static_cast<uint32_t>(text.size());
    if (!writePod(handle, len, overlappedHandle)) return false;
    return len == 0 || writeExact(handle, text.data(), len, overlappedHandle);
}

inline bool readString(
    HANDLE handle,
    std::string& out,
    bool overlappedHandle = false,
    DWORD timeoutMs = INFINITE,
    HANDLE abortEvent = nullptr) {
    uint32_t len = 0;
    if (!readPod(handle, len, overlappedHandle, timeoutMs, abortEvent)) return false;
    if (len > Constants::kMaxIpcStringBytes) return false;
    out.resize(len);
    return len == 0 || readExact(handle, out.data(), len, overlappedHandle, timeoutMs, abortEvent);
}

/// 解析命令行 --key=value 参数值
inline std::wstring findCliOptionValue(int argc, const wchar_t* const* argv, std::wstring_view prefix) {
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) continue;
        const std::wstring_view arg(argv[i]);
        if (arg.size() >= prefix.size() && arg.compare(0, prefix.size(), prefix) == 0) {
            return std::wstring(arg.substr(prefix.size()));
        }
    }
    return {};
}

/// 子进程父进程死亡看门狗：主进程一旦退出或崩溃，立即触发回调并在短宽限后强制退出自身，零僵尸残留
class ParentProcessWatchdog {
public:
    ParentProcessWatchdog(DWORD parentPid, std::function<void()> onParentDied = nullptr) {
        if (parentPid == 0) return;
        m_parent.reset(OpenProcess(SYNCHRONIZE, FALSE, parentPid));
        m_stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!m_parent || !m_stopEvent) return;
        m_thread = std::thread([this, cb = std::move(onParentDied)]() {
            HANDLE handles[2] = {m_stopEvent.get(), m_parent.get()};
            const DWORD res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (res == WAIT_OBJECT_0 + 1) {
                if (cb) {
                    try { cb(); } catch (...) {}
                }
                if (WaitForSingleObject(m_stopEvent.get(), 400) == WAIT_TIMEOUT) {
                    TerminateProcess(GetCurrentProcess(), 0);
                }
            }
        });
    }

    ~ParentProcessWatchdog() {
        stop();
    }

    ParentProcessWatchdog(const ParentProcessWatchdog&) = delete;
    ParentProcessWatchdog& operator=(const ParentProcessWatchdog&) = delete;

    void stop() noexcept {
        if (m_stopEvent) {
            SetEvent(m_stopEvent.get());
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        m_parent.reset();
        m_stopEvent.reset();
    }

private:
    ScopedHandle m_parent;
    ScopedHandle m_stopEvent;
    std::thread m_thread;
};

} // namespace tools3000::worker

#endif // TOOLS3000_CAPTURE_CAPTUREWORKERCOMMON_H
