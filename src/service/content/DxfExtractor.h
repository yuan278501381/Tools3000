#pragma once

#include "IContentExtractor.h"
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tools3000::service::content {

class DxfExtractor : public IContentExtractor {
public:
    DxfExtractor();
    ~DxfExtractor() override = default;

    bool canHandle(std::wstring_view extension) const override;

    void appendSupportedExtensions(std::unordered_set<std::wstring>& out) const override {
        out.insert(m_supportedExts.begin(), m_supportedExts.end());
    }

    bool searchContent(
        const std::wstring& filePath,
        std::wstring_view queryPattern,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3
    ) override;

    bool searchContentTerms(
        const std::wstring& filePath,
        const std::vector<std::wstring>& queryTerms,
        bool caseSensitive,
        std::vector<ContentSnippet>& outSnippets,
        size_t maxSnippetsPerFile = 3,
        const CancellationCheck& isCancelled = {}
    ) override;

private:
    std::unordered_set<std::wstring> m_supportedExts;
};

} // namespace tools3000::service::content
