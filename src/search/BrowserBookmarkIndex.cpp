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

    // 排除所有显式文件搜索操作符，做到 100% 零语法冲突
    static constexpr std::array<std::string_view, 18> kFileOps = {
        "content:", "c:", "ext:", "path:", "parent:", "p:", "file:", "folder:", "dir:",
        "size:", "dm:", "date:", "modified:", "regex:", "r:", "case:", "cs:", "exact:"
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
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_entries.shrink_to_fit();
    m_fileMtimes.clear();
    m_loadedOnce = false;
}

}  // namespace tools3000::search
