#include "ZipXmlExtractor.h"
#include "ContentMatchUtils.h"
#include "ContentTextCache.h"
#include "WindowsIFilterExtractor.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxZipDocumentBytes = 100ULL * 1024ULL * 1024ULL;
constexpr size_t MaxSingleXmlUncompressedBytes = 32ULL * 1024ULL * 1024ULL;
constexpr size_t MaxDocUncompressedTotalBytes = 48ULL * 1024ULL * 1024ULL;
constexpr size_t MaxDocExtractedChars = 15ULL * 1024ULL * 1024ULL;

#pragma pack(push, 1)
struct ZipEocd {
    uint32_t signature;           // 0x06054b50
    uint16_t diskNumber;
    uint16_t cdDiskNumber;
    uint16_t numEntriesThisDisk;
    uint16_t totalEntries;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLength;
};

struct ZipCdHeader {
    uint32_t signature;           // 0x02014b50
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;   // 0 = stored, 8 = deflated
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
    uint16_t commentLength;
    uint16_t diskNumberStart;
    uint16_t internalFileAttr;
    uint32_t externalFileAttr;
    uint32_t localHeaderOffset;
};

struct ZipLocalHeader {
    uint32_t signature;           // 0x04034b50
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;   // 0 = stored, 8 = deflated
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
};
#pragma pack(pop)

void appendUnicodeCodepointAsUtf8(uint32_t cp, std::string& out) {
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return;
    }
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

bool decodeXmlEntity(std::string_view entity, std::string& out) {
    if (entity == "lt") {
        out.push_back('<');
        return true;
    }
    if (entity == "gt") {
        out.push_back('>');
        return true;
    }
    if (entity == "amp") {
        out.push_back('&');
        return true;
    }
    if (entity == "quot") {
        out.push_back('"');
        return true;
    }
    if (entity == "apos") {
        out.push_back('\'');
        return true;
    }
    if (entity == "nbsp") {
        out.push_back(' ');
        return true;
    }
    if (entity.size() >= 2 && entity[0] == '#') {
        uint32_t cp = 0;
        if (entity.size() >= 3 && (entity[1] == 'x' || entity[1] == 'X')) {
            for (size_t i = 2; i < entity.size(); ++i) {
                const char c = entity[i];
                uint32_t nib = 0;
                if (c >= '0' && c <= '9') nib = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') nib = static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nib = static_cast<uint32_t>(c - 'A' + 10);
                else return false;
                cp = (cp << 4) | nib;
                if (cp > 0x10FFFFu) return false;
            }
        } else {
            for (size_t i = 1; i < entity.size(); ++i) {
                const char c = entity[i];
                if (c < '0' || c > '9') return false;
                cp = cp * 10u + static_cast<uint32_t>(c - '0');
                if (cp > 0x10FFFFu) return false;
            }
        }
        if (cp == 0xA0u) cp = 0x20u;
        appendUnicodeCodepointAsUtf8(cp, out);
        return true;
    }
    return false;
}

bool isBlockNewlineTag(std::string_view tagName) noexcept {
    if (tagName.empty()) return false;
    // Strip trailing '/' from self-closing tags such as "br/" or "w:br/"
    if (tagName.back() == '/') {
        tagName.remove_suffix(1);
    }
    return tagName == "/si" ||
           tagName == "/row" ||
           tagName == "/p" ||
           tagName == "/w:p" ||
           tagName == "/a:p" ||
           tagName == "/text:p" ||
           tagName == "/p:sp" ||
           tagName == "/tr" ||
           tagName == "/w:tr" ||
           tagName == "/table:table-row" ||
           tagName == "br" ||
           tagName == "/br" ||
           tagName == "w:br" ||
           tagName == "a:br" ||
           tagName == "w:cr" ||
           tagName == "/div" ||
           tagName == "/li" ||
           tagName == "/h1" ||
           tagName == "/h2" ||
           tagName == "/h3" ||
           tagName == "/h4" ||
           tagName == "/h5" ||
           tagName == "/h6";
}

