#pragma once
// -----------------------------------------------------------------------------
// TextExpanderMatcher.h -- 纯标准 C++20 滑动窗口触发词匹配与动态宏展开引擎
//
// 设计原则:
//   1. 零 Win32 API 依赖，100% 跨平台可单测（支持 x64 / ARM64）。
//   2. 最长触发词优先匹配（Longest Match First），支持 Immediate 与 Delimiter 双模式。
//   3. 面向 8 国语言生态原生支持全角/半角智能等价（含中日韩同键位标点映射）、
//      拉丁扩展/西里尔字母大小写折叠与多语种词首边界防误触，以及边界键保留策略。
//   4. 单遍扫描 14 类动态宏与相对日期偏移（{{date+Nd}} / {{date-Nd}}），
//      彻底免疫剪贴板内容递归宏展开漏洞，精准计算 UTF-16 退格与 {{cursor}} 左移步数。
// -----------------------------------------------------------------------------

#ifndef TOOLS3000_TEXT_EXPANDER_MATCHER_H
#define TOOLS3000_TEXT_EXPANDER_MATCHER_H

#include "text_expander/TextExpanderTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tools3000::text_expander {

/// 平台无关本地日期时间快照（便于单元测试注入确定性时间）
struct LocalDateTime {
    int year = 2026;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int weekday = -1;       // 0=周日 .. 6=周六；若 < 0 则按公历年月日自动精确推导
    int64_t unixSeconds = 0; // 若 == 0 则按年月日时分秒自动推导 UTC 等效秒数
};

/// 丰富的动态宏展开上下文（支持 8 语种星期、系统用户名、UUID 生成器注入）
struct TemplateExpansionContext {
    LocalDateTime now{};
    std::wstring_view clipboardText{};
    std::wstring_view userName{};
    std::string_view localeCode{"en"};
    std::function<std::wstring()> uuidGenerator{};
    size_t maxClipboardChars = 4096;
};

/// 模板动态展开结果
struct ExpandedOutput {
    std::wstring text;
    size_t moveLeftSteps = 0;
};

/// 规则触发命中结果
struct MatchResult {
    TextExpanderRule rule;
    size_t eraseCount = 0;          // 需发送的 VK_BACK 次数（Immediate=trigger长度; Delimiter=trigger长度+1）
    wchar_t delimiterChar = L'\0';  // Delimiter 模式下触发的边界字符
    bool triggeredByDelimiter = false;
};

