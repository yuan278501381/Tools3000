#pragma once
#include <string>
#include <string_view>

class PinyinEngine {
public:
    // Looks up the compact lowercase ASCII pinyin syllable from the static table (returns nullptr if not CJK).
    static const char* LookupSyllableAscii(wchar_t ch) noexcept;

    // Returns the pinyin initials for a given string (e.g. "微信" -> "wx").
    // Non-Chinese ASCII characters are folded to lowercase; zero per-character heap allocations.
    static std::wstring GetInitials(std::wstring_view text);

    // Returns the full continuous pinyin for a given string (e.g. "微信" -> "weixin").
    // Non-Chinese ASCII characters are folded to lowercase; zero per-character heap allocations.
    static std::wstring GetFullPinyin(std::wstring_view text);

    // Single-pass zero-heap-allocation builder via compact single-byte ASCII buffer + reusable wide outputs.
    // Performs at most ONE binary search per CJK code unit for both initials and full pinyin simultaneously.
    static void BuildNormalizedPinyin(
        std::wstring_view text,
        std::string& scratchAsciiInitials,
        std::string& scratchAsciiFull,
        std::wstring& outInitials,
        std::wstring& outFull
    );

    static wchar_t GetFirstLetter(wchar_t ch) noexcept;
    static std::wstring GetCharPinyin(wchar_t ch);
};
