#include "core/update/UpdateChecker.h"

#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
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
constexpr int DownloadConnectTimeoutMs = 12000;
constexpr int DownloadReceiveTimeoutMs = 30000;
constexpr size_t MaxResponseBytes = 1024 * 1024;
constexpr uint64_t MaxInstallerBytes = 512ULL * 1024ULL * 1024ULL;
constexpr int64_t AutomaticRetrySeconds = 6 * 60 * 60;
constexpr const char* DismissedVersionConfigKey = "/update/dismissedVersion";
constexpr const char* LastSeenVersionConfigKey = "/app/lastSeenVersion";
constexpr const char* ShowChangelogOnUpdateConfigKey = "/general/showChangelogOnUpdate";

using json = nlohmann::json;

int64_t unixNow() {
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string trimAscii(std::string_view sv) {
    size_t s = 0;
    while (s < sv.size() && std::isspace(static_cast<unsigned char>(sv[s]))) ++s;
    size_t e = sv.size();
    while (e > s && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(s, e - s));
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

std::string normalizeVersionTag(std::string_view raw) {
    const std::string extracted = extractVersionString(std::string(raw));
    if (!extracted.empty()) {
        return "v" + extracted;
    }
    return trimAscii(raw);
}

bool isSameVersionTag(std::string_view left, std::string_view right) {
    const std::string normLeft = normalizeVersionTag(left);
    const std::string normRight = normalizeVersionTag(right);
    return !normLeft.empty() && normLeft == normRight;
}

std::string readDismissedVersion() {
    return normalizeVersionTag(
        ConfigManager::instance().get<std::string>(DismissedVersionConfigKey, ""));
}

void writeDismissedVersion(const std::string& versionTag) {
    ConfigManager::instance().set(DismissedVersionConfigKey, versionTag);
}

bool hasInnoInstallerMarkerInRegistry(const std::filesystem::path& exeDir) noexcept {
    try {
        std::error_code ec;
        std::wstring normExe = std::filesystem::weakly_canonical(exeDir, ec).wstring();
        if (normExe.empty()) normExe = exeDir.wstring();
        while (!normExe.empty() && (normExe.back() == L'\\' || normExe.back() == L'/')) {
            normExe.pop_back();
        }
        if (normExe.empty()) return false;

        static constexpr const wchar_t* kUninstallKeys[] = {
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8B6A3F14-7D2E-4E91-B5C3-300000000001}_is1",
            L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{8B6A3F14-7D2E-4E91-B5C3-300000000001}_is1"
        };
        const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
        for (HKEY root : roots) {
            for (const wchar_t* subKey : kUninstallKeys) {
                HKEY hKey = nullptr;
                if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
                    continue;
                }
                wchar_t buf[MAX_PATH * 2]{};
                DWORD bytes = sizeof(buf) - sizeof(wchar_t);
                DWORD type = 0;
                const LONG rc = RegQueryValueExW(hKey, L"InstallLocation", nullptr, &type,
                                                 reinterpret_cast<LPBYTE>(buf), &bytes);
                RegCloseKey(hKey);
                if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
                    continue;
                }
                std::wstring regLoc(buf);
                while (!regLoc.empty() && (regLoc.back() == L'\\' || regLoc.back() == L'/')) {
                    regLoc.pop_back();
                }
                std::wstring normReg = std::filesystem::weakly_canonical(std::filesystem::path(regLoc), ec).wstring();
                if (normReg.empty()) normReg = regLoc;
                while (!normReg.empty() && (normReg.back() == L'\\' || normReg.back() == L'/')) {
                    normReg.pop_back();
                }
                if (!normReg.empty() && _wcsicmp(normExe.c_str(), normReg.c_str()) == 0) {
                    return true;
                }
            }
        }
    } catch (...) {
    }
    return false;
}

bool isRunningPortableMode() noexcept {
    try {
        return UpdateChecker::isPortableDirectory(WinUtils::getExeDirectory());
    } catch (...) {
    }
    return false;
}

std::wstring escapePowerShellSingleQuoted(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            out += L"''";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::wstring resolvePowerShellExecutable() {
    wchar_t sysDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(sysDir, static_cast<UINT>(std::size(sysDir)));
    if (len > 0 && len < std::size(sysDir)) {
        const std::filesystem::path winPs =
            std::filesystem::path(std::wstring(sysDir, len)) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
        std::error_code ec;
        if (std::filesystem::exists(winPs, ec)) {
            return winPs.wstring();
        }
    }
    return L"powershell.exe";
}

std::filesystem::path getUpdatesDirectory() {
    auto dir = WinUtils::getAppDataDirectory() / L"updates";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool isSafeAssetFileName(std::string_view name) noexcept {
    if (name.empty() || name.size() > 160) return false;
    for (unsigned char ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch < 0x20) {
            return false;
        }
    }
    return name.find("..") == std::string_view::npos;
}

bool computeFileSha256Hex(const std::filesystem::path& filePath, std::string& outHex) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return false;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return false;
    }
    auto cleanup = [&]() {
        if (hHash) CryptDestroyHash(hHash);
        if (hProv) CryptReleaseContext(hProv, 0);
    };
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        cleanup();
        return false;
    }

    std::array<char, 65536> buf{};
    while (file) {
        file.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = file.gcount();
        if (n > 0) {
            if (!CryptHashData(hHash, reinterpret_cast<const BYTE*>(buf.data()),
                               static_cast<DWORD>(n), 0)) {
                cleanup();
                return false;
            }
        }
    }

    BYTE digest[32] = {};
    DWORD digestLen = sizeof(digest);
    if (!CryptGetHashParam(hHash, HP_HASHVAL, digest, &digestLen, 0) || digestLen != 32) {
        cleanup();
        return false;
    }
    cleanup();

    static constexpr char kHex[] = "0123456789abcdef";
    outHex.clear();
    outHex.reserve(64);
    for (BYTE b : digest) {
        outHex.push_back(kHex[(b >> 4) & 0x0Fu]);
        outHex.push_back(kHex[b & 0x0Fu]);
    }
    return true;
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool valid = false;
};

ParsedUrl crackHttpsUrl(std::string_view urlUtf8) {
    ParsedUrl out{};
    const std::wstring wUrl = WinUtils::utf8ToWstring(std::string(urlUtf8));
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {};
    wchar_t pathBuf[2048] = {};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &uc)) {
        return out;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS || uc.dwHostNameLength == 0) {
        return out;
    }
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    out.valid = true;
    return out;
}

