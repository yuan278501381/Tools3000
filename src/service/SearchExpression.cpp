#include "SearchExpression.h"
#include "SearchRequestLimits.h"
#include "content/ContentSearchEngine.h"
#include "content/RustContentKernel.h"
#include <userenv.h>
#include <algorithm>
#include <cwctype>
#include <sstream>

namespace {

inline wchar_t fastTowLower(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
    if (ch < 0x80 || (ch >= 0x2000 && ch <= 0x9FFF)) {
        return ch;
    }
    return static_cast<wchar_t>(std::towlower(ch));
}

inline bool isCaselessWideChar(wchar_t ch) noexcept {
    if (ch >= 0x2000 && ch <= 0x9FFF) {
        return true;
    }
    if (ch < 0x80) {
        return !(ch >= L'a' && ch <= L'z') && !(ch >= L'A' && ch <= L'Z');
    }
    return false;
}

inline bool computeIsCaselessPattern(std::wstring_view pat) noexcept {
    if (pat.empty()) return false;
    for (wchar_t ch : pat) {
        if (!isCaselessWideChar(ch)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::wstring SearchExpression::normalize(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        result.push_back(fastTowLower(ch));
    }
    return result;
}

bool SearchExpression::matchWildcard(std::wstring_view pattern, std::wstring_view text, bool caseSensitive) {
    size_t p = 0, t = 0;
    size_t starP = std::wstring_view::npos, starT = 0;
    while (t < text.size()) {
        const wchar_t cp = (p < pattern.size())
            ? (caseSensitive ? pattern[p] : fastTowLower(pattern[p]))
            : L'\0';
        const wchar_t ct = caseSensitive ? text[t] : fastTowLower(text[t]);
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
            clause.linearRegex.reset();
            clause.regexInvalid = true;
            return clause;
        }
        // Primary (`HIGH-14`): compile into Rust linear-time O(N) Thompson NFA engine
        // (`t3k_rs_regex_compile`), immune to catastrophic backtracking ReDoS and stack overflow.
        uint32_t compileStatus = 0;
        if (!token.empty()) {
            T3kRegexHandle* rawHandle = t3k_rs_regex_compile(
                reinterpret_cast<const uint16_t*>(token.data()),
                token.size(),
                options.caseSensitive ? 1u : 0u,
                &compileStatus);
            if (rawHandle != nullptr) {
                clause.linearRegex = std::shared_ptr<T3kRegexHandle>(
                    rawHandle,
                    [](T3kRegexHandle* h) noexcept {
                        if (h != nullptr) {
                            t3k_rs_regex_free(h);
                        }
                    });
            }
        }
        if (!clause.linearRegex) {
            if (!token.empty() && compileStatus == 0) {
                clause.regexObj = std::nullopt;
                clause.regexInvalid = true;
            } else {
                try {
                    const auto regexFlags = options.caseSensitive
                        ? std::regex::optimize
                        : (std::regex::icase | std::regex::optimize);
                    clause.regexObj = std::wregex(token, regexFlags);
                } catch (...) {
                    clause.regexObj = std::nullopt;
                    clause.regexInvalid = true;
                }
            }
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
        const auto parseSizeValue = [](const std::wstring& expr) -> uint64_t {
            std::wstring clean = SearchExpression::normalize(expr);
            if (clean == L"empty" || clean == L"0") return 0ULL;
            if (clean == L"tiny") return 10ULL * 1024ULL;
            if (clean == L"small") return 100ULL * 1024ULL;
            if (clean == L"medium") return 1024ULL * 1024ULL;
            if (clean == L"large") return 16ULL * 1024ULL * 1024ULL;
            if (clean == L"huge" || clean == L"gigantic") return 128ULL * 1024ULL * 1024ULL;
            double num = 0.0;
            size_t idx = 0;
            try {
                num = std::stod(clean, &idx);
            } catch (...) {
                num = 0.0;
            }
            std::wstring unit = SearchExpression::normalize(clean.substr(idx));
            uint64_t multiplier = 1ULL;
            if (unit == L"kb" || unit == L"k") {
                multiplier = 1024ULL;
            } else if (unit == L"mb" || unit == L"m") {
                multiplier = 1024ULL * 1024ULL;
            } else if (unit == L"gb" || unit == L"g") {
                multiplier = 1024ULL * 1024ULL * 1024ULL;
            } else if (unit == L"tb" || unit == L"t") {
                multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
            }
            return static_cast<uint64_t>(num * static_cast<double>(multiplier));
        };

        const size_t rangeDots = s.find(L"..");
        const size_t rangeDash = (rangeDots == std::wstring::npos && s.size() > 2 && s[0] != L'-')
            ? s.find(L'-', 1)
            : std::wstring::npos;
        if (rangeDots != std::wstring::npos || rangeDash != std::wstring::npos) {
            const size_t splitPos = (rangeDots != std::wstring::npos) ? rangeDots : rangeDash;
            const size_t sepLen = (rangeDots != std::wstring::npos) ? 2 : 1;
            clause.sizeOp = SearchClause::SizeOp::Range;
            clause.sizeBytes = parseSizeValue(s.substr(0, splitPos));
            clause.sizeMaxBytes = parseSizeValue(s.substr(splitPos + sepLen));
            if (clause.sizeMaxBytes < clause.sizeBytes) {
                std::swap(clause.sizeBytes, clause.sizeMaxBytes);
            }
            return clause;
        }

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
            const std::wstring cleanPreset = SearchExpression::normalize(s);
            if (cleanPreset == L"empty") {
                clause.sizeOp = SearchClause::SizeOp::Equal;
                clause.sizeBytes = 0ULL;
                return clause;
            }
            if (cleanPreset == L"tiny") {
                clause.sizeOp = SearchClause::SizeOp::Range;
                clause.sizeBytes = 1ULL;
                clause.sizeMaxBytes = 10ULL * 1024ULL;
                return clause;
            }
            if (cleanPreset == L"small") {
                clause.sizeOp = SearchClause::SizeOp::Range;
                clause.sizeBytes = 10ULL * 1024ULL;
                clause.sizeMaxBytes = 100ULL * 1024ULL;
                return clause;
            }
            if (cleanPreset == L"medium") {
                clause.sizeOp = SearchClause::SizeOp::Range;
                clause.sizeBytes = 100ULL * 1024ULL;
                clause.sizeMaxBytes = 16ULL * 1024ULL * 1024ULL;
                return clause;
            }
            if (cleanPreset == L"large") {
                clause.sizeOp = SearchClause::SizeOp::Range;
                clause.sizeBytes = 16ULL * 1024ULL * 1024ULL;
                clause.sizeMaxBytes = 128ULL * 1024ULL * 1024ULL;
                return clause;
            }
            if (cleanPreset == L"huge" || cleanPreset == L"gigantic") {
                clause.sizeOp = SearchClause::SizeOp::GreaterEqual;
                clause.sizeBytes = 128ULL * 1024ULL * 1024ULL;
                return clause;
            }
            clause.sizeOp = SearchClause::SizeOp::Equal;
        }

        clause.sizeBytes = parseSizeValue(s);
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

    // If the current thread is impersonating a pipe client user (`MED-13`), expand
    // against that user's environment block rather than the `LocalSystem` process block.
    HANDLE hThreadToken = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &hThreadToken) ||
        OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hThreadToken)) {
        constexpr DWORD kMaxExpanded = 4096;
        std::vector<wchar_t> userBuf(kMaxExpanded, L'\0');
        const BOOL ok = ExpandEnvironmentStringsForUserW(
            hThreadToken, input.c_str(), userBuf.data(), kMaxExpanded);
        CloseHandle(hThreadToken);
        if (ok) {
            userBuf.back() = L'\0';
            std::wstring userExpanded(userBuf.data());
            if (!userExpanded.empty()) {
                return userExpanded;
            }
        }
    }

    DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (needed == 0) return input;
    std::wstring expanded(needed, L'\0');
    DWORD result = ExpandEnvironmentStringsW(input.c_str(), &expanded[0], needed);
    if (result == 0 || result > needed) return input;
    expanded.resize(result - 1);

    // Guard against `LocalSystem` expanding `%APPDATA%` / `%USERPROFILE%` into `systemprofile`
    // when no user token is bound (`MED-13`).
    const std::wstring lowerExpanded = SearchExpression::normalize(expanded);
    if (lowerExpanded.find(L"\\system32\\config\\systemprofile") != std::wstring::npos) {
        const std::wstring lowerInput = SearchExpression::normalize(input);
        if (lowerInput.find(L"systemprofile") == std::wstring::npos) {
            return input;
        }
    }
    return expanded;
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

bool isPureSuffixWildcardPattern(std::wstring_view pat) noexcept {
    if (pat.size() < 3 || pat[0] != L'*' || pat[1] != L'.') {
        return false;
    }
    for (size_t i = 2; i < pat.size(); ++i) {
        const wchar_t c = pat[i];
        if (c == L'*' || c == L'?' || c == L'\\' || c == L'/' || c == L':') {
            return false;
        }
    }
    return true;
}

bool isPositivePureExtensionClause(const SearchClause& clause) noexcept {
    if (clause.isNegated) return false;
    if (clause.filterType == SearchFilterType::Extension) {
        return !clause.extList.empty();
    }
    if (clause.filterType == SearchFilterType::None ||
        clause.filterType == SearchFilterType::FileOnly ||
        clause.filterType == SearchFilterType::CaseSensitive ||
        clause.filterType == SearchFilterType::NoPinyin) {
        return isPureSuffixWildcardPattern(clause.rawPattern);
    }
    return false;
}

bool isClauseSeparableForDir(const SearchClause& clause, bool requiresFullPath) noexcept {
    if (clause.pattern.empty() || clause.hasWildcard || clause.hasPathSeparator) {
        return false;
    }
    if (clause.rawPattern.find(L':') != std::wstring::npos) {
        return false;
    }
    switch (clause.filterType) {
        case SearchFilterType::Path:
            return true;
        case SearchFilterType::None:
        case SearchFilterType::NoPinyin:
        case SearchFilterType::CaseSensitive:
        case SearchFilterType::FileOnly:
        case SearchFilterType::FolderOnly:
            return requiresFullPath;
        default:
            return false;
    }
}

bool isClauseRecordLocalOnly(const SearchClause& clause, bool requiresFullPath) noexcept {
    switch (clause.filterType) {
        case SearchFilterType::Extension:
        case SearchFilterType::Drive:
        case SearchFilterType::Exact:
        case SearchFilterType::PinyinOnly:
        case SearchFilterType::Content:
            return true;
        case SearchFilterType::FileOnly:
        case SearchFilterType::FolderOnly:
            return clause.pattern.empty() || !requiresFullPath || isPureSuffixWildcardPattern(clause.rawPattern);
        case SearchFilterType::None:
        case SearchFilterType::NoPinyin:
        case SearchFilterType::CaseSensitive:
            return !requiresFullPath || isPureSuffixWildcardPattern(clause.rawPattern);
        case SearchFilterType::Regex:
            return !requiresFullPath;
        default:
            return false;
    }
}

} // namespace

