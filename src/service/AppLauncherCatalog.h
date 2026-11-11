#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "FileIndexStore.h"
#include "MftParser.h"
#include "PinyinEngine.h"
#include "PipeClientAuth.h"
#include "SearchExpression.h"
#include "db/RunHistoryManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tools3000::service::launcher {

// Minimum Frecency score required for a frequently/recently launched item
// (e.g., an executable under Program Files or a daily-used document) to promote
// across normal path tiers into the top launcher tier (`U-04`).
// Under RunHistoryManager's scoring (`runCount * 100 + recencyBonus`), items
// launched within 7 days (>= 500), >= 2 times within 30 days (>= 350), or
// >= 3 times historically (>= 320) qualify, while a stale single click (> 30d, 120) stays in its own tier.
inline constexpr double kLauncherPromotionFrecencyThreshold = 300.0;

namespace detail {

inline constexpr GUID kFolderIdAppsFolder = {
    0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}
};

inline constexpr GUID kBhidEnumItems = {
    0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}
};

inline wchar_t foldSlashAndLower(wchar_t ch) noexcept {
    if (ch == L'/') return L'\\';
    if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch + (L'a' - L'A'));
    if (ch < 0x80) return ch;
    return static_cast<wchar_t>(std::towlower(ch));
}

inline bool startsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (foldSlashAndLower(text[i]) != foldSlashAndLower(prefix[i])) return false;
    }
    return true;
}

inline bool endsWithLower(std::wstring_view lowerText, std::wstring_view lowerSuffix) noexcept {
    if (lowerText.size() < lowerSuffix.size()) return false;
    return lowerText.substr(lowerText.size() - lowerSuffix.size()) == lowerSuffix;
}

inline bool isLauncherShortcutNameLower(std::wstring_view lowerName) noexcept {
    return (lowerName.size() > 4 && (endsWithLower(lowerName, L".lnk") ||
                                     endsWithLower(lowerName, L".url"))) ||
           (lowerName.size() > 10 && endsWithLower(lowerName, L".appref-ms"));
}

inline bool isAnyShortcutNameLower(std::wstring_view lowerName) noexcept {
    return isLauncherShortcutNameLower(lowerName) ||
           (lowerName.size() > 9 && endsWithLower(lowerName, L".desklink"));
}

struct ScopedComInit {
    HRESULT hr = E_FAIL;
    bool shouldUninit = false;

    ScopedComInit() noexcept {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            shouldUninit = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            // Thread already initialized as MTA; COM calls remain valid without CoUninitialize.
            hr = S_OK;
        }
    }

    ~ScopedComInit() {
        if (shouldUninit) {
            CoUninitialize();
        }
    }

    bool ok() const noexcept { return SUCCEEDED(hr); }
};

inline bool containsCjkOrWideChars(std::wstring_view text) noexcept {
    for (wchar_t ch : text) {
        if (ch >= 0x80) return true;
    }
    return false;
}

inline bool matchesWildcardLower(std::wstring_view textLower, std::wstring_view patternLower) noexcept {
    size_t ti = 0;
    size_t pi = 0;
    size_t starIdx = std::wstring_view::npos;
    size_t matchIdx = 0;
    while (ti < textLower.size()) {
        if (pi < patternLower.size() && (patternLower[pi] == L'?' || patternLower[pi] == textLower[ti])) {
            ++ti;
            ++pi;
        } else if (pi < patternLower.size() && patternLower[pi] == L'*') {
            starIdx = pi++;
            matchIdx = ti;
        } else if (starIdx != std::wstring_view::npos) {
            pi = starIdx + 1;
            ti = ++matchIdx;
        } else {
            return false;
        }
    }
    while (pi < patternLower.size() && patternLower[pi] == L'*') {
        ++pi;
    }
    return pi == patternLower.size();
}