bool isCellSeparatorTag(std::string_view tagName) noexcept {
    if (tagName.empty()) return false;
    if (tagName.back() == '/') {
        tagName.remove_suffix(1);
    }
    // Note: "/w:t" and "/a:t" are inline run containers in Word/PowerPoint and MUST NOT insert spaces
    // so split CJK runs (<w:t>合</w:t><w:t>同</w:t>) remain seamless ("合同").
    // Exact "/t", "/c", "/tc", "/w:tc", "/a:tc", "/table:table-cell" separate Excel/table cells.
    return tagName == "/c" ||
           tagName == "/t" ||
           tagName == "/tc" ||
           tagName == "/w:tc" ||
           tagName == "/a:tc" ||
           tagName == "/table:table-cell" ||
           tagName == "w:tab" ||
           tagName == "a:tab";
}

std::wstring stripXmlTags(const std::string& xml) {
    std::string currentText;
    currentText.reserve(xml.size() / 2);

    size_t i = 0;
    while (i < xml.size()) {
        if (xml[i] == '<') {
            // Handle XML comments <!-- ... -->
            if (i + 3 < xml.size() && xml.compare(i, 4, "<!--") == 0) {
                const size_t closeComment = xml.find("-->", i + 4);
                i = (closeComment != std::string::npos) ? (closeComment + 3) : xml.size();
                continue;
            }
            // Handle CDATA sections <![CDATA[ ... ]]>
            if (i + 8 < xml.size() && xml.compare(i, 9, "<![CDATA[") == 0) {
                const size_t closeCdata = xml.find("]]>", i + 9);
                if (closeCdata != std::string::npos) {
                    currentText.append(xml, i + 9, closeCdata - (i + 9));
                    i = closeCdata + 3;
                } else {
                    currentText.append(xml, i + 9, xml.size() - (i + 9));
                    i = xml.size();
                }
                continue;
            }

            const size_t closeTag = xml.find('>', i + 1);
            if (closeTag == std::string::npos) break;

            // Extract normalized tag name up to first whitespace (preserving leading '/' and trailing '/')
            std::string tagName;
            tagName.reserve(24);
            size_t t = i + 1;
            while (t < closeTag && std::isspace(static_cast<unsigned char>(xml[t]))) ++t;
            while (t < closeTag && !std::isspace(static_cast<unsigned char>(xml[t]))) {
                tagName.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(xml[t]))));
                ++t;
            }
            if (closeTag > i + 1 && xml[closeTag - 1] == '/' && (tagName.empty() || tagName.back() != '/')) {
                tagName.push_back('/');
            }

            // Skip <script>...</script> and <style>...</style> blocks in EPUB XHTML
            if (tagName == "script" || tagName == "style") {
                const std::string endTag = (tagName == "script") ? "</script" : "</style";
                size_t searchEnd = closeTag + 1;
                while (searchEnd < xml.size()) {
                    const size_t lt = xml.find('<', searchEnd);
                    if (lt == std::string::npos) {
                        searchEnd = xml.size();
                        break;
                    }
                    if (lt + endTag.size() <= xml.size()) {
                        bool match = true;
                        for (size_t k = 0; k < endTag.size(); ++k) {
                            if (std::tolower(static_cast<unsigned char>(xml[lt + k])) != endTag[k]) {
                                match = false;
                                break;
                            }
                        }
                        if (match) {
                            const size_t gt = xml.find('>', lt + endTag.size());
                            searchEnd = (gt != std::string::npos) ? (gt + 1) : xml.size();
                            break;
                        }
                    }
                    searchEnd = lt + 1;
                }
                i = searchEnd;
                continue;
            }

            if (isBlockNewlineTag(tagName)) {
                while (!currentText.empty() && (currentText.back() == ' ' || currentText.back() == '\t')) {
                    currentText.pop_back();
                }
                if (!currentText.empty() && currentText.back() != '\n') {
                    currentText.push_back('\n');
                }
            } else if (isCellSeparatorTag(tagName)) {
                if (!currentText.empty() &&
                    currentText.back() != '\n' &&
                    currentText.back() != '\t' &&
                    currentText.back() != ' ') {
                    currentText.push_back('\t');
                }
            }

            i = closeTag + 1;
            continue;
        }

        if (xml[i] == '&') {
            const size_t semi = xml.find(';', i + 1);
            if (semi != std::string::npos && (semi - (i + 1)) <= 12) {
                std::string_view ent(xml.data() + i + 1, semi - (i + 1));
                if (decodeXmlEntity(ent, currentText)) {
                    i = semi + 1;
                    continue;
                }
            }
        }

        currentText.push_back(xml[i]);
        ++i;
    }

    while (!currentText.empty() && (currentText.back() == ' ' || currentText.back() == '\t')) {
        currentText.pop_back();
    }
    if (currentText.empty()) return {};

    const int wideLen = MultiByteToWideChar(
        CP_UTF8, 0, currentText.data(), static_cast<int>(currentText.size()), nullptr, 0);
    if (wideLen <= 0) return {};
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, currentText.data(), static_cast<int>(currentText.size()), result.data(), wideLen);
    return result;
}

