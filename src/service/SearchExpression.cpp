#include "SearchExpression.h"
#include "SearchRequestLimits.h"
#include "content/ContentSearchEngine.h"
#include <algorithm>
#include <cwctype>
#include <sstream>

std::wstring SearchExpression::normalize(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        result.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return result;
}

bool SearchExpression::matchWildcard(std::wstring_view pattern, std::wstring_view text, bool caseSensitive) {
    size_t p = 0, t = 0;
    size_t starP = std::wstring_view::npos, starT = 0;
    while (t < text.size()) {
        const wchar_t cp = (p < pattern.size())
            ? (caseSensitive ? pattern[p] : static_cast<wchar_t>(std::towlower(pattern[p])))
            : L'\0';
        const wchar_t ct = caseSensitive ? text[t] : static_cast<wchar_t>(std::towlower(text[t]));
        if (p < pattern.size() && (pattern[p] == L'?' || cp == ct)) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            starP = p++;
            starT = t;
        } else if (starP != std::wstring_view::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') p++;
    return p == pattern.size();
}

namespace {

std::wstring normalizePathSeparators(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        result.push_back(ch == L'/' ? L'\\' : ch);
    }
    return result;
}

constexpr uint64_t kOneDayFileTime = 864000000000ULL;

uint64_t localSystemTimeToFileTime(const SYSTEMTIME& localSt) noexcept {
    SYSTEMTIME utcSt{};
    FILETIME ft{};
    if (TzSpecificLocalTimeToSystemTime(nullptr, &localSt, &utcSt) &&
        SystemTimeToFileTime(&utcSt, &ft)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart;
    }
    FILETIME localFt{};
    if (SystemTimeToFileTime(&localSt, &localFt) &&
        LocalFileTimeToFileTime(&localFt, &ft)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart;
    }
    return 0;
}

WORD daysInMonth(WORD year, WORD month) noexcept {
    static constexpr WORD kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month - 1];
}

bool parseExplicitDateSpan(std::wstring_view text, uint64_t& outStartFt, uint64_t& outEndFt) noexcept {
    if (text.empty()) return false;

    std::vector<int> parts;
    parts.reserve(3);
    size_t start = 0;
    while (start < text.size()) {
        size_t sep = text.find_first_of(L"-/.", start);
        if (sep == std::wstring_view::npos) sep = text.size();
        std::wstring_view segment = text.substr(start, sep - start);
        if (segment.empty()) return false;
        int val = 0;
        for (wchar_t ch : segment) {
            if (ch < L'0' || ch > L'9') return false;
            val = val * 10 + static_cast<int>(ch - L'0');
            if (val > 9999) return false;
        }
        parts.push_back(val);
        if (parts.size() > 3) return false;
        start = sep + 1;
    }

    if (parts.empty()) return false;
    const int year = parts[0];
    if (year < 1970 || year > 2100) return false;

    SYSTEMTIME startSt{};
    SYSTEMTIME endSt{};
    startSt.wYear = static_cast<WORD>(year);
    endSt.wYear = static_cast<WORD>(year);
    endSt.wHour = 23;
    endSt.wMinute = 59;
    endSt.wSecond = 59;
    endSt.wMilliseconds = 999;

    if (parts.size() == 1) {
        startSt.wMonth = 1;
        startSt.wDay = 1;
        endSt.wMonth = 12;
        endSt.wDay = 31;
    } else if (parts.size() == 2) {
        const int month = parts[1];
        if (month < 1 || month > 12) return false;
        startSt.wMonth = static_cast<WORD>(month);
        startSt.wDay = 1;
        endSt.wMonth = static_cast<WORD>(month);
        endSt.wDay = daysInMonth(static_cast<WORD>(year), static_cast<WORD>(month));
    } else {
        const int month = parts[1];
        const int day = parts[2];
        if (month < 1 || month > 12) return false;
        const WORD maxDay = daysInMonth(static_cast<WORD>(year), static_cast<WORD>(month));
        if (day < 1 || day > static_cast<int>(maxDay)) return false;
        startSt.wMonth = static_cast<WORD>(month);
        startSt.wDay = static_cast<WORD>(day);
        endSt.wMonth = static_cast<WORD>(month);
        endSt.wDay = static_cast<WORD>(day);
    }

    outStartFt = localSystemTimeToFileTime(startSt);
    outEndFt = localSystemTimeToFileTime(endSt);
    return outStartFt > 0 && outEndFt >= outStartFt;
}

