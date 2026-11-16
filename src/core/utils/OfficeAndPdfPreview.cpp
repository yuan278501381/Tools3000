#include "core/utils/OfficeAndPdfPreview.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ole2.h>
#include <filter.h>
#include <filterr.h>
#include <zlib.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tools3000::core {

namespace {

constexpr uint64_t kMaxPreviewArchiveBytes = 64ULL * 1024ULL * 1024ULL; // 64 MB
constexpr size_t kMaxSingleEntryUncompressedBytes = 24ULL * 1024ULL * 1024ULL; // 24 MB
constexpr uint8_t kOle2Signature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

#pragma pack(push, 1)
struct ZipEocdRecord {
    uint32_t signature; // 0x06054b50
    uint16_t diskNumber;
    uint16_t cdDiskNumber;
    uint16_t numEntriesThisDisk;
    uint16_t totalEntries;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLength;
};

struct ZipCdFileHeader {
    uint32_t signature; // 0x02014b50
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;
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

struct ZipLocalFileHeader {
    uint32_t signature; // 0x04034b50
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
};
#pragma pack(pop)

bool readFileBytesZeroLock(const std::wstring& filePath, uint64_t maxBytes, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 ||
        static_cast<uint64_t>(fileSize.QuadPart) > maxBytes) {
        CloseHandle(hFile);
        return false;
    }

    const size_t totalSize = static_cast<size_t>(fileSize.QuadPart);
    outBytes.resize(totalSize);
    size_t offset = 0;
    while (offset < totalSize) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(totalSize - offset, 1024 * 1024));
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, outBytes.data() + offset, chunk, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }
        offset += bytesRead;
    }
    CloseHandle(hFile);
    if (offset == 0) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(offset);
    return true;
}

std::string normalizeZipEntryPath(std::string_view rawPath) {
    std::string out;
    out.reserve(rawPath.size());
    for (char c : rawPath) {
        char norm = (c == '\\') ? '/' : c;
        if (norm == '/' && out.empty()) {
            continue;
        }
        out.push_back(norm);
    }
    return out;
}

class MemoryZipReader {
public:
    struct EntryMeta {
        std::string name;
        uint16_t compressionMethod = 0;
        uint16_t bitFlag = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localHeaderOffset = 0;
    };

    bool open(const std::vector<uint8_t>& buffer) {
        m_data = buffer.data();
        m_size = buffer.size();
        m_entries.clear();
        m_indexByLowerName.clear();

        if (!m_data || m_size < sizeof(ZipEocdRecord)) {
            return false;
        }

        const size_t maxBackScan = (std::min<size_t>)(m_size, 65535 + sizeof(ZipEocdRecord));
        const size_t scanMin = m_size - maxBackScan;
        size_t eocdPos = std::string::npos;
        for (size_t pos = m_size - sizeof(ZipEocdRecord); ; --pos) {
            uint32_t sig = 0;
            std::memcpy(&sig, m_data + pos, sizeof(sig));
            if (sig == 0x06054b50u) {
                eocdPos = pos;
                break;
            }
            if (pos <= scanMin) {
                break;
            }
        }
        if (eocdPos == std::string::npos) {
            return false;
        }

        ZipEocdRecord eocd{};
        std::memcpy(&eocd, m_data + eocdPos, sizeof(eocd));
        if (static_cast<uint64_t>(eocd.cdOffset) + static_cast<uint64_t>(eocd.cdSize) > m_size) {
            return false;
        }

        size_t cursor = eocd.cdOffset;
        const size_t cdEnd = eocd.cdOffset + eocd.cdSize;
        while (cursor + sizeof(ZipCdFileHeader) <= cdEnd) {
            ZipCdFileHeader cd{};
            std::memcpy(&cd, m_data + cursor, sizeof(cd));
            if (cd.signature != 0x02014b50u) {
                break;
            }
            const size_t headerTotal =
                sizeof(ZipCdFileHeader) + cd.filenameLength + cd.extraFieldLength + cd.commentLength;
            if (cursor + headerTotal > cdEnd) {
                break;
            }
            std::string_view rawName(
                reinterpret_cast<const char*>(m_data + cursor + sizeof(ZipCdFileHeader)),
                cd.filenameLength);
            EntryMeta meta;
            meta.name = normalizeZipEntryPath(rawName);
            meta.compressionMethod = cd.compressionMethod;
            meta.bitFlag = cd.bitFlag;
            meta.compressedSize = cd.compressedSize;
            meta.uncompressedSize = cd.uncompressedSize;
            meta.localHeaderOffset = cd.localHeaderOffset;

            std::string lowerName = WinUtils::toLower(meta.name);
            m_indexByLowerName[lowerName] = m_entries.size();
            m_entries.push_back(std::move(meta));
            cursor += headerTotal;
        }

        return !m_entries.empty();
    }

    const std::vector<EntryMeta>& entries() const noexcept { return m_entries; }

    bool hasEntry(std::string_view path) const {
        const std::string key = WinUtils::toLower(normalizeZipEntryPath(path));
        return m_indexByLowerName.find(key) != m_indexByLowerName.end();
    }

    bool extractTextEntry(std::string_view path, std::string& outUtf8, size_t maxOutBytes = kMaxSingleEntryUncompressedBytes) const {
        outUtf8.clear();
        const std::string key = WinUtils::toLower(normalizeZipEntryPath(path));
        auto it = m_indexByLowerName.find(key);
        if (it == m_indexByLowerName.end()) {
            return false;
        }
        const EntryMeta& meta = m_entries[it->second];
        if ((meta.bitFlag & 0x0001u) != 0) {
            return false; // Encrypted ZIP entry
        }
        if (static_cast<uint64_t>(meta.localHeaderOffset) + sizeof(ZipLocalFileHeader) > m_size) {
            return false;
        }

        ZipLocalFileHeader local{};
        std::memcpy(&local, m_data + meta.localHeaderOffset, sizeof(local));
        if (local.signature != 0x04034b50u) {
            return false;
        }

        const uint64_t dataStart =
            static_cast<uint64_t>(meta.localHeaderOffset) +
            sizeof(ZipLocalFileHeader) +
            local.filenameLength +
            local.extraFieldLength;
        if (dataStart + meta.compressedSize > m_size) {
            return false;
        }

        const uint8_t* compPtr = m_data + dataStart;
        if (meta.compressionMethod == 0) {
            const size_t copyLen = (std::min<size_t>)(meta.compressedSize, maxOutBytes);
            outUtf8.assign(reinterpret_cast<const char*>(compPtr), copyLen);
            return true;
        }
        if (meta.compressionMethod != 8) {
            return false;
        }

        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compPtr));
        zs.avail_in = static_cast<uInt>(meta.compressedSize);
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            return false;
        }

        outUtf8.reserve((std::min<size_t>)(meta.uncompressedSize ? meta.uncompressedSize : 65536u, maxOutBytes));
        char chunk[32768];
        int ret = Z_OK;
        while (ret == Z_OK && outUtf8.size() < maxOutBytes) {
            zs.next_out = reinterpret_cast<Bytef*>(chunk);
            zs.avail_out = static_cast<uInt>(sizeof(chunk));
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&zs);
                return false;
            }
            const size_t have = sizeof(chunk) - zs.avail_out;
            const size_t canAppend = (std::min<size_t>)(have, maxOutBytes - outUtf8.size());
            outUtf8.append(chunk, canAppend);
            if (canAppend < have) {
                break;
            }
        }
        inflateEnd(&zs);
        return true;
    }

    bool extractBinaryEntry(std::string_view path, std::vector<uint8_t>& outBytes, size_t maxOutBytes = 8ULL * 1024ULL * 1024ULL) const {
        outBytes.clear();
        std::string tmp;
        if (!extractTextEntry(path, tmp, maxOutBytes)) {
            return false;
        }
        outBytes.assign(reinterpret_cast<const uint8_t*>(tmp.data()),
                        reinterpret_cast<const uint8_t*>(tmp.data()) + tmp.size());
        return true;
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    std::vector<EntryMeta> m_entries;
    std::unordered_map<std::string, size_t> m_indexByLowerName;
};

void appendUnicodeCpAsUtf8(uint32_t cp, std::string& out) {
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

bool decodeXmlEntityToUtf8(std::string_view entity, std::string& out) {
    if (entity == "lt") { out.push_back('<'); return true; }
    if (entity == "gt") { out.push_back('>'); return true; }
    if (entity == "amp") { out.push_back('&'); return true; }
    if (entity == "quot") { out.push_back('"'); return true; }
    if (entity == "apos") { out.push_back('\''); return true; }
    if (entity == "nbsp") { out.push_back(' '); return true; }
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
        appendUnicodeCpAsUtf8(cp, out);
        return true;
    }
    return false;
}

