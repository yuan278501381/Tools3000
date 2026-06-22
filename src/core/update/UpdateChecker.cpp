#include "core/update/UpdateChecker.h"

#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <string>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::core {
namespace {

constexpr wchar_t ApiHost[] = L"api.github.com";
constexpr wchar_t LatestReleasePath[] =
    L"/repos/yuan278501381/Tools3000/releases/latest";
constexpr wchar_t RequestHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2022-11-28\r\n";
constexpr int RequestTimeoutMs = 8000;
constexpr size_t MaxResponseBytes = 1024 * 1024;
constexpr int64_t AutomaticRetrySeconds = 6 * 60 * 60;

using json = nlohmann::json;

int64_t unixNow() {
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

std::string extractVersionString(const std::string& input) {
    if (input.empty()) return {};
    size_t i = 0;
    while (i < input.size()) {
        if (std::isdigit(static_cast<unsigned char>(input[i]))) {
            size_t start = i;
            size_t end = i;
            while (end < input.size() &&
                   (std::isdigit(static_cast<unsigned char>(input[end])) || input[end] == '.')) {
                ++end;
            }
            while (end > start && input[end - 1] == '.') {
                --end;
            }
            const std::string candidate = input.substr(start, end - start);
            if (!candidate.empty()) {
                return candidate;
            }
            i = end;
        } else {
            ++i;
        }
    }
    return {};
}

std::vector<int> parseVersion(std::string version) {
    const std::string verStr = extractVersionString(version);
    if (verStr.empty()) return {};

    std::vector<int> parts;
    size_t start = 0;
    while (start < verStr.size()) {
        const size_t end = verStr.find('.', start);
        const std::string part = verStr.substr(start, end - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            })) {
            return {};
        }
        try {
            parts.push_back(std::stoi(part));
        } catch (...) {
            return {};
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

json errorResult(std::string code) {
    return {
        {"status", "error"},
        {"currentVersion", UpdateChecker::CurrentVersion},
        {"error", std::move(code)}
    };
}

}  // namespace

UpdateChecker& UpdateChecker::instance() {
    static UpdateChecker checker;
    return checker;
}

bool UpdateChecker::isNewerVersion(std::string_view candidate, std::string_view current) {
    auto left = parseVersion(std::string(candidate));
    auto right = parseVersion(std::string(current));
    if (left.empty() || right.empty()) return false;
    const size_t count = std::max(left.size(), right.size());
    left.resize(count, 0);
    right.resize(count, 0);
    return left > right;
}

bool UpdateChecker::checkAsync(bool force) {
    if (m_stopping.load()) return false;
    auto& config = ConfigManager::instance();
    if (!force && !config.get<bool>("/general/checkUpdates", true)) return false;

    const int64_t now = unixNow();
    if (!force) {
        const int64_t lastAttempt = config.get<int64_t>("/update/lastAttemptUnix", 0);
        if (lastAttempt > 0 && now >= lastAttempt &&
            now - lastAttempt < AutomaticRetrySeconds) {
            return false;
        }
    }

    std::lock_guard lock(m_workerMutex);
    if (m_stopping.load() || m_busy.exchange(true)) return false;
    if (m_worker.joinable()) m_worker.join();
    config.set("/update/lastAttemptUnix", now);
    m_worker = std::jthread([this](std::stop_token stopToken) {
        workerMain(stopToken);
    });
    return true;
}

void UpdateChecker::workerMain(std::stop_token stopToken) {
    json result = errorResult("network");
    HINTERNET session = WinHttpOpen(
        L"Tools3000/1.0 (Update Checker)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    bool requestRegistered = false;

    if (session) {
        WinHttpSetTimeouts(session, RequestTimeoutMs, RequestTimeoutMs,
                           RequestTimeoutMs, RequestTimeoutMs);
        connection = WinHttpConnect(session, ApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (connection && !stopToken.stop_requested()) {
        request = WinHttpOpenRequest(
            connection, L"GET", LatestReleasePath, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }

    if (request) {
        {
            std::lock_guard lock(m_requestMutex);
            if (!stopToken.stop_requested()) {
                m_activeRequest = request;
                requestRegistered = true;
            }
        }
        if (!stopToken.stop_requested() &&
            WinHttpSendRequest(request, RequestHeaders, std::numeric_limits<DWORD>::max(),
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);

            if (statusCode == 200) {
                std::string body;
                bool readOk = true;
                while (!stopToken.stop_requested()) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available)) {
                        readOk = false;
                        break;
                    }
                    if (available == 0) break;
                    if (body.size() + available > MaxResponseBytes) {
                        readOk = false;
                        result = errorResult("responseTooLarge");
                        break;
                    }
                    const size_t offset = body.size();
                    body.resize(offset + available);
                    DWORD bytesRead = 0;
                    if (!WinHttpReadData(request, body.data() + offset, available, &bytesRead)) {
                        readOk = false;
                        break;
                    }
                    body.resize(offset + bytesRead);
                    if (bytesRead == 0) break;
                }

                if (readOk && !stopToken.stop_requested()) {
                    try {
                        const auto response = json::parse(body);
                        const std::string tagName = response.value("tag_name", "");
                        const std::string releaseName = response.value("name", "");
                        std::string releaseUrl = response.value("html_url", "");
                        if (releaseUrl.empty()) {
                            releaseUrl = "https://github.com/yuan278501381/Tools3000/releases";
                        }

                        std::string latestVersion = extractVersionString(tagName);
                        if (latestVersion.empty()) {
                            latestVersion = extractVersionString(releaseName);
                        }
                        if (latestVersion.empty()) {
                            latestVersion = CurrentVersion;
                        }

                        const bool available = UpdateChecker::isNewerVersion(latestVersion, CurrentVersion);
                        result = {
                            {"status", available ? "available" : "upToDate"},
                            {"currentVersion", CurrentVersion},
                            {"latestVersion", "v" + latestVersion},
                            {"releaseUrl", releaseUrl}
                        };
                        ConfigManager::instance().set("/update/lastSuccessUnix", unixNow());
                    } catch (const std::exception& e) {
                        LOG_WARN("更新响应解析失败: {}", e.what());
                        result = errorResult("invalidResponse");
                    }
                }
            } else if (statusCode == 404) {
                result = {
                    {"status", "unavailable"},
                    {"currentVersion", CurrentVersion},
                    {"error", "noPublishedRelease"}
                };
            } else if (statusCode == 403 || statusCode == 429) {
                result = errorResult("rateLimited");
            } else {
                result = errorResult("http" + std::to_string(statusCode));
            }
        }
    }

    bool requestClosedExternally = false;
    {
        std::lock_guard lock(m_requestMutex);
        if (requestRegistered) {
            if (m_activeRequest == request) m_activeRequest = nullptr;
            else requestClosedExternally = true;
        }
    }
    if (request && !requestClosedExternally) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);

    if (!stopToken.stop_requested()) {
        LOG_INFO("更新检查完成: {}", result.value("status", "error"));
        MessageBridge::instance().pushEvent("update.result", result);
        if (result.value("status", "") == "available") {
            const auto latest = result.value("latestVersion", "");
            const bool isZh = WinUtils::isSystemLanguageChinese();
            const std::wstring message = isZh
                ? L"Tools3000 有新版本可用: " + WinUtils::utf8ToWstring(latest)
                : L"A new Tools3000 version is available: " + WinUtils::utf8ToWstring(latest);
            EventBus::instance().publish(ShowToastEvent{message});
        }
    }
    m_busy = false;
}

void UpdateChecker::shutdown() {
    m_stopping = true;
    std::jthread worker;
    {
        std::lock_guard lock(m_workerMutex);
        if (m_worker.joinable()) {
            m_worker.request_stop();
            worker = std::move(m_worker);
        }
    }
    {
        std::lock_guard lock(m_requestMutex);
        if (m_activeRequest) {
            WinHttpCloseHandle(static_cast<HINTERNET>(m_activeRequest));
            m_activeRequest = nullptr;
        }
    }
    if (worker.joinable()) worker.join();
    m_busy = false;
}

UpdateChecker::~UpdateChecker() {
    shutdown();
}

}  // namespace tools3000::core
