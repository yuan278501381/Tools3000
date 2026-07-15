#pragma once

#include <cstdint>
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tools3000::service::content {

using CancellationCheck = std::function<bool()>;

struct TransparentWStringHash {
    using is_transparent = void;

    size_t operator()(std::wstring_view sv) const noexcept {
        return std::hash<std::wstring_view>{}(sv);
    }

    size_t operator()(const std::wstring& s) const noexcept {
        return std::hash<std::wstring_view>{}(s);
    }
};

struct AllowedExtensionSnapshot {
    std::unordered_set<std::wstring, TransparentWStringHash, std::equal_to<>> extensions;
    size_t maxExtLength = 0;

    void insertNormalized(std::wstring normExt) {
        if (normExt.empty()) return;
        if (normExt.size() > maxExtLength) {
            maxExtLength = normExt.size();
        }
        extensions.insert(std::move(normExt));
    }

    bool contains(std::wstring_view rawExt) const noexcept {
        if (rawExt.empty() || maxExtLength == 0) return false;
        while (!rawExt.empty() && rawExt.front() == L'.') {
            rawExt.remove_prefix(1);
        }
        if (rawExt.empty() || rawExt.size() > 32) return false;
        wchar_t lowerBuf[32];
        size_t outLen = 0;
        for (wchar_t ch : rawExt) {
            if (!std::iswspace(ch)) {
                if (outLen >= maxExtLength) return false;
                lowerBuf[outLen++] = static_cast<wchar_t>(std::towlower(ch));
            }
        }
        if (outLen == 0) return false;
        return extensions.find(std::wstring_view(lowerBuf, outLen)) != extensions.end();
    }
};

struct ContentSnippet {
    uint32_t lineNumber = 0;
    std::wstring lineContent;
    uint32_t matchOffset = 0;
    uint32_t matchLength = 0;
};

struct ContentFormatFilter {
    std::unordered_set<std::wstring> customExts;
    std::unordered_set<std::wstring> disabledExts;

    static std::wstring normalizeExt(std::wstring_view ext) {
        std::wstring lower;
        lower.reserve(ext.size());
        for (wchar_t ch : ext) {
            if (ch != L'.' && !std::iswspace(ch)) {
                lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
            }
        }
        return lower;
    }

    static ContentFormatFilter fromLists(
        const std::vector<std::wstring>& customList,
        const std::vector<std::wstring>& disabledList
    ) {
        ContentFormatFilter filter;
        for (const auto& item : customList) {
            std::wstring norm = normalizeExt(item);
            if (!norm.empty()) filter.customExts.insert(std::move(norm));
        }
        for (const auto& item : disabledList) {
            std::wstring norm = normalizeExt(item);
            if (!norm.empty()) filter.disabledExts.insert(std::move(norm));
        }
        return filter;
    }
};

class IContentExtractor {
public:
    virtual ~IContentExtractor() = default;

    virtual bool canHandle(std::wstring_view extension) const = 0;

    virtual void appendSupportedExtensions(std::unordered_set<std::wstring>& /*out*/) const {}

    virtual bool searchContent(
        const std::wstring& filePath,
        std::wstring_view queryPattern,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3
    ) = 0;

    virtual bool searchContentTerms(
        const std::wstring& filePath,
        const std::vector<std::wstring>& queryTerms,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3,
        const CancellationCheck& isCancelled = {}
    ) {
        if (isCancelled && isCancelled()) return false;
        std::wstring joined;
        for (const auto& term : queryTerms) {
            if (term.empty()) continue;
            if (!joined.empty()) joined.push_back(L' ');
            joined += term;
        }
        if (joined.empty()) return false;
        return searchContent(filePath, joined, caseSensitive, outSnippets, maxSnippetsPerFile);
    }
};

} // namespace tools3000::service::content
