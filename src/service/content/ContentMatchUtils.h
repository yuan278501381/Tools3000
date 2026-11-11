#pragma once

#include "IContentExtractor.h"
#include "RustContentKernel.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service::content {

namespace match_constants {
constexpr size_t MaxSnippetLineChars = 220;
constexpr size_t SnippetContextPaddingChars = 48;
constexpr double MaxGarbledCharRatio = 0.20;
constexpr size_t CancellationLineCheckMask = 0xFFu;
} // namespace match_constants

inline bool isHighSurrogate(wchar_t ch) noexcept {
    const uint16_t code = static_cast<uint16_t>(ch);
    return code >= 0xD800u && code <= 0xDBFFu;
}

inline bool isLowSurrogate(wchar_t ch) noexcept {
    const uint16_t code = static_cast<uint16_t>(ch);
    return code >= 0xDC00u && code <= 0xDFFFu;
}

inline std::vector<std::wstring> splitQueryPatternTerms(std::wstring_view queryPattern) {
    std::vector<std::wstring> terms;
    size_t i = 0;
    while (i < queryPattern.size()) {
        while (i < queryPattern.size() && std::iswspace(queryPattern[i])) ++i;
        if (i >= queryPattern.size()) break;

        if (queryPattern[i] == L'"') {
            size_t start = ++i;
            while (i < queryPattern.size() && queryPattern[i] != L'"') ++i;
            std::wstring phrase(queryPattern.substr(start, i - start));
            if (!phrase.empty()) terms.push_back(std::move(phrase));
            if (i < queryPattern.size()) ++i;
        } else {
            size_t start = i;
            while (i < queryPattern.size() && !std::iswspace(queryPattern[i])) ++i;
            std::wstring word(queryPattern.substr(start, i - start));
            if (!word.empty()) terms.push_back(std::move(word));
        }
    }
    return terms;
}

inline void sanitizeSnippetLineInPlace(std::wstring& line) {
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t ch = line[i];
        if (ch == L'\0' || (ch < 0x20 && ch != L'\t')) {
            line[i] = L' ';
        } else if (isHighSurrogate(ch)) {
            if (i + 1 < line.size() && isLowSurrogate(line[i + 1])) {
                ++i;
            } else {
                line[i] = 0xFFFD;
            }
        } else if (isLowSurrogate(ch)) {
            line[i] = 0xFFFD;
        }
    }
}

inline bool isSnippetLineGarbled(std::wstring_view line) noexcept {
    if (line.empty()) return false;
    size_t badChars = 0;
    for (wchar_t ch : line) {
        if (ch == 0xFFFD || ch == 0x0000 || (ch < 0x20 && ch != L'\t')) {
            ++badChars;
        }
    }
    return (static_cast<double>(badChars) / static_cast<double>(line.size())) >
           match_constants::MaxGarbledCharRatio;
}

// Formats a single line into a UTF-16 code-unit aligned ContentSnippet.
// JavaScript String.prototype.substring(matchOffset, matchOffset + matchLength) in
// SearchResultRow.tsx indexes UTF-16 code units, matching std::wstring wchar_t indices 1:1.
inline std::optional<ContentSnippet> buildAlignedSnippet(
    uint32_t lineNumber,
    std::wstring line,
    size_t matchPos,
    size_t matchLen
) {
    if (line.empty() || matchLen == 0 || matchPos + matchLen > line.size()) {
        return std::nullopt;
    }

    sanitizeSnippetLineInPlace(line);
    if (isSnippetLineGarbled(line)) {
        return std::nullopt;
    }

    uint32_t snippetOffset = static_cast<uint32_t>(matchPos);
    if (line.size() > match_constants::MaxSnippetLineChars) {
        size_t start = (matchPos > match_constants::SnippetContextPaddingChars)
            ? (matchPos - match_constants::SnippetContextPaddingChars)
            : 0;
        if (start > 0 && start < line.size() && isLowSurrogate(line[start])) {
            --start;
        }

        size_t end = (std::min)(
            line.size(),
            matchPos + matchLen + match_constants::SnippetContextPaddingChars
        );
        if (end < line.size() && end > 0 && isHighSurrogate(line[end - 1])) {
            ++end;
        }

        std::wstring truncated;
        truncated.reserve((end - start) + 8);
        if (start > 0) {
            truncated += L"...";
        }
        snippetOffset = static_cast<uint32_t>(truncated.size() + (matchPos - start));
        truncated.append(line.data() + start, end - start);
        if (end < line.size()) {
            truncated += L"...";
        }
        line = std::move(truncated);
    }

    return ContentSnippet{
        .lineNumber = lineNumber,
        .lineContent = std::move(line),
        .matchOffset = snippetOffset,
        .matchLength = static_cast<uint32_t>(matchLen)
    };
}