void parseDateModifiedClause(std::wstring s, SearchClause& clause) noexcept {
    clause.filterType = SearchFilterType::DateModified;
    clause.hasValidDateRange = false;
    clause.dateMinFileTime = 0;
    clause.dateMaxFileTime = UINT64_MAX;
    if (s.empty()) return;

    std::wstring lower = SearchExpression::normalize(s);

    SYSTEMTIME nowLocal{};
    GetLocalTime(&nowLocal);
    SYSTEMTIME todayStartSt = nowLocal;
    todayStartSt.wHour = 0;
    todayStartSt.wMinute = 0;
    todayStartSt.wSecond = 0;
    todayStartSt.wMilliseconds = 0;
    const uint64_t todayStartFt = localSystemTimeToFileTime(todayStartSt);
    const uint64_t nowFt = localSystemTimeToFileTime(nowLocal);

    if (lower == L"today") {
        clause.dateMinFileTime = todayStartFt;
        clause.dateMaxFileTime = todayStartFt + kOneDayFileTime - 1;
        clause.hasValidDateRange = true;
        return;
    }
    if (lower == L"yesterday") {
        clause.dateMinFileTime = (todayStartFt >= kOneDayFileTime) ? (todayStartFt - kOneDayFileTime) : 1;
        clause.dateMaxFileTime = (todayStartFt > 0) ? (todayStartFt - 1) : UINT64_MAX;
        clause.hasValidDateRange = true;
        return;
    }
    if (lower == L"thisweek" || lower == L"week") {
        const uint64_t daysSinceMonday = (nowLocal.wDayOfWeek == 0) ? 6ULL : static_cast<uint64_t>(nowLocal.wDayOfWeek - 1);
        const uint64_t weekStartFt = (todayStartFt >= daysSinceMonday * kOneDayFileTime)
            ? (todayStartFt - daysSinceMonday * kOneDayFileTime)
            : 1;
        clause.dateMinFileTime = weekStartFt;
        clause.dateMaxFileTime = weekStartFt + 7ULL * kOneDayFileTime - 1;
        clause.hasValidDateRange = true;
        return;
    }
    if (lower == L"thismonth" || lower == L"month") {
        SYSTEMTIME monthStart = todayStartSt;
        monthStart.wDay = 1;
        SYSTEMTIME monthEnd = todayStartSt;
        monthEnd.wDay = daysInMonth(nowLocal.wYear, nowLocal.wMonth);
        monthEnd.wHour = 23;
        monthEnd.wMinute = 59;
        monthEnd.wSecond = 59;
        monthEnd.wMilliseconds = 999;
        clause.dateMinFileTime = localSystemTimeToFileTime(monthStart);
        clause.dateMaxFileTime = localSystemTimeToFileTime(monthEnd);
        clause.hasValidDateRange = clause.dateMinFileTime > 0;
        return;
    }
    if (lower == L"thisyear" || lower == L"year") {
        SYSTEMTIME yearStart = todayStartSt;
        yearStart.wMonth = 1;
        yearStart.wDay = 1;
        SYSTEMTIME yearEnd = todayStartSt;
        yearEnd.wMonth = 12;
        yearEnd.wDay = 31;
        yearEnd.wHour = 23;
        yearEnd.wMinute = 59;
        yearEnd.wSecond = 59;
        yearEnd.wMilliseconds = 999;
        clause.dateMinFileTime = localSystemTimeToFileTime(yearStart);
        clause.dateMaxFileTime = localSystemTimeToFileTime(yearEnd);
        clause.hasValidDateRange = clause.dateMinFileTime > 0;
        return;
    }
    if (lower == L"last7days" || lower == L"pastweek" || lower == L"7d") {
        clause.dateMinFileTime = (nowFt >= 7ULL * kOneDayFileTime) ? (nowFt - 7ULL * kOneDayFileTime) : 1;
        clause.dateMaxFileTime = UINT64_MAX;
        clause.hasValidDateRange = true;
        return;
    }
    if (lower == L"last30days" || lower == L"pastmonth" || lower == L"30d") {
        clause.dateMinFileTime = (nowFt >= 30ULL * kOneDayFileTime) ? (nowFt - 30ULL * kOneDayFileTime) : 1;
        clause.dateMaxFileTime = UINT64_MAX;
        clause.hasValidDateRange = true;
        return;
    }

    // Range syntax: dm:2026-09-01..2026-09-30 or dm:2026-09-01~2026-09-30
    size_t rangePos = lower.find(L"..");
    size_t rangeSepLen = 2;
    if (rangePos == std::wstring::npos) {
        rangePos = lower.find(L'~');
        rangeSepLen = 1;
    }
    if (rangePos != std::wstring::npos) {
        uint64_t start1 = 0, end1 = 0, start2 = 0, end2 = 0;
        const bool okLeft = parseExplicitDateSpan(std::wstring_view(lower).substr(0, rangePos), start1, end1);
        const bool okRight = parseExplicitDateSpan(std::wstring_view(lower).substr(rangePos + rangeSepLen), start2, end2);
        if (okLeft && okRight && end2 >= start1) {
            clause.dateMinFileTime = start1;
            clause.dateMaxFileTime = end2;
            clause.hasValidDateRange = true;
        }
        return;
    }

    SearchClause::SizeOp op = SearchClause::SizeOp::Equal;
    std::wstring_view rest(lower);
    if (rest.starts_with(L">=")) {
        op = SearchClause::SizeOp::GreaterEqual;
        rest.remove_prefix(2);
    } else if (rest.starts_with(L"<=")) {
        op = SearchClause::SizeOp::LessEqual;
        rest.remove_prefix(2);
    } else if (rest.starts_with(L">")) {
        op = SearchClause::SizeOp::Greater;
        rest.remove_prefix(1);
    } else if (rest.starts_with(L"<")) {
        op = SearchClause::SizeOp::Less;
        rest.remove_prefix(1);
    } else if (rest.starts_with(L"=")) {
        op = SearchClause::SizeOp::Equal;
        rest.remove_prefix(1);
    }

    uint64_t spanStart = 0, spanEnd = 0;
    if (!parseExplicitDateSpan(rest, spanStart, spanEnd)) {
        return;
    }

    clause.hasValidDateRange = true;
    switch (op) {
        case SearchClause::SizeOp::Equal:
            clause.dateMinFileTime = spanStart;
            clause.dateMaxFileTime = spanEnd;
            break;
        case SearchClause::SizeOp::Greater:
            clause.dateMinFileTime = spanEnd + 1;
            clause.dateMaxFileTime = UINT64_MAX;
            break;
        case SearchClause::SizeOp::GreaterEqual:
            clause.dateMinFileTime = spanStart;
            clause.dateMaxFileTime = UINT64_MAX;
            break;
        case SearchClause::SizeOp::Less:
            clause.dateMinFileTime = 1;
            clause.dateMaxFileTime = (spanStart > 0) ? (spanStart - 1) : 0;
            break;
        case SearchClause::SizeOp::LessEqual:
            clause.dateMinFileTime = 1;
            clause.dateMaxFileTime = spanEnd;
            break;
    }
}

struct RawQueryToken {
    std::wstring text;
    bool quotedLiteral = false;
};

void stripOuterQuotes(std::wstring& value) {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    } else if (!value.empty() && value.front() == L'"') {
        value.erase(0, 1);
    }
}

