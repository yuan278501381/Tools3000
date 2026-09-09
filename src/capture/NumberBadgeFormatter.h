#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// NumberBadgeFormatter.h — 序号标注多制式格式化单一事实源 (SSOT)
//
// 支持制式:
//   1. Arabic     : 1, 2, 3 ... (默认)
//   2. UpperAlpha : A, B, C ... Z, AA, AB ...
//   3. LowerAlpha : a, b, c ... z, aa, ab ...
//   4. Roman      : I, II, III, IV, V ... XI, XII ...
//   5. Chinese    : 一, 二, 三 ... 十, 十一, 十二 ... 二十, 二十一 ...
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_NUMBER_BADGE_FORMATTER_H
#define TOOLS3000_CAPTURE_NUMBER_BADGE_FORMATTER_H

#include <algorithm>
#include <string>
#include <utility>

namespace tools3000::capture {

/// 序号文本制式枚举
enum class NumberFormat : int {
    Arabic = 0,      // 1, 2, 3 ... (默认)
    UpperAlpha = 1,  // A, B, C ...
    LowerAlpha = 2,  // a, b, c ...
    Roman = 3,       // I, II, III ...
    Chinese = 4,     // 一, 二, 三 ... 十, 十一 ...
    COUNT = 5
};

namespace detail {

inline std::wstring formatBijectiveAlpha(int value, wchar_t baseChar) {
    int n = (std::max)(1, value);
    std::wstring rev;
    while (n > 0) {
        int rem = (n - 1) % 26;
        rev.push_back(static_cast<wchar_t>(baseChar + rem));
        n = (n - 1) / 26;
    }
    std::reverse(rev.begin(), rev.end());
    return rev;
}

inline std::wstring formatRomanNumeral(int value) {
    int n = std::clamp(value, 1, 3999);
    static constexpr std::pair<int, const wchar_t*> kRomanTable[] = {
        {1000, L"M"}, {900, L"CM"}, {500, L"D"}, {400, L"CD"},
        {100,  L"C"}, {90,  L"XC"}, {50,  L"L"}, {40,  L"XL"},
        {10,   L"X"}, {9,   L"IX"}, {5,   L"V"}, {4,   L"IV"},
        {1,    L"I"},
    };
    std::wstring out;
    for (const auto& [unit, sym] : kRomanTable) {
        while (n >= unit) {
            out += sym;
            n -= unit;
        }
    }
    return out;
}

inline std::wstring formatChineseOrdinal(int value) {
    int n = (std::max)(1, value);
    if (n >= 1000) {
        return std::to_wstring(n);
    }
    static constexpr const wchar_t* kDigits[] = {
        L"零", L"一", L"二", L"三", L"四", L"五", L"六", L"七", L"八", L"九"
    };
    if (n <= 9) {
        return kDigits[n];
    }
    if (n == 10) {
        return L"十";
    }
    if (n < 20) {
        // 11 ~ 19 -> 十一 ~ 十九（地道现代中文习惯，而非“一十一”）
        return std::wstring(L"十") + kDigits[n % 10];
    }
    if (n < 100) {
        int tens = n / 10;
        int ones = n % 10;
        std::wstring out = std::wstring(kDigits[tens]) + L"十";
        if (ones > 0) {
            out += kDigits[ones];
        }
        return out;
    }
    // 100 ~ 999
    int hundreds = n / 100;
    int rem = n % 100;
    std::wstring out = std::wstring(kDigits[hundreds]) + L"百";
    if (rem == 0) {
        return out;
    }
    if (rem < 10) {
        out += L"零";
        out += kDigits[rem];
        return out;
    }
    int tens = rem / 10;
    int ones = rem % 10;
    out += kDigits[tens];
    out += L"十";
    if (ones > 0) {
        out += kDigits[ones];
    }
    return out;
}

} // namespace detail

/// 将正整数序号按指定制式格式化为展示宽字符串
inline std::wstring formatNumberBadgeLabel(int value, NumberFormat format) {
    const int safeVal = (std::max)(1, value);
    switch (format) {
        case NumberFormat::UpperAlpha:
            return detail::formatBijectiveAlpha(safeVal, L'A');
        case NumberFormat::LowerAlpha:
            return detail::formatBijectiveAlpha(safeVal, L'a');
        case NumberFormat::Roman:
            return detail::formatRomanNumeral(safeVal);
        case NumberFormat::Chinese:
            return detail::formatChineseOrdinal(safeVal);
        case NumberFormat::Arabic:
        default:
            return std::to_wstring(safeVal);
    }
}

/// 二级工具栏紧凑指示文本
inline std::wstring numberFormatToolbarTag(NumberFormat format) {
    switch (format) {
        case NumberFormat::UpperAlpha: return L"ABC";
        case NumberFormat::LowerAlpha: return L"abc";
        case NumberFormat::Roman:      return L"I,II";
        case NumberFormat::Chinese:    return L"一二";
        case NumberFormat::Arabic:
        default:                       return L"123";
    }
}

/// 下拉菜单完整选项说明
inline std::wstring numberFormatMenuLabel(NumberFormat format, bool chineseUi) {
    switch (format) {
        case NumberFormat::UpperAlpha:
            return chineseUi ? L"大写字母  (A, B, C)" : L"Uppercase  (A, B, C)";
        case NumberFormat::LowerAlpha:
            return chineseUi ? L"小写字母  (a, b, c)" : L"Lowercase  (a, b, c)";
        case NumberFormat::Roman:
            return chineseUi ? L"罗马数字  (I, II, III)" : L"Roman  (I, II, III)";
        case NumberFormat::Chinese:
            return chineseUi ? L"中文数字  (一, 二, 三...十一)" : L"Chinese  (一, 二, 三)";
        case NumberFormat::Arabic:
        default:
            return chineseUi ? L"阿拉伯数字  (1, 2, 3)" : L"Numbers  (1, 2, 3)";
    }
}

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_NUMBER_BADGE_FORMATTER_H