class MultiTermDocumentMatcher {
public:
    MultiTermDocumentMatcher(
        const std::vector<std::wstring>& rawTerms,
        bool caseSensitive,
        size_t maxSnippetsPerFile
    )
        : m_caseSensitive(caseSensitive),
          m_maxSnippets((std::max<size_t>)(1, maxSnippetsPerFile)) {
        for (const auto& term : rawTerms) {
            if (term.empty()) continue;
            std::wstring prepared = term;
            if (!m_caseSensitive) {
                for (auto& ch : prepared) {
                    ch = static_cast<wchar_t>(std::towlower(ch));
                }
            }
            m_terms.push_back(std::move(prepared));
        }
        m_termMatched.assign(m_terms.size(), false);
    }

    bool hasValidTerms() const noexcept {
        return !m_terms.empty();
    }

    bool allTermsMatched() const noexcept {
        return !m_terms.empty() && m_matchedTermCount == m_terms.size();
    }

    bool canStopEarly() const noexcept {
        return allTermsMatched() &&
               (m_primarySnippets.size() + m_extraSnippets.size()) >= m_maxSnippets;
    }

    void processLine(uint32_t lineNumber, std::wstring_view rawLine) {
        if (m_terms.empty() || rawLine.empty()) return;

        // Trim trailing CR/LF while preserving leading indentation for accurate offsets
        size_t trimmedLen = rawLine.size();
        while (trimmedLen > 0 && (rawLine[trimmedLen - 1] == L'\r' || rawLine[trimmedLen - 1] == L'\n')) {
            --trimmedLen;
        }
        if (trimmedLen == 0) return;
        std::wstring_view cleanView = rawLine.substr(0, trimmedLen);

        std::wstring foldedStorage;
        std::wstring_view haystack = cleanView;
        if (!m_caseSensitive) {
            foldedStorage.resize(cleanView.size());
            for (size_t i = 0; i < cleanView.size(); ++i) {
                foldedStorage[i] = static_cast<wchar_t>(std::towlower(cleanView[i]));
            }
            haystack = foldedStorage;
        }

        size_t firstHitPos = std::wstring_view::npos;
        size_t firstHitLen = 0;
        bool lineMatchedUnseenTerm = false;

        for (size_t idx = 0; idx < m_terms.size(); ++idx) {
            const auto& needle = m_terms[idx];
            const size_t pos = haystack.find(needle);
            if (pos != std::wstring_view::npos) {
                if (!m_termMatched[idx]) {
                    m_termMatched[idx] = true;
                    ++m_matchedTermCount;
                    if (!lineMatchedUnseenTerm) {
                        lineMatchedUnseenTerm = true;
                        firstHitPos = pos;
                        firstHitLen = needle.size();
                    }
                } else if (firstHitPos == std::wstring_view::npos) {
                    firstHitPos = pos;
                    firstHitLen = needle.size();
                }
            }
        }

        if (firstHitPos == std::wstring_view::npos) {
            return;
        }

        if (lineMatchedUnseenTerm && m_primarySnippets.size() < m_maxSnippets) {
            auto snip = buildAlignedSnippet(lineNumber, std::wstring(cleanView), firstHitPos, firstHitLen);
            if (snip.has_value()) {
                m_primarySnippets.push_back(std::move(*snip));
            }
        } else if ((m_primarySnippets.size() + m_extraSnippets.size()) < m_maxSnippets) {
            auto snip = buildAlignedSnippet(lineNumber, std::wstring(cleanView), firstHitPos, firstHitLen);
            if (snip.has_value()) {
                m_extraSnippets.push_back(std::move(*snip));
            }
        }
    }

