#include "search/InstalledAppIndex.h"

#include "common/AtomicFile.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <unordered_set>

namespace tools3000::search {

namespace {

constexpr uint64_t kAppRefreshThrottleMs = 600'000ull;  // 10 分钟 TTL，过期走 SWR 后台静默热更新
constexpr size_t kMaxIndexedApps = 2048;
constexpr int kMaxStartMenuScanDepth = 4;
constexpr int kMaxPortableScanDepth = 2;
constexpr size_t kMaxFrecencyRecords = 2000;
constexpr size_t kMaxFrecencyFileBytes = 4 * 1024 * 1024;

std::string trimAsciiCopy(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

std::string toLowerAsciiAndUtf8(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

std::string normalizePathForDedup(std::string_view rawPath) {
    std::string out = toLowerAsciiAndUtf8(trimAsciiCopy(rawPath));
    for (char& ch : out) {
        if (ch == '/') ch = '\\';
    }
    while (out.size() > 3 && out.back() == '\\') {
        out.pop_back();
    }
    return out;
}

std::string stripKnownAppExtensionLower(std::string_view name) {
    std::string lower = toLowerAsciiAndUtf8(trimAsciiCopy(name));
    static constexpr std::array<std::string_view, 5> kExts = {
        ".lnk", ".exe", ".appref-ms", ".url", ".bat"
    };
    for (const auto ext : kExts) {
        if (lower.size() > ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            lower.resize(lower.size() - ext.size());
            break;
        }
    }
    return trimAsciiCopy(lower);
}

std::string compactAlphaNumLower(std::string_view lowerText) {
    std::string out;
    out.reserve(lowerText.size());
    for (unsigned char ch : lowerText) {
        if (std::isalnum(ch) || ch >= 0x80) {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

std::vector<std::string> splitLowerTokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!cur.empty()) {
                tokens.push_back(toLowerAsciiAndUtf8(cur));
                cur.clear();
            }
        } else {
            cur.push_back(static_cast<char>(ch));
        }
    }
    if (!cur.empty()) {
        tokens.push_back(toLowerAsciiAndUtf8(cur));
    }
    return tokens;
}

char gbkFirstLetterForWideChar(wchar_t wch) noexcept {
    if (wch < 0x4E00 || wch > 0x9FFF) {
        return '\0';
    }
    char gbk[4] = {};
    BOOL usedDefault = FALSE;
    const int written = WideCharToMultiByte(
        936, 0, &wch, 1, gbk, static_cast<int>(sizeof(gbk)), nullptr, &usedDefault);
    if (written != 2 || usedDefault) {
        return '\0';
    }
    const uint16_t code = static_cast<uint16_t>(
        (static_cast<uint8_t>(gbk[0]) << 8) | static_cast<uint8_t>(gbk[1]));
    if (code < 0xB0A1 || code > 0xD7F9) {
        return '\0';
    }
    struct Boundary {
        uint16_t start;
        char letter;
    };
    static constexpr std::array<Boundary, 23> kBoundaries = {{
        {0xB0A1, 'a'}, {0xB0C5, 'b'}, {0xB2C1, 'c'}, {0xB4EE, 'd'},
        {0xB6EA, 'e'}, {0xB7A2, 'f'}, {0xB8C1, 'g'}, {0xB9FE, 'h'},
        {0xBBF7, 'j'}, {0xBFA6, 'k'}, {0xC0AC, 'l'}, {0xC2E8, 'm'},
        {0xC4C3, 'n'}, {0xC5B6, 'o'}, {0xC5BE, 'p'}, {0xC6DA, 'q'},
        {0xC8BB, 'r'}, {0xC8F6, 's'}, {0xCBFA, 't'}, {0xCDDA, 'w'},
        {0xCEF4, 'x'}, {0xD1B9, 'y'}, {0xD4D1, 'z'}
    }};
    for (size_t i = kBoundaries.size(); i > 0; --i) {
        if (code >= kBoundaries[i - 1].start) {
            return kBoundaries[i - 1].letter;
        }
    }
    return '\0';
}

bool shouldSkipAppNameLower(std::string_view lowerName) noexcept {
    if (lowerName.empty()) return true;
    static constexpr std::array<std::string_view, 21> kNoiseTerms = {
        "uninstall", "uninst", "卸载", "help", "帮助", "readme", "说明",
        "manual", "license", "website", "homepage", "documentation",
        "release notes", "changelog", "repair", "modify", "updater",
        "crashpad", "crashreporter", "vc_redist", "elevate"
    };
    for (const auto term : kNoiseTerms) {
        if (lowerName.find(term) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool shouldSkipPortableExeLower(std::string_view lowerStem) noexcept {
    if (shouldSkipAppNameLower(lowerStem)) return true;
    static constexpr std::array<std::string_view, 7> kPrefixes = {
        "unins", "setup", "install", "update", "crash", "helper", "service"
    };
    for (const auto pfx : kPrefixes) {
        if (lowerStem.starts_with(pfx)) {
            return true;
        }
    }
    return false;
}

std::wstring stripWrappingQuotes(std::wstring_view raw) {
    while (!raw.empty() && (raw.front() == L' ' || raw.front() == L'\t')) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && (raw.back() == L' ' || raw.back() == L'\t' || raw.back() == L'\0')) {
        raw.remove_suffix(1);
    }
    if (raw.size() >= 2 && raw.front() == L'"' && raw.back() == L'"') {
        raw.remove_prefix(1);
        raw.remove_suffix(1);
    }
    return std::wstring(raw);
}

std::wstring expandEnvWide(const std::wstring& input) {
    if (input.find(L'%') == std::wstring::npos) {
        return input;
    }
    constexpr DWORD kBufSize = 4096;
    std::vector<wchar_t> buf(kBufSize, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(input.c_str(), buf.data(), kBufSize);
    if (written == 0 || written > kBufSize) {
        return input;
    }
    return std::wstring(buf.data(), static_cast<size_t>(written - 1));
}

bool fileExistsWide(const std::wstring& wpath) noexcept {
    if (wpath.empty()) return false;
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

int64_t currentEpochMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void addEntryIfUnique(
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems,
    std::string displayName,
    std::string utf8Path,
    std::string source) {
    if (out.size() >= kMaxIndexedApps || displayName.empty() || utf8Path.empty()) {
        return;
    }
    std::string pathLower = normalizePathForDedup(utf8Path);
    if (pathLower.empty() || seenPaths.find(pathLower) != seenPaths.end()) {
        return;
    }
    std::string stemLower = stripKnownAppExtensionLower(displayName);
    if (stemLower.empty() || shouldSkipAppNameLower(stemLower)) {
        return;
    }
    // Start Menu 优先于 App Paths / Portable；同类去重保持列表精简
    if (seenStems.find(stemLower) != seenStems.end() && source != "portable") {
        return;
    }
    InstalledAppEntry entry;
    entry.name = std::move(displayName);
    entry.path = std::move(utf8Path);
    entry.source = std::move(source);
    entry.nameLower = toLowerAsciiAndUtf8(entry.name);
    entry.stemLower = std::move(stemLower);
    entry.acronymLower = InstalledAppIndex::buildInitialsAcronym(entry.name);
    entry.pathLower = std::move(pathLower);

    const size_t idx = out.size();
    seenPaths.emplace(entry.pathLower, idx);
    seenStems.try_emplace(entry.stemLower, idx);
    out.push_back(std::move(entry));
}

void scanDirectoryRecursive(
    const std::filesystem::path& root,
    int depth,
    int maxDepth,
    bool portableMode,
    std::string_view sourceName,
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems) {
    if (depth > maxDepth || out.size() >= kMaxIndexedApps) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }
    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        return;
    }
    for (const auto& entry : it) {
        if (out.size() >= kMaxIndexedApps) {
            break;
        }
        std::error_code entryEc;
        if (entry.is_symlink(entryEc)) {
            continue;
        }
        if (entry.is_directory(entryEc)) {
            if (depth < maxDepth) {
                scanDirectoryRecursive(
                    entry.path(), depth + 1, maxDepth, portableMode,
                    sourceName, out, seenPaths, seenStems);
            }
            continue;
        }
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        const std::wstring extLower = tools3000::core::WinUtils::toLower(
            entry.path().extension().wstring());
        const bool allowed = portableMode
            ? (extLower == L".exe" || extLower == L".lnk")
            : (extLower == L".lnk" || extLower == L".appref-ms" || extLower == L".url");
        if (!allowed) {
            continue;
        }
        const std::string displayName = tools3000::core::WinUtils::pathToUtf8(entry.path().stem());
        const std::string stemLower = toLowerAsciiAndUtf8(displayName);
        if (portableMode && extLower == L".exe" && shouldSkipPortableExeLower(stemLower)) {
            continue;
        }
        const std::string utf8Path = tools3000::core::WinUtils::pathToUtf8(entry.path());
        addEntryIfUnique(out, seenPaths, seenStems, displayName, utf8Path, std::string(sourceName));
    }
}

void scanAppPathsHive(
    HKEY rootHive,
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems) {
    HKEY hAppPaths = nullptr;
    constexpr const wchar_t* kSubKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    if (RegOpenKeyExW(rootHive, kSubKey, 0, KEY_READ, &hAppPaths) != ERROR_SUCCESS || !hAppPaths) {
        return;
    }
    struct KeyGuard {
        HKEY handle;
        ~KeyGuard() { if (handle) RegCloseKey(handle); }
    } rootGuard{hAppPaths};

    wchar_t subKeyName[260] = {};
    for (DWORD idx = 0; out.size() < kMaxIndexedApps; ++idx) {
        DWORD nameLen = static_cast<DWORD>(std::size(subKeyName));
        const LONG enumRes = RegEnumKeyExW(
            hAppPaths, idx, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (enumRes == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (enumRes != ERROR_SUCCESS || nameLen == 0) {
            continue;
        }
        HKEY hSub = nullptr;
        if (RegOpenKeyExW(hAppPaths, subKeyName, 0, KEY_READ, &hSub) != ERROR_SUCCESS || !hSub) {
            continue;
        }
        KeyGuard subGuard{hSub};

        DWORD valueType = 0;
        DWORD byteSize = 0;
        if (RegQueryValueExW(hSub, nullptr, nullptr, &valueType, nullptr, &byteSize) != ERROR_SUCCESS ||
            (valueType != REG_SZ && valueType != REG_EXPAND_SZ) ||
            byteSize < sizeof(wchar_t) || byteSize > 8192) {
            continue;
        }
        std::wstring rawBuf(byteSize / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(
                hSub, nullptr, nullptr, &valueType,
                reinterpret_cast<LPBYTE>(rawBuf.data()), &byteSize) != ERROR_SUCCESS) {
            continue;
        }
        const size_t nullPos = rawBuf.find(L'\0');
        if (nullPos != std::wstring::npos) {
            rawBuf.resize(nullPos);
        }
        std::wstring targetPath = expandEnvWide(stripWrappingQuotes(rawBuf));
        if (targetPath.empty() || !fileExistsWide(targetPath)) {
            continue;
        }

        const std::filesystem::path targetFsPath(targetPath);
        std::string displayName = tools3000::core::WinUtils::pathToUtf8(
            std::filesystem::path(std::wstring(subKeyName, nameLen)).stem());
        if (displayName.empty()) {
            displayName = tools3000::core::WinUtils::pathToUtf8(targetFsPath.stem());
        }
        const std::string stemLower = toLowerAsciiAndUtf8(displayName);
        if (shouldSkipPortableExeLower(stemLower)) {
            continue;
        }
        const std::string utf8Path = tools3000::core::WinUtils::pathToUtf8(targetFsPath);
        addEntryIfUnique(out, seenPaths, seenStems, std::move(displayName), utf8Path, "app_paths");
    }
}

struct ScoredApp {
    const InstalledAppEntry* entry = nullptr;
    int score = 0;
    bool exactMatch = false;
};

ScoredApp evaluateAppEntry(
    const InstalledAppEntry& entry,
    const std::string& fullLower,
    const std::string& fullCompact,
    const std::vector<std::string>& tokens) {
    ScoredApp best{};
    best.entry = &entry;

    const std::string stemCompact = compactAlphaNumLower(entry.stemLower);

    if (entry.stemLower == fullLower || entry.nameLower == fullLower ||
        (!fullCompact.empty() && stemCompact == fullCompact)) {
        best.score = 980;
        best.exactMatch = true;
    } else if (!entry.acronymLower.empty() && entry.acronymLower == fullLower) {
        best.score = 940;
        best.exactMatch = true;
    } else if (entry.stemLower.starts_with(fullLower) ||
               (!fullCompact.empty() && stemCompact.starts_with(fullCompact))) {
        best.score = 840;
    } else if (!entry.acronymLower.empty() && entry.acronymLower.starts_with(fullLower)) {
        best.score = 810;
    } else if (entry.stemLower.find(fullLower) != std::string::npos ||
               (!fullCompact.empty() && stemCompact.find(fullCompact) != std::string::npos)) {
        best.score = 720;
    } else if (fullLower.size() >= 2 &&
               !entry.acronymLower.empty() &&
               entry.acronymLower.find(fullLower) != std::string::npos) {
        best.score = 670;
    } else if (tokens.size() > 1) {
        bool allMatched = true;
        int tokenScore = 620;
        for (const auto& tok : tokens) {
            if (entry.stemLower.find(tok) != std::string::npos) {
                tokenScore += 25;
            } else if (!entry.acronymLower.empty() &&
                       entry.acronymLower.find(tok) != std::string::npos) {
                tokenScore += 15;
            } else {
                allMatched = false;
                break;
            }
        }
        if (allMatched) {
            best.score = tokenScore;
        }
    }

    if (best.score > 0 && entry.source == "start_menu") {
        best.score += 20;
    }
    return best;
}

bool isServiceAppShadowCandidate(const nlohmann::json& item) {
    const std::string path = item.value("path", "");
    if (path.empty()) return false;
    const std::string lower = normalizePathForDedup(path);
    if (lower.starts_with("shell:appsfolder")) {
        return true;
    }
    if (lower.ends_with(".lnk") &&
        (lower.find("\\start menu\\programs\\") != std::string::npos ||
         lower.find("\\microsoft\\windows\\start menu\\") != std::string::npos)) {
        return true;
    }
    return false;
}

bool readWholeFileUtf8Wide(const std::filesystem::path& path, std::string& outContent) {
    outContent.clear();
    HANDLE hFile = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    struct HandleGuard {
        HANDLE h;
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    } guard{hFile};

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > kMaxFrecencyFileBytes) {
        return false;
    }
    outContent.resize(static_cast<size_t>(fileSize.QuadPart));
    size_t totalRead = 0;
    while (totalRead < outContent.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min<size_t>)(outContent.size() - totalRead, 64 * 1024));
        DWORD readNow = 0;
        if (!ReadFile(hFile, outContent.data() + totalRead, chunk, &readNow, nullptr) || readNow == 0) {
            break;
        }
        totalRead += readNow;
    }
    outContent.resize(totalRead);
    return !outContent.empty();
}

}  // namespace

RankingProfile parseRankingProfile(std::string_view raw) noexcept {
    const std::string lower = toLowerAsciiAndUtf8(trimAsciiCopy(raw));
    if (lower == "files_first" || lower == "files-first" || lower == "filesfirst") {
        return RankingProfile::FilesFirst;
    }
    if (lower == "launcher_first" || lower == "launcher-first" || lower == "launcherfirst") {
        return RankingProfile::LauncherFirst;
    }
    return RankingProfile::BalancedFrecency;
}

const char* rankingProfileToString(RankingProfile profile) noexcept {
    switch (profile) {
        case RankingProfile::FilesFirst: return "files_first";
        case RankingProfile::LauncherFirst: return "launcher_first";
        case RankingProfile::BalancedFrecency:
        default:
            return "balanced_frecency";
    }
}

InstalledAppIndex& InstalledAppIndex::instance() {
    static InstalledAppIndex inst;
    return inst;
}

AppQueryParseResult InstalledAppIndex::parseAppQuery(std::string_view rawQuery) {
    AppQueryParseResult result{};
    const std::string trimmed = trimAsciiCopy(rawQuery);
    if (trimmed.size() >= 4 &&
        (trimmed[0] == 'a' || trimmed[0] == 'A') &&
        (trimmed[1] == 'p' || trimmed[1] == 'P') &&
        (trimmed[2] == 'p' || trimmed[2] == 'P') &&
        trimmed[3] == ':') {
        result.isExplicitAppQuery = true;
        result.cleanQuery = trimAsciiCopy(std::string_view(trimmed).substr(4));
        return result;
    }
    result.cleanQuery = trimmed;
    return result;
}

std::string InstalledAppIndex::buildInitialsAcronym(std::string_view utf8Name) {
    if (utf8Name.empty()) {
        return {};
    }
    const std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(std::string(utf8Name));
    std::string out;
    out.reserve(wide.size());

    bool prevIsAlphaNum = false;
    bool prevIsLowerAscii = false;

    for (wchar_t ch : wide) {
        if (ch >= L'A' && ch <= L'Z') {
            const char lower = static_cast<char>(ch - L'A' + L'a');
            // 词首或驼峰大写边界（如 VisualStudioCode -> vsc）
            if (!prevIsAlphaNum || prevIsLowerAscii) {
                out.push_back(lower);
            }
            prevIsAlphaNum = true;
            prevIsLowerAscii = false;
        } else if (ch >= L'a' && ch <= L'z') {
            if (!prevIsAlphaNum) {
                out.push_back(static_cast<char>(ch));
            }
            prevIsAlphaNum = true;
            prevIsLowerAscii = true;
        } else if (ch >= L'0' && ch <= L'9') {
            if (!prevIsAlphaNum) {
                out.push_back(static_cast<char>(ch));
            }
            prevIsAlphaNum = true;
            prevIsLowerAscii = false;
        } else if (ch >= 0x4E00 && ch <= 0x9FFF) {
            const char py = gbkFirstLetterForWideChar(ch);
            if (py != '\0') {
                out.push_back(py);
            }
            prevIsAlphaNum = false;
            prevIsLowerAscii = false;
        } else {
            prevIsAlphaNum = false;
            prevIsLowerAscii = false;
        }
    }
    return out;
}

InstalledAppIndex::~InstalledAppIndex() {
    shutdown();
}

void InstalledAppIndex::setPortableAppDirs(std::vector<std::string> dirs) {
    std::vector<std::string> cleaned;
    cleaned.reserve(dirs.size());
    for (auto& dir : dirs) {
        std::string trimmed = trimAsciiCopy(dir);
        if (!trimmed.empty() && cleaned.size() < 32) {
            cleaned.push_back(std::move(trimmed));
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cleaned != m_portableDirs) {
        m_portableDirs = std::move(cleaned);
        ++m_configVersion;
        m_dirty = true;
    }
}

std::vector<std::string> InstalledAppIndex::getPortableAppDirs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_portableDirs;
}

void InstalledAppIndex::clearCache() {
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_configVersion;
        if (!m_refreshInFlight && m_refreshWorker.joinable()) {
            workerToJoin = std::move(m_refreshWorker);
        }
        m_entries.clear();
        m_entries.shrink_to_fit();
        m_lastRefreshTickMs = 0;
        m_dirty = true;
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

void InstalledAppIndex::shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_configVersion;
        if (m_refreshWorker.joinable()) {
            workerToJoin = std::move(m_refreshWorker);
        }
        m_entries.clear();
        m_entries.shrink_to_fit();
        m_lastRefreshTickMs = 0;
        m_dirty = true;
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshInFlight = false;
    }
    m_refreshCv.notify_all();
}

std::vector<InstalledAppEntry> InstalledAppIndex::buildFreshEntries(const std::vector<std::string>& portableDirs) {
    std::vector<InstalledAppEntry> fresh;
    fresh.reserve(256);
    std::unordered_map<std::string, size_t> seenPaths;
    std::unordered_map<std::string, size_t> seenStems;
    seenPaths.reserve(512);
    seenStems.reserve(512);

    scanKnownStartMenuFolders(fresh, seenPaths, seenStems);
    scanAppPathsRegistry(fresh, seenPaths, seenStems);
    scanPortableDirectories(portableDirs, fresh, seenPaths, seenStems);

    fresh.shrink_to_fit();
    return fresh;
}

void InstalledAppIndex::launchAsyncRefreshLocked(
    uint64_t expectedVersion,
    std::vector<std::string> portableDirs) {
    if (m_refreshInFlight || m_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    if (m_refreshWorker.joinable()) {
        m_refreshWorker.join();
    }
    m_refreshInFlight = true;
    m_refreshWorker = std::thread([this, expectedVersion, dirs = std::move(portableDirs)]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::vector<InstalledAppEntry> fresh;
        try {
            if (!m_shuttingDown.load(std::memory_order_acquire)) {
                fresh = buildFreshEntries(dirs);
            }
        } catch (...) {
            fresh.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_shuttingDown.load(std::memory_order_acquire) &&
                m_configVersion == expectedVersion) {
                m_entries = std::move(fresh);
                m_lastRefreshTickMs = GetTickCount64();
                m_dirty = false;
            }
            m_refreshInFlight = false;
        }
        m_refreshCv.notify_all();
    });
}

void InstalledAppIndex::warmupAsync() {
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t now = GetTickCount64();
    const bool expired = (m_lastRefreshTickMs == 0 || now < m_lastRefreshTickMs ||
                          (now - m_lastRefreshTickMs) >= kAppRefreshThrottleMs);
    if (!m_dirty && !m_entries.empty() && !expired) {
        return;
    }
    launchAsyncRefreshLocked(m_configVersion, m_portableDirs);
}

void InstalledAppIndex::scanKnownStartMenuFolders(
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems) {
    const KNOWNFOLDERID folders[] = {
        FOLDERID_CommonPrograms,
        FOLDERID_Programs
    };
    for (const auto& folderId : folders) {
        PWSTR rawPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folderId, 0, nullptr, &rawPath)) && rawPath) {
            const std::filesystem::path dirPath(rawPath);
            CoTaskMemFree(rawPath);
            scanDirectoryRecursive(
                dirPath, 0, kMaxStartMenuScanDepth, false,
                "start_menu", out, seenPaths, seenStems);
        }
    }
}

