#include "DxfExtractor.h"
#include "ContentMatchUtils.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxDxfFileBytes = 100ULL * 1024ULL * 1024ULL;

std::wstring decodeDxfLine(const char* data, size_t length) {
    if (length == 0) return {};
    int wideLen = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0);
    const UINT fallbackCp = IsValidCodePage(936) ? 936u : CP_ACP;
    const UINT codePage = (wideLen > 0) ? CP_UTF8 : fallbackCp;
    if (wideLen <= 0) {
        wideLen = MultiByteToWideChar(codePage, 0, data, static_cast<int>(length), nullptr, 0);
    }
    if (wideLen <= 0) return {};
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(codePage, 0, data, static_cast<int>(length), result.data(), wideLen);
    return result;
}

} // namespace

DxfExtractor::DxfExtractor() {
    m_supportedExts.insert(L"dxf");
}

bool DxfExtractor::canHandle(std::wstring_view extension) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    if (lowerExt.empty()) return false;
    return m_supportedExts.find(lowerExt) != m_supportedExts.end();
}

bool DxfExtractor::searchContent(
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

bool DxfExtractor::searchContentTerms(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled
) {
    MultiTermDocumentMatcher matcher(queryTerms, caseSensitive, maxSnippetsPerFile);
    if (!matcher.hasValidTerms() || (isCancelled && isCancelled())) return false;

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

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > MaxDxfFileBytes) {
        return false;
    }

    const size_t mapSize = static_cast<size_t>(fileSize.QuadPart);
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

    const bool scanOk = runWithMappedViewGuard([&]() -> bool {
        size_t lineStart = 0;
        uint32_t lineNumber = 1;
        bool isNextLineTextValue = false;

        for (size_t i = 0; i < mapSize; ++i) {
            if (pData[i] == '\n' || i == mapSize - 1) {
                if (((lineNumber & 0x3FFu) == 0) && isCancelled && isCancelled()) {
                    return false;
                }

                const size_t lineEnd = (pData[i] == '\n') ? i : (i + 1);
                const size_t lineLen = lineEnd - lineStart;

                std::wstring line = decodeDxfLine(reinterpret_cast<const char*>(pData + lineStart), lineLen);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n' || line.back() == L' ')) {
                    line.pop_back();
                }

                if (isNextLineTextValue) {
                    isNextLineTextValue = false;
                    matcher.processLine(lineNumber, line);
                    if (matcher.canStopEarly()) {
                        break;
                    }
                } else {
                    size_t firstNonSpace = 0;
                    while (firstNonSpace < line.size() && line[firstNonSpace] == L' ') {
                        ++firstNonSpace;
                    }
                    std::wstring_view trimmed = std::wstring_view(line).substr(firstNonSpace);
                    if (trimmed == L"1" || trimmed == L"3") {
                        isNextLineTextValue = true;
                    }
                }

                lineStart = i + 1;
                ++lineNumber;
            }
        }
        return true;
    });

    if (!scanOk) {
        outSnippets.clear();
        return false;
    }

    return matcher.finalize(outSnippets);
}

} // namespace tools3000::service::content
