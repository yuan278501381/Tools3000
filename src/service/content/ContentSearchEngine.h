#pragma once

#include "IContentExtractor.h"
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tools3000::service::content {

class ContentSearchEngine {
public:
    static ContentSearchEngine& instance();

    ContentSearchEngine();
    ~ContentSearchEngine() = default;

    AllowedExtensionSnapshot buildAllowedExtensionSnapshot(
        const ContentFormatFilter* requestFilter = nullptr
    ) const;

    bool canSearchContent(
        std::wstring_view extension,
        const ContentFormatFilter* requestFilter = nullptr
    ) const;

    void configureFormats(
        const std::vector<std::wstring>& customExts,
        const std::vector<std::wstring>& disabledExts
    );

    bool searchFile(
        const std::wstring& filePath,
        std::wstring_view queryPattern,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3
    );

    bool searchFile(
        const std::wstring& filePath,
        const std::vector<std::wstring>& queryTerms,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3,
        const CancellationCheck& isCancelled = {},
        const ContentFormatFilter* requestFilter = nullptr
    );

private:
    IContentExtractor* selectExtractor(
        const std::wstring& lowerExt,
        const ContentFormatFilter* requestFilter
    ) const;

    std::vector<std::unique_ptr<IContentExtractor>> m_extractors;
    std::unordered_set<std::wstring> m_customExts;
    std::unordered_set<std::wstring> m_disabledExts;
    mutable std::shared_mutex m_mutex;
};

} // namespace tools3000::service::content
