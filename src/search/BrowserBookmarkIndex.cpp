// ─────────────────────────────────────────────────────────────────────────────
// BrowserBookmarkIndex.cpp — 零依赖离线浏览器书签极速索引实现
// ─────────────────────────────────────────────────────────────────────────────

#include "search/BrowserBookmarkIndex.h"

#include "core/utils/WinUtils.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <unordered_set>

namespace tools3000::search {

namespace {

constexpr size_t kMaxBookmarkFileBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxTotalBookmarks = 8000u;
constexpr int kStatThrottleSeconds = 10;

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return out;
}

std::string_view trimView(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return sv;
}

std::vector<std::string> splitTermsLower(std::string_view query) {
    std::vector<std::string> terms;
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        if (i >= query.size()) break;
        const size_t start = i;
        while (i < query.size() && !std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        terms.push_back(toLowerAscii(query.substr(start, i - start)));
    }
    return terms;
}

uint64_t parseChromiumDateMicros(const nlohmann::json& node) noexcept {
    if (!node.is_object()) return 0;
    auto it = node.find("date_added");
    if (it == node.end()) return 0;
    if (it->is_string()) {
        const auto& s = it->get_ref<const std::string&>();
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return 0;
            v = v * 10u + static_cast<uint64_t>(c - '0');
        }
        return v;
    }
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer() && it->get<int64_t>() > 0) {
        return static_cast<uint64_t>(it->get<int64_t>());
    }
    return 0;
}

void walkChromiumNode(
    const nlohmann::json& node,
    const std::string& folderPrefix,
    std::string_view browserSource,
    std::unordered_set<std::string>& seenUrls,
    std::vector<BookmarkEntry>& out,
    size_t maxEntries,
    int depth) {
    if (depth > 32 || out.size() >= maxEntries || !node.is_object()) return;
    const std::string type = node.value("type", "");
    if (type == "url") {
        const std::string url = node.value("url", "");
        if (!BrowserBookmarkIndex::isSafeHttpUrl(url)) return;
        const std::string urlLower = toLowerAscii(url);
        if (!seenUrls.insert(urlLower).second) return;

        std::string title = node.value("name", "");
        if (title.empty()) title = url;

        BookmarkEntry entry;
        entry.title = std::move(title);
        entry.url = url;
        entry.folderPath = folderPrefix;
        entry.browserSource = std::string(browserSource);
        entry.titleLower = toLowerAscii(entry.title);
        entry.urlLower = urlLower;
        entry.folderLower = toLowerAscii(entry.folderPath);
        entry.dateAddedMicros = parseChromiumDateMicros(node);
        out.push_back(std::move(entry));
        return;
    }

    const auto childrenIt = node.find("children");
    if (childrenIt == node.end() || !childrenIt->is_array()) return;

    const std::string nodeName = node.value("name", "");
    std::string nextFolder = folderPrefix;
    if (!nodeName.empty()) {
        if (!nextFolder.empty()) nextFolder += " / ";
        nextFolder += nodeName;
    }

    for (const auto& child : *childrenIt) {
        if (out.size() >= maxEntries) break;
        walkChromiumNode(child, nextFolder, browserSource, seenUrls, out, maxEntries, depth + 1);
    }
}

std::filesystem::path getLocalAppDataPath() {
    wchar_t path[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
        return std::filesystem::path(path);
    }
    return {};
}

}  // namespace

BrowserBookmarkIndex& BrowserBookmarkIndex::instance() {
    static BrowserBookmarkIndex inst;
    return inst;
}

