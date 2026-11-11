#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <regex>
#include <optional>
#include <cstdint>
#include <functional>

struct T3kRegexHandle;

// A resolved, read-only view of one indexed file.
//
// The index keeps names in a StringArena rather than in per-record wstrings, so
// this is a view over that storage: building one during a scan is pointer
// arithmetic with no allocation. Callers outside the index (tests, ad-hoc
// matching) can point the views at string literals or at strings they keep alive
// themselves.
struct FileRecord {
    uint64_t fileReferenceNumber = 0;
    uint64_t parentFileReferenceNumber = 0;
    std::wstring_view fileName;
    std::wstring_view normalizedName;
    std::wstring_view pinyinInitials;
    std::wstring_view pinyinFull;
    bool isDirectory = false;
    uint32_t fileAttributes = 0;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
};

// The owning form used to feed records into the index. Normalized and pinyin
// forms are derived on insert, so they are deliberately absent here.
struct FileRecordInit {
    uint64_t fileReferenceNumber = 0;
    uint64_t parentFileReferenceNumber = 0;
    std::wstring fileName;
    bool isDirectory = false;
    uint32_t fileAttributes = 0;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
};

enum class SearchFilterType {
    None,
    FileOnly,      // file:
    FolderOnly,    // folder:, dir:
    Extension,     // ext:jpg;png, ext:txt
    Path,          // path:text
    Parent,        // parent:dir, p:dir
    Drive,         // c:, d:
    Exact,         // exact:foo.txt
    Regex,         // regex:pattern, r:pattern
    CaseSensitive, // case:text
    PinyinOnly,    // pinyin:text, py:text
    NoPinyin,      // nopy:text
    Content,       // content:text, c:text, 内容:text
    Size,          // size:>100mb, size:<10kb, size:>=1gb
    DateModified   // dm:today, dm:2026-09, dm:>2026-09-01, datemodified:, date:
};

struct SearchClause {
    enum class SizeOp { Equal, Greater, GreaterEqual, Less, LessEqual, Range };

    bool isNegated = false;
    SearchFilterType filterType = SearchFilterType::None;
    std::wstring pattern;        // normalized (lowercased)
    std::wstring rawPattern;     // case preserved
    std::wstring pinyinPattern;   // normalized and stripped of apostrophes (e.g. tong'xi -> tongxi)
    std::vector<std::wstring> extList; // for ext:jpg;png
    std::optional<std::wregex> regexObj;
    std::shared_ptr<T3kRegexHandle> linearRegex;
    bool regexInvalid = false;
    bool hasWildcard = false;
    bool isAsciiOnly = false;
    bool isCaselessPattern = false;
    bool hasPathSeparator = false;
    wchar_t driveLetter = 0;
    SizeOp sizeOp = SizeOp::Equal;
    uint64_t sizeBytes = 0;
    uint64_t sizeMaxBytes = UINT64_MAX;
    bool hasValidDateRange = false;
    uint64_t dateMinFileTime = 0;
    uint64_t dateMaxFileTime = UINT64_MAX;
    bool parentDriveRootOnly = false;
    bool hasSlashPreFilter = false;
    bool slashAllowBranchA = false;
    bool slashAllowBranchB = false;
    bool slashLeafCaseSensitive = false;
    uint64_t slashBranchADirMask = 0;
    uint64_t slashBranchBDirMask = 0;
    std::wstring slashLeafPrefix;
    std::wstring slashLeafSuffix;
};

struct SearchParseOptions {
    bool pinyinEnabled = true;
    bool matchPath = false;
    bool caseSensitive = false;

    bool operator==(const SearchParseOptions& other) const noexcept = default;
};

struct SearchOrGroup {
    std::vector<SearchClause> clauses;
};

struct ContentSearchPlan;

class SearchExpression {
public:
    enum class InMemoryMatchStatus : uint8_t {
        Rejected = 0,
        Matched = 1,
        NeedsDiskStat = 2,
    };

    using PathGetter = std::function<std::wstring()>;
    using StatHydrateCallback = std::function<void(
        uint64_t frn, uint64_t fileSize, uint64_t creationTime, uint64_t lastWriteTime)>;

    static SearchExpression parse(const std::wstring& query, const SearchParseOptions& options = {});
    static ContentSearchPlan buildContentPlan(
        const std::wstring& query,
        bool implicitContentMode,
        const SearchParseOptions& options = {}
    );

