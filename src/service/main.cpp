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
#include <string_view>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cwctype>
#include <optional>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "MftParser.h"
#include "PipeProtocol.h"
#include "PipeEndpoint.h"
#include "PipeClientAuth.h"
#include "SearchCancellation.h"
#include "SearchRequestLimits.h"
#include "content/ContentSearchEngine.h"
#include "db/RunHistoryManager.h"
#include "db/SearchHistoryManager.h"
#include "db/DatabaseManager.h"

#define SERVICE_NAME L"Tools3000_SearchService"

SERVICE_STATUS        g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE                g_ServiceStopEvent = INVALID_HANDLE_VALUE;
std::atomic<bool>     g_IsRunning{false};

std::vector<std::unique_ptr<MftParser>> g_MftParsers;

// 所有会触碰 MftParser 的后台工作都由服务拥有。服务退出时先请求停止并 join，
// 再销毁解析器，禁止任何 detached worker 越过对象生命周期。
std::mutex g_BackgroundJobMutex;
std::jthread g_RebuildJob;
std::jthread g_SnapshotJob;
std::atomic<bool> g_RebuildInProgress{false};
std::atomic<bool> g_SnapshotInProgress{false};
std::atomic<int> g_InitialIndexWorkers{0};
std::atomic<bool> g_AcceptBackgroundJobs{true};
// The per-user pipe is published before expensive volume work. Until this flag
// flips, workers return an explicit initializing response and never touch the
// parser vector while it is being assembled.
std::atomic<bool> g_SearchIndexReady{false};
std::jthread g_PipePokeJob;

std::string g_SearchPipeName;
std::wstring g_SearchClientSid;
constexpr int NumPipeWorkers = 16;

namespace {

struct CachedContentCandidatePlan {
    uint64_t planId = 0;
    std::string signature;
    std::shared_ptr<const std::vector<ContentCandidate>> candidates;
    std::chrono::steady_clock::time_point createdAt{};
};

constexpr size_t MaxCachedContentPlans = 4;
constexpr auto ContentPlanCacheTtl = std::chrono::seconds(90);
constexpr size_t ProgressiveFollowupMaxBatchHits = 60;

std::mutex g_ContentPlanCacheMutex;
std::vector<CachedContentCandidatePlan> g_ContentPlanCache;
std::atomic<uint64_t> g_NextContentPlanId{1};

void invalidateContentPlanCache() {
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    g_ContentPlanCache.clear();
}

std::shared_ptr<const std::vector<ContentCandidate>> lookupCachedContentPlan(
    uint64_t planId,
    std::string_view signature
) {
    if (planId == 0) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_ContentPlanCacheMutex);
    for (auto it = g_ContentPlanCache.begin(); it != g_ContentPlanCache.end(); ++it) {
        if (it->planId == planId && it->signature == signature) {
            if (now - it->createdAt > ContentPlanCacheTtl) {
                g_ContentPlanCache.erase(it);
                return nullptr;
            }
            auto hit = *it;
            hit.createdAt = now;
            g_ContentPlanCache.erase(it);
            g_ContentPlanCache.push_back(hit);
            return hit.candidates;
        }
    }
    return nullptr;
}

uint64_t storeCachedContentPlan(
    std::string signature,
    std::shared_ptr<const std::vector<ContentCandidate>> candidates
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
        std::move(signature),
        std::move(candidates),
        now
    });
    return id;
}

}  // namespace

std::string WStringToString(const std::wstring& wstr);

bool configurePipeEndpoint(DWORD argc, wchar_t** argv) {
    std::string token;
    std::wstring sid;
    for (DWORD index = 0; index < argc; ++index) {
        const std::wstring_view arg = argv[index] ? argv[index] : L"";
        constexpr std::wstring_view tokenPrefix = L"--pipe-token=";
        constexpr std::wstring_view sidPrefix = L"--client-sid=";
        if (arg.starts_with(tokenPrefix)) {
            token = WStringToString(std::wstring(arg.substr(tokenPrefix.size())));
        } else if (arg.starts_with(sidPrefix)) {
            sid = std::wstring(arg.substr(sidPrefix.size()));
        }
    }
    PSID parsedSid = nullptr;
    const bool sidValid = !sid.empty() && ConvertStringSidToSidW(sid.c_str(), &parsedSid);
    if (parsedSid) LocalFree(parsedSid);
    const auto pipe = tools3000::service::pipe_endpoint::makePipeName(token);
    if (!sidValid || !pipe) {
        spdlog::error("Search service missing or invalid per-user IPC endpoint arguments");
        return false;
    }
    g_SearchPipeName = *pipe;
    g_SearchClientSid = std::move(sid);
    return true;
}