namespace detail {

inline bool isHighSurrogate(wchar_t ch) noexcept {
    const uint16_t u = static_cast<uint16_t>(ch);
    return u >= 0xD800u && u <= 0xDBFFu;
}

inline bool isLowSurrogate(wchar_t ch) noexcept {
    const uint16_t u = static_cast<uint16_t>(ch);
    return u >= 0xDC00u && u <= 0xDFFFu;
}

/// 全角字符与中日韩输入法同键位标点 -> 标准半角 ASCII 等价归一化
inline wchar_t foldHalfwidthWide(wchar_t ch) noexcept {
    const uint16_t u = static_cast<uint16_t>(ch);
    // 全角 ASCII 符号、数字、大小写字母区 (U+FF01 '！' .. U+FF5E '～')
    if (u >= 0xFF01u && u <= 0xFF5Eu) {
        return static_cast<wchar_t>(u - 0xFEE0u);
    }
    // 全角空格 (U+3000)
    if (u == 0x3000u) {
        return L' ';
    }
    // 中日韩输入法中文标点模式常见非 FFxx 同键位映射
    switch (u) {
        case 0x3001u: return L'\\'; // 、 -> \ (或顿号)
        case 0x3002u: return L'.';  // 。 -> .
        case 0x300Au: return L'<';  // 《 -> <
        case 0x300Bu: return L'>';  // 》 -> >
        case 0x3010u: return L'[';  // 【 -> [
        case 0x3011u: return L']';  // 】 -> ]
        case 0x300Cu:               // 「
        case 0x300Du:               // 」
        case 0x201Cu:               // “
        case 0x201Du: return L'"';  // ” -> "
        case 0x2018u:               // ‘
        case 0x2019u: return L'\''; // ’ -> '
        case 0xFFE5u: return L'$';  // ￥ -> $
        case 0x2014u: return L'-';  // — -> -
        case 0x2026u: return L'.';  // … -> .
        case 0x00B7u: return L'`';  // · -> `
        default: return ch;
    }
}

/// 覆盖 8 国语言（拉丁、西欧重音拉丁补充、俄语西里尔字母）的无区域依赖小写折叠
inline wchar_t foldCaseUnicodeWide(wchar_t ch) noexcept {
    const uint16_t u = static_cast<uint16_t>(ch);
    if (u >= L'A' && u <= L'Z') {
        return static_cast<wchar_t>(u - L'A' + L'a');
    }
    // 拉丁补充大写重音字母 (À..Ö, Ø..Þ — 覆盖 es / pt-BR / fr / de)
    if ((u >= 0x00C0u && u <= 0x00D6u) || (u >= 0x00D8u && u <= 0x00DEu)) {
        return static_cast<wchar_t>(u + 0x0020u);
    }
    // 俄语西里尔大写字母 (А..Я -> а..я, Ё -> ё)
    if (u >= 0x0410u && u <= 0x042Fu) {
        return static_cast<wchar_t>(u + 0x0020u);
    }
    if (u == 0x0401u) {
        return static_cast<wchar_t>(0x0451u);
    }
    return ch;
}

inline wchar_t toLowerAsciiWide(wchar_t ch) noexcept {
    return foldCaseUnicodeWide(ch);
}

inline wchar_t normalizeRuleChar(wchar_t ch, bool caseSensitive, bool fullwidthSensitive) noexcept {
    if (!fullwidthSensitive) {
        ch = foldHalfwidthWide(ch);
    }
    if (!caseSensitive) {
        ch = foldCaseUnicodeWide(ch);
    }
    return ch;
}

inline std::wstring normalizeRuleString(
    std::wstring_view input,
    bool caseSensitive,
    bool fullwidthSensitive) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t ch : input) {
        out.push_back(normalizeRuleChar(ch, caseSensitive, fullwidthSensitive));
    }
    return out;
}

inline std::wstring toLowerWide(std::wstring_view input) {
    return normalizeRuleString(input, /*caseSensitive=*/false, /*fullwidthSensitive=*/true);
}

/// 多语种单词字符判定（覆盖英/西/葡拉丁字母数字、俄语西里尔字母及韩文音节，用于 Delimiter 词首边界保护）
inline bool isWordCharWide(wchar_t ch) noexcept {
    const wchar_t hw = foldHalfwidthWide(ch);
    const uint16_t u = static_cast<uint16_t>(hw);
    if ((u >= L'0' && u <= L'9') ||
        (u >= L'A' && u <= L'Z') ||
        (u >= L'a' && u <= L'z') ||
        u == L'_') {
        return true;
    }
    // Латинский拡張 / 西欧葡语重音字母
    if ((u >= 0x00C0u && u <= 0x024Fu && u != 0x00D7u && u != 0x00F7u)) {
        return true;
    }
    // 西里尔字母 (俄语)
    if (u >= 0x0400u && u <= 0x04FFu) {
        return true;
    }
    // 韩文谚文音节与字母 (ko) — 韩语按空格分词，需词边界防误触
    if ((u >= 0xAC00u && u <= 0xD7A3u) || (u >= 0x3131u && u <= 0x318Eu)) {
        return true;
    }
    return false;
}

inline bool isDelimiterWide(wchar_t ch) noexcept {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000;
}

inline wchar_t canonicalDelimiterChar(wchar_t ch) noexcept {
    if (ch == L'\r') return L'\n';
    return ch;
}

