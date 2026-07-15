#include "PdfExtractor.h"
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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tools3000::service::content {

namespace {

constexpr uint64_t MaxPdfFileBytes = 100ULL * 1024ULL * 1024ULL;
constexpr size_t MaxSingleStreamDecompressedBytes = 16ULL * 1024ULL * 1024ULL;
constexpr size_t MaxTotalPdfDecompressedBytes = 48ULL * 1024ULL * 1024ULL;
constexpr size_t MaxPdfExtractedChars = 15ULL * 1024ULL * 1024ULL;
constexpr uint32_t MaxBfRangeSpan = 65536u;

inline bool isPdfWhitespace(uint8_t ch) noexcept {
    return ch == 0x00 || ch == 0x09 || ch == 0x0A || ch == 0x0C || ch == 0x0D || ch == 0x20;
}

inline bool isPdfDelimiter(uint8_t ch) noexcept {
    return ch == '(' || ch == ')' || ch == '<' || ch == '>' ||
           ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
           ch == '/' || ch == '%';
}

inline bool isPdfTokenBoundaryAt(std::string_view src, size_t idx) noexcept {
    if (idx >= src.size()) return true;
    const uint8_t ch = static_cast<uint8_t>(src[idx]);
    return isPdfWhitespace(ch) || isPdfDelimiter(ch);
}

inline bool hasPdfNameToken(std::string_view src, std::string_view token) noexcept {
    size_t searchFrom = 0;
    while (searchFrom < src.size()) {
        const size_t pos = src.find(token, searchFrom);
        if (pos == std::string_view::npos) return false;
        if (isPdfTokenBoundaryAt(src, pos + token.size())) {
            return true;
        }
        searchFrom = pos + token.size();
    }
    return false;
}

inline int hexNibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::vector<uint8_t> decodeHexBytes(std::string_view hexStr) {
    std::vector<uint8_t> out;
    out.reserve((hexStr.size() + 1) / 2);
    int high = -1;
    for (char ch : hexStr) {
        const int nib = hexNibble(ch);
        if (nib < 0) continue;
        if (high < 0) {
            high = nib;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | nib));
            high = -1;
        }
    }
    if (high >= 0) {
        out.push_back(static_cast<uint8_t>(high << 4));
    }
    return out;
}

std::wstring decodeUtf16BeBytes(const uint8_t* bytes, size_t len) {
    std::wstring out;
    if (!bytes || len == 0) return out;
    if (len == 1) {
        out.push_back(static_cast<wchar_t>(bytes[0]));
        return out;
    }
    size_t start = 0;
    if (len >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        start = 2;
    }
    out.reserve((len - start) / 2);
    for (size_t i = start; i + 1 < len; i += 2) {
        const uint16_t unit = (static_cast<uint16_t>(bytes[i]) << 8) |
                              static_cast<uint16_t>(bytes[i + 1]);
        if (unit != 0) {
            out.push_back(static_cast<wchar_t>(unit));
        }
    }
    return out;
}

void incrementUnicodeString(std::wstring& str, uint32_t delta) {
    if (str.empty() || delta == 0) return;

    // Decode trailing UTF-16 codepoint (including surrogate pair if present)
    if (str.size() >= 2 && isHighSurrogate(str[str.size() - 2]) && isLowSurrogate(str.back())) {
        const uint32_t hi = static_cast<uint16_t>(str[str.size() - 2]) - 0xD800u;
        const uint32_t lo = static_cast<uint16_t>(str.back()) - 0xDC00u;
        uint32_t cp = 0x10000u + ((hi << 10) | lo) + delta;
        if (cp <= 0x10FFFFu) {
            cp -= 0x10000u;
            str[str.size() - 2] = static_cast<wchar_t>(0xD800u + ((cp >> 10) & 0x3FFu));
            str.back() = static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu));
        }
        return;
    }

    uint32_t cp = static_cast<uint16_t>(str.back()) + delta;
    if (cp <= 0xFFFFu) {
        str.back() = static_cast<wchar_t>(cp);
    } else if (cp <= 0x10FFFFu) {
        cp -= 0x10000u;
        str.back() = static_cast<wchar_t>(0xD800u + ((cp >> 10) & 0x3FFu));
        str.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
    }
}

struct PdfCMap {
    std::unordered_map<uint8_t, std::wstring> map1Byte;
    std::unordered_map<uint16_t, std::wstring> map2Byte;
    std::unordered_map<uint32_t, std::wstring> map4Byte;
    bool prefersTwoByte = false;

    bool empty() const noexcept {
        return map1Byte.empty() && map2Byte.empty() && map4Byte.empty();
    }

    void insertCode(const std::vector<uint8_t>& srcBytes, std::wstring target) {
        if (srcBytes.empty() || target.empty()) return;
        if (srcBytes.size() == 1) {
            map1Byte[srcBytes[0]] = std::move(target);
        } else if (srcBytes.size() == 2) {
            const uint16_t code = (static_cast<uint16_t>(srcBytes[0]) << 8) |
                                  static_cast<uint16_t>(srcBytes[1]);
            map2Byte[code] = std::move(target);
            prefersTwoByte = true;
        } else {
            uint32_t code = 0;
            for (size_t i = 0; i < (std::min<size_t>)(4, srcBytes.size()); ++i) {
                code = (code << 8) | srcBytes[i];
            }
            map4Byte[code] = std::move(target);
        }
    }

    void mergeFrom(const PdfCMap& other) {
        for (const auto& [k, v] : other.map1Byte) {
            map1Byte.try_emplace(k, v);
        }
        for (const auto& [k, v] : other.map2Byte) {
            map2Byte.try_emplace(k, v);
        }
        for (const auto& [k, v] : other.map4Byte) {
            map4Byte.try_emplace(k, v);
        }
        if (other.prefersTwoByte) {
            prefersTwoByte = true;
        }
    }

    bool decodeBytes(const std::vector<uint8_t>& raw, std::wstring& out) const {
        if (raw.empty() || empty()) return false;
        std::wstring result;
        size_t matchedUnits = 0;

        if ((prefersTwoByte || !map2Byte.empty()) && (raw.size() % 2 == 0)) {
            result.reserve(raw.size() / 2);
            for (size_t i = 0; i + 1 < raw.size(); i += 2) {
                const uint16_t code = (static_cast<uint16_t>(raw[i]) << 8) |
                                      static_cast<uint16_t>(raw[i + 1]);
                const auto it = map2Byte.find(code);
                if (it != map2Byte.end()) {
                    result += it->second;
                    ++matchedUnits;
                } else if (!map1Byte.empty()) {
                    const auto it1a = map1Byte.find(raw[i]);
                    const auto it1b = map1Byte.find(raw[i + 1]);
                    if (it1a != map1Byte.end() && it1b != map1Byte.end()) {
                        result += it1a->second;
                        result += it1b->second;
                        ++matchedUnits;
                    }
                }
            }
            if (matchedUnits > 0) {
                out += result;
                return true;
            }
        }

        if (!map1Byte.empty()) {
            result.clear();
            matchedUnits = 0;
            result.reserve(raw.size());
            for (uint8_t b : raw) {
                const auto it = map1Byte.find(b);
                if (it != map1Byte.end()) {
                    result += it->second;
                    ++matchedUnits;
                } else if (b >= 0x20 && b <= 0x7E) {
                    result.push_back(static_cast<wchar_t>(b));
                }
            }
            if (matchedUnits > 0) {
                out += result;
                return true;
            }
        }

        if (!map4Byte.empty() && (raw.size() % 4 == 0)) {
            result.clear();
            matchedUnits = 0;
            for (size_t i = 0; i + 3 < raw.size(); i += 4) {
                const uint32_t code = (static_cast<uint32_t>(raw[i]) << 24) |
                                      (static_cast<uint32_t>(raw[i + 1]) << 16) |
                                      (static_cast<uint32_t>(raw[i + 2]) << 8) |
                                      static_cast<uint32_t>(raw[i + 3]);
                const auto it = map4Byte.find(code);
                if (it != map4Byte.end()) {
                    result += it->second;
                    ++matchedUnits;
                }
            }
            if (matchedUnits > 0) {
                out += result;
                return true;
            }
        }

        return false;
    }
};