void InstalledAppIndex::scanAppPathsRegistry(
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems) {
    scanAppPathsHive(HKEY_LOCAL_MACHINE, out, seenPaths, seenStems);
    scanAppPathsHive(HKEY_CURRENT_USER, out, seenPaths, seenStems);
}

void InstalledAppIndex::scanPortableDirectories(
    const std::vector<std::string>& dirs,
    std::vector<InstalledAppEntry>& out,
    std::unordered_map<std::string, size_t>& seenPaths,
    std::unordered_map<std::string, size_t>& seenStems) {
    for (const auto& dirUtf8 : dirs) {
        if (dirUtf8.empty() || out.size() >= kMaxIndexedApps) {
            continue;
        }
        const std::filesystem::path dirPath = tools3000::core::WinUtils::utf8ToPath(dirUtf8);
        scanDirectoryRecursive(
            dirPath, 0, kMaxPortableScanDepth, true,
            "portable", out, seenPaths, seenStems);
    }
}

void InstalledAppIndex::ensureFreshLocked(std::unique_lock<std::mutex>& lock) {
    const uint64_t now = GetTickCount64();
    const bool expired = (m_lastRefreshTickMs == 0 || now < m_lastRefreshTickMs ||
                          (now - m_lastRefreshTickMs) >= kAppRefreshThrottleMs);

    // 1. 缓存有效：0ms 命中
    if (!m_dirty && !m_entries.empty() && !expired) {
        return;
    }

    // 2. Stale-While-Revalidate (SWR)：已有历史缓存且非配置主动变更脏态 ->
    //    立刻 0ms 返回旧缓存，同时在后台低优先级线程静默热更新，彻底消灭 60s 周期同步卡顿。
    if (!m_dirty && !m_entries.empty()) {
        launchAsyncRefreshLocked(m_configVersion, m_portableDirs);
        return;
    }

    // 3. 若后台预热正好正在跑当前版本的冷扫描，则直接汇合等待避免重复扫描磁盘
    if (m_refreshInFlight && !m_dirty) {
        m_refreshCv.wait(lock, [this]() {
            return !m_refreshInFlight || m_shuttingDown.load(std::memory_order_acquire);
        });
        if (!m_entries.empty() && !m_dirty) {
            return;
        }
    }

    // 4. 首次冷启动或便携目录刚变更：释放锁执行磁盘/注册表扫描，再在版本匹配时原子提交
    const uint64_t expectedVersion = m_configVersion;
    const std::vector<std::string> dirsSnapshot = m_portableDirs;
    lock.unlock();
    auto fresh = buildFreshEntries(dirsSnapshot);
    lock.lock();
    if (m_configVersion == expectedVersion) {
        m_entries = std::move(fresh);
        m_lastRefreshTickMs = GetTickCount64();
        m_dirty = false;
    }
}