bool BrowserBookmarkIndex::isSafeHttpUrl(std::string_view url) noexcept {
    if (url.size() < 10 || url.size() > 2048) return false;
    auto starts = [&](std::string_view prefix) {
        if (url.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            const char c = (url[i] >= 'A' && url[i] <= 'Z') ? static_cast<char>(url[i] + 32) : url[i];
            if (c != prefix[i]) return false;
        }
        return true;
    };
    if (!starts("https://") && !starts("http://")) return false;
    for (unsigned char c : url) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

BookmarkQueryParseResult BrowserBookmarkIndex::parseBookmarkQuery(std::string_view rawQuery) {
    const std::string_view trimmed = trimView(rawQuery);
    if (trimmed.size() >= 3 &&
        (trimmed[0] == 'b' || trimmed[0] == 'B') &&
        (trimmed[1] == 'm' || trimmed[1] == 'M') &&
        trimmed[2] == ':') {
        return {true, std::string(trimView(trimmed.substr(3)))};
    }
    if (trimmed.size() >= 2 &&
        (trimmed[0] == 'b' || trimmed[0] == 'B') &&
        trimmed[1] == ':') {
        // 严格放行 B 盘路径（b:\... / b:/...），避免抢占真实盘符文件搜索
        if (trimmed.size() >= 3 && (trimmed[2] == '\\' || trimmed[2] == '/')) {
            return {false, std::string(trimmed)};
        }
        return {true, std::string(trimView(trimmed.substr(2)))};
    }
    return {false, std::string(trimmed)};
}

bool BrowserBookmarkIndex::shouldMixBookmarksInGeneralQuery(std::string_view rawQuery) noexcept {
    const std::string_view trimmed = trimView(rawQuery);
    if (trimmed.size() < 2) return false;

    // 若包含显式盘符路径（如 C:\）或绝对路径分隔符，说明用户明确找本地文件，绝不混入网页书签
    if (trimmed.find('\\') != std::string_view::npos) return false;
    if (trimmed.size() >= 2 && std::isalpha(static_cast<unsigned char>(trimmed[0])) && trimmed[1] == ':') {
        return false;
    }

    // 排除所有显式文件搜索操作符与专用前缀，做到 100% 零语法冲突
    static constexpr std::array<std::string_view, 21> kFileOps = {
        "content:", "c:", "ext:", "path:", "parent:", "p:", "file:", "folder:", "dir:",
        "size:", "dm:", "date:", "modified:", "regex:", "r:", "case:", "cs:", "exact:",
        "app:", "cmd:", "bm:"
    };
    const std::string low = toLowerAscii(trimmed);
    if (low.find("内容:") != std::string::npos) return false;
    for (const auto& op : kFileOps) {
        const size_t pos = low.find(op);
        if (pos != std::string::npos && (pos == 0 || std::isspace(static_cast<unsigned char>(low[pos - 1])))) {
            return false;
        }
    }
    // 排除纯通配符查询（如 *.png）
    if (trimmed.find('*') != std::string_view::npos || trimmed.find('?') != std::string_view::npos) {
        return false;
    }
    return true;
}

void BrowserBookmarkIndex::parseChromiumBookmarkJson(
    const nlohmann::json& root,
    std::string_view browserSource,
    std::vector<BookmarkEntry>& out,
    size_t maxEntries) {
    if (!root.is_object()) return;
    const auto rootsIt = root.find("roots");
    if (rootsIt == root.end() || !rootsIt->is_object()) return;

    std::unordered_set<std::string> seenUrls;
    seenUrls.reserve(out.size() + 256);
    for (const auto& existing : out) {
        seenUrls.insert(existing.urlLower);
    }

    for (const auto& [key, subtree] : rootsIt->items()) {
        if (out.size() >= maxEntries) break;
        if (key == "sync_transaction_version") continue;
        walkChromiumNode(subtree, "", browserSource, seenUrls, out, maxEntries, 0);
    }
}

std::vector<BrowserBookmarkIndex::TrackedBookmarkSource> BrowserBookmarkIndex::discoverLocalBookmarkFiles() {
    std::vector<TrackedBookmarkSource> sources;
    const auto localAppData = getLocalAppDataPath();
    if (localAppData.empty()) return sources;

    struct BrowserDef {
        const wchar_t* relUserData;
        const char* displayName;
    };
    static constexpr std::array<BrowserDef, 5> kBrowsers = {{
        {L"Microsoft\\Edge\\User Data", "Edge"},
        {L"Google\\Chrome\\User Data", "Chrome"},
        {L"BraveSoftware\\Brave-Browser\\User Data", "Brave"},
        {L"Vivaldi\\User Data", "Vivaldi"},
        {L"Chromium\\User Data", "Chromium"},
    }};

    std::error_code ec;
    for (const auto& b : kBrowsers) {
        const auto userDataDir = localAppData / b.relUserData;
        if (!std::filesystem::is_directory(userDataDir, ec)) continue;

        auto tryAddProfile = [&](const std::filesystem::path& profileDir) {
            const auto bmFile = profileDir / L"Bookmarks";
            if (std::filesystem::is_regular_file(bmFile, ec)) {
                sources.push_back({bmFile, b.displayName});
            }
        };

        tryAddProfile(userDataDir / L"Default");
        for (int i = 1; i <= 5; ++i) {
            tryAddProfile(userDataDir / (L"Profile " + std::to_wstring(i)));
        }
    }
    return sources;
}

void BrowserBookmarkIndex::refreshIfNeededLocked() {
    const auto now = std::chrono::steady_clock::now();
    if (m_loadedOnce &&
        std::chrono::duration_cast<std::chrono::seconds>(now - m_lastCheckTime).count() < kStatThrottleSeconds) {
        return;
    }
    m_lastCheckTime = now;

    const auto sources = discoverLocalBookmarkFiles();
    bool anyChanged = !m_loadedOnce || (sources.size() != m_fileMtimes.size());
    std::unordered_map<std::wstring, std::filesystem::file_time_type> nextMtimes;
    nextMtimes.reserve(sources.size());

    std::error_code ec;
    for (const auto& src : sources) {
        const auto mtime = std::filesystem::last_write_time(src.filePath, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const std::wstring key = src.filePath.wstring();
        nextMtimes.emplace(key, mtime);
        const auto prev = m_fileMtimes.find(key);
        if (prev == m_fileMtimes.end() || prev->second != mtime) {
            anyChanged = true;
        }
    }

    if (!anyChanged) return;

    std::vector<BookmarkEntry> fresh;
    fresh.reserve(512);
    for (const auto& src : sources) {
        if (fresh.size() >= kMaxTotalBookmarks) break;
        const auto sz = std::filesystem::file_size(src.filePath, ec);
        if (ec || sz == 0 || sz > kMaxBookmarkFileBytes) {
            ec.clear();
            continue;
        }
        std::ifstream ifs(src.filePath, std::ios::binary);
        if (!ifs) continue;
        try {
            auto parsed = nlohmann::json::parse(ifs, nullptr, false);
            if (!parsed.is_discarded()) {
                parseChromiumBookmarkJson(parsed, src.browserName, fresh, kMaxTotalBookmarks);
            }
        } catch (...) {
            // 忽略单个损坏或正在写入的浏览器书签文件，绝不崩溃
        }
    }

    m_entries = std::move(fresh);
    m_fileMtimes = std::move(nextMtimes);
    m_loadedOnce = true;
}

nlohmann::json BrowserBookmarkIndex::searchEntriesToJson(
    const std::vector<BookmarkEntry>& entries,
    std::string_view query,
    size_t limit,
    int minScore) {
    const auto terms = splitTermsLower(query);
    const size_t cap = limit == 0 ? 50u : (std::min<size_t>)(limit, 200u);

    struct ScoredBookmark {
        const BookmarkEntry* entry;
        int score;
    };
    std::vector<ScoredBookmark> hits;
    hits.reserve((std::min<size_t>)(entries.size(), 256u));

    for (const auto& item : entries) {
        if (terms.empty()) {
            hits.push_back({&item, 50});
            continue;
        }

        int totalScore = 0;
        bool allMatched = true;
        for (const auto& term : terms) {
            int termScore = 0;
            if (item.titleLower == term) {
                termScore = 120;
            } else if (item.titleLower.starts_with(term)) {
                termScore = 95;
            } else if (item.titleLower.find(term) != std::string::npos) {
                termScore = 75;
            } else if (item.urlLower.find(term) != std::string::npos) {
                termScore = 55;
            } else if (item.folderLower.find(term) != std::string::npos) {
                termScore = 30;
            }

            if (termScore == 0) {
                allMatched = false;
                break;
            }
            totalScore += termScore;
        }

        if (allMatched && totalScore >= minScore) {
            hits.push_back({&item, totalScore});
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const ScoredBookmark& a, const ScoredBookmark& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.entry->dateAddedMicros > b.entry->dateAddedMicros;
    });

    if (hits.size() > cap) {
        hits.resize(cap);
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& hit : hits) {
        const auto& e = *hit.entry;
        // Chromium WebKit 1601 微秒 * 10 = Windows FILETIME 100ns 刻度，天然兼容前端时间格式化器
        const uint64_t fileTime = e.dateAddedMicros > 0 ? e.dateAddedMicros * 10u : 0u;
        arr.push_back({
            {"name", e.title},
            {"path", e.url},
            {"isDirectory", false},
            {"resultType", "bookmark"},
            {"browserSource", e.browserSource},
            {"bookmarkFolder", e.folderPath},
            {"creationTime", fileTime},
            {"lastWriteTime", fileTime},
            {"matchRank", hit.score}
        });
    }
    return arr;
}

nlohmann::json BrowserBookmarkIndex::searchLocalBookmarks(std::string_view query, size_t limit, int minScore) {
    std::lock_guard<std::mutex> lock(m_mutex);
    refreshIfNeededLocked();
    return searchEntriesToJson(m_entries, query, limit, minScore);
}

void BrowserBookmarkIndex::clearCache() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_entries.shrink_to_fit();
        m_fileMtimes.clear();
        m_loadedOnce = false;
    }
    {
        std::lock_guard<std::mutex> flock(m_faviconMutex);
        m_faviconCache.clear();
    }
}

