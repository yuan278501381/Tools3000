#include <windows.h>
#include <iostream>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <memory>
#include <chrono>
#include <cctype>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <dbt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <psapi.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cwctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "MftParser.h"
#include "SensitivePathGuard.h"
#include "ServiceLifecyclePolicy.h"
#include "QueryThreadPool.h"
#include "AppLauncherCatalog.h"
#include "PipeProtocol.h"
#include "PipeEndpoint.h"
#include "PipeClientAuth.h"
#include "ExternalApiPolicy.h"
#include "SearchCancellation.h"
#include "SearchRequestLimits.h"
#include "content/ContentSearchEngine.h"
#include "content/ContentTextCache.h"
#include "db/RunHistoryManager.h"
#include "db/SearchHistoryManager.h"
#include "db/DatabaseManager.h"

#define SERVICE_NAME L"Tools3000_SearchService"

SERVICE_STATUS        g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
std::mutex            g_ServiceStatusMutex;
HANDLE                g_ServiceStopEvent = INVALID_HANDLE_VALUE;
std::atomic<bool>     g_IsRunning{false};
std::atomic<bool>     g_ShutdownSnapshotFlushed{false};

std::shared_mutex g_ParsersMutex;
std::vector<std::shared_ptr<MftParser>> g_MftParsers;

// 所有会触碰 MftParser 的后台工作都由服务拥有。服务退出时先请求停止并 join，
// 再销毁解析器，禁止任何 detached worker 越过对象生命周期。
std::mutex g_BackgroundJobMutex;
std::jthread g_RebuildJob;
std::jthread g_SnapshotJob;
std::atomic<bool> g_RebuildInProgress{false};
std::atomic<bool> g_SnapshotInProgress{false};
std::atomic<int> g_InitialIndexWorkers{0};
std::atomic<int> g_DynamicIndexWorkers{0};
std::mutex g_DynamicWorkerThreadsMutex;
std::vector<std::thread> g_DynamicWorkerThreads;
std::atomic<HWND> g_HotplugWindow{nullptr};
std::atomic<DWORD> g_KnownDriveMask{0};
std::atomic<bool> g_AcceptBackgroundJobs{true};
// The per-user pipe is published before expensive volume work. Until this flag
// flips, workers return an explicit initializing response and never touch the
// parser vector while it is being assembled.
std::atomic<bool> g_SearchIndexReady{false};
std::jthread g_PipePokeJob;

std::string g_SearchPipeName;
std::wstring g_SearchClientSid;
std::string g_CompatTokenPipeName;
std::wstring g_CompatTokenClientSid;
bool g_UseKernelServicePipeSddl = false;
std::string g_IsolatedPortableToken;
constexpr int NumPipeWorkers = 16;
constexpr int NumCompatPipeWorkers = 4;

namespace {

struct CachedContentCandidatePlan {
    uint64_t planId = 0;
    std::string signature;
    std::shared_ptr<const std::vector<ContentCandidate>> candidates;
    size_t baseCursorOffset = 0;
    std::chrono::steady_clock::time_point createdAt{};
};

struct ProgressivePlanAnchorState {
    uint64_t planId = 0;
    std::string signature;
    std::unordered_set<std::wstring> scannedNormPaths;
    std::chrono::steady_clock::time_point updatedAt{};
};

struct CachedBothNameRemainder {
    uint64_t planId = 0;
    std::string signature;
    std::vector<SearchResult> remainingNames;
    size_t nextOffset = 0;
    std::chrono::steady_clock::time_point updatedAt{};
};

constexpr size_t MaxCachedContentPlans = 4;
constexpr size_t MaxPlanAnchorStates = 4;
constexpr auto ContentPlanCacheTtl = std::chrono::seconds(90);
constexpr size_t ProgressiveFollowupMaxBatchHits = 60;

std::mutex g_ContentPlanCacheMutex;
std::vector<CachedContentCandidatePlan> g_ContentPlanCache;
std::vector<ProgressivePlanAnchorState> g_PlanAnchorStates;
std::vector<CachedBothNameRemainder> g_BothNameRemainders;
std::atomic<uint64_t> g_NextContentPlanId{1};

std::wstring normalizeCandidatePathKey(std::wstring_view path) {
    std::wstring norm;
    norm.reserve(path.size());
    for (wchar_t ch : path) {
        norm.push_back(tools3000::service::pipe_auth::foldAsciiOrWideLower(ch));
    }
    return norm;
}

void invalidateContentPlanCache() {
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    // Clear cached candidate vectors so USN file additions/deletions take effect,
    // while retaining `g_PlanAnchorStates` so in-flight progressive queries re-anchor
    // cleanly without skipping or duplicating documents (`MED-16`).
    g_ContentPlanCache.clear();
}

void clearAllContentCachesForTrim() {
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    g_ContentPlanCache.clear();
    g_ContentPlanCache.shrink_to_fit();
    g_PlanAnchorStates.clear();
    g_PlanAnchorStates.shrink_to_fit();
    g_BothNameRemainders.clear();
    g_BothNameRemainders.shrink_to_fit();
}

enum class ServiceMemoryMode : int {
    FullResident = 0,
    IdleTrim = 1,
    IdleStop = 2,
};

ServiceMemoryMode parseServiceMemoryMode(std::string_view value) noexcept {
    if (value == "full_resident") return ServiceMemoryMode::FullResident;
    if (value == "idle_stop") return ServiceMemoryMode::IdleStop;
    return ServiceMemoryMode::IdleTrim;
}

const char* serviceMemoryModeToString(ServiceMemoryMode mode) noexcept {
    switch (mode) {
        case ServiceMemoryMode::FullResident: return "full_resident";
        case ServiceMemoryMode::IdleStop: return "idle_stop";
        case ServiceMemoryMode::IdleTrim:
        default: return "idle_trim";
    }
}

std::atomic<int> g_MemoryMode{static_cast<int>(ServiceMemoryMode::IdleTrim)};
std::atomic<uint32_t> g_IdleTrimMinutes{2};
std::atomic<uint32_t> g_IdleStopMinutes{30};
std::atomic<uint64_t> g_LastUserActivityTick{0};
std::atomic<uint64_t> g_LastTrimmedAtMs{0};
std::atomic<uint64_t> g_LastTrimMonotonicTick{0};
std::atomic<bool> g_TrimmedDuringIdle{false};
std::atomic<int> g_InFlightUserQueries{0};
std::mutex g_MemoryTrimMutex;
std::mutex g_PersistedUsnMutex;
std::unordered_map<char, uint64_t> g_LastPersistedUsnByDrive;

struct ScopedInFlightUserQuery {
    ScopedInFlightUserQuery() noexcept {
        g_InFlightUserQueries.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ScopedInFlightUserQuery() noexcept {
        g_InFlightUserQueries.fetch_sub(1, std::memory_order_acq_rel);
    }
    ScopedInFlightUserQuery(const ScopedInFlightUserQuery&) = delete;
    ScopedInFlightUserQuery& operator=(const ScopedInFlightUserQuery&) = delete;
};

void markUserActivity() noexcept {
    g_LastUserActivityTick.store(GetTickCount64(), std::memory_order_release);
    g_TrimmedDuringIdle.store(false, std::memory_order_release);
}

uint64_t currentEpochMillis() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void querySelfMemoryUsage(uint64_t& outWorkingSetBytes, uint64_t& outPrivateBytes) noexcept {
    outWorkingSetBytes = 0;
    outPrivateBytes = 0;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        outWorkingSetBytes = static_cast<uint64_t>(pmc.WorkingSetSize);
        outPrivateBytes = static_cast<uint64_t>(pmc.PrivateUsage);
    }
}

void recordPersistedUsns(const std::vector<MftParser*>& rawParsers) {
    std::lock_guard<std::mutex> lock(g_PersistedUsnMutex);
    for (const auto* parser : rawParsers) {
        if (!parser || !parser->isInitialScanDone()) continue;
        const char drive = static_cast<char>(std::toupper(static_cast<unsigned char>(parser->getDriveLetter())));
        g_LastPersistedUsnByDrive[drive] = parser->getCurrentUsn();
    }
}

bool hasAnyUsnAdvancedSincePersist(const std::vector<MftParser*>& rawParsers) {
    std::lock_guard<std::mutex> lock(g_PersistedUsnMutex);
    for (const auto* parser : rawParsers) {
        if (!parser || !parser->isInitialScanDone()) continue;
        const char drive = static_cast<char>(std::toupper(static_cast<unsigned char>(parser->getDriveLetter())));
        const uint64_t curUsn = parser->getCurrentUsn();
        const auto it = g_LastPersistedUsnByDrive.find(drive);
        if (it == g_LastPersistedUsnByDrive.end() || it->second != curUsn) {
            return true;
        }
    }
    return false;
}

std::optional<CachedContentCandidatePlan> lookupCachedContentPlan(
    uint64_t planId,
    std::string_view signature
) {
    if (planId == 0) return std::nullopt;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (auto it = g_ContentPlanCache.begin(); it != g_ContentPlanCache.end(); ++it) {
        if (it->planId == planId && it->signature == signature) {
            if (now - it->createdAt > ContentPlanCacheTtl) {
                g_ContentPlanCache.erase(it);
                return std::nullopt;
            }
            auto hit = *it;
            hit.createdAt = now;
            g_ContentPlanCache.erase(it);
            g_ContentPlanCache.push_back(hit);
            return hit;
        }
    }
    return std::nullopt;
}

std::unordered_set<std::wstring> lookupPlanAnchorScannedPaths(
    uint64_t requestedPlanId,
    std::string_view signature
) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (const auto& state : g_PlanAnchorStates) {
        if ((now - state.updatedAt) <= ContentPlanCacheTtl &&
            state.planId == requestedPlanId &&
            state.signature == signature) {
            return state.scannedNormPaths;
        }
    }
    return {};
}

uint64_t storeCachedContentPlan(
    std::string signature,
    std::shared_ptr<const std::vector<ContentCandidate>> candidates,
    size_t baseCursorOffset = 0,
    std::unordered_set<std::wstring> inheritedScannedPaths = {}
) {
    const uint64_t id = g_NextContentPlanId.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    g_ContentPlanCache.erase(
        std::remove_if(
            g_ContentPlanCache.begin(),
            g_ContentPlanCache.end(),
            [&](const CachedContentCandidatePlan& entry) {
                return (now - entry.createdAt) > ContentPlanCacheTtl ||
                       entry.signature == signature;
            }),
        g_ContentPlanCache.end());
    while (g_ContentPlanCache.size() >= MaxCachedContentPlans) {
        g_ContentPlanCache.erase(g_ContentPlanCache.begin());
    }
    g_ContentPlanCache.push_back(CachedContentCandidatePlan{
        id,
        signature,
        std::move(candidates),
        baseCursorOffset,
        now
    });

    g_PlanAnchorStates.erase(
        std::remove_if(
            g_PlanAnchorStates.begin(),
            g_PlanAnchorStates.end(),
            [&](const ProgressivePlanAnchorState& s) {
                return (now - s.updatedAt) > ContentPlanCacheTtl;
            }),
        g_PlanAnchorStates.end());
    while (g_PlanAnchorStates.size() >= MaxPlanAnchorStates) {
        g_PlanAnchorStates.erase(g_PlanAnchorStates.begin());
    }
    g_PlanAnchorStates.push_back(ProgressivePlanAnchorState{
        id,
        std::move(signature),
        std::move(inheritedScannedPaths),
        now
    });
    return id;
}

void recordPlanScannedRange(
    uint64_t planId,
    const std::vector<ContentCandidate>& candidates,
    size_t localStart,
    size_t localEnd
) {
    if (planId == 0 || localStart >= localEnd || localStart >= candidates.size()) return;
    const size_t clampedEnd = (std::min)(localEnd, candidates.size());
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (auto& state : g_PlanAnchorStates) {
        if (state.planId == planId) {
            for (size_t idx = localStart; idx < clampedEnd; ++idx) {
                if (state.scannedNormPaths.size() < 100000) {
                    state.scannedNormPaths.insert(normalizeCandidatePathKey(candidates[idx].fullPath));
                }
            }
            state.updatedAt = now;
            break;
        }
    }
}

void storeBothModeNameRemainder(
    uint64_t planId,
    std::string signature,
    std::vector<SearchResult> remainingNames,
    size_t nextOffset
) {
    if (planId == 0 || nextOffset >= remainingNames.size()) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    g_BothNameRemainders.erase(
        std::remove_if(
            g_BothNameRemainders.begin(),
            g_BothNameRemainders.end(),
            [&](const CachedBothNameRemainder& item) {
                return (now - item.updatedAt) > ContentPlanCacheTtl ||
                       item.planId == planId ||
                       item.signature == signature;
            }),
        g_BothNameRemainders.end());
    while (g_BothNameRemainders.size() >= MaxCachedContentPlans) {
        g_BothNameRemainders.erase(g_BothNameRemainders.begin());
    }
    g_BothNameRemainders.push_back(CachedBothNameRemainder{
        planId,
        std::move(signature),
        std::move(remainingNames),
        nextOffset,
        now
    });
}

void rebindBothModeNameRemainderPlanId(uint64_t oldPlanId, uint64_t newPlanId, std::string_view signature) {
    if (oldPlanId == 0 || newPlanId == 0 || oldPlanId == newPlanId) return;
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (auto& entry : g_BothNameRemainders) {
        if (entry.planId == oldPlanId && entry.signature == signature) {
            entry.planId = newPlanId;
            entry.updatedAt = std::chrono::steady_clock::now();
            break;
        }
    }
}

std::vector<SearchResult> consumeBothModeNameRemainder(
    uint64_t planId,
    std::string_view signature,
    size_t maxCount,
    bool& outHasMoreNames
) {
    outHasMoreNames = false;
    std::vector<SearchResult> slice;
    if (planId == 0) return slice;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (auto it = g_BothNameRemainders.begin(); it != g_BothNameRemainders.end(); ++it) {
        if ((it->planId == planId || it->signature == signature) && it->signature == signature) {
            if ((now - it->updatedAt) > ContentPlanCacheTtl ||
                it->nextOffset >= it->remainingNames.size()) {
                g_BothNameRemainders.erase(it);
                return slice;
            }
            const size_t avail = it->remainingNames.size() - it->nextOffset;
            const size_t take = (std::min)(maxCount, avail);
            slice.reserve(take);
            for (size_t i = 0; i < take; ++i) {
                slice.push_back(it->remainingNames[it->nextOffset + i]);
            }
            it->nextOffset += take;
            it->updatedAt = now;
            if (it->nextOffset < it->remainingNames.size()) {
                outHasMoreNames = true;
            } else {
                g_BothNameRemainders.erase(it);
            }
            return slice;
        }
    }
    return slice;
}

std::wstring expandQueryEnvForClient(
    const std::wstring& input,
    HANDLE hClientToken,
    bool isSameUserProcessSid
) {
    if (input.find(L'%') == std::wstring::npos) {
        return input;
    }
    constexpr DWORD kMaxExpandedChars = 4096;
    std::vector<wchar_t> buf(kMaxExpandedChars, L'\0');
    if (hClientToken != nullptr && hClientToken != INVALID_HANDLE_VALUE) {
        if (ExpandEnvironmentStringsForUserW(hClientToken, input.c_str(), buf.data(), kMaxExpandedChars)) {
            buf.back() = L'\0';
            std::wstring expanded(buf.data());
            if (!expanded.empty()) {
                return expanded;
            }
        }
    } else if (isSameUserProcessSid || !tools3000::service::pipe_auth::isCurrentProcessLocalSystem()) {
        const DWORD n = ExpandEnvironmentStringsW(input.c_str(), buf.data(), kMaxExpandedChars);
        if (n > 0 && n <= kMaxExpandedChars) {
            return std::wstring(buf.data(), static_cast<size_t>(n - 1));
        }
    }
    return input;
}

}  // namespace

std::string WStringToString(const std::wstring& wstr);