void SearchExpression::recomputePathRequirement(const SearchParseOptions& options) {
    m_requiresFullPath = options.matchPath;
    m_hasRegexClause = false;
    size_t nonContentNameGroupCount = 0;

    for (auto& group : m_orGroups) {
        bool groupHasNonContentName = false;
        for (auto& clause : group.clauses) {
            clause.hasSlashPreFilter = false;
            clause.slashAllowBranchA = false;
            clause.slashAllowBranchB = false;
            clause.slashLeafCaseSensitive = false;
            clause.slashBranchADirMask = 0;
            clause.slashBranchBDirMask = 0;
            clause.slashLeafPrefix.clear();
            clause.slashLeafSuffix.clear();

            if (clause.filterType == SearchFilterType::Regex) {
                m_hasRegexClause = true;
            }
            clause.hasPathSeparator =
                (clause.rawPattern.find(L'\\') != std::wstring::npos) ||
                (clause.rawPattern.find(L'/') != std::wstring::npos);
            clause.isCaselessPattern =
                !clause.hasWildcard && computeIsCaselessPattern(clause.pattern);

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
            const bool pureSuffixWildcard = isPureSuffixWildcardPattern(clause.rawPattern);
            if (clause.hasPathSeparator || (clause.hasWildcard && !pureSuffixWildcard)) {
                m_requiresFullPath = true;
            }
            if (clause.isNegated && !clause.pattern.empty()) {
                m_requiresFullPath = true;
            }
            if (!clause.pattern.empty()) {
                groupHasNonContentName = true;
            }
        }
        if (groupHasNonContentName) {
            ++nonContentNameGroupCount;
        }
    }

    if (nonContentNameGroupCount >= 2) {
        m_requiresFullPath = true;
    }

    m_hasPureExtensionGroup = false;
    m_hasFolderOnlyFilter = false;
    for (const auto& grp : m_orGroups) {
        for (const auto& c : grp.clauses) {
            if (!c.isNegated && c.filterType == SearchFilterType::FolderOnly) {
                m_hasFolderOnlyFilter = true;
                break;
            }
        }
        if (m_hasFolderOnlyFilter) break;
    }
    if (m_orGroups.size() >= 2) {
        for (const auto& grp : m_orGroups) {
            if (grp.clauses.empty()) continue;
            bool allPureExt = true;
            for (const auto& c : grp.clauses) {
                if (!isPositivePureExtensionClause(c)) {
                    allPureExt = false;
                    break;
                }
            }
            if (allPureExt) {
                m_hasPureExtensionGroup = true;
                break;
            }
        }
    }

    m_separableGroupMask = 0;
    m_negatedSeparableGroupMask = 0;
    m_dirFileSatisfiedMask = 0;
    m_dirFolderSatisfiedMask = 0;
    m_slashDirPredicates.clear();
    m_slashPredicateMask = 0;
    m_fullySeparable = false;

    const size_t groupCount = std::min<size_t>(m_orGroups.size(), 64);
    for (size_t g = 0; g < groupCount; ++g) {
        const auto& group = m_orGroups[g];
        if (group.clauses.empty() || groupRequiresDiskStat(group)) {
            continue;
        }
        const uint64_t bit = (1ULL << g);
        if (group.clauses.size() == 1) {
            const auto& c = group.clauses[0];
            if (isClauseSeparableForDir(c, m_requiresFullPath)) {
                if (c.isNegated) {
                    if (c.filterType == SearchFilterType::None ||
                        c.filterType == SearchFilterType::Path ||
                        c.filterType == SearchFilterType::NoPinyin ||
                        c.filterType == SearchFilterType::CaseSensitive) {
                        m_separableGroupMask |= bit;
                        m_negatedSeparableGroupMask |= bit;
                    }
                } else {
                    m_separableGroupMask |= bit;
                    if (c.filterType != SearchFilterType::FolderOnly) {
                        m_dirFileSatisfiedMask |= bit;
                    }
                    if (c.filterType != SearchFilterType::FileOnly) {
                        m_dirFolderSatisfiedMask |= bit;
                    }
                }
            } else if (isClauseRecordLocalOnly(c, m_requiresFullPath)) {
                m_separableGroupMask |= bit;
            }
        } else {
            bool allPositiveSeparableOrLocal = true;
            bool hasSeparable = false;
            for (const auto& c : group.clauses) {
                if (c.isNegated) {
                    allPositiveSeparableOrLocal = false;
                    break;
                }
                if (isClauseSeparableForDir(c, m_requiresFullPath)) {
                    if (c.filterType == SearchFilterType::FolderOnly ||
                        c.filterType == SearchFilterType::FileOnly) {
                        // 多子句 OR 组共享单一 group bit，若包含仅限文件或仅限目录的路径子句，回退至逐子句精确匹配以防跨类型串位
                        allPositiveSeparableOrLocal = false;
                        break;
                    }
                    hasSeparable = true;
                } else if (!isClauseRecordLocalOnly(c, m_requiresFullPath)) {
                    allPositiveSeparableOrLocal = false;
                    break;
                }
            }
            if (allPositiveSeparableOrLocal) {
                m_separableGroupMask |= bit;
                if (hasSeparable) {
                    m_dirFileSatisfiedMask |= bit;
                    m_dirFolderSatisfiedMask |= bit;
                }
            }
        }
    }

    // `U-05`: Precompile slash-separated path fragment clauses (`A\B\C`, `service\main.cpp`,
    // `parent:src\service`, etc.) into ancestor directory bitmask predicates + leaf name
    // prefix/suffix checks so non-matching records are rejected in O(1) without `buildFullPath`.
    if (m_requiresFullPath) {
        size_t nextPredBit = groupCount;
        auto allocDirPred = [&](DirSegMatchOp op, std::wstring_view needle, bool cs, uint64_t& outMask) -> bool {
            if (needle.empty()) {
                return true;
            }
            const bool isDriveToken =
                needle.size() == 2 && std::iswalpha(needle[0]) && needle[1] == L':';
            const bool effectiveCs = isDriveToken ? false : cs;
            std::wstring storedNeedle = effectiveCs ? std::wstring(needle) : normalize(needle);
            for (const auto& existing : m_slashDirPredicates) {
                if (existing.op == op &&
                    existing.caseSensitive == effectiveCs &&
                    existing.needle == storedNeedle) {
                    outMask |= (1ULL << existing.bitIndex);
                    return true;
                }
            }
            if (nextPredBit >= 64) {
                return false;
            }
            const uint8_t bitIdx = static_cast<uint8_t>(nextPredBit++);
            m_slashDirPredicates.push_back(DirSegmentPredicate{
                op,
                effectiveCs,
                bitIdx,
                std::move(storedNeedle),
            });
            const uint64_t bitMask = (1ULL << bitIdx);
            m_slashPredicateMask |= bitMask;
            outMask |= bitMask;
            return true;
        };

        auto splitBackslashSegments = [](std::wstring_view pat) {
            std::vector<std::wstring_view> segs;
            segs.reserve(4);
            size_t start = 0;
            while (true) {
                const size_t pos = pat.find(L'\\', start);
                if (pos == std::wstring_view::npos) {
                    segs.push_back(pat.substr(start));
                    break;
                }
                segs.push_back(pat.substr(start, pos - start));
                start = pos + 1;
            }
            return segs;
        };

        for (auto& group : m_orGroups) {
            for (auto& clause : group.clauses) {
                const bool effCs =
                    m_caseSensitive || (clause.filterType == SearchFilterType::CaseSensitive);
                const std::wstring_view effPat = effCs
                    ? std::wstring_view(clause.rawPattern)
                    : std::wstring_view(clause.pattern);

                if (clause.filterType == SearchFilterType::Parent &&
                    !clause.parentDriveRootOnly && !clause.hasWildcard &&
                    !effPat.empty() && effPat.find(L'/') == std::wstring_view::npos) {
                    if (effPat.find(L'\\') == std::wstring_view::npos) {
                        uint64_t mask = 0;
                        if (allocDirPred(DirSegMatchOp::Contains, effPat, effCs, mask) && mask != 0) {
                            clause.hasSlashPreFilter = true;
                            clause.slashAllowBranchA = true;
                            clause.slashAllowBranchB = false;
                            clause.slashBranchADirMask = mask;
                            clause.slashLeafCaseSensitive = effCs;
                        }
                    } else {
                        const auto segs = splitBackslashSegments(effPat);
                        bool validMiddle = segs.size() >= 2;
                        for (size_t i = 1; validMiddle && i + 1 < segs.size(); ++i) {
                            if (segs[i].empty()) validMiddle = false;
                        }
                        if (validMiddle) {
                            uint64_t mask = 0;
                            bool ok = allocDirPred(DirSegMatchOp::Suffix, segs.front(), effCs, mask);
                            for (size_t i = 1; ok && i + 1 < segs.size(); ++i) {
                                ok = allocDirPred(DirSegMatchOp::Exact, segs[i], effCs, mask);
                            }
                            if (ok) {
                                ok = allocDirPred(DirSegMatchOp::Prefix, segs.back(), effCs, mask);
                            }
                            if (ok && mask != 0) {
                                clause.hasSlashPreFilter = true;
                                clause.slashAllowBranchA = true;
                                clause.slashAllowBranchB = false;
                                clause.slashBranchADirMask = mask;
                                clause.slashLeafCaseSensitive = effCs;
                            }
                        }
                    }
                    continue;
                }

                const bool supportsSlashPath =
                    clause.hasPathSeparator &&
                    (clause.filterType == SearchFilterType::None ||
                     clause.filterType == SearchFilterType::Path ||
                     clause.filterType == SearchFilterType::FileOnly ||
                     clause.filterType == SearchFilterType::FolderOnly ||
                     clause.filterType == SearchFilterType::NoPinyin ||
                     clause.filterType == SearchFilterType::CaseSensitive);
                if (!supportsSlashPath ||
                    effPat.find(L'\\') == std::wstring_view::npos ||
                    effPat.find(L'/') != std::wstring_view::npos) {
                    continue;
                }

                const auto segs = splitBackslashSegments(effPat);
                if (segs.size() < 2) {
                    continue;
                }

                if (!clause.hasWildcard) {
                    bool validMiddle = true;
                    for (size_t i = 1; i + 1 < segs.size(); ++i) {
                        if (segs[i].empty()) {
                            validMiddle = false;
                            break;
                        }
                    }
                    if (!validMiddle) {
                        continue;
                    }
                    uint64_t prefixMask = 0;
                    bool ok = allocDirPred(DirSegMatchOp::Suffix, segs.front(), effCs, prefixMask);
                    for (size_t i = 1; ok && i + 1 < segs.size(); ++i) {
                        ok = allocDirPred(DirSegMatchOp::Exact, segs[i], effCs, prefixMask);
                    }
                    uint64_t branchAMask = prefixMask;
                    if (ok && !segs.back().empty()) {
                        ok = allocDirPred(DirSegMatchOp::Prefix, segs.back(), effCs, branchAMask);
                    }
                    if (ok) {
                        clause.hasSlashPreFilter = true;
                        clause.slashAllowBranchA = true;
                        clause.slashAllowBranchB = true;
                        clause.slashBranchADirMask = branchAMask;
                        clause.slashBranchBDirMask = prefixMask;
                        clause.slashLeafPrefix = std::wstring(segs.back());
                        clause.slashLeafCaseSensitive = effCs;
                    }
                } else {
                    uint64_t prefixMask = 0;
                    bool ok = true;
                    for (size_t i = 0; ok && i + 1 < segs.size(); ++i) {
                        if (!segs[i].empty() && segs[i].find_first_of(L"*?") == std::wstring_view::npos) {
                            ok = allocDirPred(
                                i == 0 ? DirSegMatchOp::Suffix : DirSegMatchOp::Exact,
                                segs[i], effCs, prefixMask);
                        }
                    }
                    const std::wstring_view lastSeg = segs.back();
                    const size_t lastWild = lastSeg.find_last_of(L"*?");
                    const std::wstring_view tailLit = (lastWild == std::wstring_view::npos)
                        ? lastSeg
                        : lastSeg.substr(lastWild + 1);
                    if (ok && (prefixMask != 0 || !tailLit.empty())) {
                        clause.hasSlashPreFilter = true;
                        clause.slashAllowBranchA = false;
                        clause.slashAllowBranchB = true;
                        clause.slashBranchBDirMask = prefixMask;
                        clause.slashLeafSuffix = std::wstring(tailLit);
                        clause.slashLeafCaseSensitive = effCs;
                    }
                }
            }
        }
    }

    if (!m_orGroups.empty() && m_orGroups.size() <= 64) {
        bool allSep = true;
        for (size_t g = 0; g < m_orGroups.size(); ++g) {
            if (groupRequiresDiskStat(m_orGroups[g]) || ((m_separableGroupMask >> g) & 1ULL) == 0) {
                allSep = false;
                break;
            }
        }
        m_fullySeparable = allSep;
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

    // 展开非引号、非 ext:/regex: 词元中的紧凑管道符 '|' (如 "苏州|无锡" 或 "folder:A|B")
    {
        auto isInternalPipePrefix = [](const std::wstring& s) noexcept {
            std::wstring_view sv(s);
            if (!sv.empty() && sv.front() == L'!') sv.remove_prefix(1);
            auto startsWithI = [](std::wstring_view text, std::wstring_view prefix) noexcept {
                if (text.size() < prefix.size()) return false;
                for (size_t k = 0; k < prefix.size(); ++k) {
                    if (std::towlower(text[k]) != prefix[k]) return false;
                }
                return true;
            };
            return startsWithI(sv, L"ext:") || startsWithI(sv, L"regex:") || startsWithI(sv, L"r:");
        };

        std::vector<RawQueryToken> expandedTokens;
        expandedTokens.reserve(rawTokens.size());
        for (auto& tok : rawTokens) {
            const bool isWildcardList =
                tok.text.starts_with(L"*.") &&
                (tok.text.find(L';') != std::wstring::npos || tok.text.find(L',') != std::wstring::npos);
            if (tok.quotedLiteral || tok.text == L"|" ||
                (tok.text.find(L'|') == std::wstring::npos && !isWildcardList) ||
                isInternalPipePrefix(tok.text)) {
                expandedTokens.push_back(std::move(tok));
                continue;
            }
            size_t segStart = 0;
            bool inQuotes = false;
            for (size_t pos = 0; pos < tok.text.size(); ++pos) {
                const wchar_t ch = tok.text[pos];
                if (ch == L'"') {
                    inQuotes = !inQuotes;
                } else if (!inQuotes && (ch == L'|' || (isWildcardList && (ch == L';' || ch == L',')))) {
                    if (pos > segStart) {
                        expandedTokens.push_back(RawQueryToken{tok.text.substr(segStart, pos - segStart), false});
                    }
                    expandedTokens.push_back(RawQueryToken{L"|", false});
                    segStart = pos + 1;
                }
            }
            if (segStart < tok.text.size()) {
                expandedTokens.push_back(RawQueryToken{tok.text.substr(segStart), false});
            }
        }
        rawTokens = std::move(expandedTokens);
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
            // Absorb plain non-operator words following content: (e.g. "content: 架构", "content:星云 部署手册")
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
            expr.m_rankingTerms.push_back(clause.pattern);
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

    // 互斥后缀智能并集归并 (Smart Multi-Extension Union Coalescing):
    // 当用户输入空格分隔的多个正向纯后缀词元（如 "*.log *.txt"、"同心 *.log *.txt" 或 "ext:log ext:txt"）时，
    // 由于单个文件末尾不可能同时拥有两个互斥后缀（AND 交集恒为空集），自动将其归并入同一 OR 组 (*.log | *.txt)。
    if (expr.m_orGroups.size() >= 2) {
        size_t firstPureExtGroup = std::wstring::npos;
        std::vector<SearchOrGroup> coalescedGroups;
        coalescedGroups.reserve(expr.m_orGroups.size());
        for (auto& grp : expr.m_orGroups) {
            const bool isSinglePureExt =
                (grp.clauses.size() == 1) && isPositivePureExtensionClause(grp.clauses[0]);
            if (isSinglePureExt) {
                if (firstPureExtGroup == std::wstring::npos) {
                    firstPureExtGroup = coalescedGroups.size();
                    coalescedGroups.push_back(std::move(grp));
                } else {
                    coalescedGroups[firstPureExtGroup].clauses.push_back(std::move(grp.clauses[0]));
                }
            } else {
                coalescedGroups.push_back(std::move(grp));
            }
        }
        expr.m_orGroups = std::move(coalescedGroups);
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
    const wchar_t firstNeedle = fastTowLower(needle[0]);
    if (isCaselessWideChar(firstNeedle)) {
        size_t pos = haystack.find(firstNeedle);
        while (pos != std::wstring_view::npos && pos <= maxPos) {
            bool match = true;
            for (size_t j = 1; j < needle.size(); ++j) {
                if (fastTowLower(haystack[pos + j]) != fastTowLower(needle[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
            pos = haystack.find(firstNeedle, pos + 1);
        }
        return false;
    }
    for (size_t i = 0; i <= maxPos; ++i) {
        if (fastTowLower(haystack[i]) == firstNeedle) {
            bool match = true;
            for (size_t j = 1; j < needle.size(); ++j) {
                if (fastTowLower(haystack[i + j]) != fastTowLower(needle[j])) {
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
        if (fastTowLower(a[i]) != fastTowLower(b[i])) return false;
    }
    return true;
}

inline bool startsWithIgnoreCase(std::wstring_view str, std::wstring_view prefix) noexcept {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (fastTowLower(str[i]) != fastTowLower(prefix[i])) return false;
    }
    return true;
}

bool matchTextSegment(std::wstring_view text, std::wstring_view normalizedText,
                      const SearchClause& clause, bool caseSensitive) {
    if (clause.hasWildcard && isPureSuffixWildcardPattern(clause.rawPattern)) {
        if (caseSensitive) {
            const std::wstring_view suffix = std::wstring_view(clause.rawPattern).substr(1);
            return text.size() >= suffix.size() &&
                   text.substr(text.size() - suffix.size()) == suffix;
        }
        const std::wstring_view target = normalizedText.empty() ? text : normalizedText;
        const std::wstring_view suffix = std::wstring_view(clause.pattern).substr(1);
        return target.size() >= suffix.size() &&
               equalsIgnoreCase(target.substr(target.size() - suffix.size()), suffix);
    }
    if (caseSensitive) {
        return clause.hasWildcard
            ? SearchExpression::matchWildcard(clause.rawPattern, text, true)
            : (text.find(clause.rawPattern) != std::wstring_view::npos);
    }
    const std::wstring_view target = normalizedText.empty() ? text : normalizedText;
    if (clause.hasWildcard) {
        return SearchExpression::matchWildcard(clause.pattern, target, false);
    }
    if (clause.isCaselessPattern) {
        return target.find(clause.pattern) != std::wstring_view::npos;
    }
    return containsIgnoreCase(target, clause.pattern);
}

bool matchPathSegment(std::wstring_view fullPath, const SearchClause& clause, bool caseSensitive) {
    if (fullPath.empty()) return false;
    const bool hasForwardSlash = fullPath.find(L'/') != std::wstring_view::npos;
    if (!caseSensitive && !hasForwardSlash) {
        if (clause.hasWildcard) {
            return SearchExpression::matchWildcard(clause.pattern, fullPath, false);
        }
        if (clause.isCaselessPattern) {
            return fullPath.find(clause.pattern) != std::wstring_view::npos;
        }
        return containsIgnoreCase(fullPath, clause.pattern);
    }

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
    if (clause.hasWildcard) {
        return SearchExpression::matchWildcard(clause.pattern, slashNormPath, false);
    }
    if (clause.isCaselessPattern) {
        return slashNormPath.find(clause.pattern) != std::wstring::npos;
    }
    return containsIgnoreCase(slashNormPath, clause.pattern);
}

inline std::wstring_view stripLeafExtension(std::wstring_view s) noexcept {
    if (s.empty()) return s;
    const size_t dotPos = s.rfind(L'.');
    if (dotPos == std::wstring_view::npos || dotPos == 0) return s;
    const size_t slashPos = s.find_last_of(L"\\/");
    if (slashPos != std::wstring_view::npos && dotPos <= slashPos + 1) return s;
    return s.substr(0, dotPos);
}

inline bool startsWithText(std::wstring_view str, std::wstring_view prefix, bool caseSensitive) noexcept {
    if (str.size() < prefix.size()) return false;
    return caseSensitive
        ? (str.substr(0, prefix.size()) == prefix)
        : startsWithIgnoreCase(str, prefix);
}

inline bool endsWithText(std::wstring_view str, std::wstring_view suffix, bool caseSensitive) noexcept {
    if (str.size() < suffix.size()) return false;
    const std::wstring_view tail = str.substr(str.size() - suffix.size());
    return caseSensitive ? (tail == suffix) : equalsIgnoreCase(tail, suffix);
}

inline bool canPossiblyMatchSlashClause(const SearchClause& clause,
                                        std::wstring_view leafName,
                                        uint64_t dirClauseMask,
                                        bool hasDirMask) noexcept {
    if (!hasDirMask || !clause.hasSlashPreFilter) {
        return true;
    }
    if (clause.slashAllowBranchA &&
        (dirClauseMask & clause.slashBranchADirMask) == clause.slashBranchADirMask) {
        return true;
    }
    if (clause.slashAllowBranchB &&
        (dirClauseMask & clause.slashBranchBDirMask) == clause.slashBranchBDirMask) {
        const bool prefixOk = clause.slashLeafPrefix.empty() ||
            startsWithText(leafName, clause.slashLeafPrefix, clause.slashLeafCaseSensitive);
        const bool suffixOk = clause.slashLeafSuffix.empty() ||
            endsWithText(leafName, clause.slashLeafSuffix, clause.slashLeafCaseSensitive);
        if (prefixOk && suffixOk) {
            return true;
        }
    }
    return false;
}

}  // namespace

template <typename PathSupplier, typename AttrSupplier>
static bool matchSingleClauseLazy(const SearchClause& clause, const FileRecord& record,
                                  wchar_t driveLetter, PathSupplier&& getPath,
                                  AttrSupplier&& getAttr, bool requiresFullPath,
                                  bool pinyinEnabled, bool caseSensitive,
                                  bool hasPureExtensionGroup = false,
                                  uint64_t dirClauseMask = 0,
                                  bool hasDirMask = false,
                                  bool allowDirectorySize = false) {
    bool matched = false;
    const bool stripExt = hasPureExtensionGroup && !record.isDirectory &&
                          !clause.hasWildcard && clause.pattern.find(L'.') == std::wstring::npos;
    const std::wstring_view effFileName = stripExt ? stripLeafExtension(record.fileName) : record.fileName;
    const std::wstring_view effNormName = stripExt ? stripLeafExtension(record.normalizedName) : record.normalizedName;
    const std::wstring_view effPyInit   = stripExt ? stripLeafExtension(record.pinyinInitials) : record.pinyinInitials;
    const std::wstring_view effPyFull   = stripExt ? stripLeafExtension(record.pinyinFull) : record.pinyinFull;

    switch (clause.filterType) {
        case SearchFilterType::FileOnly:
            if (record.isDirectory) matched = false;
            else if (clause.pattern.empty()) matched = true;
            else {
                matched = matchTextSegment(effFileName, effNormName, clause, caseSensitive);
                if (!matched && requiresFullPath &&
                    canPossiblyMatchSlashClause(clause, effFileName, dirClauseMask, hasDirMask)) {
                    const std::wstring& fullP = getPath();
                    matched = matchPathSegment(stripExt ? stripLeafExtension(fullP) : std::wstring_view(fullP), clause, caseSensitive);
                }
            }
            break;

        case SearchFilterType::FolderOnly:
            if (!record.isDirectory) matched = false;
            else if (clause.pattern.empty()) matched = true;
            else {
                matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
                if (!matched && requiresFullPath &&
                    canPossiblyMatchSlashClause(clause, record.fileName, dirClauseMask, hasDirMask)) {
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
            if (!requiresFullPath) {
                matched = matchTextSegment(record.fileName, record.normalizedName, clause, caseSensitive);
            } else if (canPossiblyMatchSlashClause(clause, record.fileName, dirClauseMask, hasDirMask)) {
                matched = matchPathSegment(getPath(), clause, caseSensitive);
            }
            break;

        case SearchFilterType::Parent: {
            if (!canPossiblyMatchSlashClause(clause, {}, dirClauseMask, hasDirMask)) {
                matched = false;
                break;
            }
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
            if (clause.linearRegex) {
                matched = t3k_rs_regex_is_match(
                    clause.linearRegex.get(),
                    reinterpret_cast<const uint16_t*>(record.fileName.data()),
                    record.fileName.size());
                if (!matched && requiresFullPath) {
                    const std::wstring slashPath = normalizePathSeparators(getPath());
                    matched = t3k_rs_regex_is_match(
                        clause.linearRegex.get(),
                        reinterpret_cast<const uint16_t*>(slashPath.data()),
                        slashPath.size());
                }
            } else if (!clause.regexInvalid && clause.regexObj.has_value()) {
                try {
                    matched = std::regex_search(record.fileName.begin(), record.fileName.end(),
                                                clause.regexObj.value());
                    if (!matched && requiresFullPath) {
                        const std::wstring slashPath = normalizePathSeparators(getPath());
                        matched = std::regex_search(slashPath, clause.regexObj.value());
                    }
                } catch (const std::regex_error&) {
                    matched = false;
                } catch (...) {
                    matched = false;
                }
            }
            break;

        case SearchFilterType::CaseSensitive:
            matched = matchTextSegment(effFileName, effNormName, clause, true);
            if (!matched && requiresFullPath &&
                canPossiblyMatchSlashClause(clause, effFileName, dirClauseMask, hasDirMask)) {
                const std::wstring& fullP = getPath();
                matched = matchPathSegment(stripExt ? stripLeafExtension(fullP) : std::wstring_view(fullP), clause, true);
            }
            break;

        case SearchFilterType::PinyinOnly:
            if (clause.hasWildcard) {
                const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                matched = SearchExpression::matchWildcard(pat, effPyFull) ||
                          SearchExpression::matchWildcard(pat, effPyInit);
            } else {
                const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                matched = (containsIgnoreCase(effPyInit, pat) ||
                           containsIgnoreCase(effPyFull, pat));
            }
            break;

        case SearchFilterType::NoPinyin:
            matched = matchTextSegment(effFileName, effNormName, clause, caseSensitive);
            if (!matched && requiresFullPath &&
                canPossiblyMatchSlashClause(clause, effFileName, dirClauseMask, hasDirMask)) {
                const std::wstring& fullP = getPath();
                matched = matchPathSegment(stripExt ? stripLeafExtension(fullP) : std::wstring_view(fullP), clause, caseSensitive);
            }
            break;

        case SearchFilterType::Size:
            if (record.isDirectory && !allowDirectorySize) {
                matched = false;
            } else {
                uint64_t effectiveSize = record.fileSize;
                if (effectiveSize == 0 && !record.isDirectory) {
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
                    case SearchClause::SizeOp::Range:
                        matched = (effectiveSize >= clause.sizeBytes && effectiveSize <= clause.sizeMaxBytes);
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
            matched = matchTextSegment(effFileName, effNormName, clause, caseSensitive);
            if (!matched && pinyinEnabled && clause.isAsciiOnly) {
                const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                const bool isRedundantNonCjkPinyin =
                    !caseSensitive &&
                    record.pinyinInitials.data() == record.fileName.data() &&
                    record.pinyinFull.data() == record.fileName.data() &&
                    pat.size() == clause.pattern.size();
                if (!isRedundantNonCjkPinyin) {
                    const bool allowPinyinFallback = !caseSensitive ||
                        (clause.rawPattern == clause.pattern &&
                         !(clause.hasWildcard
                           ? SearchExpression::matchWildcard(clause.pattern, effFileName, false)
                           : containsIgnoreCase(effFileName, clause.pattern)));
                    if (allowPinyinFallback) {
                        if (clause.hasWildcard) {
                            matched = SearchExpression::matchWildcard(pat, effPyFull) ||
                                      SearchExpression::matchWildcard(pat, effPyInit);
                        } else {
                            matched = containsIgnoreCase(effPyInit, pat) ||
                                      containsIgnoreCase(effPyFull, pat);
                        }
                    }
                }
            }

            // 2. 仅当检索表达式需要全路径 (如开启 matchPath 偏好、包含多词组、斜杠或 path: 前缀) 且文件名未命中时，才兜底在路径中匹配
            if (!matched && requiresFullPath && !isPureSuffixWildcardPattern(clause.rawPattern) &&
                canPossiblyMatchSlashClause(clause, effFileName, dirClauseMask, hasDirMask)) {
                const std::wstring& fullP = getPath();
                matched = matchPathSegment(stripExt ? stripLeafExtension(fullP) : std::wstring_view(fullP), clause, caseSensitive);
            }
            break;
        }
    }

    return clause.isNegated ? !matched : matched;
}

uint64_t SearchExpression::computeDirectorySegmentMask(std::wstring_view dirSegmentName,
                                                       std::wstring_view pinyinInitials,
                                                       std::wstring_view pinyinFull) const noexcept {
    if (((m_separableGroupMask | m_slashPredicateMask) == 0) || dirSegmentName.empty()) {
        return 0;
    }
    uint64_t mask = 0;
    if (m_separableGroupMask != 0) {
        const size_t groupCount = std::min<size_t>(m_orGroups.size(), 64);
        for (size_t g = 0; g < groupCount; ++g) {
            const uint64_t bit = (1ULL << g);
            if ((m_separableGroupMask & bit) == 0) {
                continue;
            }
            const auto& group = m_orGroups[g];
            for (const auto& clause : group.clauses) {
                if (!isClauseSeparableForDir(clause, m_requiresFullPath)) {
                    continue;
                }
                const bool effCaseSensitive =
                    m_caseSensitive || (clause.filterType == SearchFilterType::CaseSensitive);
                bool segMatched = matchTextSegment(dirSegmentName, {}, clause, effCaseSensitive);
                if (!segMatched && m_pinyinEnabled && clause.isAsciiOnly &&
                    clause.filterType == SearchFilterType::None && !pinyinInitials.empty()) {
                    const auto& pat = clause.pinyinPattern.empty() ? clause.pattern : clause.pinyinPattern;
                    const bool isRedundantNonCjk =
                        !effCaseSensitive &&
                        pinyinInitials.data() == dirSegmentName.data() &&
                        pinyinFull.data() == dirSegmentName.data() &&
                        pat.size() == clause.pattern.size();
                    if (!isRedundantNonCjk) {
                        segMatched = containsIgnoreCase(pinyinInitials, pat) ||
                                     containsIgnoreCase(pinyinFull, pat);
                    }
                }
                if (segMatched) {
                    mask |= bit;
                    break;
                }
            }
        }
    }

    if (!m_slashDirPredicates.empty()) {
        wchar_t rootDriveBuf[2] = {L'\0', L':'};
        std::wstring_view slashSeg = dirSegmentName;
        if (dirSegmentName.size() == 1 && std::iswalpha(dirSegmentName[0]) && pinyinInitials.empty()) {
            rootDriveBuf[0] = dirSegmentName[0];
            slashSeg = std::wstring_view(rootDriveBuf, 2);
        }
        for (const auto& pred : m_slashDirPredicates) {
            bool hit = false;
            switch (pred.op) {
                case DirSegMatchOp::Contains:
                    hit = pred.caseSensitive
                        ? (slashSeg.find(pred.needle) != std::wstring_view::npos)
                        : containsIgnoreCase(slashSeg, pred.needle);
                    break;
                case DirSegMatchOp::Prefix:
                    hit = startsWithText(slashSeg, pred.needle, pred.caseSensitive);
                    break;
                case DirSegMatchOp::Suffix:
                    hit = endsWithText(slashSeg, pred.needle, pred.caseSensitive);
                    break;
                case DirSegMatchOp::Exact:
                    hit = pred.caseSensitive
                        ? (slashSeg == pred.needle)
                        : equalsIgnoreCase(slashSeg, pred.needle);
                    break;
            }
            if (hit) {
                mask |= (1ULL << pred.bitIndex);
            }
        }
    }
    return mask;
}

bool SearchExpression::matchesSeparable(const FileRecord& record, wchar_t driveLetter,
                                        uint64_t dirClauseMask) const noexcept {
    if (m_orGroups.empty()) return true;
    if ((dirClauseMask & m_negatedSeparableGroupMask) != 0) {
        return false;
    }

    const uint64_t dirSatisfiedMask =
        dirClauseMask & (record.isDirectory ? m_dirFolderSatisfiedMask : m_dirFileSatisfiedMask);

    static const std::wstring kEmptyPath;
    auto getNoPath = []() noexcept -> const std::wstring& { return kEmptyPath; };
    auto getNoAttr = []() noexcept -> const WIN32_FILE_ATTRIBUTE_DATA* { return nullptr; };

    for (size_t g = m_orGroups.size(); g-- > 0;) {
        if (g < 64 && ((dirSatisfiedMask >> g) & 1ULL) != 0) {
            continue;
        }
        const auto& group = m_orGroups[g];
        bool groupPassed = false;
        for (const auto& clause : group.clauses) {
            if (matchSingleClauseLazy(clause, record, driveLetter, getNoPath, getNoAttr,
                                      /*requiresFullPath=*/false, m_pinyinEnabled, m_caseSensitive,
                                      m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                      m_hasFolderOnlyFilter)) {
                groupPassed = true;
                break;
            }
        }
        if (!groupPassed) return false;
    }
    return true;
}

SearchExpression::InMemoryMatchStatus SearchExpression::evaluateInMemoryWithDirMask(
    const FileRecord& record,
    wchar_t driveLetter,
    uint64_t dirClauseMask,
    bool isMetadataHydrated,
    const PathGetter& getFullPath) const {
    if (m_orGroups.empty()) {
        return InMemoryMatchStatus::Matched;
    }
    if ((dirClauseMask & m_negatedSeparableGroupMask) != 0) {
        return InMemoryMatchStatus::Rejected;
    }

    const uint64_t dirSatisfiedMask =
        dirClauseMask & (record.isDirectory ? m_dirFolderSatisfiedMask : m_dirFileSatisfiedMask);

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
    auto getNoAttr = []() noexcept -> const WIN32_FILE_ATTRIBUTE_DATA* { return nullptr; };

    bool hasDiskStatGroups = false;
    const size_t groupCount = m_orGroups.size();
    for (size_t g = 0; g < groupCount; ++g) {
        const auto& group = m_orGroups[g];
        if (groupRequiresDiskStat(group)) {
            hasDiskStatGroups = true;
            continue;
        }

        if (g < 64) {
            const uint64_t bit = (1ULL << g);
            if ((dirSatisfiedMask & bit) != 0) {
                continue;
            }
            if ((m_separableGroupMask & bit) != 0) {
                bool groupPassed = false;
                for (const auto& clause : group.clauses) {
                    if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getNoAttr,
                                              /*requiresFullPath=*/false, m_pinyinEnabled, m_caseSensitive,
                                              m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                              m_hasFolderOnlyFilter)) {
                        groupPassed = true;
                        break;
                    }
                }
                if (!groupPassed) {
                    return InMemoryMatchStatus::Rejected;
                }
                continue;
            }
        }

        bool groupPassed = false;
        for (const auto& clause : group.clauses) {
            if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getNoAttr,
                                      m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                      m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                      m_hasFolderOnlyFilter)) {
                groupPassed = true;
                break;
            }
        }
        if (!groupPassed) {
            return InMemoryMatchStatus::Rejected;
        }
    }

    if (!hasDiskStatGroups) {
        return InMemoryMatchStatus::Matched;
    }

    bool anyGroupNeedsStat = false;
    for (const auto& group : m_orGroups) {
        if (!groupRequiresDiskStat(group)) {
            continue;
        }
        bool groupPassed = false;
        bool hasIndeterminateClause = false;
        for (const auto& clause : group.clauses) {
            if (clause.filterType == SearchFilterType::Size &&
                !record.isDirectory && !isMetadataHydrated && record.fileSize == 0) {
                hasIndeterminateClause = true;
                continue;
            }
            if (clause.filterType == SearchFilterType::DateModified &&
                clause.hasValidDateRange && !isMetadataHydrated && record.lastWriteTime == 0) {
                hasIndeterminateClause = true;
                continue;
            }
            if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getNoAttr,
                                      m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                      m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                      m_hasFolderOnlyFilter)) {
                groupPassed = true;
                break;
            }
        }
        if (!groupPassed) {
            if (!hasIndeterminateClause) {
                return InMemoryMatchStatus::Rejected;
            }
            anyGroupNeedsStat = true;
        }
    }

    return anyGroupNeedsStat
        ? InMemoryMatchStatus::NeedsDiskStat
        : InMemoryMatchStatus::Matched;
}

bool SearchExpression::matchesWithDirMask(const FileRecord& record, wchar_t driveLetter,
                                          uint64_t dirClauseMask,
                                          const PathGetter& getFullPath,
                                          const StatHydrateCallback& onStatHydrated) const {
    if (m_orGroups.empty()) return true;

    // 快速短路：若任何单子句取反可分离组已在祖先目录命中（如 !.git / !node_modules），单条位指令直接淘汰
    if ((dirClauseMask & m_negatedSeparableGroupMask) != 0) {
        return false;
    }

    const uint64_t dirSatisfiedMask =
        dirClauseMask & (record.isDirectory ? m_dirFolderSatisfiedMask : m_dirFileSatisfiedMask);

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

    bool hasDiskStatGroups = false;
    const size_t groupCount = m_orGroups.size();
    for (size_t g = 0; g < groupCount; ++g) {
        const auto& group = m_orGroups[g];
        if (groupRequiresDiskStat(group)) {
            hasDiskStatGroups = true;
            continue;
        }

        if (g < 64) {
            const uint64_t bit = (1ULL << g);
            if ((dirSatisfiedMask & bit) != 0) {
                continue;
            }
            if ((m_separableGroupMask & bit) != 0) {
                bool groupPassed = false;
                for (const auto& clause : group.clauses) {
                    if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getOrFetchAttr,
                                              /*requiresFullPath=*/false, m_pinyinEnabled, m_caseSensitive,
                                              m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                              m_hasFolderOnlyFilter)) {
                        groupPassed = true;
                        break;
                    }
                }
                if (!groupPassed) return false;
                continue;
            }
        }

        bool groupPassed = false;
        for (const auto& clause : group.clauses) {
            if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getOrFetchAttr,
                                      m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                      m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                      m_hasFolderOnlyFilter)) {
                groupPassed = true;
                break;
            }
        }
        if (!groupPassed) return false;
    }

    if (hasDiskStatGroups) {
        for (const auto& group : m_orGroups) {
            if (!groupRequiresDiskStat(group)) continue;
            bool groupPassed = false;
            for (const auto& clause : group.clauses) {
                if (matchSingleClauseLazy(clause, record, driveLetter, getOrFetchPath, getOrFetchAttr,
                                          m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                          m_hasPureExtensionGroup, dirClauseMask, /*hasDirMask=*/true,
                                          m_hasFolderOnlyFilter)) {
                    groupPassed = true;
                    break;
                }
            }
            if (!groupPassed) return false;
        }
    }
    return true;
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
                                      m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                      m_hasPureExtensionGroup, 0, false, m_hasFolderOnlyFilter)) {
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
                                          m_requiresFullPath, m_pinyinEnabled, m_caseSensitive,
                                          m_hasPureExtensionGroup, 0, false, m_hasFolderOnlyFilter)) {
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

    const std::wstring_view stem = record.isDirectory
        ? record.fileName
        : stripLeafExtension(record.fileName);
    const std::wstring_view pyFullStem = record.isDirectory
        ? record.pinyinFull
        : stripLeafExtension(record.pinyinFull);
    const std::wstring_view pyInitStem = record.isDirectory
        ? record.pinyinInitials
        : stripLeafExtension(record.pinyinInitials);

    if (equalsIgnoreCase(record.fileName, targetQuery)) return 0;

    if (m_rankingTerms.size() >= 2) {
        if (equalsIgnoreCase(stem, targetQuery)) return 0;
        if (startsWithIgnoreCase(stem, targetQuery)) return 1;

        size_t stemMatchedCount = 0;
        bool firstTermPrefix = false;
        for (size_t i = 0; i < m_rankingTerms.size(); ++i) {
            const auto& term = m_rankingTerms[i];
            bool hit = containsIgnoreCase(stem, term);
            if (!hit && m_pinyinEnabled) {
                hit = containsIgnoreCase(pyFullStem, term) || containsIgnoreCase(pyInitStem, term);
            }
            if (hit) {
                ++stemMatchedCount;
                if (i == 0 && (startsWithIgnoreCase(stem, term) ||
                               (m_pinyinEnabled && (startsWithIgnoreCase(pyFullStem, term) ||
                                                    startsWithIgnoreCase(pyInitStem, term))))) {
                    firstTermPrefix = true;
                }
            }
        }
        if (stemMatchedCount == m_rankingTerms.size()) {
            return firstTermPrefix ? 1 : 2;
        }
        if (stemMatchedCount > 0) {
            const size_t missing = m_rankingTerms.size() - stemMatchedCount;
            return 3 + static_cast<int>(std::min<size_t>(missing, 3));
        }
        return 7;
    }

    if (startsWithIgnoreCase(stem, targetQuery)) return 1;
    if (m_pinyinEnabled && m_isAsciiQuery) {
        const auto& q = m_cleanAsciiQuery.empty() ? targetQuery : m_cleanAsciiQuery;
        if (startsWithIgnoreCase(pyFullStem, q)) return 2;
        if (startsWithIgnoreCase(pyInitStem, q)) return 3;
        if (containsIgnoreCase(stem, targetQuery)) return 4;
        if (containsIgnoreCase(pyFullStem, q)) return 5;
        if (containsIgnoreCase(pyInitStem, q)) return 6;
    } else {
        if (containsIgnoreCase(stem, targetQuery)) return 4;
    }
    return 7;
}