    bool isEmpty() const { return m_orGroups.empty(); }
    bool requiresFullPath() const { return m_requiresFullPath; }
    bool hasContentFilter() const { return m_hasContentFilter; }
    bool hasRegexClause() const noexcept { return m_hasRegexClause; }
    bool isPinyinEnabled() const { return m_pinyinEnabled; }
    bool isCaseSensitive() const { return m_caseSensitive; }
    const std::wstring& getContentQuery() const { return m_contentQuery; }
    const std::vector<std::wstring>& getContentTerms() const { return m_contentTerms; }

    bool matches(const FileRecord& record, wchar_t driveLetter,
                 const std::wstring& fullPath = L"") const;

    bool matchesWithLazyPath(const FileRecord& record, wchar_t driveLetter,
                             const PathGetter& getFullPath,
                             const StatHydrateCallback& onStatHydrated = {}) const;

    uint64_t computeDirectorySegmentMask(std::wstring_view dirSegmentName,
                                         std::wstring_view pinyinInitials = {},
                                         std::wstring_view pinyinFull = {}) const noexcept;

    bool matchesWithDirMask(const FileRecord& record, wchar_t driveLetter,
                            uint64_t dirClauseMask,
                            const PathGetter& getFullPath,
                            const StatHydrateCallback& onStatHydrated = {}) const;

    // Lock-free three-state evaluation (`P-02`): evaluates all in-memory & path predicates
    // without touching disk I/O (`GetFileAttributesExW`). Returns `NeedsDiskStat` only when
    // `size:` / `dm:` clauses require unhydrated metadata so callers can stat outside locks.
    InMemoryMatchStatus evaluateInMemoryWithDirMask(
        const FileRecord& record,
        wchar_t driveLetter,
        uint64_t dirClauseMask,
        bool isMetadataHydrated,
        const PathGetter& getFullPath) const;

    bool matchesSeparable(const FileRecord& record, wchar_t driveLetter,
                          uint64_t dirClauseMask) const noexcept;

    bool hasSeparablePathGroups() const noexcept {
        return (m_dirFileSatisfiedMask | m_dirFolderSatisfiedMask |
                m_negatedSeparableGroupMask | m_slashPredicateMask) != 0;
    }
    bool isFullySeparable() const noexcept { return m_fullySeparable; }
    uint64_t getNegatedSeparableGroupMask() const noexcept { return m_negatedSeparableGroupMask; }

    int calculateRank(const FileRecord& record) const;

    const std::wstring& getNormalizedQuery() const { return m_normalizedQuery; }
    const std::vector<SearchOrGroup>& getOrGroups() const { return m_orGroups; }

    static bool matchWildcard(std::wstring_view pattern, std::wstring_view text, bool caseSensitive = false);
    static std::wstring normalize(std::wstring_view text);

private:
    enum class DirSegMatchOp : uint8_t {
        Contains = 0,
        Prefix = 1,
        Suffix = 2,
        Exact = 3,
    };

    struct DirSegmentPredicate {
        DirSegMatchOp op = DirSegMatchOp::Contains;
        bool caseSensitive = false;
        uint8_t bitIndex = 0;
        std::wstring needle;
    };

    void recomputePathRequirement(const SearchParseOptions& options);

    std::wstring m_rawQuery;
    std::wstring m_normalizedQuery;
    std::wstring m_rankingQuery;
    std::vector<std::wstring> m_rankingTerms;
    std::wstring m_cleanAsciiQuery;
    std::vector<SearchOrGroup> m_orGroups;
    std::vector<DirSegmentPredicate> m_slashDirPredicates;
    bool m_requiresFullPath = false;
    bool m_hasContentFilter = false;
    bool m_hasRegexClause = false;
    bool m_hasPureExtensionGroup = false;
    bool m_hasFolderOnlyFilter = false;
    bool m_isAsciiQuery = false;
    bool m_pinyinEnabled = true;
    bool m_caseSensitive = false;
    bool m_fullySeparable = false;
    uint64_t m_separableGroupMask = 0;
    uint64_t m_negatedSeparableGroupMask = 0;
    uint64_t m_dirFileSatisfiedMask = 0;
    uint64_t m_dirFolderSatisfiedMask = 0;
    uint64_t m_slashPredicateMask = 0;
    std::wstring m_contentQuery;
    std::vector<std::wstring> m_contentTerms;
};

struct ContentSearchPlan {
    SearchExpression fileFilterExpr;
    std::vector<std::wstring> contentTerms;
    bool hasExplicitContentSyntax = false;
    bool caseSensitive = false;
};