bool fetchSmallHttpsText(
    std::string_view url,
    std::stop_token stopToken,
    std::mutex& reqMutex,
    void*& activeReqSlot,
    std::string& outBody) {
    if (!UpdateChecker::isTrustedReleaseDownloadUrl(url)) return false;
    const ParsedUrl parsed = crackHttpsUrl(url);
    if (!parsed.valid) return false;

    HINTERNET session = WinHttpOpen(
        L"Tools3000/1.0 (Update Checker)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, RequestTimeoutMs, RequestTimeoutMs, RequestTimeoutMs, RequestTimeoutMs);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    HINTERNET request = nullptr;
    bool registered = false;
    bool ok = false;
    if (connection && !stopToken.stop_requested()) {
        request = WinHttpOpenRequest(
            connection, L"GET", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request) {
        {
            std::lock_guard lock(reqMutex);
            if (!stopToken.stop_requested()) {
                activeReqSlot = request;
                registered = true;
            }
        }
        if (!stopToken.stop_requested() &&
            WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (statusCode == 200) {
                outBody.clear();
                bool readOk = true;
                while (!stopToken.stop_requested()) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) { readOk = false; break; }
                    if (avail == 0) break;
                    if (outBody.size() + avail > MaxResponseBytes) { readOk = false; break; }
                    const size_t off = outBody.size();
                    outBody.resize(off + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(request, outBody.data() + off, avail, &got)) { readOk = false; break; }
                    outBody.resize(off + got);
                    if (got == 0) break;
                }
                ok = readOk && !stopToken.stop_requested();
            }
        }
    }
    bool closedExternally = false;
    {
        std::lock_guard lock(reqMutex);
        if (registered) {
            if (activeReqSlot == request) activeReqSlot = nullptr;
            else closedExternally = true;
        }
    }
    if (request && !closedExternally) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

void cleanupOldUpdateFiles(const std::filesystem::path& updatesDir, const std::wstring& keepFileName) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(updatesDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (!keepFileName.empty() && _wcsicmp(name.c_str(), keepFileName.c_str()) == 0) {
            continue;
        }
        std::filesystem::remove(entry.path(), ec);
    }
}

struct SelectedReleaseAsset {
    std::string assetName;
    std::string downloadUrl;
    std::string checksumUrl;
    uint64_t sizeBytes = 0;
};

