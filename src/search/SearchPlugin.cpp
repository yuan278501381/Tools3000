#include "core/plugin/IPlugin.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/utils/WinUtils.h"
#include "search/ServiceStartupPolicy.h"
#include "search/SearchInteractionHandlers.h"
#include "search/SearchQueryConfigUtils.h"
#include "service/PipeProtocol.h"
#include "service/PipeEndpoint.h"
#include "service/SearchRequestLimits.h"
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <array>
#include <bcrypt.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

std::string g_searchPipe;
std::string g_kernelServicePipe;
std::string g_portableSidPipe;
std::string g_searchPipeToken;
std::wstring g_searchClientSid;
bool g_forcePortableService = false;

// 本进程是否亲手拉起过索引服务，用于启动期状态提示。
static std::atomic<bool> g_serviceSpawnedByUs{false};
static std::atomic<bool> g_serviceConfirmedReady{false};

// 后台 WebView 预载、设置页状态查询和任意搜索 IPC 都不能成为服务启动信号。
// 只有原生 SearchWindow::show() 先发送 windowShown 后，后续请求才允许补拉服务。
static std::atomic<bool> g_searchWindowExplicitlyShown{false};
static std::atomic<bool> g_searchPluginShuttingDown{false};

bool isPipePresentOrBusy(const std::string& pipeName, DWORD waitMs) noexcept {
    if (pipeName.empty()) return false;
    if (WaitNamedPipeA(pipeName.c_str(), waitMs) != FALSE) {
        return true;
    }
    return tools3000::search::isPipeTransientBusyError(GetLastError());
}

std::string resolveActiveSearchPipe(DWORD primaryWaitMs) noexcept {
    if (g_forcePortableService) {
        return isPipePresentOrBusy(g_searchPipe, primaryWaitMs) ? g_searchPipe : std::string{};
    }
    if (isPipePresentOrBusy(g_kernelServicePipe, primaryWaitMs)) {
        return g_kernelServicePipe;
    }
    if (isPipePresentOrBusy(g_portableSidPipe, 1)) {
        return g_portableSidPipe;
    }
    if (isPipePresentOrBusy(g_searchPipe, 1)) {
        return g_searchPipe;
    }
    return {};
}

bool isAnySearchPipeReady(DWORD primaryWaitMs) noexcept {
    return !resolveActiveSearchPipe(primaryWaitMs).empty();
}

std::string expandClientUserEnvUtf8(const std::string& input, std::size_t maxUtf8Bytes) {
    if (input.find('%') == std::string::npos) {
        return input;
    }
    const std::wstring wideInput = tools3000::core::WinUtils::utf8ToWstring(input);
    if (wideInput.empty()) {
        return input;
    }
    constexpr DWORD kMaxExpandedChars = 4096;
    std::vector<wchar_t> buffer(kMaxExpandedChars, L'\0');
    const DWORD charsWritten = ExpandEnvironmentStringsW(wideInput.c_str(), buffer.data(), kMaxExpandedChars);
    if (charsWritten == 0 || charsWritten > kMaxExpandedChars) {
        return input;
    }
    const std::string expanded = tools3000::core::WinUtils::wstringToUtf8(
        std::wstring(buffer.data(), static_cast<std::size_t>(charsWritten - 1)));
    if (expanded.empty() || expanded.size() > maxUtf8Bytes) {
        return input;
    }
    return expanded;
}

bool hasCommandLineFlag(std::wstring_view wanted) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    struct ArgumentsGuard { LPWSTR* value; ~ArgumentsGuard() { LocalFree(value); } } guard{argv};
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], std::wstring(wanted).c_str()) == 0) return true;
    }
    return false;
}

std::optional<std::wstring> currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return std::nullopt;
    struct TokenGuard { HANDLE value; ~TokenGuard() { if (value) CloseHandle(value); } } guard{token};

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (!bytes) return std::nullopt;
    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) return std::nullopt;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid) || !rawSid) return std::nullopt;
    std::wstring sid(rawSid);
    LocalFree(rawSid);
    return sid;
}

std::optional<std::string> generatePipeToken() {
    std::array<unsigned char, tools3000::service::pipe_endpoint::TokenHexLength / 2> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0F]);
    }
    return result;
}

nlohmann::json invalidSearchRequest(const std::string& message) {
    return {
        {"results", nlohmann::json::array()},
        {"available", true},
        {"status", "ready"},
        {"error", message}
    };
}

bool validStringArray(const nlohmann::json& params, const char* key,
                      std::size_t maxItems, std::size_t maxStringBytes) {
    const auto it = params.find(key);
    if (it == params.end() || it->is_null()) return true;
    if (!it->is_array() || it->size() > maxItems) return false;
    return std::all_of(it->begin(), it->end(), [maxStringBytes](const auto& value) {
        return value.is_string() &&
               value.template get_ref<const std::string&>().size() <= maxStringBytes;
    });
}

bool initializePipeEndpoint() {
    auto sid = currentUserSid();
    if (!sid) {
        LOG_ERROR("SearchPlugin: 无法取得当前用户 SID，拒绝创建搜索 IPC 端点");
        return false;
    }
    g_forcePortableService = hasCommandLineFlag(L"--force-portable-search-service");
    g_kernelServicePipe = std::string(tools3000::service::pipe_endpoint::KernelAuthPipeName);
    auto sidPipe = tools3000::service::pipe_endpoint::makeUserSidPipeName(*sid);
    if (!sidPipe) {
        LOG_ERROR("SearchPlugin: 无法取得当前用户 SID，拒绝创建搜索 IPC 端点");
        return false;
    }

    auto& config = tools3000::core::ConfigManager::instance();
    std::string token = config.get<std::string>("/search/pipeToken", "");
    if (!tools3000::service::pipe_endpoint::isValidToken(token)) {
        auto generated = generatePipeToken();
        if (!generated) {
            LOG_ERROR("SearchPlugin: 无法安全生成或保存搜索 IPC 端点令牌");
            return false;
        }
        // Best-effort persist for isolated portable test harnesses; production
        // kernel-authenticated & SID endpoints (`HIGH-10`) do not depend on `config.json`!
        (void)config.set("/search/pipeToken", *generated);
        token = std::move(*generated);
    }
    auto pipe = tools3000::service::pipe_endpoint::makePipeName(token);
    if (!pipe) return false;
    g_searchClientSid = std::move(*sid);
    g_portableSidPipe = std::move(*sidPipe);
    g_searchPipeToken = std::move(token);
    g_searchPipe = std::move(*pipe);
    return true;
}