bool decompressRawDeflate(
    const uint8_t* compressed,
    size_t compSize,
    size_t uncompSize,
    std::string& out,
    size_t maxAllowed,
    const CancellationCheck& isCancelled
) {
    if (compSize == 0 || maxAllowed == 0) return false;

    const size_t targetCap = (std::min)(MaxSingleXmlUncompressedBytes, maxAllowed);
    size_t initialCap = uncompSize > 0
        ? (std::min)(uncompSize, targetCap)
        : (std::min)((std::max<size_t>)(compSize * 6, 4096), targetCap);

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(compressed);
    strm.avail_in = static_cast<uInt>((std::min<size_t>)(compSize, 0x7FFFFFFFu));

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        out.clear();
        return false;
    }

    out.clear();
    out.reserve(initialCap);
    std::vector<char> chunk(32768);
    int ret = Z_OK;

    while (out.size() < targetCap) {
        if (isCancelled && isCancelled()) {
            inflateEnd(&strm);
            out.clear();
            return false;
        }
        const size_t remain = targetCap - out.size();
        const size_t ask = (std::min)(chunk.size(), remain);
        strm.next_out = reinterpret_cast<Bytef*>(chunk.data());
        strm.avail_out = static_cast<uInt>(ask);

        ret = inflate(&strm, Z_NO_FLUSH);
        const size_t got = ask - strm.avail_out;
        if (got > 0) {
            out.append(chunk.data(), got);
        }
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK) {
            if (out.empty()) {
                inflateEnd(&strm);
                return false;
            }
            break;
        }
        if (got == 0 && strm.avail_in == 0) {
            break;
        }
    }

    inflateEnd(&strm);
    return !out.empty();
}

bool isZipNoiseEntry(std::string_view lowerName) noexcept {
    if (lowerName.find("_rels/") != std::string_view::npos ||
        lowerName.ends_with(".rels") ||
        lowerName.find("docprops/") != std::string_view::npos ||
        lowerName.find("theme/") != std::string_view::npos ||
        lowerName.find("slidemasters/") != std::string_view::npos ||
        lowerName.find("slidelayouts/") != std::string_view::npos ||
        lowerName.find("notesmasters/") != std::string_view::npos ||
        lowerName.find("handoutmasters/") != std::string_view::npos ||
        lowerName.find("customxml/") != std::string_view::npos) {
        return true;
    }

    const size_t slashPos = lowerName.rfind('/');
    const std::string_view baseName = (slashPos != std::string_view::npos)
        ? lowerName.substr(slashPos + 1)
        : lowerName;

    return baseName == "styles.xml" ||
           baseName == "fonttable.xml" ||
           baseName == "numbering.xml" ||
           baseName == "settings.xml" ||
           baseName == "websettings.xml" ||
           baseName == "[content_types].xml" ||
           baseName == "calcchain.xml" ||
           baseName == "connections.xml" ||
           baseName == "presprops.xml" ||
           baseName == "viewprops.xml" ||
           baseName == "tablestyles.xml";
}