SelectedReleaseAsset pickBestAsset(const json& releaseObj, bool portableMode) {
    SelectedReleaseAsset picked{};
    if (!releaseObj.contains("assets") || !releaseObj["assets"].is_array()) return picked;

#if defined(_M_ARM64) || defined(__aarch64__)
    const bool wantArm64 = true;
#else
    const bool wantArm64 = false;
#endif

    int bestScore = -1;
    for (const auto& asset : releaseObj["assets"]) {
        if (!asset.is_object()) continue;
        const std::string name = asset.value("name", "");
        const std::string url = asset.value("browser_download_url", "");
        if (!isSafeAssetFileName(name) || !UpdateChecker::isTrustedReleaseDownloadUrl(url)) {
            continue;
        }
        const std::string low = toLowerAscii(name);
        if (low.find("sha256") != std::string::npos && low.ends_with(".txt")) {
            picked.checksumUrl = url;
            continue;
        }

        const bool isArmAsset = (low.find("arm64") != std::string::npos);
        const bool isSetupExe = low.ends_with(".exe") && low.find("setup") != std::string::npos;
        const bool isPortableZip = low.ends_with(".zip") && low.find("portable") != std::string::npos;
        if (!isSetupExe && !isPortableZip) continue;
        if (wantArm64 && !isArmAsset) continue;
        if (!wantArm64 && isArmAsset) continue;

        int score = 0;
        if (portableMode && isPortableZip) score = 100;
        else if (!portableMode && isSetupExe) score = 100;
        else score = 40;

        if (score > bestScore) {
            bestScore = score;
            picked.assetName = name;
            picked.downloadUrl = url;
            picked.sizeBytes = asset.value("size", uint64_t{0});
        }
    }
    return picked;
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

bool UpdateChecker::isTrustedReleaseDownloadUrl(std::string_view url) noexcept {
    const std::string low = toLowerAscii(trimAscii(url));
    if (low.rfind("https://", 0) != 0) return false;
    if (low.rfind("https://github.com/yuan278501381/tools3000/", 0) == 0) {
        return true;
    }
    const size_t hostStart = 8;
    const size_t slash = low.find('/', hostStart);
    const std::string_view host = (slash == std::string::npos)
        ? std::string_view(low).substr(hostStart)
        : std::string_view(low).substr(hostStart, slash - hostStart);
    return host == "objects.githubusercontent.com" ||
           host == "release-assets.githubusercontent.com" ||
           host == "github-releases.githubusercontent.com";
}

std::string UpdateChecker::extractExpectedSha256ForAsset(
    std::string_view sha256SumsBody,
    std::string_view assetName) {
    const std::string targetLow = toLowerAscii(assetName);
    if (targetLow.empty()) return {};

    size_t pos = 0;
    while (pos < sha256SumsBody.size()) {
        const size_t nl = sha256SumsBody.find('\n', pos);
        const std::string line = trimAscii(
            (nl == std::string_view::npos)
                ? sha256SumsBody.substr(pos)
                : sha256SumsBody.substr(pos, nl - pos));
        pos = (nl == std::string_view::npos) ? sha256SumsBody.size() : (nl + 1);
        if (line.size() < 66) continue;

        const size_t sp = line.find_first_of(" \t");
        if (sp != 64) continue;
        std::string hash = toLowerAscii(std::string_view(line).substr(0, 64));
        const bool validHex = std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
        if (!validHex) continue;

        std::string fileToken = trimAscii(std::string_view(line).substr(sp + 1));
        if (!fileToken.empty() && fileToken.front() == '*') {
            fileToken.erase(fileToken.begin());
        }
        const size_t slashPos = fileToken.find_last_of("/\\");
        if (slashPos != std::string::npos) {
            fileToken = fileToken.substr(slashPos + 1);
        }
        if (toLowerAscii(fileToken) == targetLow) {
            return hash;
        }
    }
    return {};
}

std::string UpdateChecker::formatReleaseNotesSummary(std::string_view rawNotes, size_t maxBytes) {
    if (rawNotes.empty()) return {};
    if (rawNotes.size() <= maxBytes) {
        return WinUtils::sanitizeUtf8(rawNotes);
    }
    return WinUtils::truncateUtf8Head(rawNotes, maxBytes) + "...";
}

std::string UpdateChecker::getStateJsonString() const {
    std::lock_guard lock(m_stateMutex);
    const auto& s = m_state;
    const bool readyToInstall = (s.status == "readyToInstall" && !s.downloadedFilePath.empty());
    const std::string dismissedVersion = readDismissedVersion();
    const bool dismissed = s.hasUpdate && isSameVersionTag(s.latestVersion, dismissedVersion);
    const bool badgeVisible = s.hasUpdate && !dismissed;
    const json payload = {
        {"status", s.status},
        {"hasUpdate", s.hasUpdate},
        {"badgeVisible", badgeVisible},
        {"dismissed", dismissed},
        {"dismissedVersion", dismissedVersion},
        {"isPortable", s.isPortable},
        {"currentVersion", s.currentVersion},
        {"latestVersion", s.latestVersion},
        {"releaseUrl", s.releaseUrl},
        {"releaseNotes", s.releaseNotes},
        {"assetName", s.assetName},
        {"assetSize", s.assetSize},
        {"downloadedBytes", s.downloadedBytes},
        {"progress", s.progress},
        {"readyToInstall", readyToInstall},
        {"autoDownloadUpdates", ConfigManager::instance().get<bool>("/general/autoDownloadUpdates", true)},
        {"error", s.error}
    };
    return payload.dump(-1, ' ', false, json::error_handler_t::replace);
}

void UpdateChecker::broadcastStateLocked(bool emitLegacyResult, bool emitToast) {
    const auto& s = m_state;
    const bool readyToInstall = (s.status == "readyToInstall" && !s.downloadedFilePath.empty());
    const std::string dismissedVersion = readDismissedVersion();
    const bool dismissed = s.hasUpdate && isSameVersionTag(s.latestVersion, dismissedVersion);
    const bool badgeVisible = s.hasUpdate && !dismissed;
    const json statePayload = {
        {"status", s.status},
        {"hasUpdate", s.hasUpdate},
        {"badgeVisible", badgeVisible},
        {"dismissed", dismissed},
        {"dismissedVersion", dismissedVersion},
        {"isPortable", s.isPortable},
        {"currentVersion", s.currentVersion},
        {"latestVersion", s.latestVersion},
        {"releaseUrl", s.releaseUrl},
        {"releaseNotes", s.releaseNotes},
        {"assetName", s.assetName},
        {"assetSize", s.assetSize},
        {"downloadedBytes", s.downloadedBytes},
        {"progress", s.progress},
        {"readyToInstall", readyToInstall},
        {"autoDownloadUpdates", ConfigManager::instance().get<bool>("/general/autoDownloadUpdates", true)},
        {"error", s.error}
    };
    MessageBridge::instance().pushEvent("update.state", statePayload);
    if (emitLegacyResult) {
        json legacy = statePayload;
        if (readyToInstall || s.status == "downloading") {
            legacy["status"] = "available";
        }
        MessageBridge::instance().pushEvent("update.result", legacy);
    }
    EventBus::instance().publish(UpdateStatusChangedEvent{
        s.hasUpdate,
        readyToInstall,
        s.latestVersion,
        badgeVisible
    });
    if (emitToast && badgeVisible) {
        const bool isZh = WinUtils::isSystemLanguageChinese();
        std::wstring message;
        if (readyToInstall) {
            message = isZh
                ? L"Tools3000 " + WinUtils::utf8ToWstring(s.latestVersion) + L" 更新已下载就绪，可一键安装"
                : L"Tools3000 " + WinUtils::utf8ToWstring(s.latestVersion) + L" update downloaded and ready to install";
        } else {
            message = isZh
                ? L"Tools3000 有新版本可用: " + WinUtils::utf8ToWstring(s.latestVersion)
                : L"A new Tools3000 version is available: " + WinUtils::utf8ToWstring(s.latestVersion);
        }
        EventBus::instance().publish(ShowToastEvent{message});
    }
}

bool UpdateChecker::dismissCurrentUpdate() {
    std::lock_guard stLock(m_stateMutex);
    if (!m_state.hasUpdate || m_state.latestVersion.empty()) {
        return false;
    }
    const std::string normalized = normalizeVersionTag(m_state.latestVersion);
    if (normalized.empty()) {
        return false;
    }
    writeDismissedVersion(normalized);
    LOG_INFO("UpdateChecker: dismissed update badge for {}", normalized);
    broadcastStateLocked(/*emitLegacyResult=*/false, /*emitToast=*/false);
    return true;
}

void UpdateChecker::clearDismissedVersion() {
    std::lock_guard stLock(m_stateMutex);
    const std::string existing = readDismissedVersion();
    if (existing.empty()) {
        return;
    }
    writeDismissedVersion("");
    LOG_INFO("UpdateChecker: cleared dismissed update version (previous={})", existing);
    if (m_state.hasUpdate) {
        broadcastStateLocked(/*emitLegacyResult=*/false, /*emitToast=*/false);
    }
}

bool UpdateChecker::checkAsync(bool force) {
    if (m_stopping.load()) return false;
    auto& config = ConfigManager::instance();
    if (!force && !config.get<bool>("/general/checkUpdates", true)) return false;

    if (force) {
        clearDismissedVersion();
    }

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
    const bool autoDl = config.get<bool>("/general/autoDownloadUpdates", true);
    {
        std::lock_guard stLock(m_stateMutex);
        if (m_state.status != "downloading" && m_state.status != "readyToInstall") {
            m_state.status = "checking";
            m_state.error.clear();
        }
    }
    m_worker = std::jthread([this, autoDl](std::stop_token stopToken) {
        workerMain(stopToken, autoDl);
    });
    return true;
}

bool UpdateChecker::downloadUpdateAsync() {
    if (m_stopping.load()) return false;
    std::string downloadUrl;
    std::string assetName;
    {
        std::lock_guard stLock(m_stateMutex);
        if (!m_state.hasUpdate || m_state.downloadUrl.empty() || m_state.assetName.empty()) {
            return false;
        }
        if (!readDismissedVersion().empty()) {
            writeDismissedVersion("");
            LOG_INFO("UpdateChecker: cleared dismissed version on explicit download request");
        }
        if (m_state.status == "readyToInstall" && !m_state.downloadedFilePath.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(WinUtils::utf8ToWstring(m_state.downloadedFilePath), ec)) {
                broadcastStateLocked(false, false);
                return true;
            }
        }
        downloadUrl = m_state.downloadUrl;
        assetName = m_state.assetName;
    }
    if (!isTrustedReleaseDownloadUrl(downloadUrl) || !isSafeAssetFileName(assetName)) {
        return false;
    }

    std::lock_guard lock(m_workerMutex);
    if (m_stopping.load() || m_downloading.exchange(true)) return false;
    if (m_downloadWorker.joinable()) m_downloadWorker.join();
    m_downloadWorker = std::jthread([this](std::stop_token stopToken) {
        downloadWorkerMain(stopToken);
    });
    return true;
}