SearchClause parseSingleToken(const RawQueryToken& rawTok, const SearchParseOptions& options) {
    SearchClause clause;
    std::wstring token = rawTok.text;
    if (token.empty()) return clause;

    if (rawTok.quotedLiteral) {
        if (token.size() >= 3 && std::iswalpha(token[0]) && token[1] == L':' &&
            (token[2] == L'\\' || token[2] == L'/')) {
            clause.filterType = SearchFilterType::Path;
            token = normalizePathSeparators(token);
            clause.rawPattern = token;
            clause.pattern = SearchExpression::normalize(token);
            clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos ||
                                 clause.pattern.find(L'?') != std::wstring::npos;
            return clause;
        }
        clause.rawPattern = token;
        clause.pattern = SearchExpression::normalize(token);
        clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos ||
                             clause.pattern.find(L'?') != std::wstring::npos;
        clause.pinyinPattern.reserve(clause.pattern.size());
        clause.isAsciiOnly = !clause.pattern.empty();
        for (wchar_t ch : clause.pattern) {
            if (ch >= L'a' && ch <= L'z') {
                clause.pinyinPattern.push_back(ch);
            } else if (ch == L'\'' || ch == L'-') {
                continue;
            } else {
                clause.isAsciiOnly = false;
            }
        }
        if (clause.pinyinPattern.empty()) {
            clause.isAsciiOnly = false;
        }
        return clause;
    }

    if (token.front() == L'!' && token.size() > 1) {
        clause.isNegated = true;
        token.erase(0, 1);
        if (token.size() >= 2 && token.front() == L'"' && token.back() == L'"') {
            stripOuterQuotes(token);
            token = normalizePathSeparators(token);
            clause.rawPattern = token;
            clause.pattern = SearchExpression::normalize(token);
            clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos ||
                                 clause.pattern.find(L'?') != std::wstring::npos;
            return clause;
        }
    }

    auto startsWithNoCase = [](const std::wstring& str, const std::wstring& prefix) -> bool {
        if (str.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towlower(str[i]) != std::towlower(prefix[i])) return false;
        }
        return true;
    };

    if (token.size() >= 2 && std::iswalpha(token[0]) && token[1] == L':') {
        if (token.size() == 2) {
            clause.filterType = SearchFilterType::Drive;
            clause.driveLetter = static_cast<wchar_t>(std::towupper(token[0]));
            return clause;
        } else if (token[2] == L'\\' || token[2] == L'/') {
            clause.filterType = SearchFilterType::Path;
            stripOuterQuotes(token);
            token = normalizePathSeparators(token);
            clause.rawPattern = token;
            clause.pattern = SearchExpression::normalize(token);
            clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos || clause.pattern.find(L'?') != std::wstring::npos;
            return clause;
        }
    }

    if (startsWithNoCase(token, L"file:")) {
        clause.filterType = SearchFilterType::FileOnly;
        token = token.substr(5);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"folder:") || startsWithNoCase(token, L"dir:")) {
        clause.filterType = SearchFilterType::FolderOnly;
        token = token.substr(token[0] == L'f' || token[0] == L'F' ? 7 : 4);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"ext:")) {
        clause.filterType = SearchFilterType::Extension;
        std::wstring exts = token.substr(4);
        stripOuterQuotes(exts);
        size_t start = 0;
        while (start < exts.size()) {
            size_t end = exts.find_first_of(L";,|", start);
            if (end == std::wstring::npos) end = exts.size();
            std::wstring ext = SearchExpression::normalize(exts.substr(start, end - start));
            if (!ext.empty()) {
                if (ext.front() == L'.') ext.erase(0, 1);
                if (!ext.empty()) clause.extList.push_back(std::move(ext));
            }
            start = end + 1;
        }
        return clause;
    } else if (startsWithNoCase(token, L"path:")) {
        clause.filterType = SearchFilterType::Path;
        token = token.substr(5);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"parent:") || startsWithNoCase(token, L"p:")) {
        clause.filterType = SearchFilterType::Parent;
        std::wstring sub = token.substr((token[0] == L'p' || token[0] == L'P') && token[1] == L':' ? 2 : 7);
        stripOuterQuotes(sub);
        token = normalizePathSeparators(sub);
        while (!token.empty() && token.back() == L'\\') {
            token.pop_back();
        }
        if (token.size() == 2 && std::iswalpha(token[0]) && token[1] == L':') {
            clause.parentDriveRootOnly = true;
        }
    } else if (startsWithNoCase(token, L"regex:") || startsWithNoCase(token, L"r:")) {
        clause.filterType = SearchFilterType::Regex;
        token = token.substr((token[0] == L'r' || token[0] == L'R') && token[1] == L':' ? 2 : 6);
        stripOuterQuotes(token);
        if (token.size() > tools3000::service::search_limits::MaxRegexCharacters) {
            clause.rawPattern = token;
            clause.regexObj = std::nullopt;
            return clause;
        }
        try {
            const auto regexFlags = options.caseSensitive
                ? std::regex::optimize
                : (std::regex::icase | std::regex::optimize);
            clause.regexObj = std::wregex(token, regexFlags);
        } catch (...) {
            clause.regexObj = std::nullopt;
        }
        clause.rawPattern = token;
        return clause;
    } else if (startsWithNoCase(token, L"exact:")) {
        clause.filterType = SearchFilterType::Exact;
        token = token.substr(6);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"content:") || startsWithNoCase(token, L"内容:") || (startsWithNoCase(token, L"c:") && token.size() > 2 && token[2] != L'\\' && token[2] != L'/')) {
        clause.filterType = SearchFilterType::Content;
        if (startsWithNoCase(token, L"content:")) token = token.substr(8);
        else if (startsWithNoCase(token, L"内容:")) token = token.substr(3);
        else token = token.substr(2);
        stripOuterQuotes(token);
        clause.rawPattern = token;
        clause.pattern = SearchExpression::normalize(token);
        clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos || clause.pattern.find(L'?') != std::wstring::npos;
        return clause;
    } else if (startsWithNoCase(token, L"case:") || startsWithNoCase(token, L"cs:")) {
        clause.filterType = SearchFilterType::CaseSensitive;
        std::wstring sub = token.substr((token[0] == L'c' || token[0] == L'C') && (token[1] == L's' || token[1] == L'S') ? 3 : 5);
        stripOuterQuotes(sub);
        token = normalizePathSeparators(sub);
        clause.rawPattern = token;
        clause.pattern = SearchExpression::normalize(token);
        clause.hasWildcard = token.find(L'*') != std::wstring::npos || token.find(L'?') != std::wstring::npos;
        return clause;
    } else if (startsWithNoCase(token, L"pinyin:") || startsWithNoCase(token, L"py:")) {
        clause.filterType = SearchFilterType::PinyinOnly;
        token = token.substr((token[0] == L'p' || token[0] == L'P') && (token[1] == L'y' || token[1] == L'Y') ? 3 : 7);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"nopy:")) {
        clause.filterType = SearchFilterType::NoPinyin;
        token = token.substr(5);
        stripOuterQuotes(token);
    } else if (startsWithNoCase(token, L"dm:") || startsWithNoCase(token, L"datemodified:") ||
               startsWithNoCase(token, L"date:") || startsWithNoCase(token, L"modified:")) {
        const size_t colonPos = token.find(L':');
        std::wstring spec = (colonPos != std::wstring::npos) ? token.substr(colonPos + 1) : std::wstring();
        stripOuterQuotes(spec);
        parseDateModifiedClause(std::move(spec), clause);
        return clause;
    } else if (startsWithNoCase(token, L"size:")) {
        clause.filterType = SearchFilterType::Size;
        std::wstring s = token.substr(5);
        stripOuterQuotes(s);
        if (s.starts_with(L">=")) {
            clause.sizeOp = SearchClause::SizeOp::GreaterEqual;
            s = s.substr(2);
        } else if (s.starts_with(L"<=")) {
            clause.sizeOp = SearchClause::SizeOp::LessEqual;
            s = s.substr(2);
        } else if (s.starts_with(L">")) {
            clause.sizeOp = SearchClause::SizeOp::Greater;
            s = s.substr(1);
        } else if (s.starts_with(L"<")) {
            clause.sizeOp = SearchClause::SizeOp::Less;
            s = s.substr(1);
        } else if (s.starts_with(L"=")) {
            clause.sizeOp = SearchClause::SizeOp::Equal;
            s = s.substr(1);
        } else {
            clause.sizeOp = SearchClause::SizeOp::Equal;
        }

        double num = 0.0;
        size_t idx = 0;
        try {
            num = std::stod(s, &idx);
        } catch (...) {
            num = 0.0;
        }
        std::wstring unit = SearchExpression::normalize(s.substr(idx));
        uint64_t multiplier = 1;
        if (unit == L"kb" || unit == L"k") {
            multiplier = 1024ULL;
        } else if (unit == L"mb" || unit == L"m") {
            multiplier = 1024ULL * 1024ULL;
        } else if (unit == L"gb" || unit == L"g") {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else if (unit == L"tb" || unit == L"t") {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        }
        clause.sizeBytes = static_cast<uint64_t>(num * multiplier);
        return clause;
    }

    stripOuterQuotes(token);
    token = normalizePathSeparators(token);
    clause.rawPattern = token;
    clause.pattern = SearchExpression::normalize(token);
    clause.hasWildcard = clause.pattern.find(L'*') != std::wstring::npos || clause.pattern.find(L'?') != std::wstring::npos;

    clause.pinyinPattern.reserve(clause.pattern.size());
    clause.isAsciiOnly = !clause.pattern.empty();
    for (wchar_t ch : clause.pattern) {
        if (ch >= L'a' && ch <= L'z') {
            clause.pinyinPattern.push_back(ch);
        } else if (ch == L'\'' || ch == L'-') {
            // 忽略拼音音节分隔符 (如 tong'xi -> tongxi)
            continue;
        } else {
            clause.isAsciiOnly = false;
        }
    }
    if (clause.pinyinPattern.empty()) {
        clause.isAsciiOnly = false;
    }
    return clause;
}