namespace {

constexpr size_t kMaxFaviconCacheEntries = 512u;
constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteOpenReadonly = 0x00000001;
constexpr int kSqliteOpenUri = 0x00000040;

struct WinSqlite3Api {
    using OpenV2Fn = int(__stdcall*)(const char*, void**, int, const char*);
    using CloseFn = int(__stdcall*)(void*);
    using PrepareV2Fn = int(__stdcall*)(void*, const char*, int, void**, const char**);
    using BindTextFn = int(__stdcall*)(void*, int, const char*, int, void(*)(void*));
    using StepFn = int(__stdcall*)(void*);
    using ColumnBlobFn = const void*(__stdcall*)(void*, int);
    using ColumnBytesFn = int(__stdcall*)(void*, int);
    using FinalizeFn = int(__stdcall*)(void*);

    HMODULE module = nullptr;
    OpenV2Fn openV2 = nullptr;
    CloseFn close = nullptr;
    PrepareV2Fn prepareV2 = nullptr;
    BindTextFn bindText = nullptr;
    StepFn step = nullptr;
    ColumnBlobFn columnBlob = nullptr;
    ColumnBytesFn columnBytes = nullptr;
    FinalizeFn finalize = nullptr;
    bool ready = false;

    static const WinSqlite3Api& instance() noexcept {
        static const WinSqlite3Api s_api = []() {
            WinSqlite3Api api;
            api.module = LoadLibraryW(L"winsqlite3.dll");
            if (!api.module) return api;
            api.openV2 = reinterpret_cast<OpenV2Fn>(GetProcAddress(api.module, "sqlite3_open_v2"));
            api.close = reinterpret_cast<CloseFn>(GetProcAddress(api.module, "sqlite3_close"));
            api.prepareV2 = reinterpret_cast<PrepareV2Fn>(GetProcAddress(api.module, "sqlite3_prepare_v2"));
            api.bindText = reinterpret_cast<BindTextFn>(GetProcAddress(api.module, "sqlite3_bind_text"));
            api.step = reinterpret_cast<StepFn>(GetProcAddress(api.module, "sqlite3_step"));
            api.columnBlob = reinterpret_cast<ColumnBlobFn>(GetProcAddress(api.module, "sqlite3_column_blob"));
            api.columnBytes = reinterpret_cast<ColumnBytesFn>(GetProcAddress(api.module, "sqlite3_column_bytes"));
            api.finalize = reinterpret_cast<FinalizeFn>(GetProcAddress(api.module, "sqlite3_finalize"));
            api.ready = api.openV2 && api.close && api.prepareV2 && api.bindText &&
                        api.step && api.columnBlob && api.columnBytes && api.finalize;
            return api;
        }();
        return s_api;
    }
};

std::string buildSqliteFileUri(const std::filesystem::path& dbPath, const char* queryFlags) {
    const std::string utf8 = tools3000::core::WinUtils::pathToUtf8(dbPath);
    std::string encoded;
    encoded.reserve(utf8.size() + 32);
    encoded.append("file:///");
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char ch : utf8) {
        if (ch == '\\') {
            encoded.push_back('/');
        } else if (ch == '?' || ch == '#' || ch == '%' || ch == ' ') {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0F]);
            encoded.push_back(kHex[ch & 0x0F]);
        } else {
            encoded.push_back(static_cast<char>(ch));
        }
    }
    if (queryFlags && queryFlags[0] != '\0') {
        encoded.push_back('?');
        encoded.append(queryFlags);
    }
    return encoded;
}