bool UpdateChecker::isPortableDirectory(const std::filesystem::path& exeDir) noexcept {
    try {
        if (exeDir.empty()) return false;
        std::error_code ec;
        // 1. 显式便携标记或便携数据目录（与 WinUtils::isPortableMode 单一事实源完全对齐）
        if (std::filesystem::exists(exeDir / L"portable.flag", ec)) return true;
        if (std::filesystem::is_directory(exeDir / L".tools3000", ec)) return true;
        if (std::filesystem::is_directory(exeDir / L"data", ec)) return true;
        if (std::filesystem::is_directory(exeDir / L"portable_data", ec)) return true;

        // 2. Inno Setup 正式安装目录必然包含 unins000.exe/unins000.dat 或在卸载注册表中精确匹配 InstallLocation
        if (std::filesystem::exists(exeDir / L"unins000.exe", ec) ||
            std::filesystem::exists(exeDir / L"unins000.dat", ec)) {
            return false;
        }
        if (hasInnoInstallerMarkerInRegistry(exeDir)) {
            return false;
        }

        // 3. 既非 Inno Setup 安装目录也无卸载器：判定为绿色解压便携版，杜绝自动更新误走 Setup.exe 安装出重复副本
        return true;
    } catch (...) {
    }
    return false;
}

std::wstring UpdateChecker::buildInstallerParameters() {
    // 严禁传 /CLOSEAPPLICATIONS（会覆盖 installer.iss 的 CloseApplications=no 并唤醒 Windows Restart Manager，
    // 在旧进程退场空窗期抢先弹出“无法自动关闭所有应用程序”错误框）；
    // 进程优雅等待、僵尸清场与升级后自动拉起均由 installer.iss (/AUTOUPDATE) 统一毫秒级精准接管。
    return L"/SILENT /NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS /AUTOUPDATE";
}

std::wstring UpdateChecker::buildInstallerParametersForDir(const std::filesystem::path& installDir) {
    std::wstring params = buildInstallerParameters();
    if (installDir.empty()) return params;
    std::wstring dirW = installDir.wstring();
    while (dirW.size() > 3 && (dirW.back() == L'\\' || dirW.back() == L'/')) {
        dirW.pop_back();
    }
    if (dirW.empty()) return params;
    std::wstring sanitized;
    sanitized.reserve(dirW.size());
    for (wchar_t ch : dirW) {
        if (ch != L'"' && ch != L'\r' && ch != L'\n') {
            sanitized.push_back(ch);
        }
    }
    if (!sanitized.empty()) {
        params += L" /DIR=\"" + sanitized + L"\"";
    }
    return params;
}

std::wstring UpdateChecker::buildPortableUpdaterScript(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& targetDir,
    const std::filesystem::path& logFilePath,
    uint32_t parentPid,
    bool relaunchAfterUpdate) {
    const std::wstring zipEsc = escapePowerShellSingleQuoted(zipPath.wstring());
    const std::wstring targetEsc = escapePowerShellSingleQuoted(targetDir.wstring());
    const std::wstring logEsc = escapePowerShellSingleQuoted(logFilePath.wstring());
    const std::wstring relaunchLit = relaunchAfterUpdate ? L"$true" : L"$false";

    std::wstring script;
    script.reserve(4096);
    script += L"$ErrorActionPreference = 'Stop'\r\n";
    script += L"$zipPath = '" + zipEsc + L"'\r\n";
    script += L"$targetDir = '" + targetEsc + L"'\r\n";
    script += L"$logPath = '" + logEsc + L"'\r\n";
    script += L"$parentPid = " + std::to_wstring(parentPid) + L"\r\n";
    script += L"$relaunch = " + relaunchLit + L"\r\n";
    script += LR"PS(
function Write-UpdLog([string]$msg) {
    try {
        $ts = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff')
        $line = "[$ts] $msg"
        $dir = Split-Path -Parent $logPath
        if ($dir -and -not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
    } catch {}
}

$stageDir = Join-Path ([System.IO.Path]::GetTempPath()) ("Tools3000_PortableStage_" + $PID + "_" + [DateTime]::UtcNow.Ticks)
try {
    Write-UpdLog "[INFO] Portable in-place update started: zip=$zipPath target=$targetDir parentPid=$parentPid"
    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        throw "Portable ZIP not found: $zipPath"
    }
    if (-not (Test-Path -LiteralPath $targetDir -PathType Container)) {
        throw "Target directory not found: $targetDir"
    }

    # 1. Wait gracefully for parent Tools3000.exe process to exit
    if ($parentPid -gt 0) {
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $deadline) {
            $proc = Get-Process -Id $parentPid -ErrorAction SilentlyContinue
            if (-not $proc) { break }
            Start-Sleep -Milliseconds 150
        }
        Write-UpdLog "[INFO] Parent process exited or wait finished."
    }

    # 2. Stop portable isolated service running strictly inside targetDir (never touch SCM service elsewhere)
    $normTarget = [System.IO.Path]::GetFullPath($targetDir).TrimEnd('\') + '\'
    foreach ($svcProc in @(Get-Process -Name 'Tools3000_Service' -ErrorAction SilentlyContinue)) {
        try {
            $svcPath = $svcProc.MainModule.FileName
            if ($svcPath -and $svcPath.StartsWith($normTarget, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-UpdLog "[INFO] Stopping portable service inside targetDir: pid=$($svcProc.Id)"
                Stop-Process -Id $svcProc.Id -Force -ErrorAction SilentlyContinue
                Start-Sleep -Milliseconds 200
            }
        } catch {}
    }

    # 3. Extract verified portable ZIP to isolated staging folder and validate payload
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $stageDir)
    $payloadRoot = $stageDir
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot 'Tools3000.exe') -PathType Leaf)) {
        $subDirs = @(Get-ChildItem -LiteralPath $stageDir -Directory)
        if ($subDirs.Count -eq 1 -and (Test-Path -LiteralPath (Join-Path $subDirs[0].FullName 'Tools3000.exe') -PathType Leaf)) {
            $payloadRoot = $subDirs[0].FullName
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot 'Tools3000.exe') -PathType Leaf)) {
        throw "Invalid portable package: Tools3000.exe missing in extracted archive"
    }

    # 4. In-place atomic-style copy while strictly preserving all user portable data directories
    $protectedTopDirs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($d in @('.tools3000', 'data', 'portable_data', 'config', 'logs', 'screenshots', 'recordings', 'clipboard', 'updates')) {
        [void]$protectedTopDirs.Add($d)
    }

    $rootFull = [System.IO.Path]::GetFullPath($payloadRoot).TrimEnd('\') + '\'
    $allFiles = @(Get-ChildItem -LiteralPath $payloadRoot -Recurse -File)
    $copiedCount = 0
    foreach ($f in $allFiles) {
        $rel = $f.FullName.Substring($rootFull.Length)
        $firstSeg = ($rel -split '[\\/]')[0]
        if ($protectedTopDirs.Contains($firstSeg)) {
            continue
        }
        $destFile = Join-Path $targetDir $rel
        $destParent = Split-Path -Parent $destFile
        if ($destParent -and -not (Test-Path -LiteralPath $destParent)) {
            New-Item -ItemType Directory -Path $destParent -Force | Out-Null
        }
        $copied = $false
        for ($attempt = 1; $attempt -le 8; $attempt++) {
            try {
                Copy-Item -LiteralPath $f.FullName -Destination $destFile -Force
                $copied = $true
                break
            } catch {
                if ($attempt -eq 8) { throw }
                Start-Sleep -Milliseconds 300
            }
        }
        if ($copied) { $copiedCount++ }
    }

    # 5. Guarantee portable.flag stays present so subsequent updates always stay in portable mode
    $flagPath = Join-Path $targetDir 'portable.flag'
    if (-not (Test-Path -LiteralPath $flagPath -PathType Leaf)) {
        [System.IO.File]::WriteAllText($flagPath, "Tools3000 Portable Edition", (New-Object System.Text.UTF8Encoding($false)))
    }
    Write-UpdLog "[OK] Portable update copied $copiedCount files into $targetDir"

    # 6. Relaunch updated Tools3000.exe from the exact original portable path
    if ($relaunch) {
        $exePath = Join-Path $targetDir 'Tools3000.exe'
        if (Test-Path -LiteralPath $exePath -PathType Leaf) {
            Start-Process -FilePath $exePath -WorkingDirectory $targetDir
            Write-UpdLog "[OK] Relaunched updated portable binary: $exePath"
        }
    }
} catch {
    Write-UpdLog "[ERROR] Portable update failed: $($_.Exception.Message)"
    if ($relaunch -and (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        try { Start-Process -FilePath 'explorer.exe' -ArgumentList ('/select,"' + $zipPath + '"') } catch {}
    }
    exit 1
} finally {
    if (Test-Path -LiteralPath $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    try {
        $selfPath = $MyInvocation.MyCommand.Path
        if ($selfPath -and (Test-Path -LiteralPath $selfPath -PathType Leaf)) {
            Remove-Item -LiteralPath $selfPath -Force -ErrorAction SilentlyContinue
        }
    } catch {}
}
)PS";
    return script;
}

