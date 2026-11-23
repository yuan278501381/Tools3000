/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "core/ipc/SearchPipeClient.h"

#include "core/config/ConfigManager.h"
#include "core/i18n/UiI18n.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "service/PipeEndpoint.h"
#include "service/PipeProtocol.h"
#include "service/SearchRequestLimits.h"

#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace tools3000::core::ipc {

namespace {

constexpr DWORD kDefaultWriteTimeoutMs = 1500;
constexpr DWORD kOverlappedPollSliceMs = 40;
constexpr DWORD kLaunchMutexWaitMs = 10'000;
constexpr std::size_t kPipeChunkBytes = 64 * 1024;
constexpr std::size_t kMaxDialogPriorities = 32;

bool isPipeTransientBusyError(DWORD err) noexcept {
    return err == ERROR_PIPE_BUSY || err == ERROR_SEM_TIMEOUT;
}

bool isTrimSpace(unsigned char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string trimAsciiWhitespace(std::string_view input) {
    size_t start = 0;
    while (start < input.size() && isTrimSpace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && isTrimSpace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

bool containsExplicitTypeOperator(std::string_view query) noexcept {
    auto matchPrefixAt = [&](size_t pos, std::string_view op) noexcept -> bool {
        if (pos + op.size() > query.size()) return false;
        for (size_t i = 0; i < op.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(query[pos + i])));
            if (a != op[i]) return false;
        }
        return true;
    };

    for (size_t i = 0; i < query.size(); ++i) {
        const bool atTokenBoundary = (i == 0) ||
            isTrimSpace(static_cast<unsigned char>(query[i - 1])) ||
            query[i - 1] == '!' || query[i - 1] == '-';
        if (!atTokenBoundary) continue;
        if (matchPrefixAt(i, "folder:") || matchPrefixAt(i, "dir:") || matchPrefixAt(i, "file:")) {
            return true;
        }
    }
    return false;
}

bool hasCommandLineFlagW(std::wstring_view wanted) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    struct ArgsGuard {
        LPWSTR* ptr;
        ~ArgsGuard() noexcept {
            if (ptr) LocalFree(ptr);
        }
    } guard{argv};

    std::wstring wantedStr(wanted);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], wantedStr.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> queryCurrentUserSidW() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::nullopt;
    }
    struct TokenGuard {
        HANDLE value;
        ~TokenGuard() noexcept {
            if (value) CloseHandle(value);
        }
    } guard{token};

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) return std::nullopt;

    std::vector<std::byte> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        return std::nullopt;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid) || !rawSid) {
        return std::nullopt;
    }
    std::wstring sid(rawSid);
    LocalFree(rawSid);
    return sid;
}

