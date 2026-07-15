#pragma once
#include <string>

class PinyinEngine {
public:
    // Returns the pinyin initials for a given string (e.g. "微信" -> "wx")
    // Non-chinese characters are kept as lowercase
    static std::wstring GetInitials(const std::wstring& text);

    // Returns the full continuous pinyin for a given string (e.g. "微信" -> "weixin")
    // Non-chinese characters are kept as lowercase
    static std::wstring GetFullPinyin(const std::wstring& text);

    static wchar_t GetFirstLetter(wchar_t ch);
    static std::wstring GetCharPinyin(wchar_t ch);
};