static std::wstring expandEnvStrings(const std::wstring& input) {
    if (input.find(L'%') == std::wstring::npos) return input;
    DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0) return input;
    std::wstring expanded(needed, L'\0');
    DWORD result = ExpandEnvironmentStringsW(input.c_str(), &expanded[0], needed);
    if (result == 0 || result > needed) return input;
    expanded.resize(result - 1);
    return expanded;
}

} // namespace

void SearchExpression::recomputePathRequirement(const SearchParseOptions& options) {
    m_requiresFullPath = options.matchPath;
    size_t nonContentNameClauseCount = 0;

    for (const auto& group : m_orGroups) {
        for (const auto& clause : group.clauses) {
            if (clause.filterType == SearchFilterType::Content) {
                continue;
            }
            if (clause.filterType == SearchFilterType::Path ||
                clause.filterType == SearchFilterType::Parent ||
                clause.filterType == SearchFilterType::Size ||
                clause.filterType == SearchFilterType::DateModified) {
                m_requiresFullPath = true;
                continue;
            }
            if (clause.filterType == SearchFilterType::Extension ||
                clause.filterType == SearchFilterType::Drive) {
                continue;
            }
            if (clause.rawPattern.find(L'\\') != std::wstring::npos ||
                clause.rawPattern.find(L'/') != std::wstring::npos ||
                clause.hasWildcard) {
                m_requiresFullPath = true;
            }
            if (clause.isNegated && !clause.pattern.empty()) {
                m_requiresFullPath = true;
            }
            if (!clause.pattern.empty()) {
                ++nonContentNameClauseCount;
            }
        }
    }

    if (nonContentNameClauseCount >= 2) {
        m_requiresFullPath = true;
    }
}