bool configurePipeEndpoint(DWORD argc, wchar_t** argv) {
    std::string token;
    std::wstring sid;
    bool isolatedPortable = false;
    bool hasTokenArg = false;
    bool hasSidArg = false;

    for (DWORD index = 0; index < argc; ++index) {
        const std::wstring_view arg = argv[index] ? argv[index] : L"";
        constexpr std::wstring_view tokenPrefix = L"--pipe-token=";
        constexpr std::wstring_view sidPrefix = L"--client-sid=";
        if (arg.starts_with(tokenPrefix)) {
            hasTokenArg = true;
            token = WStringToString(std::wstring(arg.substr(tokenPrefix.size())));
        } else if (arg.starts_with(sidPrefix)) {
            hasSidArg = true;
            sid = std::wstring(arg.substr(sidPrefix.size()));
        } else if (arg == L"--portable-isolated") {
            isolatedPortable = true;
        }
    }

    PSID parsedSid = nullptr;
    const bool sidValid = !sid.empty() && ConvertStringSidToSidW(sid.c_str(), &parsedSid) != FALSE;
    if (parsedSid) LocalFree(parsedSid);
    const auto tokenPipe = tools3000::service::pipe_endpoint::makePipeName(token);

    if ((hasSidArg && !sidValid) || (hasTokenArg && !tokenPipe)) {
        spdlog::error("Search service received malformed per-user IPC endpoint arguments");
        return false;
    }

    g_CompatTokenPipeName.clear();
    g_CompatTokenClientSid.clear();

    if (isolatedPortable) {
        if (!sidValid || !tokenPipe) {
            spdlog::error("Isolated portable search service requires valid --pipe-token and --client-sid");
            return false;
        }
        g_UseKernelServicePipeSddl = false;
        g_SearchPipeName = *tokenPipe;
        g_SearchClientSid = std::move(sid);
        g_IsolatedPortableToken = token;
        return true;
    }

    if (tools3000::service::pipe_auth::isCurrentProcessLocalSystem()) {
        // SCM LocalSystem mode: always publish the immutable kernel-authenticated pipe so
        // resetting/importing `config.json` never breaks connectivity (`HIGH-10`).
        g_UseKernelServicePipeSddl = true;
        g_SearchPipeName = std::string(tools3000::service::pipe_endpoint::KernelAuthPipeName);
        g_SearchClientSid.clear();
        if (sidValid && tokenPipe) {
            g_CompatTokenPipeName = *tokenPipe;
            g_CompatTokenClientSid = std::move(sid);
        }
        return true;
    }

    // Standalone / portable user mode: derive deterministic per-SID endpoint from immutable Windows SID.
    std::wstring effectiveSid = sidValid ? sid : tools3000::service::pipe_auth::queryCurrentProcessUserSidString();
    PSID verifySelfSid = nullptr;
    const bool effectiveSidOk = !effectiveSid.empty() &&
                                ConvertStringSidToSidW(effectiveSid.c_str(), &verifySelfSid) != FALSE;
    if (verifySelfSid) LocalFree(verifySelfSid);
    const auto sidPipe = tools3000::service::pipe_endpoint::makeUserSidPipeName(effectiveSid);
    if (!effectiveSidOk || !sidPipe) {
        spdlog::error("Search service unable to resolve valid user SID endpoint");
        return false;
    }

    g_UseKernelServicePipeSddl = false;
    g_SearchPipeName = *sidPipe;
    g_SearchClientSid = effectiveSid;
    if (tokenPipe && *tokenPipe != g_SearchPipeName) {
        g_CompatTokenPipeName = *tokenPipe;
        g_CompatTokenClientSid = effectiveSid;
    }
    return true;
}

std::vector<std::shared_ptr<MftParser>> snapshotParsers() {
    std::shared_lock<std::shared_mutex> lock(g_ParsersMutex);
    return g_MftParsers;
}

std::vector<MftParser*> toRawParserPointers(const std::vector<std::shared_ptr<MftParser>>& owners) {
    std::vector<MftParser*> raw;
    raw.reserve(owners.size());
    for (const auto& p : owners) {
        if (p) raw.push_back(p.get());
    }
    return raw;
}