bool UpdateChecker::launchPortableInPlaceUpdater(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& targetDir,
    uint32_t parentPid,
    std::string& outError,
    bool relaunchAfterUpdate,
    std::filesystem::path* outScriptPath) {
    std::error_code ec;
    if (!std::filesystem::exists(zipPath, ec) || !std::filesystem::is_directory(targetDir, ec)) {
        outError = "file_missing";
        return false;
    }

    const std::filesystem::path scriptDir = zipPath.parent_path();
    const std::wstring stamp = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    const std::filesystem::path scriptPath = scriptDir / (L"apply-portable-update-" + stamp + L".ps1");
    const std::filesystem::path logPath = scriptDir / L"portable-update.log";

    const std::wstring scriptW =
        buildPortableUpdaterScript(zipPath, targetDir, logPath, parentPid, relaunchAfterUpdate);
    const std::string scriptUtf8 = WinUtils::wstringToUtf8(scriptW);
    {
        std::ofstream ofs(scriptPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            outError = "write_script_failed";
            return false;
        }
        // UTF-8 BOM guarantees Windows PowerShell 5.1 parses Chinese / 8-language paths cleanly
        static constexpr unsigned char kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
        ofs.write(reinterpret_cast<const char*>(kUtf8Bom), 3);
        ofs.write(scriptUtf8.data(), static_cast<std::streamsize>(scriptUtf8.size()));
    }
    if (outScriptPath) {
        *outScriptPath = scriptPath;
    }

    const std::wstring psExe = resolvePowerShellExecutable();
    std::wstring cmdLine =
        L"\"" + psExe + L"\" -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" +
        scriptPath.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(
        nullptr, cmdLine.data(), nullptr, nullptr, FALSE, flags,
        nullptr, scriptDir.wstring().c_str(), &si, &pi);
    if (!ok) {
        // 某些受限嵌套 Job 环境禁止 BREAKAWAY 时，降级为标准独立进程组启动
        flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
        ok = CreateProcessW(
            nullptr, cmdLine.data(), nullptr, nullptr, FALSE, flags,
            nullptr, scriptDir.wstring().c_str(), &si, &pi);
    }
    if (!ok) {
        outError = "launch_failed";
        std::filesystem::remove(scriptPath, ec);
        return false;
    }

    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    return true;
}

void UpdateChecker::setExitForUpdateCallback(std::function<void()> callback) {
    std::lock_guard stLock(m_stateMutex);
    m_exitForUpdateCallback = std::move(callback);
}