SearchExpression SearchExpression::parse(const std::wstring& query, const SearchParseOptions& options) {
    std::wstring workingQuery = expandEnvStrings(query);
    SearchExpression expr;
    expr.m_rawQuery = query;
    expr.m_normalizedQuery = normalize(workingQuery);
    expr.m_pinyinEnabled = options.pinyinEnabled;
    expr.m_caseSensitive = options.caseSensitive;

    std::vector<RawQueryToken> rawTokens;
    size_t i = 0;
    while (i < workingQuery.size()) {
        while (i < workingQuery.size() && iswspace(workingQuery[i])) ++i;
        if (i >= workingQuery.size()) break;

        if (workingQuery[i] == L'"') {
            size_t start = ++i;
            while (i < workingQuery.size() && workingQuery[i] != L'"') ++i;
            std::wstring quoted = workingQuery.substr(start, i - start);
            if (i < workingQuery.size()) ++i;
            // If immediately followed by non-space characters (unlikely, e.g. "foo"bar), append them as unquoted
            if (i < workingQuery.size() && !iswspace(workingQuery[i])) {
                size_t tailStart = i;
                bool inQuotes = false;
                while (i < workingQuery.size() && (inQuotes || !iswspace(workingQuery[i]))) {
                    if (workingQuery[i] == L'"') inQuotes = !inQuotes;
                    ++i;
                }
                quoted += workingQuery.substr(tailStart, i - tailStart);
                rawTokens.push_back(RawQueryToken{std::move(quoted), false});
            } else {
                rawTokens.push_back(RawQueryToken{std::move(quoted), true});
            }
        } else {
            size_t start = i;
            bool inQuotes = false;
            while (i < workingQuery.size() && (inQuotes || !iswspace(workingQuery[i]))) {
                if (workingQuery[i] == L'"') {
                    inQuotes = !inQuotes;
                }
                ++i;
            }
            rawTokens.push_back(RawQueryToken{workingQuery.substr(start, i - start), false});
        }
    }

    bool nextIsOr = false;
    bool inContentMode = false;
    bool awaitingContentOperand = false;

    auto appendContentTerm = [&](const std::wstring& term) {
        if (term.empty()) return;
        expr.m_contentTerms.push_back(term);
        if (expr.m_contentQuery.empty()) {
            expr.m_contentQuery = term;
        } else {
            expr.m_contentQuery += L" " + term;
        }
    };

    for (size_t idx = 0; idx < rawTokens.size(); ++idx) {
        const auto& rawTok = rawTokens[idx];
        const auto& token = rawTok.text;
        if (!rawTok.quotedLiteral && (token == L"|" || token == L"OR" || token == L"or")) {
            nextIsOr = true;
            inContentMode = false;
            awaitingContentOperand = false;
            continue;
        }
        const std::wstring lowerToken = normalize(token);
        if (!rawTok.quotedLiteral && (lowerToken.starts_with(L"sort:") || lowerToken.starts_with(L"orderby:"))) {
            continue;
        }

        SearchClause clause = parseSingleToken(rawTok, options);
        if (clause.filterType == SearchFilterType::Content) {
            inContentMode = true;
            expr.m_hasContentFilter = true;
            if (!clause.rawPattern.empty()) {
                appendContentTerm(clause.rawPattern);
                awaitingContentOperand = false;
            } else {
                // Space after colon (e.g. "content: 合同" or "内容: 合同"): absorb the next plain operand
                awaitingContentOperand = true;
            }
        } else if (inContentMode && clause.filterType == SearchFilterType::None && !clause.isNegated &&
                   (awaitingContentOperand ||
                    (token.find(L'\\') == std::wstring::npos && !clause.hasWildcard))) {
            // Absorb plain non-operator words following content: (e.g. "content: 合同", "content:同心 账号密码")
            appendContentTerm(token);
            clause.rawPattern = token;
            clause.pattern = normalize(token);
            clause.filterType = SearchFilterType::Content;
            awaitingContentOperand = false;
        } else {
            if (clause.filterType != SearchFilterType::None && !awaitingContentOperand) {
                inContentMode = false;
            }
        }

        // 收集用于文件名相关性排序的纯净关键词（剥离 ext:/size:/dm:/c: 等过滤算子）
        if (!clause.isNegated && !clause.pattern.empty() && !clause.hasWildcard &&
            clause.pattern.find(L'\\') == std::wstring::npos &&
            clause.pattern.find(L'/') == std::wstring::npos &&
            (clause.filterType == SearchFilterType::None ||
             clause.filterType == SearchFilterType::FileOnly ||
             clause.filterType == SearchFilterType::FolderOnly ||
             clause.filterType == SearchFilterType::Exact ||
             clause.filterType == SearchFilterType::CaseSensitive ||
             clause.filterType == SearchFilterType::PinyinOnly ||
             clause.filterType == SearchFilterType::NoPinyin)) {
            if (!expr.m_rankingQuery.empty()) {
                expr.m_rankingQuery += L' ';
            }
            expr.m_rankingQuery += clause.pattern;
        }

        if (nextIsOr && !expr.m_orGroups.empty()) {
            expr.m_orGroups.back().clauses.push_back(std::move(clause));
            nextIsOr = false;
        } else {
            SearchOrGroup newGroup;
            newGroup.clauses.push_back(std::move(clause));
            expr.m_orGroups.push_back(std::move(newGroup));
            nextIsOr = false;
        }
    }

    expr.recomputePathRequirement(options);

    const std::wstring& effectiveRankBasis = expr.m_rankingQuery.empty() ? expr.m_normalizedQuery : expr.m_rankingQuery;
    expr.m_isAsciiQuery = !effectiveRankBasis.empty() &&
        std::all_of(effectiveRankBasis.begin(), effectiveRankBasis.end(),
                    [](wchar_t c) { return static_cast<unsigned int>(c) < 128; });
    if (expr.m_isAsciiQuery) {
        expr.m_cleanAsciiQuery.reserve(effectiveRankBasis.size());
        for (wchar_t ch : effectiveRankBasis) {
            if (ch != L'\'' && ch != L'-') expr.m_cleanAsciiQuery.push_back(ch);
        }
    }

    return expr;
}

