#pragma once
// -----------------------------------------------------------------------------
// TextExpanderMatcher.h -- 纯标准 C++20 滑动窗口触发词匹配与动态宏展开引擎
//
// 设计原则:
//   1. 零 Win32 API 依赖，100% 跨平台可单测（支持 x64 / ARM64）。
//   2. 最长触发词优先匹配（Longest Match First），支持 Immediate 与 Delimiter 双模式。
//   3. 单遍扫描动态宏替换，彻底免疫剪贴板内容递归宏展开漏洞。
//   4. 精准计算 UTF-16 / 码点级退格次数与 {{cursor}} 左移步数。
// -----------------------------------------------------------------------------

#ifndef TOOLS3000_TEXT_EXPANDER_MATCHER_H
#define TOOLS3000_TEXT_EXPANDER_MATCHER_H

#include "text_expander/TextExpanderTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
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

inline wchar_t toLowerAsciiWide(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return ch;
}

inline std::wstring toLowerWide(std::wstring_view input) {
    std::wstring out;
    out.reserve(input.size());
    for (wchar_t ch : input) {
        out.push_back(toLowerAsciiWide(ch));
    }
    return out;
}

inline bool isAsciiAlphaNumWide(wchar_t ch) noexcept {
    return (ch >= L'0' && ch <= L'9') ||
           (ch >= L'A' && ch <= L'Z') ||
           (ch >= L'a' && ch <= L'z') ||
           ch == L'_';
}

inline bool isDelimiterWide(wchar_t ch) noexcept {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

/// 纯标准 RFC 3629 UTF-8 -> UTF-16 (std::wstring on Windows 16-bit wchar_t)
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

inline std::wstring formatDate(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%04d-%02d-%02d", dt.year, dt.month, dt.day);
    return std::wstring(buf);
}

inline std::wstring formatTime(const LocalDateTime& dt) {
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%02d:%02d", dt.hour, dt.minute);
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
            compiled.rule = std::move(rule);
            compiled.triggerW = std::move(trigW);
            compiled.triggerLowerW = detail::toLowerWide(compiled.triggerW);
            compiled.triggerSteps = detail::countEditorSteps(compiled.triggerW);
            compiled.requiresWordBoundaryBefore =
                detail::isAsciiAlphaNumWide(compiled.triggerW.front());
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
            // 1. 分隔符优先尝试触发 Delimiter 模式规则（最长触发词优先）
            if (auto delimHit = matchSuffix(TriggerMode::Delimiter, /*checkWordBoundary=*/true)) {
                delimHit->eraseCount += 1;  // 连带退格清除刚刚按下的分隔符本身
                delimHit->delimiterChar = ch;
                delimHit->triggeredByDelimiter = true;
                m_buffer.clear();
                return delimHit;
            }

            // 2. 未命中 Delimiter 规则：将分隔符记录到滑动窗口中以作为后续单词边界上下文
            appendToBuffer(ch);

            // 3. 若用户显式配置了以空白结尾的 Immediate 规则，也支持平滑命中
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

    /// 当前滑动缓冲只读视图（用于诊断与单元测试断言）
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
        return dt;
    }

    /// 动态宏单遍展开器：支持 {{date}} / {{time}} / {{datetime}} / {{clipboard}} / {{cursor}}
    /// 单遍扫描保证剪贴板中若含 "{{clipboard}}" 或 "{{date}}" 绝不会被递归二次展开。
    static ExpandedOutput expandTemplate(
        std::wstring_view replacement,
        const LocalDateTime& now,
        std::wstring_view clipboardText,
        size_t maxClipboardChars = kDefaultMaxClipboardChars) {
        ExpandedOutput result;
        result.text.reserve(replacement.size() + 32);

        std::optional<size_t> cursorPos;
        size_t i = 0;
        const size_t n = replacement.size();

        while (i < n) {
            if (replacement[i] == L'{' && i + 1 < n && replacement[i + 1] == L'{') {
                const size_t closePos = replacement.find(L"}}", i + 2);
                if (closePos != std::wstring_view::npos) {
                    const std::wstring_view token = replacement.substr(i + 2, closePos - (i + 2));
                    if (token == L"date") {
                        result.text += detail::formatDate(now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"time") {
                        result.text += detail::formatTime(now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"datetime") {
                        result.text += detail::formatDateTime(now);
                        i = closePos + 2;
                        continue;
                    }
                    if (token == L"clipboard") {
                        result.text += detail::truncateSafeUtf16(clipboardText, maxClipboardChars);
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

    /// 判断模板中是否引用了 {{clipboard}} 宏
    static bool templateUsesClipboard(std::wstring_view replacement) noexcept {
        return replacement.find(L"{{clipboard}}") != std::wstring_view::npos;
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
        std::wstring triggerLowerW;
        size_t triggerSteps = 0;
        bool requiresWordBoundaryBefore = false;
    };

    void appendToBuffer(wchar_t ch) {
        m_buffer.push_back(ch);
        if (m_buffer.size() > kMaxBufferLength) {
            size_t excess = m_buffer.size() - kMaxBufferLength;
            // 避免截断后缓冲区头部留下孤立低代理
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

            // Delimiter 模式下，如果触发词首字符为字母/数字，要求前一位不是字母/数字，防止英文单词后缀误触发
            if (checkWordBoundary && entry.requiresWordBoundaryBefore && startPos > 0) {
                if (detail::isAsciiAlphaNumWide(m_buffer[startPos - 1])) {
                    continue;
                }
            }

            bool matched = true;
            if (entry.rule.caseSensitive) {
                for (size_t i = 0; i < trigLen; ++i) {
                    if (m_buffer[startPos + i] != entry.triggerW[i]) {
                        matched = false;
                        break;
                    }
                }
            } else {
                for (size_t i = 0; i < trigLen; ++i) {
                    if (detail::toLowerAsciiWide(m_buffer[startPos + i]) != entry.triggerLowerW[i]) {
                        matched = false;
                        break;
                    }
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
