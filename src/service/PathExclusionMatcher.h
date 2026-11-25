#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PathExclusionMatcher.h — 统一多路径/目录段边界精确排除匹配器（SSOT）
//
// 设计原则:
//   - 统一规范化 `/` -> `\`、ASCII 与宽字符小写化、去除非盘符根尾斜杠；
//   - 绝对路径规则（如 `D:\Movies`）按路径边界前缀精确匹配：
//     命中 `D:\Movies` 本体及其下任意深度子文件/子目录，但绝不误伤 `D:\Movies2`；
//   - 多段相对路径规则（如 `pip\cache`）按 `\<pattern>\` 或 `\<pattern>` 路径段边界匹配；
//   - 单段名称规则（如 `node_modules`、`.git`）保持目录段/文件名子串兼容。
//   - 同时服务于 MftParser::Search（文件名搜索）与 MftParser::CollectContentCandidates（全文内容搜索）。
// ─────────────────────────────────────────────────────────────────────────────

#include "SearchExpression.h"
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service {

class PathExclusionMatcher {
public:
    void compile(const std::vector<std::wstring>& rawPatterns) {
        m_absolutePrefixes.clear();
        m_relativePathPatterns.clear();
        m_singleSegmentPatterns.clear();

        for (const auto& raw : rawPatterns) {
            std::wstring norm = normalizePattern(raw);
            if (norm.empty()) continue;

            const bool hasColon = norm.find(L':') != std::wstring::npos;
            const bool hasSlash = norm.find(L'\\') != std::wstring::npos;

            if (hasColon && norm.size() >= 2 && norm[1] == L':') {
                // 绝对盘符路径（例如 `d:\movies` 或 `d:\`）
                if (norm.size() == 2) {
                    norm.push_back(L'\\');
                }
                appendUnique(m_absolutePrefixes, std::move(norm));
            } else if (hasSlash) {
                // 多段相对路径（去除首尾反斜杠后统一以边界方式匹配）
                while (!norm.empty() && norm.front() == L'\\') norm.erase(norm.begin());
                while (!norm.empty() && norm.back() == L'\\') norm.pop_back();
                if (!norm.empty()) {
                    appendUnique(m_relativePathPatterns, std::move(norm));
                }
            } else {
                appendUnique(m_singleSegmentPatterns, std::move(norm));
            }
        }
    }

    bool empty() const noexcept {
        return m_absolutePrefixes.empty() &&
               m_relativePathPatterns.empty() &&
               m_singleSegmentPatterns.empty();
    }

    bool hasOnlySingleSegmentPatterns() const noexcept {
        return m_absolutePrefixes.empty() && m_relativePathPatterns.empty();
    }

    const std::vector<std::wstring>& singleSegmentPatterns() const noexcept {
        return m_singleSegmentPatterns;
    }

    /// 检查给定目录或文件完整路径（已小写且反斜杠分隔，如 `d:\movies\a.mp4` 或 `d:\movies`）是否被任一规则排除。
    /// `checkSingleSegmentInPath` 为 true 时也会检查路径内是否包含单段规则（适用于目录剪枝缓存）。
    bool matchesNormalizedFullPath(std::wstring_view lowerFullPath,
                                   bool checkSingleSegmentInPath = true) const noexcept {
        if (lowerFullPath.empty()) return false;

        for (const auto& absPrefix : m_absolutePrefixes) {
            if (matchesAbsolutePrefix(lowerFullPath, absPrefix)) {
                return true;
            }
        }

        for (const auto& relPat : m_relativePathPatterns) {
            if (matchesRelativeSegmentBoundary(lowerFullPath, relPat)) {
                return true;
            }
        }

        if (checkSingleSegmentInPath) {
            for (const auto& segPat : m_singleSegmentPatterns) {
                if (lowerFullPath.find(segPat) != std::wstring_view::npos) {
                    return true;
                }
            }
        }

        return false;
    }

    /// 检查单段文件名或目录名（原始大小写）是否命中单段排除规则
    bool matchesSingleSegmentName(std::wstring_view rawName) const noexcept {
        if (m_singleSegmentPatterns.empty() || rawName.empty()) return false;
        for (const auto& segPat : m_singleSegmentPatterns) {
            if (containsCaseInsensitive(rawName, segPat)) {
                return true;
            }
        }
        return false;
    }

    static std::wstring normalizePathForMatch(std::wstring_view rawPath) {
        std::wstring s = SearchExpression::normalize(rawPath);
        for (wchar_t& ch : s) {
            if (ch == L'/') ch = L'\\';
        }
        // 合并连续反斜杠（保留 UNC 前缀 `\\`）
        std::wstring collapsed;
        collapsed.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == L'\\' && !collapsed.empty() && collapsed.back() == L'\\' && i > 1) {
                continue;
            }
            collapsed.push_back(s[i]);
        }
        // 去除非盘符根（如 `d:\`）的尾部反斜杠
        while (collapsed.size() > 3 && collapsed.back() == L'\\') {
            collapsed.pop_back();
        }
        return collapsed;
    }

private:
    static std::wstring normalizePattern(std::wstring_view raw) {
        size_t start = 0;
        while (start < raw.size() && std::iswspace(raw[start])) ++start;
        size_t end = raw.size();
        while (end > start && std::iswspace(raw[end - 1])) --end;
        if (start >= end) return {};
        std::wstring_view trimmed = raw.substr(start, end - start);
        if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
            trimmed = trimmed.substr(1, trimmed.size() - 2);
        }
        return normalizePathForMatch(trimmed);
    }

    static void appendUnique(std::vector<std::wstring>& target, std::wstring value) {
        if (std::find(target.begin(), target.end(), value) == target.end()) {
            target.push_back(std::move(value));
        }
    }

    static bool matchesAbsolutePrefix(std::wstring_view lowerFullPath,
                                      std::wstring_view absPrefix) noexcept {
        if (lowerFullPath.size() < absPrefix.size()) return false;
        if (!lowerFullPath.starts_with(absPrefix)) return false;
        // 若 absPrefix 本身是盘符根（如 `d:\`），以它开头的所有路径均匹配
        if (!absPrefix.empty() && absPrefix.back() == L'\\') {
            return true;
        }
        // 精确边界：路径等于 absPrefix 本体，或紧跟路径分隔符 `\`
        return lowerFullPath.size() == absPrefix.size() ||
               lowerFullPath[absPrefix.size()] == L'\\';
    }

    static bool matchesRelativeSegmentBoundary(std::wstring_view lowerFullPath,
                                               std::wstring_view relPat) noexcept {
        if (relPat.empty() || lowerFullPath.size() < relPat.size()) return false;
        size_t pos = 0;
        while ((pos = lowerFullPath.find(relPat, pos)) != std::wstring_view::npos) {
            const bool leftOk = (pos == 0 || lowerFullPath[pos - 1] == L'\\');
            const size_t after = pos + relPat.size();
            const bool rightOk = (after == lowerFullPath.size() || lowerFullPath[after] == L'\\');
            if (leftOk && rightOk) {
                return true;
            }
            pos += 1;
        }
        return false;
    }

    static bool containsCaseInsensitive(std::wstring_view haystack,
                                        std::wstring_view lowerNeedle) noexcept {
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
    }

    std::vector<std::wstring> m_absolutePrefixes;
    std::vector<std::wstring> m_relativePathPatterns;
    std::vector<std::wstring> m_singleSegmentPatterns;
};

}  // namespace tools3000::service
