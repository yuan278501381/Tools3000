#include "DxfExtractor.h"
#include "ContentMatchUtils.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <climits>
#include <cwctype>
#include <string>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxDxfFileBytes = 100ULL * 1024ULL * 1024ULL;
constexpr size_t DxfStreamChunkBytes = 4ULL * 1024ULL * 1024ULL;
constexpr size_t MaxDxfSingleLineBytes = 1024ULL * 1024ULL;

std::wstring decodeDxfLine(const char* data, size_t length) {
    if (!data || length == 0) return {};
    const int cappedLen = static_cast<int>((std::min<size_t>)(length, static_cast<size_t>(INT_MAX)));
    int wideLen = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data, cappedLen, nullptr, 0);
    const UINT fallbackCp = IsValidCodePage(936) ? 936u : CP_ACP;
    const UINT codePage = (wideLen > 0) ? CP_UTF8 : fallbackCp;
    if (wideLen <= 0) {
        wideLen = MultiByteToWideChar(codePage, 0, data, cappedLen, nullptr, 0);
    }
    if (wideLen <= 0) return {};
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(codePage, 0, data, cappedLen, result.data(), wideLen);
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
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (GetFileType(hFile) != FILE_TYPE_DISK || !GetFileInformationByHandle(hFile, &fileInfo) ||
        (fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }

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

    // Stream in 4 MB resident heap chunks copied under pure-POD SEH protection so C++ string/matcher
    // objects are never live inside a `__try` frame under `/EHsc`.
    std::vector<char> chunkBuf((std::min)(mapSize, DxfStreamChunkBytes));
    std::string carryLine;
    uint32_t lineNumber = 1;
    bool isNextLineTextValue = false;
    bool stopEarly = false;

    auto handleOneLine = [&](const char* linePtr, size_t lineLen) {
        std::wstring line = decodeDxfLine(linePtr, lineLen);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n' || line.back() == L' ')) {
            line.pop_back();
        }
        if (isNextLineTextValue) {
            isNextLineTextValue = false;
            matcher.processLine(lineNumber, line);
            if (matcher.canStopEarly()) {
                stopEarly = true;
            }
        } else {
            size_t firstNonSpace = 0;
            while (firstNonSpace < line.size() && line[firstNonSpace] == L' ') {
                ++firstNonSpace;
            }
            const std::wstring_view trimmed = std::wstring_view(line).substr(firstNonSpace);
            if (trimmed == L"1" || trimmed == L"3") {
                isNextLineTextValue = true;
            }
        }
        ++lineNumber;
    };

    size_t offset = 0;
    while (offset < mapSize && !stopEarly) {
        if (isCancelled && isCancelled()) {
            outSnippets.clear();
            return false;
        }
        const size_t chunkLen = (std::min)(mapSize - offset, chunkBuf.size());
        bool inPageError = false;
        if (!safeMappedMemcpyEx(chunkBuf.data(), pData + offset, chunkLen, &inPageError) || inPageError) {
            outSnippets.clear();
            return false;
        }

        size_t lineStart = 0;
        for (size_t i = 0; i < chunkLen; ++i) {
            if (((i & 0x3FFFFu) == 0) && isCancelled && isCancelled()) {
                outSnippets.clear();
                return false;
            }
            if (chunkBuf[i] == '\n') {
                const size_t segLen = i - lineStart;
                if (carryLine.empty()) {
                    handleOneLine(chunkBuf.data() + lineStart, segLen);
                } else {
                    if (carryLine.size() < MaxDxfSingleLineBytes) {
                        const size_t appendLen = (std::min)(segLen, MaxDxfSingleLineBytes - carryLine.size());
                        carryLine.append(chunkBuf.data() + lineStart, appendLen);
                    }
                    handleOneLine(carryLine.data(), carryLine.size());
                    carryLine.clear();
                }
                lineStart = i + 1;
                if (stopEarly) {
                    break;
                }
            }
        }

        if (!stopEarly && lineStart < chunkLen) {
            const size_t rem = chunkLen - lineStart;
            if (carryLine.size() < MaxDxfSingleLineBytes) {
                const size_t appendLen = (std::min)(rem, MaxDxfSingleLineBytes - carryLine.size());
                carryLine.append(chunkBuf.data() + lineStart, appendLen);
            }
        }
        offset += chunkLen;
    }

    if (!stopEarly && !carryLine.empty()) {
        handleOneLine(carryLine.data(), carryLine.size());
        carryLine.clear();
    }

    return matcher.finalize(outSnippets);
}

} // namespace tools3000::service::content