bool readNextHexToken(std::string_view text, size_t& pos, std::vector<uint8_t>& outBytes) {
    while (pos < text.size()) {
        if (text[pos] == '%') {
            while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') ++pos;
            continue;
        }
        if (text[pos] == '<' && (pos + 1 >= text.size() || text[pos + 1] != '<')) {
            const size_t close = text.find('>', pos + 1);
            if (close == std::string_view::npos) return false;
            outBytes = decodeHexBytes(text.substr(pos + 1, close - (pos + 1)));
            pos = close + 1;
            return true;
        }
        ++pos;
    }
    return false;
}

void parseToUnicodeCMap(std::string_view cmapText, PdfCMap& outCMap) {
    // 1. Parse beginbfchar ... endbfchar sections
    size_t searchPos = 0;
    while (true) {
        const size_t bfStart = cmapText.find("beginbfchar", searchPos);
        if (bfStart == std::string_view::npos) break;
        const size_t bfEnd = cmapText.find("endbfchar", bfStart + 11);
        if (bfEnd == std::string_view::npos) break;

        std::string_view body = cmapText.substr(bfStart + 11, bfEnd - (bfStart + 11));
        size_t cursor = 0;
        std::vector<uint8_t> srcBytes;
        std::vector<uint8_t> dstBytes;
        while (readNextHexToken(body, cursor, srcBytes)) {
            if (!readNextHexToken(body, cursor, dstBytes)) break;
            std::wstring target = decodeUtf16BeBytes(dstBytes.data(), dstBytes.size());
            outCMap.insertCode(srcBytes, std::move(target));
        }

        searchPos = bfEnd + 9;
    }

    // 2. Parse beginbfrange ... endbfrange sections
    searchPos = 0;
    while (true) {
        const size_t brStart = cmapText.find("beginbfrange", searchPos);
        if (brStart == std::string_view::npos) break;
        const size_t brEnd = cmapText.find("endbfrange", brStart + 12);
        if (brEnd == std::string_view::npos) break;

        std::string_view body = cmapText.substr(brStart + 12, brEnd - (brStart + 12));
        size_t cursor = 0;
        std::vector<uint8_t> startBytes;
        std::vector<uint8_t> endBytes;
        while (readNextHexToken(body, cursor, startBytes)) {
            if (!readNextHexToken(body, cursor, endBytes)) break;
            if (startBytes.empty() || startBytes.size() != endBytes.size() || startBytes.size() > 4) {
                continue;
            }

            uint32_t startCode = 0;
            uint32_t endCode = 0;
            for (size_t k = 0; k < startBytes.size(); ++k) {
                startCode = (startCode << 8) | startBytes[k];
                endCode = (endCode << 8) | endBytes[k];
            }
            if (endCode < startCode) continue;
            const uint32_t count = (std::min)(endCode - startCode + 1u, MaxBfRangeSpan);

            while (cursor < body.size() && isPdfWhitespace(static_cast<uint8_t>(body[cursor]))) {
                ++cursor;
            }
            if (cursor >= body.size()) break;

            if (body[cursor] == '[') {
                ++cursor;
                for (uint32_t offset = 0; offset < count; ++offset) {
                    while (cursor < body.size() && isPdfWhitespace(static_cast<uint8_t>(body[cursor]))) {
                        ++cursor;
                    }
                    if (cursor >= body.size() || body[cursor] == ']') {
                        if (cursor < body.size() && body[cursor] == ']') ++cursor;
                        break;
                    }
                    std::vector<uint8_t> itemDst;
                    if (!readNextHexToken(body, cursor, itemDst)) break;

                    const uint32_t curCode = startCode + offset;
                    std::vector<uint8_t> codeVec(startBytes.size());
                    for (size_t bIdx = 0; bIdx < startBytes.size(); ++bIdx) {
                        const size_t shift = (startBytes.size() - 1 - bIdx) * 8;
                        codeVec[bIdx] = static_cast<uint8_t>((curCode >> shift) & 0xFFu);
                    }
                    outCMap.insertCode(codeVec, decodeUtf16BeBytes(itemDst.data(), itemDst.size()));
                }
                while (cursor < body.size() && body[cursor] != ']') ++cursor;
                if (cursor < body.size() && body[cursor] == ']') ++cursor;
            } else {
                std::vector<uint8_t> baseDstBytes;
                if (!readNextHexToken(body, cursor, baseDstBytes)) break;
                const std::wstring baseStr = decodeUtf16BeBytes(baseDstBytes.data(), baseDstBytes.size());
                if (baseStr.empty()) continue;

                for (uint32_t offset = 0; offset < count; ++offset) {
                    const uint32_t curCode = startCode + offset;
                    std::vector<uint8_t> codeVec(startBytes.size());
                    for (size_t bIdx = 0; bIdx < startBytes.size(); ++bIdx) {
                        const size_t shift = (startBytes.size() - 1 - bIdx) * 8;
                        codeVec[bIdx] = static_cast<uint8_t>((curCode >> shift) & 0xFFu);
                    }
                    std::wstring mapped = baseStr;
                    incrementUnicodeString(mapped, offset);
                    outCMap.insertCode(codeVec, std::move(mapped));
                }
            }
        }

        searchPos = brEnd + 10;
    }
}

uint8_t paethPredictor(int a, int b, int c) noexcept {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

std::string undoPngPredictor(
    const std::string& raw,
    int columns,
    int colors,
    int bitsPerComponent
) {
    if (columns <= 0) columns = 1;
    if (colors <= 0) colors = 1;
    if (bitsPerComponent <= 0) bitsPerComponent = 8;

    const size_t bpp = static_cast<size_t>((std::max)(1, (colors * bitsPerComponent + 7) / 8));
    const size_t rowBytes = static_cast<size_t>((columns * colors * bitsPerComponent + 7) / 8);
    if (rowBytes == 0 || rowBytes > 1024 * 1024) return raw;
    const size_t stride = rowBytes + 1;
    if (raw.size() < stride) return raw;

    const size_t numRows = raw.size() / stride;
    std::string decoded(numRows * rowBytes, '\0');
    std::vector<uint8_t> prevRow(rowBytes, 0);
    std::vector<uint8_t> curRow(rowBytes, 0);

    for (size_t r = 0; r < numRows; ++r) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(raw.data() + r * stride);
        const uint8_t filterType = src[0];
        const uint8_t* rowIn = src + 1;

        for (size_t x = 0; x < rowBytes; ++x) {
            const uint8_t left = (x >= bpp) ? curRow[x - bpp] : 0;
            const uint8_t up = prevRow[x];
            const uint8_t upLeft = (x >= bpp) ? prevRow[x - bpp] : 0;

            switch (filterType) {
                case 0: // None
                    curRow[x] = rowIn[x];
                    break;
                case 1: // Sub
                    curRow[x] = static_cast<uint8_t>(rowIn[x] + left);
                    break;
                case 2: // Up
                    curRow[x] = static_cast<uint8_t>(rowIn[x] + up);
                    break;
                case 3: // Average
                    curRow[x] = static_cast<uint8_t>(rowIn[x] + ((static_cast<int>(left) + static_cast<int>(up)) / 2));
                    break;
                case 4: // Paeth
                    curRow[x] = static_cast<uint8_t>(rowIn[x] + paethPredictor(left, up, upLeft));
                    break;
                default:
                    // Malformed or non-PNG row: return original uncompressed stream unchanged
                    return raw;
            }
        }

        std::memcpy(decoded.data() + r * rowBytes, curRow.data(), rowBytes);
        prevRow.swap(curRow);
    }

    return decoded;
}