const char* detectImageMimeFromBytes(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 4) return nullptr;
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "image/png";
    }
    if (size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return "image/webp";
    }
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "image/jpeg";
    }
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00) {
        return "image/x-icon";
    }
    if (size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8') {
        return "image/gif";
    }
    return nullptr;
}

std::string queryFaviconBlobFromDb(const std::filesystem::path& dbPath,
                                   const std::string& exactUrl,
                                   const std::vector<std::string>& originPrefixes) {
    const auto& sql = WinSqlite3Api::instance();
    if (!sql.ready) return {};

    auto tryOpen = [&](const char* flags) -> void* {
        void* db = nullptr;
        const std::string uri = buildSqliteFileUri(dbPath, flags);
        if (sql.openV2(uri.c_str(), &db, kSqliteOpenReadonly | kSqliteOpenUri, nullptr) == kSqliteOk && db) {
            return db;
        }
        if (db) sql.close(db);
        return nullptr;
    };

    void* db = tryOpen("mode=ro&nolock=1");
    if (!db) {
        db = tryOpen("mode=ro&immutable=1");
    }
    if (!db) return {};

    struct DbGuard {
        const WinSqlite3Api& api;
        void* handle;
        ~DbGuard() { if (handle) api.close(handle); }
    } dbGuard{sql, db};

    auto runBlobQuery = [&](const char* querySql,
                            const std::string& bind1,
                            const std::string* bind2) -> std::string {
        void* stmt = nullptr;
        if (sql.prepareV2(db, querySql, -1, &stmt, nullptr) != kSqliteOk || !stmt) {
            return {};
        }
        struct StmtGuard {
            const WinSqlite3Api& api;
            void* handle;
            ~StmtGuard() { if (handle) api.finalize(handle); }
        } stmtGuard{sql, stmt};

        sql.bindText(stmt, 1, bind1.c_str(), static_cast<int>(bind1.size()), nullptr);
        if (bind2) {
            sql.bindText(stmt, 2, bind2->c_str(), static_cast<int>(bind2->size()), nullptr);
        }
        if (sql.step(stmt) != kSqliteRow) {
            return {};
        }
        const void* blob = sql.columnBlob(stmt, 0);
        const int blobBytes = sql.columnBytes(stmt, 0);
        if (!blob || blobBytes < 4 || blobBytes > 256 * 1024) {
            return {};
        }
        const auto* bytes = static_cast<const uint8_t*>(blob);
        const char* mime = detectImageMimeFromBytes(bytes, static_cast<size_t>(blobBytes));
        if (!mime) {
            return {};
        }
        std::vector<uint8_t> buf(bytes, bytes + blobBytes);
        return std::string("data:") + mime + ";base64," + tools3000::core::WinUtils::base64Encode(buf);
    };

    // 1. 精确 URL 命中（利用 icon_mapping_page_url_idx 索引，<0.1ms）
    static constexpr const char* kExactSql =
        "SELECT fb.image_data FROM icon_mapping im "
        "JOIN favicon_bitmaps fb ON im.icon_id = fb.icon_id "
        "WHERE im.page_url = ?1 AND fb.image_data IS NOT NULL "
        "ORDER BY CASE WHEN fb.width >= 32 THEN 0 ELSE 1 END, fb.width DESC LIMIT 1";
    if (!exactUrl.empty()) {
        std::string hit = runBlobQuery(kExactSql, exactUrl, nullptr);
        if (!hit.empty()) return hit;
    }

    // 2. Origin / 根域名前缀 B-Tree 范围命中（ASCII '/' + 1 == '0'，纯索引范围扫描 <0.1ms）
    static constexpr const char* kPrefixSql =
        "SELECT fb.image_data FROM icon_mapping im "
        "JOIN favicon_bitmaps fb ON im.icon_id = fb.icon_id "
        "WHERE im.page_url >= ?1 AND im.page_url < ?2 AND fb.image_data IS NOT NULL "
        "ORDER BY CASE WHEN fb.width >= 32 THEN 0 ELSE 1 END, fb.width DESC LIMIT 1";
    for (const auto& prefix : originPrefixes) {
        if (prefix.size() < 10 || prefix.back() != '/') continue;
        std::string upperBound = prefix;
        upperBound.back() = static_cast<char>('/' + 1); // '0'
        std::string hit = runBlobQuery(kPrefixSql, prefix, &upperBound);
        if (!hit.empty()) return hit;
    }
    return {};
}