bool mountDynamicVolumeAsync(char driveLetter) {
    if (!std::isalpha(static_cast<unsigned char>(driveLetter))) return false;
    const char upperDrive = static_cast<char>(std::toupper(static_cast<unsigned char>(driveLetter)));
    if (!g_IsRunning.load(std::memory_order_acquire) ||
        !g_AcceptBackgroundJobs.load(std::memory_order_acquire)) {
        return false;
    }

    const DWORD logicalMask = GetLogicalDrives();
    if (!(logicalMask & (1u << (upperDrive - 'A')))) {
        return false;
    }

    const std::wstring root{static_cast<wchar_t>(upperDrive), L':', L'\\'};
    const UINT driveType = GetDriveTypeW(root.c_str());
    if (driveType != DRIVE_FIXED &&
        driveType != DRIVE_REMOVABLE &&
        driveType != DRIVE_REMOTE &&
        driveType != DRIVE_RAMDISK) {
        return false;
    }

    {
        std::shared_lock<std::shared_mutex> lock(g_ParsersMutex);
        for (const auto& existing : g_MftParsers) {
            if (existing && std::toupper(static_cast<unsigned char>(existing->getDriveLetter())) == upperDrive) {
                return false;
            }
        }
    }

    auto parser = std::make_shared<MftParser>();
    parser->setRealTimeValidation(true);
    if (!parser->Initialize(upperDrive)) {
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(g_ParsersMutex);
        if (!g_IsRunning.load(std::memory_order_acquire) ||
            !g_AcceptBackgroundJobs.load(std::memory_order_acquire)) {
            return false;
        }
        for (const auto& existing : g_MftParsers) {
            if (existing && std::toupper(static_cast<unsigned char>(existing->getDriveLetter())) == upperDrive) {
                return false;
            }
        }
        g_MftParsers.push_back(parser);
    }

    g_DynamicIndexWorkers.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> workerLock(g_DynamicWorkerThreadsMutex);
        g_DynamicWorkerThreads.emplace_back([parser, upperDrive]() {
            try {
                parser->EnumerateFiles();
                if (g_IsRunning.load(std::memory_order_acquire) &&
                    g_AcceptBackgroundJobs.load(std::memory_order_acquire)) {
                    parser->StartListening();
                }
                invalidateContentPlanCache();
                spdlog::info("Dynamically indexed volume {}:", upperDrive);
            } catch (const std::exception& e) {
                spdlog::error("Dynamic indexing failed on drive {}: {}", upperDrive, e.what());
            } catch (...) {
                spdlog::error("Dynamic indexing failed on drive {} with unknown exception", upperDrive);
            }
            g_DynamicIndexWorkers.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
    return true;
}

size_t ensureRequestedDrivesMounted(const std::vector<char>& drives) {
    size_t mounted = 0;
    for (char d : drives) {
        if (mountDynamicVolumeAsync(d)) {
            ++mounted;
        }
    }
    return mounted;
}

size_t pruneRemovedVolumes() {
    const DWORD currentMask = GetLogicalDrives();
    std::vector<std::shared_ptr<MftParser>> removed;
    {
        std::unique_lock<std::shared_mutex> lock(g_ParsersMutex);
        auto it = g_MftParsers.begin();
        while (it != g_MftParsers.end()) {
            const char d = (*it)
                ? static_cast<char>(std::toupper(static_cast<unsigned char>((*it)->getDriveLetter())))
                : '\0';
            if (d >= 'A' && d <= 'Z' && (currentMask & (1u << (d - 'A'))) == 0) {
                removed.push_back(std::move(*it));
                it = g_MftParsers.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& parser : removed) {
        if (parser) {
            spdlog::info("Pruning unplugged volume {}: from active index", parser->getDriveLetter());
            parser->requestStop();
            parser->StopListening();
        }
    }
    if (!removed.empty()) {
        invalidateContentPlanCache();
    }
    return removed.size();
}

void handleDeviceHotplugChange() {
    if (!g_IsRunning.load(std::memory_order_acquire) ||
        !g_AcceptBackgroundJobs.load(std::memory_order_acquire)) {
        return;
    }

    pruneRemovedVolumes();

    const DWORD currentMask = GetLogicalDrives();
    const DWORD prevMask = g_KnownDriveMask.exchange(currentMask, std::memory_order_acq_rel);
    const DWORD addedMask = currentMask & ~prevMask;
    if (addedMask == 0) return;

    for (char drive = 'A'; drive <= 'Z'; ++drive) {
        if (!(addedMask & (1u << (drive - 'A')))) continue;
        const std::wstring root{static_cast<wchar_t>(drive), L':', L'\\'};
        const UINT driveType = GetDriveTypeW(root.c_str());
        if (driveType != DRIVE_REMOVABLE && driveType != DRIVE_FIXED) continue;

        wchar_t fsName[MAX_PATH] = {};
        if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                  fsName, static_cast<DWORD>(std::size(fsName))) &&
            _wcsicmp(fsName, L"NTFS") == 0) {
            spdlog::info("Detected hotplugged NTFS volume {}:, starting MFT index", drive);
            mountDynamicVolumeAsync(drive);
        }
    }
}

LRESULT CALLBACK HotplugSinkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    constexpr UINT_PTR kDebounceTimerId = 1;
    switch (msg) {
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL ||
                wParam == DBT_DEVICEREMOVECOMPLETE ||
                wParam == DBT_DEVNODES_CHANGED) {
                SetTimer(hwnd, kDebounceTimerId, 250, nullptr);
            }
            return TRUE;
        case WM_TIMER:
            if (wParam == kDebounceTimerId) {
                KillTimer(hwnd, kDebounceTimerId);
                handleDeviceHotplugChange();
                return 0;
            }
            break;
        case WM_CLOSE:
            KillTimer(hwnd, kDebounceTimerId);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void DeviceHotplugWatcherThread() {
    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    constexpr const wchar_t* kClassName = L"Tools3000_ServiceHotplugSink";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HotplugSinkWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName,
        L"",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInst,
        nullptr);
    if (!hwnd) {
        return;
    }

    g_HotplugWindow.store(hwnd, std::memory_order_release);
    if (!g_IsRunning.load(std::memory_order_acquire)) {
        g_HotplugWindow.store(nullptr, std::memory_order_release);
        DestroyWindow(hwnd);
        return;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_HotplugWindow.store(nullptr, std::memory_order_release);
}

void stopBackgroundJobs() {
    g_AcceptBackgroundJobs.store(false, std::memory_order_release);
    invalidateContentPlanCache();
    for (auto& parser : snapshotParsers()) {
        if (parser) parser->requestStop();
    }

    std::jthread rebuild;
    std::jthread snapshot;
    {
        std::lock_guard lock(g_BackgroundJobMutex);
        if (g_RebuildJob.joinable()) g_RebuildJob.request_stop();
        if (g_SnapshotJob.joinable()) g_SnapshotJob.request_stop();
        rebuild = std::move(g_RebuildJob);
        snapshot = std::move(g_SnapshotJob);
    }
    std::vector<std::thread> dynamicWorkers;
    {
        std::lock_guard<std::mutex> workerLock(g_DynamicWorkerThreadsMutex);
        dynamicWorkers = std::move(g_DynamicWorkerThreads);
    }
    for (auto& w : dynamicWorkers) {
        if (w.joinable()) w.join();
    }
}

struct ScopedBackgroundIoMode {
    bool active = false;
    ScopedBackgroundIoMode() noexcept {
        active = (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE);
        if (!active) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
    }
    ~ScopedBackgroundIoMode() noexcept {
        if (active) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
    }
    ScopedBackgroundIoMode(const ScopedBackgroundIoMode&) = delete;
    ScopedBackgroundIoMode& operator=(const ScopedBackgroundIoMode&) = delete;
};

// 按物理磁盘编号将待扫描卷分组：同一块物理机械硬盘上的多个分区（例如同盘 C/D/E）
// 必须严格排队串行扫描，避免多分区线程并发争夺磁头把磁盘占满 100%；不同物理盘仍可并行。
std::vector<std::vector<std::shared_ptr<MftParser>>> groupVolumesByPhysicalDisk(
    const std::vector<std::shared_ptr<MftParser>>& parsers) {
    std::vector<std::vector<std::shared_ptr<MftParser>>> groups;
    std::unordered_map<uint64_t, size_t> groupIndexByKey;
    constexpr uint64_t kUnknownHddKey = 0xFFFFFFFF00000001ULL;
    uint64_t nextUnknownSsdKey = 0xFFFFFFFF00000010ULL;

    for (const auto& parser : parsers) {
        if (!parser) continue;
        uint64_t key = 0;
        if (parser->hasPhysicalDiskNumber()) {
            key = static_cast<uint64_t>(parser->getPhysicalDiskNumber());
        } else if (parser->isRotationalDisk()) {
            key = kUnknownHddKey;
        } else {
            key = nextUnknownSsdKey++;
        }
        auto it = groupIndexByKey.find(key);
        if (it == groupIndexByKey.end()) {
            groupIndexByKey.emplace(key, groups.size());
            groups.push_back({parser});
        } else {
            groups[it->second].push_back(parser);
        }
    }
    return groups;
}

void PerformGentlePostWorkTrim(const char* reason) {
    if (!g_IsRunning.load(std::memory_order_acquire) ||
        g_InFlightUserQueries.load(std::memory_order_acquire) > 0 ||
        g_RebuildInProgress.load(std::memory_order_acquire) ||
        g_InitialIndexWorkers.load(std::memory_order_acquire) > 0 ||
        g_DynamicIndexWorkers.load(std::memory_order_acquire) > 0) {
        return;
    }
    const auto mode = static_cast<ServiceMemoryMode>(g_MemoryMode.load(std::memory_order_acquire));
    if (mode == ServiceMemoryMode::FullResident) {
        return;
    }

    std::unique_lock<std::mutex> trimLock(g_MemoryTrimMutex, std::try_to_lock);
    if (!trimLock.owns_lock()) {
        return;
    }

    uint64_t beforeWs = 0;
    uint64_t beforePriv = 0;
    querySelfMemoryUsage(beforeWs, beforePriv);

    const auto parsers = snapshotParsers();
    for (const auto& parser : parsers) {
        if (!parser) continue;
        parser->shrinkIndexCapacity();
    }

    HeapCompact(GetProcessHeap(), 0);
    if (g_InFlightUserQueries.load(std::memory_order_acquire) == 0) {
        SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    }

    uint64_t afterWs = 0;
    uint64_t afterPriv = 0;
    querySelfMemoryUsage(afterWs, afterPriv);
    spdlog::info(
        "Service post-{} memory compact completed: workingSet {} MB -> {} MB, private {} MB",
        reason ? reason : "work",
        beforeWs / (1024ULL * 1024ULL),
        afterWs / (1024ULL * 1024ULL),
        afterPriv / (1024ULL * 1024ULL));
}

void scheduleSnapshot(std::vector<std::shared_ptr<MftParser>> parsers, std::string_view reason) {
    if (!g_AcceptBackgroundJobs.load(std::memory_order_acquire) || parsers.empty()) return;
    std::lock_guard lock(g_BackgroundJobMutex);
    if (g_SnapshotInProgress.exchange(true, std::memory_order_acq_rel)) return;
    if (g_SnapshotJob.joinable()) g_SnapshotJob.join();

    g_SnapshotJob = std::jthread(
        [parsers = std::move(parsers), reason = std::string(reason)](std::stop_token stop) {
            ScopedBackgroundIoMode bgIo;
            while (!stop.stop_requested() &&
                   (g_InitialIndexWorkers.load(std::memory_order_acquire) > 0 ||
                    g_DynamicIndexWorkers.load(std::memory_order_acquire) > 0)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            try {
                if (!stop.stop_requested()) {
                    const auto rawParsers = toRawParserPointers(parsers);
                    const bool ok = tools3000::service::db::DatabaseManager::instance().saveSnapshot(rawParsers);
                    if (ok) {
                        recordPersistedUsns(rawParsers);
                    }
                    spdlog::info("{} database snapshot {}", reason, ok ? "saved" : "failed");
                }
            } catch (const std::exception& e) {
                spdlog::error("{} database snapshot failed: {}", reason, e.what());
            } catch (...) {
                spdlog::error("{} database snapshot failed with unknown exception", reason);
            }
            g_SnapshotInProgress.store(false, std::memory_order_release);
            if (!stop.stop_requested()) {
                PerformGentlePostWorkTrim("snapshot");
            }
        });
}

bool scheduleRebuild() {
    if (!g_IsRunning.load(std::memory_order_acquire) ||
        !g_AcceptBackgroundJobs.load(std::memory_order_acquire) ||
        g_InitialIndexWorkers.load(std::memory_order_acquire) > 0) return false;

    std::lock_guard lock(g_BackgroundJobMutex);
    if (g_RebuildInProgress.exchange(true, std::memory_order_acq_rel)) return false;
    if (g_RebuildJob.joinable()) g_RebuildJob.join();

    invalidateContentPlanCache();
    tools3000::service::launcher::AppsFolderCatalog::instance().invalidate();
    pruneRemovedVolumes();
    auto parsers = snapshotParsers();
    for (auto& parser : parsers) {
        if (parser) parser->resetStopRequest();
    }
    g_RebuildJob = std::jthread([parsers = std::move(parsers)](std::stop_token stop) {
        for (const auto& parser : parsers) {
            if (parser) parser->StopListening();
        }
        const auto diskGroups = groupVolumesByPhysicalDisk(parsers);
        std::vector<std::thread> workers;
        workers.reserve(diskGroups.size());
        for (const auto& group : diskGroups) {
            workers.emplace_back([group, stop]() {
                ScopedBackgroundIoMode bgIo;
                for (const auto& parser : group) {
                    if (!parser || stop.stop_requested()) break;
                    try {
                        parser->EnumerateFiles();
                        if (!stop.stop_requested()) parser->StartListening();
                    } catch (const std::exception& e) {
                        spdlog::error("Rebuild failed on drive {}: {}", parser->getDriveLetter(), e.what());
                    } catch (...) {
                        spdlog::error("Rebuild failed on drive {} with unknown exception", parser->getDriveLetter());
                    }
                }
            });
        }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        invalidateContentPlanCache();
        try {
            if (!stop.stop_requested()) {
                ScopedBackgroundIoMode bgIo;
                const auto rawParsers = toRawParserPointers(parsers);
                if (tools3000::service::db::DatabaseManager::instance().saveSnapshot(rawParsers)) {
                    recordPersistedUsns(rawParsers);
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Rebuild snapshot failed: {}", e.what());
        } catch (...) {
            spdlog::error("Rebuild snapshot failed with unknown exception");
        }
        g_RebuildInProgress.store(false, std::memory_order_release);
        if (!stop.stop_requested()) {
            PerformGentlePostWorkTrim("rebuild");
        }
    });
    return true;
}

// 主程序退出时会通过管道请求停机。工作线程此刻大多阻塞在 ConnectNamedPipe 上，
// 单靠清掉运行标志叫不醒它们，还得真的连上来几次。这件事必须交给另一个线程，
// 否则正在处理停机请求的那个线程会连到自己身上。
void RequestServiceShutdown() {
    if (!g_IsRunning.exchange(false)) return;
    if (g_ServiceStopEvent != INVALID_HANDLE_VALUE) SetEvent(g_ServiceStopEvent);
    if (HWND hotplugHwnd = g_HotplugWindow.exchange(nullptr, std::memory_order_acq_rel)) {
        PostMessageW(hotplugHwnd, WM_CLOSE, 0, 0);
    }

    g_PipePokeJob = std::jthread([](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        const auto pokePipe = [](const std::string& name) -> bool {
            if (name.empty()) return false;
            const std::wstring wideName(name.begin(), name.end());
            HANDLE poke = CreateFileW(wideName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (poke == INVALID_HANDLE_VALUE) {
                return GetLastError() != ERROR_FILE_NOT_FOUND;
            }
            CloseHandle(poke);
            return true;
        };
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            const bool mainAlive = pokePipe(g_SearchPipeName);
            const bool compatAlive = pokePipe(g_CompatTokenPipeName);
            if (!mainAlive && !compatAlive) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void PerformServiceMemoryTrim(uint64_t* outBeforeWs = nullptr, uint64_t* outAfterWs = nullptr) {
    std::lock_guard<std::mutex> trimLock(g_MemoryTrimMutex);
    uint64_t beforeWs = 0;
    uint64_t beforePriv = 0;
    querySelfMemoryUsage(beforeWs, beforePriv);
    if (outBeforeWs) *outBeforeWs = beforeWs;

    clearAllContentCachesForTrim();
    tools3000::service::content::ContentTextCache::instance().clear();
    tools3000::service::launcher::AppsFolderCatalog::instance().invalidate();

    // 保持所有卷的 USN 实时监听线程 100% 存活，绝不遗漏任何文件变更。
    const auto parsers = snapshotParsers();
    for (const auto& parser : parsers) {
        if (!parser) continue;
        parser->releaseQueryCaches();
        parser->shrinkIndexCapacity();
    }

    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);

    uint64_t afterWs = 0;
    uint64_t afterPriv = 0;
    querySelfMemoryUsage(afterWs, afterPriv);
    if (outAfterWs) *outAfterWs = afterWs;

    g_LastTrimmedAtMs.store(currentEpochMillis(), std::memory_order_release);
    g_LastTrimMonotonicTick.store(GetTickCount64(), std::memory_order_release);
    g_TrimmedDuringIdle.store(true, std::memory_order_release);
    spdlog::info(
        "Service cold memory trim completed: workingSet {} MB -> {} MB, private {} MB",
        beforeWs / (1024ULL * 1024ULL),
        afterWs / (1024ULL * 1024ULL),
        afterPriv / (1024ULL * 1024ULL));
}

void flushHistoryAndSaveSnapshotIfNeeded(bool forceSave) {
    if (!forceSave && g_ShutdownSnapshotFlushed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (forceSave) {
        g_ShutdownSnapshotFlushed.store(true, std::memory_order_release);
    }
    try {
        tools3000::service::db::RunHistoryManager::instance().flush();
        tools3000::service::db::SearchHistoryManager::instance().save();
    } catch (...) {
    }

    const auto owners = snapshotParsers();
    const auto rawParsers = toRawParserPointers(owners);
    if (rawParsers.empty()) return;

    if (forceSave || hasAnyUsnAdvancedSincePersist(rawParsers)) {
        try {
            const bool saved = tools3000::service::db::DatabaseManager::instance().saveSnapshot(rawParsers);
            if (saved) {
                recordPersistedUsns(rawParsers);
            }
            spdlog::info("Pre-shutdown USN-advance snapshot {}", saved ? "saved" : "skipped/failed");
        } catch (const std::exception& e) {
            spdlog::error("Pre-shutdown snapshot failed: {}", e.what());
        } catch (...) {
            spdlog::error("Pre-shutdown snapshot failed with unknown exception");
        }
    }
}

void MemoryPolicyWatchdogThread() {
    spdlog::info("Memory policy watchdog thread started.");
    while (g_IsRunning.load(std::memory_order_acquire)) {
        const DWORD waitRes = (g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != nullptr)
            ? WaitForSingleObject(g_ServiceStopEvent, 15000)
            : WAIT_TIMEOUT;
        if (waitRes == WAIT_OBJECT_0 || !g_IsRunning.load(std::memory_order_acquire)) {
            break;
        }
        if (g_ServiceStopEvent == INVALID_HANDLE_VALUE || g_ServiceStopEvent == nullptr) {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            if (!g_IsRunning.load(std::memory_order_acquire)) break;
        }

        if (!g_SearchIndexReady.load(std::memory_order_acquire) ||
            g_InFlightUserQueries.load(std::memory_order_acquire) > 0 ||
            g_RebuildInProgress.load(std::memory_order_acquire) ||
            g_SnapshotInProgress.load(std::memory_order_acquire) ||
            g_InitialIndexWorkers.load(std::memory_order_acquire) > 0 ||
            g_DynamicIndexWorkers.load(std::memory_order_acquire) > 0) {
            continue;
        }

        const auto mode = static_cast<ServiceMemoryMode>(g_MemoryMode.load(std::memory_order_acquire));
        if (mode == ServiceMemoryMode::FullResident) {
            continue;
        }

        const uint64_t nowTick = GetTickCount64();
        const uint64_t lastTick = g_LastUserActivityTick.load(std::memory_order_acquire);
        const uint64_t idleMs = (lastTick > 0 && nowTick > lastTick) ? (nowTick - lastTick) : 0ULL;
        const uint64_t trimThresholdMs = static_cast<uint64_t>(g_IdleTrimMinutes.load(std::memory_order_acquire)) * 60000ULL;
        const uint64_t stopThresholdMs = static_cast<uint64_t>(g_IdleStopMinutes.load(std::memory_order_acquire)) * 60000ULL;

        // 闲置期高水位再修剪判定：首次达到闲置阈值立即修剪；若闲置期间被后台 USN 写盘活动
        // （如 Docker / Syncthing）再次将工作集顶回 >= 64 MiB，则按冷却间隔温和再修剪，
        // 避免常驻服务物理内存长时间飘在数百 MB 且保持 USN 监听 100% 存活。
        constexpr uint64_t kIdleRetrimWatermarkBytes = 64ULL * 1024ULL * 1024ULL;
        constexpr uint64_t kIdleRetrimMinCooldownMs = 120000ULL;
        const auto shouldRunIdleTrim = [&]() -> bool {
            if (idleMs < trimThresholdMs) return false;
            if (!g_TrimmedDuringIdle.load(std::memory_order_acquire)) {
                return true;
            }
            const uint64_t lastTrimTick = g_LastTrimMonotonicTick.load(std::memory_order_acquire);
            const uint64_t sinceLastTrimMs = (lastTrimTick > 0 && nowTick >= lastTrimTick)
                ? (nowTick - lastTrimTick)
                : trimThresholdMs;
            const uint64_t retrimCooldownMs = (std::max)(trimThresholdMs, kIdleRetrimMinCooldownMs);
            if (sinceLastTrimMs < retrimCooldownMs) {
                return false;
            }
            uint64_t curWs = 0;
            uint64_t curPriv = 0;
            querySelfMemoryUsage(curWs, curPriv);
            return curWs >= kIdleRetrimWatermarkBytes;
        };

        if (mode == ServiceMemoryMode::IdleTrim) {
            if (shouldRunIdleTrim()) {
                PerformServiceMemoryTrim();
            }
        } else if (mode == ServiceMemoryMode::IdleStop) {
            if (idleMs >= stopThresholdMs) {
                spdlog::info(
                    "Search service idle for {}s under idle_stop mode (threshold {}m); flushing snapshot and stopping.",
                    idleMs / 1000ULL,
                    g_IdleStopMinutes.load(std::memory_order_acquire));
                flushHistoryAndSaveSnapshotIfNeeded(false);
                RequestServiceShutdown();
                break;
            }
            if (trimThresholdMs < stopThresholdMs && shouldRunIdleTrim()) {
                PerformServiceMemoryTrim();
            }
        }
    }
    spdlog::info("Memory policy watchdog thread stopped.");
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

namespace {

// 跨代码页宽字符安全滚动日志 Sink：直接基于 Win32 CreateFileW 写入 UTF-8 日志，
// 彻底免疫无 8.3 短文件名卷与简/繁/日/韩/俄/西/葡等全 8 语种路径导致的窄串失败。
class WideServiceRotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    WideServiceRotatingFileSink(std::filesystem::path basePath, size_t maxFileSize, size_t maxFiles)
        : m_basePath(std::move(basePath)),
          m_maxFileSize(maxFileSize > 0 ? maxFileSize : 5 * 1024 * 1024),
          m_maxFiles(maxFiles) {
        openLocked();
    }

    ~WideServiceRotatingFileSink() override {
        closeLocked();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);
        if (m_handle == INVALID_HANDLE_VALUE) {
            openLocked();
        }
        if (m_handle == INVALID_HANDLE_VALUE) return;

        if (m_currentSize + formatted.size() > m_maxFileSize) {
            rotateLocked();
        }
        if (m_handle == INVALID_HANDLE_VALUE) return;

        DWORD written = 0;
        if (WriteFile(m_handle, formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr)) {
            m_currentSize += written;
        }
    }

    void flush_() override {
        if (m_handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_handle);
        }
    }

private:
    std::filesystem::path rotatedPath(size_t index) const {
        if (index == 0) return m_basePath;
        std::wstring p = m_basePath.wstring();
        const size_t dot = p.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > m_basePath.parent_path().wstring().size()) {
            return std::filesystem::path(p.substr(0, dot) + L"." + std::to_wstring(index) + p.substr(dot));
        }
        return std::filesystem::path(p + L"." + std::to_wstring(index));
    }

    void openLocked() {
        closeLocked();
        std::error_code ec;
        std::filesystem::create_directories(m_basePath.parent_path(), ec);
        m_handle = CreateFileW(
            m_basePath.wstring().c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (m_handle != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(m_handle, &sz) && sz.QuadPart >= 0) {
                m_currentSize = static_cast<size_t>(sz.QuadPart);
            } else {
                m_currentSize = 0;
            }
        }
    }

    void closeLocked() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    void rotateLocked() {
        closeLocked();
        std::error_code ec;
        for (size_t i = m_maxFiles; i > 0; --i) {
            const auto src = rotatedPath(i - 1);
            const auto dst = rotatedPath(i);
            if (!std::filesystem::exists(src, ec)) continue;
            if (i == m_maxFiles) {
                std::filesystem::remove(dst, ec);
            }
            MoveFileExW(src.wstring().c_str(), dst.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        openLocked();
    }

    std::filesystem::path m_basePath;
    size_t m_maxFileSize;
    size_t m_maxFiles;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    size_t m_currentSize = 0;
};

}  // namespace

void InitLogger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        std::filesystem::path logDir;
        PWSTR programData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr,
                                           &programData)) && programData) {
            logDir = std::filesystem::path(programData) / L"Tools3000" / L"logs";
            CoTaskMemFree(programData);
        } else {
            logDir = std::filesystem::temp_directory_path() / L"Tools3000" / L"logs";
        }
        std::filesystem::create_directories(logDir);
        auto file_sink = std::make_shared<WideServiceRotatingFileSink>(
            logDir / L"Tools3000_Service.log", 5 * 1024 * 1024, 3);
        spdlog::sinks_init_list sink_list = { console_sink, file_sink };
        auto logger = std::make_shared<spdlog::logger>("service", sink_list.begin(), sink_list.end());
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cout << "Log init failed: " << ex.what() << std::endl;
    }
}

// --------------------------------------------------------------------------------------
// Named Pipe Server
// --------------------------------------------------------------------------------------
#include <chrono>

nlohmann::json ProcessSearchQuery(
    const std::wstring& rawInput,
    const tools3000::service::pipe_auth::PipeClientAuthSession& authSession = {}
) {
    const HANDLE hClientToken = authSession.clientToken;
    const bool isSameUserProcessSid = authSession.isSameUserProcessSid;
    const std::wstring effectiveClientSid = !authSession.clientSid.empty()
        ? authSession.clientSid
        : g_SearchClientSid;

    std::wstring wQuery;
    std::string searchMode = "name";
    std::vector<char> enabledDrives;
    SearchExcludeOptions excludeOpts;
    SearchParseOptions parseOpts;
    size_t requestedLimit = 100;
    bool limitWasAutoZero = false;
    bool progressive = false;
    size_t requestCursor = 0;
    uint64_t requestedPlanId = 0;
    uint64_t queryId = 0;
    std::vector<std::pair<std::wstring, double>> dialogPriorities;
    std::optional<tools3000::service::content::ContentFormatFilter> requestFormatFilter;
    std::string formatSignaturePart;
    bool isExternalClient = false;

    std::string utf8Input = WStringToString(rawInput);
    if (!g_SearchIndexReady.load(std::memory_order_acquire) ||
        g_InitialIndexWorkers.load(std::memory_order_acquire) > 0) {
        bool isAllowedDuringInit = false;
        if (!utf8Input.empty() && utf8Input.front() == '{') {
            try {
                const auto request = nlohmann::json::parse(utf8Input);
                const std::string action = request.value("action", "");
                isAllowedDuringInit =
                    (action == "shutdown" || action == "cancel" || action == "getDbStats" ||
                     action == "getMemoryStats" || action == "updateMemoryPolicy" ||
                     action == "getIndexGeneration");
            } catch (...) {
                // Invalid requests are also deferred while startup owns the
                // parser collection, rather than racing initialization.
            }
        }
        if (!isAllowedDuringInit) {
            return {{"results", nlohmann::json::array()}, {"available", false},
                    {"initializing", true}, {"error", "search service initializing"}};
        }
    }
    if (!utf8Input.empty() && utf8Input.front() == '{') {
        try {
            auto reqJson = nlohmann::json::parse(utf8Input);
            if (reqJson.contains("clientSource") && reqJson["clientSource"].is_string()) {
                isExternalClient = tools3000::service::external_api::isExternalClientSource(
                    reqJson["clientSource"].get_ref<const std::string&>());
            }
            if (reqJson.contains("action") && reqJson["action"].is_string()) {
                std::string act = reqJson["action"].get<std::string>();
                if (isExternalClient && !tools3000::service::external_api::isActionAllowedForExternal(act)) {
                    return {{"success", false}, {"error", "read-only external API forbids mutating action: " + act}};
                }
                if (act == "cancel") {
                    tools3000::service::query::sharedEpochTracker().cancelAll();
                    return {{"success", true}, {"cancelled", true}};
                }
                if (act == "getIndexGeneration") {
                    // 极轻量只读代次探测：不调用 markUserActivity()，绝不干扰空闲内存休眠策略。
                    const auto parsers = snapshotParsers();
                    uint64_t combinedGeneration = 0;
                    uint64_t totalMemRecords = 0;
                    for (const auto& p : parsers) {
                        if (!p) continue;
                        combinedGeneration += p->getIndexGeneration();
                        totalMemRecords += p->getFileCount();
                    }
                    return {
                        {"success", true},
                        {"running", true},
                        {"ready", g_SearchIndexReady.load(std::memory_order_acquire)},
                        {"indexGeneration", combinedGeneration},
                        {"totalIndexedFiles", totalMemRecords}
                    };
                }
                if (act == "getMemoryStats") {
                    uint64_t wsBytes = 0;
                    uint64_t privBytes = 0;
                    querySelfMemoryUsage(wsBytes, privBytes);
                    const uint64_t nowTick = GetTickCount64();
                    const uint64_t lastTick = g_LastUserActivityTick.load(std::memory_order_acquire);
                    const uint64_t idleSeconds = (lastTick > 0 && nowTick > lastTick)
                        ? ((nowTick - lastTick) / 1000ULL)
                        : 0ULL;
                    const auto mode = static_cast<ServiceMemoryMode>(g_MemoryMode.load(std::memory_order_acquire));
                    return {
                        {"success", true},
                        {"running", true},
                        {"workingSetBytes", wsBytes},
                        {"privateBytes", privBytes},
                        {"memoryMode", serviceMemoryModeToString(mode)},
                        {"idleTrimMinutes", g_IdleTrimMinutes.load(std::memory_order_acquire)},
                        {"idleStopMinutes", g_IdleStopMinutes.load(std::memory_order_acquire)},
                        {"idleSeconds", idleSeconds},
                        {"lastTrimmedAt", g_LastTrimmedAtMs.load(std::memory_order_acquire)}
                    };
                }
                if (act == "updateMemoryPolicy") {
                    if (reqJson.contains("memoryMode") && reqJson["memoryMode"].is_string()) {
                        const auto nextMode = parseServiceMemoryMode(reqJson["memoryMode"].get_ref<const std::string&>());
                        g_MemoryMode.store(static_cast<int>(nextMode), std::memory_order_release);
                        g_TrimmedDuringIdle.store(false, std::memory_order_release);
                    }
                    if (reqJson.contains("idleTrimMinutes") &&
                        (reqJson["idleTrimMinutes"].is_number_integer() || reqJson["idleTrimMinutes"].is_number_unsigned())) {
                        const int64_t v = reqJson["idleTrimMinutes"].get<int64_t>();
                        g_IdleTrimMinutes.store(
                            static_cast<uint32_t>(std::clamp<int64_t>(v, 1, 120)),
                            std::memory_order_release);
                        g_TrimmedDuringIdle.store(false, std::memory_order_release);
                    }
                    if (reqJson.contains("idleStopMinutes") &&
                        (reqJson["idleStopMinutes"].is_number_integer() || reqJson["idleStopMinutes"].is_number_unsigned())) {
                        const int64_t v = reqJson["idleStopMinutes"].get<int64_t>();
                        g_IdleStopMinutes.store(
                            static_cast<uint32_t>(std::clamp<int64_t>(v, 5, 720)),
                            std::memory_order_release);
                    }
                    const auto activeMode = static_cast<ServiceMemoryMode>(g_MemoryMode.load(std::memory_order_acquire));
                    spdlog::info(
                        "Updated search service memory policy: mode={}, idleTrimMinutes={}, idleStopMinutes={}",
                        serviceMemoryModeToString(activeMode),
                        g_IdleTrimMinutes.load(std::memory_order_acquire),
                        g_IdleStopMinutes.load(std::memory_order_acquire));
                    return {
                        {"success", true},
                        {"memoryMode", serviceMemoryModeToString(activeMode)},
                        {"idleTrimMinutes", g_IdleTrimMinutes.load(std::memory_order_acquire)},
                        {"idleStopMinutes", g_IdleStopMinutes.load(std::memory_order_acquire)}
                    };
                }
                if (act == "trimMemory") {
                    uint64_t beforeWs = 0;
                    uint64_t afterWs = 0;
                    PerformServiceMemoryTrim(&beforeWs, &afterWs);
                    uint64_t curWs = 0;
                    uint64_t curPriv = 0;
                    querySelfMemoryUsage(curWs, curPriv);
                    return {
                        {"success", true},
                        {"running", true},
                        {"beforeWorkingSetBytes", beforeWs},
                        {"afterWorkingSetBytes", afterWs},
                        {"workingSetBytes", curWs},
                        {"privateBytes", curPriv},
                        {"lastTrimmedAt", g_LastTrimmedAtMs.load(std::memory_order_acquire)}
                    };
                }
                if (act == "rebuild" || act == "reindex") {
                    markUserActivity();
                    const bool started = scheduleRebuild();
                    return {{"success", started}, {"rebuilding", started},
                            {"alreadyRunning", !started && g_RebuildInProgress.load()}};
                }
                if (act == "catchup" || act == "sync") {
                    markUserActivity();
                    pruneRemovedVolumes();
                    bool anyUpdated = false;
                    for (const auto& parser : snapshotParsers()) {
                        if (!parser) continue;
                        uint64_t lastUsn = parser->getCurrentUsn();
                        if (parser->catchUpUsnJournal(lastUsn)) {
                            anyUpdated = true;
                        }
                    }
                    if (anyUpdated) {
                        invalidateContentPlanCache();
                        tools3000::service::launcher::AppsFolderCatalog::instance().invalidate();
                    }
                    return {{"success", true}, {"synced", true}, {"updated", anyUpdated}};
                }
                if (act == "syncVolumes") {
                    markUserActivity();
                    pruneRemovedVolumes();
                    std::vector<char> requested;
                    if (reqJson.contains("drives") && reqJson["drives"].is_array()) {
                        for (const auto& d : reqJson["drives"]) {
                            if (d.is_string()) {
                                const std::string s = d.get<std::string>();
                                if (!s.empty() && std::isalpha(static_cast<unsigned char>(s[0]))) {
                                    requested.push_back(static_cast<char>(
                                        std::toupper(static_cast<unsigned char>(s[0]))));
                                }
                            }
                        }
                    }
                    const size_t mounted = ensureRequestedDrivesMounted(requested);
                    return {{"success", true}, {"mounted", mounted}};
                }
                if (act == "recordRun") {
                    markUserActivity();
                    if (reqJson.contains("path") && reqJson["path"].is_string()) {
                        const auto& pathUtf8 = reqJson["path"].get_ref<const std::string&>();
                        if (pathUtf8.size() > tools3000::service::search_limits::MaxPathUtf8Bytes) {
                            return {{"success", false}, {"error", "path exceeds limit"}};
                        }
                        std::wstring path = expandQueryEnvForClient(
                            StringToWString(pathUtf8), hClientToken, isSameUserProcessSid);
                        if (!tools3000::service::pipe_auth::isPathAllowedForClient(path, authSession)) {
                            return {{"success", false}, {"error", "access denied"}};
                        }
                        tools3000::service::db::RunHistoryManager::instance().recordRun(path);
                        return {{"success", true}};
                    }
                    return {{"success", false}};
                }
                if (act == "recordSearch") {
                    markUserActivity();
                    if (reqJson.contains("query") && reqJson["query"].is_string()) {
                        const auto& queryUtf8 = reqJson["query"].get_ref<const std::string&>();
                        if (queryUtf8.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
                            return {{"success", false}, {"error", "query exceeds limit"}};
                        }
                        std::wstring q = StringToWString(queryUtf8);
                        tools3000::service::db::SearchHistoryManager::instance().recordSearch(q);
                        return {{"success", true}};
                    }
                    return {{"success", false}};
                }
                if (act == "getSearchHistory") {
                    size_t limit = 30;
                    if (const auto it = reqJson.find("limit"); it != reqJson.end()) {
                        if (!it->is_number_integer() && !it->is_number_unsigned()) {
                            return {{"success", false}, {"error", "limit must be an integer"}};
                        }
                        if (it->is_number_unsigned()) {
                            limit = static_cast<size_t>((std::min<std::uint64_t>)(
                                it->get<std::uint64_t>(), tools3000::service::search_limits::MaxHistoryResults));
                        } else {
                            const auto value = it->get<std::int64_t>();
                            limit = value <= 0 ? 0 : static_cast<size_t>((std::min<std::int64_t>)(
                                value, tools3000::service::search_limits::MaxHistoryResults));
                        }
                    }
                    auto list = tools3000::service::db::SearchHistoryManager::instance().getRecentSearches(limit);
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto& item : list) {
                        arr.push_back({
                            {"search", WStringToString(item.search)},
                            {"searchCount", item.searchCount},
                            {"lastSearchDate", item.lastSearchDate}
                        });
                    }
                    return {{"success", true}, {"history", arr}};
                }
                if (act == "removeSearchHistory") {
                    if (reqJson.contains("search") && reqJson["search"].is_string()) {
                        const auto& searchUtf8 = reqJson["search"].get_ref<const std::string&>();
                        if (searchUtf8.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
                            return {{"success", false}, {"error", "search exceeds limit"}};
                        }
                        std::wstring q = StringToWString(searchUtf8);
                        bool ok = tools3000::service::db::SearchHistoryManager::instance().removeSearch(q);
                        return {{"success", ok}};
                    }
                    return {{"success", false}};
                }
                if (act == "clearSearchHistory") {
                    tools3000::service::db::SearchHistoryManager::instance().clear();
                    return {{"success", true}};
                }
                if (act == "getDbStats") {
                    auto stats = tools3000::service::db::DatabaseManager::instance().getStats();
                    const bool searchIndexReady = g_SearchIndexReady.load(std::memory_order_acquire);
                    uint64_t totalMemRecords = 0;
                    uint32_t indexedVolumes = 0;
                    uint32_t totalVolumes = 0;

                    auto phaseToString = [](MftParser::ScanPhase ph) noexcept -> const char* {
                        switch (ph) {
                            case MftParser::ScanPhase::Scanning: return "scanning";
                            case MftParser::ScanPhase::BuildingHierarchy: return "building";
                            case MftParser::ScanPhase::Syncing: return "syncing";
                            case MftParser::ScanPhase::LoadingSnapshot: return "loading";
                            case MftParser::ScanPhase::Idle:
                            default: return "idle";
                        }
                    };

                    std::string activePhase = searchIndexReady ? "idle" : "starting";
                    std::string activeDrive;
                    std::string activeVolumeLabel;
                    uint64_t activeScanned = 0;
                    uint64_t activeEstimated = 0;
                    double weightedVolumeProgressSum = 0.0;
                    nlohmann::json volumesJson = nlohmann::json::array();

                    const auto parsers = snapshotParsers();
                    totalVolumes = static_cast<uint32_t>(parsers.size());
                    for (const auto& p : parsers) {
                        if (!p) continue;
                        const size_t recCount = p->getFileCount();
                        const auto ph = p->getScanPhase();
                        const uint64_t scanned = p->getScannedRecords();
                        const uint64_t estimated = p->getEstimatedRecords();
                        const bool done = p->isInitialScanDone();
                        const std::string driveStr(1, p->getDriveLetter());
                        const std::string labelUtf8 = WStringToString(p->getVolumeLabel());

                        totalMemRecords += recCount;
                        if (done) {
                            ++indexedVolumes;
                            weightedVolumeProgressSum += 1.0;
                        } else if (ph == MftParser::ScanPhase::BuildingHierarchy) {
                            weightedVolumeProgressSum += 0.92;
                        } else if (estimated > 0 && scanned > 0) {
                            weightedVolumeProgressSum += std::clamp(
                                (static_cast<double>(scanned) / static_cast<double>(estimated)) * 0.90,
                                0.01,
                                0.90);
                        }
                        if (ph != MftParser::ScanPhase::Idle && activeDrive.empty()) {
                            activePhase = phaseToString(ph);
                            activeDrive = driveStr;
                            activeVolumeLabel = labelUtf8;
                            activeScanned = scanned;
                            activeEstimated = estimated;
                        }

                        volumesJson.push_back({
                            {"drive", driveStr},
                            {"label", labelUtf8},
                            {"phase", phaseToString(ph)},
                            {"records", recCount},
                            {"scanned", scanned},
                            {"estimated", estimated},
                            {"done", done}
                        });
                    }

                    const uint64_t effectiveRecords = totalMemRecords > 0 ? totalMemRecords : stats.totalRecords;
                    const uint32_t effectiveVolumes = totalVolumes > 0 ? totalVolumes : stats.volumeCount;
                    const int initialWorkers = g_InitialIndexWorkers.load(std::memory_order_acquire);
                    const int dynamicWorkers = g_DynamicIndexWorkers.load(std::memory_order_acquire);
                    const bool rebuildInProgress = g_RebuildInProgress.load(std::memory_order_acquire);
                    const bool snapshotInProgress = g_SnapshotInProgress.load(std::memory_order_acquire);

                    // 只有当所有已注册卷均完成初始扫描且无在途重建/扫描工作线程时，indexing 才转为 false (`U-08`)。
                    // 快照落盘 (snapshotInProgress) 属于后台纯异步任务，绝不阻塞前端即时搜索。
                    const bool isIndexing = !searchIndexReady ||
                                            initialWorkers > 0 ||
                                            dynamicWorkers > 0 ||
                                            rebuildInProgress ||
                                            (indexedVolumes < totalVolumes);

                    if (activePhase == "idle" && snapshotInProgress) {
                        activePhase = "saving";
                    }

                    int progressPercent = isIndexing ? 0 : 100;
                    if (isIndexing) {
                        if (totalVolumes > 0) {
                            const double aggPct = (weightedVolumeProgressSum / static_cast<double>(totalVolumes)) * 100.0;
                            progressPercent = std::clamp(static_cast<int>(std::lround(aggPct)), 1, 99);
                        } else if (activePhase == "building") {
                            progressPercent = 92;
                        } else if (activeEstimated > 0 && activeScanned > 0) {
                            const double rawPct = (static_cast<double>(activeScanned) / static_cast<double>(activeEstimated)) * 90.0;
                            progressPercent = std::clamp(static_cast<int>(rawPct), 1, 90);
                        }
                    }

                    return {
                        {"success", true},
                        {"dbPath", WStringToString(stats.dbPath)},
                        {"dbSize", stats.fileSize},
                        {"timestamp", stats.timestamp},
                        {"totalRecords", effectiveRecords},
                        {"volumeCount", effectiveVolumes},
                        {"indexedVolumes", indexedVolumes},
                        {"totalVolumes", totalVolumes},
                        {"exists", stats.exists || totalMemRecords > 0},
                        {"indexing", isIndexing},
                        {"phase", activePhase},
                        {"activeDrive", activeDrive},
                        {"activeVolumeLabel", activeVolumeLabel},
                        {"scannedRecords", activeScanned},
                        {"estimatedRecords", activeEstimated},
                        {"progressPercent", progressPercent},
                        {"volumes", std::move(volumesJson)},
                        {"initialWorkers", initialWorkers},
                        {"dynamicWorkers", dynamicWorkers},
                        {"rebuildInProgress", rebuildInProgress},
                        {"snapshotInProgress", snapshotInProgress},
                        {"runHistoryCount", tools3000::service::db::RunHistoryManager::instance().size()},
                        {"searchHistoryCount", tools3000::service::db::SearchHistoryManager::instance().size()}
                    };
                }
                if (act == "saveSnapshot") {
                    markUserActivity();
                    const auto owners = snapshotParsers();
                    const auto allParsers = toRawParserPointers(owners);
                    bool ok = tools3000::service::db::DatabaseManager::instance().saveSnapshot(allParsers);
                    if (ok) {
                        recordPersistedUsns(allParsers);
                    }
                    return {{"success", ok}};
                }
                if (act == "shutdown") {
                    const bool shouldSaveSnapshot = reqJson.value("saveSnapshot", false);
                    if (shouldSaveSnapshot) {
                        spdlog::info("Received graceful shutdown request with saveSnapshot=true, flushing USN snapshot.");
                        flushHistoryAndSaveSnapshotIfNeeded(false);
                    } else {
                        // 快照不在这里落盘：写一份要几百 MB，而下次启动导入快照时本就会用
                        // USN 日志补齐停机期间的变动，多花的磁盘代价换不来任何准确性。
                        spdlog::info("Received shutdown request from client, stopping service.");
                    }
                    RequestServiceShutdown();
                    return {{"success", true}, {"stopping", true}};
                }
            }

            if (reqJson.contains("query")) {
                if (!reqJson["query"].is_string()) {
                    return {{"results", nlohmann::json::array()}, {"error", "query must be a string"}};
                }
                const auto& queryUtf8 = reqJson["query"].get_ref<const std::string&>();
                if (queryUtf8.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
                    return {{"results", nlohmann::json::array()}, {"error", "query exceeds 1024-byte limit"}};
                }
                wQuery = expandQueryEnvForClient(
                    StringToWString(queryUtf8), hClientToken, isSameUserProcessSid);
            }
            if (const auto qidIt = reqJson.find("queryId"); qidIt != reqJson.end() && !qidIt->is_null()) {
                if (qidIt->is_number_unsigned()) {
                    queryId = qidIt->get<uint64_t>();
                } else if (qidIt->is_number_integer()) {
                    const auto v = qidIt->get<int64_t>();
                    if (v > 0) queryId = static_cast<uint64_t>(v);
                }
            }
            if (reqJson.contains("progressive") && reqJson["progressive"].is_boolean()) {
                progressive = reqJson["progressive"].get<bool>();
            }
            if (const auto cursorIt = reqJson.find("cursor"); cursorIt != reqJson.end() && !cursorIt->is_null()) {
                if (cursorIt->is_number_unsigned()) {
                    requestCursor = static_cast<size_t>(cursorIt->get<std::uint64_t>());
                } else if (cursorIt->is_number_integer()) {
                    const auto v = cursorIt->get<std::int64_t>();
                    if (v > 0) requestCursor = static_cast<size_t>(v);
                }
            }
            if (const auto planIt = reqJson.find("planId"); planIt != reqJson.end() && !planIt->is_null()) {
                if (planIt->is_number_unsigned()) {
                    requestedPlanId = planIt->get<std::uint64_t>();
                } else if (planIt->is_number_integer()) {
                    const auto v = planIt->get<std::int64_t>();
                    if (v > 0) requestedPlanId = static_cast<std::uint64_t>(v);
                }
            }
            if (reqJson.contains("searchMode") && reqJson["searchMode"].is_string()) {
                searchMode = reqJson["searchMode"].get<std::string>();
                if (searchMode != "name" && searchMode != "both" && searchMode != "content") {
                    return {{"results", nlohmann::json::array()}, {"error", "invalid search mode"}};
                }
            }
            if (reqJson.contains("drives") && reqJson["drives"].is_array()) {
                if (reqJson["drives"].size() > tools3000::service::search_limits::MaxDriveItems) {
                    return {{"results", nlohmann::json::array()}, {"error", "too many drives"}};
                }
                for (const auto& d : reqJson["drives"]) {
                    if (d.is_string()) {
                        std::string s = d.get<std::string>();
                        if (s.empty() || s.size() > 3 || !std::isalpha(static_cast<unsigned char>(s[0]))) {
                            return {{"results", nlohmann::json::array()}, {"error", "invalid drive"}};
                        }
                        enabledDrives.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[0]))));
                    } else {
                        return {{"results", nlohmann::json::array()}, {"error", "invalid drive"}};
                    }
                }
            } else if (reqJson.contains("drives") && !reqJson["drives"].is_null()) {
                return {{"results", nlohmann::json::array()}, {"error", "drives must be an array"}};
            }
            if (reqJson.contains("excludes") && reqJson["excludes"].is_array()) {
                if (reqJson["excludes"].size() > tools3000::service::search_limits::MaxExcludeItems) {
                    return {{"results", nlohmann::json::array()}, {"error", "too many exclude patterns"}};
                }
                for (const auto& ex : reqJson["excludes"]) {
                    if (!ex.is_string()) return {{"results", nlohmann::json::array()}, {"error", "invalid exclude pattern"}};
                    const auto& s = ex.get_ref<const std::string&>();
                    if (s.size() > tools3000::service::search_limits::MaxExcludeUtf8Bytes) {
                        return {{"results", nlohmann::json::array()}, {"error", "exclude pattern exceeds limit"}};
                    }
                    if (!s.empty()) {
                        excludeOpts.patterns.push_back(expandQueryEnvForClient(
                            StringToWString(s), hClientToken, isSameUserProcessSid));
                    }
                }
            } else if (reqJson.contains("excludes") && !reqJson["excludes"].is_null()) {
                return {{"results", nlohmann::json::array()}, {"error", "excludes must be an array"}};
            }
            if (reqJson.contains("excludeHidden") && reqJson["excludeHidden"].is_boolean()) {
                excludeOpts.excludeHidden = reqJson["excludeHidden"].get<bool>();
            }
            if (reqJson.contains("excludeSystem") && reqJson["excludeSystem"].is_boolean()) {
                excludeOpts.excludeSystem = reqJson["excludeSystem"].get<bool>();
            }
            if (reqJson.contains("pinyinEnabled") && reqJson["pinyinEnabled"].is_boolean()) {
                parseOpts.pinyinEnabled = reqJson["pinyinEnabled"].get<bool>();
            } else {
                parseOpts.pinyinEnabled = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
            }
            if (reqJson.contains("matchPath") && reqJson["matchPath"].is_boolean()) {
                parseOpts.matchPath = reqJson["matchPath"].get<bool>();
            }
            if (reqJson.contains("caseSensitive") && reqJson["caseSensitive"].is_boolean()) {
                parseOpts.caseSensitive = reqJson["caseSensitive"].get<bool>();
            }
            if (reqJson.contains("limit")) {
                const auto& limit = reqJson["limit"];
                if (!limit.is_number_integer() && !limit.is_number_unsigned()) {
                    return {{"results", nlohmann::json::array()}, {"error", "limit must be an integer"}};
                }
                if (limit.is_number_unsigned()) {
                    const auto value = limit.get<std::uint64_t>();
                    if (value == 0) {
                        limitWasAutoZero = true;
                        requestedLimit = tools3000::service::search_limits::MaxResults;
                    } else {
                        requestedLimit = static_cast<size_t>((std::min<std::uint64_t>)(
                            value, tools3000::service::search_limits::MaxResults));
                    }
                } else {
                    const auto value = limit.get<std::int64_t>();
                    if (value <= 0) {
                        limitWasAutoZero = true;
                        requestedLimit = tools3000::service::search_limits::MaxResults;
                    } else {
                        requestedLimit = static_cast<size_t>((std::min<std::int64_t>)(
                            value, tools3000::service::search_limits::MaxResults));
                    }
                }
            }
            if (reqJson.contains("contentCustomExts") || reqJson.contains("contentDisabledExts")) {
                std::vector<std::wstring> customExts;
                std::vector<std::wstring> disabledExts;
                const auto readFormats = [&](const char* key, std::vector<std::wstring>& output, std::string& sigOut) {
                    const auto it = reqJson.find(key);
                    if (it == reqJson.end() || it->is_null()) return true;
                    if (!it->is_array() || it->size() > tools3000::service::search_limits::MaxFormatItems) return false;
                    for (const auto& item : *it) {
                        if (!item.is_string()) return false;
                        const auto& value = item.get_ref<const std::string&>();
                        if (value.empty() || value.size() > tools3000::service::search_limits::MaxFormatUtf8Bytes) return false;
                        output.push_back(StringToWString(value));
                        sigOut.append(value);
                        sigOut.push_back(',');
                    }
                    return true;
                };
                std::string customSig;
                std::string disabledSig;
                if (!readFormats("contentCustomExts", customExts, customSig) ||
                    !readFormats("contentDisabledExts", disabledExts, disabledSig)) {
                    return {{"results", nlohmann::json::array()}, {"error", "invalid content format list"}};
                }
                formatSignaturePart = customSig + "|" + disabledSig;
                requestFormatFilter = tools3000::service::content::ContentFormatFilter::fromLists(
                    customExts, disabledExts);
            }
            if (reqJson.contains("dialogPriorities") && reqJson["dialogPriorities"].is_array()) {
                for (const auto& item : reqJson["dialogPriorities"]) {
                    if (item.is_object() && item.contains("path") && item["path"].is_string()) {
                        std::wstring wp = expandQueryEnvForClient(
                            StringToWString(item["path"].get<std::string>()),
                            hClientToken,
                            isSameUserProcessSid);
                        if (!tools3000::service::pipe_auth::isPathAllowedForClient(wp, authSession)) {
                            continue;
                        }
                        for (auto& c : wp) c = std::towlower(c);
                        double sc = item.value("score", 3000.0);
                        if (!wp.empty()) dialogPriorities.emplace_back(std::move(wp), sc);
                    }
                }
            }
        } catch (const std::exception& error) {
            return {{"results", nlohmann::json::array()}, {"error", std::string("invalid request: ") + error.what()}};
        }
    } else {
        if (utf8Input.size() > tools3000::service::search_limits::MaxQueryUtf8Bytes) {
            return {{"results", nlohmann::json::array()}, {"error", "query exceeds 1024-byte limit"}};
        }
        wQuery = expandQueryEnvForClient(rawInput, hClientToken, isSameUserProcessSid);
    }
    const auto overallStartTime = std::chrono::steady_clock::now();
    markUserActivity();
    ScopedInFlightUserQuery activeQueryGuard;

    // Monotonic server-side epoch (`MED-17`): immune to client clock rollback or huge `queryId`.
    auto& epochTracker = tools3000::service::query::sharedEpochTracker();
    const uint64_t serverEpoch = epochTracker.issueServerEpoch(queryId);
    const auto isCancelled = [&epochTracker, serverEpoch]() noexcept {
        return epochTracker.isServerEpochStale(serverEpoch);
    };

    if (!enabledDrives.empty()) {
        ensureRequestedDrivesMounted(enabledDrives);
    }

    auto isDriveEnabled = [&](char driveLetter) {
        if (enabledDrives.empty()) return true;
        for (char d : enabledDrives) {
            if (std::toupper(d) == std::toupper(driveLetter)) return true;
        }
        return false;
    };

    const auto currentParsers = snapshotParsers();
    size_t totalIndexedFiles = 0;
    for (const auto& parser : currentParsers) {
        if (parser && isDriveEnabled(parser->getDriveLetter())) {
            totalIndexedFiles += parser->getFileCount();
        }
    }

    if (wQuery.empty()) {
        return {
            {"results", nlohmann::json::array()},
            {"totalIndexedFiles", totalIndexedFiles},
            {"elapsedMs", 0}
        };
    }

    SearchExpression expr = SearchExpression::parse(wQuery, parseOpts);
    const bool isExplicitContent = expr.hasContentFilter();
    const bool isContentRequest = isExplicitContent || searchMode == "content" || searchMode == "both";
    if (limitWasAutoZero && isContentRequest) {
        requestedLimit = tools3000::service::search_limits::DefaultContentMaxResults;
    }

    nlohmann::json responseJson = {{"results", nlohmann::json::array()}};

    auto buildContentPlanSignature = [&]() -> std::string {
        std::string sig;
        sig.reserve(256);
        sig.append(WStringToString(effectiveClientSid));
        sig.push_back(':');
        sig.push_back(authSession.isAdmin ? '1' : '0');
        sig.push_back('\x1f');
        sig.append(searchMode);
        sig.push_back('\x1f');
        sig.append(WStringToString(wQuery));
        sig.push_back('\x1f');
        std::vector<char> sortedDrives = enabledDrives;
        std::sort(sortedDrives.begin(), sortedDrives.end());
        for (char d : sortedDrives) sig.push_back(d);
        sig.push_back('\x1f');
        sig.push_back(excludeOpts.excludeHidden ? '1' : '0');
        sig.push_back(excludeOpts.excludeSystem ? '1' : '0');
        sig.push_back(parseOpts.pinyinEnabled ? '1' : '0');
        sig.push_back(parseOpts.matchPath ? '1' : '0');
        sig.push_back(parseOpts.caseSensitive ? '1' : '0');
        sig.push_back('\x1f');
        for (const auto& pat : excludeOpts.patterns) {
            sig.append(WStringToString(pat));
            sig.push_back(';');
        }
        sig.push_back('\x1f');
        sig.append(formatSignaturePart);
        sig.push_back('\x1f');
        sig.append(std::to_string(dialogPriorities.size()));
        if (!dialogPriorities.empty()) {
            sig.push_back(':');
            sig.append(WStringToString(dialogPriorities.front().first));
        }
        return sig;
    };

    const auto sharedHistorySnap = tools3000::service::db::RunHistoryManager::instance().captureSnapshot();

    const char systemDriveLetter = []() noexcept -> char {
        wchar_t winDir[MAX_PATH]{};
        const UINT len = GetSystemWindowsDirectoryW(winDir, MAX_PATH);
        if (len >= 2 && winDir[1] == L':' && std::iswalpha(winDir[0])) {
            return static_cast<char>(std::toupper(static_cast<unsigned char>(winDir[0])));
        }
        return 'C';
    }();

    auto runNameSearch = [&](const std::wstring& queryStr, size_t maxCount) -> std::vector<SearchResult> {
        const size_t perVolumeCap = authSession.isAdmin
            ? maxCount
            : (std::min)(
                  static_cast<size_t>(tools3000::service::search_limits::MaxResults),
                  (std::max)(maxCount * 4, static_cast<size_t>(512)));

        const auto parserOwners = snapshotParsers();
        std::vector<std::shared_ptr<MftParser>> activeParsers;
        activeParsers.reserve(parserOwners.size());
        for (const auto& parser : parserOwners) {
            if (parser && isDriveEnabled(parser->getDriveLetter())) {
                activeParsers.push_back(parser);
            }
        }

        std::vector<std::vector<SearchResult>> perVolumeResults(activeParsers.size());
        // 每个卷的 `MftParser::Search` 内部已在叶级满血调用 `QueryThreadPool::parallelFor` 并行扫描全部分片，
        // 外层按卷顺序驱动可最大化 L3 缓存局部性，同时彻底消除外层/内层嵌套入池引发的锁层次反转风险。
        for (size_t i = 0; i < activeParsers.size(); ++i) {
            if (isCancelled()) break;
            perVolumeResults[i] = activeParsers[i]->Search(
                queryStr, static_cast<int>(perVolumeCap), excludeOpts, isCancelled, parseOpts);
        }

        std::vector<SearchResult> results;
        std::unordered_set<std::wstring> existingLauncherStems;
        for (auto& volumeResults : perVolumeResults) {
            for (auto& candidate : volumeResults) {
                if (!tools3000::service::pipe_auth::isPathAllowedForClient(candidate.fullPath, authSession)) {
                    continue;
                }
                if (tools3000::service::launcher::resolveResultPathTier(candidate) ==
                    tools3000::service::kPathTierAppLauncher) {
                    existingLauncherStems.insert(
                        tools3000::service::launcher::extractLauncherStemLower(candidate.fileName, true));
                }
                results.push_back(std::move(candidate));
            }
        }

        if (!isCancelled() && !isExplicitContent && isDriveEnabled(systemDriveLetter)) {
            const SearchExpression localExpr = (queryStr == wQuery)
                ? expr
                : SearchExpression::parse(queryStr, parseOpts);
            auto uwpMatches = tools3000::service::launcher::AppsFolderCatalog::instance().queryMatchingApps(
                localExpr,
                excludeOpts,
                existingLauncherStems,
                hClientToken,
                isSameUserProcessSid,
                effectiveClientSid,
                (std::min)(maxCount, static_cast<size_t>(64)));
            for (auto& appItem : uwpMatches) {
                results.push_back(std::move(appItem));
            }
        }

        tools3000::service::launcher::sortLauncherAwareResults(results, sharedHistorySnap);
        if (results.size() > maxCount) {
            results.resize(maxCount);
        }
        return results;
    };

    struct ContentBatchOutcome {
        std::vector<nlohmann::json> items;
        bool hasMore = false;
        size_t nextCursor = 0;
        uint64_t planId = 0;
        size_t totalCandidates = 0;
    };

    auto runContentSearch = [&](const ContentSearchPlan& plan, size_t maxCount) -> ContentBatchOutcome {
        ContentBatchOutcome outcome;
        if (plan.contentTerms.empty() || maxCount == 0 || isCancelled()) {
            return outcome;
        }

        const tools3000::service::content::ContentFormatFilter* reqFilterPtr =
            requestFormatFilter.has_value() ? &(*requestFormatFilter) : nullptr;

        const std::string planSignature = progressive ? buildContentPlanSignature() : std::string{};
        std::shared_ptr<const std::vector<ContentCandidate>> candidatesPtr;
        uint64_t activePlanId = 0;
        size_t planBaseOffset = 0;

        if (progressive && requestCursor > 0 && requestedPlanId != 0) {
            if (auto cachedPlan = lookupCachedContentPlan(requestedPlanId, planSignature)) {
                candidatesPtr = std::move(cachedPlan->candidates);
                activePlanId = cachedPlan->planId;
                planBaseOffset = cachedPlan->baseCursorOffset;
            }
        }

        if (!candidatesPtr) {
            const bool isMidFlightRebuild = (progressive && requestCursor > 0 && requestedPlanId != 0);
            std::unordered_set<std::wstring> inheritedScanned = isMidFlightRebuild
                ? lookupPlanAnchorScannedPaths(requestedPlanId, planSignature)
                : std::unordered_set<std::wstring>{};

            ContentCandidateOptions candidateOpts;
            candidateOpts.excludeOpts = excludeOpts;
            candidateOpts.formatFilter = reqFilterPtr;
            candidateOpts.dialogPriorities = dialogPriorities;
            candidateOpts.maxCandidates = 300000;

            {
                wchar_t pathBuf[MAX_PATH] = {};
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, hClientToken, 0, pathBuf))) {
                    candidateOpts.userProfileDirLower = pathBuf;
                    for (auto& c : candidateOpts.userProfileDirLower) c = static_cast<wchar_t>(std::towlower(c));
                    if (!candidateOpts.userProfileDirLower.empty()) {
                        candidateOpts.downloadsDirLower = candidateOpts.userProfileDirLower + L"\\downloads";
                    }
                }
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, hClientToken, 0, pathBuf))) {
                    candidateOpts.desktopDirLower = pathBuf;
                    for (auto& c : candidateOpts.desktopDirLower) c = static_cast<wchar_t>(std::towlower(c));
                }
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, hClientToken, 0, pathBuf))) {
                    candidateOpts.documentsDirLower = pathBuf;
                    for (auto& c : candidateOpts.documentsDirLower) c = static_cast<wchar_t>(std::towlower(c));
                }
            }

            auto freshCandidates = std::make_shared<std::vector<ContentCandidate>>();
            for (const auto& parser : snapshotParsers()) {
                if (!parser || !isDriveEnabled(parser->getDriveLetter())) continue;
                if (isCancelled()) return outcome;
                auto volumeCandidates = parser->CollectContentCandidates(
                    plan.fileFilterExpr, candidateOpts, isCancelled);
                for (auto& cand : volumeCandidates) {
                    if (!tools3000::service::pipe_auth::isPathAllowedForClient(cand.fullPath, authSession)) {
                        continue;
                    }
                    if (!inheritedScanned.empty() &&
                        inheritedScanned.find(normalizeCandidatePathKey(cand.fullPath)) != inheritedScanned.end()) {
                        continue;
                    }
                    freshCandidates->push_back(std::move(cand));
                }
            }

            if (isCancelled()) {
                return outcome;
            }
            if (freshCandidates->empty()) {
                if (isMidFlightRebuild) {
                    outcome.planId = requestedPlanId;
                    outcome.nextCursor = requestCursor;
                    outcome.totalCandidates = requestCursor;
                    outcome.hasMore = false;
                }
                return outcome;
            }

            if (!sharedHistorySnap.empty()) {
                for (auto& item : *freshCandidates) {
                    const double frecency = sharedHistorySnap.lookup(item.fullPath).frecencyScore;
                    if (frecency > 0.0) {
                        item.priorityScore += (std::min)(5000.0, frecency * 100.0);
                    }
                }
            }

            constexpr size_t GlobalContentCandidateCap = 300000;
            auto compareCandidates = [](const ContentCandidate& a, const ContentCandidate& b) {
                if (a.priorityScore != b.priorityScore) return a.priorityScore > b.priorityScore;
                return a.lastWriteTime > b.lastWriteTime;
            };
            if (freshCandidates->size() > GlobalContentCandidateCap) {
                std::nth_element(
                    freshCandidates->begin(),
                    freshCandidates->begin() + GlobalContentCandidateCap,
                    freshCandidates->end(),
                    compareCandidates
                );
                freshCandidates->resize(GlobalContentCandidateCap);
            }
            std::stable_sort(freshCandidates->begin(), freshCandidates->end(), compareCandidates);

            candidatesPtr = freshCandidates;
            if (progressive) {
                planBaseOffset = isMidFlightRebuild ? requestCursor : 0;
                activePlanId = storeCachedContentPlan(
                    planSignature,
                    candidatesPtr,
                    planBaseOffset,
                    std::move(inheritedScanned));
                if (isMidFlightRebuild) {
                    rebindBothModeNameRemainderPlanId(requestedPlanId, activePlanId, planSignature);
                }
            }
        }

        const auto& textCandidates = *candidatesPtr;
        const size_t localTotal = textCandidates.size();
        outcome.planId = activePlanId;
        outcome.totalCandidates = planBaseOffset + localTotal;

        const size_t rawRelCursor = (progressive && requestCursor > planBaseOffset)
            ? (requestCursor - planBaseOffset)
            : 0;
        const size_t startCursor = progressive ? (std::min)(rawRelCursor, localTotal) : 0;
        if (startCursor >= localTotal || isCancelled()) {
            outcome.nextCursor = planBaseOffset + localTotal;
            outcome.hasMore = false;
            return outcome;
        }

        struct IndexedContentMatch {
            size_t candidateIdx = 0;
            nlohmann::json item;
        };

        auto& contentEngine = tools3000::service::content::ContentSearchEngine::instance();
        std::mutex resultsMutex;
        std::vector<IndexedContentMatch> indexedResults;
        std::atomic<size_t> matchCount{0};
        std::atomic<bool> sliceBudgetReached{false};

        const unsigned int hwThreads = std::thread::hardware_concurrency();
        // 内容搜索属于后台重 I/O 与解压任务，并发上限收敛至半核（2~6 线程），
        // 永远为前台桌面应用与 OCR 推理预留充足的 CPU、L3 缓存与 NVMe I/O 带宽。
        const size_t numWorkers = static_cast<size_t>(
            std::clamp(hwThreads / 2u, 2u, 6u));
        std::atomic<size_t> nextIndex{startCursor};
        const auto sliceStart = std::chrono::steady_clock::now();

        struct ScopedBackgroundIoPriority {
            bool bgModeEntered = false;
            int prevPriority = THREAD_PRIORITY_NORMAL;
            bool priorityLowered = false;
            ScopedBackgroundIoPriority() noexcept {
                HANDLE hThread = GetCurrentThread();
                if (SetThreadPriority(hThread, THREAD_MODE_BACKGROUND_BEGIN)) {
                    bgModeEntered = true;
                } else {
                    prevPriority = GetThreadPriority(hThread);
                    if (SetThreadPriority(hThread, THREAD_PRIORITY_BELOW_NORMAL)) {
                        priorityLowered = true;
                    }
                }
            }
            ~ScopedBackgroundIoPriority() noexcept {
                HANDLE hThread = GetCurrentThread();
                if (bgModeEntered) {
                    SetThreadPriority(hThread, THREAD_MODE_BACKGROUND_END);
                } else if (priorityLowered && prevPriority != THREAD_PRIORITY_ERROR_RETURN) {
                    SetThreadPriority(hThread, prevPriority);
                }
            }
            ScopedBackgroundIoPriority(const ScopedBackgroundIoPriority&) = delete;
            ScopedBackgroundIoPriority& operator=(const ScopedBackgroundIoPriority&) = delete;
        };

        tools3000::service::QueryThreadPool::instance().parallelFor(
            numWorkers,
            [&, hClientToken, isSameUserProcessSid, reqFilterPtr](size_t workerSlot) {
                (void)workerSlot;
                ScopedBackgroundIoPriority bgIoGuard;
                tools3000::service::pipe_auth::ScopedContentReadImpersonation impGuard(
                    hClientToken, isSameUserProcessSid, effectiveClientSid);
                if (!impGuard.isAuthorized()) {
                    spdlog::error(
                        "Refusing unauthenticated content search worker (sid={}, err={})",
                        WStringToString(effectiveClientSid),
                        impGuard.lastError()
                    );
                    return;
                }

                while (matchCount.load(std::memory_order_relaxed) < maxCount) {
                    if (isCancelled() || sliceBudgetReached.load(std::memory_order_relaxed)) break;

                    if (progressive) {
                        const auto sliceElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - sliceStart).count();
                        const size_t currentHits = matchCount.load(std::memory_order_relaxed);
                        if (requestCursor == 0 && planBaseOffset == 0) {
                            if (currentHits >= tools3000::service::search_limits::ProgressiveFirstSliceMaxHits ||
                                (currentHits >= 1 && sliceElapsedMs >= tools3000::service::search_limits::ProgressiveFirstSliceWithHitsMs) ||
                                sliceElapsedMs >= tools3000::service::search_limits::ProgressiveFirstSliceEmptyBudgetMs) {
                                sliceBudgetReached.store(true, std::memory_order_relaxed);
                                break;
                            }
                        } else {
                            if (currentHits >= ProgressiveFollowupMaxBatchHits ||
                                sliceElapsedMs >= tools3000::service::search_limits::ProgressiveNextSliceBudgetMs) {
                                sliceBudgetReached.store(true, std::memory_order_relaxed);
                                break;
                            }
                        }
                    }

                    const size_t idx = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= localTotal) break;

                    const auto& item = textCandidates[idx];
                    try {
                        const auto fileStart = std::chrono::steady_clock::now();
                        const auto perFileCancel = [&isCancelled, fileStart]() -> bool {
                            if (isCancelled()) return true;
                            const auto fileElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - fileStart).count();
                            return fileElapsedMs >= tools3000::service::search_limits::PerFileContentExtractBudgetMs;
                        };

                        if (tools3000::service::SensitivePathGuard::isSensitiveCredentialPath(item.fullPath)) {
                            continue;
                        }
                        std::vector<tools3000::service::content::ContentSnippet> snippets;
                        if (contentEngine.searchFile(
                                item.fullPath,
                                plan.contentTerms,
                                plan.caseSensitive,
                                snippets,
                                3,
                                perFileCancel,
                                reqFilterPtr)) {
                            uint64_t finalSize = item.fileSize;
                            uint64_t finalCreation = item.creationTime;
                            uint64_t finalWrite = item.lastWriteTime;
                            uint64_t finalAccess = 0;
                            uint32_t finalAttrs = 0;
                            {
                                WIN32_FILE_ATTRIBUTE_DATA attr{};
                                if (GetFileAttributesExW(item.fullPath.c_str(), GetFileExInfoStandard, &attr)) {
                                    finalAttrs = attr.dwFileAttributes;
                                    finalSize = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) |
                                                static_cast<uint64_t>(attr.nFileSizeLow);
                                    finalCreation = (static_cast<uint64_t>(attr.ftCreationTime.dwHighDateTime) << 32) |
                                                    static_cast<uint64_t>(attr.ftCreationTime.dwLowDateTime);
                                    finalWrite = (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) |
                                                 static_cast<uint64_t>(attr.ftLastWriteTime.dwLowDateTime);
                                    finalAccess = (static_cast<uint64_t>(attr.ftLastAccessTime.dwHighDateTime) << 32) |
                                                  static_cast<uint64_t>(attr.ftLastAccessTime.dwLowDateTime);
                                }
                            }

                            nlohmann::json snippetsJson = nlohmann::json::array();
                            for (const auto& snip : snippets) {
                                snippetsJson.push_back({
                                    {"lineNumber", snip.lineNumber},
                                    {"lineContent", WStringToString(snip.lineContent)},
                                    {"matchOffset", snip.matchOffset},
                                    {"matchLength", snip.matchLength}
                                });
                            }

                            const auto histStats = sharedHistorySnap.lookup(item.fullPath);

                            nlohmann::json itemJson = {
                                {"name", WStringToString(item.fileName)},
                                {"path", WStringToString(item.fullPath)},
                                {"isDirectory", false},
                                {"size", finalSize},
                                {"creationTime", finalCreation},
                                {"lastWriteTime", finalWrite},
                                {"lastAccessTime", finalAccess},
                                {"attributes", finalAttrs},
                                {"runCount", histStats.runCount},
                                {"frecencyScore", histStats.frecencyScore},
                                {"snippets", std::move(snippetsJson)}
                            };

                            std::lock_guard<std::mutex> lock(resultsMutex);
                            if (indexedResults.size() < maxCount) {
                                indexedResults.push_back(IndexedContentMatch{idx, std::move(itemJson)});
                                matchCount.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    } catch (const std::exception& e) {
                        spdlog::warn("Exception while searching content in {}: {}", WStringToString(item.fullPath), e.what());
                    } catch (...) {
                        spdlog::warn("Unknown exception while searching content in {}", WStringToString(item.fullPath));
                    }
                }
            });

        std::sort(
            indexedResults.begin(),
            indexedResults.end(),
            [](const IndexedContentMatch& a, const IndexedContentMatch& b) {
                return a.candidateIdx < b.candidateIdx;
            }
        );

        outcome.items.reserve(indexedResults.size());
        for (auto& entry : indexedResults) {
            outcome.items.push_back(std::move(entry.item));
        }

        const size_t localFinalCursor = (std::min)(nextIndex.load(std::memory_order_relaxed), localTotal);
        if (progressive && activePlanId != 0 && localFinalCursor > startCursor) {
            recordPlanScannedRange(activePlanId, textCandidates, startCursor, localFinalCursor);
        }
        outcome.nextCursor = planBaseOffset + localFinalCursor;
        // `HIGH-15`: `hasMore` reflects whether unscanned content candidates remain;
        // never clear `hasMore` merely because a slice reached `maxCount`.
        outcome.hasMore = progressive &&
                          !isCancelled() &&
                          (localFinalCursor < localTotal);
        return outcome;
    };

    auto attachProgressiveMetadata = [&](const ContentBatchOutcome& outcome) {
        if (progressive) {
            responseJson["hasMore"] = outcome.hasMore;
            responseJson["nextCursor"] = outcome.nextCursor;
            responseJson["planId"] = outcome.planId;
            responseJson["totalCandidates"] = outcome.totalCandidates;
        }
    };

    if (isExplicitContent) {
        ContentSearchPlan explicitPlan = SearchExpression::buildContentPlan(wQuery, false, parseOpts);
        if (explicitPlan.contentTerms.empty()) {
            responseJson["results"] = nlohmann::json::array();
            responseJson["totalIndexedFiles"] = totalIndexedFiles;
            responseJson["elapsedMs"] = 0;
            if (progressive) {
                responseJson["hasMore"] = false;
                responseJson["nextCursor"] = 0;
                responseJson["planId"] = 0;
                responseJson["totalCandidates"] = 0;
            }
            return responseJson;
        }
        auto outcome = runContentSearch(explicitPlan, requestedLimit);
        attachProgressiveMetadata(outcome);
        for (auto& item : outcome.items) {
            responseJson["results"].push_back(std::move(item));
        }
    } else if (searchMode == "content") {
        ContentSearchPlan contentPlan = SearchExpression::buildContentPlan(wQuery, true, parseOpts);
        if (!contentPlan.contentTerms.empty()) {
            auto outcome = runContentSearch(contentPlan, requestedLimit);
            attachProgressiveMetadata(outcome);
            for (auto& item : outcome.items) {
                responseJson["results"].push_back(std::move(item));
            }
        } else if (progressive) {
            responseJson["hasMore"] = false;
            responseJson["nextCursor"] = 0;
            responseJson["planId"] = 0;
            responseJson["totalCandidates"] = 0;
        }
    } else if (searchMode == "both") {
        // Hybrid ("both") pagination state machine (`HIGH-15`):
        // Independently tracks filename pagination completion and content scan cursor (`contentHasMore`)
        // so abundant filename hits never starve or prematurely terminate document body scanning.
        ContentSearchPlan bothPlan = SearchExpression::buildContentPlan(wQuery, true, parseOpts);

        auto makeDedupKey = [](const std::wstring& path) {
            std::wstring norm;
            norm.reserve(path.size());
            for (wchar_t ch : path) {
                norm.push_back(ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch)));
            }
            return norm;
        };

        auto appendNameResultJson = [&](const SearchResult& result, std::unordered_set<std::wstring>& seenPaths) {
            if (!seenPaths.insert(makeDedupKey(result.fullPath)).second) {
                return;
            }
            std::string p = WStringToString(result.fullPath);
            const auto histStats = sharedHistorySnap.lookup(result.fullPath);
            responseJson["results"].push_back({
                {"name", WStringToString(result.fileName)},
                {"path", std::move(p)},
                {"isDirectory", result.isDirectory},
                {"size", result.fileSize},
                {"creationTime", result.creationTime},
                {"lastWriteTime", result.lastWriteTime},
                {"lastAccessTime", result.lastAccessTime},
                {"attributes", result.fileAttributes},
                {"matchRank", result.matchRank},
                {"runCount", histStats.runCount},
                {"frecencyScore", histStats.frecencyScore}
            });
        };

        std::unordered_set<std::wstring> seenPaths;

        if (!progressive) {
            auto nameMatches = runNameSearch(wQuery, requestedLimit);
            for (const auto& result : nameMatches) {
                appendNameResultJson(result, seenPaths);
            }
            const size_t remainingBudget = requestedLimit > responseJson["results"].size()
                ? (requestedLimit - responseJson["results"].size())
                : 0;
            if (!bothPlan.contentTerms.empty() && remainingBudget > 0) {
                auto contentOutcome = runContentSearch(bothPlan, remainingBudget);
                for (auto& item : contentOutcome.items) {
                    const std::wstring wp = StringToWString(item["path"].get<std::string>());
                    if (seenPaths.insert(makeDedupKey(wp)).second) {
                        responseJson["results"].push_back(std::move(item));
                        if (responseJson["results"].size() >= requestedLimit) break;
                    }
                }
            }
        } else if (requestCursor == 0 && requestedPlanId == 0) {
            auto allNameMatches = runNameSearch(wQuery, requestedLimit);
            if (bothPlan.contentTerms.empty()) {
                for (const auto& result : allNameMatches) {
                    appendNameResultJson(result, seenPaths);
                }
                ContentBatchOutcome emptyOutcome;
                attachProgressiveMetadata(emptyOutcome);
            } else {
                // Reserve at least 25% of `requestedLimit` (>= 1 slot when `requestedLimit >= 2`)
                // for first-slice content scanning so `nextCursor` advances and `hasMore` stays `true`
                // whenever body candidates remain unscanned (`HIGH-15`).
                size_t firstSliceNameCap = allNameMatches.size();
                if (requestedLimit >= 4 && firstSliceNameCap > (requestedLimit * 3) / 4) {
                    firstSliceNameCap = (requestedLimit * 3) / 4;
                } else if (requestedLimit >= 2 && firstSliceNameCap >= requestedLimit) {
                    firstSliceNameCap = requestedLimit - 1;
                }

                for (size_t i = 0; i < firstSliceNameCap; ++i) {
                    appendNameResultJson(allNameMatches[i], seenPaths);
                }

                const size_t contentBudget = requestedLimit > responseJson["results"].size()
                    ? (requestedLimit - responseJson["results"].size())
                    : 1;
                ContentBatchOutcome contentOutcome = runContentSearch(bothPlan, contentBudget);
                for (auto& item : contentOutcome.items) {
                    const std::wstring wp = StringToWString(item["path"].get<std::string>());
                    if (seenPaths.insert(makeDedupKey(wp)).second) {
                        responseJson["results"].push_back(std::move(item));
                        if (responseJson["results"].size() >= requestedLimit) break;
                    }
                }

                bool nameHasMore = (firstSliceNameCap < allNameMatches.size());
                if (!contentOutcome.hasMore && nameHasMore) {
                    // Content scan finished on the initial slice; immediately backfill deferred name matches.
                    size_t idx = firstSliceNameCap;
                    while (idx < allNameMatches.size() && responseJson["results"].size() < requestedLimit) {
                        appendNameResultJson(allNameMatches[idx], seenPaths);
                        ++idx;
                    }
                    nameHasMore = (idx < allNameMatches.size());
                    firstSliceNameCap = idx;
                }

                if (nameHasMore && contentOutcome.planId != 0) {
                    storeBothModeNameRemainder(
                        contentOutcome.planId,
                        buildContentPlanSignature(),
                        std::move(allNameMatches),
                        firstSliceNameCap);
                }

                const bool combinedHasMore = !isCancelled() && (contentOutcome.hasMore || (nameHasMore && contentOutcome.planId != 0));
                if (combinedHasMore && contentOutcome.nextCursor == 0) {
                    contentOutcome.nextCursor = 1;
                }
                contentOutcome.hasMore = combinedHasMore;
                attachProgressiveMetadata(contentOutcome);
            }
        } else {
            // Progressive continuation slice (`requestCursor > 0` or `requestedPlanId != 0`):
            // advance content scan first, then drain any deferred filename remainder once content completes.
            ContentBatchOutcome contentOutcome;
            if (!bothPlan.contentTerms.empty()) {
                contentOutcome = runContentSearch(bothPlan, requestedLimit);
                for (auto& item : contentOutcome.items) {
                    const std::wstring wp = StringToWString(item["path"].get<std::string>());
                    if (seenPaths.insert(makeDedupKey(wp)).second) {
                        responseJson["results"].push_back(std::move(item));
                        if (responseJson["results"].size() >= requestedLimit) break;
                    }
                }
            }

            bool nameHasMore = false;
            if (!contentOutcome.hasMore && responseJson["results"].size() < requestedLimit) {
                const uint64_t lookupPlanId = contentOutcome.planId != 0 ? contentOutcome.planId : requestedPlanId;
                const size_t remainSlots = requestedLimit - responseJson["results"].size();
                auto deferredNames = consumeBothModeNameRemainder(
                    lookupPlanId,
                    buildContentPlanSignature(),
                    remainSlots,
                    nameHasMore);
                for (const auto& result : deferredNames) {
                    appendNameResultJson(result, seenPaths);
                }
                if (contentOutcome.planId == 0) {
                    contentOutcome.planId = lookupPlanId;
                }
            }

            const bool combinedHasMore = !isCancelled() && (contentOutcome.hasMore || nameHasMore);
            if (combinedHasMore && contentOutcome.nextCursor <= requestCursor) {
                contentOutcome.nextCursor = requestCursor + 1;
            }
            contentOutcome.hasMore = combinedHasMore;
            attachProgressiveMetadata(contentOutcome);
        }
    } else {
        // Name-only fast mode
        auto nameMatches = runNameSearch(wQuery, requestedLimit);
        for (const auto& result : nameMatches) {
            const auto histStats = sharedHistorySnap.lookup(result.fullPath);
            responseJson["results"].push_back({
                {"name", WStringToString(result.fileName)},
                {"path", WStringToString(result.fullPath)},
                {"isDirectory", result.isDirectory},
                {"size", result.fileSize},
                {"creationTime", result.creationTime},
                {"lastWriteTime", result.lastWriteTime},
                {"lastAccessTime", result.lastAccessTime},
                {"attributes", result.fileAttributes},
                {"matchRank", result.matchRank},
                {"runCount", histStats.runCount},
                {"frecencyScore", histStats.frecencyScore}
            });
        }
    }

    if (isExternalClient && responseJson.contains("results") && responseJson["results"].is_array()) {
        nlohmann::json filtered = nlohmann::json::array();
        for (auto& item : responseJson["results"]) {
            const std::string p = item.value("path", "");
            if (!p.empty() && !tools3000::service::external_api::isSensitiveCredentialPath(StringToWString(p))) {
                filtered.push_back(std::move(item));
            }
        }
        responseJson["results"] = std::move(filtered);
    }

    const auto overallEndTime = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(overallEndTime - overallStartTime).count();

    responseJson["totalIndexedFiles"] = totalIndexedFiles;
    responseJson["elapsedMs"] = elapsedMs;
    responseJson["queryId"] = queryId;
    responseJson["cancelled"] = isCancelled();

    return responseJson;
}