bool inflateBufferWithWindowBits(
    const uint8_t* compData,
    size_t compLen,
    int windowBits,
    size_t maxOutBytes,
    std::string& out
) {
    out.clear();
    if (!compData || compLen == 0 || maxOutBytes == 0) return false;

    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(compData);
    strm.avail_in = static_cast<uInt>((std::min<size_t>)(compLen, 0x7FFFFFFFu));
    if (inflateInit2(&strm, windowBits) != Z_OK) {
        return false;
    }

    std::vector<char> chunk(32768);
    int ret = Z_OK;
    while (out.size() < maxOutBytes) {
        const size_t remain = maxOutBytes - out.size();
        const size_t ask = (std::min)(chunk.size(), remain);
        strm.next_out = reinterpret_cast<Bytef*>(chunk.data());
        strm.avail_out = static_cast<uInt>(ask);

        ret = inflate(&strm, Z_NO_FLUSH);
        const size_t produced = ask - strm.avail_out;
        if (produced > 0) {
            out.append(chunk.data(), produced);
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
        if (produced == 0 && strm.avail_in == 0) {
            break;
        }
    }

    inflateEnd(&strm);
    return !out.empty();
}

int parseDictIntValue(std::string_view dict, std::string_view key, int defaultValue) noexcept {
    size_t searchFrom = 0;
    size_t pos = std::string_view::npos;
    while (searchFrom < dict.size()) {
        const size_t cand = dict.find(key, searchFrom);
        if (cand == std::string_view::npos) {
            return defaultValue;
        }
        if (isPdfTokenBoundaryAt(dict, cand + key.size())) {
            pos = cand;
            break;
        }
        searchFrom = cand + key.size();
    }
    if (pos == std::string_view::npos) return defaultValue;

    size_t i = pos + key.size();
    while (i < dict.size() && isPdfWhitespace(static_cast<uint8_t>(dict[i]))) ++i;
    if (i >= dict.size() || (!std::isdigit(static_cast<unsigned char>(dict[i])) && dict[i] != '-')) {
        return defaultValue;
    }
    int64_t sign = 1;
    if (dict[i] == '-') {
        sign = -1;
        ++i;
    }
    int64_t val = 0;
    bool any = false;
    while (i < dict.size() && std::isdigit(static_cast<unsigned char>(dict[i]))) {
        any = true;
        val = val * 10 + (dict[i] - '0');
        if (val > 0x7FFFFFFFLL) {
            return defaultValue;
        }
        ++i;
    }
    if (!any) return defaultValue;
    // Check whether this was actually an indirect object reference "N G R"
    size_t after = i;
    while (after < dict.size() && isPdfWhitespace(static_cast<uint8_t>(dict[after]))) ++after;
    if (after < dict.size() && std::isdigit(static_cast<unsigned char>(dict[after]))) {
        size_t genEnd = after;
        while (genEnd < dict.size() && std::isdigit(static_cast<unsigned char>(dict[genEnd]))) ++genEnd;
        size_t rPos = genEnd;
        while (rPos < dict.size() && isPdfWhitespace(static_cast<uint8_t>(dict[rPos]))) ++rPos;
        if (rPos < dict.size() && dict[rPos] == 'R' && isPdfTokenBoundaryAt(dict, rPos + 1)) {
            return defaultValue;
        }
    }
    return static_cast<int>(val * sign);
}

uint32_t parsePrecedingObjectId(std::string_view beforeDict) noexcept {
    // Search backwards in the ~128 bytes preceding "<<" for "N G obj"
    const size_t window = (std::min<size_t>)(beforeDict.size(), 160);
    std::string_view tail = beforeDict.substr(beforeDict.size() - window);
    const size_t objPos = tail.rfind("obj");
    if (objPos == std::string_view::npos) return 0;

    size_t i = objPos;
    while (i > 0 && isPdfWhitespace(static_cast<uint8_t>(tail[i - 1]))) --i;
    if (i == 0 || !std::isdigit(static_cast<unsigned char>(tail[i - 1]))) return 0;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(tail[i - 1]))) --i; // skip generation

    while (i > 0 && isPdfWhitespace(static_cast<uint8_t>(tail[i - 1]))) --i;
    if (i == 0 || !std::isdigit(static_cast<unsigned char>(tail[i - 1]))) return 0;
    const size_t numEnd = i;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(tail[i - 1]))) --i;

    uint32_t objId = 0;
    for (size_t k = i; k < numEnd; ++k) {
        objId = objId * 10u + static_cast<uint32_t>(tail[k] - '0');
    }
    return objId;
}

std::wstring decodeFallbackSingleBytePdfString(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return decodeUtf16BeBytes(bytes.data(), bytes.size());
    }

    int utf8Len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        nullptr, 0);
    if (utf8Len > 0) {
        std::wstring res(static_cast<size_t>(utf8Len), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            res.data(), utf8Len);
        return res;
    }

    const UINT cp = IsValidCodePage(936) ? 936u : CP_ACP;
    int gbkLen = MultiByteToWideChar(
        cp, 0,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        nullptr, 0);
    if (gbkLen > 0) {
        std::wstring res(static_cast<size_t>(gbkLen), L'\0');
        MultiByteToWideChar(
            cp, 0,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            res.data(), gbkLen);
        return res;
    }

    std::wstring latin;
    latin.reserve(bytes.size());
    for (uint8_t b : bytes) {
        if (b >= 0x20 || b == '\t' || b == '\n') {
            latin.push_back(static_cast<wchar_t>(b));
        }
    }
    return latin;
}

std::vector<uint8_t> readPdfLiteralStringBytes(std::string_view src, size_t& pos) {
    // Assumes src[pos] == '('
    std::vector<uint8_t> out;
    ++pos;
    int depth = 1;
    while (pos < src.size() && depth > 0) {
        const char ch = src[pos++];
        if (ch == '\\') {
            if (pos >= src.size()) break;
            const char esc = src[pos++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '(': out.push_back('('); break;
                case ')': out.push_back(')'); break;
                case '\\': out.push_back('\\'); break;
                case '\r':
                    if (pos < src.size() && src[pos] == '\n') ++pos;
                    break;
                case '\n':
                    break;
                default:
                    if (esc >= '0' && esc <= '7') {
                        uint32_t oct = static_cast<uint32_t>(esc - '0');
                        for (int d = 0; d < 2 && pos < src.size() && src[pos] >= '0' && src[pos] <= '7'; ++d) {
                            oct = (oct * 8u) + static_cast<uint32_t>(src[pos++] - '0');
                        }
                        out.push_back(static_cast<uint8_t>(oct & 0xFFu));
                    } else {
                        out.push_back(static_cast<uint8_t>(esc));
                    }
                    break;
            }
        } else if (ch == '(') {
            ++depth;
            out.push_back('(');
        } else if (ch == ')') {
            --depth;
            if (depth > 0) {
                out.push_back(')');
            }
        } else {
            out.push_back(static_cast<uint8_t>(ch));
        }
    }
    return out;
}