std::optional<std::string> generatePipeTokenHex() {
    std::array<unsigned char, tools3000::service::pipe_endpoint::TokenHexLength / 2> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return std::nullopt;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string expandUserEnvUtf8(const std::string& input, std::size_t maxUtf8Bytes) {
    if (input.find('%') == std::string::npos) {
        return input;
    }
    const std::wstring wideInput = WinUtils::utf8ToWstring(input);
    if (wideInput.empty()) return input;
    constexpr DWORD kMaxExpandedChars = 4096;
    std::vector<wchar_t> buffer(kMaxExpandedChars, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(wideInput.c_str(), buffer.data(), kMaxExpandedChars);
    if (written == 0 || written > kMaxExpandedChars) {
        return input;
    }
    const std::string expanded = WinUtils::wstringToUtf8(
        std::wstring(buffer.data(), static_cast<std::size_t>(written - 1)));
    if (expanded.empty() || expanded.size() > maxUtf8Bytes) {
        return input;
    }
    return expanded;
}

bool resolveEffectivePinyinEnabled() {
    const auto lang = UiI18n::getEffectiveLanguage();
    const bool defaultPinyin = (lang == UiLanguage::ZhCN || lang == UiLanguage::ZhTW);
    return ConfigManager::instance().get<bool>("/search/pinyinEnabled", defaultPinyin);
}

enum class ScmState {
    Missing,
    Stopped,
    StartPending,
    Running,
    StopPending,
    Failed,
};

enum class ScmOutcome {
    Ready,
    AllowPortableFallback,
    Unavailable,
};

ScmState toScmState(DWORD currentState) noexcept {
    switch (currentState) {
        case SERVICE_STOPPED: return ScmState::Stopped;
        case SERVICE_START_PENDING: return ScmState::StartPending;
        case SERVICE_RUNNING: return ScmState::Running;
        case SERVICE_STOP_PENDING: return ScmState::StopPending;
        default: return ScmState::Failed;
    }
}

}  // namespace

struct SearchPipeClient::Impl {
    mutable std::mutex endpointMutex;
    bool endpointInitialized{false};
    bool forcePortableService{false};
    std::wstring clientSid;
    std::string pipeToken;
    std::wstring kernelServicePipeW;
    std::wstring portableSidPipeW;
    std::wstring tokenPipeW;

    std::mutex launchMutex;
    std::mutex warmupMutex;
    std::thread warmupWorker;
    std::vector<std::function<void(bool)>> warmupCallbacks;
    std::atomic<bool> warmupInFlight{false};
    std::atomic<bool> serviceSpawnedByUs{false};
    std::atomic<bool> serviceConfirmedReady{false};
    std::atomic<bool> shuttingDown{false};
    std::atomic<uint64_t> cancelEpoch{1};

    std::mutex activeHandlesMutex;
    std::vector<HANDLE> activeHandles;

    bool ensureEndpointInitialized() {
        std::lock_guard lock(endpointMutex);
        if (endpointInitialized) {
            return !tokenPipeW.empty() && !clientSid.empty();
        }
        endpointInitialized = true;

        auto sid = queryCurrentUserSidW();
        if (!sid || sid->empty()) {
            LOG_ERROR("SearchPipeClient: failed to resolve current user SID");
            return false;
        }
        forcePortableService = hasCommandLineFlagW(L"--force-portable-search-service");
        kernelServicePipeW = WinUtils::utf8ToWstring(
            std::string(tools3000::service::pipe_endpoint::KernelAuthPipeName));

        auto sidPipeUtf8 = tools3000::service::pipe_endpoint::makeUserSidPipeName(*sid);
        if (!sidPipeUtf8) {
            LOG_ERROR("SearchPipeClient: failed to build per-SID pipe endpoint");
            return false;
        }

        auto& cfg = ConfigManager::instance();
        std::string token = cfg.get<std::string>("/search/pipeToken", "");
        if (!tools3000::service::pipe_endpoint::isValidToken(token)) {
            auto generated = generatePipeTokenHex();
            if (!generated) {
                LOG_ERROR("SearchPipeClient: failed to generate secure pipe token");
                return false;
            }
            (void)cfg.set("/search/pipeToken", *generated);
            token = std::move(*generated);
        }

        auto tokenPipeUtf8 = tools3000::service::pipe_endpoint::makePipeName(token);
        if (!tokenPipeUtf8) {
            return false;
        }

        clientSid = std::move(*sid);
        pipeToken = std::move(token);
        portableSidPipeW = WinUtils::utf8ToWstring(*sidPipeUtf8);
        tokenPipeW = WinUtils::utf8ToWstring(*tokenPipeUtf8);
        return true;
    }

    static bool isPipePresentOrBusyW(const std::wstring& pipeNameW, DWORD waitMs) noexcept {
        if (pipeNameW.empty()) return false;
        if (WaitNamedPipeW(pipeNameW.c_str(), waitMs) != FALSE) {
            return true;
        }
        return isPipeTransientBusyError(GetLastError());
    }

    std::wstring resolveActivePipeW(DWORD primaryWaitMs) {
        if (!ensureEndpointInitialized()) return {};
        bool forcePortable = false;
        std::wstring kernelPipe;
        std::wstring sidPipe;
        std::wstring tokPipe;
        {
            std::lock_guard lock(endpointMutex);
            forcePortable = forcePortableService;
            kernelPipe = kernelServicePipeW;
            sidPipe = portableSidPipeW;
            tokPipe = tokenPipeW;
        }

        if (forcePortable) {
            return isPipePresentOrBusyW(tokPipe, primaryWaitMs) ? tokPipe : std::wstring{};
        }
        if (isPipePresentOrBusyW(kernelPipe, primaryWaitMs)) {
            return kernelPipe;
        }
        if (isPipePresentOrBusyW(sidPipe, 1)) {
            return sidPipe;
        }
        if (isPipePresentOrBusyW(tokPipe, 1)) {
            return tokPipe;
        }
        return {};
    }

    void registerActiveHandle(HANDLE pipe) {
        if (pipe == INVALID_HANDLE_VALUE) return;
        std::lock_guard lock(activeHandlesMutex);
        activeHandles.push_back(pipe);
    }

    void unregisterActiveHandle(HANDLE pipe) {
        if (pipe == INVALID_HANDLE_VALUE) return;
        std::lock_guard lock(activeHandlesMutex);
        activeHandles.erase(
            std::remove(activeHandles.begin(), activeHandles.end(), pipe),
            activeHandles.end());
    }

    bool finishOverlapped(HANDLE pipe,
                          OVERLAPPED& overlapped,
                          DWORD timeoutMs,
                          uint64_t expectedCancelEpoch,
                          const std::atomic<bool>* externalCancel,
                          DWORD& transferred,
                          DWORD& error) const {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (true) {
            if (shuttingDown.load(std::memory_order_acquire) ||
                cancelEpoch.load(std::memory_order_acquire) != expectedCancelEpoch ||
                (externalCancel && externalCancel->load(std::memory_order_acquire))) {
                error = ERROR_OPERATION_ABORTED;
                CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                error = ERROR_TIMEOUT;
                CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return false;
            }
            const DWORD remainMs = static_cast<DWORD>(deadline - now);
            const DWORD waitChunk = (std::min)(kOverlappedPollSliceMs, remainMs);
            const DWORD waitRes = WaitForSingleObject(overlapped.hEvent, waitChunk);
            if (waitRes == WAIT_OBJECT_0) {
                if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                    return true;
                }
                error = GetLastError();
                return false;
            }
            if (waitRes != WAIT_TIMEOUT) {
                error = GetLastError();
                CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return false;
            }
        }
    }

    bool transferExact(HANDLE pipe,
                       HANDLE eventHandle,
                       char* buffer,
                       size_t bytes,
                       DWORD timeoutMs,
                       uint64_t expectedCancelEpoch,
                       const std::atomic<bool>* externalCancel,
                       DWORD& error,
                       bool writing) const {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        size_t done = 0;
        while (done < bytes) {
            if (shuttingDown.load(std::memory_order_acquire) ||
                cancelEpoch.load(std::memory_order_acquire) != expectedCancelEpoch ||
                (externalCancel && externalCancel->load(std::memory_order_acquire))) {
                error = ERROR_OPERATION_ABORTED;
                return false;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                error = ERROR_TIMEOUT;
                return false;
            }
            const DWORD remainMs = static_cast<DWORD>(deadline - now);
            const DWORD want = static_cast<DWORD>((std::min)(bytes - done, kPipeChunkBytes));

            OVERLAPPED overlapped{};
            ResetEvent(eventHandle);
            overlapped.hEvent = eventHandle;

            DWORD moved = 0;
            const BOOL ok = writing
                ? WriteFile(pipe, buffer + done, want, &moved, &overlapped)
                : ReadFile(pipe, buffer + done, want, &moved, &overlapped);
            if (!ok) {
                error = GetLastError();
                if (error != ERROR_IO_PENDING ||
                    !finishOverlapped(pipe, overlapped, remainMs, expectedCancelEpoch,
                                      externalCancel, moved, error)) {
                    return false;
                }
            }
            if (moved == 0) {
                error = ERROR_NO_DATA;
                return false;
            }
            done += moved;
        }
        return true;
    }

    ScmOutcome startScmServiceAndWait(DWORD& error) {
        error = ERROR_SUCCESS;
        if (forcePortableService) {
            return ScmOutcome::AllowPortableFallback;
        }

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            error = GetLastError();
            return error == ERROR_ACCESS_DENIED ? ScmOutcome::AllowPortableFallback
                                                : ScmOutcome::Unavailable;
        }
        struct ScmGuard {
            SC_HANDLE value;
            ~ScmGuard() noexcept {
                if (value) CloseServiceHandle(value);
            }
        } scmGuard{scm};

        SC_HANDLE service = OpenServiceW(scm, L"Tools3000_SearchService",
                                         SERVICE_START | SERVICE_QUERY_STATUS);
        bool canStart = true;
        if (!service) {
            error = GetLastError();
            if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
                return ScmOutcome::AllowPortableFallback;
            }
            if (error == ERROR_ACCESS_DENIED) {
                service = OpenServiceW(scm, L"Tools3000_SearchService", SERVICE_QUERY_STATUS);
                if (!service) {
                    error = GetLastError();
                    return ScmOutcome::AllowPortableFallback;
                }
                canStart = false;
                error = ERROR_SUCCESS;
            } else {
                return ScmOutcome::Unavailable;
            }
        }
        struct ServiceGuard {
            SC_HANDLE value;
            ~ServiceGuard() noexcept {
                if (value) CloseServiceHandle(value);
            }
        } serviceGuard{service};

        SERVICE_STATUS_PROCESS status{};
        DWORD bytesNeeded = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
            error = GetLastError();
            return canStart ? ScmOutcome::Unavailable : ScmOutcome::AllowPortableFallback;
        }

        if (!resolveActivePipeW(1).empty()) {
            return ScmOutcome::Ready;
        }

        bool spawnedInThisCall = false;
        ScmState state = toScmState(status.dwCurrentState);
        if (state == ScmState::Stopped) {
            if (!canStart) {
                return ScmOutcome::AllowPortableFallback;
            }
            const std::wstring tokenArg = L"--pipe-token=" + WinUtils::utf8ToWstring(pipeToken);
            const std::wstring sidArg = L"--client-sid=" + clientSid;
            const wchar_t* args[] = {tokenArg.c_str(), sidArg.c_str()};
            if (!StartServiceW(service, static_cast<DWORD>(std::size(args)), args)) {
                error = GetLastError();
                if (error == ERROR_SERVICE_ALREADY_RUNNING) {
                    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                              reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
                        error = GetLastError();
                        return ScmOutcome::Unavailable;
                    }
                    state = toScmState(status.dwCurrentState);
                } else {
                    return ScmOutcome::AllowPortableFallback;
                }
            } else {
                spawnedInThisCall = true;
                serviceSpawnedByUs.store(true, std::memory_order_release);
                serviceConfirmedReady.store(false, std::memory_order_release);
            }
        } else if (state == ScmState::StopPending || state == ScmState::Failed) {
            error = ERROR_SERVICE_NOT_ACTIVE;
            return ScmOutcome::Unavailable;
        }

        const ULONGLONG nowStart = GetTickCount64();
        const bool alreadyRunningAtEntry = !spawnedInThisCall && (state == ScmState::Running);
        const ULONGLONG hardDeadline = nowStart + (alreadyRunningAtEntry ? 2'500ull : 20'000ull);
        DWORD previousCheckpoint = status.dwCheckPoint;
        ULONGLONG checkpointDeadline = nowStart + (alreadyRunningAtEntry ? 2'500ull : 10'000ull);
        ULONGLONG runningWithoutPipeSince = (state == ScmState::Running) ? nowStart : 0;

        while (GetTickCount64() < hardDeadline) {
            if (shuttingDown.load(std::memory_order_acquire)) {
                error = ERROR_OPERATION_ABORTED;
                return ScmOutcome::Unavailable;
            }
            const std::wstring readyPipe = resolveActivePipeW(80);
            if (!readyPipe.empty()) {
                HANDLE probe = CreateFileW(
                    readyPipe.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    nullptr);
                if (probe != INVALID_HANDLE_VALUE) {
                    CloseHandle(probe);
                    return ScmOutcome::Ready;
                }
                const DWORD probeErr = GetLastError();
                if (isPipeTransientBusyError(probeErr)) {
                    return ScmOutcome::Ready;
                }
                if (probeErr == ERROR_ACCESS_DENIED) {
                    error = ERROR_ACCESS_DENIED;
                    return ScmOutcome::Unavailable;
                }
            }

            if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                      reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytesNeeded)) {
                error = GetLastError();
                return ScmOutcome::Unavailable;
            }
            state = toScmState(status.dwCurrentState);
            if (state == ScmState::Stopped || state == ScmState::Failed) {
                error = status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
                return ScmOutcome::AllowPortableFallback;
            }
            if (state == ScmState::StopPending) {
                error = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
                return ScmOutcome::Unavailable;
            }
            const ULONGLONG loopNow = GetTickCount64();
            if (state == ScmState::Running) {
                if (runningWithoutPipeSince == 0) {
                    runningWithoutPipeSince = loopNow;
                } else if (loopNow - runningWithoutPipeSince >= 3'000ull) {
                    break;
                }
            }
            if (state == ScmState::StartPending && status.dwCheckPoint != previousCheckpoint) {
                previousCheckpoint = status.dwCheckPoint;
                const DWORD waitHint = std::clamp<DWORD>(status.dwWaitHint, 1'000, 15'000);
                checkpointDeadline = (std::min)(hardDeadline, loopNow + waitHint * 2ull);
            } else if (loopNow >= checkpointDeadline) {
                error = ERROR_TIMEOUT;
                return ScmOutcome::Unavailable;
            }
            Sleep(std::clamp<DWORD>(status.dwWaitHint / 10, 50, 200));
        }

        error = (state == ScmState::Running) ? ERROR_NOT_SUPPORTED : ERROR_TIMEOUT;
        return ScmOutcome::Unavailable;
    }

    bool ensureServiceRunningOnDemand() {
        if (!ensureEndpointInitialized()) return false;
        if (!resolveActivePipeW(60).empty()) {
            return true;
        }

        std::lock_guard localLock(launchMutex);
        if (!resolveActivePipeW(60).empty()) {
            return true;
        }

        // 与 Plugin_Search 严格共享同一防双开命名互斥量，确保全会话单实例搜索服务
        const std::wstring launchMutexName = forcePortableService
            ? (L"Local\\Tools3000SearchLaunch-" + WinUtils::utf8ToWstring(pipeToken))
            : (L"Local\\Tools3000SearchLaunch-" + clientSid);

        HANDLE procMutex = CreateMutexW(nullptr, FALSE, launchMutexName.c_str());
        if (!procMutex) return false;
        struct MutexGuard {
            HANDLE handle;
            bool locked{false};
            ~MutexGuard() noexcept {
                if (locked) ReleaseMutex(handle);
                if (handle) CloseHandle(handle);
            }
        } procMutexGuard{procMutex};

        const DWORD waitRes = WaitForSingleObject(procMutex, kLaunchMutexWaitMs);
        if (waitRes != WAIT_OBJECT_0 && waitRes != WAIT_ABANDONED) {
            SetLastError(waitRes == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
            return false;
        }
        procMutexGuard.locked = true;

        if (!resolveActivePipeW(80).empty()) {
            return true;
        }

        DWORD scmError = ERROR_SUCCESS;
        const ScmOutcome scmResult = startScmServiceAndWait(scmError);
        if (scmResult == ScmOutcome::Ready) {
            return true;
        }
        if (scmResult == ScmOutcome::Unavailable) {
            SetLastError(scmError);
            LOG_WARN("SearchPipeClient: SCM service unavailable for current user, refusing duplicate portable launch, error={}", scmError);
            return false;
        }

        // SCM 明确缺失或不可启动后，按需启动便携进程（不挂主进程 Job，满足首次搜索后本次 Windows 会话常驻）
        const std::filesystem::path exeDir = WinUtils::getExeDirectory();
        const std::filesystem::path serviceExe = exeDir / L"Tools3000_Service.exe";
        std::error_code ec;
        if (!std::filesystem::exists(serviceExe, ec)) {
            SetLastError(ec ? static_cast<DWORD>(ec.value()) : ERROR_FILE_NOT_FOUND);
            return false;
        }

        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + serviceExe.wstring() + L"\" --pipe-token=" +
            WinUtils::utf8ToWstring(pipeToken) + L" --client-sid=" + clientSid;
        if (forcePortableService) {
            cmd += L" --portable-isolated";
        }
        if (!CreateProcessW(serviceExe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, exeDir.c_str(), &si, &pi)) {
            const DWORD launchErr = GetLastError();
            SetLastError(launchErr);
            LOG_WARN("SearchPipeClient: failed to launch portable search service, error={}", launchErr);
            return false;
        }
        serviceSpawnedByUs.store(true, std::memory_order_release);
        serviceConfirmedReady.store(false, std::memory_order_release);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);

        return !resolveActivePipeW(3000).empty();
    }
};