inline bool isExcludedByPatterns(
    std::wstring_view displayLower,
    std::wstring_view uriLower,
    const SearchExcludeOptions& excludeOpts
) {
    for (const auto& rawPat : excludeOpts.patterns) {
        if (rawPat.empty()) continue;
        std::wstring pat;
        pat.reserve(rawPat.size());
        bool hasWildcard = false;
        for (wchar_t ch : rawPat) {
            if (ch == L'*' || ch == L'?') hasWildcard = true;
            pat.push_back(foldSlashAndLower(ch));
        }
        if (hasWildcard) {
            if (matchesWildcardLower(displayLower, pat) || matchesWildcardLower(uriLower, pat)) {
                return true;
            }
        } else {
            if (uriLower.find(pat) != std::wstring_view::npos ||
                displayLower.find(pat) != std::wstring_view::npos) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace detail

inline bool isVirtualShellUri(std::wstring_view path) noexcept {
    return detail::startsWithIgnoreCase(path, L"shell:");
}

// Fallback classifier used only when `SearchResult::pathTier == kPathTierUncomputed`
// (for example, synthetic results in unit tests). Normal MFT search results already carry
// O(1) precomputed `pathTier` from `StoredFileRecord` (`P-02`).
inline uint8_t classifyPathTierFromFullPath(
    std::wstring_view fullPath,
    bool isDirectory,
    int matchRank = 0
) {
    if (detail::startsWithIgnoreCase(fullPath, L"shell:appsfolder\\")) {
        return (matchRank <= 6) ? kPathTierAppLauncher : kPathTierGenericShortcut;
    }

    wchar_t stackBuf[520];
    std::wstring heapBuf;
    wchar_t* buf = stackBuf;
    if (fullPath.size() > 512) {
        heapBuf.resize(fullPath.size());
        buf = heapBuf.data();
    }
    for (size_t i = 0; i < fullPath.size(); ++i) {
        buf[i] = detail::foldSlashAndLower(fullPath[i]);
    }
    const std::wstring_view lowerPath(buf, fullPath.size());

    const size_t lastSlash = lowerPath.rfind(L'\\');
    const std::wstring_view dirPart = (lastSlash != std::wstring_view::npos)
        ? lowerPath.substr(0, lastSlash + 1)
        : std::wstring_view{};
    const std::wstring_view namePart = (lastSlash != std::wstring_view::npos)
        ? lowerPath.substr(lastSlash + 1)
        : lowerPath;

    if (!isDirectory) {
        if (namePart.starts_with(L"~$") ||
            (namePart.size() > 4 && (detail::endsWithLower(namePart, L".tmp") ||
                                     detail::endsWithLower(namePart, L".bak"))) ||
            (namePart.size() > 5 && (detail::endsWithLower(namePart, L".temp") ||
                                     detail::endsWithLower(namePart, L".part"))) ||
            (namePart.size() > 11 && detail::endsWithLower(namePart, L".crdownload"))) {
            return kPathTierTempNoise;
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
        return kPathTierTempNoise;
    }

    if (!isDirectory && detail::isAnyShortcutNameLower(namePart)) {
        if (detail::isLauncherShortcutNameLower(namePart)) {
            const bool inStartMenu = (dirPart.find(L"\\start menu\\") != std::wstring_view::npos);
            const bool inUserDesktop = (dirPart.find(L"\\users\\") != std::wstring_view::npos) &&
                                       detail::endsWithLower(dirPart, L"\\desktop\\");
            if (inStartMenu || inUserDesktop) {
                return (matchRank <= 6) ? kPathTierAppLauncher : kPathTierGenericShortcut;
            }
        }
        return kPathTierGenericShortcut;
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
        return kPathTierChatHistory;
    }

    if (dirPart.find(L"\\appdata\\") != std::wstring_view::npos ||
        dirPart.find(L"\\programdata\\") != std::wstring_view::npos ||
        dirPart.find(L"\\program files\\") != std::wstring_view::npos ||
        dirPart.find(L"\\program files (x86)\\") != std::wstring_view::npos ||
        dirPart.find(L"\\windows\\") != std::wstring_view::npos) {
        return kPathTierSystemAppData;
    }

    return kPathTierPrimaryUser;
}

inline uint8_t resolveResultPathTier(const SearchResult& r) {
    if (r.pathTier != kPathTierUncomputed) {
        if (r.pathTier == kPathTierAppLauncher && r.matchRank >= 7) {
            return kPathTierGenericShortcut;
        }
        return r.pathTier;
    }
    return classifyPathTierFromFullPath(r.fullPath, r.isDirectory, r.matchRank);
}

inline std::wstring extractLauncherStemLower(std::wstring_view fileName, bool stripShortcutExt) {
    std::wstring lower;
    lower.reserve(fileName.size());
    for (wchar_t ch : fileName) {
        lower.push_back(detail::foldSlashAndLower(ch));
    }
    if (stripShortcutExt) {
        if (lower.size() > 4 && (detail::endsWithLower(lower, L".lnk") ||
                                 detail::endsWithLower(lower, L".url"))) {
            lower.resize(lower.size() - 4);
        } else if (lower.size() > 10 && detail::endsWithLower(lower, L".appref-ms")) {
            lower.resize(lower.size() - 10);
        }
    }
    while (!lower.empty() && std::iswspace(lower.back())) {
        lower.pop_back();
    }
    size_t lead = 0;
    while (lead < lower.size() && std::iswspace(lower[lead])) {
        ++lead;
    }
    if (lead > 0) {
        lower.erase(0, lead);
    }
    return lower;
}

// Sorts merged volume + AppsFolder results in O(N log N) without per-item lowercase allocations:
//   1. Start Menu\Programs & User/Public Desktop app shortcuts + AppsFolder UWP apps sit at
//      `effectiveTier = kPathTierAppLauncher (0)`.
//   2. Any non-noise item with `frecencyScore >= kLauncherPromotionFrecencyThreshold` and
//      filename/pinyin match (`matchRank <= 6`) also promotes to `effectiveTier = 0`, allowing
//      frequently clicked executables and daily files to surface directly at the top (`U-04`).
inline void sortLauncherAwareResults(
    std::vector<SearchResult>& results,
    const tools3000::service::db::RunHistorySnapshot& historySnap
) {
    if (results.size() <= 1) return;

    struct RankedItem {
        SearchResult result;
        uint8_t effectiveTier = kPathTierPrimaryUser;
        uint8_t baseTier = kPathTierPrimaryUser;
        double frecencyScore = 0.0;
    };

    std::vector<RankedItem> ranked;
    ranked.reserve(results.size());
    for (auto& item : results) {
        const uint8_t baseTier = resolveResultPathTier(item);
        const double frecency = historySnap.empty()
            ? 0.0
            : historySnap.lookup(item.fullPath).frecencyScore;
        uint8_t effectiveTier = baseTier;
        if (frecency >= kLauncherPromotionFrecencyThreshold &&
            baseTier < kPathTierTempNoise &&
            item.matchRank <= 6) {
            effectiveTier = kPathTierAppLauncher;
        }
        ranked.push_back(RankedItem{std::move(item), effectiveTier, baseTier, frecency});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedItem& a, const RankedItem& b) {
        if (a.effectiveTier != b.effectiveTier) return a.effectiveTier < b.effectiveTier;
        if (a.result.matchRank != b.result.matchRank) return a.result.matchRank < b.result.matchRank;
        if (a.frecencyScore != b.frecencyScore) return a.frecencyScore > b.frecencyScore;
        if (a.baseTier != b.baseTier) return a.baseTier < b.baseTier;
        if (a.result.folderPriority != b.result.folderPriority) {
            return a.result.folderPriority > b.result.folderPriority;
        }
        if (a.result.lastWriteTime != b.result.lastWriteTime) {
            return a.result.lastWriteTime > b.result.lastWriteTime;
        }
        if (a.result.fileName.size() != b.result.fileName.size()) {
            return a.result.fileName.size() < b.result.fileName.size();
        }
        const int cmp = _wcsicmp(a.result.fileName.c_str(), b.result.fileName.c_str());
        if (cmp != 0) return cmp < 0;
        return a.result.fileName < b.result.fileName;
    });

    results.clear();
    results.reserve(ranked.size());
    for (auto& entry : ranked) {
        results.push_back(std::move(entry.result));
    }
}

struct CachedAppEntry {
    std::wstring displayName;
    std::wstring normalizedName;
    std::wstring normalizedStem;
    std::wstring shellUri;
    std::wstring uriLower;
    std::wstring pinyinInitials;
    std::wstring pinyinFull;
};

// Per-user TTL cache enumerating `FOLDERID_AppsFolder` (`SHGetKnownFolderItem`)
// so modern UWP / MSIX / Windows system apps (Calculator, Settings, Microsoft Store, etc.)
// are seamlessly searchable alongside NTFS Start Menu & Desktop shortcuts (`U-04`).
class AppsFolderCatalog {
public:
    static AppsFolderCatalog& instance() {
        static AppsFolderCatalog s_catalog;
        return s_catalog;
    }

    void invalidate() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.clear();
    }

    std::vector<CachedAppEntry> getSnapshot(
        HANDLE hClientToken = nullptr,
        bool isSameUserProcessSid = true,
        std::wstring_view clientSid = {}
    ) {
        const std::wstring key = clientSid.empty() ? L"__default__" : std::wstring(clientSid);
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& session : m_sessions) {
                if (session.sidKey == key && (now - session.capturedAt) < kCacheTtl) {
                    return session.entries;
                }
            }
        }

        std::vector<CachedAppEntry> fresh = enumerateFromShell(
            hClientToken, isSameUserProcessSid, clientSid);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto refreshTime = std::chrono::steady_clock::now();
            bool updated = false;
            for (auto& session : m_sessions) {
                if (session.sidKey == key) {
                    session.entries = fresh;
                    session.capturedAt = refreshTime;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                if (m_sessions.size() >= kMaxCachedSessions) {
                    m_sessions.erase(m_sessions.begin());
                }
                m_sessions.push_back(SessionCache{key, fresh, refreshTime});
            }
        }
        return fresh;
    }

    std::vector<SearchResult> queryMatchingApps(
        const SearchExpression& expr,
        const SearchExcludeOptions& excludeOpts,
        const std::unordered_set<std::wstring>& existingLauncherStems,
        HANDLE hClientToken = nullptr,
        bool isSameUserProcessSid = true,
        std::wstring_view clientSid = {},
        size_t maxResults = 64
    ) {
        std::vector<SearchResult> matches;
        if (maxResults == 0 || expr.isEmpty() || expr.hasContentFilter()) {
            return matches;
        }

        const auto entries = getSnapshot(hClientToken, isSameUserProcessSid, clientSid);
        if (entries.empty()) {
            return matches;
        }

        matches.reserve((std::min)(maxResults, static_cast<size_t>(16)));
        for (const auto& app : entries) {
            if (!existingLauncherStems.empty() &&
                existingLauncherStems.find(app.normalizedStem) != existingLauncherStems.end()) {
                continue;
            }
            if (!excludeOpts.patterns.empty() &&
                detail::isExcludedByPatterns(app.normalizedName, app.uriLower, excludeOpts)) {
                continue;
            }

            const FileRecord rec{
                0,
                0,
                app.displayName,
                app.normalizedName,
                app.pinyinInitials,
                app.pinyinFull,
                false,
                0,
                0,
                0,
                0
            };

            if (!expr.matchesWithLazyPath(
                    rec,
                    L'\0',
                    [&app]() -> const std::wstring& { return app.shellUri; })) {
                continue;
            }

            const int rank = expr.calculateRank(rec);
            SearchResult res;
            res.fileName = app.displayName;
            res.fullPath = app.shellUri;
            res.isDirectory = false;
            res.fileSize = 0;
            res.creationTime = 0;
            res.lastWriteTime = 0;
            res.matchRank = rank;
            res.folderPriority = 4700;
            res.pathTier = (rank <= 6) ? kPathTierAppLauncher : kPathTierGenericShortcut;
            matches.push_back(std::move(res));

            if (matches.size() >= maxResults) {
                break;
            }
        }
        return matches;
    }

private:
    static constexpr auto kCacheTtl = std::chrono::seconds(120);
    static constexpr size_t kMaxCachedSessions = 8;

    struct SessionCache {
        std::wstring sidKey;
        std::vector<CachedAppEntry> entries;
        std::chrono::steady_clock::time_point capturedAt{};
    };

    static std::vector<CachedAppEntry> enumerateFromShell(
        HANDLE hClientToken,
        bool isSameUserProcessSid,
        std::wstring_view clientSid
    ) {
        std::vector<CachedAppEntry> result;

        tools3000::service::pipe_auth::ScopedContentReadImpersonation impGuard(
            hClientToken, isSameUserProcessSid, clientSid);
        if (!impGuard.isAuthorized()) {
            return result;
        }

        detail::ScopedComInit comGuard;
        if (!comGuard.ok()) {
            return result;
        }

        IShellItem* appsFolder = nullptr;
        HRESULT hr = SHGetKnownFolderItem(
            detail::kFolderIdAppsFolder,
            KF_FLAG_DONT_VERIFY,
            hClientToken,
            __uuidof(IShellItem),
            reinterpret_cast<void**>(&appsFolder));
        if (FAILED(hr) && hClientToken != nullptr) {
            hr = SHGetKnownFolderItem(
                detail::kFolderIdAppsFolder,
                KF_FLAG_DONT_VERIFY,
                nullptr,
                __uuidof(IShellItem),
                reinterpret_cast<void**>(&appsFolder));
        }
        if (FAILED(hr) || !appsFolder) {
            return result;
        }

        IEnumShellItems* enumItems = nullptr;
        hr = appsFolder->BindToHandler(
            nullptr,
            detail::kBhidEnumItems,
            __uuidof(IEnumShellItems),
            reinterpret_cast<void**>(&enumItems));
        appsFolder->Release();
        if (FAILED(hr) || !enumItems) {
            return result;
        }

        std::unordered_set<std::wstring> seenUris;
        result.reserve(128);

        IShellItem* item = nullptr;
        ULONG fetched = 0;
        while (enumItems->Next(1, &item, &fetched) == S_OK && item != nullptr) {
            PWSTR rawDisplay = nullptr;
            PWSTR rawParsing = nullptr;
            HRESULT hrName = item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawDisplay);
            HRESULT hrParse = item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &rawParsing);
            if (FAILED(hrParse) || !rawParsing || rawParsing[0] == L'\0') {
                if (rawParsing) {
                    CoTaskMemFree(rawParsing);
                    rawParsing = nullptr;
                }
                hrParse = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &rawParsing);
            }
            item->Release();
            item = nullptr;

            std::wstring displayName = (SUCCEEDED(hrName) && rawDisplay) ? rawDisplay : L"";
            std::wstring parsingName = (SUCCEEDED(hrParse) && rawParsing) ? rawParsing : L"";
            if (rawDisplay) CoTaskMemFree(rawDisplay);
            if (rawParsing) CoTaskMemFree(rawParsing);

            while (!displayName.empty() && std::iswspace(displayName.back())) {
                displayName.pop_back();
            }
            if (displayName.empty() || parsingName.empty()) {
                continue;
            }

            // Skip raw filesystem paths (`C:\...` or `\\server\...`) already indexed by MFT.
            if ((parsingName.size() >= 3 && std::iswalpha(parsingName[0]) && parsingName[1] == L':' &&
                 (parsingName[2] == L'\\' || parsingName[2] == L'/')) ||
                parsingName.starts_with(L"\\\\")) {
                continue;
            }

            // Strip any leading shell namespace CLSID prefix (`::{...}\`) if fallback returned it.
            std::wstring_view appIdView = parsingName;
            if (appIdView.starts_with(L"::{")) {
                const size_t closePos = appIdView.find(L"}\\");
                if (closePos != std::wstring_view::npos) {
                    appIdView = appIdView.substr(closePos + 2);
                }
            }
            if (appIdView.empty()) {
                continue;
            }

            std::wstring shellUri;
            if (detail::startsWithIgnoreCase(appIdView, L"shell:")) {
                shellUri.assign(appIdView);
            } else {
                shellUri.reserve(17 + appIdView.size());
                shellUri.assign(L"shell:AppsFolder\\");
                shellUri.append(appIdView);
            }

            std::wstring uriLower;
            uriLower.reserve(shellUri.size());
            for (wchar_t ch : shellUri) {
                uriLower.push_back(detail::foldSlashAndLower(ch));
            }
            if (!seenUris.insert(uriLower).second) {
                continue;
            }

            CachedAppEntry entry;
            entry.displayName = std::move(displayName);
            entry.normalizedName.reserve(entry.displayName.size());
            for (wchar_t ch : entry.displayName) {
                entry.normalizedName.push_back(
                    ch < 0x80 ? ((ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + 32) : ch)
                              : static_cast<wchar_t>(std::towlower(ch)));
            }
            entry.normalizedStem = extractLauncherStemLower(entry.displayName, false);
            entry.shellUri = std::move(shellUri);
            entry.uriLower = std::move(uriLower);

            if (detail::containsCjkOrWideChars(entry.displayName)) {
                entry.pinyinInitials = PinyinEngine::GetInitials(entry.displayName);
                entry.pinyinFull = PinyinEngine::GetFullPinyin(entry.displayName);
            }

            result.push_back(std::move(entry));
        }

        enumItems->Release();
        return result;
    }

    std::mutex m_mutex;
    std::vector<SessionCache> m_sessions;
};

}  // namespace tools3000::service::launcher