std::wstring decodePdfStringToken(
    const std::vector<uint8_t>& rawBytes,
    bool isHexToken,
    const PdfCMap* activeFontCMap,
    const PdfCMap& fallbackCMap
) {
    if (rawBytes.empty()) return {};

    if (rawBytes.size() >= 2 && rawBytes[0] == 0xFE && rawBytes[1] == 0xFF) {
        return decodeUtf16BeBytes(rawBytes.data(), rawBytes.size());
    }

    if (activeFontCMap && !activeFontCMap->empty()) {
        std::wstring mapped;
        if (activeFontCMap->decodeBytes(rawBytes, mapped) && !mapped.empty()) {
            return mapped;
        }
    }

    if (isHexToken && !fallbackCMap.empty()) {
        std::wstring mapped;
        if (fallbackCMap.decodeBytes(rawBytes, mapped) && !mapped.empty()) {
            return mapped;
        }
    }

    if (isHexToken && rawBytes.size() >= 2 && (rawBytes.size() % 2 == 0)) {
        // Many Identity-H / Unicode hex strings directly encode UTF-16BE code units
        bool looksUtf16Be = false;
        for (size_t i = 0; i + 1 < rawBytes.size(); i += 2) {
            const uint16_t u16 = (static_cast<uint16_t>(rawBytes[i]) << 8) | rawBytes[i + 1];
            if ((u16 >= 0x4E00u && u16 <= 0x9FFFu) || (rawBytes[i] == 0x00 && rawBytes[i + 1] >= 0x20)) {
                looksUtf16Be = true;
                break;
            }
        }
        if (looksUtf16Be) {
            return decodeUtf16BeBytes(rawBytes.data(), rawBytes.size());
        }
    }

    return decodeFallbackSingleBytePdfString(rawBytes);
}

void parsePdfContentStream(
    std::string_view content,
    const std::unordered_map<std::string, uint32_t>* scopedFontNameToCMapObj,
    const std::unordered_map<std::string, uint32_t>& fallbackFontNameToCMapObj,
    const std::unordered_map<uint32_t, PdfCMap>& cmapsByObj,
    const PdfCMap& fallbackCMap,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    if (content.find("Tj") == std::string_view::npos &&
        content.find("TJ") == std::string_view::npos &&
        content.find("BT") == std::string_view::npos) {
        return;
    }

    struct ArrayElement {
        bool isNumber = false;
        double number = 0.0;
        std::wstring text;
    };

    const PdfCMap* activeCMap = nullptr;
    std::string pendingName;
    std::vector<double> numStack;
    std::wstring lastStringOperand;
    std::vector<ArrayElement> lastArrayOperand;
    bool hasLastMatrixY = false;
    double lastMatrixY = 0.0;

    auto emitNewLine = [&]() {
        if (!outText.empty() && outText.back() != L'\n' && outText.size() < MaxPdfExtractedChars) {
            outText.push_back(L'\n');
        }
    };

    auto appendSafeText = [&](std::wstring_view str) {
        for (wchar_t wc : str) {
            if (outText.size() >= MaxPdfExtractedChars) break;
            if (wc == L'\r') {
                emitNewLine();
            } else if (wc != L'\0') {
                outText.push_back(wc);
            }
        }
    };

    size_t pos = 0;
    size_t iterCount = 0;
    while (pos < content.size() && outText.size() < MaxPdfExtractedChars) {
        if (((++iterCount & 0xFFFu) == 0) && isCancelled && isCancelled()) {
            return;
        }

        const uint8_t ch = static_cast<uint8_t>(content[pos]);
        if (isPdfWhitespace(ch)) {
            ++pos;
            continue;
        }
        if (ch == '%') {
            while (pos < content.size() && content[pos] != '\n' && content[pos] != '\r') ++pos;
            continue;
        }
        if (ch == '/') {
            ++pos;
            const size_t start = pos;
            while (pos < content.size() &&
                   !isPdfWhitespace(static_cast<uint8_t>(content[pos])) &&
                   !isPdfDelimiter(static_cast<uint8_t>(content[pos]))) {
                ++pos;
            }
            pendingName.assign(content.substr(start, pos - start));
            continue;
        }
        if (ch == '(') {
            const auto rawBytes = readPdfLiteralStringBytes(content, pos);
            lastStringOperand = decodePdfStringToken(rawBytes, false, activeCMap, fallbackCMap);
            continue;
        }
        if (ch == '<') {
            if (pos + 1 < content.size() && content[pos + 1] == '<') {
                pos += 2;
                continue;
            }
            const size_t close = content.find('>', pos + 1);
            if (close == std::string_view::npos) break;
            const auto rawBytes = decodeHexBytes(content.substr(pos + 1, close - (pos + 1)));
            pos = close + 1;
            lastStringOperand = decodePdfStringToken(rawBytes, true, activeCMap, fallbackCMap);
            continue;
        }
        if (ch == '>' && pos + 1 < content.size() && content[pos + 1] == '>') {
            pos += 2;
            continue;
        }
        if (ch == '[') {
            ++pos;
            lastArrayOperand.clear();
            while (pos < content.size()) {
                const uint8_t ac = static_cast<uint8_t>(content[pos]);
                if (isPdfWhitespace(ac)) {
                    ++pos;
                    continue;
                }
                if (ac == '%') {
                    while (pos < content.size() && content[pos] != '\n' && content[pos] != '\r') ++pos;
                    continue;
                }
                if (ac == ']') {
                    ++pos;
                    break;
                }
                if (ac == '(') {
                    const auto rawBytes = readPdfLiteralStringBytes(content, pos);
                    lastArrayOperand.push_back(ArrayElement{
                        false, 0.0, decodePdfStringToken(rawBytes, false, activeCMap, fallbackCMap)
                    });
                    continue;
                }
                if (ac == '<' && (pos + 1 >= content.size() || content[pos + 1] != '<')) {
                    const size_t close = content.find('>', pos + 1);
                    if (close == std::string_view::npos) break;
                    const auto rawBytes = decodeHexBytes(content.substr(pos + 1, close - (pos + 1)));
                    pos = close + 1;
                    lastArrayOperand.push_back(ArrayElement{
                        false, 0.0, decodePdfStringToken(rawBytes, true, activeCMap, fallbackCMap)
                    });
                    continue;
                }
                if (std::isdigit(ac) || ac == '-' || ac == '+' || ac == '.') {
                    const size_t numStart = pos++;
                    while (pos < content.size() &&
                           (std::isdigit(static_cast<unsigned char>(content[pos])) || content[pos] == '.')) {
                        ++pos;
                    }
                    std::string numStr(content.substr(numStart, pos - numStart));
                    char* endPtr = nullptr;
                    const double val = std::strtod(numStr.c_str(), &endPtr);
                    lastArrayOperand.push_back(ArrayElement{true, val, {}});
                    continue;
                }
                ++pos;
            }
            continue;
        }

        if (std::isdigit(ch) || ch == '-' || ch == '+' || ch == '.') {
            const size_t numStart = pos++;
            while (pos < content.size() &&
                   (std::isdigit(static_cast<unsigned char>(content[pos])) || content[pos] == '.')) {
                ++pos;
            }
            std::string numStr(content.substr(numStart, pos - numStart));
            char* endPtr = nullptr;
            const double val = std::strtod(numStr.c_str(), &endPtr);
            if (endPtr != numStr.c_str()) {
                if (numStack.size() >= 16) numStack.erase(numStack.begin());
                numStack.push_back(val);
                continue;
            }
            pos = numStart;
        }

        // Operator or keyword token
        const size_t opStart = pos;
        if (ch == '\'' || ch == '"') {
            ++pos;
        } else {
            while (pos < content.size() &&
                   !isPdfWhitespace(static_cast<uint8_t>(content[pos])) &&
                   !isPdfDelimiter(static_cast<uint8_t>(content[pos]))) {
                ++pos;
            }
        }
        std::string_view op = content.substr(opStart, pos - opStart);

        if (op == "Tf") {
            if (!pendingName.empty()) {
                uint32_t cmapObj = 0;
                if (scopedFontNameToCMapObj) {
                    const auto sIt = scopedFontNameToCMapObj->find(pendingName);
                    if (sIt != scopedFontNameToCMapObj->end()) {
                        cmapObj = sIt->second;
                    }
                }
                if (cmapObj == 0) {
                    const auto fIt = fallbackFontNameToCMapObj.find(pendingName);
                    if (fIt != fallbackFontNameToCMapObj.end()) {
                        cmapObj = fIt->second;
                    }
                }
                if (cmapObj > 0) {
                    const auto cIt = cmapsByObj.find(cmapObj);
                    activeCMap = (cIt != cmapsByObj.end()) ? &(cIt->second) : nullptr;
                } else {
                    activeCMap = nullptr;
                }
            }
            numStack.clear();
        } else if (op == "BT") {
            numStack.clear();
        } else if (op == "ET") {
            numStack.clear();
        } else if (op == "T*") {
            emitNewLine();
            numStack.clear();
        } else if (op == "Td" || op == "TD") {
            if (numStack.size() >= 2) {
                const double tx = numStack[numStack.size() - 2];
                const double ty = numStack[numStack.size() - 1];
                if (std::abs(ty) > 0.25) {
                    emitNewLine();
                    if (hasLastMatrixY) {
                        lastMatrixY += ty;
                    }
                } else if (tx > 8.0 && !outText.empty() &&
                           outText.back() != L'\n' && outText.back() != L' ') {
                    outText.push_back(L' ');
                }
            }
            numStack.clear();
        } else if (op == "Tm") {
            if (numStack.size() >= 6) {
                const double newY = numStack.back();
                if (hasLastMatrixY && std::abs(newY - lastMatrixY) > 1.5) {
                    emitNewLine();
                }
                lastMatrixY = newY;
                hasLastMatrixY = true;
            }
            numStack.clear();
        } else if (op == "Tj") {
            appendSafeText(lastStringOperand);
            lastStringOperand.clear();
            numStack.clear();
        } else if (op == "'" || op == "\"") {
            emitNewLine();
            appendSafeText(lastStringOperand);
            lastStringOperand.clear();
            numStack.clear();
        } else if (op == "TJ") {
            for (size_t idx = 0; idx < lastArrayOperand.size(); ++idx) {
                const auto& elem = lastArrayOperand[idx];
                if (elem.isNumber) {
                    if (elem.number < -180.0 && !outText.empty() &&
                        outText.back() != L' ' && outText.back() != L'\n') {
                        const wchar_t prevCh = outText.back();
                        if (prevCh < 0x3000 || elem.number < -600.0) {
                            outText.push_back(L' ');
                        }
                    }
                } else if (!elem.text.empty()) {
                    appendSafeText(elem.text);
                }
            }
            lastArrayOperand.clear();
            numStack.clear();
        } else {
            numStack.clear();
        }
    }

    if (!outText.empty() && outText.back() != L'\n' && outText.size() < MaxPdfExtractedChars) {
        outText.push_back(L'\n');
    }
}