nlohmann::json InstalledAppIndex::searchLocalApps(
    std::string_view query,
    size_t limit,
    bool allowEmptyQuery) {
    nlohmann::json out = nlohmann::json::array();
    const std::string trimmed = trimAsciiCopy(query);
    if (trimmed.empty() && !allowEmptyQuery) {
        return out;
    }
    const size_t safeLimit = (std::clamp<size_t>)(limit == 0 ? 30u : limit, 1u, 100u);

    std::unique_lock<std::mutex> lock(m_mutex);
    ensureFreshLocked(lock);

    if (trimmed.empty()) {
        std::vector<const InstalledAppEntry*> sorted;
        sorted.reserve(m_entries.size());
        for (const auto& entry : m_entries) {
            sorted.push_back(&entry);
        }
        std::sort(sorted.begin(), sorted.end(), [](const InstalledAppEntry* a, const InstalledAppEntry* b) {
            if (a->source != b->source) {
                return a->source == "start_menu";
            }
            return a->nameLower < b->nameLower;
        });
        const size_t take = (std::min)(sorted.size(), safeLimit);
        for (size_t i = 0; i < take; ++i) {
            const auto* entry = sorted[i];
            out.push_back({
                {"name", entry->name},
                {"path", entry->path},
                {"isDirectory", false},
                {"size", 0},
                {"lastWriteTime", 0},
                {"resultType", "app"},
                {"appSource", entry->source},
                {"exactAliasMatch", false},
                {"launcherScore", 500},
                {"matchRank", 500},
                {"dedupStem", entry->stemLower},
                {"runCount", 0},
                {"frecencyScore", 0.0}
            });
        }
        return out;
    }

    const std::string fullLower = toLowerAsciiAndUtf8(trimmed);
    const std::string fullCompact = compactAlphaNumLower(fullLower);
    const auto tokens = splitLowerTokens(trimmed);

    std::vector<ScoredApp> matches;
    matches.reserve(32);
    for (const auto& entry : m_entries) {
        ScoredApp evaluated = evaluateAppEntry(entry, fullLower, fullCompact, tokens);
        if (evaluated.score > 0) {
            matches.push_back(evaluated);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const ScoredApp& a, const ScoredApp& b) {
        if (a.exactMatch != b.exactMatch) return a.exactMatch > b.exactMatch;
        if (a.score != b.score) return a.score > b.score;
        return a.entry->nameLower < b.entry->nameLower;
    });

    if (matches.size() > safeLimit) {
        matches.resize(safeLimit);
    }

    for (const auto& m : matches) {
        out.push_back({
            {"name", m.entry->name},
            {"path", m.entry->path},
            {"isDirectory", false},
            {"size", 0},
            {"lastWriteTime", 0},
            {"resultType", "app"},
            {"appSource", m.entry->source},
            {"exactAliasMatch", m.exactMatch},
            {"launcherScore", m.score},
            {"matchRank", m.score},
            {"dedupStem", m.entry->stemLower},
            {"runCount", 0},
            {"frecencyScore", 0.0}
        });
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// LauncherFrecencyStore
// ─────────────────────────────────────────────────────────────────────────────

LauncherFrecencyStore& LauncherFrecencyStore::instance() {
    static LauncherFrecencyStore inst;
    return inst;
}

std::string LauncherFrecencyStore::normalizeKey(std::string_view rawKey) {
    std::string trimmed = trimAsciiCopy(rawKey);
    if (trimmed.empty()) return {};
    std::string lower = toLowerAsciiAndUtf8(trimmed);
    if (lower.starts_with("cmd://") || lower.starts_with("http://") || lower.starts_with("https://")) {
        return lower;
    }
    for (char& ch : lower) {
        if (ch == '/') ch = '\\';
    }
    return lower;
}

double LauncherFrecencyStore::computeScore(const FrecencyRecord& rec, int64_t nowEpochMs) noexcept {
    if (rec.runCount == 0) {
        return 0.0;
    }
    const double cappedCount = static_cast<double>((std::min<uint32_t>)(rec.runCount, 500u));
    const int64_t ageMs = (nowEpochMs > rec.lastRunEpochMs && rec.lastRunEpochMs > 0)
        ? (nowEpochMs - rec.lastRunEpochMs)
        : 0;
    constexpr int64_t kHourMs = 3'600'000ll;
    constexpr int64_t kDayMs = 24ll * kHourMs;
    double recencyBonus = 40.0;
    if (ageMs <= kDayMs) {
        recencyBonus = 500.0;
    } else if (ageMs <= 7ll * kDayMs) {
        recencyBonus = 350.0;
    } else if (ageMs <= 30ll * kDayMs) {
        recencyBonus = 180.0;
    }
    return cappedCount * 100.0 + recencyBonus;
}

std::filesystem::path LauncherFrecencyStore::resolveStoragePathLocked() const {
    if (!m_customStoragePath.empty()) {
        return m_customStoragePath;
    }
    return tools3000::core::WinUtils::getAppDataDirectory() / L"launcher_frecency.json";
}

void LauncherFrecencyStore::ensureLoadedLocked() {
    if (m_loaded) return;
    m_loaded = true;
    const auto filePath = resolveStoragePathLocked();
    std::string raw;
    if (!readWholeFileUtf8Wide(filePath, raw)) {
        return;
    }
    try {
        const auto parsed = nlohmann::json::parse(raw);
        if (!parsed.is_object()) return;
        const auto& entries = parsed.contains("entries") && parsed["entries"].is_object()
            ? parsed["entries"]
            : parsed;
        for (const auto& [k, v] : entries.items()) {
            if (!v.is_object()) continue;
            const std::string norm = normalizeKey(k);
            if (norm.empty()) continue;
            FrecencyRecord rec;
            rec.runCount = v.value("runCount", 0u);
            rec.lastRunEpochMs = v.value("lastRunEpochMs", int64_t{0});
            if (rec.runCount > 0) {
                m_records.emplace(norm, rec);
            }
        }
        pruneLocked();
    } catch (...) {
        // 损坏文件静默忽略并将在下次写入时覆盖恢复
    }
}

void LauncherFrecencyStore::pruneLocked() {
    if (m_records.size() <= kMaxFrecencyRecords) {
        return;
    }
    std::vector<std::pair<std::string, FrecencyRecord>> items;
    items.reserve(m_records.size());
    for (const auto& [k, v] : m_records) {
        items.emplace_back(k, v);
    }
    const int64_t now = currentEpochMs();
    std::sort(items.begin(), items.end(), [now](const auto& a, const auto& b) {
        const double sa = computeScore(a.second, now);
        const double sb = computeScore(b.second, now);
        if (sa != sb) return sa > sb;
        return a.second.lastRunEpochMs > b.second.lastRunEpochMs;
    });
    items.resize(kMaxFrecencyRecords);
    m_records.clear();
    for (auto& [k, v] : items) {
        m_records.emplace(std::move(k), v);
    }
}

bool LauncherFrecencyStore::persistLocked() const {
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json entries = nlohmann::json::object();
    for (const auto& [k, v] : m_records) {
        entries[k] = {
            {"runCount", v.runCount},
            {"lastRunEpochMs", v.lastRunEpochMs}
        };
    }
    root["entries"] = std::move(entries);
    const auto filePath = resolveStoragePathLocked();
    return tools3000::common::atomicWriteFileWithFlush(filePath.wstring(), root.dump());
}

void LauncherFrecencyStore::recordRun(std::string_view rawKey, int64_t nowEpochMsOverride) {
    const std::string key = normalizeKey(rawKey);
    if (key.empty()) return;
    const int64_t now = nowEpochMsOverride > 0 ? nowEpochMsOverride : currentEpochMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    ensureLoadedLocked();
    auto& rec = m_records[key];
    if (rec.runCount < 1'000'000u) {
        ++rec.runCount;
    }
    rec.lastRunEpochMs = now;
    pruneLocked();
    m_dirty = !persistLocked();
}

FrecencyRecord LauncherFrecencyStore::lookup(std::string_view rawKey) {
    const std::string key = normalizeKey(rawKey);
    if (key.empty()) return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    ensureLoadedLocked();
    const auto it = m_records.find(key);
    return it != m_records.end() ? it->second : FrecencyRecord{};
}

double LauncherFrecencyStore::scoreFor(std::string_view rawKey, int64_t nowEpochMsOverride) {
    const auto rec = lookup(rawKey);
    const int64_t now = nowEpochMsOverride > 0 ? nowEpochMsOverride : currentEpochMs();
    return computeScore(rec, now);
}

void LauncherFrecencyStore::applyToResults(nlohmann::json& resultsArray, int64_t nowEpochMsOverride) {
    if (!resultsArray.is_array() || resultsArray.empty()) {
        return;
    }
    const int64_t now = nowEpochMsOverride > 0 ? nowEpochMsOverride : currentEpochMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    ensureLoadedLocked();
    for (auto& item : resultsArray) {
        if (!item.is_object()) continue;
        const std::string path = item.value("path", "");
        if (path.empty()) continue;
        const std::string key = normalizeKey(path);
        const auto it = m_records.find(key);
        uint32_t effectiveRunCount = item.value("runCount", 0u);
        double frecency = item.value("frecencyScore", 0.0);
        if (it != m_records.end()) {
            effectiveRunCount = (std::max)(effectiveRunCount, it->second.runCount);
            frecency = (std::max)(frecency, computeScore(it->second, now));
        } else if (effectiveRunCount > 0 && frecency <= 0.0) {
            FrecencyRecord synthetic{effectiveRunCount, now};
            frecency = computeScore(synthetic, now) * 0.5;
        }
        item["runCount"] = effectiveRunCount;
        item["frecencyScore"] = frecency;
    }
}

void LauncherFrecencyStore::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dirty) {
        if (persistLocked()) {
            m_dirty = false;
        }
    }
}