namespace {

struct OverlappedIoTransfer {
    HANDLE m_event{nullptr};
    explicit OverlappedIoTransfer() {
        m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~OverlappedIoTransfer() {
        if (m_event) CloseHandle(m_event);
    }
    OverlappedIoTransfer(const OverlappedIoTransfer&) = delete;
    OverlappedIoTransfer& operator=(const OverlappedIoTransfer&) = delete;

    bool waitOne(HANDLE pipe, OVERLAPPED& ov, DWORD timeoutMs, DWORD& transferred) {
        HANDLE handles[2] = { ov.hEvent, g_ServiceStopEvent };
        DWORD count = (g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != nullptr) ? 2 : 1;
        DWORD wait = WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
        }
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return false;
    }

    bool readExact(HANDLE pipe, char* buffer, size_t bytes, DWORD timeoutMs) {
        if (pipe == INVALID_HANDLE_VALUE || !buffer || bytes == 0) return false;
        const ULONGLONG deadline = (timeoutMs == INFINITE) ? (std::numeric_limits<ULONGLONG>::max)() : (GetTickCount64() + timeoutMs);
        size_t done = 0;
        while (done < bytes && g_IsRunning.load(std::memory_order_relaxed)) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            const DWORD remainMs = (timeoutMs == INFINITE) ? INFINITE : static_cast<DWORD>(deadline - now);
            const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes - done, tools3000::service::pipe::IoChunkBytes));