bool parseIndirectObjRef(std::string_view src, size_t& cursor, uint32_t& outObjId) noexcept {
    outObjId = 0;
    size_t p = cursor;
    while (p < src.size() && isPdfWhitespace(static_cast<uint8_t>(src[p]))) ++p;
    if (p >= src.size() || !std::isdigit(static_cast<unsigned char>(src[p]))) return false;
    uint64_t objNum = 0;
    while (p < src.size() && std::isdigit(static_cast<unsigned char>(src[p]))) {
        objNum = objNum * 10u + static_cast<uint64_t>(src[p++] - '0');
        if (objNum > 0xFFFFFFFFULL) return false;
    }
    if (objNum == 0) return false;
    while (p < src.size() && isPdfWhitespace(static_cast<uint8_t>(src[p]))) ++p;
    if (p >= src.size() || !std::isdigit(static_cast<unsigned char>(src[p]))) return false;
    while (p < src.size() && std::isdigit(static_cast<unsigned char>(src[p]))) ++p;
    while (p < src.size() && isPdfWhitespace(static_cast<uint8_t>(src[p]))) ++p;
    if (p >= src.size() || src[p] != 'R' || !isPdfTokenBoundaryAt(src, p + 1)) return false;
    cursor = p + 1;
    outObjId = static_cast<uint32_t>(objNum);
    return true;
}

void parseFontResourceDictEntries(
    std::string_view fontDict,
    uint32_t ownerObj,
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>>& ownerObjToFontMap,
    std::unordered_map<std::string, uint32_t>& fallbackFontNameToFontObj
) {
    size_t k = 0;
    while (k < fontDict.size()) {
        if (fontDict[k] == '/') {
            ++k;
            const size_t ns = k;
            while (k < fontDict.size() &&
                   !isPdfWhitespace(static_cast<uint8_t>(fontDict[k])) &&
                   !isPdfDelimiter(static_cast<uint8_t>(fontDict[k]))) {
                ++k;
            }
            std::string fname(fontDict.substr(ns, k - ns));
            uint32_t fObj = 0;
            if (!fname.empty() && parseIndirectObjRef(fontDict, k, fObj)) {
                if (ownerObj > 0) {
                    ownerObjToFontMap[ownerObj][fname] = fObj;
                }
                fallbackFontNameToFontObj.try_emplace(fname, fObj);
            }
        } else {
            ++k;
        }
    }
}