bool finishOverlapped(HANDLE pipe, OVERLAPPED& overlapped, DWORD timeoutMs,
                      DWORD& transferred, DWORD& error) {
    const DWORD stepMs = 100;
    DWORD elapsed = 0;
    while (elapsed < timeoutMs) {
        if (g_searchPluginShuttingDown.load(std::memory_order_relaxed)) {
            error = ERROR_OPERATION_ABORTED;
            CancelIoEx(pipe, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            return false;
        }
        const DWORD waitChunk = (std::min)(stepMs, timeoutMs - elapsed);
        const DWORD wait = WaitForSingleObject(overlapped.hEvent, waitChunk);
        if (wait == WAIT_OBJECT_0) {
            if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                return true;
            }
            error = GetLastError();
            return false;
        }
        if (wait != WAIT_TIMEOUT) {
            error = GetLastError();
            CancelIoEx(pipe, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            return false;
        }
        elapsed += waitChunk;
    }
    error = ERROR_TIMEOUT;
    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
    return false;
}

/// 分块的重叠 I/O 收发器，复用同一个事件对象完成一整帧的传输。
class PipeTransfer {
public:
    PipeTransfer() : m_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PipeTransfer() { if (m_event) CloseHandle(m_event); }
    PipeTransfer(const PipeTransfer&) = delete;
    PipeTransfer& operator=(const PipeTransfer&) = delete;

    bool valid() const { return m_event != nullptr; }

    bool readExact(HANDLE pipe, char* buffer, size_t bytes, DWORD timeoutMs, DWORD& error) {
        return transfer(pipe, buffer, bytes, timeoutMs, error, false);
    }

    bool writeExact(HANDLE pipe, const char* data, size_t bytes, DWORD timeoutMs, DWORD& error) {
        return transfer(pipe, const_cast<char*>(data), bytes, timeoutMs, error, true);
    }

private:
    static constexpr size_t ChunkBytes = 64 * 1024;

    bool transfer(HANDLE pipe, char* buffer, size_t bytes, DWORD timeoutMs,
                  DWORD& error, bool writing) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        size_t done = 0;
        while (done < bytes) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                error = ERROR_TIMEOUT;
                return false;
            }
            const DWORD remainMs = static_cast<DWORD>(deadline - now);
            const DWORD want = static_cast<DWORD>((std::min)(bytes - done, ChunkBytes));

            OVERLAPPED overlapped{};
            ResetEvent(m_event);
            overlapped.hEvent = m_event;

            DWORD moved = 0;
            const BOOL ok = writing
                ? WriteFile(pipe, buffer + done, want, &moved, &overlapped)
                : ReadFile(pipe, buffer + done, want, &moved, &overlapped);
            if (!ok) {
                error = GetLastError();
                if (error != ERROR_IO_PENDING ||
                    !finishOverlapped(pipe, overlapped, remainMs, moved, error)) {
                    return false;
                }
            }
            if (moved == 0) {
                error = ERROR_NO_DATA;  // 对端已关闭
                return false;
            }
            done += moved;
        }
        return true;
    }

    HANDLE m_event;
};

enum class ScmEndpointResult {
    Ready,
    AllowPortableFallback,
    Unavailable,
};

tools3000::search::ScmServiceState toScmServiceState(DWORD state) noexcept {
    switch (state) {
        case SERVICE_STOPPED: return tools3000::search::ScmServiceState::Stopped;
        case SERVICE_START_PENDING: return tools3000::search::ScmServiceState::StartPending;
        case SERVICE_RUNNING: return tools3000::search::ScmServiceState::Running;
        case SERVICE_STOP_PENDING: return tools3000::search::ScmServiceState::StopPending;
        default: return tools3000::search::ScmServiceState::Failed;
    }
}