ContentSearchPlan SearchExpression::buildContentPlan(
    const std::wstring& query,
    bool implicitContentMode,
    const SearchParseOptions& options
) {
    ContentSearchPlan plan;
    SearchExpression expr = SearchExpression::parse(query, options);
    plan.caseSensitive = options.caseSensitive;

    if (expr.hasContentFilter()) {
        plan.hasExplicitContentSyntax = true;
        plan.contentTerms = expr.getContentTerms();
        plan.fileFilterExpr = std::move(expr);
        return plan;
    }

    if (!implicitContentMode) {
        plan.fileFilterExpr = std::move(expr);
        return plan;
    }

    // Implicit content mode (searchMode == "content" or content leg of "both" without explicit content: prefix):
    // Keep file-scope filters (ext:, Drive, Path, Parent, Size, DateModified, FileOnly, FolderOnly, Regex, Exact,
    // wildcards like *.pdf, and negated exclusions like !temp / !node_modules) as candidate file filters, while
    // converting plain positive free terms / quoted phrases into contentTerms for document body matching.
    expr.m_hasContentFilter = true;
    expr.m_contentQuery.clear();
    expr.m_contentTerms.clear();

    for (auto& group : expr.m_orGroups) {
        for (auto& clause : group.clauses) {
            const bool isPlainFreeTerm =
                !clause.isNegated &&
                !clause.hasWildcard &&
                clause.rawPattern.find(L'\\') == std::wstring::npos &&
                (clause.filterType == SearchFilterType::None ||
                 clause.filterType == SearchFilterType::CaseSensitive ||
                 clause.filterType == SearchFilterType::PinyinOnly ||
                 clause.filterType == SearchFilterType::NoPinyin);

            if (isPlainFreeTerm) {
                if (clause.filterType == SearchFilterType::CaseSensitive) {
                    plan.caseSensitive = true;
                }
                if (!clause.rawPattern.empty()) {
                    plan.contentTerms.push_back(clause.rawPattern);
                    expr.m_contentTerms.push_back(clause.rawPattern);
                    if (expr.m_contentQuery.empty()) {
                        expr.m_contentQuery = clause.rawPattern;
                    } else {
                        expr.m_contentQuery += L" " + clause.rawPattern;
                    }
                }
                clause.filterType = SearchFilterType::Content;
            }
        }
    }

    expr.recomputePathRequirement(options);
    plan.fileFilterExpr = std::move(expr);
    return plan;
}

namespace {

inline bool containsIgnoreCase(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    const size_t maxPos = haystack.size() - needle.size();
    const wchar_t firstNeedle = static_cast<wchar_t>(std::towlower(needle[0]));
    for (size_t i = 0; i <= maxPos; ++i) {
        if (static_cast<wchar_t>(std::towlower(haystack[i])) == firstNeedle) {
            bool match = true;
            for (size_t j = 1; j < needle.size(); ++j) {
                if (static_cast<wchar_t>(std::towlower(haystack[i + j])) != static_cast<wchar_t>(std::towlower(needle[j]))) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

inline bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(a[i])) != static_cast<wchar_t>(std::towlower(b[i]))) return false;
    }
    return true;
}

inline bool startsWithIgnoreCase(std::wstring_view str, std::wstring_view prefix) noexcept {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(str[i])) != static_cast<wchar_t>(std::towlower(prefix[i]))) return false;
    }
    return true;
}

bool groupRequiresDiskStat(const SearchOrGroup& group) noexcept {
    for (const auto& clause : group.clauses) {
        if (clause.filterType == SearchFilterType::Size ||
            clause.filterType == SearchFilterType::DateModified) {
            return true;
        }
    }
    return false;
}

bool matchTextSegment(std::wstring_view text, std::wstring_view normalizedText,
                      const SearchClause& clause, bool caseSensitive) {
    if (caseSensitive) {
        return clause.hasWildcard
            ? SearchExpression::matchWildcard(clause.rawPattern, text, true)
            : (text.find(clause.rawPattern) != std::wstring_view::npos);
    }
    const std::wstring_view target = normalizedText.empty() ? text : normalizedText;
    return clause.hasWildcard
        ? SearchExpression::matchWildcard(clause.pattern, target, false)
        : containsIgnoreCase(target, clause.pattern);
}

bool matchPathSegment(std::wstring_view fullPath, const SearchClause& clause, bool caseSensitive) {
    if (fullPath.empty()) return false;
    std::wstring slashNormPath = normalizePathSeparators(fullPath);
    if (caseSensitive) {
        std::wstring pat = clause.rawPattern;
        if (slashNormPath.size() >= 2 && std::iswalpha(slashNormPath[0]) && slashNormPath[1] == L':' &&
            pat.size() >= 2 && std::iswalpha(pat[0]) && pat[1] == L':') {
            slashNormPath[0] = static_cast<wchar_t>(std::towupper(slashNormPath[0]));
            pat[0] = static_cast<wchar_t>(std::towupper(pat[0]));
        }
        return clause.hasWildcard
            ? SearchExpression::matchWildcard(pat, slashNormPath, true)
            : (slashNormPath.find(pat) != std::wstring::npos);
    }
    std::wstring normPath = SearchExpression::normalize(slashNormPath);
    return clause.hasWildcard
        ? SearchExpression::matchWildcard(clause.pattern, normPath, false)
        : containsIgnoreCase(normPath, clause.pattern);
}

}  // namespace

