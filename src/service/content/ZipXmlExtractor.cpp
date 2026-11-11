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
constexpr size_t MaxCentralDirectoryBytes = 16ULL * 1024ULL * 1024ULL;
constexpr size_t MaxSingleXmlUncompressedBytes = 32ULL * 1024ULL * 1024ULL;
constexpr size_t MaxDocUncompressedTotalBytes = 48ULL * 1024ULL * 1024ULL;
constexpr size_t MaxDocExtractedChars = 15ULL * 1024ULL * 1024ULL;
constexpr size_t MappedStreamChunkBytes = 64ULL * 1024ULL;

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

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int wideLen = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLen <= 0) return {};
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), wideLen);
    return result;
}

void appendXmlDecodedText(std::string_view raw, std::string& out) {
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '&') {
            const size_t semi = raw.find(';', i + 1);
            if (semi != std::string_view::npos && (semi - (i + 1)) <= 12) {
                if (decodeXmlEntity(raw.substr(i + 1, semi - (i + 1)), out)) {
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(raw[i]);
        ++i;
    }
}

struct XmlTagInfo {
    std::string localLower;
    bool isClosing = false;
    bool isSelfClosing = false;
    size_t attrStart = 0;
    size_t closeBracket = std::string_view::npos;
};

bool parseXmlTagAt(std::string_view xml, size_t ltPos, XmlTagInfo& outTag) {
    if (ltPos >= xml.size() || xml[ltPos] != '<') return false;
    const size_t closeTag = xml.find('>', ltPos + 1);
    if (closeTag == std::string_view::npos) return false;

    size_t p = ltPos + 1;
    while (p < closeTag && std::isspace(static_cast<unsigned char>(xml[p]))) ++p;
    if (p >= closeTag || xml[p] == '!' || xml[p] == '?') return false;

    outTag = XmlTagInfo{};
    outTag.closeBracket = closeTag;
    if (xml[p] == '/') {
        outTag.isClosing = true;
        ++p;
        while (p < closeTag && std::isspace(static_cast<unsigned char>(xml[p]))) ++p;
    }

    size_t nameEnd = p;
    while (nameEnd < closeTag &&
           !std::isspace(static_cast<unsigned char>(xml[nameEnd])) &&
           xml[nameEnd] != '/' && xml[nameEnd] != '>') {
        ++nameEnd;
    }
    if (nameEnd == p) return false;

    std::string_view fullName = xml.substr(p, nameEnd - p);
    const size_t colonPos = fullName.rfind(':');
    std::string_view localPart = (colonPos != std::string_view::npos)
        ? fullName.substr(colonPos + 1)
        : fullName;

    outTag.localLower.reserve(localPart.size());
    for (char c : localPart) {
        outTag.localLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    outTag.attrStart = nameEnd;

    size_t back = closeTag;
    while (back > nameEnd && std::isspace(static_cast<unsigned char>(xml[back - 1]))) {
        --back;
    }
    if (!outTag.isClosing && back > nameEnd && xml[back - 1] == '/') {
        outTag.isSelfClosing = true;
    }
    return true;
}

std::string_view extractXmlAttrValue(std::string_view xml, size_t attrStart, size_t closeBracket, std::string_view targetAttrLower) {
    size_t p = attrStart;
    while (p < closeBracket) {
        while (p < closeBracket && (std::isspace(static_cast<unsigned char>(xml[p])) || xml[p] == '/')) ++p;
        if (p >= closeBracket) break;

        size_t nameStart = p;
        while (p < closeBracket &&
               !std::isspace(static_cast<unsigned char>(xml[p])) &&
               xml[p] != '=' && xml[p] != '/' && xml[p] != '>') {
            ++p;
        }
        std::string_view rawAttr = xml.substr(nameStart, p - nameStart);
        const size_t colon = rawAttr.rfind(':');
        if (colon != std::string_view::npos) {
            rawAttr = rawAttr.substr(colon + 1);
        }

        while (p < closeBracket && std::isspace(static_cast<unsigned char>(xml[p]))) ++p;
        if (p >= closeBracket || xml[p] != '=') continue;
        ++p;
        while (p < closeBracket && std::isspace(static_cast<unsigned char>(xml[p]))) ++p;
        if (p >= closeBracket) break;

        std::string_view val;
        if (xml[p] == '"' || xml[p] == '\'') {
            const char quote = xml[p++];
            const size_t endQuote = xml.find(quote, p);
            if (endQuote == std::string_view::npos || endQuote > closeBracket) break;
            val = xml.substr(p, endQuote - p);
            p = endQuote + 1;
        } else {
            const size_t valStart = p;
            while (p < closeBracket && !std::isspace(static_cast<unsigned char>(xml[p])) && xml[p] != '/') ++p;
            val = xml.substr(valStart, p - valStart);
        }

        if (rawAttr.size() == targetAttrLower.size()) {
            bool eq = true;
            for (size_t k = 0; k < rawAttr.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(rawAttr[k])) != targetAttrLower[k]) {
                    eq = false;
                    break;
                }
            }
            if (eq) return val;
        }
    }
    return {};
}