            OVERLAPPED ov{};
            ResetEvent(m_event);
            ov.hEvent = m_event;
            DWORD moved = 0;
            if (!ReadFile(pipe, buffer + done, want, &moved, &ov)) {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING || !waitOne(pipe, ov, remainMs, moved)) {
                    return false;
                }
            }
            if (moved == 0) return false;
            done += moved;
        }
        return done == bytes;
    }

    bool writeExact(HANDLE pipe, const char* data, size_t bytes, DWORD timeoutMs) {
        if (pipe == INVALID_HANDLE_VALUE || !data || bytes == 0) return false;
        const ULONGLONG deadline = (timeoutMs == INFINITE) ? (std::numeric_limits<ULONGLONG>::max)() : (GetTickCount64() + timeoutMs);
        size_t done = 0;
        while (done < bytes && g_IsRunning.load(std::memory_order_relaxed)) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            const DWORD remainMs = (timeoutMs == INFINITE) ? INFINITE : static_cast<DWORD>(deadline - now);
            const DWORD want = static_cast<DWORD>((std::min<size_t>)(bytes - done, tools3000::service::pipe::IoChunkBytes));

            OVERLAPPED ov{};
            ResetEvent(m_event);
            ov.hEvent = m_event;
            DWORD moved = 0;
            if (!WriteFile(pipe, data + done, want, &moved, &ov)) {
                DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING || !waitOne(pipe, ov, remainMs, moved)) {
                    return false;
                }
            }
            if (moved == 0) return false;
            done += moved;
        }
        return done == bytes;
    }
};

}  // namespace