void LauncherFrecencyStore::clearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_dirty) {
        (void)persistLocked();
        m_dirty = false;
    }
    m_records.clear();
    m_loaded = false;
}

void LauncherFrecencyStore::setStoragePathForTesting(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_customStoragePath = path;
    m_records.clear();
    m_loaded = false;
    m_dirty = false;
}

void LauncherFrecencyStore::resetForTesting() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_records.clear();
    m_customStoragePath.clear();
    m_loaded = false;
    m_dirty = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3 档可配置结果优先级融合与去重引擎
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::json fuseSearchResults(
    const nlohmann::json& filesJson,
    const nlohmann::json& appsJson,
    const nlohmann::json& commandsJson,
    const nlohmann::json& bookmarksJson,
    const FusionOptions& options) {
    // 1. 收集并规范化启动项（系统命令 + 本地已安装应用）
    std::vector<nlohmann::json> launchers;
    std::unordered_map<std::string, size_t> launcherPathIdx;
    std::unordered_map<std::string, size_t> appStemIdx;

    auto ingestLaunchers = [&](const nlohmann::json& src, const char* defaultType) {
        if (!src.is_array()) return;
        for (const auto& rawItem : src) {
            if (!rawItem.is_object()) continue;
            nlohmann::json item = rawItem;
            if (!item.contains("resultType") || !item["resultType"].is_string()) {
                item["resultType"] = defaultType;
            }
            const std::string pathKey = normalizePathForDedup(item.value("path", ""));
            if (!pathKey.empty() && launcherPathIdx.find(pathKey) != launcherPathIdx.end()) {
                continue;
            }
            const size_t idx = launchers.size();
            if (!pathKey.empty()) {
                launcherPathIdx.emplace(pathKey, idx);
            }
            if (item.value("resultType", "") == "app") {
                std::string stem = item.value("dedupStem", "");
                if (stem.empty()) {
                    stem = stripKnownAppExtensionLower(item.value("name", ""));
                }
                if (!stem.empty()) {
                    appStemIdx.try_emplace(stem, idx);
                }
            }
            launchers.push_back(std::move(item));
        }
    };

    ingestLaunchers(commandsJson, "command");
    ingestLaunchers(appsJson, "app");

    // 2. 过滤服务端文件结果中的重复项（严格仅去重同路径或 StartMenu/AppsFolder 应用影子，绝不误删同名文档）
    std::vector<nlohmann::json> files;
    std::unordered_set<std::string> seenFilePaths;
    if (filesJson.is_array()) {
        files.reserve(filesJson.size());
        for (const auto& rawFile : filesJson) {
            if (!rawFile.is_object()) continue;
            const std::string pathKey = normalizePathForDedup(rawFile.value("path", ""));
            if (!pathKey.empty()) {
                auto lpIt = launcherPathIdx.find(pathKey);
                if (lpIt != launcherPathIdx.end()) {
                    auto& existing = launchers[lpIt->second];
                    const uint32_t mergedRuns = (std::max)(
                        existing.value("runCount", 0u), rawFile.value("runCount", 0u));
                    const double mergedFrecency = (std::max)(
                        existing.value("frecencyScore", 0.0), rawFile.value("frecencyScore", 0.0));
                    existing["runCount"] = mergedRuns;
                    existing["frecencyScore"] = mergedFrecency;
                    continue;
                }
                if (!seenFilePaths.insert(pathKey).second) {
                    continue;
                }
            }
            if (isServiceAppShadowCandidate(rawFile)) {
                const std::string stem = stripKnownAppExtensionLower(rawFile.value("name", ""));
                auto stemIt = appStemIdx.find(stem);
                if (stemIt != appStemIdx.end()) {
                    auto& existing = launchers[stemIt->second];
                    const uint32_t mergedRuns = (std::max)(
                        existing.value("runCount", 0u), rawFile.value("runCount", 0u));
                    const double mergedFrecency = (std::max)(
                        existing.value("frecencyScore", 0.0), rawFile.value("frecencyScore", 0.0));
                    existing["runCount"] = mergedRuns;
                    existing["frecencyScore"] = mergedFrecency;
                    continue;
                }
            }
            nlohmann::json item = rawFile;
            if (!item.contains("resultType") || !item["resultType"].is_string()) {
                item["resultType"] = "file";
            }
            files.push_back(std::move(item));
        }
    }

    // 3. 收集并去重书签结果
    std::vector<nlohmann::json> bookmarks;
    if (bookmarksJson.is_array()) {
        bookmarks.reserve(bookmarksJson.size());
        std::unordered_set<std::string> seenUrls;
        for (const auto& rawBm : bookmarksJson) {
            if (!rawBm.is_object()) continue;
            const std::string urlKey = toLowerAsciiAndUtf8(rawBm.value("path", ""));
            if (!urlKey.empty() && !seenUrls.insert(urlKey).second) {
                continue;
            }
            nlohmann::json item = rawBm;
            item["resultType"] = "bookmark";
            bookmarks.push_back(std::move(item));
        }
        std::stable_sort(bookmarks.begin(), bookmarks.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            const double fa = a.value("frecencyScore", 0.0);
            const double fb = b.value("frecencyScore", 0.0);
            if (fa != fb) return fa > fb;
            return a.value("matchRank", 0) > b.value("matchRank", 0);
        });
    }

    // 4. 启动项内部确定性排序（精确别名 -> 综合分 [launcherScore + frecencyScore] -> 命令优先 -> 名称字典序）
    auto launcherEffectiveScore = [](const nlohmann::json& item) -> double {
        const double base = static_cast<double>(
            item.value("launcherScore", item.value("matchRank", 0)));
        const double frecency = item.value("frecencyScore", 0.0);
        return base + frecency;
    };

    std::stable_sort(launchers.begin(), launchers.end(), [&](const nlohmann::json& a, const nlohmann::json& b) {
        const bool exactA = a.value("exactAliasMatch", false);
        const bool exactB = b.value("exactAliasMatch", false);
        if (exactA != exactB) return exactA > exactB;
        const double sa = launcherEffectiveScore(a);
        const double sb = launcherEffectiveScore(b);
        if (sa != sb) return sa > sb;
        const bool cmdA = a.value("resultType", "") == "command";
        const bool cmdB = b.value("resultType", "") == "command";
        if (cmdA != cmdB) return cmdA > cmdB;
        return toLowerAsciiAndUtf8(a.value("name", "")) < toLowerAsciiAndUtf8(b.value("name", ""));
    });

    // 5. 按 3 档配置切分顶栏与尾部启动项
    std::vector<nlohmann::json> topLaunchers;
    std::vector<nlohmann::json> tailLaunchers;

    if (options.profile == RankingProfile::LauncherFirst) {
        topLaunchers = std::move(launchers);
    } else if (options.profile == RankingProfile::FilesFirst) {
        for (auto& item : launchers) {
            const bool isExact = item.value("exactAliasMatch", false);
            if (isExact && topLaunchers.size() < options.balancedTopLauncherCap) {
                topLaunchers.push_back(std::move(item));
            } else {
                tailLaunchers.push_back(std::move(item));
            }
        }
    } else {
        // RankingProfile::BalancedFrecency（默认推荐）
        for (auto& item : launchers) {
            const bool isExact = item.value("exactAliasMatch", false);
            const int baseScore = item.value("launcherScore", item.value("matchRank", 0));
            const double frecency = item.value("frecencyScore", 0.0);
            const bool qualifiesForTop = isExact ||
                baseScore >= options.balancedScoreThreshold ||
                frecency >= options.balancedFrecencyThreshold;
            if (qualifiesForTop && topLaunchers.size() < options.balancedTopLauncherCap) {
                topLaunchers.push_back(std::move(item));
            } else {
                tailLaunchers.push_back(std::move(item));
            }
        }
    }

    // 6. 拼装最终结果（受 maxTotalResults 约束）
    nlohmann::json merged = nlohmann::json::array();
    const size_t limit = options.maxTotalResults == 0 ? 100u : options.maxTotalResults;
    auto pushCapped = [&](nlohmann::json&& item) {
        if (merged.size() < limit) {
            merged.push_back(std::move(item));
        }
    };

    for (auto& item : topLaunchers) {
        pushCapped(std::move(item));
    }

    if (options.profile == RankingProfile::FilesFirst) {
        for (auto& f : files) {
            pushCapped(std::move(f));
        }
        for (auto& item : tailLaunchers) {
            pushCapped(std::move(item));
        }
        for (auto& bm : bookmarks) {
            pushCapped(std::move(bm));
        }
    } else {
        const size_t leadFiles = (std::min)(files.size(), options.maxLeadFilesBeforeBookmarks);
        for (size_t i = 0; i < leadFiles; ++i) {
            pushCapped(std::move(files[i]));
        }
        for (auto& bm : bookmarks) {
            pushCapped(std::move(bm));
        }
        for (size_t i = leadFiles; i < files.size(); ++i) {
            pushCapped(std::move(files[i]));
        }
        for (auto& item : tailLaunchers) {
            pushCapped(std::move(item));
        }
    }

    return merged;
}

}  // namespace tools3000::search
