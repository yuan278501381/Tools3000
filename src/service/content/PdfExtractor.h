#pragma once

#include "IContentExtractor.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service::content {

class PdfExtractor : public IContentExtractor {
public:
    PdfExtractor() = default;
    ~PdfExtractor() override = default;

    bool canHandle(std::wstring_view extension) const override;

    void appendSupportedExtensions(std::unordered_set<std::wstring>& out) const override {
        out.insert(L"pdf");
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

    static bool extractPdfTextFromMemory(
        const uint8_t* data,
        size_t size,
        std::wstring& outText,
        const CancellationCheck& isCancelled = {}
    );
};

} // namespace tools3000::service::content