void PipeWorkerThread(
    std::string pipeName,
    std::wstring pipeExpectedSid,
    PSECURITY_DESCRIPTOR pipeDescriptor,
    SECURITY_ATTRIBUTES* pipeSecurity
) {
    namespace frame = tools3000::service::pipe;

    OverlappedIoTransfer transfer;
    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EventGuard {
        HANDLE h;
        ~EventGuard() { if (h) CloseHandle(h); }
    } connectGuard{connectEvent};

    while (g_IsRunning.load()) {
        HANDLE hPipe = CreateNamedPipeA(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            frame::IoChunkBytes, frame::IoChunkBytes, 0,
            pipeDescriptor ? pipeSecurity : nullptr
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        OVERLAPPED connectOv{};
        ResetEvent(connectEvent);
        connectOv.hEvent = connectEvent;

        BOOL connected = ConnectNamedPipe(hPipe, &connectOv);
        DWORD connErr = GetLastError();
        bool isConnected = false;
        if (connected) {
            isConnected = true;
        } else if (connErr == ERROR_PIPE_CONNECTED) {
            isConnected = true;
        } else if (connErr == ERROR_IO_PENDING) {
            DWORD dummy = 0;
            if (transfer.waitOne(hPipe, connectOv, INFINITE, dummy)) {
                isConnected = true;
            }
        }

        if (isConnected && g_IsRunning.load()) {
            bool clientAuthEvaluated = false;
            tools3000::service::pipe_auth::PipeClientAuthSession authSession;

            while (g_IsRunning.load()) {
                char header[frame::HeaderSize] = {};
                if (!transfer.readExact(hPipe, header, sizeof(header), 60000)) break;

                uint32_t requestBytes = 0;
                if (!frame::decodeFrameHeader(header, requestBytes, frame::MaxRequestBytes)) {
                    spdlog::warn("Pipe: invalid or oversized request frame header, disconnecting client");
                    FlushFileBuffers(hPipe);
                    DisconnectNamedPipe(hPipe);
                    break;
                }

                std::string request(requestBytes, '\0');
                if (!transfer.readExact(hPipe, request.data(), requestBytes, 10000)) break;

                // Authenticate/impersonate AFTER initial client data has been read from the pipe
                // so Windows kernel ImpersonateNamedPipeClient never hits ERROR_CANNOT_IMPERSONATE.
                if (!clientAuthEvaluated) {
                    authSession = tools3000::service::pipe_auth::authenticatePipeClientAfterFirstRead(
                        hPipe, pipeExpectedSid);
                    clientAuthEvaluated = true;
                    if (!authSession.authorized) {
                        spdlog::warn(
                            "Pipe: client authentication failed (err={}), disconnecting immediately",
                            authSession.lastError
                        );
                        const std::string errBody =
                            R"({"results":[],"available":false,"error":"unauthorized pipe client"})";
                        const auto errHeader =
                            frame::encodeFrameHeader(static_cast<uint32_t>(errBody.size()));
                        transfer.writeExact(hPipe, errHeader.data(), errHeader.size(), 2000);
                        transfer.writeExact(hPipe, errBody.data(), errBody.size(), 2000);
                        FlushFileBuffers(hPipe);
                        DisconnectNamedPipe(hPipe);
                        break;
                    }

                    // Bind per-user history stores under client impersonation (`CRIT-05`):
                    // resolves `%LOCALAPPDATA%\Tools3000\service_user_data` using client token
                    // so `LocalSystem` never writes into user-writable directories as SYSTEM.
                    tools3000::service::db::RunHistoryManager::instance().bindClientUserContext(
                        authSession.clientToken,
                        authSession.isSameUserProcessSid,
                        authSession.clientSid
                    );
                    tools3000::service::db::SearchHistoryManager::instance().bindClientUserContext(
                        authSession.clientToken,
                        authSession.isSameUserProcessSid,
                        authSession.clientSid
                    );
                }

                nlohmann::json responseJson = ProcessSearchQuery(
                    StringToWString(request),
                    authSession
                );
                const std::string response = responseJson.dump();
                if (!frame::fitsInFrame(response.size())) {
                    spdlog::error("Pipe: response exceeds single-frame limit ({} bytes), disconnecting client", response.size());
                    break;
                }

                const auto responseHeader =
                    frame::encodeFrameHeader(static_cast<uint32_t>(response.size()));
                if (!transfer.writeExact(hPipe, responseHeader.data(), responseHeader.size(), 10000)) break;
                if (!transfer.writeExact(hPipe, response.data(), response.size(), 30000)) break;
            }
        }

        CancelIo(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

void IPCServerThread() {
    spdlog::info("IPC Server Thread started.");

    // Build restricted DACL + Medium Mandatory Integrity Label (`S:(ML;;NW;;;ME)`) before creating pipes.
    PSECURITY_DESCRIPTOR pipeDescriptor = nullptr;
    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    const std::optional<std::wstring> securitySddl = g_UseKernelServicePipeSddl
        ? std::optional<std::wstring>(tools3000::service::pipe_endpoint::makeKernelServiceSecurityDescriptor())
        : tools3000::service::pipe_endpoint::makeSecurityDescriptor(g_SearchClientSid);
    if (!securitySddl || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            securitySddl->c_str(), SDDL_REVISION_1, &pipeDescriptor, nullptr)) {
        const DWORD securityError = GetLastError();
        spdlog::error("Failed to create restricted named pipe security descriptor: {}", securityError);
        g_IsRunning.store(false, std::memory_order_release);
        if (g_ServiceStopEvent != INVALID_HANDLE_VALUE) SetEvent(g_ServiceStopEvent);
        return;
    }
    pipeSecurity.lpSecurityDescriptor = pipeDescriptor;

    PSECURITY_DESCRIPTOR compatPipeDescriptor = nullptr;
    SECURITY_ATTRIBUTES compatPipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    if (!g_CompatTokenPipeName.empty() && !g_CompatTokenClientSid.empty()) {
        const auto compatSddl =
            tools3000::service::pipe_endpoint::makeSecurityDescriptor(g_CompatTokenClientSid);
        if (compatSddl && ConvertStringSecurityDescriptorToSecurityDescriptorW(
                compatSddl->c_str(), SDDL_REVISION_1, &compatPipeDescriptor, nullptr)) {
            compatPipeSecurity.lpSecurityDescriptor = compatPipeDescriptor;
        }
    }

    std::vector<std::thread> pipeWorkers;
    pipeWorkers.reserve(NumPipeWorkers + NumCompatPipeWorkers);
    for (int i = 0; i < NumPipeWorkers; ++i) {
        pipeWorkers.emplace_back(
            PipeWorkerThread,
            g_SearchPipeName,
            g_SearchClientSid,
            pipeDescriptor,
            &pipeSecurity
        );
    }
    if (compatPipeDescriptor != nullptr) {
        for (int i = 0; i < NumCompatPipeWorkers; ++i) {
            pipeWorkers.emplace_back(
                PipeWorkerThread,
                g_CompatTokenPipeName,
                g_CompatTokenClientSid,
                compatPipeDescriptor,
                &compatPipeSecurity
            );
        }
    }
    spdlog::info("Launched {} restricted named pipe worker(s) before index initialization", pipeWorkers.size());

    // Note (`CRIT-05`): RunHistoryManager and SearchHistoryManager are bound per-connection
    // under the authenticated low-privilege client token (`%LOCALAPPDATA%\Tools3000\service_user_data`),
    // never initialized globally under `LocalSystem` in user-writable directories.
    tools3000::service::db::DatabaseManager::instance().init();

    g_AcceptBackgroundJobs.store(true, std::memory_order_release);

    // 扫描所有本地固定驱动器（启动期严格仅索引 DRIVE_FIXED，热插拔与非固定卷走运行期动态接入）
    const DWORD driveMask = GetLogicalDrives();
    g_KnownDriveMask.store(driveMask, std::memory_order_release);
    char isolatedOnlyDrive = '\0';
    if (!g_IsolatedPortableToken.empty()) {
        wchar_t envBuf[8]{};
        if (GetEnvironmentVariableW(L"TOOLS3000_TEST_ONLY_DRIVE", envBuf, 8) > 0 && envBuf[0] != L'\0') {
            const wchar_t ch = envBuf[0];
            if (ch >= L'a' && ch <= L'z') isolatedOnlyDrive = static_cast<char>(ch - (L'a' - L'A'));
            else if (ch >= L'A' && ch <= L'Z') isolatedOnlyDrive = static_cast<char>(ch);
        }
    }
    std::vector<std::shared_ptr<MftParser>> initialParsers;
    for (char drive = 'A'; drive <= 'Z' && g_IsRunning.load(); ++drive) {
        if (isolatedOnlyDrive != '\0' && drive != isolatedOnlyDrive) continue;
        if (!(driveMask & (1u << (drive - 'A')))) continue;
        const std::wstring root{static_cast<wchar_t>(drive), L':', L'\\'};
        UINT driveType = GetDriveTypeW(root.c_str());
        // 后台服务只自动索引本地固定卷。远程盘/可移动盘可能离线、休眠或在目录枚举
        // 中无限期阻塞，不能拖慢登录和服务关停；后续由 WM_DEVICECHANGE 热插拔与按需索引接入。
        if (driveType != DRIVE_FIXED) {
            if (driveType == DRIVE_REMOTE || driveType == DRIVE_REMOVABLE) {
                spdlog::info("Skipping non-fixed drive {}: during automatic indexing", drive);
            }
            continue;
        }

        auto parser = std::make_shared<MftParser>();
        parser->setRealTimeValidation(true);
        if (parser->Initialize(drive)) {
            initialParsers.push_back(std::move(parser));
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(g_ParsersMutex);
        g_MftParsers = initialParsers;
    }

    // 按需唤醒时全速从 Tools3000.db 二进制快照加载，保障用户显式打开搜索窗口后秒级就绪
    std::vector<MftParser*> rawParsers = toRawParserPointers(initialParsers);
    bool snapshotLoaded = tools3000::service::db::DatabaseManager::instance().loadSnapshot(rawParsers);
    std::vector<std::thread> indexThreads;

    auto launchGroupedInitialScans = [&indexThreads](const std::vector<std::shared_ptr<MftParser>>& toScan, const char* tag) {
        const auto diskGroups = groupVolumesByPhysicalDisk(toScan);
        for (const auto& group : diskGroups) {
            g_InitialIndexWorkers.fetch_add(1, std::memory_order_relaxed);
            indexThreads.emplace_back([group, tag]() {
                ScopedBackgroundIoMode bgIo;
                for (const auto& pShared : group) {
                    if (!pShared || !g_IsRunning.load(std::memory_order_acquire)) break;
                    const char drive = pShared->getDriveLetter();
                    try {
                        pShared->EnumerateFiles();
                        if (g_IsRunning.load(std::memory_order_acquire)) pShared->StartListening();
                        spdlog::info("Indexed drive {}: ({})", drive, tag);
                    } catch (const std::exception& e) {
                        spdlog::error("Initial indexing failed on drive {}: {}", drive, e.what());
                    } catch (...) {
                        spdlog::error("Initial indexing failed on drive {} with unknown exception", drive);
                    }
                }
                g_InitialIndexWorkers.fetch_sub(1, std::memory_order_release);
            });
        }
    };

    if (snapshotLoaded) {
        recordPersistedUsns(rawParsers);
        spdlog::info("Successfully loaded database snapshot from Tools3000.db, checking volume readiness...");
        std::vector<std::shared_ptr<MftParser>> missingParsers;
        bool anyCatchUpRescanned = false;
        for (const auto& pShared : initialParsers) {
            if (!pShared) continue;
            if (pShared->consumeRescannedDuringCatchUp()) {
                anyCatchUpRescanned = true;
            }
            if (pShared->getFileCount() == 0 && !pShared->isInitialScanDone()) {
                missingParsers.push_back(pShared);
            } else {
                pShared->StartListening();
            }
        }
        if (!missingParsers.empty()) {
            launchGroupedInitialScans(missingParsers, "volume was unpopulated in snapshot");
        }
        if (!missingParsers.empty() || anyCatchUpRescanned) {
            scheduleSnapshot(initialParsers, anyCatchUpRescanned ? "USN catchup rescan" : "Updated complete");
        } else {
            HeapCompact(GetProcessHeap(), 0);
        }
    } else {
        spdlog::info("No valid database snapshot found, performing full MFT scan and building initial index...");
        launchGroupedInitialScans(initialParsers, "initial scan");
        // 初始全量索引完成后在后台异步保存快照
        scheduleSnapshot(initialParsers, "Initial");
    }
    spdlog::info("Search index launched for {} volume(s)", initialParsers.size());
    markUserActivity();
    g_SearchIndexReady.store(true, std::memory_order_release);

    std::thread hotplugThread(DeviceHotplugWatcherThread);
    std::thread memoryWatchdogThread(MemoryPolicyWatchdogThread);

    for (auto& w : pipeWorkers) {
        if (w.joinable()) w.join();
    }
    g_SearchIndexReady.store(false, std::memory_order_release);
    if (memoryWatchdogThread.joinable()) {
        memoryWatchdogThread.join();
    }
    if (HWND hotplugHwnd = g_HotplugWindow.exchange(nullptr, std::memory_order_acq_rel)) {
        PostMessageW(hotplugHwnd, WM_CLOSE, 0, 0);
    }
    if (hotplugThread.joinable()) {
        hotplugThread.join();
    }
    if (g_PipePokeJob.joinable()) {
        g_PipePokeJob.request_stop();
        g_PipePokeJob.join();
    }
    stopBackgroundJobs();
    for (auto& t : indexThreads) {
        if (t.joinable()) t.join();
    }
    std::vector<std::shared_ptr<MftParser>> remainingParsers;
    {
        std::unique_lock<std::shared_mutex> lock(g_ParsersMutex);
        remainingParsers = std::move(g_MftParsers);
        g_MftParsers.clear();
    }
    for (auto& parser : remainingParsers) {
        if (parser) parser->StopListening();
    }
    tools3000::service::launcher::AppsFolderCatalog::instance().shutdown();
    tools3000::service::QueryThreadPool::instance().shutdown();
    if (compatPipeDescriptor) LocalFree(compatPipeDescriptor);
    if (pipeDescriptor) LocalFree(pipeDescriptor);
    spdlog::info("IPC Server Thread stopped.");
}

// --------------------------------------------------------------------------------------
// Service Control
// --------------------------------------------------------------------------------------
uint32_t countRemainingInteractiveUserSessions(uint32_t excludedSessionId) noexcept {
    PWTS_SESSION_INFOW rawSessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &rawSessions, &sessionCount) || !rawSessions) {
        spdlog::warn("WTSEnumerateSessionsW failed (err={}); keeping search service running conservatively.", GetLastError());
        return 1;
    }
    std::vector<tools3000::service::lifecycle::SessionDescriptor> descriptors;
    descriptors.reserve(sessionCount);
    for (DWORD i = 0; i < sessionCount; ++i) {
        descriptors.push_back({rawSessions[i].SessionId, rawSessions[i].State});
    }
    WTSFreeMemory(rawSessions);
    return tools3000::service::lifecycle::countRemainingInteractiveSessions(descriptors, excludedSessionId);
}

bool beginServiceStopPending(DWORD checkPoint) noexcept {
    std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
    if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING) {
        return false;
    }
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = checkPoint;
    g_ServiceStatus.dwWaitHint = tools3000::service::lifecycle::kGracefulStopWaitHintMs;
    if (g_StatusHandle) {
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }
    return true;
}

DWORD WINAPI ServiceCtrlHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    (void)lpContext;
    if (dwControl == SERVICE_CONTROL_INTERROGATE) {
        return NO_ERROR;
    }
    if (tools3000::service::lifecycle::shouldStopOnServiceControl(dwControl)) {
        spdlog::info("Service stop control received (ctrlCode={}).", dwControl);
        if (beginServiceStopPending(4) && g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != nullptr) {
            SetEvent(g_ServiceStopEvent);
        }
        return NO_ERROR;
    }
    if (dwControl == SERVICE_CONTROL_SESSIONCHANGE) {
        if (dwEventType == WTS_SESSION_LOGOFF) {
            const uint32_t loggedOffSessionId = lpEventData
                ? reinterpret_cast<const WTSSESSION_NOTIFICATION*>(lpEventData)->dwSessionId
                : tools3000::service::lifecycle::kInvalidSessionId;
            const uint32_t remaining = countRemainingInteractiveUserSessions(loggedOffSessionId);
            if (tools3000::service::lifecycle::shouldStopOnSessionChange(dwEventType, remaining)) {
                spdlog::info(
                    "All interactive user sessions logged off (sessionId={}); flushing snapshot and stopping search service so next boot starts clean.",
                    loggedOffSessionId);
                if (beginServiceStopPending(5) && g_ServiceStopEvent != INVALID_HANDLE_VALUE && g_ServiceStopEvent != nullptr) {
                    SetEvent(g_ServiceStopEvent);
                }
            } else {
                spdlog::info(
                    "Session {} logged off, {} interactive session(s) remain active; keeping search service running.",
                    loggedOffSessionId,
                    remaining);
            }
        }
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandlerEx, nullptr);
    if (!g_StatusHandle) return;

    g_ShutdownSnapshotFlushed.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
        ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
        g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwServiceSpecificExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }

    if (!configurePipeEndpoint(argc, argv)) {
        std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_INVALID_PARAMETER;
        g_ServiceStatus.dwCheckPoint = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) {
        std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        g_ServiceStatus.dwCheckPoint = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
        g_ServiceStatus.dwControlsAccepted = tools3000::service::lifecycle::acceptedServiceControls();
        g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 0;
        g_ServiceStatus.dwWaitHint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }

    spdlog::info("Service started successfully.");
    g_IsRunning = true;
    
    // Start IPC Thread
    std::thread ipcThread(IPCServerThread);

    // Wait for stop event
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    // 在销毁内存解析器前落盘最新 USN 增量快照（由一次性原子标志防重复写盘），
    // 确保快速启动关机 / 注销 / 系统重启后下次按需启动搜索服务时秒级恢复、无需重扫全盘。
    flushHistoryAndSaveSnapshotIfNeeded(false);

    RequestServiceShutdown();
    if (ipcThread.joinable()) ipcThread.join();

    CloseHandle(g_ServiceStopEvent);
    g_ServiceStopEvent = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(g_ServiceStatusMutex);
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 3;
        g_ServiceStatus.dwWaitHint = 0;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    }
    spdlog::info("Service stopped successfully.");
}