bool isMatchingZipEntry(std::string_view entryName, std::wstring_view lowerExt) {
    std::string lowerName;
    lowerName.reserve(entryName.size());
    for (char c : entryName) {
        lowerName.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (isZipNoiseEntry(lowerName)) {
        return false;
    }

    if (lowerExt == L"docx" || lowerExt == L"wps" || lowerExt == L"dotx" || lowerExt == L"docm") {
        if (lowerName.find("word/") != std::string::npos || lowerName.find("wps/") != std::string::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"xlsx" || lowerExt == L"et" || lowerExt == L"xlsm" || lowerExt == L"xltx") {
        if (lowerName.find("xl/") != std::string::npos || lowerName.find("et/") != std::string::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"pptx" || lowerExt == L"dps" || lowerExt == L"pptm" || lowerExt == L"potx") {
        if (lowerName.find("ppt/") != std::string::npos || lowerName.find("dps/") != std::string::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"epub") {
        return lowerName.ends_with(".xhtml") ||
               lowerName.ends_with(".html") ||
               lowerName.ends_with(".htm") ||
               lowerName.ends_with(".opf");
    } else if (lowerExt == L"cdr") {
        return lowerName.find("content/") != std::string::npos || lowerName == "metadata.xml";
    } else if (lowerExt == L"xmind") {
        return lowerName == "content.json" || lowerName == "content.xml" || lowerName == "comments.xml";
    } else if (lowerExt == L"odt" || lowerExt == L"ods" || lowerExt == L"odp") {
        return lowerName == "content.xml" || lowerName == "meta.xml";
    }

    return false;
}

bool hasOle2CompoundHeader(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 8) return false;
    uint8_t header[8]{};
    if (!safeMappedMemcpy(header, data, sizeof(header))) return false;
    static constexpr uint8_t kOle2[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    return std::memcmp(header, kOle2, sizeof(kOle2)) == 0;
}

} // namespace

ZipXmlExtractor::ZipXmlExtractor() {
    const wchar_t* exts[] = {
        L"docx", L"dotx", L"docm", L"wps",
        L"xlsx", L"xltx", L"xlsm", L"et",
        L"pptx", L"potx", L"pptm", L"dps",
        L"epub", L"cdr", L"xmind", L"odt", L"ods", L"odp"
    };
    for (const auto* ext : exts) {
        m_supportedExts.insert(ext);
    }
}

bool ZipXmlExtractor::canHandle(std::wstring_view extension) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    if (lowerExt.empty()) return false;
    return m_supportedExts.find(lowerExt) != m_supportedExts.end();
}

bool ZipXmlExtractor::extractZipEntriesText(
    const uint8_t* pZipData,
    size_t zipSize,
    std::wstring_view extension,
    std::wstring& outExtractedText,
    const CancellationCheck& isCancelled
) const {
    if (!pZipData || zipSize < sizeof(ZipLocalHeader)) return false;

    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    size_t totalUncompressedBytes = 0;

    auto appendDecodedEntry = [&](const std::string& uncompData) -> bool {
        if (uncompData.empty()) return true;
        totalUncompressedBytes += uncompData.size();
        const std::wstring plain = stripXmlTags(uncompData);
        if (!plain.empty()) {
            if (outExtractedText.size() + plain.size() + 1 > MaxDocExtractedChars) {
                const size_t allowedChars = MaxDocExtractedChars - outExtractedText.size();
                if (allowedChars > 0) {
                    outExtractedText.append(plain.data(), (std::min)(plain.size(), allowedChars));
                }
                return false;
            }
            outExtractedText += plain;
            if (outExtractedText.back() != L'\n') {
                outExtractedText.push_back(L'\n');
            }
        }
        return true;
    };

    // 1. Primary path: Central Directory traversal (handles streaming Data Descriptor entries accurately)
    if (zipSize >= sizeof(ZipEocd)) {
        ZipEocd validEocd{};
        bool foundValidEocd = false;
        const size_t maxSearch = (std::min)(zipSize, static_cast<size_t>(65535 + sizeof(ZipEocd)));
        const size_t searchStart = zipSize - maxSearch;

        for (size_t i = zipSize - sizeof(ZipEocd);; --i) {
            if (pZipData[i] == 0x50u && pZipData[i + 1] == 0x4Bu &&
                pZipData[i + 2] == 0x05u && pZipData[i + 3] == 0x06u) {
                ZipEocd cand{};
                std::memcpy(&cand, pZipData + i, sizeof(ZipEocd));
                const uint64_t endWithComment =
                    static_cast<uint64_t>(i) + sizeof(ZipEocd) + static_cast<uint64_t>(cand.commentLength);
                const uint64_t cdEnd64 =
                    static_cast<uint64_t>(cand.cdOffset) + static_cast<uint64_t>(cand.cdSize);
                if (endWithComment <= zipSize && cand.cdOffset < zipSize && cdEnd64 <= i) {
                    validEocd = cand;
                    foundValidEocd = true;
                    break;
                }
            }
            if (i <= searchStart) break;
        }

        if (foundValidEocd) {
            size_t cdOffset = validEocd.cdOffset;
            const size_t cdEnd = static_cast<size_t>(validEocd.cdOffset) + static_cast<size_t>(validEocd.cdSize);
            while (cdOffset + sizeof(ZipCdHeader) <= cdEnd && cdOffset + sizeof(ZipCdHeader) <= zipSize) {
                if (isCancelled && isCancelled()) {
                    outExtractedText.clear();
                    return false;
                }

                ZipCdHeader cdHeader{};
                std::memcpy(&cdHeader, pZipData + cdOffset, sizeof(ZipCdHeader));
                if (cdHeader.signature != 0x02014b50u) break;

                const size_t nameOffset = cdOffset + sizeof(ZipCdHeader);
                if (nameOffset + cdHeader.filenameLength > zipSize) break;

                std::string filename(reinterpret_cast<const char*>(pZipData + nameOffset), cdHeader.filenameLength);
                const size_t entryAdvance = sizeof(ZipCdHeader) +
                                            static_cast<size_t>(cdHeader.filenameLength) +
                                            static_cast<size_t>(cdHeader.extraFieldLength) +
                                            static_cast<size_t>(cdHeader.commentLength);
                if (entryAdvance == 0 || cdOffset + entryAdvance > zipSize) break;
                cdOffset += entryAdvance;

                if (!isMatchingZipEntry(filename, lowerExt)) continue;

                if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                    outExtractedText.size() >= MaxDocExtractedChars) {
                    break;
                }
                const size_t remainingBudget = MaxDocUncompressedTotalBytes - totalUncompressedBytes;

                if (static_cast<uint64_t>(cdHeader.localHeaderOffset) + sizeof(ZipLocalHeader) > zipSize) continue;
                ZipLocalHeader localHeader{};
                std::memcpy(&localHeader, pZipData + cdHeader.localHeaderOffset, sizeof(ZipLocalHeader));
                if (localHeader.signature != 0x04034b50u) continue;

                const uint64_t dataOffset64 =
                    static_cast<uint64_t>(cdHeader.localHeaderOffset) + sizeof(ZipLocalHeader) +
                    static_cast<uint64_t>(localHeader.filenameLength) +
                    static_cast<uint64_t>(localHeader.extraFieldLength);
                if (dataOffset64 + static_cast<uint64_t>(cdHeader.compressedSize) > zipSize) continue;
                const size_t dataOffset = static_cast<size_t>(dataOffset64);

                std::string uncompData;
                if (cdHeader.compressionMethod == 0) {
                    const size_t takeSize = (std::min)(static_cast<size_t>(cdHeader.compressedSize), remainingBudget);
                    uncompData.assign(reinterpret_cast<const char*>(pZipData + dataOffset), takeSize);
                } else if (cdHeader.compressionMethod == 8) {
                    decompressRawDeflate(
                        pZipData + dataOffset,
                        cdHeader.compressedSize,
                        cdHeader.uncompressedSize,
                        uncompData,
                        remainingBudget,
                        isCancelled
                    );
                }

                if (!appendDecodedEntry(uncompData)) {
                    break;
                }
            }
            if (!outExtractedText.empty()) return true;
        }
    }

    // 2. Fallback: Local Header scan for truncated or non-standard ZIP archives
    size_t offset = 0;
    while (offset + sizeof(ZipLocalHeader) <= zipSize) {
        if (isCancelled && isCancelled()) {
            outExtractedText.clear();
            return false;
        }

        ZipLocalHeader header{};
        std::memcpy(&header, pZipData + offset, sizeof(ZipLocalHeader));
        if (header.signature != 0x04034b50u) break;

        const size_t nameOffset = offset + sizeof(ZipLocalHeader);
        if (nameOffset + header.filenameLength > zipSize) break;

        std::string filename(reinterpret_cast<const char*>(pZipData + nameOffset), header.filenameLength);
        const size_t dataOffset = nameOffset +
                                  static_cast<size_t>(header.filenameLength) +
                                  static_cast<size_t>(header.extraFieldLength);
        if (dataOffset > zipSize) break;

        const bool hasStreamingDataDescriptor =
            ((header.bitFlag & 0x08u) != 0) && (header.compressedSize == 0);
        if (!hasStreamingDataDescriptor &&
            dataOffset + static_cast<size_t>(header.compressedSize) > zipSize) {
            break;
        }

        if (isMatchingZipEntry(filename, lowerExt)) {
            if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                outExtractedText.size() >= MaxDocExtractedChars) {
                break;
            }
            const size_t remainingBudget = MaxDocUncompressedTotalBytes - totalUncompressedBytes;

            std::string uncompData;
            if (header.compressionMethod == 0 && header.compressedSize > 0) {
                const size_t takeSize = (std::min)(static_cast<size_t>(header.compressedSize), remainingBudget);
                uncompData.assign(reinterpret_cast<const char*>(pZipData + dataOffset), takeSize);
            } else if (header.compressionMethod == 8) {
                const size_t availComp = hasStreamingDataDescriptor
                    ? (zipSize - dataOffset)
                    : static_cast<size_t>(header.compressedSize);
                if (availComp > 0) {
                    decompressRawDeflate(
                        pZipData + dataOffset,
                        availComp,
                        header.uncompressedSize,
                        uncompData,
                        remainingBudget,
                        isCancelled
                    );
                }
            }

            if (!appendDecodedEntry(uncompData)) {
                break;
            }
        }

        if (hasStreamingDataDescriptor) {
            // Advance to next PK\x03\x04 Local File Header when compressedSize was deferred to Data Descriptor
            size_t nextSig = zipSize;
            for (size_t scan = dataOffset; scan + sizeof(ZipLocalHeader) <= zipSize; ++scan) {
                if (((scan - dataOffset) & 0x3FFFFu) == 0 && isCancelled && isCancelled()) {
                    outExtractedText.clear();
                    return false;
                }
                if (pZipData[scan] == 0x50u && pZipData[scan + 1] == 0x4Bu &&
                    pZipData[scan + 2] == 0x03u && pZipData[scan + 3] == 0x04u) {
                    nextSig = scan;
                    break;
                }
            }
            if (nextSig >= zipSize) break;
            offset = nextSig;
        } else {
            // 0-byte directory entries (e.g. "META-INF/", "word/") legitimately have compressedSize == 0
            // with bit 3 clear; their next header starts directly at dataOffset.
            offset = dataOffset + static_cast<size_t>(header.compressedSize);
        }
    }

    return !outExtractedText.empty();
}