// 等待 SCM 已接受的启动请求完成。服务创建本用户受限端点之前可能需要加载快照或
// 扫描卷，不能用固定数百毫秒猜测失败并再拉起一个便携服务。
ScmEndpointResult startScmServiceAndWait(DWORD& error) {
    error = ERROR_SUCCESS;
    // Lifecycle verification needs to exercise the packaged service binary,
    // independent of any older SCM registration on the developer machine.
    if (hasCommandLineFlag(L"--force-portable-search-service")) {
        return ScmEndpointResult::AllowPortableFallback;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = GetLastError();
        return error == ERROR_ACCESS_DENIED ? ScmEndpointResult::AllowPortableFallback
                                            : ScmEndpointResult::Unavailable;
    }
    struct ScmGuard { SC_HANDLE value; ~ScmGuard() { if (value) CloseServiceHandle(value); } } scmGuard{scm};

    SC_HANDLE service = OpenServiceW(scm, L"Tools3000_SearchService",
                                     SERVICE_START | SERVICE_QUERY_STATUS);
    bool canStart = true;
    if (!service) {
        error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return ScmEndpointResult::AllowPortableFallback;
        }
        if (tools3000::search::scmOpenShouldRetryQueryOnly(error)) {
            service = OpenServiceW(scm, L"Tools3000_SearchService", SERVICE_QUERY_STATUS);
            if (!service) {
                error = GetLastError();
                LOG_WARN("SearchPlugin: 无法查询 SCM 搜索服务，回退便携进程, error={}", error);
                return ScmEndpointResult::AllowPortableFallback;
            }
            canStart = false;
            error = ERROR_SUCCESS;
        } else {
            return ScmEndpointResult::Unavailable;
        }
    }
    struct ServiceGuard { SC_HANDLE value; ~ServiceGuard() { if (value) CloseServiceHandle(value); } } serviceGuard{service};

    SERVICE_STATUS_PROCESS status{};
    DWORD bytesNeeded = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
        error = GetLastError();
        return canStart ? ScmEndpointResult::Unavailable : ScmEndpointResult::AllowPortableFallback;
    }

    bool startExplicitlyFailed = !canStart;
    bool spawnedInThisCall = false;
    auto action = tools3000::search::decideStartupAction(
        isAnySearchPipeReady(1),
        toScmServiceState(status.dwCurrentState), startExplicitlyFailed);
    if (action == tools3000::search::StartupAction::UseEndpoint) return ScmEndpointResult::Ready;
    if (action == tools3000::search::StartupAction::StartScmService) {
        const std::wstring tokenArg = L"--pipe-token=" +
            tools3000::core::WinUtils::utf8ToWstring(g_searchPipeToken);
        const std::wstring sidArg = L"--client-sid=" + g_searchClientSid;
        const wchar_t* args[] = {tokenArg.c_str(), sidArg.c_str()};
        if (!StartServiceW(service, static_cast<DWORD>(std::size(args)), args)) {
            error = GetLastError();
            // Another Tools3000 process can win the StartService race after our
            // status query. Its SCM service remains the only safe singleton;
            // never turn this specific result into a portable duplicate.
            if (error == ERROR_SERVICE_ALREADY_RUNNING) {
                if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                          reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
                    error = GetLastError();
                    return ScmEndpointResult::Unavailable;
                }
                action = tools3000::search::decideStartupAction(
                    isAnySearchPipeReady(1), toScmServiceState(status.dwCurrentState), false);
                if (action == tools3000::search::StartupAction::UseEndpoint) {
                    return ScmEndpointResult::Ready;
                }
                if (action == tools3000::search::StartupAction::WaitForScmEndpoint) {
                    // Continue into the bounded endpoint wait below.
                } else {
                    return ScmEndpointResult::Unavailable;
                }
            } else {
                // A stopped service whose start request was rejected has not
                // become a singleton, so portable mode remains compatible.
                startExplicitlyFailed = true;
                action = tools3000::search::decideStartupAction(false,
                    tools3000::search::ScmServiceState::Stopped, startExplicitlyFailed);
                return action == tools3000::search::StartupAction::AllowPortableFallback
                    ? ScmEndpointResult::AllowPortableFallback : ScmEndpointResult::Unavailable;
            }
        } else {
            spawnedInThisCall = true;
            g_serviceSpawnedByUs.store(true);
            g_serviceConfirmedReady.store(false, std::memory_order_relaxed);
        }
    } else if (action == tools3000::search::StartupAction::AllowPortableFallback) {
        LOG_WARN("SearchPlugin: SCM 无法为本用户启动（无启动权限或服务已停止），回退便携索引进程");
        return ScmEndpointResult::AllowPortableFallback;
    } else if (action != tools3000::search::StartupAction::WaitForScmEndpoint) {
        error = ERROR_SERVICE_NOT_ACTIVE;
        return ScmEndpointResult::Unavailable;
    }

    // Fast-diagnose (`HIGH-10`): If the SCM service was already `SERVICE_RUNNING` prior to this
    // call yet has not published any pipe, cap diagnostic wait at 2.5s instead of hanging 120s.
    const ULONGLONG nowStart = GetTickCount64();
    const bool alreadyRunningAtEntry =
        !spawnedInThisCall && (status.dwCurrentState == SERVICE_RUNNING);
    const ULONGLONG hardDeadline = nowStart + (alreadyRunningAtEntry ? 2'500ull : 30'000ull);
    DWORD previousCheckpoint = status.dwCheckPoint;
    ULONGLONG checkpointDeadline = nowStart + (alreadyRunningAtEntry ? 2'500ull : 10'000ull);
    ULONGLONG runningWithoutPipeSince = (status.dwCurrentState == SERVICE_RUNNING) ? nowStart : 0;
    auto lastKnownState = toScmServiceState(status.dwCurrentState);
    while (GetTickCount64() < hardDeadline) {
        if (g_searchPluginShuttingDown.load(std::memory_order_relaxed)) {
            error = ERROR_OPERATION_ABORTED;
            return ScmEndpointResult::Unavailable;
        }
        const std::string readyPipe = resolveActiveSearchPipe(100);
        if (!readyPipe.empty()) {
            // Quick probe: verify the pipe accepts open (or is legitimately busy with another worker)
            // so ACL/handshake refusal fails fast rather than spinning until timeout (`HIGH-10`).
            HANDLE probe = CreateFileA(
                readyPipe.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr);
            if (probe != INVALID_HANDLE_VALUE) {
                CloseHandle(probe);
                return ScmEndpointResult::Ready;
            }
            const DWORD probeErr = GetLastError();
            if (tools3000::search::isPipeTransientBusyError(probeErr)) {
                return ScmEndpointResult::Ready;
            }
            if (probeErr == ERROR_ACCESS_DENIED) {
                error = ERROR_ACCESS_DENIED;
                return ScmEndpointResult::Unavailable;
            }
        }
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
            error = GetLastError();
            return ScmEndpointResult::Unavailable;
        }
        const auto state = toScmServiceState(status.dwCurrentState);
        lastKnownState = state;
        if (state == tools3000::search::ScmServiceState::Stopped ||
            state == tools3000::search::ScmServiceState::Failed) {
            error = status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
            return ScmEndpointResult::AllowPortableFallback;
        }
        if (state == tools3000::search::ScmServiceState::StopPending) {
            error = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            return ScmEndpointResult::Unavailable;
        }
        const ULONGLONG loopNow = GetTickCount64();
        if (state == tools3000::search::ScmServiceState::Running) {
            if (runningWithoutPipeSince == 0) {
                runningWithoutPipeSince = loopNow;
            } else if (loopNow - runningWithoutPipeSince >= 3'000ull) {
                // `IPCServerThread` creates pipe listeners BEFORE indexing starts; if `Running`
                // persists >3s without an endpoint, diagnose immediately instead of waiting 120s.
                break;
            }
        }
        // SCM 的 checkpoint 有推进时，按照 WaitHint 延长当前阶段等待；没有推进
        // 则不能无限延长，防止损坏的服务永久占用客户端请求。
        if (state == tools3000::search::ScmServiceState::StartPending &&
            status.dwCheckPoint != previousCheckpoint) {
            previousCheckpoint = status.dwCheckPoint;
            const DWORD waitHint = (std::clamp)(status.dwWaitHint,
                                                static_cast<DWORD>(1'000),
                                                static_cast<DWORD>(15'000));
            checkpointDeadline = (std::min)(hardDeadline, loopNow + waitHint * 2ull);
        } else if (loopNow >= checkpointDeadline) {
            error = ERROR_TIMEOUT;
            return ScmEndpointResult::Unavailable;
        }
        const DWORD sleepMs = (std::clamp)(status.dwWaitHint / 10,
                                            static_cast<DWORD>(50),
                                            static_cast<DWORD>(250));
        Sleep(sleepMs);
    }
    // Since the service publishes the authenticated pipe before indexing, a
    // still-running SCM instance that never creates an accessible endpoint
    // belongs to an incompatible/stale deployment; surface that boundary fast.
    error = tools3000::search::isScmEndpointIdentityConflict(lastKnownState, false, true)
        ? ERROR_NOT_SUPPORTED : ERROR_TIMEOUT;
    return ScmEndpointResult::Unavailable;
}

static bool ensureSearchServiceRunning() {
    if (g_searchPipe.empty()) return false;
    if (isAnySearchPipeReady(100)) {
        return true;
    }

    // 查询走线程池并发执行，若不串行化，多个线程会在服务建好管道之前的空窗期里
    // 各自拉起一个索引进程，每个都会建一份完整索引。
    static std::mutex launchMutex;
    std::lock_guard<std::mutex> launchGuard(launchMutex);

    const std::wstring launchMutexName = g_forcePortableService
        ? (L"Local\\Tools3000SearchLaunch-" +
           tools3000::core::WinUtils::utf8ToWstring(g_searchPipeToken))
        : (L"Local\\Tools3000SearchLaunch-" + g_searchClientSid);
    HANDLE processLaunchMutex = CreateMutexW(nullptr, FALSE, launchMutexName.c_str());
    if (!processLaunchMutex) return false;
    struct LaunchMutexGuard {
        HANDLE value;
        bool locked = false;
        ~LaunchMutexGuard() {
            if (locked) ReleaseMutex(value);
            if (value) CloseHandle(value);
        }
    } processLaunchGuard{processLaunchMutex};
    const DWORD waitResult = WaitForSingleObject(processLaunchMutex, 10'000);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        SetLastError(waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        return false;
    }
    processLaunchGuard.locked = true;

    if (isAnySearchPipeReady(100)) {
        return true;
    }

    DWORD scmError = ERROR_SUCCESS;
    const auto scmResult = startScmServiceAndWait(scmError);
    if (scmResult == ScmEndpointResult::Ready) return true;
    if (scmResult == ScmEndpointResult::Unavailable) {
        SetLastError(scmError);
        LOG_WARN("SearchPlugin: SCM 服务未提供当前用户端点，拒绝启动第二个索引进程, error={}", scmError);
        return false;
    }

    // SCM 明确不存在、无法启动或已停止后，才允许便携进程回退。
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
    std::filesystem::path serviceExe = exeDir / L"Tools3000_Service.exe";
    std::error_code ec;
    if (!std::filesystem::exists(serviceExe, ec)) {
        serviceExe = exeDir / L"Tools3000_Service.exe";
    }

    if (std::filesystem::exists(serviceExe, ec)) {
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + serviceExe.wstring() + L"\" --pipe-token=" +
            tools3000::core::WinUtils::utf8ToWstring(g_searchPipeToken) + L" --client-sid=" + g_searchClientSid;
        if (g_forcePortableService) {
            cmd += L" --portable-isolated";
        }
        if (CreateProcessW(serviceExe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, exeDir.c_str(), &si, &pi)) {
            g_serviceSpawnedByUs.store(true);
            g_serviceConfirmedReady.store(false, std::memory_order_relaxed);
            // 便携模式也遵循“首次搜索后常驻”：不把索引进程加入
            // 主程序的 KILL_ON_JOB_CLOSE Job，使 Tools3000 关闭后它仍可服务。
            if (pi.hProcess) CloseHandle(pi.hProcess);
            if (pi.hThread) CloseHandle(pi.hThread);
        } else {
            // Preserve the launch failure. WaitNamedPipe below has its own error
            // path and must not hide a malformed executable/ACL/creation error.
            const DWORD launchError = GetLastError();
            SetLastError(launchError);
            return false;
        }
    } else {
        const DWORD missingServiceError = ec ? static_cast<DWORD>(ec.value()) : ERROR_FILE_NOT_FOUND;
        SetLastError(missingServiceError);
        return false;
    }

    if (isAnySearchPipeReady(3000)) return true;
    const DWORD pipeWaitError = GetLastError();
    SetLastError(pipeWaitError);
    return false;
}

// autoStart 为 false 时，服务没在跑就直接放弃本次调用。历史记录、数据库统计这类
// 辅助数据会在搜索窗 WebView 预热完成的瞬间被前端拉一遍，若允许它们拉起服务，
// 用户从没按过搜索热键也会在开机时凭空多出几百 MB 的索引进程。
std::optional<std::string> querySearchService(const std::string& query, DWORD& error,
                                              bool autoStart = true, DWORD timeoutMs = 120000) {
    error = ERROR_SUCCESS;
    if (g_searchPipe.empty()) {
        error = ERROR_ACCESS_DENIED;
        return std::nullopt;
    }
    const bool mayStart = autoStart && g_searchWindowExplicitlyShown.load();
    std::string activePipe = resolveActiveSearchPipe(mayStart ? 200 : 1);
    if (activePipe.empty()) {
        if (!mayStart) {
            error = ERROR_SERVICE_NOT_ACTIVE;
            return std::nullopt;
        }
        if (!ensureSearchServiceRunning()) {
            error = GetLastError();
            return std::nullopt;
        }
        activePipe = resolveActiveSearchPipe(1000);
        if (activePipe.empty()) {
            error = ERROR_FILE_NOT_FOUND;
            return std::nullopt;
        }
    }

    const DWORD connectBudgetMs = autoStart ? 4500 : 2000;
    const ULONGLONG connectDeadline = GetTickCount64() + connectBudgetMs;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD connectError = ERROR_SUCCESS;
    while (!g_searchPluginShuttingDown.load(std::memory_order_relaxed)) {
        pipe = CreateFileA(activePipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        connectError = GetLastError();
        if (connectError == ERROR_FILE_NOT_FOUND) {
            const std::string refreshed = resolveActiveSearchPipe(50);
            if (!refreshed.empty() && refreshed != activePipe) {
                activePipe = refreshed;
                continue;
            }
            break;
        }
        if (connectError != ERROR_PIPE_BUSY) {
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= connectDeadline) {
            break;
        }
        const DWORD waitSlice = static_cast<DWORD>((std::min<ULONGLONG>)(connectDeadline - now, 400));
        if (!WaitNamedPipeA(activePipe.c_str(), waitSlice)) {
            const DWORD waitErr = GetLastError();
            if (!tools3000::search::isPipeTransientBusyError(waitErr)) {
                connectError = waitErr;
                break;
            }
            Sleep(15);
        }
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        error = connectError != ERROR_SUCCESS ? connectError : GetLastError();
        return std::nullopt;
    }
    struct PipeGuard {
        HANDLE value;
        ~PipeGuard() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } pipeGuard{pipe};

    namespace frame = tools3000::service::pipe;

    if (!frame::fitsInFrame(query.size(), frame::MaxRequestBytes)) {
        error = ERROR_INVALID_PARAMETER;
        return std::nullopt;
    }

    PipeTransfer transfer;
    if (!transfer.valid()) {
        error = GetLastError();
        return std::nullopt;
    }

    const auto requestHeader = frame::encodeFrameHeader(static_cast<uint32_t>(query.size()));
    if (!transfer.writeExact(pipe, requestHeader.data(), requestHeader.size(), 3000, error) ||
        !transfer.writeExact(pipe, query.data(), query.size(), 3000, error)) {
        return std::nullopt;
    }

    // 帧头到达即表示服务端已完成计算，因此这一步承担查询本身的等待时间 (内容搜索支持最高 30 分钟)。
    char responseHeader[frame::HeaderSize] = {};
    if (!transfer.readExact(pipe, responseHeader, sizeof(responseHeader), timeoutMs, error)) {
        return std::nullopt;
    }

    uint32_t responseBytes = 0;
    if (!frame::decodeFrameHeader(responseHeader, responseBytes)) {
        error = ERROR_INVALID_DATA;
        return std::nullopt;
    }

    std::string response(responseBytes, '\0');
    if (!transfer.readExact(pipe, response.data(), responseBytes, timeoutMs, error)) {
        return std::nullopt;
    }
    return response;
}

static const std::unordered_map<std::string, std::vector<std::string>> CONTENT_CATEGORY_EXTS = {
    {"doc", {"doc", "dot", "docx", "docm", "dotx", "xls", "xlt", "xlsx", "xlsm", "xltx", "ppt", "pot", "pptx", "pptm", "potx", "wps", "wpt", "et", "ett", "dps", "dpt", "odt", "ods", "odp", "eml", "msg", "chm", "one", "pdf", "txt", "md", "markdown", "rtf", "csv", "tsv", "log", "epub", "tex", "bib", "rst", "adoc"}},
    {"code", {"c", "cpp", "cc", "cxx", "h", "hpp", "hxx", "cs", "rs", "go", "zig", "nim", "java", "kt", "kts", "scala", "groovy", "dart", "swift", "m", "mm", "js", "jsx", "mjs", "cjs", "ts", "tsx", "vue", "svelte", "astro", "html", "htm", "css", "scss", "sass", "less", "proto", "graphql", "gql", "thrift", "prisma", "json", "jsonc", "json5", "xml", "xaml", "yaml", "yml", "toml", "ini", "cfg", "conf", "config", "properties", "env", "reg", "lock", "plist", "prefs", "py", "pyw", "rb", "php", "pl", "pm", "lua", "sh", "bash", "zsh", "ps1", "psm1", "psd1", "bat", "cmd", "vbs", "ahk", "au3", "sql", "prc", "fnc", "trg", "pks", "pkb", "pls", "ch", "pld", "asm", "s", "glsl", "hlsl", "vert", "frag", "geom", "comp", "shader", "wgsl"}},
    {"design", {"dxf", "psd", "ai"}},
    {"archive", {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "iso"}}
};

static std::vector<std::string> getDisabledContentExtsFromConfig() {
    auto& cfg = tools3000::core::ConfigManager::instance();
    std::vector<std::string> disabled;
    for (const auto& [cat, exts] : CONTENT_CATEGORY_EXTS) {
        const std::string key = "/search/contentCategory_" + cat;
        const bool enabled = cfg.get<bool>(key, true);
        if (!enabled) {
            disabled.insert(disabled.end(), exts.begin(), exts.end());
        }
    }
    return disabled;
}

static std::vector<std::pair<std::string, double>> getDialogPriorityPathsFromConfig() {
    std::vector<std::pair<std::string, double>> result;
    auto& cfg = tools3000::core::ConfigManager::instance();
    auto favs = cfg.get<std::vector<std::string>>("/dialog/favorites", {});
    for (const auto& path : favs) {
        if (!path.empty()) result.emplace_back(path, 4500.0);
    }
    auto recents = cfg.get<std::vector<std::string>>("/dialog/recentPaths", {});
    for (std::size_t idx = 0; idx < recents.size(); ++idx) {
        const auto& path = recents[idx];
        if (!path.empty()) {
            const double decayed = (std::max)(2600.0, 4800.0 - static_cast<double>(idx) * 180.0);
            result.emplace_back(path, decayed);
        }
    }
    auto appMem = cfg.get<nlohmann::json>("/dialog/appMemories", nlohmann::json::object());
    if (appMem.is_object()) {
        for (const auto& [_, item] : appMem.items()) {
            if (item.is_object()) {
                std::string p = item.value("path", "");
                if (!p.empty()) result.emplace_back(p, 5200.0);
                std::string fw = item.value("fixedWorkspace", "");
                if (!fw.empty()) result.emplace_back(fw, 5500.0);
            }
        }
    }
    return result;
}

}  // namespace

namespace tools3000::search {

class SearchPlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Search"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("SearchPlugin: 初始化搜索引擎");
        g_searchPluginShuttingDown.store(false, std::memory_order_release);
        if (!initializePipeEndpoint()) return false;
        g_searchWindowExplicitlyShown.store(false);

        auto& mb = tools3000::core::MessageBridge::instance();
        
        mb.registerHandler("search.query", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object()) return invalidSearchRequest("search params must be an object");
            const auto queryIt = params.find("query");
            if (queryIt == params.end() || !queryIt->is_string()) {
                return invalidSearchRequest("query must be a string");
            }
            std::string query = queryIt->get<std::string>();
            if (query.empty()) {
                return {{"results", nlohmann::json::array()}, {"available", true}};
            }

            if (query.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
                return invalidSearchRequest("query exceeds 1024-byte limit");
            }
            // Expand desktop user environment variables (`%APPDATA%`, `%LOCALAPPDATA%`,
            // `%USERPROFILE%`, `%TEMP%`, etc.) on the interactive client before IPC (`MED-13`).
            query = expandClientUserEnvUtf8(query, tools3000::service::search_limits::MaxQueryUtf8Bytes);

            if (!validStringArray(params, "drives", tools3000::service::search_limits::MaxDriveItems, 2) ||
                !validStringArray(params, "excludes", tools3000::service::search_limits::MaxExcludeItems,
                                  tools3000::service::search_limits::MaxExcludeUtf8Bytes) ||
                !validStringArray(params, "contentCustomExts", tools3000::service::search_limits::MaxFormatItems,
                                  tools3000::service::search_limits::MaxFormatUtf8Bytes) ||
                !validStringArray(params, "contentDisabledExts", tools3000::service::search_limits::MaxFormatItems,
                                  tools3000::service::search_limits::MaxFormatUtf8Bytes)) {
                return invalidSearchRequest("search options exceed their limits");
            }

            nlohmann::json normalized = params;
            if (normalized.contains("excludes") && normalized["excludes"].is_array()) {
                for (auto& exItem : normalized["excludes"]) {
                    if (exItem.is_string()) {
                        exItem = expandClientUserEnvUtf8(
                            exItem.get<std::string>(),
                            tools3000::service::search_limits::MaxExcludeUtf8Bytes);
                    }
                }
            }
            {
                auto& cfg = tools3000::core::ConfigManager::instance();
                if (!normalized.contains("pinyinEnabled") || !normalized["pinyinEnabled"].is_boolean()) {
                    normalized["pinyinEnabled"] = cfg.get<bool>("/search/pinyinEnabled", true);
                }
                if (!normalized.contains("matchPath") || !normalized["matchPath"].is_boolean()) {
                    normalized["matchPath"] = cfg.get<bool>("/search/matchPath", false);
                }
                if (!normalized.contains("caseSensitive") || !normalized["caseSensitive"].is_boolean()) {
                    normalized["caseSensitive"] = cfg.get<bool>("/search/caseSensitive", false);
                }
            }

            // 合并前端局部禁用后缀与 ConfigManager 中 /search/contentCategory_* 的禁用类别后缀
            {
                const auto mergedDisabledExts = mergeDisabledContentExtensions(
                    normalized,
                    getDisabledContentExtsFromConfig(),
                    tools3000::service::search_limits::MaxFormatItems);
                if (!mergedDisabledExts.empty() || normalized.contains("contentDisabledExts")) {
                    normalized["contentDisabledExts"] = mergedDisabledExts;
                }
            }

            // 智能跨模块联动：将文件对话框增强 (DialogEnhancer) 记录的常用/收藏工作区路径下发给搜索引擎作为 Top 优先扫描源
            const auto dialogPriorities = getDialogPriorityPathsFromConfig();
            if (!dialogPriorities.empty()) {
                nlohmann::json diagArr = nlohmann::json::array();
                for (const auto& [dpath, score] : dialogPriorities) {
                    diagArr.push_back({{"path", dpath}, {"score", score}});
                }
                normalized["dialogPriorities"] = std::move(diagArr);
            }

            const std::string modeValue = normalized.value("searchMode", "name");
            const bool isContentSearch = modeValue == "content" ||
                                         modeValue == "both" ||
                                         hasExplicitContentQuerySyntax(query);
            const std::uint64_t autoDefaultLimit = isContentSearch
                ? tools3000::service::search_limits::DefaultContentMaxResults
                : tools3000::service::search_limits::MaxResults;

            if (const auto limitIt = normalized.find("limit"); limitIt != normalized.end()) {
                if (!limitIt->is_number_integer() && !limitIt->is_number_unsigned()) {
                    return invalidSearchRequest("limit must be an integer");
                }
                std::uint64_t limit = autoDefaultLimit;
                if (limitIt->is_number_unsigned()) {
                    limit = limitIt->get<std::uint64_t>();
                    if (limit == 0) limit = autoDefaultLimit;
                } else {
                    const auto signedLimit = limitIt->get<std::int64_t>();
                    limit = signedLimit <= 0 ? autoDefaultLimit
                                             : static_cast<std::uint64_t>(signedLimit);
                }
                normalized["limit"] = (std::min<std::uint64_t>)(
                    limit, tools3000::service::search_limits::MaxResults);
            }
            if (normalized.contains("progressive") && !normalized["progressive"].is_boolean()) {
                return invalidSearchRequest("progressive must be a boolean");
            }
            if (normalized.contains("cursor") && !normalized["cursor"].is_null() &&
                !normalized["cursor"].is_number_integer() && !normalized["cursor"].is_number_unsigned()) {
                return invalidSearchRequest("cursor must be an integer");
            }
            if (normalized.contains("planId") && !normalized["planId"].is_null() &&
                !normalized["planId"].is_number_integer() && !normalized["planId"].is_number_unsigned()) {
                return invalidSearchRequest("planId must be an integer");
            }
            normalized["query"] = query;
            const std::string payload = normalized.dump();

            const DWORD queryTimeout = isContentSearch ? 1800000 : 120000;
            DWORD pipeError = ERROR_SUCCESS;
            auto response = querySearchService(payload, pipeError, true, queryTimeout);

            if (response) {
                try {
                    auto result = nlohmann::json::parse(*response);
                    const bool isInit = result.value("initializing", false);
                    if (!isInit) {
                        g_serviceConfirmedReady.store(true, std::memory_order_relaxed);
                    }
                    result["available"] = !isInit;
                    result["status"] = isInit ? "starting" : "ready";
                    const auto resultIt = result.find("results");
                    const size_t resultCount = resultIt != result.end() && resultIt->is_array()
                        ? resultIt->size() : 0;
                    LOG_DEBUG("SearchPlugin query completed: mode={}, queryId={}, results={}, cancelled={}, elapsedMs={}",
                              modeValue, normalized.value("queryId", std::uint64_t{0}),
                              resultCount, result.value("cancelled", false), result.value("elapsedMs", 0));
                    return result;
                } catch (...) {
                    LOG_ERROR("SearchPlugin: 无法解析 JSON 结果");
                }
            } else {
                LOG_WARN("SearchPlugin: 管道调用超时或返回空, error={}", pipeError);
            }

            if (isPipeTransientBusyError(pipeError)) {
                return {
                    {"results", nlohmann::json::array()},
                    {"available", true},
                    {"status", "busy"},
                    {"busy", true},
                    {"error", "search service busy"}
                };
            }

            const bool isStarting = pipeError == ERROR_FILE_NOT_FOUND ||
                                    (!g_serviceConfirmedReady.load(std::memory_order_relaxed) &&
                                     (pipeError == ERROR_TIMEOUT || g_serviceSpawnedByUs.load()));
            const char* statusError = pipeError == ERROR_NOT_SUPPORTED
                ? "search service is attached to another Windows user or session"
                : (isStarting ? "search service starting" : "search service unavailable");
            return {
                {"results", nlohmann::json::array()},
                {"available", false},
                {"status", isStarting ? "starting" : "unavailable"},
                {"error", statusError}
            };
        });

        mb.registerHandler("search.rebuildIndex", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "rebuild";
            DWORD pipeError = ERROR_SUCCESS;
            auto resp = querySearchService(req.dump(), pipeError);
            if (resp) {
                try {
                    return nlohmann::json::parse(*resp);
                } catch (const nlohmann::json::exception& error) {
                    LOG_WARN("search.rebuildIndex 返回了无效 JSON: {}", error.what());
                }
            }
            return {{"success", false}};
        });

        mb.registerHandler("search.sync", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "catchup";
            DWORD pipeError = ERROR_SUCCESS;
            auto resp = querySearchService(req.dump(), pipeError, /*autoStart=*/false);
            if (resp) {
                try {
                    return nlohmann::json::parse(*resp);
                } catch (const nlohmann::json::exception& error) {
                    LOG_WARN("search.sync 返回了无效 JSON: {}", error.what());
                }
            }
            return {{"success", false}};
        });

        mb.registerHandler("search.getDrives", [](const nlohmann::json&) -> nlohmann::json {
            auto drives = tools3000::core::WinUtils::getSystemDrives();
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& d : drives) {
                arr.push_back({
                    {"letter", std::string(1, d.letter)},
                    {"path", tools3000::core::WinUtils::wstringToUtf8(d.path)},
                    {"volumeLabel", tools3000::core::WinUtils::wstringToUtf8(d.volumeLabel)},
                    {"fileSystem", tools3000::core::WinUtils::wstringToUtf8(d.fileSystem)},
                    {"type", tools3000::core::WinUtils::wstringToUtf8(d.typeStr)},
                    {"totalBytes", d.totalBytes},
                    {"freeBytes", d.freeBytes}
                });
            }
            return arr;
        });

        registerSearchInteractionHandlers(mb);

        // 只报告状态，不再顺手拉起服务：设置页一打开就会查一次状态，那不足以
        // 说明用户要用搜索。需要拉起时走 search.warmup。
        mb.registerHandler("search.getServiceStatus", [](const nlohmann::json&) -> nlohmann::json {
            return {
                {"available", !g_searchPipe.empty() && isAnySearchPipeReady(1)}
            };
        });

        mb.registerHandler("search.warmup", [](const nlohmann::json&) -> nlohmann::json {
            // warmup 只由 SearchWindow::show 在用户显式唤起搜索时发出。
            // 首次启动后服务常驻，直到 Windows 关机/重启或管理员停止。
            if (!g_searchWindowExplicitlyShown.load()) {
                return {{"available", false}, {"error", "search window was not explicitly shown"}};
            }
            return {{"available", ensureSearchServiceRunning()}};
        });

        mb.registerHandler("search.windowShown", [](const nlohmann::json&) -> nlohmann::json {
            g_searchWindowExplicitlyShown.store(true);
            return {{"success", true}};
        });

        mb.registerHandler("search.windowHidden", [](const nlohmann::json&) -> nlohmann::json {
            DWORD error = 0;
            querySearchService(R"({"action":"cancel"})", error, false);
            return {{"success", true}};
        });

        mb.registerHandler("search.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& cfg = tools3000::core::ConfigManager::instance();
            std::string hotkey = cfg.get<std::string>("/hotkeys/Toggle Search", "Alt+Space");
            int maxResults = cfg.get<int>("/search/maxResults", 50);
            std::string defaultCategory = cfg.get<std::string>("/search/defaultCategory", "all");
            bool caseSensitive = cfg.get<bool>("/search/caseSensitive", false);
            bool matchPath = cfg.get<bool>("/search/matchPath", false);
            bool pinyinEnabled = cfg.get<bool>("/search/pinyinEnabled", true);
            std::string enabledDrives = cfg.get<std::string>("/search/enabledDrives", "");
            std::string excludePatterns = cfg.get<std::string>("/search/excludePatterns", "$Recycle.Bin,System Volume Information,node_modules,.git,__pycache__");
            bool excludeHidden = cfg.get<bool>("/search/excludeHidden", false);
            bool excludeSystem = cfg.get<bool>("/search/excludeSystem", false);
            bool autoBypassFullscreen = cfg.get<bool>("/search/autoBypassFullscreen", true);
            bool catDoc = cfg.get<bool>("/search/contentCategory_doc", true);
            bool catCode = cfg.get<bool>("/search/contentCategory_code", true);
            bool catDesign = cfg.get<bool>("/search/contentCategory_design", true);
            bool catArchive = cfg.get<bool>("/search/contentCategory_archive", true);
            std::string iconStyle = cfg.get<std::string>("/search/iconStyle", "native");

            return {
                // 保留字段以兼容旧 UI/API，但服务生命周期现已固定为：
                // 首次显式唤起时按需启动，随后在本次 Windows 会话中常驻。
                {"residentInBackground", true},
                {"keepServiceRunning", true},
                {"hotkey", hotkey},
                {"maxResults", maxResults},
                {"defaultCategory", defaultCategory},
                {"caseSensitive", caseSensitive},
                {"matchPath", matchPath},
                {"pinyinEnabled", pinyinEnabled},
                {"enabledDrives", enabledDrives},
                {"excludePatterns", excludePatterns},
                {"excludeHidden", excludeHidden},
                {"excludeSystem", excludeSystem},
                {"autoBypassFullscreen", autoBypassFullscreen},
                {"iconStyle", iconStyle},
                {"contentCategory_doc", catDoc},
                {"contentCategory_code", catCode},
                {"contentCategory_design", catDesign},
                {"contentCategory_archive", catArchive}
            };
        });

        mb.registerHandler("search.saveSettings", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object()) {
                return {{"success", false}, {"error", "params must be an object"}};
            }
            auto& cfg = tools3000::core::ConfigManager::instance();
            nlohmann::json patch = nlohmann::json::object();
            nlohmann::json searchPatch = nlohmann::json::object();

            if (params.contains("hotkey") && params["hotkey"].is_string()) {
                const std::string newHotkey = params["hotkey"].get<std::string>();
                auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
                if (newHotkey.empty()) {
                    hotkeyMgr.clearHotkey("Toggle Search");
                    patch["hotkeys"] = {{"Toggle Search", ""}};
                } else if (const auto parsed = tools3000::core::HotkeyDef::fromString(newHotkey)) {
                    if (hotkeyMgr.rebindHotkey("Toggle Search", *parsed)) {
                        patch["hotkeys"] = {{"Toggle Search", parsed->toString()}};
                    } else {
                        patch["hotkeys"] = {{"Toggle Search", newHotkey}};
                    }
                }
            }
            if (params.contains("maxResults") && params["maxResults"].is_number()) {
                searchPatch["maxResults"] = params["maxResults"].get<int>();
            }
            if (params.contains("defaultCategory") && params["defaultCategory"].is_string()) {
                searchPatch["defaultCategory"] = params["defaultCategory"].get<std::string>();
            }
            if (params.contains("caseSensitive") && params["caseSensitive"].is_boolean()) {
                searchPatch["caseSensitive"] = params["caseSensitive"].get<bool>();
            }
            if (params.contains("matchPath") && params["matchPath"].is_boolean()) {
                searchPatch["matchPath"] = params["matchPath"].get<bool>();
            }
            if (params.contains("pinyinEnabled") && params["pinyinEnabled"].is_boolean()) {
                searchPatch["pinyinEnabled"] = params["pinyinEnabled"].get<bool>();
            }
            if (params.contains("enabledDrives") && params["enabledDrives"].is_string()) {
                searchPatch["enabledDrives"] = params["enabledDrives"].get<std::string>();
            }
            if (params.contains("excludePatterns") && params["excludePatterns"].is_string()) {
                searchPatch["excludePatterns"] = params["excludePatterns"].get<std::string>();
            }
            if (params.contains("excludeHidden") && params["excludeHidden"].is_boolean()) {
                searchPatch["excludeHidden"] = params["excludeHidden"].get<bool>();
            }
            if (params.contains("excludeSystem") && params["excludeSystem"].is_boolean()) {
                searchPatch["excludeSystem"] = params["excludeSystem"].get<bool>();
            }
            if (params.contains("contentCategory_doc") && params["contentCategory_doc"].is_boolean()) {
                searchPatch["contentCategory_doc"] = params["contentCategory_doc"].get<bool>();
            }
            if (params.contains("contentCategory_code") && params["contentCategory_code"].is_boolean()) {
                searchPatch["contentCategory_code"] = params["contentCategory_code"].get<bool>();
            }
            if (params.contains("contentCategory_design") && params["contentCategory_design"].is_boolean()) {
                searchPatch["contentCategory_design"] = params["contentCategory_design"].get<bool>();
            }
            if (params.contains("contentCategory_archive") && params["contentCategory_archive"].is_boolean()) {
                searchPatch["contentCategory_archive"] = params["contentCategory_archive"].get<bool>();
            }
            if (params.contains("autoBypassFullscreen") && params["autoBypassFullscreen"].is_boolean()) {
                searchPatch["autoBypassFullscreen"] = params["autoBypassFullscreen"].get<bool>();
            }
            if (params.contains("iconStyle") && params["iconStyle"].is_string()) {
                searchPatch["iconStyle"] = params["iconStyle"].get<std::string>();
            }
            if (!searchPatch.empty()) {
                patch["search"] = std::move(searchPatch);
            }
            if (!patch.empty() && !cfg.mergePatch(patch)) {
                return {{"success", false}, {"error", "failed to persist search settings"}};
            }
            return {{"success", true}};
        });

        mb.registerHandler("search.getFileIcon", [](const nlohmann::json& params) -> nlohmann::json {
            std::string ext = params.value("ext", "");
            bool isDir = params.value("isDirectory", false);
            std::string path = params.value("path", "");
            if (ext.empty() && !isDir && path.size() >= 6 &&
                _strnicmp(path.c_str(), "shell:", 6) == 0) {
                ext = ".exe";
            }
            std::string base64 = tools3000::core::WinUtils::getFileTypeIconBase64(
                tools3000::core::WinUtils::utf8ToWstring(ext),
                isDir,
                tools3000::core::WinUtils::utf8ToWstring(path));
            return {{"success", true}, {"iconBase64", base64}};
        });

        mb.registerHandler("search.batchGetIcons", [](const nlohmann::json& params) -> nlohmann::json {
            nlohmann::json result = nlohmann::json::object();
            if (params.contains("items") && params["items"].is_array()) {
                for (const auto& item : params["items"]) {
                    std::string ext = item.value("ext", "");
                    bool isDir = item.value("isDirectory", false);
                    std::string path = item.value("path", "");
                    std::string key = item.value("key", "");
                    if (key.empty()) {
                        key = isDir ? "::dir::" : (!path.empty() ? ("path:" + tools3000::core::WinUtils::toLower(path)) : ext);
                    }
                    if (ext.empty() && !isDir && path.size() >= 6 &&
                        _strnicmp(path.c_str(), "shell:", 6) == 0) {
                        ext = ".exe";
                    }
                    if (!result.contains(key)) {
                        result[key] = tools3000::core::WinUtils::getFileTypeIconBase64(
                            tools3000::core::WinUtils::utf8ToWstring(ext),
                            isDir,
                            tools3000::core::WinUtils::utf8ToWstring(path));
                    }
                }
            }
            return {{"success", true}, {"icons", result}};
        });

        mb.registerHandler("search.recordRun", [](const nlohmann::json& params) -> nlohmann::json {
            std::string path = expandClientUserEnvUtf8(
                params.value("path", ""),
                tools3000::service::search_limits::MaxPathUtf8Bytes);
            if (path.empty()) return {{"success", false}};
            nlohmann::json req;
            req["action"] = "recordRun";
            req["path"] = path;
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err, /*autoStart=*/false);
            return {{"success", res.has_value()}};
        });

        mb.registerHandler("search.recordSearch", [](const nlohmann::json& params) -> nlohmann::json {
            std::string query = params.value("query", "");
            if (query.empty()) return {{"success", false}};
            nlohmann::json req;
            req["action"] = "recordSearch";
            req["query"] = query;
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err, /*autoStart=*/false);
            return {{"success", res.has_value()}};
        });

        mb.registerHandler("search.getSearchHistory", [](const nlohmann::json& params) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "getSearchHistory";
            if (params.contains("limit")) req["limit"] = params["limit"];
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err, /*autoStart=*/false);
            if (res && !res->empty()) {
                try {
                    return nlohmann::json::parse(*res);
                } catch (const nlohmann::json::exception& error) {
                    LOG_WARN("search.getSearchHistory 返回了无效 JSON: {}", error.what());
                }
            }
            return {{"success", false}, {"history", nlohmann::json::array()}};
        });

        mb.registerHandler("search.removeSearchHistory", [](const nlohmann::json& params) -> nlohmann::json {
            std::string search = params.value("search", "");
            nlohmann::json req;
            req["action"] = "removeSearchHistory";
            req["search"] = search;
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err);
            return {{"success", res.has_value()}};
        });

        mb.registerHandler("search.clearSearchHistory", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "clearSearchHistory";
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err);
            return {{"success", res.has_value()}};
        });

        mb.registerHandler("search.getDbStats", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "getDbStats";
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err, /*autoStart=*/false);
            if (res && !res->empty()) {
                try {
                    return nlohmann::json::parse(*res);
                } catch (const nlohmann::json::exception& error) {
                    LOG_WARN("search.getDbStats 返回了无效 JSON: {}", error.what());
                }
            }
            return {{"success", false}};
        });

        mb.registerHandler("search.saveSnapshot", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json req;
            req["action"] = "saveSnapshot";
            DWORD err = 0;
            auto res = querySearchService(req.dump(), err);
            return {{"success", res.has_value()}};
        });

        // 这些处理器要跨进程等待搜索服务或执行潜在阻塞的磁盘/Shell 图标提取。放在
        // WebView2 的 UI 线程上同步执行会让搜索窗口在整个等待期间无法响应键盘
        // 输入。打开文件、定位文件夹、属性窗口和右键菜单等 Shell 操作有前台窗口/
        // COM 线程亲和性，必须留在调用窗口的同步路径上；其内部需要时会自行异步。
        for (const char* method : {
                 "search.query",
                 "search.sync",
                 "search.rebuildIndex",
                 "search.getSearchHistory",
                 "search.removeSearchHistory",
                 "search.clearSearchHistory",
                 "search.recordRun",
                 "search.recordSearch",
                 "search.windowHidden",
                 "search.getDrives",
                 "search.getFileIcon",
                 "search.batchGetIcons",
                 "search.getDbStats",
                 "search.saveSnapshot",
                 "search.warmup",
                 "search.renamePath",
             }) {
            mb.markMethodAsync(method);
        }

        return true;
    }

    void shutdown() override {
        LOG_INFO("SearchPlugin: 关闭");
        g_searchPluginShuttingDown.store(true, std::memory_order_release);
        tools3000::core::MessageBridge::instance().unregisterHandlersByPrefix("search.");
        // 索引服务一旦因显式搜索需求启动，即独立常驻。
        // 主程序退出不发送 shutdown；SCM 的 demand start 保证下次开机不自启。
    }
};

} // namespace tools3000::search

extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin() {
    static tools3000::search::SearchPlugin instance;
    return &instance;
}

extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}