SearchPipeClient& SearchPipeClient::instance() {
    static SearchPipeClient s_instance;
    return s_instance;
}

SearchPipeClient::SearchPipeClient() : m_impl(new Impl()) {}

SearchPipeClient::~SearchPipeClient() {
    shutdown();
    delete m_impl;
    m_impl = nullptr;
}

std::string SearchPipeClient::buildEffectiveQuery(const std::string& rawQuery, bool onlyFolders) {
    const std::string trimmed = trimAsciiWhitespace(rawQuery);
    if (trimmed.empty()) return {};
    if (!onlyFolders) return trimmed;
    if (containsExplicitTypeOperator(trimmed)) return trimmed;
    return "folder: " + trimmed;
}

bool SearchPipeClient::shouldAllowAutoStart(
    const std::string& rawQuery,
    bool explicitUserTyped,
    bool autoStartRequested) noexcept {
    if (!autoStartRequested || !explicitUserTyped) return false;
    for (unsigned char ch : rawQuery) {
        if (!isTrimSpace(ch)) return true;
    }
    return false;
}

bool SearchPipeClient::isServiceRunningNow(DWORD waitMs) const {
    if (!m_impl || m_impl->shuttingDown.load(std::memory_order_acquire)) return false;
    return !m_impl->resolveActivePipeW(waitMs).empty();
}