    bool finalize(std::vector<ContentSnippet>& outSnippets) {
        if (!allTermsMatched()) {
            outSnippets.clear();
            return false;
        }

        outSnippets.clear();
        outSnippets.reserve((std::min)(m_maxSnippets, m_primarySnippets.size() + m_extraSnippets.size()));
        for (auto& snip : m_primarySnippets) {
            if (outSnippets.size() >= m_maxSnippets) break;
            outSnippets.push_back(std::move(snip));
        }
        for (auto& snip : m_extraSnippets) {
            if (outSnippets.size() >= m_maxSnippets) break;
            outSnippets.push_back(std::move(snip));
        }
        std::sort(outSnippets.begin(), outSnippets.end(), [](const ContentSnippet& a, const ContentSnippet& b) {
            return a.lineNumber < b.lineNumber;
        });
        return !outSnippets.empty();
    }

private:
    bool m_caseSensitive = false;
    size_t m_maxSnippets = 3;
    std::vector<std::wstring> m_terms;
    std::vector<bool> m_termMatched;
    size_t m_matchedTermCount = 0;
    std::vector<ContentSnippet> m_primarySnippets;
    std::vector<ContentSnippet> m_extraSnippets;
};

inline uint8_t rustCancelBridgeCallback(const void* ctx) noexcept {
    if (!ctx) return 0;
    try {
        const auto* cancelFn = static_cast<const CancellationCheck*>(ctx);
        return (*cancelFn && (*cancelFn)()) ? 1 : 0;
    } catch (...) {
        return 1;
    }
}

inline void rustEmitSnippetBridgeCallback(
    void* ctx,
    uint32_t lineNumber,
    const uint16_t* linePtr,
    size_t lineLen,
    uint32_t matchOffset,
    uint32_t matchLength
) noexcept {
    if (!ctx || !linePtr || lineLen == 0) return;
    try {
        auto* outSnippets = static_cast<std::vector<ContentSnippet>*>(ctx);
        outSnippets->push_back(ContentSnippet{
            .lineNumber = lineNumber,
            .lineContent = std::wstring(reinterpret_cast<const wchar_t*>(linePtr), lineLen),
            .matchOffset = matchOffset,
            .matchLength = matchLength
        });
    } catch (...) {
        // Never allow C++ allocation or callback exceptions to unwind into Rust frames.
    }
}

inline bool matchDocumentTextAndBuildSnippets(
    std::wstring_view fullText,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile = 3,
    const CancellationCheck& isCancelled = {}
) {
    outSnippets.clear();
    if (fullText.empty() || queryTerms.empty()) {
        return false;
    }

    std::vector<T3kUtf16Slice> termSlices;
    termSlices.reserve(queryTerms.size());
    for (const auto& t : queryTerms) {
        if (!t.empty()) {
            termSlices.push_back(T3kUtf16Slice{
                reinterpret_cast<const uint16_t*>(t.data()),
                t.size()
            });
        }
    }
    if (termSlices.empty()) {
        return false;
    }

    const uint8_t matched = t3k_rs_match_utf16_document(
        reinterpret_cast<const uint16_t*>(fullText.data()),
        fullText.size(),
        termSlices.data(),
        termSlices.size(),
        caseSensitive ? 1 : 0,
        maxSnippetsPerFile,
        isCancelled ? &rustCancelBridgeCallback : nullptr,
        isCancelled ? &isCancelled : nullptr,
        &rustEmitSnippetBridgeCallback,
        &outSnippets
    );
    if (!matched) {
        outSnippets.clear();
        return false;
    }
    return !outSnippets.empty();
}

} // namespace tools3000::service::content