bool UpdateChecker::installDownloadedUpdate(std::string& outError) {
    if (m_stopping.load()) {
        outError = "stopping";
        return false;
    }
    if (m_launchingInstaller.exchange(true)) {
        outError = "already_launching";
        return false;
    }
    struct LaunchGuard {
        std::atomic<bool>& flag;
        bool release = true;
        ~LaunchGuard() {
            if (release) flag.store(false);
        }
    } guard{m_launchingInstaller, true};

    std::string filePathUtf8;
    bool portableMode = false;
    std::function<void()> exitCallback;
    {
        std::lock_guard stLock(m_stateMutex);
        if (m_state.status != "readyToInstall" || m_state.downloadedFilePath.empty()) {
            outError = "not_ready";
            return false;
        }
        filePathUtf8 = m_state.downloadedFilePath;
        portableMode = m_state.isPortable;
        exitCallback = m_exitForUpdateCallback;
    }

    const std::filesystem::path targetPath(WinUtils::utf8ToWstring(filePathUtf8));
    const std::filesystem::path updatesDir = getUpdatesDirectory();
    std::error_code ec;
    const auto canonTarget = std::filesystem::weakly_canonical(targetPath, ec);
    const auto canonUpdates = std::filesystem::weakly_canonical(updatesDir, ec);
    if (!std::filesystem::exists(canonTarget, ec)) {
        outError = "file_missing";
        return false;
    }
    // 路径防穿越：严禁执行 updates 目录外的任何文件
    const std::wstring targetW = canonTarget.wstring();
    const std::wstring dirW = canonUpdates.wstring();
    if (targetW.size() <= dirW.size() ||
        _wcsnicmp(targetW.c_str(), dirW.c_str(), dirW.size()) != 0) {
        outError = "invalid_path";
        return false;
    }

    const std::wstring extW = canonTarget.extension().wstring();
    const bool isZipExt = (_wcsicmp(extW.c_str(), L".zip") == 0);
    const bool isExeExt = (_wcsicmp(extW.c_str(), L".exe") == 0);
    if (portableMode || isZipExt) {
        const auto currentExeDir = WinUtils::getExeDirectory();
        if (!launchPortableInPlaceUpdater(
                canonTarget, currentExeDir, static_cast<uint32_t>(GetCurrentProcessId()),
                outError, /*relaunchAfterUpdate=*/true)) {
            LOG_WARN("UpdateChecker: portable in-place updater launch failed (err={}), opening folder fallback", outError);
            const std::wstring params = L"/select,\"" + targetW + L"\"";
            HINSTANCE hi = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<intptr_t>(hi) <= 32) {
                return false;
            }
            return true;
        }
        guard.release = false;
        LOG_INFO("UpdateChecker: launched portable in-place updater for targetDir");
        if (exitCallback) {
            exitCallback();
        }
        return true;
    }

    if (!isExeExt) {
        outError = "unsupported_asset";
        return false;
    }

    const std::wstring installerParams = buildInstallerParametersForDir(WinUtils::getExeDirectory());
    const std::wstring workDir = canonTarget.parent_path().wstring();
    bool launched = false;
    DWORD launchErr = ERROR_SUCCESS;

    // 当当前进程已提权时，优先使用 CreateProcessW + CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP
    // 直接启动安装包（零额外 UAC 弹窗且彻底脱离当前进程 Job Object）；否则经 ShellExecuteExW("runas")
    // 交由系统 Appinfo 服务跨进程提权启动（天然脱离父进程树与 Job Object）。
    if (WinUtils::isCurrentProcessElevated()) {
        std::wstring cmdLine = L"\"" + targetW + L"\" " + installerParams;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        const DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP;
        if (CreateProcessW(targetW.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, flags,
                           nullptr, workDir.c_str(), &si, &pi)) {
            if (pi.hThread) CloseHandle(pi.hThread);
            if (pi.hProcess) CloseHandle(pi.hProcess);
            launched = true;
        } else {
            launchErr = GetLastError();
            LOG_WARN("UpdateChecker: CreateProcessW breakaway launch failed (err={}), falling back to ShellExecuteExW",
                     launchErr);
        }
    }

    if (!launched) {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(comHr);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = WinUtils::isCurrentProcessElevated() ? L"open" : L"runas";
        sei.lpFile = targetW.c_str();
        sei.lpParameters = installerParams.c_str();
        sei.lpDirectory = workDir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            launched = true;
        } else {
            launchErr = GetLastError();
        }
        if (needUninit) CoUninitialize();
    }

    if (!launched) {
        if (launchErr == ERROR_CANCELLED) {
            LOG_INFO("UpdateChecker: update installation cancelled by user at UAC prompt");
            outError = "cancelled";
        } else {
            LOG_WARN("UpdateChecker: failed to launch installer, err={}", launchErr);
            outError = "launch_failed";
        }
        return false;
    }

    // 安装器已成功启动：保持 m_launchingInstaller 锁死防重复点击，并触发当前旧实例延迟优雅退场，
    // 让安装器 0 锁无阻完成升级并自动拉起新版。
    guard.release = false;
    LOG_INFO("UpdateChecker: launched verified update installer");
    if (exitCallback) {
        exitCallback();
    }
    return true;
}

void UpdateChecker::workerMain(std::stop_token stopToken, bool autoDownloadIfAvailable) {
    struct BusyGuard {
        std::atomic<bool>& flag;
        ~BusyGuard() noexcept { flag.store(false); }
    } busyGuard{m_busy};

    UpdateStateSnapshot next{};
    next.status = "error";
    next.currentVersion = CurrentVersion;
    next.isPortable = isRunningPortableMode();
    next.error = "network";

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
                        next.error = "responseTooLarge";
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
                        next.status = available ? "available" : "upToDate";
                        next.hasUpdate = available;
                        next.latestVersion = "v" + latestVersion;
                        next.releaseUrl = releaseUrl;
                        next.error.clear();

                        next.releaseNotes = formatReleaseNotesSummary(response.value("body", ""), 600);

                        if (available) {
                            const auto picked = pickBestAsset(response, next.isPortable);
                            next.assetName = picked.assetName;
                            next.downloadUrl = picked.downloadUrl;
                            next.checksumUrl = picked.checksumUrl;
                            next.assetSize = picked.sizeBytes;

                            // 若本地 updates 缓存已存在同版安装包，直接验证并标记 readyToInstall 免重复下载
                            if (!next.assetName.empty()) {
                                const auto cachedPath = getUpdatesDirectory() / WinUtils::utf8ToWstring(next.assetName);
                                std::error_code ec;
                                if (std::filesystem::exists(cachedPath, ec) &&
                                    (next.assetSize == 0 || std::filesystem::file_size(cachedPath, ec) == next.assetSize)) {
                                    bool hashOk = true;
                                    if (!next.checksumUrl.empty()) {
                                        std::string sumsBody;
                                        if (fetchSmallHttpsText(next.checksumUrl, stopToken, m_requestMutex, m_activeRequest, sumsBody)) {
                                            const std::string expected = extractExpectedSha256ForAsset(sumsBody, next.assetName);
                                            if (!expected.empty()) {
                                                std::string actual;
                                                hashOk = computeFileSha256Hex(cachedPath, actual) && (actual == expected);
                                            }
                                        }
                                    }
                                    if (hashOk) {
                                        next.status = "readyToInstall";
                                        next.downloadedBytes = std::filesystem::file_size(cachedPath, ec);
                                        next.progress = 100;
                                        next.downloadedFilePath = WinUtils::wstringToUtf8(cachedPath.wstring());
                                    }
                                }
                            }
                        }
                        ConfigManager::instance().set("/update/lastSuccessUnix", unixNow());
                    } catch (const std::exception& e) {
                        LOG_WARN("更新响应解析失败: {}", e.what());
                        next.status = "error";
                        next.error = "invalidResponse";
                    }
                }
            } else if (statusCode == 404) {
                next.status = "unavailable";
                next.error = "noPublishedRelease";
            } else if (statusCode == 403 || statusCode == 429) {
                next.status = "error";
                next.error = "rateLimited";
            } else {
                next.status = "error";
                next.error = "http" + std::to_string(statusCode);
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

    bool shouldAutoDownload = false;
    if (!stopToken.stop_requested()) {
        LOG_INFO("更新检查完成: {}", next.status);
        {
            std::lock_guard stLock(m_stateMutex);
            const std::string prevDismissed = readDismissedVersion();
            if (next.hasUpdate && !prevDismissed.empty() &&
                isNewerVersion(next.latestVersion, prevDismissed)) {
                writeDismissedVersion("");
                LOG_INFO("UpdateChecker: newer release {} supersedes dismissed version {}, clearing dismissal",
                         next.latestVersion, prevDismissed);
            }
            const bool isDismissed =
                next.hasUpdate && isSameVersionTag(next.latestVersion, readDismissedVersion());

            m_state = next;
            shouldAutoDownload = autoDownloadIfAvailable &&
                                 m_state.hasUpdate &&
                                 !isDismissed &&
                                 m_state.status == "available" &&
                                 !m_state.downloadUrl.empty();
            broadcastStateLocked(/*emitLegacyResult=*/true, /*emitToast=*/!shouldAutoDownload && !isDismissed);
        }
    }
    m_busy = false;

    if (shouldAutoDownload && !stopToken.stop_requested()) {
        downloadUpdateAsync();
    }
}