bool SearchPipeClient::isWarmupInFlight() const noexcept {
    return m_impl && m_impl->warmupInFlight.load(std::memory_order_acquire);
}

void SearchPipeClient::warmupAsync(std::function<void(bool ready)> onReady) {
    if (!m_impl || m_impl->shuttingDown.load(std::memory_order_acquire)) {
        if (onReady) onReady(false);
        return;
    }
    if (isServiceRunningNow(1)) {
        if (onReady) onReady(true);
        return;
    }

    bool shouldSpawnWorker = false;
    {
        std::lock_guard lock(m_impl->warmupMutex);
        if (onReady) {
            m_impl->warmupCallbacks.push_back(std::move(onReady));
        }
        if (!m_impl->warmupInFlight.exchange(true, std::memory_order_acq_rel)) {
            if (m_impl->warmupWorker.joinable() &&
                m_impl->warmupWorker.get_id() != std::this_thread::get_id()) {
                m_impl->warmupWorker.join();
            }
            shouldSpawnWorker = true;
        }
    }

    if (shouldSpawnWorker) {
        std::lock_guard lock(m_impl->warmupMutex);
        m_impl->warmupWorker = std::thread([impl = m_impl]() noexcept {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            bool ready = false;
            try {
                ready = impl->ensureServiceRunningOnDemand();
            } catch (...) {
                ready = false;
            }
            impl->warmupInFlight.store(false, std::memory_order_release);
            std::vector<std::function<void(bool)>> callbacks;
            {
                std::lock_guard cbLock(impl->warmupMutex);
                callbacks.swap(impl->warmupCallbacks);
            }
            if (!impl->shuttingDown.load(std::memory_order_acquire)) {
                for (auto& cb : callbacks) {
                    if (!cb) continue;
                    try {
                        cb(ready);
                    } catch (...) {
                    }
                }
            }
        });
    }
}