/// 纯标准 RFC 3629 UTF-8 -> UTF-16
inline std::wstring utf8ToUtf16(std::string_view utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    size_t i = 0;
    const size_t n = utf8.size();
    while (i < n) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0xFFFDu;
        if (b0 < 0x80u) {
            cp = b0;
            ++i;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < n) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                const uint32_t cand = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
                if (cand >= 0x80u) cp = cand;
                i += 2;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < n) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                const uint32_t cand =
                    ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
                if (cand >= 0x800u && !(cand >= 0xD800u && cand <= 0xDFFFu)) {
                    cp = cand;
                }
                i += 3;
            } else {
                ++i;
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < n) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
            const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                const uint32_t cand =
                    ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                    ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
                if (cand >= 0x10000u && cand <= 0x10FFFFu) {
                    cp = cand;
                }
                i += 4;
            } else {
                ++i;
            }
        } else {
            ++i;
        }

        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            const uint32_t v = cp - 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
        }
    }
    return out;
}

/// 纯标准 RFC 3629 UTF-16 -> UTF-8
inline std::string utf16ToUtf8(std::wstring_view utf16) {
    std::string out;
    out.reserve(utf16.size());
    size_t i = 0;
    const size_t n = utf16.size();
    while (i < n) {
        uint32_t cp = static_cast<uint16_t>(utf16[i]);
        if (isHighSurrogate(utf16[i]) && i + 1 < n && isLowSurrogate(utf16[i + 1])) {
            const uint32_t hi = static_cast<uint16_t>(utf16[i]) - 0xD800u;
            const uint32_t lo = static_cast<uint16_t>(utf16[i + 1]) - 0xDC00u;
            cp = 0x10000u + ((hi << 10) | lo);
            i += 2;
        } else if (isHighSurrogate(utf16[i]) || isLowSurrogate(utf16[i])) {
            cp = 0xFFFDu;
            ++i;
        } else {
            ++i;
        }

        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

/// 统计编辑器光标移动 / 退格逻辑步数（\r\n 计 1 步，代理对计 1 步，其余单 UTF-16 码元计 1 步）
inline size_t countEditorSteps(std::wstring_view text) noexcept {
    size_t steps = 0;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        if (text[i] == L'\r' && i + 1 < n && text[i + 1] == L'\n') {
            ++steps;
            i += 2;
        } else if (isHighSurrogate(text[i]) && i + 1 < n && isLowSurrogate(text[i + 1])) {
            ++steps;
            i += 2;
        } else {
            ++steps;
            ++i;
        }
    }
    return steps;
}

/// 在不劈断 UTF-16 代理对的前提下按上限截断宽字符串
inline std::wstring_view truncateSafeUtf16(std::wstring_view text, size_t maxUnits) noexcept {
    if (text.size() <= maxUnits) return text;
    if (maxUnits == 0) return {};
    size_t cut = maxUnits;
    if (isHighSurrogate(text[cut - 1])) {
        --cut;
    }
    return text.substr(0, cut);
}

inline std::wstring_view trimWhitespaceWide(std::wstring_view sv) noexcept {
    auto isWs = [](wchar_t c) noexcept {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
    };
    while (!sv.empty() && isWs(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && isWs(sv.back())) sv.remove_suffix(1);
    return sv;
}

// Howard Hinnant 公历与纪元天数无溢出双向转换（支持任意跨月/跨年/闰年偏移）
constexpr int64_t daysFromCivil(int y, int m, int d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

constexpr void civilFromDays(int64_t z, int& outY, int& outM, int& outD) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp < 10u ? mp + 3u : mp - 9u;
    if (m <= 2u) ++y;
    outY = y;
    outM = static_cast<int>(m);
    outD = static_cast<int>(d);
}

constexpr int computeWeekday(int y, int m, int d) noexcept {
    const int64_t z = daysFromCivil(y, m, d);
    // 1970-01-01 (z=0) 为星期四 (4)
    const int64_t w = (z + 4) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

inline LocalDateTime shiftDateByDays(const LocalDateTime& base, int deltaDays) noexcept {
    LocalDateTime shifted = base;
    const int64_t targetDays = daysFromCivil(base.year, base.month, base.day) + deltaDays;
    civilFromDays(targetDays, shifted.year, shifted.month, shifted.day);
    shifted.weekday = computeWeekday(shifted.year, shifted.month, shifted.day);
    return shifted;
}

inline std::wstring formatDate(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%04d-%02d-%02d", dt.year, dt.month, dt.day);
    return std::wstring(buf);
}

inline std::wstring formatDateCompact(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%04d%02d%02d", dt.year, dt.month, dt.day);
    return std::wstring(buf);
}

inline std::wstring formatDateSlash(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%04d/%02d/%02d", dt.year, dt.month, dt.day);
    return std::wstring(buf);
}

inline std::wstring formatTime(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%02d:%02d", dt.hour, dt.minute);
    return std::wstring(buf);
}

inline std::wstring formatTimeSec(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    return std::wstring(buf);
}

inline std::wstring formatDateTime(const LocalDateTime& dt) {
    wchar_t buf[48]{};
    std::swprintf(
        buf,
        std::size(buf),
        L"%04d-%02d-%02d %02d:%02d",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute);
    return std::wstring(buf);
}

/// 8 国语言本地化星期名映射 (0=周日 .. 6=周六)
inline std::wstring formatLocalizedWeekday(const LocalDateTime& dt, std::string_view localeCode) {
    const int w = (dt.weekday >= 0 && dt.weekday <= 6)
        ? dt.weekday
        : computeWeekday(dt.year, dt.month, dt.day);
    const std::string loc = toLowerAscii(localeCode);

    static constexpr std::array<const wchar_t*, 7> kZhCn = {
        L"\u661F\u671F\u65E5", L"\u661F\u671F\u4E00", L"\u661F\u671F\u4E8C",
        L"\u661F\u671F\u4E09", L"\u661F\u671F\u56DB", L"\u661F\u671F\u4E94", L"\u661F\u671F\u516D"};
    static constexpr std::array<const wchar_t*, 7> kZhTw = {
        L"\u9031\u65E5", L"\u9031\u4E00", L"\u9031\u4E8C",
        L"\u9031\u4E09", L"\u9031\u56DB", L"\u9031\u4E94", L"\u9031\u516D"};
    static constexpr std::array<const wchar_t*, 7> kJa = {
        L"\u65E5\u66DC\u65E5", L"\u6708\u66DC\u65E5", L"\u706B\u66DC\u65E5",
        L"\u6C34\u66DC\u65E5", L"\u6728\u66DC\u65E5", L"\u91D1\u66DC\u65E5", L"\u571F\u66DC\u65E5"};
    static constexpr std::array<const wchar_t*, 7> kKo = {
        L"\uC77C\uC694\uC77C", L"\uC6D4\uC694\uC77C", L"\uD654\uC694\uC77C",
        L"\uC218\uC694\uC77C", L"\uBAA9\uC694\uC77C", L"\uAE08\uC694\uC77C", L"\uD1A0\uC694\uC77C"};
    static constexpr std::array<const wchar_t*, 7> kRu = {
        L"\u0432\u043E\u0441\u043A\u0440\u0435\u0441\u0435\u043D\u044C\u0435",
        L"\u043F\u043E\u043D\u0435\u0434\u0435\u043B\u044C\u043D\u0438\u043A",
        L"\u0432\u0442\u043E\u0440\u043D\u0438\u043A",
        L"\u0441\u0440\u0435\u0434\u0430",
        L"\u0447\u0435\u0442\u0432\u0435\u0440\u0433",
        L"\u043F\u044F\u0442\u043D\u0438\u0446\u0430",
        L"\u0441\u0443\u0431\u0431\u043E\u0442\u0430"};
    static constexpr std::array<const wchar_t*, 7> kPtBr = {
        L"domingo", L"segunda-feira", L"ter\u00E7a-feira",
        L"quarta-feira", L"quinta-feira", L"sexta-feira", L"s\u00E1bado"};
    static constexpr std::array<const wchar_t*, 7> kEs = {
        L"domingo", L"lunes", L"martes",
        L"mi\u00E9rcoles", L"jueves", L"viernes", L"s\u00E1bado"};
    static constexpr std::array<const wchar_t*, 7> kEn = {
        L"Sunday", L"Monday", L"Tuesday",
        L"Wednesday", L"Thursday", L"Friday", L"Saturday"};

    if (loc.rfind("zh-tw", 0) == 0 || loc.rfind("zh-hk", 0) == 0 || loc.rfind("zh-mo", 0) == 0) {
        return kZhTw[static_cast<size_t>(w)];
    }
    if (loc.rfind("zh", 0) == 0) {
        return kZhCn[static_cast<size_t>(w)];
    }
    if (loc.rfind("ja", 0) == 0) {
        return kJa[static_cast<size_t>(w)];
    }
    if (loc.rfind("ko", 0) == 0) {
        return kKo[static_cast<size_t>(w)];
    }
    if (loc.rfind("ru", 0) == 0) {
        return kRu[static_cast<size_t>(w)];
    }
    if (loc.rfind("pt", 0) == 0) {
        return kPtBr[static_cast<size_t>(w)];
    }
    if (loc.rfind("es", 0) == 0) {
        return kEs[static_cast<size_t>(w)];
    }
    return kEn[static_cast<size_t>(w)];
}

/// 解析 {{date+Nd}} 或 {{date-Nd}} 相对日期偏移语法（限制 -36500..+36500 天防溢出）
inline bool tryParseDateOffsetToken(std::wstring_view token, int& outDeltaDays) noexcept {
    outDeltaDays = 0;
    if (token.size() < 7u || token.substr(0, 4) != L"date") return false;
    const wchar_t sign = token[4];
    if (sign != L'+' && sign != L'-') return false;
    if (token.back() != L'd' && token.back() != L'D') return false;
    const std::wstring_view digits = token.substr(5, token.size() - 6);
    if (digits.empty() || digits.size() > 5u) return false;
    int val = 0;
    for (wchar_t c : digits) {
        if (c < L'0' || c > L'9') return false;
        val = val * 10 + static_cast<int>(c - L'0');
        if (val > 36500) return false;
    }
    outDeltaDays = (sign == L'-') ? -val : val;
    return true;
}

inline std::wstring fallbackDeterministicUuidV4(uint64_t seed) {
    auto next64 = [&seed]() noexcept -> uint64_t {
        seed += 0x9E3779B97F4A7C15ull;
        uint64_t z = seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const uint64_t hi = (next64() & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    const uint64_t lo = (next64() & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    wchar_t buf[40]{};
    std::swprintf(
        buf,
        std::size(buf),
        L"%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFu),
        static_cast<unsigned>((hi >> 16) & 0xFFFFu),
        static_cast<unsigned>(hi & 0xFFFFu),
        static_cast<unsigned>((lo >> 48) & 0xFFFFu),
        static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFull));
    return std::wstring(buf);
}

}  // namespace detail

class TextExpanderMatcher {
public:
    static constexpr size_t kMaxBufferLength = 64;
    static constexpr size_t kDefaultMaxClipboardChars = 4096;

    /// 重载规则列表：仅保留启用的有效规则，并按触发词长度降序稳定排序（最长匹配优先）
    void setRules(std::vector<TextExpanderRule> rules) {
        m_rules.clear();
        m_rules.reserve(rules.size());
        for (auto& rule : rules) {
            if (!rule.enabled || rule.trigger.empty()) continue;
            std::wstring trigW = detail::utf8ToUtf16(rule.trigger);
            if (trigW.empty() || trigW.size() > kMaxBufferLength) continue;
            CompiledRule compiled;
            compiled.normalizedTriggerW = detail::normalizeRuleString(
                trigW, rule.caseSensitive, rule.fullwidthSensitive);
            compiled.triggerSteps = detail::countEditorSteps(trigW);
            compiled.requiresWordBoundaryBefore =
                detail::isWordCharWide(trigW.front());
            compiled.triggerW = std::move(trigW);
            compiled.rule = std::move(rule);
            m_rules.push_back(std::move(compiled));
        }

        std::stable_sort(m_rules.begin(), m_rules.end(), [](const CompiledRule& a, const CompiledRule& b) {
            return a.triggerW.size() > b.triggerW.size();
        });
    }

    /// 推送单个键入字符并尝试匹配触发词
    std::optional<MatchResult> pushChar(wchar_t ch) {
        if (ch == L'\0') return std::nullopt;

        if (detail::isDelimiterWide(ch)) {
            if (auto delimHit = matchSuffix(TriggerMode::Delimiter, /*checkWordBoundary=*/true)) {
                delimHit->eraseCount += 1;  // 连带退格清除刚刚按下的分隔符，若 keepDelimiter=true 在展开输出末尾无损补回
                delimHit->delimiterChar = detail::canonicalDelimiterChar(ch);
                delimHit->triggeredByDelimiter = true;
                m_buffer.clear();
                return delimHit;
            }

            appendToBuffer(ch);

            if (auto immHit = matchSuffix(TriggerMode::Immediate, /*checkWordBoundary=*/false)) {
                m_buffer.clear();
                return immHit;
            }
            return std::nullopt;
        }

        appendToBuffer(ch);

        if (auto immHit = matchSuffix(TriggerMode::Immediate, /*checkWordBoundary=*/false)) {
            m_buffer.clear();
            return immHit;
        }

        return std::nullopt;
    }

    /// 退格键修正输入缓冲（正确处理 UTF-16 代理对）
    void backspace() noexcept {
        if (m_buffer.empty()) return;
        const wchar_t back = m_buffer.back();
        m_buffer.pop_back();
        if (detail::isLowSurrogate(back) && !m_buffer.empty() && detail::isHighSurrogate(m_buffer.back())) {
            m_buffer.pop_back();
        }
    }

    /// 清空滑动输入缓冲（鼠标点击、窗口切换、快捷键组合、导航键等场景调用）
    void clear() noexcept {
        m_buffer.clear();
    }

    /// 当前滑动缓冲只读视图
    const std::wstring& buffer() const noexcept {
        return m_buffer;
    }

    /// 已编译启用规则数量
    size_t activeRuleCount() const noexcept {
        return m_rules.size();
    }

    /// 获取当前系统本地日期时间快照
    static LocalDateTime currentLocalDateTime() noexcept {
        const std::time_t now = std::time(nullptr);
        std::tm localTm{};
#if defined(_WIN32)
        localtime_s(&localTm, &now);
#else
        localtime_r(&now, &localTm);
#endif
        LocalDateTime dt;
        dt.year = localTm.tm_year + 1900;
        dt.month = localTm.tm_mon + 1;
        dt.day = localTm.tm_mday;
        dt.hour = localTm.tm_hour;
        dt.minute = localTm.tm_min;
        dt.second = localTm.tm_sec;
        dt.weekday = localTm.tm_wday;
        dt.unixSeconds = static_cast<int64_t>(now);
        return dt;
    }

    /// 完整上下文动态宏单遍展开器（支持 14 类动态变量与 {{date+Nd}} / {{date-Nd}} 相对偏移）
    static ExpandedOutput expandTemplateWithContext(
        std::wstring_view replacement,
        const TemplateExpansionContext& ctx) {
        ExpandedOutput result;
        result.text.reserve(replacement.size() + 48);

        std::optional<size_t> cursorPos;
        uint64_t uuidCounter = static_cast<uint64_t>(
            ctx.now.unixSeconds != 0
                ? ctx.now.unixSeconds
                : detail::daysFromCivil(ctx.now.year, ctx.now.month, ctx.now.day) * 86400ll +
                      ctx.now.hour * 3600 + ctx.now.minute * 60 + ctx.now.second);

        size_t i = 0;
        const size_t n = replacement.size();

        while (i < n) {
            if (replacement[i] == L'{' && i + 1 < n && replacement[i + 1] == L'{') {
                const size_t closePos = replacement.find(L"}}", i + 2);
                if (closePos != std::wstring_view::npos && (closePos - (i + 2)) <= 28u) {
                    const std::wstring_view token = replacement.substr(i + 2, closePos - (i + 2));
                    if (token == L"date") {
                        result.text += detail::formatDate(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"date_compact") {
                        result.text += detail::formatDateCompact(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"date_slash") {
                        result.text += detail::formatDateSlash(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"time") {
                        result.text += detail::formatTime(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"time_sec") {
                        result.text += detail::formatTimeSec(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"datetime") {
                        result.text += detail::formatDateTime(ctx.now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"weekday") {
                        result.text += detail::formatLocalizedWeekday(ctx.now, ctx.localeCode);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"clipboard") {
                        result.text += detail::truncateSafeUtf16(ctx.clipboardText, ctx.maxClipboardChars);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"clipboard_trim") {
                        const auto trimmed = detail::trimWhitespaceWide(ctx.clipboardText);
                        result.text += detail::truncateSafeUtf16(trimmed, ctx.maxClipboardChars);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"cursor") {
                        if (!cursorPos.has_value()) {
                            cursorPos = result.text.size();
                        }
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"uuid") {
                        if (ctx.uuidGenerator) {
                            result.text += ctx.uuidGenerator();
                        } else {
                            ++uuidCounter;
                            result.text += detail::fallbackDeterministicUuidV4(uuidCounter);
                        }
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"timestamp") {
                        const int64_t ts = (ctx.now.unixSeconds != 0)
                            ? ctx.now.unixSeconds
                            : (detail::daysFromCivil(ctx.now.year, ctx.now.month, ctx.now.day) * 86400ll +
                               ctx.now.hour * 3600 + ctx.now.minute * 60 + ctx.now.second);
                        wchar_t tsBuf[32]{};
                        std::swprintf(tsBuf, std::size(tsBuf), L"%lld", static_cast<long long>(ts));
                        result.text += tsBuf;
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"user") {
                        result.text += detail::truncateSafeUtf16(ctx.userName, 128);
                        i = closePos + 2;
                        continue;
                    }
                    int deltaDays = 0;
                    if (detail::tryParseDateOffsetToken(token, deltaDays)) {
                        result.text += detail::formatDate(detail::shiftDateByDays(ctx.now, deltaDays));
                        i = closePos + 2;
                        continue;
                    }
                }
            }
            result.text.push_back(replacement[i]);
            ++i;
        }

        if (cursorPos.has_value() && *cursorPos <= result.text.size()) {
            const std::wstring_view tail(result.text.data() + *cursorPos, result.text.size() - *cursorPos);
            result.moveLeftSteps = detail::countEditorSteps(tail);
        }

        return result;
    }

    /// 兼容三参数重载
    static ExpandedOutput expandTemplate(
        std::wstring_view replacement,
        const LocalDateTime& now,
        std::wstring_view clipboardText,
        size_t maxClipboardChars = kDefaultMaxClipboardChars) {
        TemplateExpansionContext ctx;
        ctx.now = now;
        ctx.clipboardText = clipboardText;
        ctx.maxClipboardChars = maxClipboardChars;
        return expandTemplateWithContext(replacement, ctx);
    }

    /// 便捷 UTF-8 动态宏展开重载
    static ExpandedOutput expandTemplateUtf8(
        std::string_view replacementUtf8,
        const LocalDateTime& now,
        std::string_view clipboardUtf8,
        size_t maxClipboardChars = kDefaultMaxClipboardChars) {
        return expandTemplate(
            detail::utf8ToUtf16(replacementUtf8),
            now,
            detail::utf8ToUtf16(clipboardUtf8),
            maxClipboardChars);
    }

    /// 在 Delimiter 边界触发后按规则的 keepDelimiter 策略保留边界字符，并精准维护 {{cursor}} 光标偏移
    static void applyDelimiterRetention(ExpandedOutput& expanded, const MatchResult& match) {
        if (!match.triggeredByDelimiter || !match.rule.keepDelimiter || match.delimiterChar == L'\0') {
            return;
        }
        expanded.text.push_back(match.delimiterChar);
        if (expanded.moveLeftSteps > 0) {
            expanded.moveLeftSteps += 1;
        }
    }

    /// 判断模板中是否引用了 {{clipboard}} 或 {{clipboard_trim}} 宏
    static bool templateUsesClipboard(std::wstring_view replacement) noexcept {
        return replacement.find(L"{{clipboard}}") != std::wstring_view::npos ||
               replacement.find(L"{{clipboard_trim}}") != std::wstring_view::npos;
    }

    /// 判断展开后的纯文本是否适合零剪贴板触碰的极速 Unicode 直接注入
    static bool isFastDirectUnicodeCandidate(std::wstring_view expandedText) noexcept {
        constexpr size_t kMaxDirectChars = 48;
        if (expandedText.size() > kMaxDirectChars) return false;
        for (wchar_t ch : expandedText) {
            if (ch < 0x20 || ch > 0x7E) {
                return false;
            }
        }
        return true;
    }

private:
    struct CompiledRule {
        TextExpanderRule rule;
        std::wstring triggerW;
        std::wstring normalizedTriggerW;
        size_t triggerSteps = 0;
        bool requiresWordBoundaryBefore = false;
    };

    void appendToBuffer(wchar_t ch) {
        m_buffer.push_back(ch);
        if (m_buffer.size() > kMaxBufferLength) {
            size_t excess = m_buffer.size() - kMaxBufferLength;
            if (excess < m_buffer.size() && detail::isLowSurrogate(m_buffer[excess])) {
                ++excess;
            }
            m_buffer.erase(0, std::min(excess, m_buffer.size()));
        }
    }

    std::optional<MatchResult> matchSuffix(TriggerMode targetMode, bool checkWordBoundary) const {
        const size_t bufLen = m_buffer.size();
        if (bufLen == 0) return std::nullopt;

        for (const auto& entry : m_rules) {
            if (entry.rule.mode != targetMode) continue;
            const size_t trigLen = entry.triggerW.size();
            if (trigLen == 0 || bufLen < trigLen) continue;

            const size_t startPos = bufLen - trigLen;

            // Delimiter 模式下，如果触发词首字符为多语种单词字符，要求前一位不是单词字符，防止词尾误触发
            if (checkWordBoundary && entry.requiresWordBoundaryBefore && startPos > 0) {
                if (detail::isWordCharWide(m_buffer[startPos - 1])) {
                    continue;
                }
            }

            bool matched = true;
            for (size_t i = 0; i < trigLen; ++i) {
                const wchar_t normalizedInput = detail::normalizeRuleChar(
                    m_buffer[startPos + i],
                    entry.rule.caseSensitive,
                    entry.rule.fullwidthSensitive);
                if (normalizedInput != entry.normalizedTriggerW[i]) {
                    matched = false;
                    break;
                }
            }

            if (matched) {
                MatchResult res;
                res.rule = entry.rule;
                res.eraseCount = entry.triggerSteps;
                return res;
            }
        }
        return std::nullopt;
    }

    std::vector<CompiledRule> m_rules;
    std::wstring m_buffer;
};

}  // namespace tools3000::text_expander

#endif  // TOOLS3000_TEXT_EXPANDER_MATCHER_H