void UpdateChecker::downloadWorkerMain(std::stop_token stopToken) {
    std::string downloadUrl;
    std::string checksumUrl;
    std::string assetName;
    uint64_t expectedSize = 0;
    {
        std::lock_guard stLock(m_stateMutex);
        downloadUrl = m_state.downloadUrl;
        checksumUrl = m_state.checksumUrl;
        assetName = m_state.assetName;
        expectedSize = m_state.assetSize;
        m_state.status = "downloading";
        m_state.downloadedBytes = 0;
        m_state.progress = 0;
        m_state.error.clear();
        broadcastStateLocked(false, false);
    }

    const ParsedUrl parsed = crackHttpsUrl(downloadUrl);
    const std::filesystem::path updatesDir = getUpdatesDirectory();
    const std::wstring wAssetName = WinUtils::utf8ToWstring(assetName);
    const std::filesystem::path finalPath = updatesDir / wAssetName;
    const std::filesystem::path partPath = updatesDir / (wAssetName + L".part");

    bool ok = false;
    std::string errCode = "downloadFailed";

    if (parsed.valid && isTrustedReleaseDownloadUrl(downloadUrl)) {
        HINTERNET session = WinHttpOpen(
            L"Tools3000/1.0 (Update Downloader)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connection = nullptr;
        HINTERNET request = nullptr;
        bool registered = false;

        if (session) {
            WinHttpSetTimeouts(session, DownloadConnectTimeoutMs, DownloadConnectTimeoutMs,
                               DownloadConnectTimeoutMs, DownloadReceiveTimeoutMs);
            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
            connection = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
        }
        if (connection && !stopToken.stop_requested()) {
            request = WinHttpOpenRequest(
                connection, L"GET", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        }
        if (request) {
            {
                std::lock_guard lock(m_requestMutex);
                if (!stopToken.stop_requested()) {
                    m_activeDownloadRequest = request;
                    registered = true;
                }
            }
            if (!stopToken.stop_requested() &&
                WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
                wchar_t clBuf[64] = {};
                DWORD clSize = sizeof(clBuf);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        clBuf, &clSize, WINHTTP_NO_HEADER_INDEX)) {
                    try {
                        const uint64_t headerLen = std::stoull(std::wstring(clBuf));
                        if (headerLen > 0) expectedSize = headerLen;
                    } catch (...) {
                    }
                }

                if (statusCode == 200 && (expectedSize == 0 || expectedSize <= MaxInstallerBytes)) {
                    std::ofstream outFile(partPath, std::ios::binary | std::ios::trunc);
                    if (outFile) {
                        std::array<char, 65536> buf{};
                        uint64_t written = 0;
                        auto lastEmit = std::chrono::steady_clock::now();
                        bool writeOk = true;

                        while (!stopToken.stop_requested()) {
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable(request, &avail)) { writeOk = false; break; }
                            if (avail == 0) break;
                            const DWORD chunk = (std::min)(avail, static_cast<DWORD>(buf.size()));
                            DWORD got = 0;
                            if (!WinHttpReadData(request, buf.data(), chunk, &got)) { writeOk = false; break; }
                            if (got == 0) break;
                            if (written + got > MaxInstallerBytes) {
                                writeOk = false;
                                errCode = "fileTooLarge";
                                break;
                            }
                            outFile.write(buf.data(), static_cast<std::streamsize>(got));
                            if (!outFile) {
                                writeOk = false;
                                errCode = "diskWriteFailed";
                                break;
                            }
                            written += got;
                            const auto nowTick = std::chrono::steady_clock::now();
                            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTick - lastEmit).count() >= 250) {
                                lastEmit = nowTick;
                                const int pct = (expectedSize > 0)
                                    ? std::clamp(static_cast<int>((written * 100ULL) / expectedSize), 0, 99)
                                    : 0;
                                std::lock_guard stLock(m_stateMutex);
                                m_state.assetSize = expectedSize;
                                m_state.downloadedBytes = written;
                                m_state.progress = pct;
                                broadcastStateLocked(false, false);
                            }
                        }
                        outFile.flush();
                        outFile.close();

                        if (writeOk && !stopToken.stop_requested() && written > 0 &&
                            (expectedSize == 0 || written == expectedSize)) {
                            ok = true;
                        }
                    }
                }
            }
        }
        bool closedExternally = false;
        {
            std::lock_guard lock(m_requestMutex);
            if (registered) {
                if (m_activeDownloadRequest == request) m_activeDownloadRequest = nullptr;
                else closedExternally = true;
            }
        }
        if (request && !closedExternally) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }

    if (ok && !stopToken.stop_requested()) {
        // 若 Release 提供 SHA256SUMS.txt，强制执行字节级 SHA-256 哈希强校验
        if (!checksumUrl.empty()) {
            std::string sumsBody;
            if (fetchSmallHttpsText(checksumUrl, stopToken, m_requestMutex, m_activeDownloadRequest, sumsBody)) {
                const std::string expectedHex = extractExpectedSha256ForAsset(sumsBody, assetName);
                if (!expectedHex.empty()) {
                    std::string actualHex;
                    if (!computeFileSha256Hex(partPath, actualHex) || actualHex != expectedHex) {
                        LOG_WARN("UpdateChecker: SHA-256 mismatch for downloaded asset {}", assetName);
                        ok = false;
                        errCode = "checksumMismatch";
                    }
                }
            }
        }
    }

    std::error_code ec;
    if (ok && !stopToken.stop_requested()) {
        std::filesystem::remove(finalPath, ec);
        std::filesystem::rename(partPath, finalPath, ec);
        if (ec) {
            ok = false;
            errCode = "diskRenameFailed";
        } else {
            cleanupOldUpdateFiles(updatesDir, wAssetName);
        }
    }
    if (!ok) {
        std::filesystem::remove(partPath, ec);
    }

    if (!stopToken.stop_requested()) {
        std::lock_guard stLock(m_stateMutex);
        if (ok) {
            m_state.status = "readyToInstall";
            m_state.downloadedBytes = std::filesystem::file_size(finalPath, ec);
            m_state.progress = 100;
            m_state.downloadedFilePath = WinUtils::wstringToUtf8(finalPath.wstring());
            m_state.error.clear();
            LOG_INFO("UpdateChecker: update package verified and ready ({})", assetName);
            broadcastStateLocked(false, /*emitToast=*/true);
        } else {
            m_state.status = "available";
            m_state.progress = 0;
            m_state.downloadedBytes = 0;
            m_state.error = errCode;
            LOG_WARN("UpdateChecker: update download failed ({})", errCode);
            broadcastStateLocked(false, false);
        }
    }
    m_downloading = false;
}

