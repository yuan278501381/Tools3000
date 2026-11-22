#include "MftParser.h"
#include "PinyinEngine.h"
#include "QueryThreadPool.h"
#include "content/ContentSearchEngine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <cwctype>
#include <unordered_set>


namespace {

std::wstring normalize(std::wstring_view value) {
    return SearchExpression::normalize(value);
}

uint64_t fileTimeToUint64(const FILETIME& value) noexcept {
    ULARGE_INTEGER raw{};
    raw.LowPart = value.dwLowDateTime;
    raw.HighPart = value.dwHighDateTime;
    return raw.QuadPart;
}

enum class HydrateResult {
    Valid,
    RejectedRecycleBin,
    GhostNotFound
};

// FSCTL_ENUM_USN_DATA is deliberately fast, but its records do not contain a
// file's size or creation time; USN_RECORD::TimeStamp is also the journal event
// time rather than the authoritative last-write time. Hydrate only the small
// final result set so the index stays compact while every displayed property is
// accurate.
//
// Real-time verification: if GetFileAttributesExW indicates the file physically
// does not exist on disk (ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND), return
// GhostNotFound so the caller can drop the phantom node and prune it from the store.
HydrateResult hydrateResultProperties(SearchResult& result, bool allowRecycleBin = false, bool allowRecentShadow = false) {
    if (result.fullPath.empty()) {
        return HydrateResult::GhostNotFound;
    }
    if (!allowRecycleBin || !allowRecentShadow) {
        std::wstring lowerPath = normalize(result.fullPath);
        if (!allowRecycleBin &&
            (lowerPath.find(L"\\$recycle.bin") != std::wstring::npos ||
             lowerPath.find(L"/$recycle.bin") != std::wstring::npos)) {
            return HydrateResult::RejectedRecycleBin;
        }
        if (!allowRecentShadow &&
            (lowerPath.find(L"\\microsoft\\windows\\recent\\") != std::wstring::npos ||
             lowerPath.find(L"/microsoft/windows/recent/") != std::wstring::npos ||
             lowerPath.find(L"\\microsoft\\office\\recent\\") != std::wstring::npos ||
             lowerPath.find(L"/microsoft/office/recent/") != std::wstring::npos)) {
            return HydrateResult::RejectedRecycleBin;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(result.fullPath.c_str(), GetFileExInfoStandard, &data)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_INVALID_NAME || err == ERROR_BAD_PATHNAME ||
            err == ERROR_DIRECTORY || err == ERROR_NOT_READY ||
            err == ERROR_INVALID_DRIVE) {
            return HydrateResult::GhostNotFound;
        }
        return HydrateResult::Valid;
    }

    const uint64_t preservedDirSize = result.fileSize;
    result.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.fileAttributes = data.dwFileAttributes;
    result.creationTime = fileTimeToUint64(data.ftCreationTime);
    result.lastWriteTime = fileTimeToUint64(data.ftLastWriteTime);
    result.lastAccessTime = fileTimeToUint64(data.ftLastAccessTime);
    result.fileSize = result.isDirectory
        ? preservedDirSize
        : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return HydrateResult::Valid;
}

double computeExtensionSignalOffset(
    std::wstring_view extView,
    std::wstring_view normFileName,
    bool explicitLock
) noexcept {
    if (!explicitLock &&
        (normFileName == L"package-lock.json" ||
         normFileName == L"pnpm-lock.yaml" ||
         normFileName == L"yarn.lock" ||
         normFileName == L"cargo.lock" ||
         normFileName == L"composer.lock" ||
         normFileName == L"poetry.lock" ||
         normFileName == L"uv.lock")) {
        return -350.0;
    }

    if (extView.empty() || extView.size() > 16) return 0.0;
    wchar_t lowerExt[16];
    for (size_t i = 0; i < extView.size(); ++i) {
        lowerExt[i] = static_cast<wchar_t>(std::towlower(extView[i]));
    }
    const std::wstring_view ext(lowerExt, extView.size());

    if (!explicitLock && ext == L"lock") {
        return -350.0;
    }
    if (ext == L"md" || ext == L"markdown" || ext == L"txt" ||
        ext == L"docx" || ext == L"doc" || ext == L"xlsx" || ext == L"xls" ||
        ext == L"pptx" || ext == L"ppt" || ext == L"pdf" || ext == L"rtf" ||
        ext == L"sql" || ext == L"py" || ext == L"ts" || ext == L"tsx" ||
        ext == L"js" || ext == L"jsx" || ext == L"cpp" || ext == L"c" ||
        ext == L"h" || ext == L"hpp" || ext == L"cs" || ext == L"java" ||
        ext == L"go" || ext == L"rs" || ext == L"vue" || ext == L"svelte" ||
        ext == L"ch" || ext == L"pld" || ext == L"ps1" || ext == L"sh" || ext == L"lua") {
        return 90.0;
    }
    if (ext == L"yaml" || ext == L"yml" || ext == L"toml" || ext == L"ini" ||
        ext == L"conf" || ext == L"config" || ext == L"json" || ext == L"jsonc" ||
        ext == L"xml") {
        return 35.0;
    }
    if (ext == L"html" || ext == L"htm" || ext == L"log" || ext == L"csv" || ext == L"tsv") {
        return -60.0;
    }
    return 0.0;
}

}  // namespace