void harvestFontToUnicodeLinks(
    std::string_view rawPdfOrObjStream,
    std::unordered_map<uint32_t, uint32_t>& fontObjToToUnicodeObj,
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>>& ownerObjToFontMap,
    std::unordered_map<uint32_t, uint32_t>& streamObjToResourceOwnerObj,
    std::unordered_map<uint32_t, uint32_t>& resourceOwnerToFontDictObj,
    std::unordered_map<std::string, uint32_t>& fallbackFontNameToFontObj
) {
    // 1. Scan indirect Font objects containing "/ToUnicode M G R"
    size_t pos = 0;
    while (true) {
        const size_t tuPos = rawPdfOrObjStream.find("/ToUnicode", pos);
        if (tuPos == std::string_view::npos) break;
        if (isPdfTokenBoundaryAt(rawPdfOrObjStream, tuPos + 10)) {
            size_t p = tuPos + 10;
            uint32_t targetObj = 0;
            if (parseIndirectObjRef(rawPdfOrObjStream, p, targetObj)) {
                const uint32_t ownerObj = parsePrecedingObjectId(rawPdfOrObjStream.substr(0, tuPos));
                if (ownerObj > 0) {
                    fontObjToToUnicodeObj[ownerObj] = targetObj;
                }
            }
        }
        pos = tuPos + 10;
    }

    // 2. Scan "/Font" resource dictionaries ("<< /F1 K 0 R >>" or indirect "/Font M 0 R")
    pos = 0;
    while (true) {
        const size_t fPos = rawPdfOrObjStream.find("/Font", pos);
        if (fPos == std::string_view::npos) break;
        if (isPdfTokenBoundaryAt(rawPdfOrObjStream, fPos + 5)) {
            const uint32_t ownerObj = parsePrecedingObjectId(rawPdfOrObjStream.substr(0, fPos));
            size_t p = fPos + 5;
            while (p < rawPdfOrObjStream.size() && isPdfWhitespace(static_cast<uint8_t>(rawPdfOrObjStream[p]))) ++p;
            if (p + 1 < rawPdfOrObjStream.size() && rawPdfOrObjStream[p] == '<' && rawPdfOrObjStream[p + 1] == '<') {
                p += 2;
                const size_t closeDict = rawPdfOrObjStream.find(">>", p);
                if (closeDict != std::string_view::npos && closeDict - p < 8192) {
                    parseFontResourceDictEntries(
                        rawPdfOrObjStream.substr(p, closeDict - p),
                        ownerObj,
                        ownerObjToFontMap,
                        fallbackFontNameToFontObj
                    );
                }
            } else if (ownerObj > 0) {
                uint32_t indirectFontDictObj = 0;
                if (parseIndirectObjRef(rawPdfOrObjStream, p, indirectFontDictObj)) {
                    resourceOwnerToFontDictObj[ownerObj] = indirectFontDictObj;
                }
            }
        }
        pos = fPos + 5;
    }

    // 2b. If any "/Font M 0 R" pointed at a standalone dictionary object "M 0 obj << /F1 K 0 R >>",
    //     harvest that indirect font dictionary into ownerObjToFontMap[M].
    if (!resourceOwnerToFontDictObj.empty()) {
        for (const auto& [resOwner, fontDictObj] : resourceOwnerToFontDictObj) {
            (void)resOwner;
            if (ownerObjToFontMap.find(fontDictObj) != ownerObjToFontMap.end()) continue;
            const std::string needle = std::to_string(fontDictObj) + " 0 obj";
            size_t searchFrom = 0;
            while (searchFrom < rawPdfOrObjStream.size()) {
                const size_t objPos = rawPdfOrObjStream.find(needle, searchFrom);
                if (objPos == std::string_view::npos) break;
                const bool leftBoundary =
                    (objPos == 0) || !std::isdigit(static_cast<unsigned char>(rawPdfOrObjStream[objPos - 1]));
                if (leftBoundary) {
                    const size_t afterObj = objPos + needle.size();
                    const size_t endObj = rawPdfOrObjStream.find("endobj", afterObj);
                    const size_t limit = (endObj != std::string_view::npos)
                        ? (std::min)(endObj, afterObj + 8192)
                        : (std::min)(rawPdfOrObjStream.size(), afterObj + 8192);
                    const std::string_view slice = rawPdfOrObjStream.substr(afterObj, limit - afterObj);
                    const size_t openDict = slice.find("<<");
                    const size_t closeDict = (openDict != std::string_view::npos)
                        ? slice.find(">>", openDict + 2)
                        : std::string_view::npos;
                    if (openDict != std::string_view::npos && closeDict != std::string_view::npos) {
                        parseFontResourceDictEntries(
                            slice.substr(openDict + 2, closeDict - (openDict + 2)),
                            fontDictObj,
                            ownerObjToFontMap,
                            fallbackFontNameToFontObj
                        );
                    }
                    break;
                }
                searchFrom = objPos + needle.size();
            }
        }
    }

    // 3. Associate Page "/Contents" stream object IDs with their enclosing Page / "/Resources" owner
    pos = 0;
    while (true) {
        const size_t cPos = rawPdfOrObjStream.find("/Contents", pos);
        if (cPos == std::string_view::npos) break;
        if (isPdfTokenBoundaryAt(rawPdfOrObjStream, cPos + 9)) {
            const uint32_t pageObj = parsePrecedingObjectId(rawPdfOrObjStream.substr(0, cPos));
            // Locate enclosing object bounds to inspect "/Resources N G R" if indirect
            const size_t lookback = (cPos > 4096) ? (cPos - 4096) : 0;
            const std::string_view beforeSlice = rawPdfOrObjStream.substr(lookback, cPos - lookback);
            const size_t objKw = beforeSlice.rfind("obj");
            const size_t objBodyStart = (objKw != std::string_view::npos) ? (lookback + objKw + 3) : lookback;
            const size_t endObjKw = rawPdfOrObjStream.find("endobj", cPos + 9);
            const size_t objBodyEnd = (endObjKw != std::string_view::npos)
                ? (std::min)(endObjKw, cPos + 4096)
                : (std::min)(rawPdfOrObjStream.size(), cPos + 4096);
            const std::string_view pageBody =
                rawPdfOrObjStream.substr(objBodyStart, objBodyEnd - objBodyStart);

            uint32_t resourceOwner = pageObj;
            size_t rSearch = 0;
            while (rSearch < pageBody.size()) {
                const size_t rPos = pageBody.find("/Resources", rSearch);
                if (rPos == std::string_view::npos) break;
                if (isPdfTokenBoundaryAt(pageBody, rPos + 10)) {
                    size_t rp = rPos + 10;
                    uint32_t indirectResObj = 0;
                    if (parseIndirectObjRef(pageBody, rp, indirectResObj)) {
                        resourceOwner = indirectResObj;
                    }
                    break;
                }
                rSearch = rPos + 10;
            }

            if (resourceOwner > 0) {
                size_t p = cPos + 9;
                while (p < rawPdfOrObjStream.size() &&
                       isPdfWhitespace(static_cast<uint8_t>(rawPdfOrObjStream[p]))) {
                    ++p;
                }
                if (p < rawPdfOrObjStream.size() && rawPdfOrObjStream[p] == '[') {
                    ++p;
                    const size_t closeBracket = rawPdfOrObjStream.find(']', p);
                    const size_t arrLimit = (closeBracket != std::string_view::npos)
                        ? (std::min)(closeBracket, p + 4096)
                        : (std::min)(rawPdfOrObjStream.size(), p + 4096);
                    std::string_view arrView = rawPdfOrObjStream.substr(p, arrLimit - p);
                    size_t ap = 0;
                    while (ap < arrView.size()) {
                        uint32_t streamObj = 0;
                        if (parseIndirectObjRef(arrView, ap, streamObj)) {
                            streamObjToResourceOwnerObj[streamObj] = resourceOwner;
                        } else {
                            ++ap;
                        }
                    }
                } else {
                    uint32_t streamObj = 0;
                    if (parseIndirectObjRef(rawPdfOrObjStream, p, streamObj)) {
                        streamObjToResourceOwnerObj[streamObj] = resourceOwner;
                    }
                }
            }
        }
        pos = cPos + 9;
    }
}