void stopBackgroundJobs() {
    g_AcceptBackgroundJobs.store(false, std::memory_order_release);
    invalidateContentPlanCache();
    for (auto& parser : g_MftParsers) parser->requestStop();

    std::jthread rebuild;
    std::jthread snapshot;
    {
        std::lock_guard lock(g_BackgroundJobMutex);
        if (g_RebuildJob.joinable()) g_RebuildJob.request_stop();
        if (g_SnapshotJob.joinable()) g_SnapshotJob.request_stop();
        rebuild = std::move(g_RebuildJob);
        snapshot = std::move(g_SnapshotJob);
    }
    // jthread 的析构在当前作用域末尾完成 join。此时不持有任务锁，避免工作线程
    // 在收尾路径更新状态时形成锁反转。
}

void scheduleSnapshot(std::vector<MftParser*> parsers, std::string_view reason) {
    if (!g_AcceptBackgroundJobs.load(std::memory_order_acquire) || parsers.empty()) return;
    std::lock_guard lock(g_BackgroundJobMutex);
    if (g_SnapshotInProgress.exchange(true, std::memory_order_acq_rel)) return;
    if (g_SnapshotJob.joinable()) g_SnapshotJob.join();

    g_SnapshotJob = std::jthread(
        [parsers = std::move(parsers), reason = std::string(reason)](std::stop_token stop) {
            // 等待初始索引真正完成，而不是猜测固定 5 秒；慢盘和网络盘不会再保存半份快照。
            while (!stop.stop_requested() && g_InitialIndexWorkers.load(std::memory_order_acquire) > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            try {
                if (!stop.stop_requested()) {
                    const bool ok = tools3000::service::db::DatabaseManager::instance().saveSnapshot(parsers);
                    spdlog::info("{} database snapshot {}", reason, ok ? "saved" : "failed");
                }
            } catch (const std::exception& e) {
                spdlog::error("{} database snapshot failed: {}", reason, e.what());
            } catch (...) {
                spdlog::error("{} database snapshot failed with unknown exception", reason);
            }
            g_SnapshotInProgress.store(false, std::memory_order_release);
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
    std::vector<MftParser*> parsers;
    parsers.reserve(g_MftParsers.size());
    for (auto& parser : g_MftParsers) {
        parser->resetStopRequest();
        parsers.push_back(parser.get());
    }
    g_RebuildJob = std::jthread([parsers = std::move(parsers)](std::stop_token stop) {
        std::vector<std::thread> workers;
        workers.reserve(parsers.size());
        for (auto* parser : parsers) {
            parser->StopListening();
            workers.emplace_back([parser, stop]() {
                if (stop.stop_requested()) return;
                try {
                    parser->EnumerateFiles();
                    if (!stop.stop_requested()) parser->StartListening();
                } catch (const std::exception& e) {
                    spdlog::error("Rebuild failed on drive {}: {}", parser->getDriveLetter(), e.what());
                } catch (...) {
                    spdlog::error("Rebuild failed on drive {} with unknown exception", parser->getDriveLetter());
                }
            });
        }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        invalidateContentPlanCache();
        try {
            if (!stop.stop_requested()) {
                tools3000::service::db::DatabaseManager::instance().saveSnapshot(parsers);
            }
        } catch (const std::exception& e) {
            spdlog::error("Rebuild snapshot failed: {}", e.what());
        } catch (...) {
            spdlog::error("Rebuild snapshot failed with unknown exception");
        }
        g_RebuildInProgress.store(false, std::memory_order_release);
    });
    return true;
}

// 主程序退出时会通过管道请求停机。工作线程此刻大多阻塞在 ConnectNamedPipe 上，
// 单靠清掉运行标志叫不醒它们，还得真的连上来几次。这件事必须交给另一个线程，
// 否则正在处理停机请求的那个线程会连到自己身上。
void RequestServiceShutdown() {
    if (!g_IsRunning.exchange(false)) return;
    if (g_ServiceStopEvent != INVALID_HANDLE_VALUE) SetEvent(g_ServiceStopEvent);

    g_PipePokeJob = std::jthread([](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            HANDLE poke = CreateFileA(g_SearchPipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (poke == INVALID_HANDLE_VALUE) {
                // 管道实例全部消失，说明工作线程已经退干净了。
                if (GetLastError() == ERROR_FILE_NOT_FOUND) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            CloseHandle(poke);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
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
#ifdef SPDLOG_WCHAR_FILENAMES
        const auto logPath = (logDir / L"Tools3000_Service.log").wstring();
#else
        const std::wstring logDirW = logDir.wstring();
        wchar_t shortDir[MAX_PATH] = {};
        const DWORD shortLen = GetShortPathNameW(logDirW.c_str(), shortDir, MAX_PATH);
        const std::filesystem::path resolvedDir =
            (shortLen > 0 && shortLen < MAX_PATH) ? std::filesystem::path(shortDir) : logDir;
        const auto logPath = (resolvedDir / "Tools3000_Service.log").string();
#endif
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logPath, 5 * 1024 * 1024, 3);
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
    HANDLE hClientToken = nullptr,
    bool isSameUserProcessSid = false
) {
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

    std::string utf8Input = WStringToString(rawInput);
    if (!g_SearchIndexReady.load(std::memory_order_acquire) ||
        g_InitialIndexWorkers.load(std::memory_order_acquire) > 0) {
        bool isAllowedDuringInit = false;
        if (!utf8Input.empty() && utf8Input.front() == '{') {
            try {
                const auto request = nlohmann::json::parse(utf8Input);
                const std::string action = request.value("action", "");
                isAllowedDuringInit =
                    (action == "shutdown" || action == "cancel" || action == "getDbStats");
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
            if (reqJson.contains("action") && reqJson["action"].is_string()) {
                std::string act = reqJson["action"].get<std::string>();
                if (act == "cancel") {
                    tools3000::service::query::sharedEpochTracker().cancelAll();
                    return {{"success", true}, {"cancelled", true}};
                }
                if (act == "rebuild" || act == "reindex") {
                    const bool started = scheduleRebuild();
                    return {{"success", started}, {"rebuilding", started},
                            {"alreadyRunning", !started && g_RebuildInProgress.load()}};
                }
                if (act == "catchup" || act == "sync") {
                    bool anyUpdated = false;
                    for (auto& parser : g_MftParsers) {
                        uint64_t lastUsn = parser->getCurrentUsn();
                        if (parser->catchUpUsnJournal(lastUsn)) {
                            anyUpdated = true;
                        }
                    }
                    if (anyUpdated) {
                        invalidateContentPlanCache();
                    }
                    return {{"success", true}, {"synced", true}, {"updated", anyUpdated}};
                }
                if (act == "recordRun") {
                    if (reqJson.contains("path") && reqJson["path"].is_string()) {
                        const auto& pathUtf8 = reqJson["path"].get_ref<const std::string&>();
                        if (pathUtf8.size() > tools3000::service::search_limits::MaxPathUtf8Bytes) {
                            return {{"success", false}, {"error", "path exceeds limit"}};
                        }
                        std::wstring path = StringToWString(pathUtf8);
                        tools3000::service::db::RunHistoryManager::instance().recordRun(path);
                        return {{"success", true}};
                    }
                    return {{"success", false}};
                }
                if (act == "recordSearch") {
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
                    if (searchIndexReady) {
                        for (const auto& p : g_MftParsers) {
                            if (p) totalMemRecords += p->getFileCount();
                        }
                    }
                    const uint64_t effectiveRecords = totalMemRecords > 0 ? totalMemRecords : stats.totalRecords;
                    const int initialWorkers = g_InitialIndexWorkers.load(std::memory_order_acquire);
                    const bool rebuildInProgress = g_RebuildInProgress.load(std::memory_order_acquire);
                    const bool snapshotInProgress = g_SnapshotInProgress.load(std::memory_order_acquire);

                    // 只要内存中已解析出文件记录 (totalMemRecords > 0) 且初始扫描线程完成，系统就 100% 具备全速搜索能力。
                    // 快照落盘 (snapshotInProgress) 属于后台纯异步任务，绝不阻塞前端即时搜索！
                    const bool isIndexing = (initialWorkers > 0 || rebuildInProgress || !searchIndexReady) && (totalMemRecords == 0);
                    return {
                        {"success", true},
                        {"dbPath", WStringToString(stats.dbPath)},
                        {"dbSize", stats.fileSize},
                        {"timestamp", stats.timestamp},
                        {"totalRecords", effectiveRecords},
                        {"volumeCount", stats.volumeCount},
                        {"exists", stats.exists || totalMemRecords > 0},
                        {"indexing", isIndexing},
                        {"initialWorkers", initialWorkers},
                        {"rebuildInProgress", rebuildInProgress},
                        {"snapshotInProgress", snapshotInProgress},
                        {"runHistoryCount", tools3000::service::db::RunHistoryManager::instance().size()},
                        {"searchHistoryCount", tools3000::service::db::SearchHistoryManager::instance().size()}
                    };
                }
                if (act == "saveSnapshot") {
                    std::vector<MftParser*> allParsers;
                    for (auto& p : g_MftParsers) allParsers.push_back(p.get());
                    bool ok = tools3000::service::db::DatabaseManager::instance().saveSnapshot(allParsers);
                    return {{"success", ok}};
                }
                if (act == "shutdown") {
                    // 快照不在这里落盘：写一份要几百 MB，而下次启动导入快照时本就会用
                    // USN 日志补齐停机期间的变动，多花的磁盘代价换不来任何准确性。
                    spdlog::info("Received shutdown request from client, stopping service.");
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
                wQuery = StringToWString(queryUtf8);
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
                    if (!s.empty()) excludeOpts.patterns.push_back(StringToWString(s));
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
                        std::wstring wp = StringToWString(item["path"].get<std::string>());
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
        wQuery = rawInput;
    }
    const auto overallStartTime = std::chrono::steady_clock::now();

    auto& epochTracker = tools3000::service::query::sharedEpochTracker();
    epochTracker.observe(queryId);
    const auto isCancelled = [&epochTracker, queryId]() { return epochTracker.isStale(queryId); };

    auto isDriveEnabled = [&](char driveLetter) {
        if (enabledDrives.empty()) return true;
        for (char d : enabledDrives) {
            if (std::toupper(d) == std::toupper(driveLetter)) return true;
        }
        return false;
    };

    size_t totalIndexedFiles = 0;
    for (const auto& parser : g_MftParsers) {
        if (isDriveEnabled(parser->getDriveLetter())) {
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
        sig.append(WStringToString(g_SearchClientSid));
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

    auto runNameSearch = [&](const std::wstring& queryStr, size_t maxCount) -> std::vector<SearchResult> {
        std::vector<std::future<std::vector<SearchResult>>> futures;
        futures.reserve(g_MftParsers.size());
        for (auto& parser : g_MftParsers) {
            if (!isDriveEnabled(parser->getDriveLetter())) continue;
            futures.push_back(std::async(std::launch::async, [&parser, &queryStr, &excludeOpts, &isCancelled, &parseOpts, maxCount]() {
                return parser->Search(queryStr, static_cast<int>(maxCount), excludeOpts, isCancelled, parseOpts);
            }));
        }

        std::vector<SearchResult> results;
        for (auto& f : futures) {
            auto volumeResults = f.get();
            results.insert(results.end(),
                           std::make_move_iterator(volumeResults.begin()),
                           std::make_move_iterator(volumeResults.end()));
        }
        const auto classifyPathTier = [](const SearchResult& r) -> int {
            std::wstring lowerPath;
            lowerPath.reserve(r.fullPath.size());
            for (wchar_t c : r.fullPath) lowerPath.push_back(c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c)));
            const size_t lastSlash = lowerPath.rfind(L'\\');
            const std::wstring_view dirPart = (lastSlash != std::wstring::npos)
                ? std::wstring_view(lowerPath).substr(0, lastSlash + 1)
                : std::wstring_view{};
            const std::wstring_view namePart = (lastSlash != std::wstring::npos)
                ? std::wstring_view(lowerPath).substr(lastSlash + 1)
                : std::wstring_view(lowerPath);
            if (!r.isDirectory) {
                if (namePart.rfind(L"~$", 0) == 0 ||
                    (namePart.size() > 4 && (namePart.substr(namePart.size() - 4) == L".tmp" ||
                                             namePart.substr(namePart.size() - 4) == L".bak")) ||
                    (namePart.size() > 5 && (namePart.substr(namePart.size() - 5) == L".temp" ||
                                             namePart.substr(namePart.size() - 5) == L".part")) ||
                    (namePart.size() > 11 && namePart.substr(namePart.size() - 11) == L".crdownload")) {
                    return 4;
                }
            }
            if (dirPart.find(L"\\temp\\") != std::wstring_view::npos ||
                dirPart.find(L"\\tmp\\") != std::wstring_view::npos ||
                dirPart.find(L"\\cache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\caches\\") != std::wstring_view::npos ||
                dirPart.find(L"\\inetcache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\webcache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\gpucache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\code cache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\shadercache\\") != std::wstring_view::npos ||
                dirPart.find(L"\\recent\\") != std::wstring_view::npos ||
                dirPart.find(L"\\prefetch\\") != std::wstring_view::npos ||
                dirPart.find(L"\\$recycle.bin\\") != std::wstring_view::npos ||
                dirPart.find(L"\\node_modules\\") != std::wstring_view::npos ||
                dirPart.find(L"\\.git\\") != std::wstring_view::npos) {
                return 4;
            }
            if (!r.isDirectory &&
                ((namePart.size() > 4 && (namePart.substr(namePart.size() - 4) == L".lnk" ||
                                          namePart.substr(namePart.size() - 4) == L".url")) ||
                 (namePart.size() > 9 && namePart.substr(namePart.size() - 9) == L".desklink"))) {
                return 3;
            }
            if (dirPart.find(L"\\xwechat_files\\") != std::wstring_view::npos ||
                dirPart.find(L"\\xwechat\\") != std::wstring_view::npos ||
                dirPart.find(L"\\wechat files\\") != std::wstring_view::npos ||
                dirPart.find(L"\\tencent files\\") != std::wstring_view::npos ||
                dirPart.find(L"\\nt_qq\\") != std::wstring_view::npos ||
                dirPart.find(L"\\wxwork\\") != std::wstring_view::npos ||
                dirPart.find(L"\\wecom\\") != std::wstring_view::npos ||
                dirPart.find(L"\\dingtalk\\") != std::wstring_view::npos ||
                dirPart.find(L"\\feishu\\") != std::wstring_view::npos ||
                dirPart.find(L"\\lark\\") != std::wstring_view::npos ||
                dirPart.find(L"\\telegram desktop\\") != std::wstring_view::npos ||
                dirPart.find(L"\\filestorage\\") != std::wstring_view::npos ||
                dirPart.find(L"\\msg\\file\\") != std::wstring_view::npos ||
                dirPart.find(L"\\file_recv\\") != std::wstring_view::npos ||
                dirPart.find(L"\\filerecv\\") != std::wstring_view::npos ||
                dirPart.find(L"\\msgattach\\") != std::wstring_view::npos) {
                return 1;
            }
            if (dirPart.find(L"\\appdata\\") != std::wstring_view::npos ||
                dirPart.find(L"\\programdata\\") != std::wstring_view::npos ||
                dirPart.find(L"\\program files\\") != std::wstring_view::npos ||
                dirPart.find(L"\\program files (x86)\\") != std::wstring_view::npos ||
                dirPart.find(L"\\windows\\") != std::wstring_view::npos) {
                return 2;
            }
            return 0;
        };
        const auto historySnap = tools3000::service::db::RunHistoryManager::instance().captureSnapshot();
        struct TieredSearchResult {
            SearchResult result;
            uint8_t tier;
            double frecencyScore = 0.0;
        };
        std::vector<TieredSearchResult> tieredResults;
        tieredResults.reserve(results.size());
        for (auto& item : results) {
            const uint8_t tier = static_cast<uint8_t>(classifyPathTier(item));
            const double frecency = historySnap.empty() ? 0.0 : historySnap.lookup(item.fullPath).frecencyScore;
            tieredResults.push_back({std::move(item), tier, frecency});
        }
        std::stable_sort(tieredResults.begin(), tieredResults.end(), [](const auto& a, const auto& b) {
            if (a.tier != b.tier) return a.tier < b.tier;
            if (a.result.matchRank != b.result.matchRank) return a.result.matchRank < b.result.matchRank;
            if (a.frecencyScore != b.frecencyScore) return a.frecencyScore > b.frecencyScore;
            if (a.result.folderPriority != b.result.folderPriority) return a.result.folderPriority > b.result.folderPriority;
            if (a.result.lastWriteTime != b.result.lastWriteTime) return a.result.lastWriteTime > b.result.lastWriteTime;
            if (a.result.fileName.size() != b.result.fileName.size()) return a.result.fileName.size() < b.result.fileName.size();
            return _wcsicmp(a.result.fileName.c_str(), b.result.fileName.c_str()) < 0;
        });
        const size_t finalCount = (std::min)(tieredResults.size(), maxCount);
        results.clear();
        results.reserve(finalCount);
        for (size_t i = 0; i < finalCount; ++i) {
            results.push_back(std::move(tieredResults[i].result));
        }
        return results;
    };

    const auto sharedHistorySnap = tools3000::service::db::RunHistoryManager::instance().captureSnapshot();

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

        if (progressive && requestCursor > 0 && requestedPlanId != 0) {
            candidatesPtr = lookupCachedContentPlan(requestedPlanId, planSignature);
            if (candidatesPtr) {
                activePlanId = requestedPlanId;
            }
        }

        if (!candidatesPtr) {
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
            for (auto& parser : g_MftParsers) {
                if (!isDriveEnabled(parser->getDriveLetter())) continue;
                if (isCancelled()) return outcome;
                auto volumeCandidates = parser->CollectContentCandidates(
                    plan.fileFilterExpr, candidateOpts, isCancelled);
                freshCandidates->insert(
                    freshCandidates->end(),
                    std::make_move_iterator(volumeCandidates.begin()),
                    std::make_move_iterator(volumeCandidates.end())
                );
            }

            if (freshCandidates->empty() || isCancelled()) {
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
                activePlanId = storeCachedContentPlan(planSignature, candidatesPtr);
            }
        }

        const auto& textCandidates = *candidatesPtr;
        const size_t totalCandidates = textCandidates.size();
        outcome.planId = activePlanId;
        outcome.totalCandidates = totalCandidates;

        const size_t startCursor = progressive ? (std::min)(requestCursor, totalCandidates) : 0;
        if (startCursor >= totalCandidates || isCancelled()) {
            outcome.nextCursor = totalCandidates;
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

        const unsigned int numThreads = (std::max)(2u, (std::min)(16u, std::thread::hardware_concurrency()));
        std::vector<std::thread> workers;
        workers.reserve(numThreads);
        std::atomic<size_t> nextIndex{startCursor};
        const auto sliceStart = std::chrono::steady_clock::now();

        for (unsigned int t = 0; t < numThreads; ++t) {
            workers.emplace_back([&, hClientToken, isSameUserProcessSid, reqFilterPtr]() {
                tools3000::service::pipe_auth::ScopedContentReadImpersonation impGuard(
                    hClientToken, isSameUserProcessSid, g_SearchClientSid);
                if (!impGuard.isAuthorized()) {
                    spdlog::error(
                        "Refusing unauthenticated content search worker (sid={}, err={})",
                        WStringToString(g_SearchClientSid),
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
                        if (startCursor == 0) {
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
                    if (idx >= totalCandidates) break;

                    const auto& item = textCandidates[idx];
                    try {
                        const auto fileStart = std::chrono::steady_clock::now();
                        const auto perFileCancel = [&isCancelled, fileStart]() -> bool {
                            if (isCancelled()) return true;
                            const auto fileElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - fileStart).count();
                            return fileElapsedMs >= tools3000::service::search_limits::PerFileContentExtractBudgetMs;
                        };

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
                            if (finalSize == 0 || finalWrite == 0) {
                                WIN32_FILE_ATTRIBUTE_DATA attr{};
                                if (GetFileAttributesExW(item.fullPath.c_str(), GetFileExInfoStandard, &attr)) {
                                    finalSize = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) |
                                                static_cast<uint64_t>(attr.nFileSizeLow);
                                    finalCreation = (static_cast<uint64_t>(attr.ftCreationTime.dwHighDateTime) << 32) |
                                                    static_cast<uint64_t>(attr.ftCreationTime.dwLowDateTime);
                                    finalWrite = (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) |
                                                 static_cast<uint64_t>(attr.ftLastWriteTime.dwLowDateTime);
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
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

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

        const size_t finalCursor = (std::min)(nextIndex.load(std::memory_order_relaxed), totalCandidates);
        outcome.nextCursor = finalCursor;
        outcome.hasMore = progressive &&
                          !isCancelled() &&
                          (outcome.items.size() < maxCount) &&
                          (finalCursor < totalCandidates);
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
        // Dual mode: 首拍并发跑文件名检索 + 内容首拍，后续续扫拍仅增量拉取内容检索结果
        std::vector<SearchResult> nameMatches;
        if (!progressive || requestCursor == 0) {
            nameMatches = runNameSearch(wQuery, requestedLimit);
        }
        ContentSearchPlan bothPlan = SearchExpression::buildContentPlan(wQuery, true, parseOpts);
        ContentBatchOutcome contentOutcome;
        const size_t remainingBudget = requestedLimit > nameMatches.size()
            ? (requestedLimit - nameMatches.size())
            : 0;
        if (!bothPlan.contentTerms.empty() && remainingBudget > 0) {
            contentOutcome = runContentSearch(bothPlan, remainingBudget);
        }
        attachProgressiveMetadata(contentOutcome);

        auto makeDedupKey = [](const std::wstring& path) {
            std::wstring norm;
            norm.reserve(path.size());
            for (wchar_t ch : path) {
                norm.push_back(ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch)));
            }
            return norm;
        };

        std::unordered_set<std::wstring> seenPaths;
        for (const auto& result : nameMatches) {
            seenPaths.insert(makeDedupKey(result.fullPath));
            std::string p = WStringToString(result.fullPath);
            const auto histStats = sharedHistorySnap.lookup(result.fullPath);
            responseJson["results"].push_back({
                {"name", WStringToString(result.fileName)},
                {"path", std::move(p)},
                {"isDirectory", result.isDirectory},
                {"size", result.fileSize},
                {"creationTime", result.creationTime},
                {"lastWriteTime", result.lastWriteTime},
                {"matchRank", result.matchRank},
                {"runCount", histStats.runCount},
                {"frecencyScore", histStats.frecencyScore}
            });
        }

        for (auto& item : contentOutcome.items) {
            const std::string p = item["path"].get<std::string>();
            const std::wstring wp = StringToWString(p);
            if (seenPaths.insert(makeDedupKey(wp)).second) {
                responseJson["results"].push_back(std::move(item));
            }
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
                {"matchRank", result.matchRank},
                {"runCount", histStats.runCount},
                {"frecencyScore", histStats.frecencyScore}
            });
        }
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

void PipeWorkerThread(PSECURITY_DESCRIPTOR pipeDescriptor, SECURITY_ATTRIBUTES* pipeSecurity) {
    namespace frame = tools3000::service::pipe;

    OverlappedIoTransfer transfer;
    HANDLE connectEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EventGuard {
        HANDLE h;
        ~EventGuard() { if (h) CloseHandle(h); }
    } connectGuard{connectEvent};

    while (g_IsRunning.load()) {
        HANDLE hPipe = CreateNamedPipeA(
            g_SearchPipeName.c_str(),
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
                    spdlog::warn("Pipe: invalid request frame header, disconnecting client");
                    break;
                }

                std::string request(requestBytes, '\0');
                if (!transfer.readExact(hPipe, request.data(), requestBytes, 10000)) break;

                // Authenticate/impersonate AFTER initial client data has been read from the pipe
                // so Windows kernel ImpersonateNamedPipeClient never hits ERROR_CANNOT_IMPERSONATE.
                if (!clientAuthEvaluated) {
                    authSession = tools3000::service::pipe_auth::authenticatePipeClientAfterFirstRead(
                        hPipe, g_SearchClientSid);
                    clientAuthEvaluated = true;
                    if (!authSession.authorized && !g_SearchClientSid.empty()) {
                        spdlog::warn(
                            "Pipe: client auth did not produce impersonation token (err={}), content reads restricted",
                            authSession.lastError
                        );
                    }
                }

                nlohmann::json responseJson = ProcessSearchQuery(
                    StringToWString(request),
                    authSession.clientToken,
                    authSession.isSameUserProcessSid
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

    // Build the DACL before creating any pipe instance. Publishing the endpoint
    // early avoids SCM/portable duplicate starts, but it must remain private to
    // the requesting SID even if later disk initialization is slow.
    PSECURITY_DESCRIPTOR pipeDescriptor = nullptr;
    SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    const auto securitySddl = tools3000::service::pipe_endpoint::makeSecurityDescriptor(g_SearchClientSid);
    if (!securitySddl || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
            securitySddl->c_str(), SDDL_REVISION_1, &pipeDescriptor, nullptr)) {
        const DWORD securityError = GetLastError();
        spdlog::error("Failed to create restricted named pipe security descriptor: {}", securityError);
        g_IsRunning.store(false, std::memory_order_release);
        if (g_ServiceStopEvent != INVALID_HANDLE_VALUE) SetEvent(g_ServiceStopEvent);
        return;
    }
    pipeSecurity.lpSecurityDescriptor = pipeDescriptor;

    std::vector<std::thread> pipeWorkers;
    pipeWorkers.reserve(NumPipeWorkers);
    for (int i = 0; i < NumPipeWorkers; ++i) {
        pipeWorkers.emplace_back(PipeWorkerThread, pipeDescriptor, &pipeSecurity);
    }
    spdlog::info("Launched {} restricted named pipe worker(s) before index initialization", NumPipeWorkers);

    // 初始化历史与快照数据库管理器 (按客户端 SID 隔离, A11)
    if (!g_SearchClientSid.empty()) {
        tools3000::service::db::RunHistoryManager::instance().initForSid(g_SearchClientSid);
        tools3000::service::db::SearchHistoryManager::instance().initForSid(g_SearchClientSid);
    } else {
        tools3000::service::db::RunHistoryManager::instance().init();
        tools3000::service::db::SearchHistoryManager::instance().init();
    }
    tools3000::service::db::DatabaseManager::instance().init();

    // 扫描所有本地驱动器
    const DWORD driveMask = GetLogicalDrives();
    for (char drive = 'A'; drive <= 'Z' && g_IsRunning.load(); ++drive) {
        if (!(driveMask & (1u << (drive - 'A')))) continue;
        const std::wstring root{static_cast<wchar_t>(drive), L':', L'\\'};
        UINT driveType = GetDriveTypeW(root.c_str());
        // 后台服务只自动索引本地固定卷。远程盘/可移动盘可能离线、休眠或在目录枚举
        // 中无限期阻塞，不能拖慢登录和服务关停；后续应由显式的按需索引入口接入。
        if (driveType != DRIVE_FIXED) {
            if (driveType == DRIVE_REMOTE || driveType == DRIVE_REMOVABLE) {
                spdlog::info("Skipping non-fixed drive {}: during automatic indexing", drive);
            }
            continue;
        }

        auto parser = std::make_unique<MftParser>();
        parser->setRealTimeValidation(true);
        if (parser->Initialize(drive)) {
            g_MftParsers.push_back(std::move(parser));
        }
    }

    // 尝试从 Tools3000.db 二进制快照秒级冷启动 (< 30ms)
    std::vector<MftParser*> rawParsers;
    for (auto& p : g_MftParsers) rawParsers.push_back(p.get());

    bool snapshotLoaded = tools3000::service::db::DatabaseManager::instance().loadSnapshot(rawParsers);
    std::vector<std::thread> indexThreads;

    if (snapshotLoaded) {
        spdlog::info("Successfully loaded database snapshot from Tools3000.db, checking volume readiness...");
        bool anyMissing = false;
        bool anyCatchUpRescanned = false;
        for (auto* pRaw : rawParsers) {
            if (pRaw->consumeRescannedDuringCatchUp()) {
                anyCatchUpRescanned = true;
            }
            if (pRaw->getFileCount() == 0) {
                anyMissing = true;
                char drive = pRaw->getDriveLetter();
                g_InitialIndexWorkers.fetch_add(1, std::memory_order_relaxed);
                indexThreads.emplace_back([pRaw, drive]() {
                    try {
                        pRaw->EnumerateFiles();
                        if (g_IsRunning.load(std::memory_order_acquire)) pRaw->StartListening();
                        spdlog::info("Indexed drive {}: (volume was unpopulated in snapshot)", drive);
                    } catch (const std::exception& e) {
                        spdlog::error("Initial indexing failed on drive {}: {}", drive, e.what());
                    } catch (...) {
                        spdlog::error("Initial indexing failed on drive {} with unknown exception", drive);
                    }
                    g_InitialIndexWorkers.fetch_sub(1, std::memory_order_release);
                });
            } else {
                pRaw->StartListening();
            }
        }
        if (anyMissing || anyCatchUpRescanned) {
            scheduleSnapshot(rawParsers, anyCatchUpRescanned ? "USN catchup rescan" : "Updated complete");
        }
    } else {
        spdlog::info("No valid database snapshot found, performing full MFT scan and building initial index...");
        for (auto* pRaw : rawParsers) {
            char drive = pRaw->getDriveLetter();
            g_InitialIndexWorkers.fetch_add(1, std::memory_order_relaxed);
            indexThreads.emplace_back([pRaw, drive]() {
                try {
                    pRaw->EnumerateFiles();
                    if (g_IsRunning.load(std::memory_order_acquire)) pRaw->StartListening();
                    spdlog::info("Indexed drive {}:", drive);
                } catch (const std::exception& e) {
                    spdlog::error("Initial indexing failed on drive {}: {}", drive, e.what());
                } catch (...) {
                    spdlog::error("Initial indexing failed on drive {} with unknown exception", drive);
                }
                g_InitialIndexWorkers.fetch_sub(1, std::memory_order_release);
            });
        }
        // 初始全量索引完成后在后台异步保存快照
        scheduleSnapshot(rawParsers, "Initial");
    }
    spdlog::info("Search index launched for {} volume(s)", g_MftParsers.size());
    g_SearchIndexReady.store(true, std::memory_order_release);

    for (auto& w : pipeWorkers) {
        if (w.joinable()) w.join();
    }
    g_SearchIndexReady.store(false, std::memory_order_release);
    if (g_PipePokeJob.joinable()) {
        g_PipePokeJob.request_stop();
        g_PipePokeJob.join();
    }
    stopBackgroundJobs();
    for (auto& t : indexThreads) {
        if (t.joinable()) t.join();
    }
    for (auto& parser : g_MftParsers) parser->StopListening();
    g_MftParsers.clear();
    if (pipeDescriptor) LocalFree(pipeDescriptor);
    spdlog::info("IPC Server Thread stopped.");
}

// --------------------------------------------------------------------------------------
// Service Control
// --------------------------------------------------------------------------------------
void WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
        case SERVICE_CONTROL_STOP:
            spdlog::info("SERVICE_CONTROL_STOP received.");
            if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
                break;
            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwWin32ExitCode = 0;
            g_ServiceStatus.dwCheckPoint = 4;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            SetEvent(g_ServiceStopEvent);
            break;
        default:
            break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) return;

    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    if (!configurePipeEndpoint(argc, argv)) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = ERROR_INVALID_PARAMETER;
        g_ServiceStatus.dwCheckPoint = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) {
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        g_ServiceStatus.dwCheckPoint = 1;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    spdlog::info("Service started successfully.");
    g_IsRunning = true;
    
    // Start IPC Thread
    std::thread ipcThread(IPCServerThread);

    // Wait for stop event
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    RequestServiceShutdown();
    if (ipcThread.joinable()) ipcThread.join();

    CloseHandle(g_ServiceStopEvent);
    g_ServiceStopEvent = INVALID_HANDLE_VALUE;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 3;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
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
    lock = CreateMutexW(nullptr, TRUE, L"Local\\Tools3000_SearchService_Singleton");
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
