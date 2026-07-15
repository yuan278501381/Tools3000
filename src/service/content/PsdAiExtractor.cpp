#include "PsdAiExtractor.h"
#include "ContentMatchUtils.h"
#include "ContentTextCache.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxPsdAiFileBytes = 100ULL * 1024ULL * 1024ULL;

std::wstring utf8ToWide(const char* data, size_t length) {
    if (length == 0) return {};
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(length), nullptr, 0);
    if (wideLen <= 0) return {};
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(length), result.data(), wideLen);
    return result;
}

std::wstring extractXmpText(const std::string& xmp) {
    bool insideTag = false;
    std::string currentText;
    currentText.reserve(xmp.size() / 2);

    for (size_t i = 0; i < xmp.size(); ++i) {
        const char c = xmp[i];
        if (c == '<') {
            insideTag = true;
            currentText.push_back(' ');
        } else if (c == '>') {
            insideTag = false;
        } else if (!insideTag) {
            currentText.push_back(c);
        }
    }

    if (currentText.empty()) return {};
    return utf8ToWide(currentText.data(), currentText.size());
}

} // namespace

PsdAiExtractor::PsdAiExtractor() {
    m_supportedExts.insert(L"psd");
    m_supportedExts.insert(L"psb");
    m_supportedExts.insert(L"ai");
}

bool PsdAiExtractor::canHandle(std::wstring_view extension) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    if (lowerExt.empty()) return false;
    return m_supportedExts.find(lowerExt) != m_supportedExts.end();
}

bool PsdAiExtractor::extractPsdAiText(
    const uint8_t* pData,
    size_t dataSize,
    std::wstring& outExtractedText,
    const CancellationCheck& isCancelled
) const {
    if (dataSize < 12) return false;

    std::string_view fullView(reinterpret_cast<const char*>(pData), dataSize);
    const size_t xmpStart = fullView.find("<?xpacket");
    if (xmpStart != std::string_view::npos) {
        const size_t xmpEnd = fullView.find("</xpacket>", xmpStart);
        if (xmpEnd != std::string_view::npos) {
            std::string xmpContent(fullView.substr(xmpStart, xmpEnd - xmpStart + 10));
            std::wstring xmpText = extractXmpText(xmpContent);
            if (!xmpText.empty()) {
                outExtractedText += xmpText;
                outExtractedText += L"\n";
            }
        }
    }

    size_t i = 0;
    while (i + 4 < dataSize) {
        if ((i & 0xFFFFu) == 0 && isCancelled && isCancelled()) {
            outExtractedText.clear();
            return false;
        }
        if (pData[i] == 'T' && pData[i + 1] == 'y' && pData[i + 2] == 'S' && pData[i + 3] == 'h') {
            const size_t scanLimit = (std::min)(dataSize, i + 8192);
            for (size_t k = i + 4; k + 4 < scanLimit; ++k) {
                if ((pData[k] == 0x00 && pData[k + 1] >= 0x20 && pData[k + 1] <= 0x7E) ||
                    (pData[k] >= 0x4E && pData[k] <= 0x9F) ||
                    (pData[k] >= 0x34 && pData[k] <= 0x4D)) {
                    std::wstring layerText;
                    while (k + 1 < scanLimit) {
                        if (pData[k] == 0x00 && pData[k + 1] >= 0x20 && pData[k + 1] <= 0x7E) {
                            layerText.push_back(static_cast<wchar_t>(pData[k + 1]));
                            k += 2;
                        } else if ((pData[k] >= 0x4E && pData[k] <= 0x9F) ||
                                   (pData[k] >= 0x34 && pData[k] <= 0x4D)) {
                            const wchar_t zhChar = static_cast<wchar_t>((pData[k] << 8) | pData[k + 1]);
                            layerText.push_back(zhChar);
                            k += 2;
                        } else {
                            break;
                        }
                    }
                    if (layerText.size() >= 2) {
                        outExtractedText += layerText;
                        outExtractedText += L"\n";
                    }
                }
            }
            i += 4;
        } else {
            ++i;
        }
    }

    return !outExtractedText.empty();
}

bool PsdAiExtractor::searchContent(
    const std::wstring& filePath,
    std::wstring_view queryPattern,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile
) {
    return searchContentTerms(
        filePath,
        splitQueryPatternTerms(queryPattern),
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        {}
    );
}

bool PsdAiExtractor::searchContentTerms(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled
) {
    if (queryTerms.empty() || (isCancelled && isCancelled())) return false;

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    const FileCacheStamp stamp = queryFileCacheStampByHandle(hFile);
    if (!stamp.valid || stamp.fileSize == 0 || stamp.fileSize > MaxPsdAiFileBytes) {
        return false;
    }

    std::wstring extractedAllText;
    if (!ContentTextCache::instance().tryGet(filePath, stamp.fileSize, stamp.lastWriteTime, extractedAllText)) {
        const size_t mapSize = static_cast<size_t>(stamp.fileSize);
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) {
            return false;
        }
        HandleCloser mapCloser{hMapping};

        const auto* pData = static_cast<const uint8_t*>(
            MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
        if (!pData) {
            return false;
        }
        struct ViewGuard {
            const void* ptr;
            ~ViewGuard() {
                if (ptr) UnmapViewOfFile(ptr);
            }
        } viewGuard{pData};

        const bool guardOk = runWithMappedViewGuard([&]() -> bool {
            return extractPsdAiText(pData, mapSize, extractedAllText, isCancelled);
        });
        if (!guardOk) {
            extractedAllText.clear();
        }
        if (isCancelled && isCancelled()) {
            return false;
        }

        ContentTextCache::instance().put(filePath, stamp.fileSize, stamp.lastWriteTime, extractedAllText);
    }

    if (extractedAllText.empty()) {
        return false;
    }

    return matchDocumentTextAndBuildSnippets(
        extractedAllText,
        queryTerms,
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        isCancelled
    );
}

} // namespace tools3000::service::content