// 索引持有 USN 日志游标与内存索引，多开会导致重复扫描和不一致。互斥量在进程
// 退出时由内核释放，因此崩溃不会留下永久阻塞；创建失败则必须安全失败，不能放行
// 第二个服务实例。
enum class SingleInstanceLockResult {
    Acquired,
    AlreadyRunning,
    Failed,
};

static SingleInstanceLockResult AcquireSingleInstanceLock() {
    static HANDLE lock = nullptr;

    // Isolated portable harness mode (`--portable-isolated`) uses a per-token mutex so
    // isolated lifecycle verification harnesses can coexist with an installed SCM service.
    bool isolatedPortable = false;
    std::wstring isolatedToken;
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv) {
        for (int i = 1; i < wideArgc; ++i) {
            const std::wstring_view arg = wideArgv[i] ? wideArgv[i] : L"";
            constexpr std::wstring_view tokenPrefix = L"--pipe-token=";
            if (arg == L"--portable-isolated") {
                isolatedPortable = true;
            } else if (arg.starts_with(tokenPrefix)) {
                isolatedToken = std::wstring(arg.substr(tokenPrefix.size()));
            }
        }
        LocalFree(wideArgv);
    }

    if (isolatedPortable && !isolatedToken.empty()) {
        const std::wstring mutexName = L"Local\\Tools3000_Service_Instance_" + isolatedToken;
        lock = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    } else {
        // Primary cross-session singleton lock (`HIGH-10`): `Global\Tools3000_Service_Instance`.
        // Gracefully falls back to `Local\Tools3000_Service_Instance` on `ERROR_ACCESS_DENIED`
        // for unprivileged debug/portable runs.
        lock = CreateMutexW(nullptr, TRUE, L"Global\\Tools3000_Service_Instance");
        if (!lock && GetLastError() == ERROR_ACCESS_DENIED) {
            lock = CreateMutexW(nullptr, TRUE, L"Local\\Tools3000_Service_Instance");
        }
    }

    if (!lock) {
        spdlog::error("Unable to acquire search-service singleton mutex: {}", GetLastError());
        return SingleInstanceLockResult::Failed;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(lock);
        lock = nullptr;
        return SingleInstanceLockResult::AlreadyRunning;
    }
    return SingleInstanceLockResult::Acquired;
}