template <typename PathSupplier, typename AttrSupplier>
static bool matchSingleClauseLazy(const SearchClause& clause, const FileRecord& record,
                                  wchar_t driveLetter, PathSupplier&& getPath,
                                  AttrSupplier&& getAttr, bool requiresFullPath,
                                  bool pinyinEnabled, bool caseSensitive) {
    bool matched = false;

    switch (clause.filterType) {
        case SearchFilterType::FileOnly:
            if (record.isDirectory) matched = false;
            else if (clause.pattern.empty()) matched = true;
            else {
                matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
                if (!matched && requiresFullPath) {
                    matched = matchPathSegment(getPath(), clause, caseSensitive);
                }
            }
            break;

        case SearchFilterType::FolderOnly:
            if (!record.isDirectory) matched = false;
            else if (clause.pattern.empty()) matched = true;
            else {
                matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
                if (!matched && requiresFullPath) {
                    matched = matchPathSegment(getPath(), clause, caseSensitive);
                }
            }
            break;

        case SearchFilterType::Extension: {
            if (record.isDirectory) {
                matched = false;
                break;
            }
            const auto dotPos = record.fileName.rfind(L'.');
            if (dotPos == std::wstring_view::npos) {
                matched = false;
                break;
            }
            std::wstring_view fileExt = record.fileName.substr(dotPos + 1);
            for (const auto& ext : clause.extList) {
                if (equalsIgnoreCase(fileExt, ext)) {
                    matched = true;
                    break;
                }
            }
            break;
        }

        case SearchFilterType::Drive:
            matched = (std::towupper(driveLetter) == std::towupper(clause.driveLetter));
            break;

        case SearchFilterType::Path:
            matched = matchPathSegment(getPath(), clause, caseSensitive);
            break;

        case SearchFilterType::Parent: {
            const std::wstring slashPath = normalizePathSeparators(getPath());
            const auto slashPos = slashPath.rfind(L'\\');
            if (slashPos != std::wstring::npos) {
                std::wstring parentPath = slashPath.substr(0, slashPos);
                while (parentPath.size() > 2 && parentPath.back() == L'\\') {
                    parentPath.pop_back();
                }
                if (clause.parentDriveRootOnly) {
                    matched = equalsIgnoreCase(parentPath, clause.pattern);
                } else if (clause.hasWildcard) {
                    if (caseSensitive) {
                        std::wstring pat = clause.rawPattern;
                        if (parentPath.size() >= 2 && std::iswalpha(parentPath[0]) && parentPath[1] == L':' &&
                            pat.size() >= 2 && std::iswalpha(pat[0]) && pat[1] == L':') {
                            parentPath[0] = static_cast<wchar_t>(std::towupper(parentPath[0]));
                            pat[0] = static_cast<wchar_t>(std::towupper(pat[0]));
                        }
                        matched = SearchExpression::matchWildcard(pat, parentPath, true);
                    } else {
                        matched = SearchExpression::matchWildcard(clause.pattern, parentPath, false);
                    }
                } else if (clause.pattern.find(L'\\') != std::wstring::npos) {
                    const bool hasDrivePrefix = clause.rawPattern.size() >= 2 &&
                                                std::iswalpha(clause.rawPattern[0]) &&
                                                clause.rawPattern[1] == L':';
                    if (hasDrivePrefix) {
                        if (caseSensitive) {
                            matched = (parentPath.size() == clause.rawPattern.size()) &&
                                      (std::towupper(parentPath[0]) == std::towupper(clause.rawPattern[0])) &&
                                      (std::wstring_view(parentPath).substr(1) == std::wstring_view(clause.rawPattern).substr(1));
                        } else {
                            matched = equalsIgnoreCase(parentPath, clause.pattern);
                        }
                    } else {
                        matched = caseSensitive
                            ? (parentPath.find(clause.rawPattern) != std::wstring::npos)
                            : containsIgnoreCase(parentPath, clause.pattern);
                    }
                } else {
                    const auto parentSlash = parentPath.rfind(L'\\');
                    const std::wstring_view directParent = (parentSlash != std::wstring::npos)
                        ? std::wstring_view(parentPath).substr(parentSlash + 1)
                        : std::wstring_view{};
                    if (caseSensitive) {
                        matched = !directParent.empty() && directParent.find(clause.rawPattern) != std::wstring_view::npos;
                    } else {
                        matched = !directParent.empty() && containsIgnoreCase(directParent, clause.pattern);
                    }
                }
            }
            break;
        }

        case SearchFilterType::Exact:
            matched = caseSensitive
                ? (record.fileName == clause.rawPattern)
                : equalsIgnoreCase(record.fileName, clause.pattern);
            break;

        case SearchFilterType::Regex:
            if (clause.regexObj.has_value()) {
                matched = std::regex_search(record.fileName.begin(), record.fileName.end(),
                                            clause.regexObj.value());
                if (!matched && requiresFullPath) {
                    const std::wstring slashPath = normalizePathSeparators(getPath());
                    matched = std::regex_search(slashPath, clause.regexObj.value());
                }
            }
            break;

        case SearchFilterType::CaseSensitive:
            matched = matchTextSegment(record.fileName, record.normalizedName, clause, true);
            if (!matched && requiresFullPath) {
                matched = matchPathSegment(getPath(), clause, true);
            }
            break;

        case SearchFilterType::PinyinOnly:
            if (clause.hasWildcard) {
                const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                matched = SearchExpression::matchWildcard(pat, record.pinyinFull) ||
                          SearchExpression::matchWildcard(pat, record.pinyinInitials);
            } else {
                const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                matched = (containsIgnoreCase(record.pinyinInitials, pat) ||
                           containsIgnoreCase(record.pinyinFull, pat));
            }
            break;

        case SearchFilterType::NoPinyin:
            matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
            if (!matched && requiresFullPath) {
                matched = matchPathSegment(getPath(), clause, caseSensitive);
            }
            break;

        case SearchFilterType::Size:
            if (record.isDirectory) {
                matched = false;
            } else {
                uint64_t effectiveSize = record.fileSize;
                if (effectiveSize == 0) {
                    if (const WIN32_FILE_ATTRIBUTE_DATA* attr = getAttr()) {
                        if ((attr->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                            effectiveSize = (static_cast<uint64_t>(attr->nFileSizeHigh) << 32) | attr->nFileSizeLow;
                        }
                    }
                }
                switch (clause.sizeOp) {
                    case SearchClause::SizeOp::Equal:
                        matched = (effectiveSize == clause.sizeBytes);
                        break;
                    case SearchClause::SizeOp::Greater:
                        matched = (effectiveSize > clause.sizeBytes);
                        break;
                    case SearchClause::SizeOp::GreaterEqual:
                        matched = (effectiveSize >= clause.sizeBytes);
                        break;
                    case SearchClause::SizeOp::Less:
                        matched = (effectiveSize < clause.sizeBytes);
                        break;
                    case SearchClause::SizeOp::LessEqual:
                        matched = (effectiveSize <= clause.sizeBytes);
                        break;
                }
            }
            break;

        case SearchFilterType::DateModified:
            if (!clause.hasValidDateRange) {
                matched = true;
            } else {
                uint64_t effectiveWriteTime = record.lastWriteTime;
                if (effectiveWriteTime == 0) {
                    if (const WIN32_FILE_ATTRIBUTE_DATA* attr = getAttr()) {
                        ULARGE_INTEGER uli{};
                        uli.LowPart = attr->ftLastWriteTime.dwLowDateTime;
                        uli.HighPart = attr->ftLastWriteTime.dwHighDateTime;
                        effectiveWriteTime = uli.QuadPart;
                    }
                }
                matched = (effectiveWriteTime > 0 &&
                           effectiveWriteTime >= clause.dateMinFileTime &&
                           effectiveWriteTime <= clause.dateMaxFileTime);
            }
            break;

        case SearchFilterType::Content:
            // Content 过滤在内存初筛阶段始终放行候选文件，后续由内容解析引擎穿透
            matched = true;
            break;

        case SearchFilterType::None:
        default: {
            // 1. 优先在文件名中匹配 (零路径回溯，极致毫秒级热路径)
            matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
            if (!matched && pinyinEnabled && clause.isAsciiOnly) {
                const bool allowPinyinFallback = !caseSensitive ||
                    (clause.rawPattern == clause.pattern &&
                     !(clause.hasWildcard
                       ? SearchExpression::matchWildcard(clause.pattern, record.fileName, false)
                       : containsIgnoreCase(record.fileName, clause.pattern)));
                if (allowPinyinFallback) {
                    const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                    if (clause.hasWildcard) {
                        matched = SearchExpression::matchWildcard(pat, record.pinyinFull) ||
                                  SearchExpression::matchWildcard(pat, record.pinyinInitials);
                    } else {
                        matched = containsIgnoreCase(record.pinyinInitials, pat) ||
                                  containsIgnoreCase(record.pinyinFull, pat);
                    }
                }
            }

            // 2. 仅当检索表达式需要全路径 (如开启 matchPath 偏好、包含多词组、斜杠或 path: 前缀) 且文件名未命中时，才兜底在路径中匹配
            if (!matched && requiresFullPath) {
                matched = matchPathSegment(getPath(), clause, caseSensitive);
            }
            break;
        }
    }

    return clause.isNegated ? !matched : matched;
}

bool SearchExpression::matchesWithLazyPath(const FileRecord& record, wchar_t driveLetter,
                                           const PathGetter& getFullPath,
                                           const StatHydrateCallback& onStatHydrated) const {
    if (m_orGroups.empty()) return true;

    std::wstring cachedFullPath;
    bool pathLoaded = false;
    auto getOrFetchPath = [&]() -> const std::wstring& {
        if (!pathLoaded) {
            if (getFullPath) {
                cachedFullPath = getFullPath();
            }
            pathLoaded = true;
        }
        return cachedFullPath;
    };

    WIN32_FILE_ATTRIBUTE_DATA cachedAttrData{};
    bool attrLoaded = false;
    bool attrValid = false;
    auto getOrFetchAttr = [&]() -> const WIN32_FILE_ATTRIBUTE_DATA* {
        if (!attrLoaded) {
            attrLoaded = true;
            const std::wstring& fullPath = getOrFetchPath();
            if (!fullPath.empty()) {
                attrValid = (GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &cachedAttrData) != FALSE);
                if (attrValid && onStatHydrated && record.fileReferenceNumber != 0) {
                    const uint64_t fSize =
                        (static_cast<uint64_t>(cachedAttrData.nFileSizeHigh) << 32) |
                        static_cast<uint64_t>(cachedAttrData.nFileSizeLow);
                    ULARGE_INTEGER cTime{};
                    cTime.LowPart = cachedAttrData.ftCreationTime.dwLowDateTime;
                    cTime.HighPart = cachedAttrData.ftCreationTime.dwHighDateTime;
                    ULARGE_INTEGER mTime{};
                    mTime.LowPart = cachedAttrData.ftLastWriteTime.dwLowDateTime;
                    mTime.HighPart = cachedAttrData.ftLastWriteTime.dwHighDateTime;
                    onStatHydrated(record.fileReferenceNumber, fSize, cTime.QuadPart, mTime.QuadPart);
                }
            }
        }
        return attrValid ? &cachedAttrData : nullptr;
    };

    // Pass 1: 优先执行不需要磁盘元数据探测 (Size / DateModified) 的纯内存过滤组，纳秒级淘汰 99.9% 的无关记录
    bool hasDiskStatGroups = false;
    for (const auto& group : m_orGroups) {
        if (groupRequiresDiskStat(group)) {
            hasDiskStatGroups = true;
            continue;
        }
        bool groupPassed = false;
        for (const auto& clause : group.clauses) {
            if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getOrFetchAttr,
                                      m_requiresFullPath, m_pinyinEnabled, m_caseSensitive)) {
                groupPassed = true;
                break;
            }
        }
        if (!groupPassed) return false;
    }

    // Pass 2: 仅对通过内存过滤的候选记录执行 Size / DateModified 过滤（若内存记录未缓存文件大小或修改时间则按需单次调用 GetFileAttributesExW）
    if (hasDiskStatGroups) {
        for (const auto& group : m_orGroups) {
            if (!groupRequiresDiskStat(group)) continue;
            bool groupPassed = false;
            for (const auto& clause : group.clauses) {
                if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getOrFetchAttr,
                                          m_requiresFullPath, m_pinyinEnabled, m_caseSensitive)) {
                    groupPassed = true;
                    break;
                }
            }
            if (!groupPassed) return false;
        }
    }
    return true;
}