bool ZipXmlExtractor::searchContent(
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

bool ZipXmlExtractor::searchContentTerms(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled
) {
    if (queryTerms.empty() || (isCancelled && isCancelled())) {
        return false;
    }

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

    const FileCacheStamp stamp = queryFileCacheStampByHandle(hFile);
    if (!stamp.valid || stamp.fileSize == 0 || stamp.fileSize > MaxZipDocumentBytes) {
        return false;
    }

    std::wstring extractedAllText;
    bool cacheTruncated = false;
    bool cacheHit = ContentTextCache::instance().tryGetWithTruncation(
        filePath, stamp.fileSize, stamp.lastWriteTime, extractedAllText, cacheTruncated);
    if (cacheHit && cacheTruncated) {
        if (matchDocumentTextAndBuildSnippets(
                extractedAllText, queryTerms, caseSensitive, outSnippets, maxSnippetsPerFile, isCancelled)) {
            return true;
        }
        cacheHit = false;
        extractedAllText.clear();
    }

    if (!cacheHit) {
        const size_t mapSize = static_cast<size_t>(stamp.fileSize);
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) return false;
        HandleCloser mapCloser{hMapping};

        const auto* pData = static_cast<const uint8_t*>(
            MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
        if (!pData) return false;
        struct ViewGuard {
            const void* ptr;
            ~ViewGuard() {
                if (ptr) UnmapViewOfFile(ptr);
            }
        } viewGuard{pData};

        std::wstring ext;
        const size_t dotPos = filePath.rfind(L'.');
        if (dotPos != std::wstring::npos) {
            ext = ContentFormatFilter::normalizeExt(std::wstring_view(filePath).substr(dotPos + 1));
        }

        // Legacy WPS/ET/DPS documents often use OLE2 Compound Binary format instead of ZIP
        if (hasOle2CompoundHeader(pData, mapSize)) {
            bool hasText = WindowsIFilterExtractor::extractViaSystemIFilterFromHandle(
                hFile, filePath, pData, mapSize, extractedAllText, isCancelled);
            if (!hasText && !(isCancelled && isCancelled())) {
                WindowsIFilterExtractor::extractViaBinaryTextScan(pData, mapSize, extractedAllText, isCancelled);
            }
        } else {
            std::vector<uint8_t> residentZipBytes;
            bool inPageError = false;
            if (!copyMappedViewSafe(pData, mapSize, residentZipBytes, inPageError, isCancelled) || inPageError) {
                return false;
            }
            if (!extractZipEntriesText(
                    residentZipBytes.data(), residentZipBytes.size(), ext, extractedAllText, isCancelled)) {
                extractedAllText.clear();
            }
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