void appendXmlDecodedUtf8(std::string_view raw, std::string& out) {
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '&') {
            const size_t semi = raw.find(';', i + 1);
            if (semi != std::string_view::npos && (semi - (i + 1)) <= 12) {
                if (decodeXmlEntityToUtf8(raw.substr(i + 1, semi - (i + 1)), out)) {
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(raw[i]);
        ++i;
    }
}

std::string decodeXmlAttrToUtf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    appendXmlDecodedUtf8(raw, out);
    return out;
}

struct XmlTag {
    std::string fullLower;
    std::string localLower;
    bool isClosing = false;
    bool isSelfClosing = false;
    size_t attrStart = 0;
    size_t closeBracket = std::string_view::npos;
};

bool parseXmlTag(std::string_view xml, size_t ltPos, XmlTag& out) {
    if (ltPos >= xml.size() || xml[ltPos] != '<') return false;
    const size_t closeTag = xml.find('>', ltPos + 1);
    if (closeTag == std::string_view::npos) return false;

    size_t p = ltPos + 1;
    while (p < closeTag && std::isspace(static_cast<unsigned char>(xml[p]))) ++p;
    if (p >= closeTag || xml[p] == '!' || xml[p] == '?') return false;

    out = XmlTag{};
    out.closeBracket = closeTag;
    if (xml[p] == '/') {
        out.isClosing = true;
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
    out.fullLower.reserve(fullName.size());
    for (char c : fullName) {
        out.fullLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    const size_t colonPos = out.fullLower.rfind(':');
    out.localLower = (colonPos != std::string::npos) ? out.fullLower.substr(colonPos + 1) : out.fullLower;
    out.attrStart = nameEnd;

    size_t back = closeTag;
    while (back > nameEnd && std::isspace(static_cast<unsigned char>(xml[back - 1]))) {
        --back;
    }
    if (!out.isClosing && back > nameEnd && xml[back - 1] == '/') {
        out.isSelfClosing = true;
    }
    return true;
}

std::string_view getXmlAttr(
    std::string_view xml,
    size_t attrStart,
    size_t closeBracket,
    std::string_view targetAttrLower,
    bool matchLocalName = true) {
    size_t p = attrStart;
    while (p < closeBracket) {
        while (p < closeBracket && (std::isspace(static_cast<unsigned char>(xml[p])) || xml[p] == '/')) ++p;
        if (p >= closeBracket) break;

        const size_t nameStart = p;
        while (p < closeBracket &&
               !std::isspace(static_cast<unsigned char>(xml[p])) &&
               xml[p] != '=' && xml[p] != '/' && xml[p] != '>') {
            ++p;
        }
        std::string_view rawAttr = xml.substr(nameStart, p - nameStart);
        if (matchLocalName) {
            const size_t colon = rawAttr.rfind(':');
            if (colon != std::string_view::npos) {
                rawAttr = rawAttr.substr(colon + 1);
            }
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

void extractXlsxTextRuns(std::string_view fragment, std::string& outUtf8) {
    size_t pos = 0;
    bool inT = false;
    int rPhDepth = 0;
    while (pos < fragment.size()) {
        const size_t lt = fragment.find('<', pos);
        if (lt == std::string_view::npos) {
            if (inT && rPhDepth == 0) appendXmlDecodedUtf8(fragment.substr(pos), outUtf8);
            break;
        }
        if (inT && rPhDepth == 0 && lt > pos) {
            appendXmlDecodedUtf8(fragment.substr(pos, lt - pos), outUtf8);
        }
        if (lt + 8 < fragment.size() && fragment.substr(lt, 9) == "<![CDATA[") {
            const size_t closeCdata = fragment.find("]]>", lt + 9);
            if (closeCdata != std::string_view::npos) {
                if (inT && rPhDepth == 0) outUtf8.append(fragment.data() + lt + 9, closeCdata - (lt + 9));
                pos = closeCdata + 3;
                continue;
            }
        }
        if (lt + 3 < fragment.size() && fragment.substr(lt, 4) == "<!--") {
            const size_t closeComment = fragment.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : fragment.size();
            continue;
        }
        XmlTag tag{};
        if (!parseXmlTag(fragment, lt, tag)) {
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
            if (tag.isClosing) inT = false;
            else if (!tag.isSelfClosing) inT = true;
        }
        pos = tag.closeBracket + 1;
    }
}

void parseSharedStringsXml(std::string_view xml, std::vector<std::string>& outStrings) {
    size_t pos = 0;
    std::string buf;
    while (pos < xml.size()) {
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;
        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t closeComment = xml.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : xml.size();
            continue;
        }
        XmlTag tag{};
        if (!parseXmlTag(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }
        if (!tag.isClosing && tag.localLower == "si") {
            if (tag.isSelfClosing) {
                outStrings.emplace_back();
                pos = tag.closeBracket + 1;
                continue;
            }
            size_t scan = tag.closeBracket + 1;
            size_t siCloseStart = xml.size();
            size_t siAfterClose = xml.size();
            while (scan < xml.size()) {
                const size_t innerLt = xml.find('<', scan);
                if (innerLt == std::string_view::npos) break;
                XmlTag innerTag{};
                if (parseXmlTag(xml, innerLt, innerTag) && innerTag.isClosing && innerTag.localLower == "si") {
                    siCloseStart = innerLt;
                    siAfterClose = innerTag.closeBracket + 1;
                    break;
                }
                scan = (innerTag.closeBracket != std::string_view::npos) ? (innerTag.closeBracket + 1) : (innerLt + 1);
            }
            buf.clear();
            extractXlsxTextRuns(xml.substr(tag.closeBracket + 1, siCloseStart - (tag.closeBracket + 1)), buf);
            outStrings.push_back(buf);
            pos = siAfterClose;
            continue;
        }
        pos = tag.closeBracket + 1;
    }
}

bool parseExcelCellColumnIndex(std::string_view cellRef, size_t& outColIndex) noexcept {
    size_t letters = 0;
    size_t colOneBased = 0;
    for (char c : cellRef) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            const size_t digit = static_cast<size_t>(std::toupper(static_cast<unsigned char>(c)) - 'A' + 1);
            if (colOneBased > (SIZE_MAX - digit) / 26u) return false;
            colOneBased = colOneBased * 26u + digit;
            ++letters;
        } else {
            break;
        }
    }
    if (letters == 0 || colOneBased == 0) return false;
    outColIndex = colOneBased - 1;
    return true;
}

std::string decodeXlsxCellValue(
    std::string_view cellInnerXml,
    std::string_view cellType,
    const std::vector<std::string>& sharedStrings) {
    if (cellType == "inlineStr") {
        std::string utf8;
        extractXlsxTextRuns(cellInnerXml, utf8);
        return utf8;
    }

    std::string vRaw;
    size_t pos = 0;
    bool inV = false;
    int fDepth = 0;
    while (pos < cellInnerXml.size()) {
        const size_t lt = cellInnerXml.find('<', pos);
        if (lt == std::string_view::npos) {
            if (inV && fDepth == 0) appendXmlDecodedUtf8(cellInnerXml.substr(pos), vRaw);
            break;
        }
        if (inV && fDepth == 0 && lt > pos) {
            appendXmlDecodedUtf8(cellInnerXml.substr(pos, lt - pos), vRaw);
        }
        XmlTag tag{};
        if (!parseXmlTag(cellInnerXml, lt, tag)) {
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
        size_t s = 0;
        while (s < vRaw.size() && std::isspace(static_cast<unsigned char>(vRaw[s]))) ++s;
        size_t e = vRaw.size();
        while (e > s && std::isspace(static_cast<unsigned char>(vRaw[e - 1]))) --e;
        if (s >= e) return {};
        size_t idx = 0;
        for (size_t k = s; k < e; ++k) {
            if (vRaw[k] < '0' || vRaw[k] > '9') return {};
            idx = idx * 10u + static_cast<size_t>(vRaw[k] - '0');
        }
        return (idx < sharedStrings.size()) ? sharedStrings[idx] : std::string{};
    }

    if (cellType == "b") {
        if (vRaw == "1") return "TRUE";
        if (vRaw == "0") return "FALSE";
    }

    if (!vRaw.empty()) {
        return vRaw;
    }
    std::string fallbackT;
    extractXlsxTextRuns(cellInnerXml, fallbackT);
    return fallbackT;
}

SpreadsheetSheetPreview parseWorksheetXml(
    std::string_view sheetName,
    std::string_view xml,
    const std::vector<std::string>& sharedStrings,
    size_t maxRows,
    size_t maxCols) {
    SpreadsheetSheetPreview sheet;
    sheet.name = std::string(sheetName);

    size_t pos = 0;
    size_t maxObservedCols = 0;

    while (pos < xml.size()) {
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;
        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t closeComment = xml.find("-->", lt + 4);
            pos = (closeComment != std::string_view::npos) ? (closeComment + 3) : xml.size();
            continue;
        }
        XmlTag tag{};
        if (!parseXmlTag(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }
        if (!tag.isClosing && tag.localLower == "row") {
            if (tag.isSelfClosing) {
                pos = tag.closeBracket + 1;
                continue;
            }
            if (sheet.rows.size() >= maxRows) {
                sheet.truncatedRows = true;
                break;
            }

            std::vector<std::string> rowCells;
            size_t nextColIdx = 0;
            size_t scan = tag.closeBracket + 1;
            while (scan < xml.size()) {
                const size_t cellLt = xml.find('<', scan);
                if (cellLt == std::string_view::npos) {
                    scan = xml.size();
                    break;
                }
                XmlTag innerTag{};
                if (!parseXmlTag(xml, cellLt, innerTag)) {
                    scan = cellLt + 1;
                    continue;
                }
                if (innerTag.isClosing && innerTag.localLower == "row") {
                    scan = innerTag.closeBracket + 1;
                    break;
                }
                if (!innerTag.isClosing && innerTag.localLower == "c") {
                    const std::string_view cellRef = getXmlAttr(xml, innerTag.attrStart, innerTag.closeBracket, "r");
                    size_t colIdx = nextColIdx;
                    if (!cellRef.empty()) {
                        parseExcelCellColumnIndex(cellRef, colIdx);
                    }
                    nextColIdx = colIdx + 1;

                    if (innerTag.isSelfClosing) {
                        scan = innerTag.closeBracket + 1;
                        continue;
                    }
                    const std::string_view cellType = getXmlAttr(xml, innerTag.attrStart, innerTag.closeBracket, "t");
                    size_t cScan = innerTag.closeBracket + 1;
                    size_t cCloseStart = xml.size();
                    size_t cAfterClose = xml.size();
                    while (cScan < xml.size()) {
                        const size_t subLt = xml.find('<', cScan);
                        if (subLt == std::string_view::npos) break;
                        XmlTag subTag{};
                        if (parseXmlTag(xml, subLt, subTag) && subTag.isClosing && subTag.localLower == "c") {
                            cCloseStart = subLt;
                            cAfterClose = subTag.closeBracket + 1;
                            break;
                        }
                        cScan = (subTag.closeBracket != std::string_view::npos) ? (subTag.closeBracket + 1) : (subLt + 1);
                    }

                    if (colIdx >= maxCols) {
                        sheet.truncatedCols = true;
                    } else {
                        std::string val = decodeXlsxCellValue(
                            xml.substr(innerTag.closeBracket + 1, cCloseStart - (innerTag.closeBracket + 1)),
                            cellType,
                            sharedStrings);
                        if (!val.empty()) {
                            if (rowCells.size() <= colIdx) {
                                rowCells.resize(colIdx + 1);
                            }
                            rowCells[colIdx] = std::move(val);
                        }
                    }
                    scan = cAfterClose;
                    continue;
                }
                scan = innerTag.closeBracket + 1;
            }

            while (!rowCells.empty() && rowCells.back().empty()) {
                rowCells.pop_back();
            }
            if (!rowCells.empty()) {
                maxObservedCols = (std::max)(maxObservedCols, rowCells.size());
                sheet.rows.push_back(std::move(rowCells));
            }
            pos = scan;
            continue;
        }
        pos = tag.closeBracket + 1;
    }

    if (maxObservedCols > 0) {
        for (auto& r : sheet.rows) {
            if (r.size() < maxObservedCols) {
                r.resize(maxObservedCols);
            }
        }
    }
    return sheet;
}

std::string sanitizeMarkdownTableCell(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') {
            if (out.empty() || out.back() != ' ') out.push_back(' ');
        } else if (c == '|') {
            out += "\\|";
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void extractWordParagraphText(std::string_view pInnerXml, int& outHeadingLevel, bool& outIsListItem, std::string& outText) {
    outHeadingLevel = 0;
    outIsListItem = false;
    outText.clear();

    size_t pos = 0;
    bool inT = false;
    int skipDepth = 0; // Skip <w:instrText>, <w:delText>, or redundant <mc:Fallback>
    size_t textSizeBeforeChoice = 0;
    bool choiceProducedText = false;

    while (pos < pInnerXml.size()) {
        const size_t lt = pInnerXml.find('<', pos);
        if (lt == std::string_view::npos) {
            if (inT && skipDepth == 0) appendXmlDecodedUtf8(pInnerXml.substr(pos), outText);
            break;
        }
        if (inT && skipDepth == 0 && lt > pos) {
            appendXmlDecodedUtf8(pInnerXml.substr(pos, lt - pos), outText);
        }

        XmlTag tag{};
        if (!parseXmlTag(pInnerXml, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (tag.localLower == "choice" && skipDepth == 0) {
            if (!tag.isClosing && !tag.isSelfClosing) {
                textSizeBeforeChoice = outText.size();
                choiceProducedText = false;
            } else if (tag.isClosing) {
                choiceProducedText = (outText.size() > textSizeBeforeChoice);
            }
            pos = tag.closeBracket + 1;
            continue;
        }

        if (tag.localLower == "instrtext" || tag.localLower == "deltext" ||
            (tag.localLower == "fallback" && choiceProducedText)) {
            if (tag.isClosing) {
                if (skipDepth > 0) --skipDepth;
                if (tag.localLower == "fallback" && skipDepth == 0) {
                    choiceProducedText = false;
                }
            } else if (!tag.isSelfClosing) {
                ++skipDepth;
            }
        } else if (skipDepth == 0) {
            if (!tag.isClosing && tag.localLower == "pstyle") {
                const std::string styleVal = WinUtils::toLower(decodeXmlAttrToUtf8(
                    getXmlAttr(pInnerXml, tag.attrStart, tag.closeBracket, "val")));
                if (styleVal == "1" || styleVal.find("heading1") != std::string::npos || styleVal.find("heading 1") != std::string::npos) {
                    outHeadingLevel = 1;
                } else if (styleVal == "2" || styleVal.find("heading2") != std::string::npos || styleVal.find("heading 2") != std::string::npos) {
                    outHeadingLevel = 2;
                } else if (styleVal == "3" || styleVal.find("heading3") != std::string::npos || styleVal.find("heading 3") != std::string::npos) {
                    outHeadingLevel = 3;
                }
            } else if (!tag.isClosing && tag.localLower == "numpr") {
                outIsListItem = true;
            } else if (tag.localLower == "t") {
                if (tag.isClosing) inT = false;
                else if (!tag.isSelfClosing) inT = true;
            } else if (!tag.isClosing && tag.localLower == "tab") {
                outText.push_back('\t');
            } else if (!tag.isClosing && (tag.localLower == "br" || tag.localLower == "cr")) {
                outText.push_back('\n');
            }
        }
        pos = tag.closeBracket + 1;
    }
}

size_t findMatchingCloseTag(std::string_view xml, size_t startAfterOpen, std::string_view localTagName, size_t& outAfterClose) {
    int depth = 1;
    size_t scan = startAfterOpen;
    while (scan < xml.size()) {
        const size_t lt = xml.find('<', scan);
        if (lt == std::string_view::npos) break;
        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t cc = xml.find("-->", lt + 4);
            scan = (cc != std::string_view::npos) ? (cc + 3) : xml.size();
            continue;
        }
        XmlTag t{};
        if (!parseXmlTag(xml, lt, t)) {
            scan = lt + 1;
            continue;
        }
        if (t.localLower == localTagName) {
            if (t.isClosing) {
                --depth;
                if (depth == 0) {
                    outAfterClose = t.closeBracket + 1;
                    return lt;
                }
            } else if (!t.isSelfClosing) {
                ++depth;
            }
        }
        scan = t.closeBracket + 1;
    }
    outAfterClose = xml.size();
    return xml.size();
}

void parseWordDocumentXmlToMarkdown(std::string_view xml, size_t maxChars, std::string& outMd, bool& outTruncated) {
    outMd.clear();
    outTruncated = false;

    size_t pos = 0;
    int tblDepth = 0;
    std::string paraText;

    while (pos < xml.size()) {
        if (outMd.size() >= maxChars) {
            outTruncated = true;
            break;
        }
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;
        if (lt + 3 < xml.size() && xml.substr(lt, 4) == "<!--") {
            const size_t cc = xml.find("-->", lt + 4);
            pos = (cc != std::string_view::npos) ? (cc + 3) : xml.size();
            continue;
        }
        XmlTag tag{};
        if (!parseXmlTag(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }

        if (!tag.isClosing && tag.localLower == "tbl" && tblDepth == 0 && !tag.isSelfClosing) {
            size_t afterTbl = xml.size();
            const size_t tblClose = findMatchingCloseTag(xml, tag.closeBracket + 1, "tbl", afterTbl);
            std::string_view tblInner = xml.substr(tag.closeBracket + 1, tblClose - (tag.closeBracket + 1));

            std::vector<std::vector<std::string>> tableRows;
            size_t maxCols = 0;
            size_t trScan = 0;
            while (trScan < tblInner.size() && tableRows.size() < 120) {
                const size_t trLt = tblInner.find('<', trScan);
                if (trLt == std::string_view::npos) break;
                XmlTag trTag{};
                if (!parseXmlTag(tblInner, trLt, trTag)) {
                    trScan = trLt + 1;
                    continue;
                }
                if (!trTag.isClosing && trTag.localLower == "tr" && !trTag.isSelfClosing) {
                    size_t afterTr = tblInner.size();
                    const size_t trClose = findMatchingCloseTag(tblInner, trTag.closeBracket + 1, "tr", afterTr);
                    std::string_view trInner = tblInner.substr(trTag.closeBracket + 1, trClose - (trTag.closeBracket + 1));

                    std::vector<std::string> cells;
                    size_t tcScan = 0;
                    while (tcScan < trInner.size() && cells.size() < 24) {
                        const size_t tcLt = trInner.find('<', tcScan);
                        if (tcLt == std::string_view::npos) break;
                        XmlTag tcTag{};
                        if (!parseXmlTag(trInner, tcLt, tcTag)) {
                            tcScan = tcLt + 1;
                            continue;
                        }
                        if (!tcTag.isClosing && tcTag.localLower == "tc") {
                            if (tcTag.isSelfClosing) {
                                cells.emplace_back();
                                tcScan = tcTag.closeBracket + 1;
                                continue;
                            }
                            size_t afterTc = trInner.size();
                            const size_t tcClose = findMatchingCloseTag(trInner, tcTag.closeBracket + 1, "tc", afterTc);
                            std::string_view tcInner = trInner.substr(tcTag.closeBracket + 1, tcClose - (tcTag.closeBracket + 1));

                            std::string cellAcc;
                            size_t pScan = 0;
                            while (pScan < tcInner.size()) {
                                const size_t pLt = tcInner.find('<', pScan);
                                if (pLt == std::string_view::npos) break;
                                XmlTag pTag{};
                                if (!parseXmlTag(tcInner, pLt, pTag)) {
                                    pScan = pLt + 1;
                                    continue;
                                }
                                if (!pTag.isClosing && pTag.localLower == "p" && !pTag.isSelfClosing) {
                                    size_t afterP = tcInner.size();
                                    const size_t pClose = findMatchingCloseTag(tcInner, pTag.closeBracket + 1, "p", afterP);
                                    int h = 0;
                                    bool li = false;
                                    extractWordParagraphText(
                                        tcInner.substr(pTag.closeBracket + 1, pClose - (pTag.closeBracket + 1)),
                                        h, li, paraText);
                                    if (!paraText.empty()) {
                                        if (!cellAcc.empty()) cellAcc.push_back(' ');
                                        cellAcc += sanitizeMarkdownTableCell(paraText);
                                    }
                                    pScan = afterP;
                                    continue;
                                }
                                pScan = pTag.closeBracket + 1;
                            }
                            cells.push_back(std::move(cellAcc));
                            tcScan = afterTc;
                            continue;
                        }
                        tcScan = tcTag.closeBracket + 1;
                    }
                    if (!cells.empty()) {
                        maxCols = (std::max)(maxCols, cells.size());
                        tableRows.push_back(std::move(cells));
                    }
                    trScan = afterTr;
                    continue;
                }
                trScan = trTag.closeBracket + 1;
            }

            if (!tableRows.empty() && maxCols > 0) {
                if (!outMd.empty() && outMd.back() != '\n') outMd.push_back('\n');
                outMd.push_back('\n');
                for (size_t r = 0; r < tableRows.size(); ++r) {
                    outMd += "| ";
                    for (size_t c = 0; c < maxCols; ++c) {
                        const std::string& val = (c < tableRows[r].size()) ? tableRows[r][c] : std::string{};
                        outMd += val;
                        outMd += " | ";
                    }
                    outMd.push_back('\n');
                    if (r == 0) {
                        outMd += "| ";
                        for (size_t c = 0; c < maxCols; ++c) {
                            outMd += "--- | ";
                        }
                        outMd.push_back('\n');
                    }
                    if (outMd.size() >= maxChars) {
                        outTruncated = true;
                        break;
                    }
                }
                outMd.push_back('\n');
            }
            pos = afterTbl;
            continue;
        }

        if (!tag.isClosing && tag.localLower == "p" && !tag.isSelfClosing) {
            size_t afterP = xml.size();
            const size_t pClose = findMatchingCloseTag(xml, tag.closeBracket + 1, "p", afterP);
            int heading = 0;
            bool isList = false;
            extractWordParagraphText(
                xml.substr(tag.closeBracket + 1, pClose - (tag.closeBracket + 1)),
                heading, isList, paraText);

            while (!paraText.empty() && (paraText.back() == ' ' || paraText.back() == '\t')) {
                paraText.pop_back();
            }
            if (!paraText.empty()) {
                if (heading == 1) outMd += "# ";
                else if (heading == 2) outMd += "## ";
                else if (heading == 3) outMd += "### ";
                else if (isList) outMd += "- ";

                outMd += paraText;
                outMd += "\n\n";
            }
            pos = afterP;
            continue;
        }

        pos = tag.closeBracket + 1;
    }
}

void parseSlideXmlToMarkdown(std::string_view xml, int slideNumber, std::string& outMd) {
    std::vector<std::string> lines;
    size_t pos = 0;
    std::string para;
    while (pos < xml.size()) {
        const size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) break;
        XmlTag tag{};
        if (!parseXmlTag(xml, lt, tag)) {
            pos = lt + 1;
            continue;
        }
        if (!tag.isClosing && tag.localLower == "p" && !tag.isSelfClosing) {
            size_t afterP = xml.size();
            const size_t pClose = findMatchingCloseTag(xml, tag.closeBracket + 1, "p", afterP);
            std::string_view pInner = xml.substr(tag.closeBracket + 1, pClose - (tag.closeBracket + 1));

            para.clear();
            size_t scan = 0;
            bool inT = false;
            while (scan < pInner.size()) {
                const size_t innerLt = pInner.find('<', scan);
                if (innerLt == std::string_view::npos) {
                    if (inT) appendXmlDecodedUtf8(pInner.substr(scan), para);
                    break;
                }
                if (inT && innerLt > scan) {
                    appendXmlDecodedUtf8(pInner.substr(scan, innerLt - scan), para);
                }
                XmlTag sub{};
                if (!parseXmlTag(pInner, innerLt, sub)) {
                    scan = innerLt + 1;
                    continue;
                }
                if (sub.localLower == "t") {
                    if (sub.isClosing) inT = false;
                    else if (!sub.isSelfClosing) inT = true;
                } else if (!sub.isClosing && sub.localLower == "br") {
                    para.push_back(' ');
                }
                scan = sub.closeBracket + 1;
            }
            while (!para.empty() && std::isspace(static_cast<unsigned char>(para.back()))) para.pop_back();
            size_t lead = 0;
            while (lead < para.size() && std::isspace(static_cast<unsigned char>(para[lead]))) ++lead;
            if (lead < para.size()) {
                lines.push_back(para.substr(lead));
            }
            pos = afterP;
            continue;
        }
        pos = tag.closeBracket + 1;
    }

    if (lines.empty()) return;
    outMd += "## Slide " + std::to_string(slideNumber) + " · " + lines[0] + "\n\n";
    for (size_t i = 1; i < lines.size(); ++i) {
        outMd += "- " + lines[i] + "\n";
    }
    outMd.push_back('\n');
}

// ── Legacy Office IFilter Safe Extraction (SEH Protected) ─────────────────────
using PfnLoadIFilter = HRESULT (STDAPICALLTYPE*)(PCWSTR pwcsPath, IUnknown* pUnkOuter, void** ppIUnk);

PfnLoadIFilter resolveLoadIFilter() noexcept {
    static std::once_flag s_once;
    static PfnLoadIFilter s_fn = nullptr;
    std::call_once(s_once, []() {
        HMODULE hMod = LoadLibraryExW(L"query.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!hMod) hMod = LoadLibraryW(L"query.dll");
        if (hMod) {
            s_fn = reinterpret_cast<PfnLoadIFilter>(GetProcAddress(hMod, "LoadIFilter"));
        }
    });
    return s_fn;
}

HRESULT sehCallLoadIFilter(PfnLoadIFilter fn, PCWSTR path, void** ppFilter) noexcept {
    if (!fn || !path || !ppFilter) return E_INVALIDARG;
    *ppFilter = nullptr;
    __try {
        return fn(path, nullptr, ppFilter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ppFilter = nullptr;
        return E_FAIL;
    }
}

HRESULT sehCallIFilterInit(IFilter* pFilter, ULONG grfFlags, ULONG* pdwFlags) noexcept {
    __try {
        return pFilter->Init(grfFlags, 0, nullptr, pdwFlags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT sehCallIFilterGetChunk(IFilter* pFilter, STAT_CHUNK* pStat) noexcept {
    __try {
        return pFilter->GetChunk(pStat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT sehCallIFilterGetText(IFilter* pFilter, ULONG* pcwcBuffer, WCHAR* awcBuffer) noexcept {
    __try {
        return pFilter->GetText(pcwcBuffer, awcBuffer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *pcwcBuffer = 0;
        return E_FAIL;
    }
}

void sehReleaseUnknown(IUnknown* ptr) noexcept {
    if (!ptr) return;
    __try {
        ptr->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool isCurrentThreadStaApartment() noexcept {
    APTTYPE aptType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER aptQualifier = APTTYPEQUALIFIER_NONE;
    if (FAILED(CoGetApartmentType(&aptType, &aptQualifier))) {
        return false;
    }
    return aptType == APTTYPE_STA || aptType == APTTYPE_MAINSTA;
}

std::string encodePngBase64DataUri(const uint8_t* pngData, size_t pngLen) {
    if (!pngData || pngLen == 0) return {};
    DWORD b64Chars = 0;
    if (!CryptBinaryToStringA(
            pngData,
            static_cast<DWORD>(pngLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &b64Chars) || b64Chars == 0) {
        return {};
    }
    static constexpr std::string_view kPrefix = "data:image/png;base64,";
    std::string out;
    out.resize(kPrefix.size() + b64Chars);
    std::memcpy(out.data(), kPrefix.data(), kPrefix.size());
    DWORD written = b64Chars;
    if (!CryptBinaryToStringA(
            pngData,
            static_cast<DWORD>(pngLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            out.data() + kPrefix.size(),
            &written)) {
        return {};
    }
    out.resize(kPrefix.size() + written);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

bool rawPdfContainsEncryptDictionary(const std::vector<uint8_t>& raw) noexcept {
    static constexpr char kNeedle[] = "/Encrypt";
    if (raw.size() < sizeof(kNeedle) - 1) return false;
    auto it = std::search(
        raw.begin(),
        raw.end(),
        reinterpret_cast<const uint8_t*>(kNeedle),
        reinterpret_cast<const uint8_t*>(kNeedle) + (sizeof(kNeedle) - 1));
    return it != raw.end();
}

PdfPreviewResult rasterizePdfBufferOnMta(
    const std::vector<uint8_t>& rawPdfBytes,
    uint32_t maxPages,
    uint32_t targetWidthPx) {
    PdfPreviewResult res;
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCo);

    try {
        winrt::Windows::Storage::Streams::InMemoryRandomAccessStream pdfStream;
        {
            winrt::Windows::Storage::Streams::DataWriter writer(pdfStream);
            writer.WriteBytes(winrt::array_view<const uint8_t>(
                rawPdfBytes.data(), rawPdfBytes.data() + rawPdfBytes.size()));
            writer.StoreAsync().get();
            writer.FlushAsync().get();
            (void)writer.DetachStream();
        }
        pdfStream.Seek(0);

        auto pdfDoc = winrt::Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(pdfStream).get();
        if (!pdfDoc) {
            if (needUninit) CoUninitialize();
            return res;
        }
        if (pdfDoc.IsPasswordProtected()) {
            res.passwordProtected = true;
            if (needUninit) CoUninitialize();
            return res;
        }

        const uint32_t pageCount = pdfDoc.PageCount();
        res.totalPageCount = pageCount;
        const uint32_t renderCount = (std::min)(pageCount, maxPages);
        res.pageDataUris.reserve(renderCount);

        for (uint32_t i = 0; i < renderCount; ++i) {
            auto page = pdfDoc.GetPage(i);
            const auto sz = page.Size();
            winrt::Windows::Data::Pdf::PdfPageRenderOptions opts;
            const uint32_t destW = (sz.Width > 1.0f)
                ? (std::clamp)(targetWidthPx, 640u, 1920u)
                : 1200u;
            opts.DestinationWidth(destW);
            opts.BackgroundColor(winrt::Windows::UI::Color{255, 255, 255, 255});

            winrt::Windows::Storage::Streams::InMemoryRandomAccessStream pngStream;
            page.RenderToStreamAsync(pngStream, opts).get();

            const uint64_t pngSize64 = pngStream.Size();
            if (pngSize64 > 0 && pngSize64 <= 16ULL * 1024ULL * 1024ULL) {
                pngStream.Seek(0);
                const uint32_t pngSize = static_cast<uint32_t>(pngSize64);
                std::vector<uint8_t> pngBytes(pngSize);
                winrt::Windows::Storage::Streams::DataReader reader(pngStream);
                reader.LoadAsync(pngSize).get();
                reader.ReadBytes(winrt::array_view<uint8_t>(pngBytes.data(), pngBytes.data() + pngBytes.size()));
                (void)reader.DetachStream();

                std::string uri = encodePngBase64DataUri(pngBytes.data(), pngBytes.size());
                if (!uri.empty()) {
                    res.pageDataUris.push_back(std::move(uri));
                }
            }
        }

        res.ok = !res.pageDataUris.empty();
    } catch (const winrt::hresult_error& ex) {
        const uint32_t code = static_cast<uint32_t>(ex.code().value);
        if (code == 0x8007052Bu /* ERROR_WRONG_PASSWORD */ || rawPdfContainsEncryptDictionary(rawPdfBytes)) {
            res.passwordProtected = true;
        }
    } catch (...) {
        if (rawPdfContainsEncryptDictionary(rawPdfBytes)) {
            res.passwordProtected = true;
        }
    }

    // Release cached WinRT activation factories BEFORE CoUninitialize() and before DLL_PROCESS_DETACH
    // so process teardown never releases COM/D2D PDF factories under the Windows Loader Lock!
    winrt::clear_factory_cache();

    if (needUninit) {
        CoUninitialize();
    }
    return res;
}

} // namespace

namespace {

bool containsWideAsciiNeedle(const std::vector<uint8_t>& raw, std::string_view asciiNeedle) noexcept {
    if (asciiNeedle.empty() || raw.size() < asciiNeedle.size() * 2) return false;
    const size_t scanLimit = (std::min<size_t>)(raw.size(), 256 * 1024);
    std::vector<uint8_t> utf16Needle(asciiNeedle.size() * 2, 0);
    for (size_t i = 0; i < asciiNeedle.size(); ++i) {
        utf16Needle[i * 2] = static_cast<uint8_t>(asciiNeedle[i]);
    }
    auto it = std::search(
        raw.begin(),
        raw.begin() + static_cast<std::ptrdiff_t>(scanLimit),
        utf16Needle.begin(),
        utf16Needle.end());
    return it != (raw.begin() + static_cast<std::ptrdiff_t>(scanLimit));
}

bool isEncryptedOle2Container(const std::vector<uint8_t>& raw) noexcept {
    return containsWideAsciiNeedle(raw, "EncryptedPackage") ||
           containsWideAsciiNeedle(raw, "EncryptionInfo") ||
           containsWideAsciiNeedle(raw, "DRMEncryptedTransform");
}

std::string encodeBinaryImageDataUri(const uint8_t* imgData, size_t imgLen, std::string_view mimeType) {
    if (!imgData || imgLen == 0 || mimeType.empty()) return {};
    DWORD b64Chars = 0;
    if (!CryptBinaryToStringA(
            imgData,
            static_cast<DWORD>(imgLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &b64Chars) || b64Chars == 0) {
        return {};
    }
    std::string prefix = "data:";
    prefix.append(mimeType);
    prefix.append(";base64,");

    std::string out;
    out.resize(prefix.size() + b64Chars);
    std::memcpy(out.data(), prefix.data(), prefix.size());
    DWORD written = b64Chars;
    if (!CryptBinaryToStringA(
            imgData,
            static_cast<DWORD>(imgLen),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            out.data() + prefix.size(),
            &written)) {
        return {};
    }
    out.resize(prefix.size() + written);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

std::string_view mediaMimeForExtension(std::string_view lowerName) noexcept {
    if (lowerName.ends_with(".png")) return "image/png";
    if (lowerName.ends_with(".jpg") || lowerName.ends_with(".jpeg")) return "image/jpeg";
    if (lowerName.ends_with(".gif")) return "image/gif";
    if (lowerName.ends_with(".webp")) return "image/webp";
    if (lowerName.ends_with(".bmp")) return "image/bmp";
    return {};
}

void extractPackageEmbeddedImages(
    const MemoryZipReader& zip,
    std::string_view mediaPrefixLower,
    size_t maxImages,
    std::vector<std::string>& outDataUris) {
    outDataUris.clear();
    std::vector<std::string> mediaEntries;
    for (const auto& entry : zip.entries()) {
        const std::string lower = WinUtils::toLower(entry.name);
        if (lower.rfind(mediaPrefixLower, 0) == 0 && !mediaMimeForExtension(lower).empty()) {
            mediaEntries.push_back(entry.name);
        }
    }
    std::sort(mediaEntries.begin(), mediaEntries.end());

    std::vector<uint8_t> imgBytes;
    for (const auto& entryPath : mediaEntries) {
        if (outDataUris.size() >= maxImages) break;
        const std::string lower = WinUtils::toLower(entryPath);
        const std::string_view mime = mediaMimeForExtension(lower);
        if (mime.empty()) continue;
        if (!zip.extractBinaryEntry(entryPath, imgBytes, 6ULL * 1024ULL * 1024ULL) || imgBytes.empty()) {
            continue;
        }
        std::string uri = encodeBinaryImageDataUri(imgBytes.data(), imgBytes.size(), mime);
        if (!uri.empty()) {
            outDataUris.push_back(std::move(uri));
        }
    }
}

std::string findSheetRelationshipId(
    std::string_view xml,
    size_t attrStart,
    size_t closeBracket,
    const std::unordered_map<std::string, std::string>& relTargetByLowerId) {
    std::string rid = decodeXmlAttrToUtf8(getXmlAttr(xml, attrStart, closeBracket, "r:id", false));
    if (!rid.empty() && relTargetByLowerId.find(WinUtils::toLower(rid)) != relTargetByLowerId.end()) {
        return rid;
    }
    // 扫描任意命名空间前缀的 *:id 属性（如 ns0:id / d2p1:id / relationships:id）
    size_t p = attrStart;
    while (p < closeBracket) {
        while (p < closeBracket && (std::isspace(static_cast<unsigned char>(xml[p])) || xml[p] == '/')) ++p;
        if (p >= closeBracket) break;
        const size_t nameStart = p;
        while (p < closeBracket && !std::isspace(static_cast<unsigned char>(xml[p])) &&
               xml[p] != '=' && xml[p] != '/' && xml[p] != '>') {
            ++p;
        }
        std::string_view rawAttr = xml.substr(nameStart, p - nameStart);
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

        const std::string lowerAttr = WinUtils::toLower(std::string(rawAttr));
        if (lowerAttr == "id" || (lowerAttr.size() > 3 && lowerAttr.ends_with(":id"))) {
            std::string decoded = decodeXmlAttrToUtf8(val);
            if (relTargetByLowerId.find(WinUtils::toLower(decoded)) != relTargetByLowerId.end()) {
                return decoded;
            }
            if (rid.empty()) {
                rid = std::move(decoded);
            }
        }
    }
    return rid;
}

} // namespace

SpreadsheetPreviewResult OfficeAndPdfPreview::extractXlsxPreview(
    const std::wstring& filePath,
    size_t maxSheets,
    size_t maxRowsPerSheet,
    size_t maxColsPerRow) {
    SpreadsheetPreviewResult result;
    std::vector<uint8_t> raw;
    if (!readFileBytesZeroLock(filePath, kMaxPreviewArchiveBytes, raw)) {
        return result;
    }
    if (raw.size() >= 8 && std::memcmp(raw.data(), kOle2Signature, 8) == 0) {
        result.encryptedOrUnsupported = isEncryptedOle2Container(raw);
        return result;
    }

    MemoryZipReader zip;
    if (!zip.open(raw)) {
        return result;
    }

    // Parse xl/_rels/workbook.xml.rels -> rId to worksheet path & custom sharedStrings path
    std::unordered_map<std::string, std::string> relTargetByLowerId;
    std::string customSharedStringsPath;
    std::string relsXml;
    if (zip.extractTextEntry("xl/_rels/workbook.xml.rels", relsXml)) {
        size_t pos = 0;
        while (pos < relsXml.size()) {
            const size_t lt = relsXml.find('<', pos);
            if (lt == std::string::npos) break;
            XmlTag tag{};
            if (!parseXmlTag(relsXml, lt, tag)) {
                pos = lt + 1;
                continue;
            }
            if (!tag.isClosing && tag.localLower == "relationship") {
                const std::string id = decodeXmlAttrToUtf8(getXmlAttr(relsXml, tag.attrStart, tag.closeBracket, "id"));
                const std::string relType = WinUtils::toLower(
                    decodeXmlAttrToUtf8(getXmlAttr(relsXml, tag.attrStart, tag.closeBracket, "type")));
                std::string target = decodeXmlAttrToUtf8(getXmlAttr(relsXml, tag.attrStart, tag.closeBracket, "target"));
                target = normalizeZipEntryPath(target);
                if (!target.empty() && target.rfind("xl/", 0) != 0) {
                    if (target.rfind("../", 0) == 0) {
                        target = target.substr(3);
                    } else if (!zip.hasEntry(target)) {
                        target = "xl/" + target;
                    }
                }
                if (relType.find("sharedstrings") != std::string::npos && !target.empty()) {
                    customSharedStringsPath = target;
                }
                if (!id.empty() && !target.empty()) {
                    relTargetByLowerId[WinUtils::toLower(id)] = std::move(target);
                }
            }
            pos = tag.closeBracket + 1;
        }
    }

    std::vector<std::string> sharedStrings;
    std::string sharedXml;
    if (zip.extractTextEntry("xl/sharedStrings.xml", sharedXml) ||
        (!customSharedStringsPath.empty() && zip.extractTextEntry(customSharedStringsPath, sharedXml))) {
        parseSharedStringsXml(sharedXml, sharedStrings);
        sharedXml.clear();
    }

    struct SheetEntryRef {
        std::string name;
        std::string zipPath;
    };
    std::vector<SheetEntryRef> sheetRefs;

    std::string workbookXml;
    if (zip.extractTextEntry("xl/workbook.xml", workbookXml)) {
        size_t pos = 0;
        size_t fallbackIdx = 1;
        while (pos < workbookXml.size()) {
            const size_t lt = workbookXml.find('<', pos);
            if (lt == std::string::npos) break;
            XmlTag tag{};
            if (!parseXmlTag(workbookXml, lt, tag)) {
                pos = lt + 1;
                continue;
            }
            if (!tag.isClosing && tag.localLower == "sheet") {
                std::string name = decodeXmlAttrToUtf8(getXmlAttr(workbookXml, tag.attrStart, tag.closeBracket, "name"));
                const std::string rid = findSheetRelationshipId(
                    workbookXml, tag.attrStart, tag.closeBracket, relTargetByLowerId);
                std::string targetPath;
                auto it = relTargetByLowerId.find(WinUtils::toLower(rid));
                if (it != relTargetByLowerId.end() && zip.hasEntry(it->second)) {
                    targetPath = it->second;
                } else {
                    std::string candidate = "xl/worksheets/sheet" + std::to_string(fallbackIdx) + ".xml";
                    if (zip.hasEntry(candidate)) {
                        targetPath = std::move(candidate);
                    }
                }
                if (name.empty()) {
                    name = "Sheet" + std::to_string(fallbackIdx);
                }
                if (!targetPath.empty()) {
                    sheetRefs.push_back({std::move(name), std::move(targetPath)});
                }
                ++fallbackIdx;
            }
            pos = tag.closeBracket + 1;
        }
    }

    if (sheetRefs.empty()) {
        for (size_t i = 1; i <= 24; ++i) {
            std::string candidate = "xl/worksheets/sheet" + std::to_string(i) + ".xml";
            if (zip.hasEntry(candidate)) {
                sheetRefs.push_back({"Sheet" + std::to_string(i), std::move(candidate)});
            }
        }
    }

    result.totalSheetCount = sheetRefs.size();
    if (sheetRefs.size() > maxSheets) {
        result.truncatedSheets = true;
    }

    const size_t limit = (std::min)(sheetRefs.size(), maxSheets);
    std::string sheetXml;
    for (size_t i = 0; i < limit; ++i) {
        if (!zip.extractTextEntry(sheetRefs[i].zipPath, sheetXml)) {
            continue;
        }
        SpreadsheetSheetPreview parsed = parseWorksheetXml(
            sheetRefs[i].name, sheetXml, sharedStrings, maxRowsPerSheet, maxColsPerRow);
        result.sheets.push_back(std::move(parsed));
    }

    result.ok = !result.sheets.empty();
    return result;
}

DocumentPreviewResult OfficeAndPdfPreview::extractDocxOrPptxPreview(
    const std::wstring& filePath,
    const std::string& lowerExt,
    size_t maxChars) {
    DocumentPreviewResult result;
    std::vector<uint8_t> raw;
    if (!readFileBytesZeroLock(filePath, kMaxPreviewArchiveBytes, raw)) {
        return result;
    }
    if (raw.size() >= 8 && std::memcmp(raw.data(), kOle2Signature, 8) == 0) {
        result.encryptedOrUnsupported = isEncryptedOle2Container(raw);
        return result;
    }

    MemoryZipReader zip;
    if (!zip.open(raw)) {
        return result;
    }

    if (lowerExt == "docx") {
        std::string mainDocPath = "word/document.xml";
        if (!zip.hasEntry(mainDocPath)) {
            std::string rootRels;
            if (zip.extractTextEntry("_rels/.rels", rootRels)) {
                size_t pos = 0;
                while (pos < rootRels.size()) {
                    const size_t lt = rootRels.find('<', pos);
                    if (lt == std::string::npos) break;
                    XmlTag tag{};
                    if (!parseXmlTag(rootRels, lt, tag)) {
                        pos = lt + 1;
                        continue;
                    }
                    if (!tag.isClosing && tag.localLower == "relationship") {
                        const std::string relType = WinUtils::toLower(
                            decodeXmlAttrToUtf8(getXmlAttr(rootRels, tag.attrStart, tag.closeBracket, "type")));
                        std::string target = normalizeZipEntryPath(
                            decodeXmlAttrToUtf8(getXmlAttr(rootRels, tag.attrStart, tag.closeBracket, "target")));
                        if (relType.find("officedocument") != std::string::npos && !target.empty() && zip.hasEntry(target)) {
                            mainDocPath = std::move(target);
                            break;
                        }
                    }
                    pos = tag.closeBracket + 1;
                }
            }
        }

        std::string docXml;
        if (!zip.extractTextEntry(mainDocPath, docXml)) {
            return result;
        }
        parseWordDocumentXmlToMarkdown(docXml, maxChars, result.markdownText, result.truncated);

        // 若正文区域为空（如纯页眉/批注/文本框模板），补充扫描页眉、页脚与脚注
        if (result.markdownText.empty()) {
            std::vector<std::string> extraXmlPaths;
            for (const auto& entry : zip.entries()) {
                const std::string lower = WinUtils::toLower(entry.name);
                if ((lower.rfind("word/header", 0) == 0 ||
                     lower.rfind("word/footer", 0) == 0 ||
                     lower == "word/footnotes.xml" ||
                     lower == "word/endnotes.xml") &&
                    lower.ends_with(".xml")) {
                    extraXmlPaths.push_back(entry.name);
                }
            }
            std::sort(extraXmlPaths.begin(), extraXmlPaths.end());
            std::string partXml;
            std::string partMd;
            bool partTrunc = false;
            for (const auto& ep : extraXmlPaths) {
                if (result.markdownText.size() >= maxChars) break;
                if (zip.extractTextEntry(ep, partXml)) {
                    parseWordDocumentXmlToMarkdown(partXml, maxChars - result.markdownText.size(), partMd, partTrunc);
                    if (!partMd.empty()) {
                        result.markdownText += partMd;
                    }
                }
            }
        }

        // 提取 word/media/* 内嵌图片（完美支持纯截图粘贴型工单/需求文档！）
        extractPackageEmbeddedImages(zip, "word/media/", 8, result.embeddedImages);

        result.emptyDocument = result.markdownText.empty() && result.embeddedImages.empty();
        result.ok = true;
        return result;
    }

    if (lowerExt == "pptx") {
        std::map<int, std::string> slideEntries;
        for (const auto& entry : zip.entries()) {
            const std::string lower = WinUtils::toLower(entry.name);
            static constexpr std::string_view kPrefix = "ppt/slides/slide";
            static constexpr std::string_view kSuffix = ".xml";
            if (lower.size() > kPrefix.size() + kSuffix.size() &&
                lower.compare(0, kPrefix.size(), kPrefix) == 0 &&
                lower.compare(lower.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
                std::string_view numPart(
                    lower.data() + kPrefix.size(),
                    lower.size() - kPrefix.size() - kSuffix.size());
                int slideNo = 0;
                bool validDigits = !numPart.empty();
                for (char ch : numPart) {
                    if (ch < '0' || ch > '9') { validDigits = false; break; }
                    slideNo = slideNo * 10 + (ch - '0');
                }
                if (validDigits && slideNo > 0) {
                    slideEntries[slideNo] = entry.name;
                }
            }
        }

        std::string slideXml;
        size_t slideCount = 0;
        for (const auto& [slideNo, entryName] : slideEntries) {
            if (slideCount >= 30 || result.markdownText.size() >= maxChars) {
                result.truncated = true;
                break;
            }
            if (zip.extractTextEntry(entryName, slideXml)) {
                parseSlideXmlToMarkdown(slideXml, slideNo, result.markdownText);
                ++slideCount;
            }
        }
        extractPackageEmbeddedImages(zip, "ppt/media/", 8, result.embeddedImages);
        const bool validPresentation = !slideEntries.empty() || zip.hasEntry("ppt/presentation.xml");
        if (validPresentation) {
            result.emptyDocument = result.markdownText.empty() && result.embeddedImages.empty();
            result.ok = true;
        }
        return result;
    }

    return result;
}

DocumentPreviewResult OfficeAndPdfPreview::extractLegacyOfficePreview(
    const std::wstring& filePath,
    size_t maxChars) {
    DocumentPreviewResult result;
    PfnLoadIFilter loadIFilter = resolveLoadIFilter();
    if (!loadIFilter) {
        return result;
    }

    auto workerFn = [&]() {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = SUCCEEDED(hrCo);

        void* rawFilter = nullptr;
        if (SUCCEEDED(sehCallLoadIFilter(loadIFilter, filePath.c_str(), &rawFilter)) && rawFilter) {
            auto* pFilter = static_cast<IFilter*>(rawFilter);
            ULONG flags = 0;
            const ULONG initFlags =
                IFILTER_INIT_CANON_PARAGRAPHS |
                IFILTER_INIT_HARD_LINE_BREAKS |
                IFILTER_INIT_CANON_SPACES;
            if (SUCCEEDED(sehCallIFilterInit(pFilter, initFlags, &flags))) {
                std::wstring wideAcc;
                wideAcc.reserve(16384);
                WCHAR chunkBuf[8192];
                STAT_CHUNK stat{};
                while (wideAcc.size() < maxChars) {
                    const HRESULT hrChunk = sehCallIFilterGetChunk(pFilter, &stat);
                    if (hrChunk != S_OK) break;
                    if ((stat.flags & CHUNK_TEXT) == 0) continue;

                    if (!wideAcc.empty() && wideAcc.back() != L'\n') {
                        wideAcc.push_back(L'\n');
                    }
                    while (wideAcc.size() < maxChars) {
                        ULONG cwc = static_cast<ULONG>(std::size(chunkBuf));
                        const HRESULT hrText = sehCallIFilterGetText(pFilter, &cwc, chunkBuf);
                        if (SUCCEEDED(hrText) && cwc > 0) {
                            wideAcc.append(chunkBuf, cwc);
                        }
                        if (hrText == FILTER_S_LAST_TEXT || FAILED(hrText) || cwc == 0) {
                            break;
                        }
                    }
                }
                if (wideAcc.size() >= maxChars) {
                    result.truncated = true;
                }
                result.markdownText = WinUtils::wstringToUtf8(wideAcc);
                // Trim leading/trailing whitespace
                while (!result.markdownText.empty() &&
                       std::isspace(static_cast<unsigned char>(result.markdownText.back()))) {
                    result.markdownText.pop_back();
                }
                result.ok = !result.markdownText.empty();
            }
            sehReleaseUnknown(pFilter);
        }

        if (needUninit) {
            CoUninitialize();
        }
    };

    if (isCurrentThreadStaApartment()) {
        auto sharedRes = std::make_shared<DocumentPreviewResult>();
        std::thread mtaWorker([sharedRes, filePath, maxChars, loadIFilter]() {
            const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool needUninit = SUCCEEDED(hrCo);

            void* rawFilter = nullptr;
            if (SUCCEEDED(sehCallLoadIFilter(loadIFilter, filePath.c_str(), &rawFilter)) && rawFilter) {
                auto* pFilter = static_cast<IFilter*>(rawFilter);
                ULONG flags = 0;
                const ULONG initFlags =
                    IFILTER_INIT_CANON_PARAGRAPHS |
                    IFILTER_INIT_HARD_LINE_BREAKS |
                    IFILTER_INIT_CANON_SPACES;
                if (SUCCEEDED(sehCallIFilterInit(pFilter, initFlags, &flags))) {
                    std::wstring wideAcc;
                    wideAcc.reserve(16384);
                    WCHAR chunkBuf[8192];
                    STAT_CHUNK stat{};
                    while (wideAcc.size() < maxChars) {
                        const HRESULT hrChunk = sehCallIFilterGetChunk(pFilter, &stat);
                        if (hrChunk != S_OK) break;
                        if ((stat.flags & CHUNK_TEXT) == 0) continue;

                        if (!wideAcc.empty() && wideAcc.back() != L'\n') {
                            wideAcc.push_back(L'\n');
                        }
                        while (wideAcc.size() < maxChars) {
                            ULONG cwc = static_cast<ULONG>(std::size(chunkBuf));
                            const HRESULT hrText = sehCallIFilterGetText(pFilter, &cwc, chunkBuf);
                            if (SUCCEEDED(hrText) && cwc > 0) {
                                wideAcc.append(chunkBuf, cwc);
                            }
                            if (hrText == FILTER_S_LAST_TEXT || FAILED(hrText) || cwc == 0) {
                                break;
                            }
                        }
                    }
                    if (wideAcc.size() >= maxChars) {
                        sharedRes->truncated = true;
                    }
                    sharedRes->markdownText = WinUtils::wstringToUtf8(wideAcc);
                    while (!sharedRes->markdownText.empty() &&
                           std::isspace(static_cast<unsigned char>(sharedRes->markdownText.back()))) {
                        sharedRes->markdownText.pop_back();
                    }
                    sharedRes->ok = !sharedRes->markdownText.empty();
                }
                sehReleaseUnknown(pFilter);
            }

            if (needUninit) {
                CoUninitialize();
            }
        });
        if (joinWorkerWhilePumpingSentMessages(mtaWorker, 5000)) {
            return std::move(*sharedRes);
        }
        return result;
    } else {
        workerFn();
    }
    return result;
}

PdfPreviewResult OfficeAndPdfPreview::renderPdfPreviewPages(
    const std::wstring& filePath,
    uint32_t maxPages,
    uint32_t targetWidthPx) {
    PdfPreviewResult res;
    std::vector<uint8_t> rawPdf;
    if (!readFileBytesZeroLock(filePath, kMaxPreviewArchiveBytes, rawPdf)) {
        return res;
    }
    if (rawPdf.size() < 5) {
        return res;
    }
    return renderPdfPreviewPagesFromMemory(rawPdf.data(), rawPdf.size(), maxPages, targetWidthPx);
}

PdfPreviewResult OfficeAndPdfPreview::renderPdfPreviewPagesFromMemory(
    const uint8_t* pdfBytes,
    size_t pdfSize,
    uint32_t maxPages,
    uint32_t targetWidthPx) {
    PdfPreviewResult res;
    if (!pdfBytes || pdfSize < 8) {
        return res;
    }

    std::vector<uint8_t> buffer(pdfBytes, pdfBytes + pdfSize);
    if (isCurrentThreadStaApartment()) {
        auto sharedRes = std::make_shared<PdfPreviewResult>();
        auto sharedBuf = std::make_shared<std::vector<uint8_t>>(std::move(buffer));
        std::thread mtaWorker([sharedRes, sharedBuf, maxPages, targetWidthPx]() {
            *sharedRes = rasterizePdfBufferOnMta(*sharedBuf, maxPages, targetWidthPx);
        });
        if (joinWorkerWhilePumpingSentMessages(mtaWorker, 8000)) {
            return std::move(*sharedRes);
        }
        return res;
    }
    return rasterizePdfBufferOnMta(buffer, maxPages, targetWidthPx);
}

PdfPreviewResult OfficeAndPdfPreview::renderAiOrEpsPreviewPages(
    const std::wstring& filePath,
    uint32_t maxPages,
    uint32_t targetWidthPx) {
    PdfPreviewResult res;
    std::vector<uint8_t> raw;
    if (!readFileBytesZeroLock(filePath, kMaxPreviewArchiveBytes, raw)) {
        return res;
    }
    if (raw.size() < 8) {
        return res;
    }

    static constexpr size_t kMaxPdfHeaderScanBytes = 4ULL * 1024ULL * 1024ULL;
    const size_t scanLimit = (std::min)(raw.size(), kMaxPdfHeaderScanBytes);
    size_t pdfOffset = std::string::npos;
    for (size_t i = 0; i + 7 < scanLimit; ++i) {
        if (raw[i] == '%' && raw[i + 1] == 'P' && raw[i + 2] == 'D' &&
            raw[i + 3] == 'F' && raw[i + 4] == '-' &&
            std::isdigit(static_cast<unsigned char>(raw[i + 5])) &&
            raw[i + 6] == '.') {
            pdfOffset = i;
            break;
        }
    }
    if (pdfOffset == std::string::npos) {
        return res;
    }

    const uint8_t* sliceStart = raw.data() + pdfOffset;
    const size_t sliceLen = raw.size() - pdfOffset;
    res = renderPdfPreviewPagesFromMemory(sliceStart, sliceLen, maxPages, targetWidthPx);
    if (res.ok || res.passwordProtected) {
        return res;
    }

    // 部分混合 PostScript + PDF 的 .eps/.ai 在 PDF %%EOF 后跟有大量 PostScript 尾注，
    // 导致严格校验尾部 %%EOF 距离的解析器退回；自动裁剪至首个有效 %%EOF 边界重试。
    static constexpr char kEofMarker[] = "%%EOF";
    std::string_view pdfView(reinterpret_cast<const char*>(sliceStart), sliceLen);
    size_t searchFrom = 8;
    while (searchFrom < pdfView.size()) {
        const size_t eofPos = pdfView.find(kEofMarker, searchFrom);
        if (eofPos == std::string_view::npos) {
            break;
        }
        size_t cutLen = eofPos + (sizeof(kEofMarker) - 1);
        while (cutLen < pdfView.size() && (pdfView[cutLen] == '\r' || pdfView[cutLen] == '\n')) {
            ++cutLen;
        }
        if (cutLen < sliceLen) {
            auto retryRes = renderPdfPreviewPagesFromMemory(sliceStart, cutLen, maxPages, targetWidthPx);
            if (retryRes.ok || retryRes.passwordProtected) {
                return retryRes;
            }
        }
        searchFrom = eofPos + 5;
    }

    return res;
}

} // namespace tools3000::core