// Extract concatenated <t>...</t> runs within an XML fragment while skipping phonetic <rPh>...</rPh> blocks.
void extractSpreadsheetTextRuns(std::string_view fragment, std::string& outUtf8) {
    size_t pos = 0;
    bool inT = false;
    int rPhDepth = 0;

    while (pos < fragment.size()) {
        const size_t lt = fragment.find('<', pos);
        if (lt == std::string_view::npos) {
            if (inT && rPhDepth == 0) {
                appendXmlDecodedText(fragment.substr(pos), outUtf8);
            }
            break;
        }
        if (inT && rPhDepth == 0 && lt > pos) {
            appendXmlDecodedText(fragment.substr(pos, lt - pos), outUtf8);
        }

        if (lt + 8 < fragment.size() && fragment.substr(lt, 9) == "<![CDATA[") {
            const size_t closeCdata = fragment.find("]]>", lt + 9);
            if (closeCdata != std::string_view::npos) {
                if (inT && rPhDepth == 0) {
                    outUtf8.append(fragment.data() + lt + 9, closeCdata - (lt + 9));
                }
                pos = closeCdata + 3;
                continue;
            }
        }
        if (lt + 3 < fragment.size() && fragment.substr(lt, 4) == "<!--") {
            const size_t closeComment = fragment.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : fragment.size();
            continue;
        }

        XmlTagInfo tag{};
        if (!parseXmlTagAt(fragment, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (tag.localLower == "rph") {
            if (tag.isClosing) {
                if (rPhDepth > 0) --rPhDepth;
            } else if (!tag.isSelfClosing) {
                ++rPhDepth;
            }
        } else if (tag.localLower == "t") {
            if (tag.isClosing) {
                inT = false;
            } else if (!tag.isSelfClosing) {
                inT = true;
            }
        }

        pos = tag.closeBracket + 1;
    }
}

void parseXlsxSharedStrings(
    std::string_view xml,
    std::vector<std::wstring>& outSharedStrings,
    const CancellationCheck& isCancelled
) {
    size_t pos = 0;
    std::string itemUtf8;
    while (pos < xml.size()) {
        if ((outSharedStrings.size() & 0x3FFu) == 0 && isCancelled && isCancelled()) {
            return;
        }
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;

        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t closeComment = xml.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : xml.size();
            continue;
        }

        XmlTagInfo tag{};
        if (!parseXmlTagAt(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (!tag.isClosing && tag.localLower == "si") {
            if (tag.isSelfClosing) {
                outSharedStrings.emplace_back();
                pos = tag.closeBracket + 1;
                continue;
            }

            // Locate matching </si>
            size_t scan = tag.closeBracket + 1;
            size_t siCloseStart = xml.size();
            size_t siAfterClose = xml.size();
            while (scan < xml.size()) {
                const size_t innerLt = xml.find('<', scan);
                if (innerLt == std::string_view::npos) break;
                XmlTagInfo innerTag{};
                if (parseXmlTagAt(xml, innerLt, innerTag) &&
                    innerTag.isClosing && innerTag.localLower == "si") {
                    siCloseStart = innerLt;
                    siAfterClose = innerTag.closeBracket + 1;
                    break;
                }
                scan = (innerTag.closeBracket != std::string_view::npos)
                    ? (innerTag.closeBracket + 1)
                    : (innerLt + 1);
            }

            itemUtf8.clear();
            extractSpreadsheetTextRuns(
                xml.substr(tag.closeBracket + 1, siCloseStart - (tag.closeBracket + 1)),
                itemUtf8
            );
            outSharedStrings.push_back(utf8ToWide(itemUtf8));
            pos = siAfterClose;
            continue;
        }

        pos = tag.closeBracket + 1;
    }
}

void extractSpreadsheetCellValueText(
    std::string_view cellInnerXml,
    std::string_view cellType,
    const std::vector<std::wstring>& sharedStrings,
    std::vector<uint8_t>& referencedShared,
    std::wstring& outCellText
) {
    outCellText.clear();
    if (cellType == "inlineStr") {
        std::string utf8;
        extractSpreadsheetTextRuns(cellInnerXml, utf8);
        outCellText = utf8ToWide(utf8);
        return;
    }

    // Extract <v>...</v> while skipping <f>...</f> formula source expressions
    std::string vRawUtf8;
    size_t pos = 0;
    bool inV = false;
    int fDepth = 0;
    while (pos < cellInnerXml.size()) {
        const size_t lt = cellInnerXml.find('<', pos);
        if (lt == std::string_view::npos) {
            if (inV && fDepth == 0) {
                appendXmlDecodedText(cellInnerXml.substr(pos), vRawUtf8);
            }
            break;
        }
        if (inV && fDepth == 0 && lt > pos) {
            appendXmlDecodedText(cellInnerXml.substr(pos, lt - pos), vRawUtf8);
        }

        XmlTagInfo tag{};
        if (!parseXmlTagAt(cellInnerXml, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (tag.localLower == "f") {
            if (tag.isClosing) {
                if (fDepth > 0) --fDepth;
            } else if (!tag.isSelfClosing) {
                ++fDepth;
            }
        } else if (tag.localLower == "v") {
            if (tag.isClosing) {
                inV = false;
                break;
            } else if (!tag.isSelfClosing) {
                inV = true;
            }
        }
        pos = tag.closeBracket + 1;
    }

    if (cellType == "s") {
        // Shared string cell: resolve index against sharedStrings; NEVER emit the raw integer index!
        size_t sStart = 0;
        while (sStart < vRawUtf8.size() && std::isspace(static_cast<unsigned char>(vRawUtf8[sStart]))) ++sStart;
        size_t sEnd = vRawUtf8.size();
        while (sEnd > sStart && std::isspace(static_cast<unsigned char>(vRawUtf8[sEnd - 1]))) --sEnd;
        if (sStart >= sEnd) return;

        size_t idx = 0;
        for (size_t k = sStart; k < sEnd; ++k) {
            const char ch = vRawUtf8[k];
            if (ch < '0' || ch > '9') return;
            const size_t digit = static_cast<size_t>(ch - '0');
            if (idx > (SIZE_MAX - digit) / 10u) return;
            idx = idx * 10u + digit;
        }
        if (idx < sharedStrings.size()) {
            referencedShared[idx] = 1;
            outCellText = sharedStrings[idx];
        }
        return;
    }

    if (!vRawUtf8.empty()) {
        outCellText = utf8ToWide(vRawUtf8);
    } else {
        std::string fallbackT;
        extractSpreadsheetTextRuns(cellInnerXml, fallbackT);
        if (!fallbackT.empty()) {
            outCellText = utf8ToWide(fallbackT);
        }
    }
}

std::wstring extractXlsxWorksheetText(
    std::string_view xml,
    const std::vector<std::wstring>& sharedStrings,
    std::vector<uint8_t>& referencedShared,
    const CancellationCheck& isCancelled
) {
    std::wstring outSheet;
    outSheet.reserve((std::min<size_t>)(xml.size() / 3, 256 * 1024));

    size_t pos = 0;
    size_t rowCounter = 0;
    std::wstring rowBuffer;
    std::wstring cellText;

    while (pos < xml.size()) {
        if ((rowCounter & 0xFFu) == 0 && isCancelled && isCancelled()) {
            return {};
        }
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;

        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t closeComment = xml.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : xml.size();
            continue;
        }

        XmlTagInfo tag{};
        if (!parseXmlTagAt(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (!tag.isClosing && tag.localLower == "row") {
            ++rowCounter;
            if (tag.isSelfClosing) {
                pos = tag.closeBracket + 1;
                continue;
            }

            // Scan cells inside this <row>...</row>
            rowBuffer.clear();
            size_t scan = tag.closeBracket + 1;
            while (scan < xml.size()) {
                const size_t cellLt = xml.find('<', scan);
                if (cellLt == std::string_view::npos) {
                    scan = xml.size();
                    break;
                }
                XmlTagInfo innerTag{};
                if (!parseXmlTagAt(xml, cellLt, innerTag)) {
                    scan = cellLt + 1;
                    continue;
                }
                if (innerTag.isClosing && innerTag.localLower == "row") {
                    scan = innerTag.closeBracket + 1;
                    break;
                }
                if (!innerTag.isClosing && innerTag.localLower == "c") {
                    if (innerTag.isSelfClosing) {
                        scan = innerTag.closeBracket + 1;
                        continue;
                    }
                    const std::string_view cellType = extractXmlAttrValue(
                        xml, innerTag.attrStart, innerTag.closeBracket, "t");

                    // Find </c>
                    size_t cScan = innerTag.closeBracket + 1;
                    size_t cCloseStart = xml.size();
                    size_t cAfterClose = xml.size();
                    while (cScan < xml.size()) {
                        const size_t subLt = xml.find('<', cScan);
                        if (subLt == std::string_view::npos) break;
                        XmlTagInfo subTag{};
                        if (parseXmlTagAt(xml, subLt, subTag) &&
                            subTag.isClosing && subTag.localLower == "c") {
                            cCloseStart = subLt;
                            cAfterClose = subTag.closeBracket + 1;
                            break;
                        }
                        cScan = (subTag.closeBracket != std::string_view::npos)
                            ? (subTag.closeBracket + 1)
                            : (subLt + 1);
                    }

                    extractSpreadsheetCellValueText(
                        xml.substr(innerTag.closeBracket + 1, cCloseStart - (innerTag.closeBracket + 1)),
                        cellType,
                        sharedStrings,
                        referencedShared,
                        cellText
                    );

                    // Trim trailing horizontal spaces on cell
                    while (!cellText.empty() && (cellText.back() == L' ' || cellText.back() == L'\t')) {
                        cellText.pop_back();
                    }
                    if (!cellText.empty()) {
                        if (!rowBuffer.empty()) {
                            rowBuffer.push_back(L'\t');
                        }
                        rowBuffer += cellText;
                    }
                    scan = cAfterClose;
                    continue;
                }

                scan = innerTag.closeBracket + 1;
            }

            if (!rowBuffer.empty()) {
                outSheet += rowBuffer;
                if (outSheet.back() != L'\n') {
                    outSheet.push_back(L'\n');
                }
                if (outSheet.size() >= MaxDocExtractedChars) {
                    break;
                }
            }
            pos = scan;
            continue;
        }

        pos = tag.closeBracket + 1;
    }

    return outSheet;
}

bool copyMappedSliceToString(
    const uint8_t* mappedSrc,
    size_t byteCount,
    std::string& out,
    bool& outInPageError,
    const CancellationCheck& isCancelled
) {
    out.clear();
    outInPageError = false;
    if (byteCount == 0) return true;
    out.resize(byteCount);
    size_t copied = 0;
    while (copied < byteCount) {
        if (isCancelled && isCancelled()) {
            out.clear();
            return false;
        }
        const size_t chunk = (std::min)(MappedStreamChunkBytes, byteCount - copied);
        bool pageErr = false;
        if (!safeMappedMemcpyEx(out.data() + copied, mappedSrc + copied, chunk, &pageErr)) {
            outInPageError = pageErr;
            out.clear();
            return false;
        }
        copied += chunk;
    }
    return true;
}

// SEH-safe streaming raw deflate decompression directly from a memory-mapped region via 64KB chunks.
bool decompressMappedRawDeflate(
    const uint8_t* mappedCompressed,
    size_t compSize,
    size_t uncompSize,
    std::string& out,
    size_t maxAllowed,
    bool& outInPageError,
    const CancellationCheck& isCancelled,
    size_t* outConsumedCompressed = nullptr
) {
    outInPageError = false;
    if (outConsumedCompressed) *outConsumedCompressed = 0;
    if (!mappedCompressed || compSize == 0 || maxAllowed == 0) return false;

    const size_t targetCap = (std::min)(MaxSingleXmlUncompressedBytes, maxAllowed);
    size_t initialCap = uncompSize > 0
        ? (std::min)(uncompSize, targetCap)
        : (std::min)((std::max<size_t>)(compSize * 4, 4096), targetCap);

    z_stream strm{};
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        out.clear();
        return false;
    }

    out.clear();
    out.reserve(initialCap);
    std::vector<uint8_t> inChunk(MappedStreamChunkBytes);
    std::vector<char> outChunk(32768);
    size_t srcOffset = 0;
    int ret = Z_OK;

    while (out.size() < targetCap) {
        if (isCancelled && isCancelled()) {
            inflateEnd(&strm);
            out.clear();
            return false;
        }

        if (strm.avail_in == 0 && srcOffset < compSize) {
            const size_t toRead = (std::min)(inChunk.size(), compSize - srcOffset);
            bool pageErr = false;
            if (!safeMappedMemcpyEx(inChunk.data(), mappedCompressed + srcOffset, toRead, &pageErr)) {
                outInPageError = pageErr;
                inflateEnd(&strm);
                out.clear();
                return false;
            }
            srcOffset += toRead;
            strm.next_in = inChunk.data();
            strm.avail_in = static_cast<uInt>(toRead);
        }

        const size_t remain = targetCap - out.size();
        const size_t ask = (std::min)(outChunk.size(), remain);
        strm.next_out = reinterpret_cast<Bytef*>(outChunk.data());
        strm.avail_out = static_cast<uInt>(ask);

        ret = inflate(&strm, Z_NO_FLUSH);
        const size_t got = ask - strm.avail_out;
        if (got > 0) {
            out.append(outChunk.data(), got);
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
        if (got == 0 && strm.avail_in == 0 && srcOffset >= compSize) {
            break;
        }
    }

    if (outConsumedCompressed) {
        *outConsumedCompressed = srcOffset - static_cast<size_t>(strm.avail_in);
    }
    inflateEnd(&strm);
    return !out.empty();
}

std::string normalizeZipEntryPath(std::string_view entryName) {
    std::string lowerName;
    lowerName.reserve(entryName.size());
    for (char c : entryName) {
        lowerName.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!lowerName.empty() && lowerName.front() == '/') {
        lowerName.erase(lowerName.begin());
    }
    return lowerName;
}

bool isSpreadsheetExtension(std::wstring_view lowerExt) noexcept {
    return lowerExt == L"xlsx" || lowerExt == L"xlsm" || lowerExt == L"xltx" || lowerExt == L"et";
}

bool isSpreadsheetSharedStringsEntry(std::string_view lowerName) noexcept {
    return lowerName == "xl/sharedstrings.xml" ||
           lowerName == "et/sharedstrings.xml" ||
           lowerName.ends_with("/sharedstrings.xml");
}

bool isSpreadsheetWorksheetEntry(std::string_view lowerName) noexcept {
    return ((lowerName.find("xl/worksheets/") != std::string_view::npos) ||
            (lowerName.find("et/worksheets/") != std::string_view::npos)) &&
           lowerName.ends_with(".xml");
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

bool isMatchingNormalizedZipEntry(std::string_view lowerName, std::wstring_view lowerExt) noexcept {
    if (isZipNoiseEntry(lowerName)) {
        return false;
    }

    if (lowerExt == L"docx" || lowerExt == L"wps" || lowerExt == L"dotx" || lowerExt == L"docm") {
        if (lowerName.find("word/") != std::string_view::npos || lowerName.find("wps/") != std::string_view::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"xlsx" || lowerExt == L"et" || lowerExt == L"xlsm" || lowerExt == L"xltx") {
        if (lowerName.find("xl/") != std::string_view::npos || lowerName.find("et/") != std::string_view::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"pptx" || lowerExt == L"dps" || lowerExt == L"pptm" || lowerExt == L"potx") {
        if (lowerName.find("ppt/") != std::string_view::npos || lowerName.find("dps/") != std::string_view::npos) {
            return lowerName.ends_with(".xml");
        }
    } else if (lowerExt == L"epub") {
        return lowerName.ends_with(".xhtml") ||
               lowerName.ends_with(".html") ||
               lowerName.ends_with(".htm") ||
               lowerName.ends_with(".opf");
    } else if (lowerExt == L"cdr") {
        return lowerName.find("content/") != std::string_view::npos || lowerName == "metadata.xml";
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

struct ZipTargetEntry {
    std::string lowerName;
    uint32_t localHeaderOffset = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t compressionMethod = 0;
};

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
    const bool isSpreadsheet = isSpreadsheetExtension(lowerExt);
    size_t totalUncompressedBytes = 0;

    auto appendPlainTextBlock = [&](const std::wstring& plain) -> bool {
        if (plain.empty()) return true;
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
        return true;
    };

    auto appendGenericXmlEntry = [&](const std::string& uncompData) -> bool {
        if (uncompData.empty()) return true;
        return appendPlainTextBlock(stripXmlTags(uncompData));
    };

    auto readEntryPayloadByCd = [&](const ZipTargetEntry& entry, std::string& outXml, bool& outPageErr) -> bool {
        outXml.clear();
        outPageErr = false;
        if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
            outExtractedText.size() >= MaxDocExtractedChars) {
            return false;
        }
        const size_t remainingBudget = MaxDocUncompressedTotalBytes - totalUncompressedBytes;

        if (static_cast<uint64_t>(entry.localHeaderOffset) + sizeof(ZipLocalHeader) > zipSize) {
            return false;
        }
        ZipLocalHeader localHeader{};
        if (!safeMappedMemcpyEx(&localHeader, pZipData + entry.localHeaderOffset, sizeof(ZipLocalHeader), &outPageErr)) {
            return false;
        }
        if (localHeader.signature != 0x04034b50u) return false;

        const uint64_t dataOffset64 =
            static_cast<uint64_t>(entry.localHeaderOffset) + sizeof(ZipLocalHeader) +
            static_cast<uint64_t>(localHeader.filenameLength) +
            static_cast<uint64_t>(localHeader.extraFieldLength);
        if (dataOffset64 + static_cast<uint64_t>(entry.compressedSize) > zipSize) {
            return false;
        }
        const size_t dataOffset = static_cast<size_t>(dataOffset64);

        if (entry.compressionMethod == 0) {
            const size_t takeSize = (std::min)(static_cast<size_t>(entry.compressedSize), remainingBudget);
            if (!copyMappedSliceToString(pZipData + dataOffset, takeSize, outXml, outPageErr, isCancelled)) {
                return false;
            }
        } else if (entry.compressionMethod == 8) {
            if (!decompressMappedRawDeflate(
                    pZipData + dataOffset,
                    entry.compressedSize,
                    entry.uncompressedSize,
                    outXml,
                    remainingBudget,
                    outPageErr,
                    isCancelled)) {
                return false;
            }
        } else {
            return false;
        }

        totalUncompressedBytes += outXml.size();
        return !outXml.empty();
    };

    // 1. Primary path: SEH-safe Central Directory tail slice + per-entry streaming decompression
    if (zipSize >= sizeof(ZipEocd)) {
        const size_t tailSize = (std::min)(zipSize, static_cast<size_t>(65535 + sizeof(ZipEocd)));
        const size_t tailStart = zipSize - tailSize;
        std::vector<uint8_t> tailBuf(tailSize);
        bool inPageError = false;
        if (!safeMappedMemcpyEx(tailBuf.data(), pZipData + tailStart, tailSize, &inPageError) && inPageError) {
            outExtractedText.clear();
            return false;
        }

        ZipEocd validEocd{};
        bool foundValidEocd = false;
        for (size_t rel = tailSize - sizeof(ZipEocd);; --rel) {
            if (tailBuf[rel] == 0x50u && tailBuf[rel + 1] == 0x4Bu &&
                tailBuf[rel + 2] == 0x05u && tailBuf[rel + 3] == 0x06u) {
                ZipEocd cand{};
                std::memcpy(&cand, tailBuf.data() + rel, sizeof(ZipEocd));
                const size_t absPos = tailStart + rel;
                const uint64_t endWithComment =
                    static_cast<uint64_t>(absPos) + sizeof(ZipEocd) + static_cast<uint64_t>(cand.commentLength);
                const uint64_t cdEnd64 =
                    static_cast<uint64_t>(cand.cdOffset) + static_cast<uint64_t>(cand.cdSize);
                if (endWithComment <= zipSize && cand.cdOffset < zipSize &&
                    cdEnd64 <= absPos && cand.cdSize <= MaxCentralDirectoryBytes) {
                    validEocd = cand;
                    foundValidEocd = true;
                    break;
                }
            }
            if (rel == 0) break;
        }

        if (foundValidEocd && validEocd.cdSize > 0) {
            std::vector<uint8_t> cdBuf(validEocd.cdSize);
            if (!safeMappedMemcpyEx(cdBuf.data(), pZipData + validEocd.cdOffset, validEocd.cdSize, &inPageError)) {
                if (inPageError) {
                    outExtractedText.clear();
                    return false;
                }
            } else {
                std::vector<ZipTargetEntry> targetEntries;
                targetEntries.reserve(32);

                size_t relCd = 0;
                while (relCd + sizeof(ZipCdHeader) <= cdBuf.size()) {
                    if (isCancelled && isCancelled()) {
                        outExtractedText.clear();
                        return false;
                    }
                    ZipCdHeader cdHeader{};
                    std::memcpy(&cdHeader, cdBuf.data() + relCd, sizeof(ZipCdHeader));
                    if (cdHeader.signature != 0x02014b50u) break;

                    const size_t nameOffset = relCd + sizeof(ZipCdHeader);
                    const size_t entryAdvance = sizeof(ZipCdHeader) +
                                                static_cast<size_t>(cdHeader.filenameLength) +
                                                static_cast<size_t>(cdHeader.extraFieldLength) +
                                                static_cast<size_t>(cdHeader.commentLength);
                    if (entryAdvance == 0 || relCd + entryAdvance > cdBuf.size() ||
                        nameOffset + cdHeader.filenameLength > cdBuf.size()) {
                        break;
                    }

                    std::string_view rawName(
                        reinterpret_cast<const char*>(cdBuf.data() + nameOffset),
                        cdHeader.filenameLength
                    );
                    relCd += entryAdvance;

                    std::string lowerName = normalizeZipEntryPath(rawName);
                    if (!isMatchingNormalizedZipEntry(lowerName, lowerExt)) continue;

                    targetEntries.push_back(ZipTargetEntry{
                        .lowerName = std::move(lowerName),
                        .localHeaderOffset = cdHeader.localHeaderOffset,
                        .compressedSize = cdHeader.compressedSize,
                        .uncompressedSize = cdHeader.uncompressedSize,
                        .compressionMethod = cdHeader.compressionMethod
                    });
                }

                if (isSpreadsheet) {
                    std::vector<std::wstring> sharedStrings;
                    for (const auto& entry : targetEntries) {
                        if (!isSpreadsheetSharedStringsEntry(entry.lowerName)) continue;
                        std::string xml;
                        bool pageErr = false;
                        if (readEntryPayloadByCd(entry, xml, pageErr)) {
                            parseXlsxSharedStrings(xml, sharedStrings, isCancelled);
                        }
                        if (pageErr || (isCancelled && isCancelled())) {
                            outExtractedText.clear();
                            return false;
                        }
                        break;
                    }

                    std::vector<uint8_t> referencedShared(sharedStrings.size(), 0);
                    for (const auto& entry : targetEntries) {
                        if (isSpreadsheetSharedStringsEntry(entry.lowerName) ||
                            !isSpreadsheetWorksheetEntry(entry.lowerName)) {
                            continue;
                        }
                        if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                            outExtractedText.size() >= MaxDocExtractedChars) {
                            break;
                        }
                        std::string xml;
                        bool pageErr = false;
                        if (readEntryPayloadByCd(entry, xml, pageErr)) {
                            const std::wstring sheetText = extractXlsxWorksheetText(
                                xml, sharedStrings, referencedShared, isCancelled);
                            if (!appendPlainTextBlock(sheetText)) break;
                        }
                        if (pageErr || (isCancelled && isCancelled())) {
                            outExtractedText.clear();
                            return false;
                        }
                    }

                    for (const auto& entry : targetEntries) {
                        if (isSpreadsheetSharedStringsEntry(entry.lowerName) ||
                            isSpreadsheetWorksheetEntry(entry.lowerName)) {
                            continue;
                        }
                        if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                            outExtractedText.size() >= MaxDocExtractedChars) {
                            break;
                        }
                        std::string xml;
                        bool pageErr = false;
                        if (readEntryPayloadByCd(entry, xml, pageErr)) {
                            if (!appendGenericXmlEntry(xml)) break;
                        }
                        if (pageErr || (isCancelled && isCancelled())) {
                            outExtractedText.clear();
                            return false;
                        }
                    }

                    for (size_t i = 0; i < sharedStrings.size(); ++i) {
                        if (!referencedShared[i] && !sharedStrings[i].empty()) {
                            if (!appendPlainTextBlock(sharedStrings[i])) break;
                        }
                    }
                } else {
                    for (const auto& entry : targetEntries) {
                        if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                            outExtractedText.size() >= MaxDocExtractedChars) {
                            break;
                        }
                        std::string xml;
                        bool pageErr = false;
                        if (readEntryPayloadByCd(entry, xml, pageErr)) {
                            if (!appendGenericXmlEntry(xml)) break;
                        }
                        if (pageErr || (isCancelled && isCancelled())) {
                            outExtractedText.clear();
                            return false;
                        }
                    }
                }

                if (!outExtractedText.empty()) return true;
            }
        }
    }

    // 2. Fallback: SEH-safe Local Header scan for truncated or non-standard ZIP archives
    std::vector<std::wstring> fallbackSharedStrings;
    std::vector<std::string> deferredWorksheets;
    std::vector<std::string> deferredOtherXmls;

    size_t offset = 0;
    std::vector<uint8_t> scanChunk(MappedStreamChunkBytes);
    while (offset + sizeof(ZipLocalHeader) <= zipSize) {
        if (isCancelled && isCancelled()) {
            outExtractedText.clear();
            return false;
        }

        ZipLocalHeader header{};
        bool inPageError = false;
        if (!safeMappedMemcpyEx(&header, pZipData + offset, sizeof(ZipLocalHeader), &inPageError)) {
            if (inPageError) {
                outExtractedText.clear();
                return false;
            }
            break;
        }
        if (header.signature != 0x04034b50u) break;

        const size_t nameOffset = offset + sizeof(ZipLocalHeader);
        if (nameOffset + header.filenameLength > zipSize) break;

        std::string filename(header.filenameLength, '\0');
        if (header.filenameLength > 0 &&
            !safeMappedMemcpyEx(filename.data(), pZipData + nameOffset, header.filenameLength, &inPageError)) {
            if (inPageError) {
                outExtractedText.clear();
                return false;
            }
            break;
        }

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

        const std::string lowerName = normalizeZipEntryPath(filename);
        size_t consumedStreamComp = 0;
        if (isMatchingNormalizedZipEntry(lowerName, lowerExt)) {
            if (totalUncompressedBytes >= MaxDocUncompressedTotalBytes ||
                outExtractedText.size() >= MaxDocExtractedChars) {
                break;
            }
            const size_t remainingBudget = MaxDocUncompressedTotalBytes - totalUncompressedBytes;

            std::string uncompData;
            if (header.compressionMethod == 0 && header.compressedSize > 0) {
                const size_t takeSize = (std::min)(static_cast<size_t>(header.compressedSize), remainingBudget);
                if (!copyMappedSliceToString(pZipData + dataOffset, takeSize, uncompData, inPageError, isCancelled) &&
                    inPageError) {
                    outExtractedText.clear();
                    return false;
                }
            } else if (header.compressionMethod == 8) {
                const size_t availComp = hasStreamingDataDescriptor
                    ? (zipSize - dataOffset)
                    : static_cast<size_t>(header.compressedSize);
                if (availComp > 0) {
                    if (!decompressMappedRawDeflate(
                            pZipData + dataOffset,
                            availComp,
                            header.uncompressedSize,
                            uncompData,
                            remainingBudget,
                            inPageError,
                            isCancelled,
                            &consumedStreamComp) && inPageError) {
                        outExtractedText.clear();
                        return false;
                    }
                }
            }

            if (!uncompData.empty()) {
                totalUncompressedBytes += uncompData.size();
                if (isSpreadsheet) {
                    if (isSpreadsheetSharedStringsEntry(lowerName)) {
                        parseXlsxSharedStrings(uncompData, fallbackSharedStrings, isCancelled);
                    } else if (isSpreadsheetWorksheetEntry(lowerName)) {
                        deferredWorksheets.push_back(std::move(uncompData));
                    } else {
                        deferredOtherXmls.push_back(std::move(uncompData));
                    }
                } else {
                    if (!appendGenericXmlEntry(uncompData)) {
                        break;
                    }
                }
            }
        }

        if (hasStreamingDataDescriptor) {
            size_t searchFrom = dataOffset + consumedStreamComp;
            size_t nextSig = zipSize;
            while (searchFrom + sizeof(ZipLocalHeader) <= zipSize) {
                if (isCancelled && isCancelled()) {
                    outExtractedText.clear();
                    return false;
                }
                const size_t avail = zipSize - searchFrom;
                const size_t readLen = (std::min)(scanChunk.size(), avail);
                if (!safeMappedMemcpyEx(scanChunk.data(), pZipData + searchFrom, readLen, &inPageError)) {
                    outExtractedText.clear();
                    return false;
                }
                bool found = false;
                for (size_t s = 0; s + 4 <= readLen; ++s) {
                    if (scanChunk[s] == 0x50u && scanChunk[s + 1] == 0x4Bu &&
                        scanChunk[s + 2] == 0x03u && scanChunk[s + 3] == 0x04u) {
                        nextSig = searchFrom + s;
                        found = true;
                        break;
                    }
                }
                if (found || readLen <= 4) break;
                searchFrom += readLen - 3;
            }
            if (nextSig >= zipSize) break;
            offset = nextSig;
        } else {
            offset = dataOffset + static_cast<size_t>(header.compressedSize);
        }
    }

    if (isSpreadsheet &&
        (!fallbackSharedStrings.empty() || !deferredWorksheets.empty() || !deferredOtherXmls.empty())) {
        std::vector<uint8_t> referencedShared(fallbackSharedStrings.size(), 0);
        for (const auto& sheetXml : deferredWorksheets) {
            const std::wstring sheetText = extractXlsxWorksheetText(
                sheetXml, fallbackSharedStrings, referencedShared, isCancelled);
            if (!appendPlainTextBlock(sheetText)) break;
        }
        for (const auto& otherXml : deferredOtherXmls) {
            if (!appendGenericXmlEntry(otherXml)) break;
        }
        for (size_t i = 0; i < fallbackSharedStrings.size(); ++i) {
            if (!referencedShared[i] && !fallbackSharedStrings[i].empty()) {
                if (!appendPlainTextBlock(fallbackSharedStrings[i])) break;
            }
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
            // Stream directly from mapped Central Directory + target XML slices without copying 100MB archives
            if (!extractZipEntriesText(pData, mapSize, ext, extractedAllText, isCancelled)) {
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