std::wstring queryAppPathExeForBrowser(const wchar_t* exeName) {
    if (!exeName || !exeName[0]) return {};
    const std::wstring subKey = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exeName;
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(hive, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS || !hKey) {
            continue;
        }
        wchar_t buf[1024]{};
        DWORD bytes = sizeof(buf);
        DWORD type = 0;
        const LONG res = RegQueryValueExW(hKey, nullptr, nullptr, &type,
                                          reinterpret_cast<LPBYTE>(buf), &bytes);
        RegCloseKey(hKey);
        if (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0') {
            std::wstring path(buf);
            if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
                path = path.substr(1, path.size() - 2);
            }
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return path;
            }
        }
    }
    return {};
}

std::string resolveBrowserFallbackNativeIcon(std::string_view browserSource) {
    const std::string lower = toLowerAscii(browserSource);
    const wchar_t* exeName = nullptr;
    if (lower == "chrome") exeName = L"chrome.exe";
    else if (lower == "edge") exeName = L"msedge.exe";
    else if (lower == "brave") exeName = L"brave.exe";
    else if (lower == "vivaldi") exeName = L"vivaldi.exe";
    else if (lower == "firefox") exeName = L"firefox.exe";

    if (exeName) {
        const std::wstring exePath = queryAppPathExeForBrowser(exeName);
        if (!exePath.empty()) {
            return tools3000::core::WinUtils::getFileTypeIconBase64(L".exe", false, exePath);
        }
    }
    return tools3000::core::WinUtils::getFileTypeIconBase64(L".url", false);
}

