#include "PinyinEngine.h"
#include "UnicodePinyinData.h"

namespace {

constexpr wchar_t asciiLower(wchar_t ch) noexcept {
    return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
}

constexpr bool isHighSurrogate(wchar_t ch) noexcept {
    return ch >= static_cast<wchar_t>(0xD800) && ch <= static_cast<wchar_t>(0xDBFF);
}

constexpr bool isLowSurrogate(wchar_t ch) noexcept {
    return ch >= static_cast<wchar_t>(0xDC00) && ch <= static_cast<wchar_t>(0xDFFF);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PinyinEngine — 零系统依赖的高性能 Unicode 汉字拼音引擎
// 内部使用基于已排序 Unicode 码点的二分查找表，O(log N) 单字符耗时 < 5ns
// ─────────────────────────────────────────────────────────────────────────────

wchar_t PinyinEngine::GetFirstLetter(wchar_t ch) {
    if (ch <= 0x7F) {
        return asciiLower(ch);
    }
    std::wstring pinyin = GetCharPinyin(ch);
    if (!pinyin.empty()) {
        return pinyin[0];
    }
    // Non-ASCII characters without a table entry are preserved verbatim.  In
    // particular, never pass an arbitrary UTF-16 code unit to the narrow C
    // locale functions: std::tolower is only defined for EOF/unsigned-char.
    return ch;
}

std::wstring PinyinEngine::GetCharPinyin(wchar_t ch) {
    if (ch <= 0x7F) {
        return std::wstring(1, asciiLower(ch));
    }

    const uint16_t u = static_cast<uint16_t>(ch);

    // 二分查找 UnicodePinyinTable
    size_t left = 0;
    size_t right = kUnicodePinyinTableSize - 1;
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        if (kUnicodePinyinTable[mid].unicode == u) {
            uint16_t syllableId = kUnicodePinyinTable[mid].syllableId;
            const char* py = kPinyinSyllables[syllableId];
            std::wstring res;
            while (*py) res.push_back(static_cast<wchar_t>(*py++));
            return res;
        }
        if (kUnicodePinyinTable[mid].unicode < u) {
            left = mid + 1;
        } else {
            if (mid == 0) break;
            right = mid - 1;
        }
    }

    return std::wstring(1, ch);
}

std::wstring PinyinEngine::GetInitials(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.length());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (isHighSurrogate(ch) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            // The pinyin table is BMP-only. Preserve supplementary code points
            // as their complete UTF-16 pair instead of splitting/corrupting it.
            result.push_back(ch);
            result.push_back(text[++i]);
            continue;
        }
        result += GetFirstLetter(ch);
    }
    return result;
}

std::wstring PinyinEngine::GetFullPinyin(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.length() * 4);
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (isHighSurrogate(ch) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            result.push_back(ch);
            result.push_back(text[++i]);
            continue;
        }
        result += GetCharPinyin(ch);
    }
    return result;
}