int main(int argc, char** argv) {
    InitLogger();
    const auto lockResult = AcquireSingleInstanceLock();
    if (lockResult == SingleInstanceLockResult::AlreadyRunning) {
        spdlog::info("Another Tools3000_Service instance is already running, exiting.");
        return 0;
    }
    if (lockResult == SingleInstanceLockResult::Failed) return 1;

    if (argc > 1 && std::string(argv[1]) == "--debug") {
        int wideArgc = 0;
        LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
        const bool configured = wideArgv && configurePipeEndpoint(static_cast<DWORD>(wideArgc), wideArgv);
        if (wideArgv) LocalFree(wideArgv);
        if (!configured) return 1;
        spdlog::info("Running in debug mode (Console).");
        g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        g_IsRunning = true;
        std::thread ipcThread(IPCServerThread);
        std::cout << "Press ENTER to stop..." << std::endl;
        std::cin.get();
        RequestServiceShutdown();
        if (ipcThread.joinable()) ipcThread.join();
        CloseHandle(g_ServiceStopEvent);
        g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        return 0;
    }

    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        {(LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain},
        {NULL, NULL}
    };
    
    if (!StartServiceCtrlDispatcherW(ServiceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // 当非 SCM 服务环境（如便携版、免安装模式或主程序子进程拉起）直接执行时，自动退化为独立后台管道服务进程
            int wideArgc = 0;
            LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
            const bool configured = wideArgv && configurePipeEndpoint(static_cast<DWORD>(wideArgc), wideArgv);
            if (wideArgv) LocalFree(wideArgv);
            if (!configured) return 1;
            spdlog::info("Running in standalone background mode (per-user named pipe server).");
            g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            g_IsRunning = true;
            IPCServerThread();
            CloseHandle(g_ServiceStopEvent);
            g_ServiceStopEvent = INVALID_HANDLE_VALUE;
            return 0;
        }
        spdlog::error("StartServiceCtrlDispatcher failed: {}.", err);
        return 1;
    }
    return 0;
}