std::vector<SearchResult> MftParser::Search(const std::wstring& query, int limit,
                                           const SearchExcludeOptions& excludeOpts,
                                           const CancellationCheck& isCancelled,
                                           const SearchParseOptions& parseOpts) {
    m_LastSearchPathBuildCount.store(0, std::memory_order_relaxed);
    std::vector<SearchResult> results;
    if (query.empty() || limit <= 0 || (isCancelled && isCancelled())) return results;

    const std::wstring lowerQuery = normalize(query);
    const std::wstring cacheQueryKey = parseOpts.caseSensitive ? query : lowerQuery;
    const auto expr = SearchExpression::parse(query, parseOpts);
    const bool queryExplicitRecycle = lowerQuery.find(L"recycle") != std::wstring::npos;
    const bool queryExplicitRecent = lowerQuery.find(L"recent") != std::wstring::npos ||
                                     lowerQuery.find(L"lnk") != std::wstring::npos;
    const auto searchAllowedExts = expr.hasContentFilter()
        ? tools3000::service::content::ContentSearchEngine::instance().buildAllowedExtensionSnapshot()
        : tools3000::service::content::AllowedExtensionSnapshot{};

    struct HydratedStat {
        uint64_t frn = 0;
        uint64_t fileSize = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
    };

    auto eqAsciiLower = [](std::wstring_view s, std::wstring_view lowerLit) noexcept -> bool {
        if (s.size() != lowerLit.size()) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            wchar_t ch = s[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            if (ch != lowerLit[i]) return false;
        }
        return true;
    };

    auto containsLower = [](std::wstring_view haystack, std::wstring_view lowerNeedle) noexcept -> bool {
        if (lowerNeedle.empty()) return true;
        if (haystack.size() < lowerNeedle.size()) return false;
        const size_t maxPos = haystack.size() - lowerNeedle.size();
        const wchar_t first = lowerNeedle[0];
        for (size_t i = 0; i <= maxPos; ++i) {
            wchar_t ch = haystack[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            else if (ch >= 0x80 && (ch < 0x2000 || ch > 0x9FFF)) ch = static_cast<wchar_t>(std::towlower(ch));
            if (ch == first) {
                bool ok = true;
                for (size_t j = 1; j < lowerNeedle.size(); ++j) {
                    wchar_t cj = haystack[i + j];
                    if (cj >= L'A' && cj <= L'Z') cj = static_cast<wchar_t>(cj - L'A' + L'a');
                    else if (cj >= 0x80 && (cj < 0x2000 || cj > 0x9FFF)) cj = static_cast<wchar_t>(std::towlower(cj));
                    if (cj != lowerNeedle[j]) {
                        ok = false;
                        break;
                    }
                }
                if (ok) return true;
            }
        }
        return false;
    };

    std::vector<std::wstring> lowerExcludePatterns;
    bool allExcludePatternsSingleSegment = true;
    lowerExcludePatterns.reserve(excludeOpts.patterns.size());
    for (const auto& pat : excludeOpts.patterns) {
        if (pat.empty()) continue;
        std::wstring lp = normalize(pat);
        if (lp.find(L'\\') != std::wstring::npos || lp.find(L'/') != std::wstring::npos) {
            allExcludePatternsSingleSegment = false;
        }
        lowerExcludePatterns.push_back(std::move(lp));
    }

    struct DirExcludeCacheMeta {
        bool excluded = false;
        uint8_t dirClassFlags = 0;
        uint8_t dirStateBits = 0;
        int32_t folderPriority = 200;
    };

    const wchar_t driveW = static_cast<wchar_t>(m_DriveLetter);
    const uint64_t rootClauseMask = expr.computeDirectorySegmentMask(std::wstring_view(&driveW, 1));
    const int32_t baseDrivePriority = (m_DriveLetter != 'C' && m_DriveLetter != 'c') ? 800 : 200;

    std::shared_lock lock(m_MapMutex);
    const uint64_t generation = m_IndexGeneration.load(std::memory_order_acquire);

    const bool hasSeparableClauses = expr.hasSeparablePathGroups();
    struct FlatMaskSlot {
        DWORDLONG frn = UINT64_MAX;
        uint64_t mask = 0;
    };
    constexpr size_t kFlatMaskCacheSize = 4096;
    constexpr size_t kFlatMaskCacheMask = kFlatMaskCacheSize - 1;
    auto hashFrnSlot = [](DWORDLONG frn) noexcept -> size_t {
        uint64_t x = frn;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x) & kFlatMaskCacheMask;
    };

    // `LOW-07`: Allow deep directory ancestor chains up to 512 levels with explicit cycle
    // detection so deeply nested folders (>64 levels) never escape blacklist/exclusion rules.
    constexpr size_t kMaxAncestorChainDepth = 512;

    auto getFastDirClauseMask = [&](DWORDLONG parentFrn,
                                    DWORDLONG& lastMaskParentFrn,
                                    uint64_t& lastDirMask,
                                    std::vector<FlatMaskSlot>& maskCache) -> uint64_t {
        if (!hasSeparableClauses) {
            return 0ULL;
        }
        if (parentFrn == 0) {
            return rootClauseMask;
        }
        if (parentFrn == lastMaskParentFrn) {
            return lastDirMask;
        }
        if (maskCache.empty()) {
            maskCache.resize(kFlatMaskCacheSize);
        }
        const size_t topSlot = hashFrnSlot(parentFrn);
        if (maskCache[topSlot].frn == parentFrn) {
            lastMaskParentFrn = parentFrn;
            lastDirMask = maskCache[topSlot].mask;
            return lastDirMask;
        }

        DWORDLONG chain[kMaxAncestorChainDepth];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        uint64_t baseMask = rootClauseMask;

        while (cur != 0 && chainLen < kMaxAncestorChainDepth) {
            const size_t s = hashFrnSlot(cur);
            if (maskCache[s].frn == cur) {
                baseMask = maskCache[s].mask;
                break;
            }
            chain[chainLen++] = cur;
            const auto* node = m_Store.find(cur);
            if (!node) {
                break;
            }
            const FileRecord dirRec = m_Store.view(*node);
            const DWORDLONG nextParent = dirRec.parentFileReferenceNumber;
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                nextParent == cur || nextParent == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            if (std::find(chain, chain + chainLen, nextParent) != (chain + chainLen)) {
                break;
            }
            cur = nextParent;
        }

        for (size_t idx = chainLen; idx > 0; --idx) {
            const DWORDLONG stepFrn = chain[idx - 1];
            const auto* node = m_Store.find(stepFrn);
            if (node) {
                const FileRecord dirRec = m_Store.view(*node);
                if (!dirRec.fileName.empty() && dirRec.fileName != L".") {
                    baseMask |= expr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                }
            }
            maskCache[hashFrnSlot(stepFrn)] = FlatMaskSlot{stepFrn, baseMask};
        }
        lastMaskParentFrn = parentFrn;
        lastDirMask = baseMask;
        return baseMask;
    };

    auto isAncestorExcludedByPattern = [&](DWORDLONG parentFrn,
                                           DWORDLONG& lastExParentFrn,
                                           bool& lastExResult,
                                           std::unordered_map<DWORDLONG, bool>& exCache) -> bool {
        if (lowerExcludePatterns.empty() || parentFrn == 0) {
            return false;
        }
        if (parentFrn == lastExParentFrn) {
            return lastExResult;
        }
        auto it = exCache.find(parentFrn);
        if (it != exCache.end()) {
            lastExParentFrn = parentFrn;
            lastExResult = it->second;
            return lastExResult;
        }

        DWORDLONG chain[kMaxAncestorChainDepth];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        bool excluded = false;
        while (cur != 0 && chainLen < kMaxAncestorChainDepth) {
            auto cIt = exCache.find(cur);
            if (cIt != exCache.end()) {
                excluded = cIt->second;
                break;
            }
            chain[chainLen++] = cur;
            const auto* node = m_Store.find(cur);
            if (!node) break;
            const FileRecord dirRec = m_Store.view(*node);
            const DWORDLONG nextParent = dirRec.parentFileReferenceNumber;
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                nextParent == cur || nextParent == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            if (std::find(chain, chain + chainLen, nextParent) != (chain + chainLen)) {
                break;
            }
            cur = nextParent;
        }

        for (size_t idx = chainLen; idx > 0; --idx) {
            const DWORDLONG stepFrn = chain[idx - 1];
            if (!excluded) {
                const auto* node = m_Store.find(stepFrn);
                if (node) {
                    const FileRecord dirRec = m_Store.view(*node);
                    for (const auto& lp : lowerExcludePatterns) {
                        if (lp.find(L'\\') == std::wstring::npos && lp.find(L'/') == std::wstring::npos) {
                            if (containsLower(dirRec.fileName, lp)) {
                                excluded = true;
                                break;
                            }
                        }
                    }
                }
            }
            exCache.emplace(stepFrn, excluded);
        }
        lastExParentFrn = parentFrn;
        lastExResult = excluded;
        return excluded;
    };

    auto resolveFallbackTier = [&](const tools3000::service::StoredFileRecord& stored,
                                   const FileRecord& record,
                                   uint8_t& outDirClassFlags,
                                   uint8_t& outPathTier,
                                   int& outPriority) {
        if (stored.isTierComputed()) {
            outDirClassFlags = stored.dirClassFlags;
            outPathTier = stored.pathTier;
            outPriority = stored.folderPriority;
            return;
        }
        // Fallback when an incremental orphan node has not yet had hierarchy metadata computed.
        uint8_t parentClassFlags = 0;
        uint8_t parentStateBits = 0;
        int32_t parentPriority = baseDrivePriority;
        if (record.parentFileReferenceNumber != 0) {
            const auto* pNode = m_Store.find(record.parentFileReferenceNumber);
            if (pNode != nullptr) {
                parentClassFlags = pNode->dirClassFlags;
                parentStateBits = pNode->dirStateBits;
                parentPriority = pNode->folderPriority;
            }
        }
        outDirClassFlags = parentClassFlags;
        int32_t computedPrio = parentPriority;
        tools3000::service::computeLeafTierAndPriority(
            record.fileName,
            record.isDirectory,
            record.parentFileReferenceNumber,
            parentClassFlags,
            parentStateBits,
            parentPriority,
            outPathTier,
            computedPrio);
        outPriority = computedPrio;
    };

    enum class CandidateEvalResult : uint8_t {
        Rejected = 0,
        MatchedInMemory = 1,
        NeedsLockFreeStat = 2,
    };

    struct RankedCandidate {
        DWORDLONG id = 0;
        FileRecord record;
        int rank = 7;
        int folderPriority = 200;
        uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
    };

    struct PendingStatCandidate {
        DWORDLONG id = 0;
        uint64_t dirMask = 0;
        std::wstring fileName;
        std::wstring normalizedName;
        std::wstring pinyinInitials;
        std::wstring pinyinFull;
        std::wstring fullPath;
        bool isDirectory = false;
        uint32_t fileAttributes = 0;
        uint64_t fileSize = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
        int folderPriority = 200;
        uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
    };

    auto testCandidate = [&](DWORDLONG id,
                             const tools3000::service::StoredFileRecord& stored,
                             const FileRecord& record,
                             uint64_t dirClauseMask,
                             DWORDLONG& lastExParentFrn,
                             bool& lastExResult,
                             std::unordered_map<DWORDLONG, bool>& localExCache,
                             std::wstring& outBuiltPath,
                             int& outPriority,
                             uint8_t& outPathTier) -> CandidateEvalResult {
        if (excludeOpts.excludeHidden && (record.fileAttributes & FILE_ATTRIBUTE_HIDDEN)) {
            return CandidateEvalResult::Rejected;
        }
        if (excludeOpts.excludeSystem && (record.fileAttributes & FILE_ATTRIBUTE_SYSTEM)) {
            return CandidateEvalResult::Rejected;
        }

        // 1. Prune directories and unsupported extensions early for content queries.
        if (expr.hasContentFilter()) {
            if (record.isDirectory) return CandidateEvalResult::Rejected;
            const size_t dotPos = record.fileName.rfind(L'.');
            if (dotPos == std::wstring::npos) return CandidateEvalResult::Rejected;
            const std::wstring_view ext = std::wstring_view(record.fileName).substr(dotPos + 1);
            if (!searchAllowedExts.contains(ext)) {
                return CandidateEvalResult::Rejected;
            }
        }

        bool hasLazyPath = false;
        auto getPath = [&]() -> const std::wstring& {
            if (!hasLazyPath) {
                outBuiltPath = buildFullPath(id);
                hasLazyPath = true;
            }
            return outBuiltPath;
        };

        // 2. Lock-free in-memory evaluation (`P-02` & `U-05`): ZERO disk I/O under `m_MapMutex`,
        // and slash path fragments (`A\B\C`) short-circuit via ancestor bitmask + leaf prefix.
        SearchExpression::InMemoryMatchStatus matchStatus = SearchExpression::InMemoryMatchStatus::Matched;
        if (expr.isFullySeparable()) {
            if (!expr.matchesSeparable(record, driveW, dirClauseMask)) {
                return CandidateEvalResult::Rejected;
            }
        } else {
            matchStatus = expr.evaluateInMemoryWithDirMask(
                record, driveW, dirClauseMask, stored.isMetadataHydrated(), getPath);
            if (matchStatus == SearchExpression::InMemoryMatchStatus::Rejected) {
                return CandidateEvalResult::Rejected;
            }
        }

        // 3. Consume precomputed `dirClassFlags`, `pathTier`, and `folderPriority` in O(1) (`P-02`).
        uint8_t dirClassFlags = 0;
        resolveFallbackTier(stored, record, dirClassFlags, outPathTier, outPriority);

        if (expr.hasContentFilter() &&
            (dirClassFlags & tools3000::service::kDirClassContentSysPruned) != 0) {
            return CandidateEvalResult::Rejected;
        }
        if (!queryExplicitRecycle) {
            if ((dirClassFlags & tools3000::service::kDirClassRecycleBin) != 0 ||
                (record.isDirectory && eqAsciiLower(record.fileName, L"$recycle.bin"))) {
                return CandidateEvalResult::Rejected;
            }
        }
        if (!queryExplicitRecent &&
            (dirClassFlags & tools3000::service::kDirClassRecentDir) != 0) {
            return CandidateEvalResult::Rejected;
        }
        if (!lowerExcludePatterns.empty()) {
            if (isAncestorExcludedByPattern(record.parentFileReferenceNumber, lastExParentFrn, lastExResult, localExCache)) {
                return CandidateEvalResult::Rejected;
            }
            if (allExcludePatternsSingleSegment) {
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (containsLower(record.fileName, lowerPat)) {
                        return CandidateEvalResult::Rejected;
                    }
                }
            } else {
                const std::wstring& p = getPath();
                const std::wstring lowerP = normalize(p);
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (lowerP.find(lowerPat) != std::wstring::npos ||
                        containsLower(record.fileName, lowerPat)) {
                        return CandidateEvalResult::Rejected;
                    }
                }
            }
        }

        if (matchStatus == SearchExpression::InMemoryMatchStatus::NeedsDiskStat) {
            (void)getPath();
            return CandidateEvalResult::NeedsLockFreeStat;
        }
        return CandidateEvalResult::MatchedInMemory;
    };

    auto effectiveSingleDriveTier = [](uint8_t rawTier, int rank) noexcept -> uint8_t {
        if (rawTier == tools3000::service::kPathTierAppLauncher && rank >= 7) {
            return tools3000::service::kPathTierGenericShortcut;
        }
        return rawTier;
    };

    std::vector<DWORDLONG> candidates;
    bool canNarrow = false;
    {
        std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
        if (generation == m_CachedGeneration && !m_CachedQuery.empty() &&
            !expr.requiresFullPath() &&
            excludeOpts.excludeHidden == m_CachedExcludeHidden &&
            excludeOpts.excludeSystem == m_CachedExcludeSystem &&
            excludeOpts.patterns == m_CachedExcludePatterns &&
            parseOpts == m_CachedParseOptions &&
            cacheQueryKey.rfind(m_CachedQuery, 0) == 0 &&
            cacheQueryKey.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos &&
            m_CachedQuery.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos) {
            candidates = m_CachedCandidates;
            canNarrow = true;
        }
    }

    std::vector<RankedCandidate> ranked;
    std::vector<PendingStatCandidate> pendingStatCandidates;
    std::vector<HydratedStat> hydratedStats;

    // `HIGH-14`: Check cancellation at high frequency (every 64 items for regex queries,
    // every 256 items otherwise) so `m_MapMutex` shared lock is promptly released.
    const size_t cancelMask = expr.hasRegexClause() ? 0x3Fu : 0xFFu;

    auto appendPendingStat = [](std::vector<PendingStatCandidate>& vec,
                                DWORDLONG id,
                                uint64_t dirMask,
                                const FileRecord& record,
                                std::wstring&& fullPath,
                                int fPriority,
                                uint8_t pTier) {
        if (vec.size() >= 65536) {
            return;
        }
        const bool sameInitials = (record.pinyinInitials.data() == record.fileName.data());
        const bool sameFull = (record.pinyinFull.data() == record.fileName.data());
        PendingStatCandidate item;
        item.id = id;
        item.dirMask = dirMask;
        item.fileName = std::wstring(record.fileName);
        item.normalizedName = std::wstring(record.normalizedName);
        if (!sameInitials) {
            item.pinyinInitials = std::wstring(record.pinyinInitials);
        }
        if (!sameFull) {
            item.pinyinFull = std::wstring(record.pinyinFull);
        }
        item.fullPath = std::move(fullPath);
        item.isDirectory = record.isDirectory;
        item.fileAttributes = record.fileAttributes;
        item.fileSize = record.fileSize;
        item.creationTime = record.creationTime;
        item.lastWriteTime = record.lastWriteTime;
        item.folderPriority = fPriority;
        item.pathTier = pTier;
        vec.push_back(std::move(item));
    };

    if (canNarrow) {
        std::vector<DWORDLONG> filtered;
        filtered.reserve(std::min<size_t>(candidates.size(), 65536));
        ranked.reserve(std::min<size_t>(candidates.size(), 65536));
        std::vector<FlatMaskSlot> flatMaskCache;
        DWORDLONG lastMaskParentFrn = UINT64_MAX;
        uint64_t lastDirClauseMask = 0;
        std::unordered_map<DWORDLONG, bool> localExCache;
        DWORDLONG lastExParentFrn = UINT64_MAX;
        bool lastExResult = false;
        std::wstring scratchPath;
        size_t checked = 0;
        for (const auto id : candidates) {
            if (((checked++ & cancelMask) == 0) && isCancelled && isCancelled()) return results;
            const auto* stored = m_Store.find(id);
            if (!stored) continue;
            try {
                const FileRecord record = m_Store.view(*stored);
                const uint64_t dirMask = getFastDirClauseMask(
                    record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                int fPriority = 0;
                uint8_t pTier = tools3000::service::kPathTierPrimaryUser;
                scratchPath.clear();
                const CandidateEvalResult eval = testCandidate(
                    id, *stored, record, dirMask, lastExParentFrn, lastExResult, localExCache, scratchPath, fPriority, pTier);
                if (eval == CandidateEvalResult::MatchedInMemory) {
                    filtered.push_back(id);
                    const int rnk = expr.calculateRank(record);
                    ranked.push_back({id, record, rnk, fPriority, effectiveSingleDriveTier(pTier, rnk)});
                } else if (eval == CandidateEvalResult::NeedsLockFreeStat) {
                    appendPendingStat(pendingStatCandidates, id, dirMask, record, std::move(scratchPath), fPriority, pTier);
                }
            } catch (...) {
            }
        }
        candidates = std::move(filtered);
    } else {
        const size_t totalRecords = m_Store.slotCount();
        const unsigned int hwThreads = std::thread::hardware_concurrency();
        unsigned int numThreads = (hwThreads <= 2) ? (std::max)(1u, hwThreads) : (std::min)(hwThreads, 16u);
        if (totalRecords < 10000) numThreads = 1;

        const size_t chunkSize = (totalRecords + numThreads - 1) / numThreads;
        unsigned int activeThreads = 0;
        for (unsigned int t = 0; t < numThreads; ++t) {
            if (t * chunkSize < totalRecords) {
                ++activeThreads;
            }
        }

        std::vector<std::vector<DWORDLONG>> threadCandidates(activeThreads);
        std::vector<std::vector<RankedCandidate>> threadRanked(activeThreads);
        std::vector<std::vector<PendingStatCandidate>> threadPendingStat(activeThreads);
        std::vector<std::vector<FlatMaskSlot>> threadFlatMaskCaches(activeThreads);

        const bool useOptimisticDirPreFilter = expr.isFullySeparable() && hasSeparableClauses;
        const uint64_t negMask = expr.getNegatedSeparableGroupMask();
        uint64_t maximalDirMasks[8] = {rootClauseMask & ~negMask};
        size_t maximalDirMaskCount = 1;

        // Pass 1 (optional): compute directory mask antichain across pooled workers without barriers.
        if (useOptimisticDirPreFilter && activeThreads > 0) {
            std::vector<std::vector<uint64_t>> threadDirMasks(activeThreads);
            tools3000::service::QueryThreadPool::instance().parallelFor(activeThreads, [&](size_t t) {
                const size_t startIdx = t * chunkSize;
                const size_t endIdx = (std::min)(startIdx + chunkSize, totalRecords);
                auto& flatMaskCache = threadFlatMaskCaches[t];
                DWORDLONG lastMaskParentFrn = UINT64_MAX;
                uint64_t lastDirClauseMask = 0;
                auto& localMasks = threadDirMasks[t];
                for (size_t i = startIdx; i < endIdx; ++i) {
                    if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) break;
                    const auto* stored = m_Store.at(i);
                    if (!stored || !stored->isDirectory()) continue;
                    const FileRecord dirRec = m_Store.view(*stored);
                    if (dirRec.fileName.empty() || dirRec.fileName == L".") continue;
                    const uint64_t segMask = expr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                    if (segMask != 0) {
                        const uint64_t parentMask = getFastDirClauseMask(
                            dirRec.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                        const uint64_t rawFullMask = parentMask | segMask;
                        if ((rawFullMask & negMask) == 0 && rawFullMask != 0 &&
                            std::find(localMasks.begin(), localMasks.end(), rawFullMask) == localMasks.end()) {
                            localMasks.push_back(rawFullMask);
                        }
                    }
                }
            });

            if (isCancelled && isCancelled()) return results;

            std::vector<uint64_t> allMasks;
            if ((rootClauseMask & negMask) == 0) {
                allMasks.push_back(rootClauseMask);
            }
            for (const auto& tm : threadDirMasks) {
                for (uint64_t m : tm) {
                    if (std::find(allMasks.begin(), allMasks.end(), m) == allMasks.end()) {
                        allMasks.push_back(m);
                    }
                }
            }
            std::vector<uint64_t> antichain;
            for (size_t i = 0; i < allMasks.size(); ++i) {
                bool dominated = false;
                for (size_t j = 0; j < allMasks.size(); ++j) {
                    if (i != j && (allMasks[i] & allMasks[j]) == allMasks[i] &&
                        allMasks[i] != allMasks[j]) {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated) {
                    antichain.push_back(allMasks[i]);
                }
            }
            if (antichain.empty()) {
                maximalDirMasks[0] = rootClauseMask & ~negMask;
                maximalDirMaskCount = 1;
            } else if (antichain.size() <= 8) {
                maximalDirMaskCount = antichain.size();
                for (size_t k = 0; k < maximalDirMaskCount; ++k) {
                    maximalDirMasks[k] = antichain[k];
                }
            } else {
                uint64_t unionMask = 0;
                for (uint64_t m : antichain) unionMask |= m;
                maximalDirMasks[0] = unionMask;
                maximalDirMaskCount = 1;
            }
        }

        // Pass 2: scan index shards across reusable `QueryThreadPool` (`P-02`).
        if (activeThreads > 0) {
            tools3000::service::QueryThreadPool::instance().parallelFor(activeThreads, [&](size_t t) {
                const size_t startIdx = t * chunkSize;
                const size_t endIdx = (std::min)(startIdx + chunkSize, totalRecords);
                auto& flatMaskCache = threadFlatMaskCaches[t];
                DWORDLONG lastMaskParentFrn = UINT64_MAX;
                uint64_t lastDirClauseMask = 0;
                auto& localCand = threadCandidates[t];
                auto& localRank = threadRanked[t];
                auto& localPendingStat = threadPendingStat[t];
                localCand.reserve(256);
                localRank.reserve(256);
                std::unordered_map<DWORDLONG, bool> localExCache;
                DWORDLONG lastExParentFrn = UINT64_MAX;
                bool lastExResult = false;
                std::wstring scratchPath;

                for (size_t i = startIdx; i < endIdx; ++i) {
                    if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) break;
                    const auto* stored = m_Store.at(i);
                    if (!stored) continue;
                    const FileRecord record = m_Store.view(*stored);
                    if (useOptimisticDirPreFilter) {
                        bool possible = false;
                        for (size_t m = 0; m < maximalDirMaskCount; ++m) {
                            if (expr.matchesSeparable(record, driveW, maximalDirMasks[m])) {
                                possible = true;
                                break;
                            }
                        }
                        if (!possible) {
                            continue;
                        }
                    }
                    const DWORDLONG id = record.fileReferenceNumber;
                    const uint64_t dirMask = getFastDirClauseMask(
                        record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                    int fPriority = 0;
                    uint8_t pTier = tools3000::service::kPathTierPrimaryUser;
                    scratchPath.clear();
                    const CandidateEvalResult eval = testCandidate(
                        id, *stored, record, dirMask, lastExParentFrn, lastExResult, localExCache, scratchPath, fPriority, pTier);
                    if (eval == CandidateEvalResult::MatchedInMemory) {
                        localCand.push_back(id);
                        const int rnk = expr.calculateRank(record);
                        localRank.push_back({id, record, rnk, fPriority, effectiveSingleDriveTier(pTier, rnk)});
                    } else if (eval == CandidateEvalResult::NeedsLockFreeStat) {
                        appendPendingStat(localPendingStat, id, dirMask, record, std::move(scratchPath), fPriority, pTier);
                    }
                }
            });
        }

        if (isCancelled && isCancelled()) return results;

        size_t totalFound = 0;
        size_t totalPendingStat = 0;
        for (unsigned int t = 0; t < activeThreads; ++t) {
            totalFound += threadCandidates[t].size();
            totalPendingStat += threadPendingStat[t].size();
        }

        candidates.reserve(totalFound);
        ranked.reserve(totalFound);
        pendingStatCandidates.reserve(totalPendingStat);
        for (unsigned int t = 0; t < activeThreads; ++t) {
            candidates.insert(candidates.end(),
                              std::make_move_iterator(threadCandidates[t].begin()),
                              std::make_move_iterator(threadCandidates[t].end()));
            ranked.insert(ranked.end(),
                          std::make_move_iterator(threadRanked[t].begin()),
                          std::make_move_iterator(threadRanked[t].end()));
            pendingStatCandidates.insert(pendingStatCandidates.end(),
                                         std::make_move_iterator(threadPendingStat[t].begin()),
                                         std::make_move_iterator(threadPendingStat[t].end()));
        }
    }

    const auto compareRank = [&expr](const auto& a, const auto& b) {
        if (a.pathTier != b.pathTier) return a.pathTier < b.pathTier;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (expr.hasContentFilter()) {
            if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
            return a.record.lastWriteTime > b.record.lastWriteTime;
        }
        if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
        if (a.record.lastWriteTime != b.record.lastWriteTime)
            return a.record.lastWriteTime > b.record.lastWriteTime;
        if (a.record.fileName.size() != b.record.fileName.size())
            return a.record.fileName.size() < b.record.fileName.size();
        const int cmp = _wcsnicmp(
            a.record.fileName.data(),
            b.record.fileName.data(),
            a.record.fileName.size());
        if (cmp != 0) return cmp < 0;
        return a.record.fileName < b.record.fileName;
    };

    std::vector<DWORDLONG> resultIds;

    if (pendingStatCandidates.empty()) {
        if (isCancelled && isCancelled()) return results;
        const size_t resultCount = std::min(ranked.size(), static_cast<size_t>(limit));
        if (resultCount < ranked.size()) {
            std::partial_sort(ranked.begin(), ranked.begin() + resultCount, ranked.end(), compareRank);
        } else {
            std::sort(ranked.begin(), ranked.end(), compareRank);
        }

        {
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            if (!expr.requiresFullPath() &&
                cacheQueryKey.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos) {
                m_CachedQuery = cacheQueryKey;
                m_CachedCandidates = std::move(candidates);
                m_CachedGeneration = generation;
                m_CachedExcludeHidden = excludeOpts.excludeHidden;
                m_CachedExcludeSystem = excludeOpts.excludeSystem;
                m_CachedExcludePatterns = excludeOpts.patterns;
                m_CachedParseOptions = parseOpts;
            } else {
                m_CachedQuery.clear();
                m_CachedCandidates.clear();
                m_CachedExcludePatterns.clear();
            }
        }

        results.reserve(resultCount);
        resultIds.reserve(resultCount);
        for (size_t i = 0; i < resultCount; ++i) {
            const auto& candidate = ranked[i];
            results.push_back(SearchResult{
                std::wstring(candidate.record.fileName),
                buildFullPath(candidate.id),
                candidate.record.isDirectory,
                candidate.record.fileSize,
                candidate.record.creationTime,
                candidate.record.lastWriteTime,
                candidate.rank,
                candidate.folderPriority,
                candidate.pathTier,
                0ULL,
                candidate.record.fileAttributes,
            });
            resultIds.push_back(candidate.id);
        }

        // Never hold the index lock across filesystem I/O.
        lock.unlock();
    } else {
        // Materialize in-memory matches before unlocking `m_MapMutex`, then resolve
        // unhydrated `size:` / `dm:` candidates in parallel strictly OUTSIDE `m_MapMutex` (`P-02`).
        struct OwnedRankedItem {
            DWORDLONG id = 0;
            std::wstring fileName;
            std::wstring fullPath;
            bool isDirectory = false;
            uint64_t fileSize = 0;
            uint64_t creationTime = 0;
            uint64_t lastWriteTime = 0;
            int rank = 7;
            int folderPriority = 200;
            uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
            uint32_t fileAttributes = 0;
        };

        std::vector<OwnedRankedItem> ownedRanked;
        ownedRanked.reserve(ranked.size() + pendingStatCandidates.size());
        for (const auto& r : ranked) {
            ownedRanked.push_back(OwnedRankedItem{
                r.id,
                std::wstring(r.record.fileName),
                buildFullPath(r.id),
                r.record.isDirectory,
                r.record.fileSize,
                r.record.creationTime,
                r.record.lastWriteTime,
                r.rank,
                r.folderPriority,
                r.pathTier,
                r.record.fileAttributes,
            });
        }
        {
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            m_CachedQuery.clear();
            m_CachedCandidates.clear();
            m_CachedExcludePatterns.clear();
        }

        // Release `m_MapMutex` BEFORE any disk stat calls so `UsnListenerLoop` never blocks!
        lock.unlock();

        if (isCancelled && isCancelled()) return results;

        const size_t pendingCount = pendingStatCandidates.size();
        const size_t statBatchSize = 64;
        const size_t numStatBatches = (pendingCount + statBatchSize - 1) / statBatchSize;
        std::vector<std::vector<OwnedRankedItem>> batchMatched(numStatBatches);
        std::vector<std::vector<HydratedStat>> batchHydrated(numStatBatches);

        tools3000::service::QueryThreadPool::instance().parallelFor(numStatBatches, [&](size_t bIdx) {
            const size_t start = bIdx * statBatchSize;
            const size_t end = (std::min)(start + statBatchSize, pendingCount);
            auto& localHits = batchMatched[bIdx];
            auto& localStats = batchHydrated[bIdx];
            for (size_t k = start; k < end; ++k) {
                if (isCancelled && isCancelled()) break;
                auto& item = pendingStatCandidates[k];
                uint64_t fSize = item.fileSize;
                uint64_t cTime = item.creationTime;
                uint64_t mTime = item.lastWriteTime;
                const bool statOk = queryFrnDiskMetadata(item.id, item.fullPath, fSize, cTime, mTime);
                if (statOk) {
                    item.fileSize = fSize;
                    item.creationTime = cTime;
                    item.lastWriteTime = mTime;
                    localStats.push_back(HydratedStat{item.id, fSize, cTime, mTime});
                } else if (isRealTimeValidationEnabled()) {
                    continue;
                }

                FileRecord viewRec{};
                viewRec.fileReferenceNumber = item.id;
                viewRec.fileName = item.fileName;
                viewRec.normalizedName = item.normalizedName;
                viewRec.pinyinInitials = item.pinyinInitials.empty()
                    ? std::wstring_view(item.fileName)
                    : std::wstring_view(item.pinyinInitials);
                viewRec.pinyinFull = !item.pinyinFull.empty()
                    ? std::wstring_view(item.pinyinFull)
                    : (item.pinyinInitials.empty() ? std::wstring_view(item.fileName) : std::wstring_view{});
                viewRec.isDirectory = item.isDirectory;
                viewRec.fileAttributes = item.fileAttributes;
                viewRec.fileSize = item.fileSize;
                viewRec.creationTime = item.creationTime;
                viewRec.lastWriteTime = item.lastWriteTime;

                const auto status = expr.evaluateInMemoryWithDirMask(
                    viewRec, driveW, item.dirMask, /*isMetadataHydrated=*/true,
                    [&item]() { return item.fullPath; });
                if (status == SearchExpression::InMemoryMatchStatus::Matched) {
                    const int rnk = expr.calculateRank(viewRec);
                    localHits.push_back(OwnedRankedItem{
                        item.id,
                        std::move(item.fileName),
                        std::move(item.fullPath),
                        item.isDirectory,
                        item.fileSize,
                        item.creationTime,
                        item.lastWriteTime,
                        rnk,
                        item.folderPriority,
                        effectiveSingleDriveTier(item.pathTier, rnk),
                        item.fileAttributes,
                    });
                }
            }
        });

        if (isCancelled && isCancelled()) return results;

        for (size_t bIdx = 0; bIdx < numStatBatches; ++bIdx) {
            ownedRanked.insert(ownedRanked.end(),
                               std::make_move_iterator(batchMatched[bIdx].begin()),
                               std::make_move_iterator(batchMatched[bIdx].end()));
            hydratedStats.insert(hydratedStats.end(),
                                 batchHydrated[bIdx].begin(),
                                 batchHydrated[bIdx].end());
        }

        const auto compareOwned = [&expr](const OwnedRankedItem& a, const OwnedRankedItem& b) {
            if (a.pathTier != b.pathTier) return a.pathTier < b.pathTier;
            if (a.rank != b.rank) return a.rank < b.rank;
            if (expr.hasContentFilter()) {
                if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
                return a.lastWriteTime > b.lastWriteTime;
            }
            if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
            if (a.lastWriteTime != b.lastWriteTime) return a.lastWriteTime > b.lastWriteTime;
            if (a.fileName.size() != b.fileName.size()) return a.fileName.size() < b.fileName.size();
            const int cmp = _wcsnicmp(a.fileName.data(), b.fileName.data(), a.fileName.size());
            if (cmp != 0) return cmp < 0;
            return a.fileName < b.fileName;
        };

        const size_t resultCount = std::min(ownedRanked.size(), static_cast<size_t>(limit));
        if (resultCount < ownedRanked.size()) {
            std::partial_sort(ownedRanked.begin(), ownedRanked.begin() + resultCount, ownedRanked.end(), compareOwned);
        } else {
            std::sort(ownedRanked.begin(), ownedRanked.end(), compareOwned);
        }

        results.reserve(resultCount);
        resultIds.reserve(resultCount);
        for (size_t i = 0; i < resultCount; ++i) {
            auto& item = ownedRanked[i];
            results.push_back(SearchResult{
                std::move(item.fileName),
                std::move(item.fullPath),
                item.isDirectory,
                item.fileSize,
                item.creationTime,
                item.lastWriteTime,
                item.rank,
                item.folderPriority,
                item.pathTier,
                0ULL,
                item.fileAttributes,
            });
            resultIds.push_back(item.id);
        }
    }

    std::vector<SearchResult> validatedResults;
    validatedResults.reserve(results.size());
    std::vector<DWORDLONG> ghostFrns;
    const bool validateDiskExistence = isRealTimeValidationEnabled();

    for (size_t i = 0; i < results.size(); ++i) {
        if (isCancelled && isCancelled()) {
            validatedResults.clear();
            break;
        }
        const bool neededStat = (!results[i].isDirectory && results[i].fileSize == 0) ||
                                (results[i].lastWriteTime == 0);
        const HydrateResult status = hydrateResultProperties(results[i], queryExplicitRecycle, queryExplicitRecent);
        if (status == HydrateResult::Valid) {
            if (neededStat && (results[i].fileSize != 0 || results[i].lastWriteTime != 0)) {
                hydratedStats.push_back(HydratedStat{
                    resultIds[i],
                    results[i].fileSize,
                    results[i].creationTime,
                    results[i].lastWriteTime
                });
            }
            validatedResults.push_back(std::move(results[i]));
        } else if (status == HydrateResult::GhostNotFound) {
            if (validateDiskExistence) {
                ghostFrns.push_back(resultIds[i]);
            } else {
                // In mock / in-memory test mode without physical files on disk, keep the mock result
                validatedResults.push_back(std::move(results[i]));
            }
        } else if (status == HydrateResult::RejectedRecycleBin) {
            // Recycled files must never be returned when not explicitly requested
            if (validateDiskExistence && !queryExplicitRecycle &&
                (results[i].fullPath.find(L"$Recycle.Bin") != std::wstring::npos ||
                 results[i].fullPath.find(L"$RECYCLE.BIN") != std::wstring::npos)) {
                ghostFrns.push_back(resultIds[i]);
            }
        }
    }

    if (!ghostFrns.empty() || !hydratedStats.empty()) {
        std::unique_lock writeLock(m_MapMutex);
        for (const auto& st : hydratedStats) {
            m_Store.updateMetadata(st.frn, st.fileSize, st.creationTime, st.lastWriteTime);
        }
        bool anyErased = false;
        for (DWORDLONG ghostId : ghostFrns) {
            anyErased = m_Store.erase(ghostId) || anyErased;
        }
        if (anyErased) {
            m_Store.compactIfSparse();
            m_FileCount.store(m_Store.size(), std::memory_order_release);
            m_IndexGeneration.fetch_add(1, std::memory_order_release);
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            m_CachedQuery.clear();
            m_CachedCandidates.clear();
            m_CachedExcludePatterns.clear();
        }
    }
    return validatedResults;
}

std::vector<ContentCandidate> MftParser::CollectContentCandidates(
    const SearchExpression& fileFilterExpr,
    const ContentCandidateOptions& options,
    const CancellationCheck& isCancelled
) const {
    std::vector<ContentCandidate> collected;
    if (isCancelled && isCancelled()) return collected;

    constexpr uint64_t MaxContentCandidateBytes = 100ULL * 1024ULL * 1024ULL;
    const std::wstring& normQuery = fileFilterExpr.getNormalizedQuery();
    const bool explicitRecycle = normQuery.find(L"recycle") != std::wstring::npos;
    const bool explicitRecent = normQuery.find(L"recent") != std::wstring::npos ||
                                normQuery.find(L"lnk") != std::wstring::npos;
    const bool explicitWindows = normQuery.find(L"windows") != std::wstring::npos ||
                                 normQuery.find(L"winsxs") != std::wstring::npos ||
                                 normQuery.find(L"system32") != std::wstring::npos;
    const bool explicitSystemDirs = explicitWindows ||
                                    normQuery.find(L"appdata") != std::wstring::npos ||
                                    normQuery.find(L"programdata") != std::wstring::npos ||
                                    normQuery.find(L"program files") != std::wstring::npos ||
                                    normQuery.find(L"program") != std::wstring::npos;
    const bool explicitNodeModules = normQuery.find(L"node_modules") != std::wstring::npos ||
                                     normQuery.find(L"bower_components") != std::wstring::npos;
    const bool explicitVcs = normQuery.find(L".git") != std::wstring::npos ||
                             normQuery.find(L".svn") != std::wstring::npos;
    const bool explicitCache = normQuery.find(L"cache") != std::wstring::npos ||
                               normQuery.find(L"pycache") != std::wstring::npos ||
                               normQuery.find(L"gradle") != std::wstring::npos ||
                               normQuery.find(L"webview") != std::wstring::npos ||
                               normQuery.find(L"gemini") != std::wstring::npos ||
                               normQuery.find(L"cursor") != std::wstring::npos ||
                               normQuery.find(L"vscode") != std::wstring::npos;
    const bool explicitDeps = normQuery.find(L"venv") != std::wstring::npos ||
                              normQuery.find(L"site-packages") != std::wstring::npos ||
                              normQuery.find(L"cargo") != std::wstring::npos ||
                              normQuery.find(L"rustup") != std::wstring::npos ||
                              normQuery.find(L"nuget") != std::wstring::npos ||
                              normQuery.find(L"vcpkg") != std::wstring::npos ||
                              normQuery.find(L"packages") != std::wstring::npos;
    const bool explicitBuild = normQuery.find(L"target") != std::wstring::npos ||
                               normQuery.find(L"cmakefiles") != std::wstring::npos ||
                               normQuery.find(L"build") != std::wstring::npos ||
                               normQuery.find(L"dist") != std::wstring::npos ||
                               normQuery.find(L"release") != std::wstring::npos ||
                               normQuery.find(L"coverage") != std::wstring::npos ||
                               normQuery.find(L"obj") != std::wstring::npos;
    const bool explicitLock = normQuery.find(L"lock") != std::wstring::npos;

    std::vector<std::wstring> lowerExcludes;
    lowerExcludes.reserve(options.excludeOpts.patterns.size());
    for (const auto& pat : options.excludeOpts.patterns) {
        if (!pat.empty()) {
            lowerExcludes.push_back(normalize(pat));
        }
    }

    struct NormalizedDialogPriority {
        std::wstring pathLower;
        double score = 0.0;
    };
    std::vector<NormalizedDialogPriority> normalizedPriorities;
    normalizedPriorities.reserve(options.dialogPriorities.size());
    for (const auto& [rawPath, rawScore] : options.dialogPriorities) {
        std::wstring clean = normalize(rawPath);
        while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
            clean.pop_back();
        }
        if (!clean.empty()) {
            normalizedPriorities.push_back(NormalizedDialogPriority{std::move(clean), rawScore});
        }
    }

    const auto allowedExts =
        tools3000::service::content::ContentSearchEngine::instance().buildAllowedExtensionSnapshot(
            options.formatFilter);
    if (allowedExts.maxExtLength == 0) {
        return collected;
    }

    std::shared_lock lock(m_MapMutex);
    const size_t totalSlots = m_Store.slotCount();
    if (totalSlots == 0) return collected;

    struct CachedFolderNode {
        bool pruned = false;
        uint64_t dirClauseMask = 0;
        std::wstring fullDirPath;
        std::wstring lowerDirPath;
        double basePriority = 200.0;
    };

    const bool hasFileFilter = !fileFilterExpr.isEmpty();
    const bool filterHasSeparableGroups = hasFileFilter && fileFilterExpr.hasSeparablePathGroups();
    const bool filterIsFullySeparable = hasFileFilter && fileFilterExpr.isFullySeparable();

    const bool isNonSystemDrive = (m_DriveLetter != 'C' && m_DriveLetter != 'c');
    const wchar_t driveW = static_cast<wchar_t>(m_DriveLetter);
    const uint64_t rootContentClauseMask = filterHasSeparableGroups
        ? fileFilterExpr.computeDirectorySegmentMask(std::wstring_view(&driveW, 1))
        : 0ULL;

    auto resolveFolderNode = [&](DWORDLONG parentFrn,
                                 std::unordered_map<DWORDLONG, CachedFolderNode>& folderCache) -> const CachedFolderNode& {
        auto it = folderCache.find(parentFrn);
        if (it != folderCache.end()) {
            return it->second;
        }

        CachedFolderNode node;
        node.dirClauseMask = rootContentClauseMask;
        if (parentFrn == 0) {
            node.fullDirPath = {driveW, L':'};
            node.lowerDirPath = {static_cast<wchar_t>(std::towlower(driveW)), L':'};
            node.basePriority = isNonSystemDrive ? 2500.0 : 900.0;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        std::vector<std::wstring_view> segments;
        segments.reserve(16);
        bool prunedByAlwaysNoise = false;
        bool prunedByDevNoise = false;
        bool hasSystemProgramSeg = false;
        bool hasAppDataSeg = false;
        bool hasWorkspaceSeg = false;
        bool hasDesktopSeg = false;
        bool hasDocsSeg = false;
        bool hasDownloadsSeg = false;
        bool hasUsersSeg = false;
        bool hasChatSeg = false;
        bool hasLowValueSeg = false;
        bool hasSecondaryArtifactSeg = false;

        DWORDLONG visited[512];
        size_t visitedLen = 0;
        DWORDLONG cur = parentFrn;
        for (size_t depth = 0; depth < 512; ++depth) {
            if (visitedLen < 512) {
                visited[visitedLen++] = cur;
            }
            const auto* storedDir = m_Store.find(cur);
            if (!storedDir) break;
            const FileRecord dirRec = m_Store.view(*storedDir);
            if (!dirRec.fileName.empty() && dirRec.fileName != L".") {
                if (filterHasSeparableGroups) {
                    node.dirClauseMask |= fileFilterExpr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                }
                segments.push_back(dirRec.fileName);
                const std::wstring segNorm = normalize(dirRec.fileName);
                if (!explicitRecycle &&
                    (segNorm == L"$recycle.bin" || segNorm == L"system volume information")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitWindows &&
                           (segNorm == L"winsxs" || segNorm == L"servicing" ||
                            segNorm == L"prefetch" || segNorm == L"driverstore")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitNodeModules &&
                           (segNorm == L"node_modules" || segNorm == L"bower_components" ||
                            segNorm == L"jspm_packages")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitVcs &&
                           (segNorm == L".git" || segNorm == L".svn" || segNorm == L".hg")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitCache &&
                           (segNorm == L"gpucache" || segNorm == L"code cache" ||
                            segNorm == L"shadercache" || segNorm == L"dawncache" ||
                            segNorm == L"dawnwebgpucache" || segNorm == L"inetcache" ||
                            segNorm == L"webcache" || segNorm == L"cefcache" ||
                            segNorm == L"browsercache" || segNorm == L"ebwebview" ||
                            segNorm == L"webview2" || segNorm == L"crashpad" ||
                            segNorm == L"__pycache__" || segNorm == L".pytest_cache" ||
                            segNorm == L".mypy_cache" || segNorm == L".ruff_cache" ||
                            segNorm == L".parcel-cache" || segNorm == L".vite" ||
                            segNorm == L".pnpm-store" || segNorm == L".yarn" ||
                            segNorm == L"npm-cache" || segNorm == L"go-build" ||
                            segNorm == L".gradle" || segNorm == L".gemini" ||
                            segNorm == L".cursor" || segNorm == L".vscode-server")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitDeps &&
                           (segNorm == L".venv" || segNorm == L"venv" ||
                            segNorm == L"site-packages" || segNorm == L".tox" ||
                            segNorm == L".nox" || segNorm == L".cargo" ||
                            segNorm == L".rustup" || segNorm == L".nuget" ||
                            segNorm == L"vcpkg" || segNorm == L"vcpkg_installed")) {
                    prunedByDevNoise = true;
                } else if (!explicitBuild &&
                           (segNorm == L"target" || segNorm == L"cmakefiles" ||
                            segNorm == L"obj" || segNorm == L"build-x64" ||
                            segNorm == L"build-arm64" || segNorm == L".next" ||
                            segNorm == L".nuxt" || segNorm == L".svelte-kit" ||
                            segNorm == L".turbo" || segNorm == L".output" ||
                            segNorm == L"coverage" || segNorm == L"playwright-report" ||
                            segNorm == L"test-results")) {
                    prunedByDevNoise = true;
                } else if (segNorm == L"temp" || segNorm == L"tmp" || segNorm == L".vs" ||
                           segNorm == L"cache" || segNorm == L"caches") {
                    hasLowValueSeg = true;
                } else if (!explicitBuild &&
                           (segNorm == L"dist" || segNorm == L"out" ||
                            segNorm == L"build" || segNorm == L".build" ||
                            segNorm == L"release" || segNorm == L"debug" ||
                            segNorm == L"vendor" || segNorm == L"third_party" ||
                            segNorm == L"thirdparty" || segNorm == L"3rdparty" ||
                            segNorm == L"sapb1help" || segNorm == L"javadoc" ||
                            segNorm == L"apidocs")) {
                    hasSecondaryArtifactSeg = true;
                } else if (segNorm == L"xwechat_files" || segNorm == L"xwechat" ||
                           segNorm == L"wechat files" || segNorm == L"tencent files" ||
                           segNorm == L"nt_qq" || segNorm == L"wxwork" || segNorm == L"wecom" ||
                           segNorm == L"dingtalk" || segNorm == L"feishu" || segNorm == L"lark" ||
                           segNorm == L"telegram desktop" || segNorm == L"filestorage" ||
                           segNorm == L"file_recv" || segNorm == L"filerecv" || segNorm == L"msgattach") {
                    hasChatSeg = true;
                } else if (segNorm == L"appdata") {
                    hasAppDataSeg = true;
                } else if (segNorm == L"programdata" || segNorm == L"program files" ||
                           segNorm == L"program files (x86)" || segNorm == L"windows") {
                    hasSystemProgramSeg = true;
                } else if (segNorm == L"desktop") {
                    hasDesktopSeg = true;
                } else if (segNorm == L"documents") {
                    hasDocsSeg = true;
                } else if (segNorm == L"downloads") {
                    hasDownloadsSeg = true;
                } else if (segNorm == L"users") {
                    hasUsersSeg = true;
                } else if (segNorm == L"repo" || segNorm == L"repos" || segNorm == L"workspace" ||
                           segNorm == L"projects" || segNorm == L"work" || segNorm == L"docs" ||
                           segNorm == L"code" || segNorm == L"src" || segNorm == L"source" ||
                           segNorm == L"dev" || segNorm == L"github") {
                    hasWorkspaceSeg = true;
                }
            }
            if (dirRec.parentFileReferenceNumber == cur || dirRec.parentFileReferenceNumber == 0) {
                break;
            }
            if (std::find(visited, visited + visitedLen, dirRec.parentFileReferenceNumber) !=
                (visited + visitedLen)) {
                break;
            }
            cur = dirRec.parentFileReferenceNumber;
        }

        node.fullDirPath.reserve(128);
        node.fullDirPath.push_back(driveW);
        node.fullDirPath.push_back(L':');
        for (auto rIt = segments.rbegin(); rIt != segments.rend(); ++rIt) {
            node.fullDirPath.push_back(L'\\');
            node.fullDirPath.append(*rIt);
        }
        node.lowerDirPath = normalize(node.fullDirPath);

        if (prunedByAlwaysNoise) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        double bestDialogBoost = 0.0;
        double multiHitBonus = 0.0;
        size_t matchedDialogCount = 0;
        for (const auto& dp : normalizedPriorities) {
            const bool isBoundaryPrefix =
                (node.lowerDirPath.size() == dp.pathLower.size() && node.lowerDirPath == dp.pathLower) ||
                (node.lowerDirPath.size() > dp.pathLower.size() &&
                 node.lowerDirPath.starts_with(dp.pathLower) &&
                 node.lowerDirPath[dp.pathLower.size()] == L'\\');
            if (isBoundaryPrefix) {
                ++matchedDialogCount;
                const double specificity =
                    static_cast<double>((std::min<size_t>)(dp.pathLower.size(), 120)) * 30.0;
                const double candidateBoost = dp.score + specificity;
                if (candidateBoost > bestDialogBoost) {
                    bestDialogBoost = candidateBoost;
                }
                if (matchedDialogCount > 1) {
                    multiHitBonus = (std::min)(1200.0, multiHitBonus + 400.0);
                }
            }
        }
        const bool explicitlyPinnedByDialog = (matchedDialogCount > 0);

        if (prunedByDevNoise) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        if (!explicitSystemDirs && !explicitlyPinnedByDialog) {
            if (hasSystemProgramSeg || (hasAppDataSeg && !hasChatSeg)) {
                node.pruned = true;
                return folderCache.emplace(parentFrn, std::move(node)).first->second;
            }
        }

        if (!explicitWindows &&
            (node.lowerDirPath.find(L"\\windows\\system32") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\windows\\syswow64") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\windows\\installer") != std::wstring::npos)) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }
        if (!explicitRecent &&
            (node.lowerDirPath.find(L"\\microsoft\\windows\\recent") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\microsoft\\office\\recent") != std::wstring::npos)) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        if (!lowerExcludes.empty()) {
            std::wstring dirWithTrail = node.lowerDirPath;
            dirWithTrail.push_back(L'\\');
            for (const auto& exPat : lowerExcludes) {
                if (dirWithTrail.find(exPat) != std::wstring::npos) {
                    node.pruned = true;
                    return folderCache.emplace(parentFrn, std::move(node)).first->second;
                }
            }
        }

        if (hasLowValueSeg) {
            node.basePriority = 1.0;
        } else if (hasSecondaryArtifactSeg) {
            node.basePriority = 45.0;
        } else if (hasChatSeg) {
            node.basePriority = 80.0;
        } else if (hasSystemProgramSeg || hasAppDataSeg) {
            node.basePriority = 20.0 + bestDialogBoost;
        } else {
            double score = 200.0;
            if (isNonSystemDrive) {
                score += 900.0;
            }
            const size_t slashDepth = segments.size();
            if (slashDepth <= 4) {
                score += static_cast<double>(5 - slashDepth) * 350.0;
            }
            if (hasWorkspaceSeg) {
                score += 3500.0;
            }
            if ((!options.desktopDirLower.empty() && node.lowerDirPath.starts_with(options.desktopDirLower)) ||
                hasDesktopSeg) {
                score += 2200.0;
            } else if ((!options.documentsDirLower.empty() && node.lowerDirPath.starts_with(options.documentsDirLower)) ||
                       hasDocsSeg) {
                score += 2000.0;
            } else if ((!options.downloadsDirLower.empty() && node.lowerDirPath.starts_with(options.downloadsDirLower)) ||
                       hasDownloadsSeg) {
                score += 1600.0;
            } else if ((!options.userProfileDirLower.empty() && node.lowerDirPath.starts_with(options.userProfileDirLower)) ||
                       hasUsersSeg) {
                score += 1000.0;
            }
            score += bestDialogBoost + multiHitBonus;
            node.basePriority = score;
        }

        return folderCache.emplace(parentFrn, std::move(node)).first->second;
    };

    const unsigned int hwThreads = std::thread::hardware_concurrency();
    unsigned int numThreads = (hwThreads <= 2) ? (std::max)(1u, hwThreads) : (std::min)(hwThreads, 12u);
    if (totalSlots < 16384) {
        numThreads = 1;
    }

    const size_t chunkSize = (totalSlots + numThreads - 1) / numThreads;
    unsigned int activeBuckets = 0;
    for (unsigned int t = 0; t < numThreads; ++t) {
        if (t * chunkSize < totalSlots) {
            ++activeBuckets;
        }
    }

    struct DeferredContentStatItem {
        uint64_t frn = 0;
        uint64_t dirMask = 0;
        std::wstring fileName;
        std::wstring normalizedName;
        std::wstring pinyinInitials;
        std::wstring pinyinFull;
        std::wstring fullPath;
        uint32_t fileAttributes = 0;
        ContentCandidate candidate;
    };

    std::vector<std::vector<ContentCandidate>> threadBuckets(activeBuckets);
    std::vector<std::vector<DeferredContentStatItem>> threadDeferredStats(activeBuckets);

    const size_t cancelMask = fileFilterExpr.hasRegexClause() ? 0x3Fu : 0xFFu;

    auto scanRange = [&](size_t bucketIdx, size_t startIdx, size_t endIdx) {
        try {
            auto& localOut = threadBuckets[bucketIdx];
            auto& localDeferred = threadDeferredStats[bucketIdx];
            localOut.reserve((std::min<size_t>)((endIdx - startIdx) / 32 + 64, 16384));
            std::unordered_map<DWORDLONG, CachedFolderNode> localFolderCache;
            localFolderCache.reserve(2048);

            for (size_t i = startIdx; i < endIdx; ++i) {
                if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) {
                    break;
                }
                const auto* stored = m_Store.at(i);
                if (!stored) continue;
                const FileRecord record = m_Store.view(*stored);
                if (record.isDirectory) continue;

                if (options.excludeOpts.excludeHidden && (record.fileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (options.excludeOpts.excludeSystem && (record.fileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
                if (record.fileSize > MaxContentCandidateBytes) continue;

                const std::wstring_view normName = record.normalizedName;
                if (normName.size() >= 2 && normName[0] == L'~' && normName[1] == L'$') {
                    continue;
                }

                const size_t dotPos = record.fileName.rfind(L'.');
                if (dotPos == std::wstring_view::npos || dotPos + 1 >= record.fileName.size()) {
                    continue;
                }
                const std::wstring_view extView = record.fileName.substr(dotPos + 1);
                if (!allowedExts.contains(extView)) {
                    continue;
                }

                const CachedFolderNode& dirNode = resolveFolderNode(
                    record.parentFileReferenceNumber, localFolderCache);
                if (dirNode.pruned) {
                    continue;
                }

                std::wstring builtFullPath;
                auto buildFullPathNow = [&]() -> std::wstring& {
                    if (builtFullPath.empty()) {
                        builtFullPath.reserve(dirNode.fullDirPath.size() + 1 + record.fileName.size());
                        builtFullPath += dirNode.fullDirPath;
                        builtFullPath.push_back(L'\\');
                        builtFullPath += record.fileName;
                    }
                    return builtFullPath;
                };

                bool needsDeferredStat = false;
                if (hasFileFilter) {
                    if (filterIsFullySeparable) {
                        if (!fileFilterExpr.matchesSeparable(record, driveW, dirNode.dirClauseMask)) {
                            continue;
                        }
                    } else {
                        auto getFullPath = [&]() -> std::wstring {
                            return buildFullPathNow();
                        };
                        const auto status = fileFilterExpr.evaluateInMemoryWithDirMask(
                            record, driveW, dirNode.dirClauseMask, stored->isMetadataHydrated(), getFullPath);
                        if (status == SearchExpression::InMemoryMatchStatus::Rejected) {
                            continue;
                        }
                        if (status == SearchExpression::InMemoryMatchStatus::NeedsDiskStat) {
                            needsDeferredStat = true;
                        }
                    }
                }

                std::wstring finalPath = std::move(buildFullPathNow());
                if (!lowerExcludes.empty()) {
                    const std::wstring lowerFileName = normalize(record.fileName);
                    bool excludedByName = false;
                    for (const auto& exPat : lowerExcludes) {
                        if (lowerFileName.find(exPat) != std::wstring::npos) {
                            excludedByName = true;
                            break;
                        }
                    }
                    if (excludedByName) continue;
                }

                double extOffset = computeExtensionSignalOffset(extView, normName, explicitLock);
                if (!explicitLock) {
                    if (normName.ends_with(L".min.js") || normName.ends_with(L".min.css") ||
                        normName.ends_with(L".bundle.js") || normName.ends_with(L".chunk.js") ||
                        normName.ends_with(L".js.map")) {
                        extOffset -= 2200.0;
                    }
                    if (record.fileSize > 0 && record.fileSize <= 2ULL * 1024ULL * 1024ULL) {
                        extOffset += 160.0;
                    } else if (record.fileSize > 25ULL * 1024ULL * 1024ULL) {
                        extOffset -= 260.0;
                    }
                }
                ContentCandidate cand{
                    .fileName = std::wstring(record.fileName),
                    .fullPath = std::move(finalPath),
                    .fileSize = record.fileSize,
                    .creationTime = record.creationTime,
                    .lastWriteTime = record.lastWriteTime,
                    .priorityScore = (std::max)(1.0, dirNode.basePriority + extOffset)
                };

                if (needsDeferredStat) {
                    if (localDeferred.size() < 32768) {
                        const bool sameInit = (record.pinyinInitials.data() == record.fileName.data());
                        const bool sameFull = (record.pinyinFull.data() == record.fileName.data());
                        localDeferred.push_back(DeferredContentStatItem{
                            record.fileReferenceNumber,
                            dirNode.dirClauseMask,
                            cand.fileName,
                            std::wstring(record.normalizedName),
                            sameInit ? std::wstring{} : std::wstring(record.pinyinInitials),
                            sameFull ? std::wstring{} : std::wstring(record.pinyinFull),
                            cand.fullPath,
                            record.fileAttributes,
                            std::move(cand),
                        });
                    }
                } else {
                    localOut.push_back(std::move(cand));
                }
            }
        } catch (...) {
        }
    };

    if (activeBuckets > 0) {
        tools3000::service::QueryThreadPool::instance().parallelFor(activeBuckets, [&](size_t t) {
            const size_t startIdx = t * chunkSize;
            const size_t endIdx = (std::min)(startIdx + chunkSize, totalSlots);
            scanRange(t, startIdx, endIdx);
        });
    }

    lock.unlock();
    if (isCancelled && isCancelled()) return collected;

    size_t totalFound = 0;
    size_t totalDeferred = 0;
    for (size_t b = 0; b < activeBuckets; ++b) {
        totalFound += threadBuckets[b].size();
        totalDeferred += threadDeferredStats[b].size();
    }
    collected.reserve(totalFound + totalDeferred);
    for (auto& b : threadBuckets) {
        collected.insert(
            collected.end(),
            std::make_move_iterator(b.begin()),
            std::make_move_iterator(b.end())
        );
    }

    if (totalDeferred > 0) {
        for (auto& db : threadDeferredStats) {
            for (auto& dItem : db) {
                if (isCancelled && isCancelled()) return {};
                uint64_t fSize = dItem.candidate.fileSize;
                uint64_t cTime = dItem.candidate.creationTime;
                uint64_t mTime = dItem.candidate.lastWriteTime;
                if (queryFrnDiskMetadata(dItem.frn, dItem.fullPath, fSize, cTime, mTime)) {
                    dItem.candidate.fileSize = fSize;
                    dItem.candidate.creationTime = cTime;
                    dItem.candidate.lastWriteTime = mTime;
                }
                if (dItem.candidate.fileSize > MaxContentCandidateBytes) {
                    continue;
                }
                FileRecord recView{};
                recView.fileReferenceNumber = dItem.frn;
                recView.fileName = dItem.fileName;
                recView.normalizedName = dItem.normalizedName;
                recView.pinyinInitials = dItem.pinyinInitials.empty()
                    ? std::wstring_view(dItem.fileName)
                    : std::wstring_view(dItem.pinyinInitials);
                recView.pinyinFull = !dItem.pinyinFull.empty()
                    ? std::wstring_view(dItem.pinyinFull)
                    : (dItem.pinyinInitials.empty() ? std::wstring_view(dItem.fileName) : std::wstring_view{});
                recView.isDirectory = false;
                recView.fileAttributes = dItem.fileAttributes;
                recView.fileSize = dItem.candidate.fileSize;
                recView.creationTime = dItem.candidate.creationTime;
                recView.lastWriteTime = dItem.candidate.lastWriteTime;

                if (fileFilterExpr.evaluateInMemoryWithDirMask(
                        recView, driveW, dItem.dirMask, /*isMetadataHydrated=*/true,
                        [&dItem]() { return dItem.fullPath; }) ==
                    SearchExpression::InMemoryMatchStatus::Matched) {
                    collected.push_back(std::move(dItem.candidate));
                }
            }
        }
    }

    auto comparePriority = [](const ContentCandidate& a, const ContentCandidate& b) {
        if (a.priorityScore != b.priorityScore) return a.priorityScore > b.priorityScore;
        return a.lastWriteTime > b.lastWriteTime;
    };

    const size_t cap = options.maxCandidates > 0 ? options.maxCandidates : collected.size();
    if (collected.size() > cap) {
        std::nth_element(collected.begin(), collected.begin() + cap, collected.end(), comparePriority);
        collected.resize(cap);
    }
    std::sort(collected.begin(), collected.end(), comparePriority);
    return collected;
}