bool hasReadablePdfText(std::wstring_view text) noexcept {
    for (wchar_t ch : text) {
        if (ch > 0x20 && ch != 0xFFFD) {
            return true;
        }
    }
    return false;
}

} // namespace

bool PdfExtractor::canHandle(std::wstring_view extension) const {
    return ContentFormatFilter::normalizeExt(extension) == L"pdf";
}

bool PdfExtractor::extractPdfTextFromMemory(
    const uint8_t* data,
    size_t size,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    outText.clear();
    if (!data || size < 16) return false;

    std::string_view pdfView(reinterpret_cast<const char*>(data), size);
    if (pdfView.substr(0, (std::min<size_t>)(size, 1024)).find("%PDF-") == std::string_view::npos) {
        return false;
    }

    struct DecodedStreamEntry {
        uint32_t objId = 0;
        bool isCMap = false;
        bool isObjStm = false;
        std::string payload;
    };

    std::unordered_map<uint32_t, uint32_t> fontObjToToUnicodeObj;
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> ownerObjToFontMap;
    std::unordered_map<uint32_t, uint32_t> streamObjToResourceOwnerObj;
    std::unordered_map<uint32_t, uint32_t> resourceOwnerToFontDictObj;
    std::unordered_map<std::string, uint32_t> fallbackFontNameToFontObj;
    harvestFontToUnicodeLinks(
        pdfView,
        fontObjToToUnicodeObj,
        ownerObjToFontMap,
        streamObjToResourceOwnerObj,
        resourceOwnerToFontDictObj,
        fallbackFontNameToFontObj
    );

    std::vector<DecodedStreamEntry> decodedStreams;
    size_t totalDecompressedBytes = 0;
    size_t searchPos = 0;

    while (searchPos < size && totalDecompressedBytes < MaxTotalPdfDecompressedBytes) {
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        const size_t streamKeyword = pdfView.find("stream", searchPos);
        if (streamKeyword == std::string_view::npos) break;

        // Ensure "stream" is a standalone keyword, not "endstream" or "/Bitstream"
        if (streamKeyword > 0) {
            const char prev = pdfView[streamKeyword - 1];
            if (std::isalpha(static_cast<unsigned char>(prev)) || prev == '/') {
                searchPos = streamKeyword + 6;
                continue;
            }
        }

        size_t dataStart = streamKeyword + 6;
        // PDF specification: "stream" is followed by CRLF or LF (tolerate trailing spaces before EOL)
        while (dataStart < size && (pdfView[dataStart] == ' ' || pdfView[dataStart] == '\t')) {
            ++dataStart;
        }
        if (dataStart < size && pdfView[dataStart] == '\r') {
            ++dataStart;
            if (dataStart < size && pdfView[dataStart] == '\n') {
                ++dataStart;
            }
        } else if (dataStart < size && pdfView[dataStart] == '\n') {
            ++dataStart;
        } else {
            searchPos = streamKeyword + 6;
            continue;
        }

        // Locate preceding dictionary "<< ... >>"
        const size_t dictLookbackStart = (streamKeyword > 4096) ? (streamKeyword - 4096) : 0;
        std::string_view headerSlice = pdfView.substr(dictLookbackStart, streamKeyword - dictLookbackStart);
        const size_t dictClose = headerSlice.rfind(">>");
        if (dictClose == std::string_view::npos) {
            searchPos = dataStart;
            continue;
        }
        // Find matching outer "<<" before dictClose
        int dictDepth = 0;
        size_t dictOpen = std::string_view::npos;
        for (size_t k = dictClose + 2; k >= 2; --k) {
            if (headerSlice[k - 1] == '>' && headerSlice[k - 2] == '>') {
                ++dictDepth;
                --k;
            } else if (headerSlice[k - 1] == '<' && headerSlice[k - 2] == '<') {
                --dictDepth;
                if (dictDepth == 0) {
                    dictOpen = k - 2;
                    break;
                }
                --k;
            }
            if (k == 2) break;
        }
        if (dictOpen == std::string_view::npos) {
            searchPos = dataStart;
            continue;
        }

        const std::string_view dictView = headerSlice.substr(dictOpen, (dictClose + 2) - dictOpen);
        const uint32_t objId = parsePrecedingObjectId(pdfView.substr(0, dictLookbackStart + dictOpen));

        // Skip obvious image/font binary streams early to save CPU & memory
        if (dictView.find("/Subtype /Image") != std::string_view::npos ||
            dictView.find("/Subtype/Image") != std::string_view::npos ||
            hasPdfNameToken(dictView, "/FontFile") ||
            hasPdfNameToken(dictView, "/FontFile2") ||
            hasPdfNameToken(dictView, "/FontFile3") ||
            dictView.find("/Subtype /XML") != std::string_view::npos) {
            const size_t nextEnd = pdfView.find("endstream", dataStart);
            searchPos = (nextEnd != std::string_view::npos) ? (nextEnd + 9) : size;
            continue;
        }

        // Determine stream byte slice
        size_t streamLen = 0;
        size_t afterEndStream = std::string_view::npos;
        const int explicitLength = parseDictIntValue(dictView, "/Length", -1);
        if (explicitLength >= 0 && dataStart + static_cast<size_t>(explicitLength) <= size) {
            const size_t candidateEnd = dataStart + static_cast<size_t>(explicitLength);
            const std::string_view tailProbe = pdfView.substr(
                candidateEnd, (std::min<size_t>)(32, size - candidateEnd));
            const size_t esRel = tailProbe.find("endstream");
            if (esRel != std::string_view::npos) {
                streamLen = static_cast<size_t>(explicitLength);
                afterEndStream = candidateEnd + esRel + 9;
            }
        }
        if (afterEndStream == std::string_view::npos) {
            const size_t esAbs = pdfView.find("endstream", dataStart);
            if (esAbs == std::string_view::npos) break;
            size_t endTrim = esAbs;
            if (endTrim > dataStart && pdfView[endTrim - 1] == '\n') --endTrim;
            if (endTrim > dataStart && pdfView[endTrim - 1] == '\r') --endTrim;
            streamLen = endTrim - dataStart;
            afterEndStream = esAbs + 9;
        }

        searchPos = afterEndStream;
        if (streamLen == 0) continue;

        const bool hasFlate =
            hasPdfNameToken(dictView, "/FlateDecode") ||
            hasPdfNameToken(dictView, "/Fl");
        const bool hasUnsupportedFilter =
            !hasFlate && hasPdfNameToken(dictView, "/Filter");
        if (hasUnsupportedFilter) {
            continue;
        }

        const size_t maxAllowed = (std::min)(
            MaxSingleStreamDecompressedBytes,
            MaxTotalPdfDecompressedBytes - totalDecompressedBytes
        );
        std::string payload;
        if (hasFlate) {
            const uint8_t* rawStart = data + dataStart;
            if (!inflateBufferWithWindowBits(rawStart, streamLen, MAX_WBITS, maxAllowed, payload) &&
                !inflateBufferWithWindowBits(rawStart, streamLen, -MAX_WBITS, maxAllowed, payload)) {
                const uint8_t* compPtr = rawStart;
                size_t compRemain = streamLen;
                while (compRemain > 0 && isPdfWhitespace(*compPtr)) {
                    ++compPtr;
                    --compRemain;
                }
                if (compRemain > 0 && compRemain != streamLen) {
                    if (!inflateBufferWithWindowBits(compPtr, compRemain, MAX_WBITS, maxAllowed, payload)) {
                        inflateBufferWithWindowBits(compPtr, compRemain, -MAX_WBITS, maxAllowed, payload);
                    }
                }
            }
            if (!payload.empty()) {
                const int predictor = parseDictIntValue(dictView, "/Predictor", 1);
                if (predictor >= 10) {
                    const int columns = parseDictIntValue(dictView, "/Columns", 1);
                    const int colors = parseDictIntValue(dictView, "/Colors", 1);
                    const int bpc = parseDictIntValue(dictView, "/BitsPerComponent", 8);
                    payload = undoPngPredictor(payload, columns, colors, bpc);
                }
            }
        } else {
            const size_t take = (std::min)(streamLen, maxAllowed);
            payload.assign(reinterpret_cast<const char*>(data + dataStart), take);
        }

        if (payload.empty()) continue;
        totalDecompressedBytes += payload.size();

        const bool isObjStm = hasPdfNameToken(dictView, "/ObjStm");
        const bool isCMap =
            payload.find("begincmap") != std::string::npos ||
            payload.find("beginbfchar") != std::string::npos ||
            payload.find("beginbfrange") != std::string::npos;

        if (isObjStm) {
            harvestFontToUnicodeLinks(
                payload,
                fontObjToToUnicodeObj,
                ownerObjToFontMap,
                streamObjToResourceOwnerObj,
                resourceOwnerToFontDictObj,
                fallbackFontNameToFontObj
            );
        }

        decodedStreams.push_back(DecodedStreamEntry{
            .objId = objId,
            .isCMap = isCMap,
            .isObjStm = isObjStm,
            .payload = std::move(payload)
        });
    }

    // Pass 1: Build per-object CMap tables and fallback merged CMap
    std::unordered_map<uint32_t, PdfCMap> cmapsByObj;
    PdfCMap mergedFallbackCMap;
    for (const auto& entry : decodedStreams) {
        if (!entry.isCMap) continue;
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }
        PdfCMap cmap;
        parseToUnicodeCMap(entry.payload, cmap);
        if (!cmap.empty()) {
            mergedFallbackCMap.mergeFrom(cmap);
            if (entry.objId > 0) {
                cmapsByObj[entry.objId] = std::move(cmap);
            }
        }
    }

    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> ownerObjToFontCMap;
    for (const auto& [ownerObj, fontMap] : ownerObjToFontMap) {
        auto& resolved = ownerObjToFontCMap[ownerObj];
        for (const auto& [fontName, fontObj] : fontMap) {
            const auto it = fontObjToToUnicodeObj.find(fontObj);
            if (it != fontObjToToUnicodeObj.end()) {
                resolved[fontName] = it->second;
            }
        }
    }

    std::unordered_map<std::string, uint32_t> fallbackFontNameToCMapObj;
    for (const auto& [fontName, fontObj] : fallbackFontNameToFontObj) {
        const auto it = fontObjToToUnicodeObj.find(fontObj);
        if (it != fontObjToToUnicodeObj.end()) {
            fallbackFontNameToCMapObj[fontName] = it->second;
        }
    }

    // Pass 2: Extract text from content streams with per-page /Resources scoping
    for (const auto& entry : decodedStreams) {
        if (entry.isCMap || entry.isObjStm) continue;
        if (isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        const std::unordered_map<std::string, uint32_t>* scopedMap = nullptr;
        if (entry.objId > 0) {
            uint32_t targetOwner = entry.objId;
            const auto sIt = streamObjToResourceOwnerObj.find(entry.objId);
            if (sIt != streamObjToResourceOwnerObj.end()) {
                targetOwner = sIt->second;
            }
            const auto indIt = resourceOwnerToFontDictObj.find(targetOwner);
            if (indIt != resourceOwnerToFontDictObj.end()) {
                targetOwner = indIt->second;
            }
            const auto mIt = ownerObjToFontCMap.find(targetOwner);
            if (mIt != ownerObjToFontCMap.end() && !mIt->second.empty()) {
                scopedMap = &(mIt->second);
            }
        }

        parsePdfContentStream(
            entry.payload,
            scopedMap,
            fallbackFontNameToCMapObj,
            cmapsByObj,
            mergedFallbackCMap,
            outText,
            isCancelled
        );
        if (outText.size() >= MaxPdfExtractedChars) break;
    }

    return hasReadablePdfText(outText);
}