ChangelogLaunchDecision UpdateChecker::evaluateChangelogOnStartup(
    std::string_view currentVersion,
    std::string_view lastSeenVersion,
    bool showChangelogOnUpdate) {
    ChangelogLaunchDecision decision{};
    const std::string cur = extractVersionString(std::string(currentVersion));
    const std::string seen = extractVersionString(std::string(lastSeenVersion));
    decision.currentVersion = cur.empty() ? trimAscii(currentVersion) : cur;

    // 1. 全新安装或首次记录基线：静默锚定当前版本，绝不打扰首次使用体验
    if (seen.empty()) {
        decision.shouldPersistCurrentVersion = !decision.currentVersion.empty();
        decision.shouldShow = false;
        return decision;
    }

    // 2. 检测到真实版本升级（支持跨多版迁移，如 1.4.11 -> 1.4.15）
    if (isNewerVersion(decision.currentVersion, seen)) {
        decision.previousVersion = seen;
        if (showChangelogOnUpdate) {
            decision.shouldShow = true;
        } else {
            // 用户主动关闭了更新后自动展示：同步静默推进已读基线，避免日后重新打开开关时弹出陈旧迁移提示
            decision.shouldShow = false;
            decision.shouldPersistCurrentVersion = !decision.currentVersion.empty();
        }
        return decision;
    }

    // 3. 同版本重启或降级回滚：静默不弹
    decision.shouldShow = false;
    return decision;
}

void UpdateChecker::initializeStartupChangelogState() {
    auto& cfg = ConfigManager::instance();
    const std::string lastSeen = cfg.get<std::string>(LastSeenVersionConfigKey, "");
    const bool showOnUpdate = cfg.get<bool>(ShowChangelogOnUpdateConfigKey, true);
    const ChangelogLaunchDecision decision =
        evaluateChangelogOnStartup(CurrentVersion, lastSeen, showOnUpdate);

    if (decision.shouldPersistCurrentVersion && !decision.currentVersion.empty()) {
        cfg.set<std::string>(LastSeenVersionConfigKey, decision.currentVersion);
    }

    std::lock_guard stLock(m_stateMutex);
    m_pendingUpgradeChangelog = decision.shouldShow;
    m_changelogPreviousVersion = decision.shouldShow ? decision.previousVersion : std::string{};
    if (decision.shouldShow) {
        LOG_INFO("UpdateChecker: detected upgrade from {} to {}, What's New ready",
                 decision.previousVersion, decision.currentVersion);
    }
}

std::string UpdateChecker::getChangelogStateJsonString() const {
    auto& cfg = ConfigManager::instance();
    const bool showOnUpdate = cfg.get<bool>(ShowChangelogOnUpdateConfigKey, true);
    const std::string lastSeen = cfg.get<std::string>(LastSeenVersionConfigKey, "");
    bool pending = false;
    std::string previousVersion;
    {
        std::lock_guard stLock(m_stateMutex);
        pending = m_pendingUpgradeChangelog;
        previousVersion = m_changelogPreviousVersion;
    }
    const std::string cur = extractVersionString(std::string(CurrentVersion));
    json payload = {
        {"currentVersion", cur.empty() ? std::string(CurrentVersion) : cur},
        {"previousVersion", previousVersion},
        {"lastSeenVersion", lastSeen},
        {"shouldShow", pending && showOnUpdate},
        {"showChangelogOnUpdate", showOnUpdate},
    };
    return payload.dump();
}

bool UpdateChecker::markChangelogSeen() {
    const std::string cur = extractVersionString(std::string(CurrentVersion));
    const std::string targetVer = cur.empty() ? std::string(CurrentVersion) : cur;
    {
        std::lock_guard stLock(m_stateMutex);
        m_pendingUpgradeChangelog = false;
        m_changelogPreviousVersion.clear();
    }
    return ConfigManager::instance().set<std::string>(LastSeenVersionConfigKey, targetVer);
}

bool UpdateChecker::hasPendingUpgradeChangelog() const {
    std::lock_guard stLock(m_stateMutex);
    return m_pendingUpgradeChangelog;
}

void UpdateChecker::shutdown() {
    m_stopping = true;
    std::jthread worker;
    std::jthread dlWorker;
    {
        std::lock_guard lock(m_workerMutex);
        if (m_worker.joinable()) {
            m_worker.request_stop();
            worker = std::move(m_worker);
        }
        if (m_downloadWorker.joinable()) {
            m_downloadWorker.request_stop();
            dlWorker = std::move(m_downloadWorker);
        }
    }
    {
        std::lock_guard lock(m_requestMutex);
        if (m_activeRequest) {
            WinHttpCloseHandle(static_cast<HINTERNET>(m_activeRequest));
            m_activeRequest = nullptr;
        }
        if (m_activeDownloadRequest) {
            WinHttpCloseHandle(static_cast<HINTERNET>(m_activeDownloadRequest));
            m_activeDownloadRequest = nullptr;
        }
    }
    if (worker.joinable()) worker.join();
    if (dlWorker.joinable()) dlWorker.join();
    {
        std::lock_guard stLock(m_stateMutex);
        m_exitForUpdateCallback = nullptr;
    }
    m_busy = false;
    m_downloading = false;
    m_launchingInstaller = false;
}

UpdateChecker::~UpdateChecker() {
    shutdown();
}

}  // namespace tools3000::core