void SearchPipeClient::cancelActiveQueries() {
    if (!m_impl) return;
    m_impl->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lock(m_impl->activeHandlesMutex);
    for (HANDLE h : m_impl->activeHandles) {
        if (h != INVALID_HANDLE_VALUE) {
            CancelIoEx(h, nullptr);
        }
    }
}

void SearchPipeClient::shutdown() {
    if (!m_impl) return;
    m_impl->shuttingDown.store(true, std::memory_order_release);
    cancelActiveQueries();
    std::thread workerToJoin;
    {
        std::lock_guard lock(m_impl->warmupMutex);
        m_impl->warmupCallbacks.clear();
        if (m_impl->warmupWorker.joinable() &&
            m_impl->warmupWorker.get_id() != std::this_thread::get_id()) {
            workerToJoin = std::move(m_impl->warmupWorker);
        }
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

SearchQueryResponse SearchPipeClient::queryFiles(
    const std::string& rawQuery,
    uint64_t queryId,
    size_t limit,
    bool onlyFolders,
    const std::vector<DialogPriorityEntry>& dialogPriorities,
    bool autoStart,
    DWORD timeoutMs,
    const std::atomic<bool>* externalCancel) {
    SearchQueryResponse resp;
    resp.queryId = queryId;

    if (!m_impl || m_impl->shuttingDown.load(std::memory_order_acquire)) {
        resp.status = SearchPipeStatus::Cancelled;
        resp.cancelled = true;
        return resp;
    }

    const std::string effectiveQuery = buildEffectiveQuery(rawQuery, onlyFolders);
    if (effectiveQuery.empty()) {
        resp.status = SearchPipeStatus::Ready;
        return resp;
    }

    if (effectiveQuery.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
        resp.status = SearchPipeStatus::Error;
        resp.errorMessage = "query exceeds byte limit";
        return resp;
    }

    if (externalCancel && externalCancel->load(std::memory_order_acquire)) {
        resp.status = SearchPipeStatus::Cancelled;
        resp.cancelled = true;
        return resp;
    }

    // 严格 DEMAND_START 守卫：autoStart=false 时仅探测，绝不拉起服务
    std::wstring activePipeW = m_impl->resolveActivePipeW(autoStart ? 40 : 1);
    if (activePipeW.empty()) {
        if (!autoStart) {
            resp.status = SearchPipeStatus::ServiceNotRunning;
            resp.win32Error = ERROR_SERVICE_NOT_ACTIVE;
            resp.errorMessage = "search service not running";
            return resp;
        }
        // 首次主动搜索按需异步拉起，绝不阻塞调用线程数十秒
        if (!m_impl->warmupInFlight.load(std::memory_order_acquire)) {
            warmupAsync();
        }
        // 极短等待看是否已在百毫秒内发布管道
        activePipeW = m_impl->resolveActivePipeW(120);
        if (activePipeW.empty()) {
            resp.status = SearchPipeStatus::Starting;
            resp.initializing = true;
            resp.errorMessage = "search service starting on demand";
            return resp;
        }
    }

    const uint64_t myCancelEpoch = m_impl->cancelEpoch.load(std::memory_order_acquire);
    const ULONGLONG connectDeadline = GetTickCount64() + (std::min<DWORD>)(timeoutMs, 1000);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD lastError = ERROR_SUCCESS;

    while (!m_impl->shuttingDown.load(std::memory_order_acquire) &&
           m_impl->cancelEpoch.load(std::memory_order_acquire) == myCancelEpoch &&
           !(externalCancel && externalCancel->load(std::memory_order_acquire))) {
        pipe = CreateFileW(
            activePipeW.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND) {
            const std::wstring refreshed = m_impl->resolveActivePipeW(20);
            if (!refreshed.empty() && refreshed != activePipeW) {
                activePipeW = refreshed;
                continue;
            }
            break;
        }
        if (lastError != ERROR_PIPE_BUSY) {
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= connectDeadline) {
            break;
        }
        const DWORD waitSlice = static_cast<DWORD>((std::min<ULONGLONG>)(connectDeadline - now, 120));
        if (!WaitNamedPipeW(activePipeW.c_str(), waitSlice)) {
            const DWORD waitErr = GetLastError();
            if (!isPipeTransientBusyError(waitErr)) {
                lastError = waitErr;
                break;
            }
            Sleep(10);
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        if (m_impl->shuttingDown.load(std::memory_order_acquire) ||
            m_impl->cancelEpoch.load(std::memory_order_acquire) != myCancelEpoch ||
            (externalCancel && externalCancel->load(std::memory_order_acquire))) {
            resp.status = SearchPipeStatus::Cancelled;
            resp.cancelled = true;
            return resp;
        }
        resp.win32Error = lastError;
        if (isPipeTransientBusyError(lastError)) {
            resp.status = SearchPipeStatus::Busy;
            resp.errorMessage = "search service busy";
        } else if (m_impl->warmupInFlight.load(std::memory_order_acquire) ||
                   (!m_impl->serviceConfirmedReady.load(std::memory_order_acquire) &&
                    m_impl->serviceSpawnedByUs.load(std::memory_order_acquire))) {
            resp.status = SearchPipeStatus::Starting;
            resp.initializing = true;
            resp.errorMessage = "search service starting";
        } else {
            resp.status = SearchPipeStatus::Unavailable;
            resp.errorMessage = "search pipe unavailable";
        }
        return resp;
    }

    struct PipeHandleGuard {
        Impl* impl;
        HANDLE handle;
        explicit PipeHandleGuard(Impl* i, HANDLE h) : impl(i), handle(h) {
            impl->registerActiveHandle(handle);
        }
        ~PipeHandleGuard() noexcept {
            impl->unregisterActiveHandle(handle);
            if (handle != INVALID_HANDLE_VALUE) {
                CancelIoEx(handle, nullptr);
                CloseHandle(handle);
            }
        }
    } pipeGuard(m_impl, pipe);

    HANDLE ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent) {
        resp.status = SearchPipeStatus::Error;
        resp.win32Error = GetLastError();
        resp.errorMessage = "failed to create overlapped event";
        return resp;
    }
    struct EventGuard {
        HANDLE handle;
        ~EventGuard() noexcept {
            if (handle) CloseHandle(handle);
        }
    } eventGuard{ioEvent};

    // 构造与 Tools3000_Service 完全一致的 JSON 请求载荷
    auto& cfg = ConfigManager::instance();
    const std::string expandedQuery = expandUserEnvUtf8(
        effectiveQuery, tools3000::service::search_limits::MaxQueryUtf8Bytes);
    const size_t clampedLimit = std::clamp<size_t>(limit == 0 ? 30 : limit, 1, 200);

    nlohmann::json reqJson = {
        {"query", expandedQuery},
        {"queryId", queryId},
        {"limit", clampedLimit},
        {"searchMode", "name"},
        {"pinyinEnabled", resolveEffectivePinyinEnabled()},
        {"matchPath", cfg.get<bool>("/search/matchPath", false)},
        {"caseSensitive", cfg.get<bool>("/search/caseSensitive", false)},
        {"excludeHidden", cfg.get<bool>("/search/excludeHidden", false)},
        {"excludeSystem", cfg.get<bool>("/search/excludeSystem",
                                        cfg.get<bool>("/search/excludeHidden", false))}
    };

    if (!dialogPriorities.empty()) {
        nlohmann::json prioArr = nlohmann::json::array();
        const size_t count = (std::min)(dialogPriorities.size(), kMaxDialogPriorities);
        for (size_t i = 0; i < count; ++i) {
            if (!dialogPriorities[i].path.empty()) {
                prioArr.push_back({
                    {"path", dialogPriorities[i].path},
                    {"score", dialogPriorities[i].score}
                });
            }
        }
        if (!prioArr.empty()) {
            reqJson["dialogPriorities"] = std::move(prioArr);
        }
    }

    const std::string payload = reqJson.dump();
    namespace frame = tools3000::service::pipe;
    if (!frame::fitsInFrame(payload.size(), frame::MaxRequestBytes)) {
        resp.status = SearchPipeStatus::Error;
        resp.errorMessage = "request frame exceeds limit";
        return resp;
    }

    const auto reqHeader = frame::encodeFrameHeader(static_cast<uint32_t>(payload.size()));
    const DWORD writeBudget = (std::min)(timeoutMs, kDefaultWriteTimeoutMs);
    DWORD ioError = ERROR_SUCCESS;

    if (!m_impl->transferExact(pipe, ioEvent, const_cast<char*>(reqHeader.data()),
                               reqHeader.size(), writeBudget, myCancelEpoch, externalCancel,
                               ioError, true) ||
        !m_impl->transferExact(pipe, ioEvent, const_cast<char*>(payload.data()),
                               payload.size(), writeBudget, myCancelEpoch, externalCancel,
                               ioError, true)) {
        resp.win32Error = ioError;
        if (ioError == ERROR_OPERATION_ABORTED) {
            resp.status = SearchPipeStatus::Cancelled;
            resp.cancelled = true;
        } else if (ioError == ERROR_TIMEOUT) {
            resp.status = SearchPipeStatus::Timeout;
        } else {
            resp.status = SearchPipeStatus::Unavailable;
        }
        return resp;
    }

    char respHeader[frame::HeaderSize]{};
    if (!m_impl->transferExact(pipe, ioEvent, respHeader, sizeof(respHeader),
                               timeoutMs, myCancelEpoch, externalCancel, ioError, false)) {
        resp.win32Error = ioError;
        if (ioError == ERROR_OPERATION_ABORTED) {
            resp.status = SearchPipeStatus::Cancelled;
            resp.cancelled = true;
        } else if (ioError == ERROR_TIMEOUT) {
            resp.status = SearchPipeStatus::Timeout;
        } else {
            resp.status = SearchPipeStatus::Unavailable;
        }
        return resp;
    }

    uint32_t respBytes = 0;
    if (!frame::decodeFrameHeader(respHeader, respBytes)) {
        resp.status = SearchPipeStatus::Error;
        resp.win32Error = ERROR_INVALID_DATA;
        resp.errorMessage = "corrupted response frame header";
        return resp;
    }

    std::string rawResp(respBytes, '\0');
    if (!m_impl->transferExact(pipe, ioEvent, rawResp.data(), respBytes,
                               timeoutMs, myCancelEpoch, externalCancel, ioError, false)) {
        resp.win32Error = ioError;
        if (ioError == ERROR_OPERATION_ABORTED) {
            resp.status = SearchPipeStatus::Cancelled;
            resp.cancelled = true;
        } else if (ioError == ERROR_TIMEOUT) {
            resp.status = SearchPipeStatus::Timeout;
        } else {
            resp.status = SearchPipeStatus::Unavailable;
        }
        return resp;
    }

    try {
        const auto jsonResp = nlohmann::json::parse(rawResp);
        const bool isInit = jsonResp.value("initializing", false);
        const bool isCancelled = jsonResp.value("cancelled", false);
        resp.initializing = isInit;
        resp.cancelled = isCancelled;
        resp.elapsedMs = jsonResp.value("elapsedMs", int64_t{0});
        resp.queryId = jsonResp.value("queryId", queryId);

        if (isInit) {
            resp.status = SearchPipeStatus::Starting;
            return resp;
        }
        m_impl->serviceConfirmedReady.store(true, std::memory_order_release);

        if (isCancelled) {
            resp.status = SearchPipeStatus::Cancelled;
            return resp;
        }

        if (jsonResp.contains("results") && jsonResp["results"].is_array()) {
            const auto& arr = jsonResp["results"];
            resp.items.reserve(arr.size());
            for (const auto& entry : arr) {
                if (!entry.is_object()) continue;
                SearchFileItem item;
                item.name = entry.value("name", "");
                item.path = entry.value("path", "");
                if (item.path.empty()) continue;
                item.isDirectory = entry.value("isDirectory", false);
                item.size = entry.value("size", uint64_t{0});
                item.lastWriteTime = entry.value("lastWriteTime", uint64_t{0});
                item.matchRank = entry.value("matchRank", 0.0);
                item.frecencyScore = entry.value("frecencyScore", 0.0);
                if (item.name.empty()) {
                    const size_t slash = item.path.find_last_of("\\/");
                    item.name = (slash != std::string::npos && slash + 1 < item.path.size())
                        ? item.path.substr(slash + 1)
                        : item.path;
                }
                resp.items.push_back(std::move(item));
            }
        }
        resp.status = SearchPipeStatus::Ready;
        return resp;
    } catch (const std::exception& ex) {
        resp.status = SearchPipeStatus::Error;
        resp.errorMessage = ex.what();
        return resp;
    }
}

}  // namespace tools3000::core::ipc