std::vector<std::string> buildOriginFallbackPrefixes(const std::string& originLower) {
    std::vector<std::string> prefixes;
    if (originLower.empty()) return prefixes;
    prefixes.push_back(originLower);

    // 若为子域名（如 https://zhuanlan.zhihu.com/），补充尝试 https://www.zhihu.com/ 与 https://zhihu.com/
    const size_t schemeEnd = originLower.find("://");
    if (schemeEnd == std::string::npos) return prefixes;
    const std::string scheme = originLower.substr(0, schemeEnd + 3);
    std::string host = originLower.substr(schemeEnd + 3);
    if (!host.empty() && host.back() == '/') host.pop_back();
    const size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) host.resize(colonPos);

    const size_t firstDot = host.find('.');
    if (firstDot != std::string::npos) {
        const std::string parentHost = host.substr(firstDot + 1);
        if (parentHost.find('.') != std::string::npos) {
            if (host.starts_with("www.")) {
                prefixes.push_back(scheme + parentHost + "/");
            } else {
                prefixes.push_back(scheme + "www." + parentHost + "/");
                prefixes.push_back(scheme + parentHost + "/");
            }
        } else if (!host.starts_with("www.")) {
            prefixes.push_back(scheme + "www." + host + "/");
        }
    }
    return prefixes;
}

}  // namespace