bool SearchExpression::matches(const FileRecord& record, wchar_t driveLetter,
                               const std::wstring& fullPath) const {
    return matchesWithLazyPath(record, driveLetter, [&fullPath]() {
        return fullPath;
    });
}

int SearchExpression::calculateRank(const FileRecord& record) const {
    if (m_hasContentFilter && !m_contentQuery.empty()) {
        if (m_caseSensitive) {
            return record.fileName.find(m_contentQuery) != std::wstring_view::npos ? 0 : 1;
        }
        std::wstring normContentQuery = normalize(m_contentQuery);
        if (containsIgnoreCase(record.fileName, normContentQuery)) {
            return 0;
        }
        return 1;
    }
    const std::wstring& targetQuery = m_rankingQuery.empty() ? m_normalizedQuery : m_rankingQuery;
    if (targetQuery.empty()) return 6;
    if (equalsIgnoreCase(record.fileName, targetQuery)) return 0;
    if (startsWithIgnoreCase(record.fileName, targetQuery)) return 1;
    if (m_pinyinEnabled && m_isAsciiQuery) {
        const auto& q = m_cleanAsciiQuery.empty() ? targetQuery : m_cleanAsciiQuery;
        if (startsWithIgnoreCase(record.pinyinFull, q)) return 2;
        if (startsWithIgnoreCase(record.pinyinInitials, q)) return 3;
        if (containsIgnoreCase(record.fileName, targetQuery)) return 4;
        if (containsIgnoreCase(record.pinyinFull, q)) return 5;
        if (containsIgnoreCase(record.pinyinInitials, q)) return 6;
    } else {
        if (containsIgnoreCase(record.fileName, targetQuery)) return 4;
    }
    return 7;
}