bool PdfExtractor::searchContent(
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

bool PdfExtractor::searchContentTerms(
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
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    const FileCacheStamp stamp = queryFileCacheStampByHandle(hFile);
    if (!stamp.valid || stamp.fileSize == 0 || stamp.fileSize > MaxPdfFileBytes) {
        return false;
    }

    std::wstring extractedText;
    if (!ContentTextCache::instance().tryGet(filePath, stamp.fileSize, stamp.lastWriteTime, extractedText)) {
        const size_t mapSize = static_cast<size_t>(stamp.fileSize);
        HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        bool extracted = false;
        if (hMapping) {
            HandleCloser mapCloser{hMapping};
            const auto* pData = static_cast<const uint8_t*>(
                MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, mapSize));
            if (pData) {
                struct ViewGuard {
                    const void* ptr;
                    ~ViewGuard() {
                        if (ptr) UnmapViewOfFile(ptr);
                    }
                } viewGuard{pData};

                extracted = runWithMappedViewGuard([&]() -> bool {
                    return extractPdfTextFromMemory(pData, mapSize, extractedText, isCancelled);
                });
                if (!extracted) {
                    extractedText.clear();
                }

                const auto needsSystemPdfFilterFallback = [&]() noexcept -> bool {
                    if (extracted && !extractedText.empty()) return false;
                    const std::string_view rawView(reinterpret_cast<const char*>(pData), mapSize);
                    if (rawView.substr(0, (std::min<size_t>)(1024, mapSize)).find("%PDF-") == std::string_view::npos) {
                        return true;
                    }
                    return rawView.find("/Encrypt") != std::string_view::npos ||
                           rawView.find("/LZWDecode") != std::string_view::npos ||
                           rawView.find("/ASCII85Decode") != std::string_view::npos ||
                           rawView.find("/ASCIIHexDecode") != std::string_view::npos;
                };
                if (!(isCancelled && isCancelled()) && needsSystemPdfFilterFallback()) {
                    // Secondary fallback: delegate to Windows system PDF IFilter over our already-opened view/handle
                    extracted = WindowsIFilterExtractor::extractViaSystemIFilterFromHandle(
                        hFile, filePath, pData, mapSize, extractedText, isCancelled);
                }
            } else {
                extracted = WindowsIFilterExtractor::extractViaSystemIFilterFromHandle(
                    hFile, filePath, nullptr, 0, extractedText, isCancelled);
            }
        } else {
            extracted = WindowsIFilterExtractor::extractViaSystemIFilterFromHandle(
                hFile, filePath, nullptr, 0, extractedText, isCancelled);
        }

        if (isCancelled && isCancelled()) {
            return false;
        }

        if (!extracted) {
            extractedText.clear();
        }
        ContentTextCache::instance().put(filePath, stamp.fileSize, stamp.lastWriteTime, extractedText);
    }

    if (extractedText.empty()) {
        return false;
    }

    return matchDocumentTextAndBuildSnippets(
        extractedText,
        queryTerms,
        caseSensitive,
        outSnippets,
        maxSnippetsPerFile,
        isCancelled
    );
}

} // namespace tools3000::service::content