std::string BrowserBookmarkIndex::extractOriginLower(std::string_view url) {
    if (!isSafeHttpUrl(url)) return {};
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos) return {};
    const size_t hostStart = schemeEnd + 3;
    const size_t pathStart = url.find_first_of("/?#", hostStart);
    const std::string_view originPart = (pathStart == std::string_view::npos)
        ? url
        : url.substr(0, pathStart);
    if (originPart.size() <= hostStart) return {};
    std::string out = toLowerAscii(originPart);
    out.push_back('/');
    return out;
}

std::string BrowserBookmarkIndex::getBookmarkIconBase64(std::string_view url,
                                                        std::string_view browserSource) {
    if (!isSafeHttpUrl(url)) {
        return resolveBrowserFallbackNativeIcon(browserSource);
    }

    const std::string urlStr(url);
    const std::string originLower = extractOriginLower(url);
    const std::string cacheKey = !originLower.empty() ? originLower : toLowerAscii(url);

    {
        std::lock_guard<std::mutex> lock(m_faviconMutex);
        auto it = m_faviconCache.find(cacheKey);
        if (it != m_faviconCache.end()) {
            return it->second;
        }
    }

    const auto sources = discoverLocalBookmarkFiles();
    std::vector<std::filesystem::path> faviconDbs;
    faviconDbs.reserve(sources.size());

    const std::string preferredBrowser = toLowerAscii(browserSource);
    std::error_code ec;
    // 优先检索与该书签归属浏览器一致的 Favicons 库，其次检索其余本机浏览器
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& src : sources) {
            const bool isPreferred = !preferredBrowser.empty() &&
                                     toLowerAscii(src.browserName) == preferredBrowser;
            if ((pass == 0 && !isPreferred) || (pass == 1 && isPreferred)) {
                continue;
            }
            const auto favPath = src.filePath.parent_path() / L"Favicons";
            if (std::filesystem::is_regular_file(favPath, ec)) {
                faviconDbs.push_back(favPath);
            } else {
                ec.clear();
            }
        }
    }

    const auto originPrefixes = buildOriginFallbackPrefixes(originLower);
    std::string iconDataUri;
    for (const auto& dbPath : faviconDbs) {
        iconDataUri = queryFaviconBlobFromDb(dbPath, urlStr, originPrefixes);
        if (!iconDataUri.empty()) {
            break;
        }
    }

    if (iconDataUri.empty()) {
        iconDataUri = resolveBrowserFallbackNativeIcon(browserSource);
    }

    if (!iconDataUri.empty()) {
        std::lock_guard<std::mutex> lock(m_faviconMutex);
        if (m_faviconCache.size() >= kMaxFaviconCacheEntries) {
            m_faviconCache.clear();
        }
        m_faviconCache[cacheKey] = iconDataUri;
    }
    return iconDataUri;
}

}  // namespace tools3000::search
