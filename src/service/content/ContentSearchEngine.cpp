#include "ContentSearchEngine.h"
#include "ContentMatchUtils.h"
#include "PlainTextExtractor.h"
#include "ZipXmlExtractor.h"
#include "PdfExtractor.h"
#include "WindowsIFilterExtractor.h"
#include "PsdAiExtractor.h"
#include "DxfExtractor.h"

#include <algorithm>
#include <cwctype>
#include <mutex>

namespace tools3000::service::content {

ContentSearchEngine& ContentSearchEngine::instance() {
    static ContentSearchEngine engine;
    return engine;
}

ContentSearchEngine::ContentSearchEngine() {
    m_extractors.push_back(std::make_unique<PlainTextExtractor>());
    m_extractors.push_back(std::make_unique<ZipXmlExtractor>());
    m_extractors.push_back(std::make_unique<PdfExtractor>());
    m_extractors.push_back(std::make_unique<WindowsIFilterExtractor>());
    m_extractors.push_back(std::make_unique<PsdAiExtractor>());
    m_extractors.push_back(std::make_unique<DxfExtractor>());
}

void ContentSearchEngine::configureFormats(
    const std::vector<std::wstring>& customExts,
    const std::vector<std::wstring>& disabledExts
) {
    std::unique_lock lock(m_mutex);
    m_disabledExts.clear();
    for (const auto& ext : disabledExts) {
        std::wstring lower = ContentFormatFilter::normalizeExt(ext);
        if (!lower.empty()) {
            m_disabledExts.insert(std::move(lower));
        }
    }

    m_customExts.clear();
    for (const auto& ext : customExts) {
        std::wstring lower = ContentFormatFilter::normalizeExt(ext);
        if (!lower.empty()) {
            m_customExts.insert(std::move(lower));
        }
    }
}

IContentExtractor* ContentSearchEngine::selectExtractor(
    const std::wstring& lowerExt,
    const ContentFormatFilter* requestFilter
) const {
    if (lowerExt.empty()) return nullptr;

    if (requestFilter && requestFilter->disabledExts.find(lowerExt) != requestFilter->disabledExts.end()) {
        return nullptr;
    }

    std::shared_lock lock(m_mutex);
    if (m_disabledExts.find(lowerExt) != m_disabledExts.end()) {
        return nullptr;
    }

    for (const auto& extractor : m_extractors) {
        if (extractor->canHandle(lowerExt)) {
            return extractor.get();
        }
    }

    const bool isCustom =
        (requestFilter && requestFilter->customExts.find(lowerExt) != requestFilter->customExts.end()) ||
        (m_customExts.find(lowerExt) != m_customExts.end());
    if (isCustom && !m_extractors.empty()) {
        // Custom user extensions default to PlainTextExtractor (index 0)
        return m_extractors.front().get();
    }

    return nullptr;
}

bool ContentSearchEngine::canSearchContent(
    std::wstring_view extension,
    const ContentFormatFilter* requestFilter
) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    return selectExtractor(lowerExt, requestFilter) != nullptr;
}

AllowedExtensionSnapshot ContentSearchEngine::buildAllowedExtensionSnapshot(
    const ContentFormatFilter* requestFilter
) const {
    AllowedExtensionSnapshot snapshot;
    std::unordered_set<std::wstring> rawSupported;
    std::shared_lock lock(m_mutex);
    if (!m_extractors.empty()) {
        for (const auto& extractor : m_extractors) {
            extractor->appendSupportedExtensions(rawSupported);
        }
        for (const auto& customExt : m_customExts) {
            rawSupported.insert(customExt);
        }
        if (requestFilter) {
            for (const auto& customExt : requestFilter->customExts) {
                rawSupported.insert(customExt);
            }
        }
    }
    for (const auto& ext : rawSupported) {
        if (m_disabledExts.find(ext) != m_disabledExts.end()) {
            continue;
        }
        if (requestFilter && requestFilter->disabledExts.find(ext) != requestFilter->disabledExts.end()) {
            continue;
        }
        snapshot.insertNormalized(ext);
    }
    return snapshot;
}

bool ContentSearchEngine::searchFile(
    const std::wstring& filePath,
    std::wstring_view queryPattern,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile
) {
    if (queryPattern.empty()) return false;
    return searchFile(
        filePath,
        splitQueryPatternTerms(queryPattern),
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        {},
        nullptr
    );
}

bool ContentSearchEngine::searchFile(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled,
    const ContentFormatFilter* requestFilter
) {
    if (queryTerms.empty() || (isCancelled && isCancelled())) return false;

    const size_t dotPos = filePath.rfind(L'.');
    if (dotPos == std::wstring::npos) return false;

    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(
        std::wstring_view(filePath).substr(dotPos + 1));
    if (lowerExt.empty()) return false;

    IContentExtractor* selected = selectExtractor(lowerExt, requestFilter);
    if (!selected) return false;

    return selected->searchContentTerms(
        filePath,
        queryTerms,
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        isCancelled
    );
}

} // namespace tools3000::service::content
