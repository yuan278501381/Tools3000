#pragma once

#include "IContentExtractor.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tools3000::service::content {

class WindowsIFilterExtractor : public IContentExtractor {
public:
    WindowsIFilterExtractor();
    ~WindowsIFilterExtractor() override = default;

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

    static bool extractDocumentText(
        const std::wstring& filePath,
        std::wstring_view lowerExt,
        std::wstring& outText,
        const CancellationCheck& isCancelled = {}
    );

    static bool extractViaSystemIFilter(
        const std::wstring& filePath,
        std::wstring& outText,
        const CancellationCheck& isCancelled = {}
    );

    static bool extractViaSystemIFilterFromHandle(
        void* hFile,
        const std::wstring& fileHintPath,
        const uint8_t* mappedData,
        size_t mappedSize,
        std::wstring& outText,
        const CancellationCheck& isCancelled = {}
    );

    static bool extractViaBinaryTextScan(
        const uint8_t* data,
        size_t size,
        std::wstring& outText,
        const CancellationCheck& isCancelled = {}
    );

private:
    std::unordered_set<std::wstring> m_supportedExts;
};

} // namespace tools3000::service::content
