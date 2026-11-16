#include "core/utils/DesignAndCadPreview.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ole2.h>
#include <shobjidl_core.h>
#include <wincodec.h>
#include <zlib.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tools3000::core {

namespace {

constexpr uint64_t kMaxPsdFullLoadBytes = 128ULL * 1024ULL * 1024ULL;
constexpr size_t kPsdFallbackPrefixBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxCdrArchiveBytes = 96ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxCadScanBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxDxfBytes = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxMeshBytes = 96ULL * 1024ULL * 1024ULL;
constexpr size_t kMaxZipEntryUncompressedBytes = 32ULL * 1024ULL * 1024ULL;
constexpr double kPi = 3.14159265358979323846;

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr uint8_t kDwgPreviewSentinel[16] = {
    0x1F, 0x25, 0x6D, 0x07, 0xD4, 0x36, 0x28, 0x28,
    0x9D, 0x57, 0xCA, 0x3F, 0x9D, 0x44, 0x10, 0x2B
};

// ── Endianness Safe Readers & Writers ────────────────────────────────────────

uint16_t readBe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

uint32_t readBe32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t readBe64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(readBe32(p)) << 32) | static_cast<uint64_t>(readBe32(p + 4));
}

uint16_t readLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLeI32(const uint8_t* p) noexcept {
    return static_cast<int32_t>(readLe32(p));
}

void appendBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void writeLe16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void writeLe32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

std::string asciiToUpper(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char c : sv) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// ── Zero-Lock File Readers ───────────────────────────────────────────────────

bool readFileZeroLock(
    const std::wstring& filePath,
    uint64_t maxFullBytes,
    size_t fallbackPrefixBytes,
    std::vector<uint8_t>& outBytes,
    uint64_t* outActualFileSize = nullptr,
    bool* outTruncatedPrefix = nullptr) {
    outBytes.clear();
    if (outActualFileSize) *outActualFileSize = 0;
    if (outTruncatedPrefix) *outTruncatedPrefix = false;

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
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }

    const uint64_t totalFileBytes = static_cast<uint64_t>(fileSize.QuadPart);
    if (outActualFileSize) *outActualFileSize = totalFileBytes;

    size_t targetRead = 0;
    if (totalFileBytes <= maxFullBytes) {
        targetRead = static_cast<size_t>(totalFileBytes);
    } else if (fallbackPrefixBytes > 0) {
        targetRead = (std::min<size_t>)(fallbackPrefixBytes, static_cast<size_t>(totalFileBytes));
        if (outTruncatedPrefix) *outTruncatedPrefix = true;
    } else {
        CloseHandle(hFile);
        return false;
    }

    outBytes.resize(targetRead);
    size_t offset = 0;
    while (offset < targetRead) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(targetRead - offset, 1024 * 1024));
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

// ── Base64 & Pure In-Memory PNG / DIB Encoders ───────────────────────────────

std::string encodeBase64DataUri(std::string_view mimePrefix, const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > 64ULL * 1024ULL * 1024ULL) return {};
    DWORD b64Chars = 0;
    if (!CryptBinaryToStringA(
            data,
            static_cast<DWORD>(len),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &b64Chars) || b64Chars == 0) {
        return {};
    }
    std::string out;
    out.resize(mimePrefix.size() + b64Chars);
    std::memcpy(out.data(), mimePrefix.data(), mimePrefix.size());
    DWORD written = b64Chars;
    if (!CryptBinaryToStringA(
            data,
            static_cast<DWORD>(len),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            out.data() + mimePrefix.size(),
            &written)) {
        return {};
    }
    out.resize(mimePrefix.size() + written);
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

void appendPngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* payload, size_t len) {
    appendBe32(out, static_cast<uint32_t>(len));
    const size_t typePos = out.size();
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(type), reinterpret_cast<const uint8_t*>(type) + 4);
    if (payload && len > 0) {
        out.insert(out.end(), payload, payload + len);
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + typePos, static_cast<uInt>(4));
    if (payload && len > 0) {
        size_t rem = len;
        const uint8_t* p = payload;
        while (rem > 0) {
            const uInt step = static_cast<uInt>((std::min<size_t>)(rem, 0x40000000u));
            crc = crc32(crc, p, step);
            p += step;
            rem -= step;
        }
    }
    appendBe32(out, static_cast<uint32_t>(crc));
}

bool encodeRgbaToPngBytes(const uint8_t* rgba, uint32_t width, uint32_t height, std::vector<uint8_t>& outPng) {
    outPng.clear();
    if (!rgba || width == 0 || height == 0 || width > 8192 || height > 8192) {
        return false;
    }
    const size_t rowStride = static_cast<size_t>(width) * 4u;
    const size_t rawScanlinesBytes = (1u + rowStride) * static_cast<size_t>(height);
    std::vector<uint8_t> filtered(rawScanlinesBytes);

    // PNG Sub filter (filter type 1) yields significantly tighter compression on continuous tone / CAD art
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = rgba + static_cast<size_t>(y) * rowStride;
        uint8_t* dstRow = filtered.data() + static_cast<size_t>(y) * (1u + rowStride);
        dstRow[0] = 1; // Sub filter
        dstRow[1] = srcRow[0];
        dstRow[2] = srcRow[1];
        dstRow[3] = srcRow[2];
        dstRow[4] = srcRow[3];
        for (size_t i = 4; i < rowStride; ++i) {
            dstRow[1 + i] = static_cast<uint8_t>(srcRow[i] - srcRow[i - 4]);
        }
    }

    uLongf compBound = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> compressed(compBound);
    if (compress2(
            compressed.data(),
            &compBound,
            filtered.data(),
            static_cast<uLong>(filtered.size()),
            Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    compressed.resize(compBound);

    outPng.reserve(64 + compressed.size());
    outPng.insert(outPng.end(), std::begin(kPngSignature), std::end(kPngSignature));

    uint8_t ihdr[13] = {};
    ihdr[0] = static_cast<uint8_t>((width >> 24) & 0xFFu);
    ihdr[1] = static_cast<uint8_t>((width >> 16) & 0xFFu);
    ihdr[2] = static_cast<uint8_t>((width >> 8) & 0xFFu);
    ihdr[3] = static_cast<uint8_t>(width & 0xFFu);
    ihdr[4] = static_cast<uint8_t>((height >> 24) & 0xFFu);
    ihdr[5] = static_cast<uint8_t>((height >> 16) & 0xFFu);
    ihdr[6] = static_cast<uint8_t>((height >> 8) & 0xFFu);
    ihdr[7] = static_cast<uint8_t>(height & 0xFFu);
    ihdr[8] = 8; // 8-bit depth
    ihdr[9] = 6; // RGBA color type
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    appendPngChunk(outPng, "IHDR", ihdr, sizeof(ihdr));
    appendPngChunk(outPng, "IDAT", compressed.data(), compressed.size());
    appendPngChunk(outPng, "IEND", nullptr, 0);
    return true;
}

std::string encodeRgbaToPngDataUri(const uint8_t* rgba, uint32_t width, uint32_t height) {
    std::vector<uint8_t> png;
    if (!encodeRgbaToPngBytes(rgba, width, height, png)) {
        return {};
    }
    return encodeBase64DataUri("data:image/png;base64,", png.data(), png.size());
}

bool parsePngDimensions(const uint8_t* data, size_t len, uint32_t& outW, uint32_t& outH) noexcept {
    if (!data || len < 24 || std::memcmp(data, kPngSignature, 8) != 0) {
        return false;
    }
    if (std::memcmp(data + 12, "IHDR", 4) != 0) {
        return false;
    }
    outW = readBe32(data + 16);
    outH = readBe32(data + 20);
    return outW > 0 && outH > 0 && outW <= 32768 && outH <= 32768;
}

bool findValidEmbeddedPngSpan(
    const uint8_t* data,
    size_t len,
    size_t startPos,
    size_t& outSpanBytes,
    uint32_t& outW,
    uint32_t& outH) noexcept {
    if (!data || startPos + 33 > len || std::memcmp(data + startPos, kPngSignature, 8) != 0) {
        return false;
    }
    uint32_t w = 0, h = 0;
    if (!parsePngDimensions(data + startPos, len - startPos, w, h)) {
        return false;
    }
    size_t cursor = startPos + 8;
    int chunkGuard = 0;
    while (cursor + 12 <= len && chunkGuard < 4096) {
        ++chunkGuard;
        const uint32_t chunkLen = readBe32(data + cursor);
        if (chunkLen > 32ULL * 1024ULL * 1024ULL || cursor + 12ULL + chunkLen > len) {
            return false;
        }
        const uint8_t* chunkType = data + cursor + 4;
        for (int k = 0; k < 4; ++k) {
            const unsigned char c = chunkType[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
                return false;
            }
        }
        cursor += 12ULL + chunkLen;
        if (std::memcmp(chunkType, "IEND", 4) == 0) {
            outSpanBytes = cursor - startPos;
            outW = w;
            outH = h;
            return true;
        }
    }
    return false;
}

bool wrapDibAsBmpAndConvertToDataUri(
    const uint8_t* rawData,
    size_t rawLen,
    std::string& outDataUri,
    uint32_t& outW,
    uint32_t& outH) {
    outDataUri.clear();
    outW = 0;
    outH = 0;
    if (!rawData || rawLen < 16) return false;

    const uint8_t* dibPtr = rawData;
    size_t dibLen = rawLen;
    uint32_t explicitPixelOffset = 0;

    if (rawLen >= 54 && rawData[0] == 'B' && rawData[1] == 'M') {
        explicitPixelOffset = readLe32(rawData + 10);
        dibPtr = rawData + 14;
        dibLen = rawLen - 14;
    }

    if (dibLen < 40) return false;
    const uint32_t biSize = readLe32(dibPtr);
    if (biSize != 40 && biSize != 52 && biSize != 56 && biSize != 108 && biSize != 124) {
        return false;
    }
    if (biSize > dibLen) return false;

    const int32_t rawW = readLeI32(dibPtr + 4);
    const int32_t rawH = readLeI32(dibPtr + 8);
    const uint16_t planes = readLe16(dibPtr + 12);
    const uint16_t bitCount = readLe16(dibPtr + 14);
    const uint32_t compression = readLe32(dibPtr + 16);
    const uint32_t clrUsed = readLe32(dibPtr + 32);

    if (rawW <= 0 || rawW > 16384 || rawH == 0 || rawH == (std::numeric_limits<int32_t>::min)()) {
        return false;
    }
    const bool topDown = (rawH < 0);
    const uint32_t absH = static_cast<uint32_t>(topDown ? -rawH : rawH);
    if (absH == 0 || absH > 16384 || planes != 1) {
        return false;
    }
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32) {
        return false;
    }

    size_t paletteBytes = 0;
    if (bitCount <= 8) {
        const uint32_t maxColors = 1u << bitCount;
        const uint32_t count = (clrUsed > 0 && clrUsed <= maxColors) ? clrUsed : maxColors;
        paletteBytes = static_cast<size_t>(count) * 4u;
    } else if (compression == 3 /* BI_BITFIELDS */ && biSize == 40) {
        paletteBytes = 12u;
    } else if (compression == 6 /* BI_ALPHABITFIELDS */ && biSize == 40) {
        paletteBytes = 16u;
    }

    size_t dibPixelOffset = static_cast<size_t>(biSize) + paletteBytes;
    if (explicitPixelOffset >= 14 && static_cast<size_t>(explicitPixelOffset - 14) <= dibLen) {
        dibPixelOffset = static_cast<size_t>(explicitPixelOffset - 14);
    }
    if (dibPixelOffset > dibLen) {
        return false;
    }

    outW = static_cast<uint32_t>(rawW);
    outH = absH;

    // Fast lossless conversion to PNG for standard 8/24/32-bit uncompressed DIBs
    if (compression == 0 /* BI_RGB */ && (bitCount == 8 || bitCount == 24 || bitCount == 32)) {
        const size_t srcStride = ((static_cast<size_t>(outW) * bitCount + 31u) / 32u) * 4u;
        if (dibPixelOffset + srcStride * outH <= dibLen &&
            static_cast<uint64_t>(outW) * static_cast<uint64_t>(outH) <= 16777216ULL) {
            std::vector<uint8_t> rgba(static_cast<size_t>(outW) * outH * 4u, 255u);
            const uint8_t* palettePtr = dibPtr + biSize;
            const uint8_t* pixelBase = dibPtr + dibPixelOffset;
            bool anyNonZeroAlpha = false;

            for (uint32_t y = 0; y < outH; ++y) {
                const uint32_t srcY = topDown ? y : (outH - 1u - y);
                const uint8_t* srcRow = pixelBase + static_cast<size_t>(srcY) * srcStride;
                uint8_t* dstRow = rgba.data() + static_cast<size_t>(y) * outW * 4u;

                if (bitCount == 24) {
                    for (uint32_t x = 0; x < outW; ++x) {
                        dstRow[x * 4 + 0] = srcRow[x * 3 + 2];
                        dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                        dstRow[x * 4 + 2] = srcRow[x * 3 + 0];
                        dstRow[x * 4 + 3] = 255;
                    }
                } else if (bitCount == 32) {
                    for (uint32_t x = 0; x < outW; ++x) {
                        const uint8_t a = srcRow[x * 4 + 3];
                        if (a != 0) anyNonZeroAlpha = true;
                        dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                        dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                        dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                        dstRow[x * 4 + 3] = a;
                    }
                } else if (bitCount == 8) {
                    const size_t maxPalIdx = paletteBytes / 4u;
                    for (uint32_t x = 0; x < outW; ++x) {
                        const uint8_t idx = srcRow[x];
                        if (idx < maxPalIdx) {
                            dstRow[x * 4 + 0] = palettePtr[idx * 4 + 2];
                            dstRow[x * 4 + 1] = palettePtr[idx * 4 + 1];
                            dstRow[x * 4 + 2] = palettePtr[idx * 4 + 0];
                        } else {
                            dstRow[x * 4 + 0] = idx;
                            dstRow[x * 4 + 1] = idx;
                            dstRow[x * 4 + 2] = idx;
                        }
                        dstRow[x * 4 + 3] = 255;
                    }
                }
            }

            if (bitCount == 32 && !anyNonZeroAlpha) {
                for (size_t i = 3; i < rgba.size(); i += 4) {
                    rgba[i] = 255;
                }
            }

            outDataUri = encodeRgbaToPngDataUri(rgba.data(), outW, outH);
            if (!outDataUri.empty()) {
                return true;
            }
        }
    }

    // Fallback: assemble valid 14-byte BITMAPFILEHEADER + DIB as data:image/bmp;base64
    std::vector<uint8_t> bmpFile(14u + dibLen, 0);
    bmpFile[0] = 'B';
    bmpFile[1] = 'M';
    writeLe32(bmpFile.data() + 2, static_cast<uint32_t>(bmpFile.size()));
    writeLe32(bmpFile.data() + 10, static_cast<uint32_t>(14u + dibPixelOffset));
    std::memcpy(bmpFile.data() + 14, dibPtr, dibLen);
    outDataUri = encodeBase64DataUri("data:image/bmp;base64,", bmpFile.data(), bmpFile.size());
    return !outDataUri.empty();
}

// ── Minimal Zero-Lock Memory ZIP Reader (for CorelDRAW X4~2026 .cdr) ─────────

#pragma pack(push, 1)
struct CdrZipEocd {
    uint32_t signature;
    uint16_t diskNumber;
    uint16_t cdDiskNumber;
    uint16_t numEntriesThisDisk;
    uint16_t totalEntries;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLength;
};

struct CdrZipCdHeader {
    uint32_t signature;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32Val;
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

struct CdrZipLocalHeader {
    uint32_t signature;
    uint16_t versionNeeded;
    uint16_t bitFlag;
    uint16_t compressionMethod;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32Val;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t filenameLength;
    uint16_t extraFieldLength;
};
#pragma pack(pop)

std::string normalizeArchivePath(std::string_view rawPath) {
    std::string out;
    out.reserve(rawPath.size());
    for (char c : rawPath) {
        char norm = (c == '\\') ? '/' : c;
        if (norm == '/' && out.empty()) continue;
        out.push_back(norm);
    }
    return out;
}

class BinaryZipReader {
public:
    struct Entry {
        std::string name;
        std::string lowerName;
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
        if (!m_data || m_size < sizeof(CdrZipEocd)) return false;

        const size_t maxBack = (std::min<size_t>)(m_size, 65535u + sizeof(CdrZipEocd));
        const size_t scanMin = m_size - maxBack;
        size_t eocdPos = std::string::npos;
        for (size_t pos = m_size - sizeof(CdrZipEocd); ; --pos) {
            if (readLe32(m_data + pos) == 0x06054b50u) {
                eocdPos = pos;
                break;
            }
            if (pos <= scanMin) break;
        }
        if (eocdPos == std::string::npos) return false;

        CdrZipEocd eocd{};
        std::memcpy(&eocd, m_data + eocdPos, sizeof(eocd));
        if (static_cast<uint64_t>(eocd.cdOffset) + static_cast<uint64_t>(eocd.cdSize) > m_size) {
            return false;
        }

        size_t cursor = eocd.cdOffset;
        const size_t cdEnd = eocd.cdOffset + eocd.cdSize;
        while (cursor + sizeof(CdrZipCdHeader) <= cdEnd) {
            CdrZipCdHeader cd{};
            std::memcpy(&cd, m_data + cursor, sizeof(cd));
            if (cd.signature != 0x02014b50u) break;
            const size_t totalHdr =
                sizeof(CdrZipCdHeader) + cd.filenameLength + cd.extraFieldLength + cd.commentLength;
            if (cursor + totalHdr > cdEnd) break;

            std::string_view rawName(
                reinterpret_cast<const char*>(m_data + cursor + sizeof(CdrZipCdHeader)),
                cd.filenameLength);
            Entry e;
            e.name = normalizeArchivePath(rawName);
            e.lowerName = WinUtils::toLower(e.name);
            e.compressionMethod = cd.compressionMethod;
            e.bitFlag = cd.bitFlag;
            e.compressedSize = cd.compressedSize;
            e.uncompressedSize = cd.uncompressedSize;
            e.localHeaderOffset = cd.localHeaderOffset;
            m_entries.push_back(std::move(e));
            cursor += totalHdr;
        }
        return !m_entries.empty();
    }

    const std::vector<Entry>& entries() const noexcept { return m_entries; }

    bool extractBinary(size_t idx, std::vector<uint8_t>& outBytes) const {
        outBytes.clear();
        if (idx >= m_entries.size()) return false;
        const Entry& e = m_entries[idx];
        if ((e.bitFlag & 0x0001u) != 0) return false;
        if (static_cast<uint64_t>(e.localHeaderOffset) + sizeof(CdrZipLocalHeader) > m_size) {
            return false;
        }
        CdrZipLocalHeader local{};
        std::memcpy(&local, m_data + e.localHeaderOffset, sizeof(local));
        if (local.signature != 0x04034b50u) return false;

        const uint64_t dataStart =
            static_cast<uint64_t>(e.localHeaderOffset) +
            sizeof(CdrZipLocalHeader) +
            local.filenameLength +
            local.extraFieldLength;
        if (dataStart + e.compressedSize > m_size) return false;

        const uint8_t* compPtr = m_data + dataStart;
        if (e.compressionMethod == 0) {
            if (e.compressedSize > kMaxZipEntryUncompressedBytes) return false;
            outBytes.assign(compPtr, compPtr + e.compressedSize);
            return !outBytes.empty();
        }
        if (e.compressionMethod != 8) return false;

        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compPtr));
        zs.avail_in = static_cast<uInt>(e.compressedSize);
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;

        outBytes.reserve((std::min<size_t>)(e.uncompressedSize ? e.uncompressedSize : 65536u, kMaxZipEntryUncompressedBytes));
        uint8_t chunk[32768];
        int ret = Z_OK;
        while (ret == Z_OK && outBytes.size() < kMaxZipEntryUncompressedBytes) {
            zs.next_out = chunk;
            zs.avail_out = static_cast<uInt>(sizeof(chunk));
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                inflateEnd(&zs);
                outBytes.clear();
                return false;
            }
            const size_t have = sizeof(chunk) - zs.avail_out;
            const size_t canTake = (std::min<size_t>)(have, kMaxZipEntryUncompressedBytes - outBytes.size());
            outBytes.insert(outBytes.end(), chunk, chunk + canTake);
            if (canTake < have) break;
        }
        inflateEnd(&zs);
        return !outBytes.empty();
    }

private:
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    std::vector<Entry> m_entries;
};

// ── PSD / PSB Helpers ────────────────────────────────────────────────────────

const char* psdColorModeLabel(uint16_t mode) noexcept {
    switch (mode) {
        case 0: return "Bitmap";
        case 1: return "Grayscale";
        case 2: return "Indexed";
        case 3: return "RGB";
        case 4: return "CMYK";
        case 7: return "Multichannel";
        case 8: return "Duotone";
        case 9: return "Lab";
        default: return "Color";
    }
}

bool extractPsdThumbnailResource(
    const uint8_t* resData,
    size_t resLen,
    std::string& outDataUri,
    uint32_t& outW,
    uint32_t& outH) {
    outDataUri.clear();
    outW = 0;
    outH = 0;
    size_t pos = 0;
    while (pos + 12 <= resLen) {
        if (std::memcmp(resData + pos, "8BIM", 4) != 0) {
            break;
        }
        const uint16_t resId = readBe16(resData + pos + 4);
        const uint8_t nameLen = resData[pos + 6];
        const size_t paddedName = ((static_cast<size_t>(nameLen) + 1u) + 1u) & ~static_cast<size_t>(1u);
        if (pos + 6u + paddedName + 4u > resLen) break;

        const uint32_t blockLen = readBe32(resData + pos + 6u + paddedName);
        const size_t dataOffset = pos + 6u + paddedName + 4u;
        if (dataOffset + blockLen > resLen) break;

        if ((resId == 1036u /* 0x040C RGB */ || resId == 1033u /* 0x0409 BGR */) && blockLen > 28u) {
            const uint8_t* blk = resData + dataOffset;
            const uint32_t fmt = readBe32(blk);
            const uint32_t tw = readBe32(blk + 4);
            const uint32_t th = readBe32(blk + 8);
            const uint8_t* payload = blk + 28;
            const size_t payloadLen = blockLen - 28u;
            if (fmt == 1u && payloadLen > 4 && payload[0] == 0xFF && payload[1] == 0xD8 && resId == 1036u) {
                outW = tw;
                outH = th;
                outDataUri = encodeBase64DataUri("data:image/jpeg;base64,", payload, payloadLen);
                if (!outDataUri.empty()) return true;
            } else if (fmt == 0u && tw > 0 && th > 0 && tw <= 4096 && th <= 4096) {
                const uint32_t widthBytes = readBe32(blk + 12);
                const size_t rowStride = widthBytes ? widthBytes : (((static_cast<size_t>(tw) * 24u + 31u) / 32u) * 4u);
                if (rowStride * th <= payloadLen) {
                    std::vector<uint8_t> rgba(static_cast<size_t>(tw) * th * 4u, 255u);
                    for (uint32_t y = 0; y < th; ++y) {
                        const uint8_t* sr = payload + static_cast<size_t>(y) * rowStride;
                        uint8_t* dr = rgba.data() + static_cast<size_t>(y) * tw * 4u;
                        for (uint32_t x = 0; x < tw; ++x) {
                            if (x * 3u + 2u < rowStride) {
                                if (resId == 1033u) {
                                    dr[x * 4 + 0] = sr[x * 3 + 2];
                                    dr[x * 4 + 1] = sr[x * 3 + 1];
                                    dr[x * 4 + 2] = sr[x * 3 + 0];
                                } else {
                                    dr[x * 4 + 0] = sr[x * 3 + 0];
                                    dr[x * 4 + 1] = sr[x * 3 + 1];
                                    dr[x * 4 + 2] = sr[x * 3 + 2];
                                }
                            }
                        }
                    }
                    outW = tw;
                    outH = th;
                    outDataUri = encodeRgbaToPngDataUri(rgba.data(), tw, th);
                    if (!outDataUri.empty()) return true;
                }
            }
        }

        const size_t paddedBlock = (static_cast<size_t>(blockLen) + 1u) & ~static_cast<size_t>(1u);
        pos = dataOffset + paddedBlock;
    }
    return false;
}

void decodePackBitsRow(const uint8_t* src, size_t compLen, uint8_t* dst, size_t expectedBytes) noexcept {
    size_t inPos = 0;
    size_t outPos = 0;
    while (inPos < compLen && outPos < expectedBytes) {
        const int8_t hdr = static_cast<int8_t>(src[inPos++]);
        if (hdr >= 0) {
            const size_t count = static_cast<size_t>(hdr) + 1u;
            const size_t avail = compLen - inPos;
            const size_t safeIn = (std::min)(count, avail);
            const size_t toCopy = (std::min)(safeIn, expectedBytes - outPos);
            std::memcpy(dst + outPos, src + inPos, toCopy);
            inPos += safeIn;
            outPos += toCopy;
        } else if (hdr != -128) {
            if (inPos >= compLen) break;
            const uint8_t val = src[inPos++];
            const size_t count = static_cast<size_t>(1 - static_cast<int>(hdr));
            const size_t toFill = (std::min)(count, expectedBytes - outPos);
            std::memset(dst + outPos, val, toFill);
            outPos += toFill;
        }
    }
    if (outPos < expectedBytes) {
        std::memset(dst + outPos, 0, expectedBytes - outPos);
    }
}

// ── RIFF CDR DISP Chunk Scanner ──────────────────────────────────────────────

bool scanRiffForDispBitmap(
    const uint8_t* data,
    size_t size,
    size_t offset,
    size_t endOffset,
    int depth,
    std::string& outDataUri,
    uint32_t& outW,
    uint32_t& outH) {
    if (depth > 8 || endOffset > size) return false;
    size_t pos = offset;
    while (pos + 8 <= endOffset) {
        const uint8_t* tag = data + pos;
        const uint32_t chunkLen = readLe32(data + pos + 4);
        const size_t payloadPos = pos + 8;
        if (payloadPos + chunkLen > endOffset) break;

        if ((std::memcmp(tag, "RIFF", 4) == 0 || std::memcmp(tag, "LIST", 4) == 0) && chunkLen >= 4) {
            if (scanRiffForDispBitmap(data, size, payloadPos + 4, payloadPos + chunkLen, depth + 1, outDataUri, outW, outH)) {
                return true;
            }
        } else if (std::memcmp(tag, "DISP", 4) == 0 && chunkLen > 40) {
            const uint8_t* dispPtr = data + payloadPos;
            // CF_DIB inside RIFF DISP typically has a 4-byte clipboard format prefix (e.g. CF_DIB = 8) before BITMAPINFOHEADER
            for (size_t shift : {size_t(4), size_t(0), size_t(8), size_t(12)}) {
                if (shift + 40 <= chunkLen) {
                    if (wrapDibAsBmpAndConvertToDataUri(dispPtr + shift, chunkLen - shift, outDataUri, outW, outH)) {
                        return true;
                    }
                }
            }
        }

        const size_t padded = (static_cast<size_t>(chunkLen) + 1u) & ~static_cast<size_t>(1u);
        pos = payloadPos + padded;
    }
    return false;
}

// ── DXF String, MTEXT & ACI Color Helpers ────────────────────────────────────

void appendCodepointUtf8(uint32_t cp, std::string& out) {
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return;
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

std::string decodeDxfBytesToUtf8(std::string_view raw, bool& outUsedGbk) {
    if (raw.empty()) return {};
    bool pureAscii = true;
    for (unsigned char c : raw) {
        if (c >= 0x80u) {
            pureAscii = false;
            break;
        }
    }
    std::string utf8;
    if (pureAscii) {
        utf8.assign(raw.data(), raw.size());
    } else {
        const int capLen = static_cast<int>((std::min<size_t>)(raw.size(), 1024 * 1024));
        const int utf8Wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), capLen, nullptr, 0);
        if (utf8Wide > 0) {
            utf8.assign(raw.data(), static_cast<size_t>(capLen));
        } else {
            outUsedGbk = true;
            const UINT cp = IsValidCodePage(936) ? 936u : CP_ACP;
            const int gbkWide = MultiByteToWideChar(cp, 0, raw.data(), capLen, nullptr, 0);
            if (gbkWide > 0) {
                std::wstring wide(static_cast<size_t>(gbkWide), L'\0');
                MultiByteToWideChar(cp, 0, raw.data(), capLen, wide.data(), gbkWide);
                utf8 = WinUtils::wstringToUtf8(wide);
            }
        }
    }

    // Decode AutoCAD \U+XXXX hex escapes & %%c / %%d / %%p engineering symbols
    if (utf8.find("\\U+") == std::string::npos && utf8.find("\\u+") == std::string::npos &&
        utf8.find("%%") == std::string::npos) {
        return utf8;
    }

    std::string decoded;
    decoded.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ) {
        if (i + 6 < utf8.size() && utf8[i] == '\\' && (utf8[i + 1] == 'U' || utf8[i + 1] == 'u') && utf8[i + 2] == '+') {
            uint32_t cp = 0;
            bool hexOk = true;
            for (size_t k = 0; k < 4; ++k) {
                const char h = utf8[i + 3 + k];
                uint32_t nib = 0;
                if (h >= '0' && h <= '9') nib = static_cast<uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f') nib = static_cast<uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') nib = static_cast<uint32_t>(h - 'A' + 10);
                else { hexOk = false; break; }
                cp = (cp << 4) | nib;
            }
            if (hexOk) {
                appendCodepointUtf8(cp, decoded);
                i += 7;
                continue;
            }
        }
        if (i + 2 < utf8.size() && utf8[i] == '%' && utf8[i + 1] == '%') {
            const char sym = static_cast<char>(std::tolower(static_cast<unsigned char>(utf8[i + 2])));
            if (sym == 'c') {
                appendCodepointUtf8(0x2300u, decoded); // Diameter symbol
                i += 3;
                continue;
            }
            if (sym == 'd') {
                appendCodepointUtf8(0x00B0u, decoded); // Degree symbol
                i += 3;
                continue;
            }
            if (sym == 'p') {
                appendCodepointUtf8(0x00B1u, decoded); // Plus-minus symbol
                i += 3;
                continue;
            }
            if (sym == 'u' || sym == 'o') {
                i += 3;
                continue;
            }
            if (sym == '%') {
                decoded.push_back('%');
                i += 3;
                continue;
            }
        }
        decoded.push_back(utf8[i++]);
    }
    return decoded;
}

std::string cleanDxfMText(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        const char c = raw[i];
        if (c == '{' || c == '}') {
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < raw.size()) {
            const char cmd = raw[i + 1];
            if (cmd == '\\' || cmd == '{' || cmd == '}') {
                out.push_back(cmd);
                i += 2;
                continue;
            }
            if (cmd == 'P') {
                out.push_back('\n');
                i += 2;
                continue;
            }
            if (cmd == '~') {
                out.push_back(' ');
                i += 2;
                continue;
            }
            if (cmd == 'L' || cmd == 'l' || cmd == 'O' || cmd == 'o' || cmd == 'K' || cmd == 'k') {
                i += 2;
                continue;
            }
            if (cmd == 'S') {
                // Stacked fraction \S1/2; or \S+0.02^-0.01; -> keep readable content
                const size_t semi = raw.find(';', i + 2);
                if (semi != std::string_view::npos) {
                    for (size_t k = i + 2; k < semi; ++k) {
                        out.push_back(raw[k] == '^' ? '/' : raw[k]);
                    }
                    i = semi + 1;
                    continue;
                }
            }
            if (cmd == 'f' || cmd == 'F' || cmd == 'C' || cmd == 'c' ||
                cmd == 'H' || cmd == 'W' || cmd == 'Q' || cmd == 'T' ||
                cmd == 'A' || cmd == 'p') {
                const size_t semi = raw.find(';', i + 2);
                if (semi != std::string_view::npos && semi - i <= 96) {
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::string aciColorToHex(int aci, bool& outAdaptiveDefault) {
    outAdaptiveDefault = false;
    const int absAci = std::abs(aci);
    if (absAci == 0 || absAci == 7 || absAci >= 256) {
        outAdaptiveDefault = true;
        return "#F8FAFC";
    }
    switch (absAci) {
        case 1: return "#FF3B30"; // Red
        case 2: return "#FACC15"; // Yellow
        case 3: return "#22C55E"; // Green
        case 4: return "#06B6D4"; // Cyan
        case 5: return "#3B82F6"; // Blue
        case 6: return "#EC4899"; // Magenta
        case 8: return "#94A3B8"; // Gray
        case 9: return "#CBD5E1"; // Light Gray
        default: break;
    }
    if (absAci >= 250 && absAci <= 255) {
        const int v = 51 + (absAci - 250) * 38;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", v, v, v);
        return buf;
    }
    // Standard AutoCAD 10..249 hue wheel (24 hue steps * 10 brightness/saturation tones)
    const int hueIdx = (absAci - 10) / 10; // 0..23
    const int tone = (absAci - 10) % 10;   // 0..9
    const double hueDeg = std::fmod(hueIdx * 15.0, 360.0);
    const double sat = (tone & 1) ? 0.55 : 0.92;
    const double val = 1.0 - (tone / 2) * 0.14;

    const double c = val * sat;
    const double x = c * (1.0 - std::abs(std::fmod(hueDeg / 60.0, 2.0) - 1.0));
    const double m = val - c;
    double r1 = 0, g1 = 0, b1 = 0;
    if (hueDeg < 60) { r1 = c; g1 = x; }
    else if (hueDeg < 120) { r1 = x; g1 = c; }
    else if (hueDeg < 180) { g1 = c; b1 = x; }
    else if (hueDeg < 240) { g1 = x; b1 = c; }
    else if (hueDeg < 300) { r1 = x; b1 = c; }
    else { r1 = c; b1 = x; }

    const int r = std::clamp(static_cast<int>(std::lround((r1 + m) * 255.0)), 0, 255);
    const int g = std::clamp(static_cast<int>(std::lround((g1 + m) * 255.0)), 0, 255);
    const int b = std::clamp(static_cast<int>(std::lround((b1 + m) * 255.0)), 0, 255);
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    return hex;
}

bool parseDoubleFast(std::string_view sv, double& outVal) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    if (sv.empty()) return false;
    if (sv.front() == '+') sv.remove_prefix(1);
    if (sv.empty()) return false;
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || !std::isfinite(v)) return false;
    outVal = v;
    return true;
}

bool parseIntFast(std::string_view sv, int& outVal) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    if (sv.empty()) return false;
    if (sv.front() == '+') sv.remove_prefix(1);
    if (sv.empty()) return false;
    int v = 0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{}) return false;
    outVal = v;
    return true;
}

void appendBulgeArcPoints(
    double x1,
    double y1,
    double x2,
    double y2,
    double bulge,
    std::vector<double>& outCoords) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double chord = std::hypot(dx, dy);
    if (chord < 1e-9 || std::abs(bulge) < 1e-4 || std::abs(bulge) > 1e4) {
        outCoords.push_back(x2);
        outCoords.push_back(y2);
        return;
    }

    const double factor = (1.0 - bulge * bulge) / (4.0 * bulge);
    const double cx = 0.5 * (x1 + x2) - factor * dy;
    const double cy = 0.5 * (y1 + y2) + factor * dx;
    const double radius = std::hypot(x1 - cx, y1 - cy);
    const double startAng = std::atan2(y1 - cy, x1 - cx);
    const double sweep = 4.0 * std::atan(bulge);
    const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 12.0))), 4, 24);
    for (int s = 1; s <= steps; ++s) {
        const double ang = startAng + sweep * (static_cast<double>(s) / steps);
        outCoords.push_back(cx + radius * std::cos(ang));
        outCoords.push_back(cy + radius * std::sin(ang));
    }
}

} // namespace

// ── 1. Shell Thumbnail Helper (STA Isolated + Message Pump Timeout) ──────────

DesignImagePreviewResult DesignAndCadPreview::extractShellThumbnailPng(
    const std::wstring& filePath,
    uint32_t requestedSizePx,
    uint32_t timeoutMs) {
    DesignImagePreviewResult empty;
    if (filePath.empty()) return empty;

    const uint32_t clampedReq = std::clamp(requestedSizePx, 128u, 2560u);
    auto sharedRes = std::make_shared<DesignImagePreviewResult>();

    std::thread staWorker([sharedRes, filePath, clampedReq]() {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(hrCo);

        IShellItem* pItem = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(filePath.c_str(), nullptr, IID_PPV_ARGS(&pItem))) && pItem) {
            IShellItemImageFactory* pFactory = nullptr;
            if (SUCCEEDED(pItem->QueryInterface(IID_PPV_ARGS(&pFactory))) && pFactory) {
                HBITMAP hBmp = nullptr;
                const SIZE sz = {static_cast<LONG>(clampedReq), static_cast<LONG>(clampedReq)};
                const HRESULT hrImg = pFactory->GetImage(
                    sz, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hBmp);
                if (SUCCEEDED(hrImg) && hBmp) {
                    BITMAP bm{};
                    if (GetObjectW(hBmp, sizeof(bm), &bm) != 0 &&
                        bm.bmWidth > 0 && bm.bmHeight != 0 &&
                        bm.bmWidth <= 8192 && std::abs(bm.bmHeight) <= 8192) {
                        const uint32_t w = static_cast<uint32_t>(bm.bmWidth);
                        const uint32_t h = static_cast<uint32_t>(std::abs(bm.bmHeight));
                        BITMAPINFO bmi{};
                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
                        bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
                        bmi.bmiHeader.biPlanes = 1;
                        bmi.bmiHeader.biBitCount = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4u, 0);
                        HDC hdc = GetDC(nullptr);
                        if (hdc) {
                            const int lines = GetDIBits(
                                hdc, hBmp, 0, h, bgra.data(), &bmi, DIB_RGB_COLORS);
                            ReleaseDC(nullptr, hdc);
                            if (lines > 0) {
                                bool hasAlpha = false;
                                for (size_t i = 0; i < bgra.size(); i += 4) {
                                    std::swap(bgra[i], bgra[i + 2]); // BGRA -> RGBA
                                    if (bgra[i + 3] != 0) hasAlpha = true;
                                }
                                if (!hasAlpha) {
                                    for (size_t i = 3; i < bgra.size(); i += 4) {
                                        bgra[i] = 255;
                                    }
                                }
                                std::string uri = encodeRgbaToPngDataUri(bgra.data(), w, h);
                                if (!uri.empty()) {
                                    sharedRes->ok = true;
                                    sharedRes->isSnapshot = true;
                                    sharedRes->width = w;
                                    sharedRes->height = h;
                                    sharedRes->dataUri = std::move(uri);
                                    sharedRes->pageDataUris = {sharedRes->dataUri};
                                }
                            }
                        }
                    }
                    DeleteObject(hBmp);
                }
                pFactory->Release();
            }
            pItem->Release();
        }

        if (needUninit) {
            CoUninitialize();
        }
    });

    if (joinWorkerWhilePumpingSentMessages(staWorker, timeoutMs)) {
        return std::move(*sharedRes);
    }
    return empty;
}

// ── 2. Adobe Photoshop (.psd / .psb) Native Zero-Lock Decoder ────────────────

DesignImagePreviewResult DesignAndCadPreview::extractPsdPreview(
    const std::wstring& filePath,
    uint32_t maxDimensionPx,
    bool allowShellFallback) {
    DesignImagePreviewResult result;
    std::vector<uint8_t> raw;
    uint64_t actualFileBytes = 0;
    bool prefixOnly = false;
    if (!readFileZeroLock(filePath, kMaxPsdFullLoadBytes, kPsdFallbackPrefixBytes, raw, &actualFileBytes, &prefixOnly)) {
        return result;
    }
    if (raw.size() < 26 || std::memcmp(raw.data(), "8BPS", 4) != 0) {
        return result;
    }

    const uint16_t version = readBe16(raw.data() + 4);
    if (version != 1 && version != 2) {
        return result;
    }

    const uint16_t channels = readBe16(raw.data() + 12);
    const uint32_t height = readBe32(raw.data() + 14);
    const uint32_t width = readBe32(raw.data() + 18);
    const uint16_t depth = readBe16(raw.data() + 22);
    const uint16_t colorMode = readBe16(raw.data() + 24);

    if (channels == 0 || channels > 64 || width == 0 || height == 0 ||
        width > 300000 || height > 300000) {
        return result;
    }

    result.width = width;
    result.height = height;
    result.formatLabel = (version == 2) ? "PSB" : "PSD";
    result.metaSummary =
        std::to_string(width) + "×" + std::to_string(height) + " · " +
        psdColorModeLabel(colorMode) + "/" + std::to_string(depth);

    size_t cursor = 26;
    if (cursor + 4 > raw.size()) return result;
    const uint32_t colorModeDataLen = readBe32(raw.data() + cursor);
    cursor += 4;
    const uint8_t* palettePtr = nullptr;
    if (cursor + colorModeDataLen > raw.size()) return result;
    if (colorMode == 2 && colorModeDataLen >= 768) {
        palettePtr = raw.data() + cursor;
    }
    cursor += colorModeDataLen;

    // Section 3: Image Resources
    if (cursor + 4 > raw.size()) return result;
    const uint32_t imgResLen = readBe32(raw.data() + cursor);
    cursor += 4;
    const uint8_t* imgResPtr = raw.data() + cursor;
    const size_t availImgResLen = (cursor < raw.size())
        ? (std::min<size_t>)(imgResLen, raw.size() - cursor)
        : 0;

    bool hasComposite = false;
    bool hasMergedAlpha = false;

    if (!prefixOnly && cursor + imgResLen <= raw.size()) {
        cursor += imgResLen;

        // Section 4: Layer and Mask Info (4B length for PSD, 8B length for PSB)
        const size_t lenFieldBytes = (version == 2) ? 8u : 4u;
        if (cursor + lenFieldBytes <= raw.size()) {
            const uint64_t layerMaskLen = (version == 2)
                ? readBe64(raw.data() + cursor)
                : static_cast<uint64_t>(readBe32(raw.data() + cursor));
            cursor += lenFieldBytes;

            // Peek layer count if present
            if (layerMaskLen >= lenFieldBytes + 2u && cursor + lenFieldBytes + 2u <= raw.size()) {
                const uint64_t layerInfoLen = (version == 2)
                    ? readBe64(raw.data() + cursor)
                    : static_cast<uint64_t>(readBe32(raw.data() + cursor));
                if (layerInfoLen >= 2u) {
                    const int16_t rawLayerCount = static_cast<int16_t>(readBe16(raw.data() + cursor + lenFieldBytes));
                    if (rawLayerCount < 0) {
                        result.layerCount = static_cast<uint32_t>(-static_cast<int32_t>(rawLayerCount));
                        hasMergedAlpha = true;
                    } else {
                        result.layerCount = static_cast<uint32_t>(rawLayerCount);
                    }
                }
            }

            if (result.layerCount > 0) {
                result.metaSummary += " · " + std::to_string(result.layerCount) + " layers";
            }

            // Section 5: Composite Image Data
            if (layerMaskLen <= raw.size() - cursor) {
                cursor += static_cast<size_t>(layerMaskLen);
                if (cursor + 2 <= raw.size() && (depth == 8 || depth == 16) &&
                    (colorMode == 1 || (colorMode == 2 && palettePtr != nullptr) || colorMode == 3 || colorMode == 4)) {
                    const uint16_t compression = readBe16(raw.data() + cursor);
                    cursor += 2;

                    uint16_t neededChannels = 1;
                    bool modeSupported = true;
                    if (colorMode == 1) {
                        neededChannels = (channels >= 2 && hasMergedAlpha) ? 2 : 1;
                    } else if (colorMode == 2) {
                        neededChannels = 1;
                    } else if (colorMode == 3) {
                        if (channels < 3) modeSupported = false;
                        else neededChannels = (channels >= 4 && hasMergedAlpha) ? 4 : 3;
                    } else if (colorMode == 4) {
                        if (channels < 4) modeSupported = false;
                        else neededChannels = (channels >= 5 && hasMergedAlpha) ? 5 : 4;
                    }

                    if (modeSupported && (compression == 0 || compression == 1)) {
                        const uint32_t clampedMax = std::clamp(maxDimensionPx, 256u, 4096u);
                        uint32_t outW = width;
                        uint32_t outH = height;
                        const uint32_t maxSide = (std::max)(width, height);
                        if (maxSide > clampedMax) {
                            const double scale = static_cast<double>(clampedMax) / static_cast<double>(maxSide);
                            outW = (std::max)(1u, static_cast<uint32_t>(std::lround(width * scale)));
                            outH = (std::max)(1u, static_cast<uint32_t>(std::lround(height * scale)));
                        }

                        std::vector<uint32_t> srcXMap(outW);
                        for (uint32_t ox = 0; ox < outW; ++ox) {
                            srcXMap[ox] = (std::min)(width - 1u, static_cast<uint32_t>((static_cast<uint64_t>(ox) * width) / outW));
                        }
                        std::vector<uint32_t> srcYMap(outH);
                        for (uint32_t oy = 0; oy < outH; ++oy) {
                            srcYMap[oy] = (std::min)(height - 1u, static_cast<uint32_t>((static_cast<uint64_t>(oy) * height) / outH));
                        }

                        const size_t bytesPerSample = (depth == 16) ? 2u : 1u;
                        const size_t rowBytes = static_cast<size_t>(width) * bytesPerSample;
                        std::vector<std::vector<uint8_t>> sampledPlanes(
                            neededChannels, std::vector<uint8_t>(static_cast<size_t>(outW) * outH, 0));
                        std::vector<uint8_t> rowBuffer(rowBytes, 0);
                        bool decodeOk = true;

                        if (compression == 0) {
                            const uint64_t requiredRaw =
                                static_cast<uint64_t>(neededChannels) * static_cast<uint64_t>(height) * rowBytes;
                            if (requiredRaw > raw.size() - cursor) {
                                decodeOk = false;
                            } else {
                                for (uint16_t ch = 0; ch < neededChannels; ++ch) {
                                    const uint8_t* chBase = raw.data() + cursor + static_cast<size_t>(ch) * height * rowBytes;
                                    for (uint32_t oy = 0; oy < outH; ++oy) {
                                        const uint8_t* srcRow = chBase + static_cast<size_t>(srcYMap[oy]) * rowBytes;
                                        uint8_t* dstRow = sampledPlanes[ch].data() + static_cast<size_t>(oy) * outW;
                                        if (depth == 8) {
                                            for (uint32_t ox = 0; ox < outW; ++ox) {
                                                dstRow[ox] = srcRow[srcXMap[ox]];
                                            }
                                        } else {
                                            for (uint32_t ox = 0; ox < outW; ++ox) {
                                                dstRow[ox] = srcRow[static_cast<size_t>(srcXMap[ox]) * 2u];
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (compression == 1) {
                            const size_t countElemBytes = (version == 2) ? 4u : 2u;
                            const uint64_t totalCountRows = static_cast<uint64_t>(channels) * static_cast<uint64_t>(height);
                            const uint64_t countTableBytes = totalCountRows * countElemBytes;
                            if (totalCountRows > 8000000ULL || countTableBytes > raw.size() - cursor) {
                                decodeOk = false;
                            } else {
                                const uint8_t* countTablePtr = raw.data() + cursor;
                                size_t dataPos = cursor + static_cast<size_t>(countTableBytes);
                                for (uint16_t ch = 0; ch < neededChannels && decodeOk; ++ch) {
                                    uint32_t nextOutY = 0;
                                    for (uint32_t y = 0; y < height; ++y) {
                                        const size_t tableIdx = (static_cast<size_t>(ch) * height + y) * countElemBytes;
                                        const uint32_t compRowLen = (version == 2)
                                            ? readBe32(countTablePtr + tableIdx)
                                            : static_cast<uint32_t>(readBe16(countTablePtr + tableIdx));
                                        if (dataPos + compRowLen > raw.size()) {
                                            decodeOk = false;
                                            break;
                                        }
                                        if (nextOutY < outH && srcYMap[nextOutY] == y) {
                                            decodePackBitsRow(raw.data() + dataPos, compRowLen, rowBuffer.data(), rowBytes);
                                            while (nextOutY < outH && srcYMap[nextOutY] == y) {
                                                uint8_t* dstRow = sampledPlanes[ch].data() + static_cast<size_t>(nextOutY) * outW;
                                                if (depth == 8) {
                                                    for (uint32_t ox = 0; ox < outW; ++ox) {
                                                        dstRow[ox] = rowBuffer[srcXMap[ox]];
                                                    }
                                                } else {
                                                    for (uint32_t ox = 0; ox < outW; ++ox) {
                                                        dstRow[ox] = rowBuffer[static_cast<size_t>(srcXMap[ox]) * 2u];
                                                    }
                                                }
                                                ++nextOutY;
                                            }
                                        }
                                        dataPos += compRowLen;
                                    }
                                }
                            }
                        }

                        if (decodeOk) {
                            const size_t pixelCount = static_cast<size_t>(outW) * outH;
                            std::vector<uint8_t> rgba(pixelCount * 4u, 255u);
                            if (colorMode == 1) {
                                for (size_t i = 0; i < pixelCount; ++i) {
                                    const uint8_t g = sampledPlanes[0][i];
                                    rgba[i * 4 + 0] = g;
                                    rgba[i * 4 + 1] = g;
                                    rgba[i * 4 + 2] = g;
                                    rgba[i * 4 + 3] = (neededChannels >= 2) ? sampledPlanes[1][i] : 255u;
                                }
                            } else if (colorMode == 2 && palettePtr) {
                                for (size_t i = 0; i < pixelCount; ++i) {
                                    const uint8_t idx = sampledPlanes[0][i];
                                    rgba[i * 4 + 0] = palettePtr[idx];
                                    rgba[i * 4 + 1] = palettePtr[256u + idx];
                                    rgba[i * 4 + 2] = palettePtr[512u + idx];
                                    rgba[i * 4 + 3] = 255u;
                                }
                            } else if (colorMode == 3) {
                                for (size_t i = 0; i < pixelCount; ++i) {
                                    rgba[i * 4 + 0] = sampledPlanes[0][i];
                                    rgba[i * 4 + 1] = sampledPlanes[1][i];
                                    rgba[i * 4 + 2] = sampledPlanes[2][i];
                                    rgba[i * 4 + 3] = (neededChannels >= 4) ? sampledPlanes[3][i] : 255u;
                                }
                            } else if (colorMode == 4) {
                                for (size_t i = 0; i < pixelCount; ++i) {
                                    const uint32_t c = sampledPlanes[0][i];
                                    const uint32_t m = sampledPlanes[1][i];
                                    const uint32_t yv = sampledPlanes[2][i];
                                    const uint32_t k = sampledPlanes[3][i];
                                    rgba[i * 4 + 0] = static_cast<uint8_t>((c * k + 127u) / 255u);
                                    rgba[i * 4 + 1] = static_cast<uint8_t>((m * k + 127u) / 255u);
                                    rgba[i * 4 + 2] = static_cast<uint8_t>((yv * k + 127u) / 255u);
                                    rgba[i * 4 + 3] = (neededChannels >= 5) ? sampledPlanes[4][i] : 255u;
                                }
                            }

                            std::string pngUri = encodeRgbaToPngDataUri(rgba.data(), outW, outH);
                            if (!pngUri.empty()) {
                                result.ok = true;
                                result.dataUri = std::move(pngUri);
                                result.pageDataUris = {result.dataUri};
                                hasComposite = true;
                            }
                        }
                    }
                }
            }
        }
    }

    // Fallback to Section 3 Image Resource 0x040C Thumbnail + Shell Thumbnail if composite was ZIP-compressed or oversized
    if (!hasComposite) {
        std::string thumbUri;
        uint32_t tw = 0, th = 0;
        if (availImgResLen > 0 && extractPsdThumbnailResource(imgResPtr, availImgResLen, thumbUri, tw, th)) {
            result.ok = true;
            result.isSnapshot = true;
            result.dataUri = std::move(thumbUri);
            result.pageDataUris = {result.dataUri};
        }
        if (allowShellFallback) {
            auto shellThumb = extractShellThumbnailPng(filePath, 1600);
            if (shellThumb.ok &&
                (static_cast<uint64_t>(shellThumb.width) * shellThumb.height > static_cast<uint64_t>(tw) * th)) {
                result.ok = true;
                result.isSnapshot = true;
                result.dataUri = std::move(shellThumb.dataUri);
                result.pageDataUris = {result.dataUri};
            }
        }
    }

    return result;
}

// ── 3. CorelDRAW (.cdr) Multi-Version Zero-Lock Preview ──────────────────────

DesignImagePreviewResult DesignAndCadPreview::extractCdrPreview(
    const std::wstring& filePath,
    bool allowShellFallback) {
    DesignImagePreviewResult result;
    result.formatLabel = "CorelDRAW";

    std::vector<uint8_t> raw;
    if (!readFileZeroLock(filePath, kMaxCdrArchiveBytes, 0, raw)) {
        if (allowShellFallback) {
            auto shell = extractShellThumbnailPng(filePath, 1600);
            if (shell.ok) {
                shell.formatLabel = "CorelDRAW";
                shell.metaSummary = "CorelDRAW · " + std::to_string(shell.width) + "×" + std::to_string(shell.height);
                return shell;
            }
        }
        return result;
    }

    // Case A: CorelDRAW X4 ~ 2026 ZIP Container
    if (raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K') {
        BinaryZipReader zip;
        if (zip.open(raw)) {
            struct PageCandidate {
                int pageNum = 0;
                size_t entryIdx = 0;
                bool isPng = false;
            };
            std::map<int, PageCandidate> numberedPages;
            std::vector<size_t> fallbackThumbIndices;

            const auto& entries = zip.entries();
            for (size_t i = 0; i < entries.size(); ++i) {
                const std::string& ln = entries[i].lowerName;
                const bool isPng = ln.size() > 4 && ln.compare(ln.size() - 4, 4, ".png") == 0;
                const bool isBmp = ln.size() > 4 && ln.compare(ln.size() - 4, 4, ".bmp") == 0;
                if (!isPng && !isBmp) continue;

                const size_t slash = ln.rfind('/');
                const std::string base = (slash != std::string::npos) ? ln.substr(slash + 1) : ln;
                const std::string stem = base.substr(0, base.size() - 4);

                if (stem.rfind("page", 0) == 0 && stem.size() > 4 &&
                    (ln.rfind("previews/", 0) == 0 || ln.rfind("metadata/thumbnails/", 0) == 0)) {
                    int pageNo = 0;
                    if (parseIntFast(std::string_view(stem).substr(4), pageNo) && pageNo > 0) {
                        auto it = numberedPages.find(pageNo);
                        if (it == numberedPages.end() || (!it->second.isPng && isPng)) {
                            numberedPages[pageNo] = PageCandidate{pageNo, i, isPng};
                        }
                        continue;
                    }
                }
                if (ln == "previews/thumbnail.png" || ln == "metadata/thumbnails/thumbnail.bmp" ||
                    ln.rfind("previews/", 0) == 0 || ln.rfind("metadata/", 0) == 0) {
                    fallbackThumbIndices.push_back(i);
                }
            }

            auto appendEntryImage = [&](size_t entryIdx) {
                std::vector<uint8_t> bytes;
                if (!zip.extractBinary(entryIdx, bytes) || bytes.size() < 16) return;
                uint32_t w = 0, h = 0;
                std::string uri;
                if (parsePngDimensions(bytes.data(), bytes.size(), w, h)) {
                    uri = encodeBase64DataUri("data:image/png;base64,", bytes.data(), bytes.size());
                } else {
                    wrapDibAsBmpAndConvertToDataUri(bytes.data(), bytes.size(), uri, w, h);
                }
                if (!uri.empty()) {
                    if (result.pageDataUris.empty()) {
                        result.width = w;
                        result.height = h;
                        result.dataUri = uri;
                    }
                    result.pageDataUris.push_back(std::move(uri));
                }
            };

            size_t count = 0;
            for (const auto& [pageNo, cand] : numberedPages) {
                if (count >= 12) break;
                appendEntryImage(cand.entryIdx);
                ++count;
            }
            if (result.pageDataUris.empty()) {
                for (size_t idx : fallbackThumbIndices) {
                    appendEntryImage(idx);
                    if (!result.pageDataUris.empty()) break;
                }
            }

            if (!result.pageDataUris.empty()) {
                result.ok = true;
                result.metaSummary = "CorelDRAW (ZIP)";
                if (result.width > 0 && result.height > 0) {
                    result.metaSummary += " · " + std::to_string(result.width) + "×" + std::to_string(result.height);
                }
                if (result.pageDataUris.size() > 1) {
                    result.metaSummary += " · " + std::to_string(result.pageDataUris.size()) + " pages";
                }
            }
        }
    }
    // Case B: Legacy RIFF CDR Container (X3 and earlier: "RIFF....CDR*")
    else if (raw.size() >= 12 && std::memcmp(raw.data(), "RIFF", 4) == 0 &&
             (std::memcmp(raw.data() + 8, "CDR", 3) == 0 || std::memcmp(raw.data() + 8, "cdr", 3) == 0)) {
        std::string uri;
        uint32_t w = 0, h = 0;
        if (scanRiffForDispBitmap(raw.data(), raw.size(), 12, raw.size(), 0, uri, w, h) && !uri.empty()) {
            result.ok = true;
            result.isSnapshot = true;
            result.width = w;
            result.height = h;
            result.dataUri = std::move(uri);
            result.pageDataUris = {result.dataUri};
            result.metaSummary = "CorelDRAW (RIFF) · " + std::to_string(w) + "×" + std::to_string(h);
        }
    }

    if (allowShellFallback && (!result.ok || ((std::max)(result.width, result.height) < 320 && result.pageDataUris.size() <= 1))) {
        auto shell = extractShellThumbnailPng(filePath, 1600);
        if (shell.ok &&
            (static_cast<uint64_t>(shell.width) * shell.height >
             static_cast<uint64_t>(result.width) * result.height)) {
            result.ok = true;
            result.isSnapshot = true;
            result.width = shell.width;
            result.height = shell.height;
            result.dataUri = std::move(shell.dataUri);
            result.pageDataUris = {result.dataUri};
            result.metaSummary = "CorelDRAW · " + std::to_string(shell.width) + "×" + std::to_string(shell.height);
        }
    }

    return result;
}

// ── 4. AutoCAD / TArch (.dwg), SolidWorks (.sldprt/.sldasm), SketchUp (.skp) ─

namespace {

std::string resolveDwgVersionLabel(std::string_view ver6) {
    if (ver6 == "AC1012") return "AutoCAD R13";
    if (ver6 == "AC1014") return "AutoCAD R14";
    if (ver6 == "AC1015") return "AutoCAD 2000";
    if (ver6 == "AC1018") return "AutoCAD 2004";
    if (ver6 == "AC1021") return "AutoCAD 2007";
    if (ver6 == "AC1024") return "AutoCAD 2010";
    if (ver6 == "AC1027") return "AutoCAD 2013";
    if (ver6 == "AC1032") return "AutoCAD 2018+ / TArch";
    if (ver6.size() == 6 && ver6.rfind("AC10", 0) == 0) {
        return "AutoCAD (" + std::string(ver6) + ")";
    }
    return "AutoCAD DWG";
}

DesignImagePreviewResult extractDwgHeaderSnapshot(const std::wstring& filePath) {
    DesignImagePreviewResult res;
    res.isSnapshot = true;

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return res;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 32) {
        CloseHandle(hFile);
        return res;
    }
    const uint64_t totalFileSize = static_cast<uint64_t>(fileSize.QuadPart);

    uint8_t header[64] = {};
    DWORD headerRead = 0;
    if (!ReadFile(hFile, header, static_cast<DWORD>((std::min<uint64_t>)(totalFileSize, sizeof(header))), &headerRead, nullptr) ||
        headerRead < 24 || header[0] != 'A' || header[1] != 'C') {
        CloseHandle(hFile);
        return res;
    }

    std::string_view verTag(reinterpret_cast<const char*>(header), 6);
    res.formatLabel = resolveDwgVersionLabel(verTag);
    res.metaSummary = res.formatLabel;

    const uint32_t imageSeeker = readLe32(header + 0x0D);
    uint64_t readStart = 0;
    size_t readLen = 0;
    if (imageSeeker >= 0x14 && imageSeeker + 24ULL < totalFileSize) {
        readStart = imageSeeker;
        readLen = static_cast<size_t>((std::min<uint64_t>)(totalFileSize - imageSeeker, 4ULL * 1024ULL * 1024ULL));
    } else {
        readStart = 0;
        readLen = static_cast<size_t>((std::min<uint64_t>)(totalFileSize, 1024ULL * 1024ULL));
    }

    std::vector<uint8_t> previewBuf(readLen);
    LARGE_INTEGER seekPos{};
    seekPos.QuadPart = static_cast<LONGLONG>(readStart);
    DWORD bytesRead = 0;
    const bool seekOk = SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN) != FALSE &&
                        ReadFile(hFile, previewBuf.data(), static_cast<DWORD>(readLen), &bytesRead, nullptr);
    CloseHandle(hFile); // Immediate zero-lock release!

    if (!seekOk || bytesRead < 30) {
        return res;
    }
    previewBuf.resize(bytesRead);

    size_t sentinelLocal = std::string::npos;
    if (previewBuf.size() >= 16 && std::memcmp(previewBuf.data(), kDwgPreviewSentinel, 16) == 0) {
        sentinelLocal = 0;
    } else {
        auto it = std::search(
            previewBuf.begin(), previewBuf.end(),
            std::begin(kDwgPreviewSentinel), std::end(kDwgPreviewSentinel));
        if (it != previewBuf.end()) {
            sentinelLocal = static_cast<size_t>(std::distance(previewBuf.begin(), it));
        }
    }
    if (sentinelLocal == std::string::npos || sentinelLocal + 21 > previewBuf.size()) {
        return res;
    }

    const uint64_t sentinelAbsFileOffset = readStart + sentinelLocal;
    const uint8_t numEntries = previewBuf[sentinelLocal + 20];
    size_t dirPos = sentinelLocal + 21;

    uint64_t bestPixels = 0;
    for (uint8_t i = 0; i < numEntries; ++i) {
        if (dirPos + 9 > previewBuf.size()) break;
        const uint8_t code = previewBuf[dirPos];
        const uint32_t entryStart = readLe32(previewBuf.data() + dirPos + 1);
        const uint32_t entrySize = readLe32(previewBuf.data() + dirPos + 5);
        dirPos += 9;

        if (entrySize < 16 || entrySize > previewBuf.size()) continue;

        size_t localOffset = std::string::npos;
        if (entryStart >= readStart && (entryStart - readStart) + entrySize <= previewBuf.size()) {
            localOffset = static_cast<size_t>(entryStart - readStart);
        } else if (entryStart < sentinelAbsFileOffset && sentinelLocal + entryStart + entrySize <= previewBuf.size()) {
            localOffset = sentinelLocal + entryStart;
        }
        if (localOffset == std::string::npos) continue;

        const uint8_t* imgPtr = previewBuf.data() + localOffset;
        uint32_t w = 0, h = 0;
        std::string uri;

        if (parsePngDimensions(imgPtr, entrySize, w, h)) {
            uri = encodeBase64DataUri("data:image/png;base64,", imgPtr, entrySize);
        } else if (code == 2 || code == 3 || code == 6) {
            wrapDibAsBmpAndConvertToDataUri(imgPtr, entrySize, uri, w, h);
        }

        const uint64_t area = static_cast<uint64_t>(w) * h;
        if (!uri.empty() && area >= bestPixels) {
            bestPixels = area;
            res.ok = true;
            res.width = w;
            res.height = h;
            res.dataUri = std::move(uri);
            res.pageDataUris = {res.dataUri};
        }
    }

    if (res.ok && res.width > 0 && res.height > 0) {
        res.metaSummary = res.formatLabel + " · " + std::to_string(res.width) + "×" + std::to_string(res.height);
    }
    return res;
}

DesignImagePreviewResult scanBestEmbeddedPngSnapshot(
    const std::wstring& filePath,
    std::string_view formatLabel) {
    DesignImagePreviewResult res;
    res.isSnapshot = true;
    res.formatLabel = std::string(formatLabel);
    res.metaSummary = res.formatLabel;

    std::vector<uint8_t> raw;
    uint64_t actualBytes = 0;
    bool prefixOnly = false;
    if (!readFileZeroLock(filePath, kMaxCadScanBytes, static_cast<size_t>(kMaxCadScanBytes), raw, &actualBytes, &prefixOnly)) {
        return res;
    }

    uint64_t bestArea = 0;
    size_t searchPos = 0;
    while (searchPos + 33 <= raw.size()) {
        auto it = std::search(
            raw.begin() + static_cast<std::ptrdiff_t>(searchPos),
            raw.end(),
            std::begin(kPngSignature),
            std::end(kPngSignature));
        if (it == raw.end()) break;

        const size_t hitPos = static_cast<size_t>(std::distance(raw.begin(), it));
        size_t spanLen = 0;
        uint32_t w = 0, h = 0;
        if (findValidEmbeddedPngSpan(raw.data(), raw.size(), hitPos, spanLen, w, h)) {
            const uint64_t area = static_cast<uint64_t>(w) * h;
            if (area > bestArea) {
                std::string uri = encodeBase64DataUri("data:image/png;base64,", raw.data() + hitPos, spanLen);
                if (!uri.empty()) {
                    bestArea = area;
                    res.ok = true;
                    res.width = w;
                    res.height = h;
                    res.dataUri = std::move(uri);
                    res.pageDataUris = {res.dataUri};
                }
            }
            searchPos = hitPos + spanLen;
        } else {
            searchPos = hitPos + 8;
        }
    }

    if (res.ok && res.width > 0 && res.height > 0) {
        res.metaSummary = res.formatLabel + " · " + std::to_string(res.width) + "×" + std::to_string(res.height);
    }
    return res;
}

DesignImagePreviewResult extractDosEpsTiffPreview(const std::wstring& filePath) {
    DesignImagePreviewResult res;
    res.isSnapshot = true;
    res.formatLabel = "EPS";
    res.metaSummary = "EPS";

    std::vector<uint8_t> raw;
    if (!readFileZeroLock(filePath, 16ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL, raw) || raw.size() < 30) {
        return res;
    }
    // DOS EPS Binary File Header magic: C5 D0 D3 C6
    if (raw[0] != 0xC5 || raw[1] != 0xD0 || raw[2] != 0xD3 || raw[3] != 0xC6) {
        return res;
    }
    const uint32_t tiffOffset = readLe32(raw.data() + 20);
    const uint32_t tiffLength = readLe32(raw.data() + 24);
    if (tiffOffset == 0 || tiffLength < 16 || static_cast<uint64_t>(tiffOffset) + tiffLength > raw.size()) {
        return res;
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCo);

    IWICImagingFactory* pFactory = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))) && pFactory) {
        IWICStream* pStream = nullptr;
        if (SUCCEEDED(pFactory->CreateStream(&pStream)) && pStream) {
            if (SUCCEEDED(pStream->InitializeFromMemory(raw.data() + tiffOffset, tiffLength))) {
                IWICBitmapDecoder* pDecoder = nullptr;
                if (SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder)) && pDecoder) {
                    IWICBitmapFrameDecode* pFrame = nullptr;
                    if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame)) && pFrame) {
                        UINT w = 0, h = 0;
                        if (SUCCEEDED(pFrame->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
                            IWICFormatConverter* pConv = nullptr;
                            if (SUCCEEDED(pFactory->CreateFormatConverter(&pConv)) && pConv) {
                                if (SUCCEEDED(pConv->Initialize(
                                        pFrame, GUID_WICPixelFormat32bppRGBA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                                    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
                                    if (SUCCEEDED(pConv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(rgba.size()), rgba.data()))) {
                                        std::string uri = encodeRgbaToPngDataUri(rgba.data(), w, h);
                                        if (!uri.empty()) {
                                            res.ok = true;
                                            res.width = w;
                                            res.height = h;
                                            res.dataUri = std::move(uri);
                                            res.pageDataUris = {res.dataUri};
                                            res.metaSummary = "EPS · " + std::to_string(w) + "×" + std::to_string(h);
                                        }
                                    }
                                }
                                pConv->Release();
                            }
                        }
                        pFrame->Release();
                    }
                    pDecoder->Release();
                }
            }
            pStream->Release();
        }
        pFactory->Release();
    }

    if (needUninit) CoUninitialize();
    return res;
}

} // namespace

DesignImagePreviewResult DesignAndCadPreview::extractCadOr3dSnapshotPreview(
    const std::wstring& filePath,
    const std::string& lowerExt,
    bool allowShellFallback) {
    DesignImagePreviewResult res;

    if (lowerExt == "dwg") {
        res = extractDwgHeaderSnapshot(filePath);
    } else if (lowerExt == "sldprt") {
        res = scanBestEmbeddedPngSnapshot(filePath, "SolidWorks Part");
    } else if (lowerExt == "sldasm") {
        res = scanBestEmbeddedPngSnapshot(filePath, "SolidWorks Assembly");
    } else if (lowerExt == "skp") {
        res = scanBestEmbeddedPngSnapshot(filePath, "SketchUp Model");
    } else if (lowerExt == "eps" || lowerExt == "ai") {
        res = extractDosEpsTiffPreview(filePath);
        if (res.formatLabel.empty()) {
            res.formatLabel = (lowerExt == "ai") ? "Illustrator" : "EPS";
        }
    } else {
        res.formatLabel = asciiToUpper(lowerExt);
    }

    if (allowShellFallback && (!res.ok || (std::max)(res.width, res.height) < 320)) {
        auto shell = extractShellThumbnailPng(filePath, 1600);
        if (shell.ok &&
            (static_cast<uint64_t>(shell.width) * shell.height >
             static_cast<uint64_t>(res.width) * res.height)) {
            const std::string keepLabel = res.formatLabel.empty() ? asciiToUpper(lowerExt) : res.formatLabel;
            res.ok = true;
            res.isSnapshot = true;
            res.width = shell.width;
            res.height = shell.height;
            res.formatLabel = keepLabel;
            res.metaSummary = keepLabel + " · " + std::to_string(shell.width) + "×" + std::to_string(shell.height);
            res.dataUri = std::move(shell.dataUri);
            res.pageDataUris = {res.dataUri};
        }
    }

    return res;
}

// ── 5. AutoCAD .dxf Interactive Vector Parser (UTF-8 & GBK Zero-Garble) ──────

DxfVectorPreviewResult DesignAndCadPreview::extractDxfVectorPreview(
    const std::wstring& filePath,
    size_t maxEntities) {
    DxfVectorPreviewResult result;
    std::vector<uint8_t> raw;
    uint64_t actualBytes = 0;
    bool prefixOnly = false;
    if (!readFileZeroLock(filePath, kMaxDxfBytes, static_cast<size_t>(kMaxDxfBytes), raw, &actualBytes, &prefixOnly)) {
        return result;
    }
    if (prefixOnly) {
        result.truncated = true;
    }

    // Split lines view-wise without allocating per line
    std::vector<std::string_view> lines;
    lines.reserve(raw.size() / 12u + 16u);
    size_t lineStart = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            size_t len = i - lineStart;
            if (len > 0 && raw[lineStart + len - 1] == '\r') --len;
            lines.emplace_back(reinterpret_cast<const char*>(raw.data() + lineStart), len);
            lineStart = i + 1;
        }
    }
    if (lineStart < raw.size()) {
        size_t len = raw.size() - lineStart;
        if (len > 0 && raw[lineStart + len - 1] == '\r') --len;
        lines.emplace_back(reinterpret_cast<const char*>(raw.data() + lineStart), len);
    }

    // Align to first valid integer group code line (usually 0 or 999)
    size_t pairIdx = 0;
    for (size_t k = 0; k < (std::min<size_t>)(lines.size(), 8u); ++k) {
        int probeCode = -1;
        if (parseIntFast(lines[k], probeCode) && (probeCode == 0 || probeCode == 999)) {
            pairIdx = k;
            break;
        }
    }

    bool usedGbk = false;
    std::unordered_map<std::string, int> layerAciTable;
    std::unordered_map<std::string, size_t> layerIndexByName;

    enum class DxfSection { None, Tables, Entities, Other };
    DxfSection currentSection = DxfSection::None;
    bool pendingSectionName = false;
    bool inLayerTableRecord = false;
    std::string curTableLayerName;
    int curTableLayerAci = 7;

    struct ActiveEntity {
        std::string type;
        std::string layer = "0";
        int aciColor = 256; // 256 = BYLAYER
        int flags70 = 0;
        double x1 = 0.0, y1 = 0.0;
        double x2 = 0.0, y2 = 0.0;
        bool hasX2 = false, hasY2 = false;
        double radius = 0.0;
        double startAng = 0.0;
        double endAng = 360.0;
        double height40 = 2.5;
        double rot50 = 0.0;
        bool hasRot50 = false;
        std::string textAcc;
        std::string textTail;
        // LWPOLYLINE vertices & bulges
        std::vector<std::pair<double, double>> lwPts;
        std::vector<double> lwBulges;
        bool lwHasPendingX = false;
        double lwPendingX = 0.0;
    };

    ActiveEntity ent;
    bool hasActiveEnt = false;
    bool inClassicPolyline = false;
    ActiveEntity polyParent;
    std::vector<std::pair<double, double>> polyVertices;
    std::vector<double> polyBulges;

    double minX = (std::numeric_limits<double>::infinity)();
    double minY = (std::numeric_limits<double>::infinity)();
    double maxX = -(std::numeric_limits<double>::infinity)();
    double maxY = -(std::numeric_limits<double>::infinity)();

    auto expandBounds = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e11 || std::abs(y) > 1e11) return;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
    };

    auto registerAndResolveColor = [&](const std::string& layerName, int entAci, std::string& outHex, bool& outAdaptive) {
        const std::string safeLayer = layerName.empty() ? "0" : layerName;
        int effectiveAci = entAci;
        if (effectiveAci == 256 || effectiveAci == 0) {
            auto it = layerAciTable.find(safeLayer);
            effectiveAci = (it != layerAciTable.end()) ? it->second : 7;
        }
        outHex = aciColorToHex(effectiveAci, outAdaptive);

        auto idxIt = layerIndexByName.find(safeLayer);
        if (idxIt == layerIndexByName.end()) {
            bool layerAdaptive = true;
            int layerDefAci = 7;
            auto tblIt = layerAciTable.find(safeLayer);
            if (tblIt != layerAciTable.end()) layerDefAci = tblIt->second;
            std::string layerHex = aciColorToHex(layerDefAci, layerAdaptive);

            layerIndexByName[safeLayer] = result.layers.size();
            result.layers.push_back(DxfLayerInfo{safeLayer, std::move(layerHex), layerAdaptive, 1u});
        } else {
            result.layers[idxIt->second].entityCount += 1u;
        }
    };

    auto pushPrimitive = [&](DxfEntityPrimitive&& prim) {
        ++result.totalEntities;
        if (result.entities.size() < maxEntities) {
            result.entities.push_back(std::move(prim));
        } else {
            result.truncated = true;
        }
    };

    auto flushEntity = [&]() {
        if (!hasActiveEnt) return;
        hasActiveEnt = false;

        if (ent.lwHasPendingX) {
            ent.lwPts.emplace_back(ent.lwPendingX, 0.0);
            ent.lwBulges.push_back(0.0);
            ent.lwHasPendingX = false;
        }

        if (ent.type == "POLYLINE") {
            inClassicPolyline = true;
            polyParent = ent;
            polyVertices.clear();
            polyBulges.clear();
            return;
        }
        if (ent.type == "VERTEX") {
            if (inClassicPolyline && (ent.flags70 & 16) == 0) {
                polyVertices.emplace_back(ent.x1, ent.y1);
                polyBulges.push_back(0.0);
            }
            return;
        }
        if (ent.type == "SEQEND") {
            if (inClassicPolyline && polyVertices.size() >= 2) {
                DxfEntityPrimitive prim;
                prim.kind = DxfEntityKind::Polyline;
                prim.layer = polyParent.layer.empty() ? "0" : polyParent.layer;
                prim.closed = (polyParent.flags70 & 1) != 0;
                registerAndResolveColor(prim.layer, polyParent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);
                prim.coords.reserve(polyVertices.size() * 2u);
                for (const auto& [vx, vy] : polyVertices) {
                    prim.coords.push_back(vx);
                    prim.coords.push_back(vy);
                    expandBounds(vx, vy);
                }
                pushPrimitive(std::move(prim));
            }
            inClassicPolyline = false;
            polyVertices.clear();
            polyBulges.clear();
            return;
        }

        if (ent.type == "LINE") {
            DxfEntityPrimitive prim;
            prim.kind = DxfEntityKind::Line;
            prim.layer = ent.layer.empty() ? "0" : ent.layer;
            registerAndResolveColor(prim.layer, ent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);
            prim.coords = {ent.x1, ent.y1, ent.x2, ent.y2};
            expandBounds(ent.x1, ent.y1);
            expandBounds(ent.x2, ent.y2);
            pushPrimitive(std::move(prim));
        } else if (ent.type == "LWPOLYLINE" || ent.type == "SPLINE") {
            if (ent.lwPts.size() >= 2) {
                DxfEntityPrimitive prim;
                prim.kind = DxfEntityKind::Polyline;
                prim.layer = ent.layer.empty() ? "0" : ent.layer;
                const bool closed = (ent.flags70 & 1) != 0;
                prim.closed = closed;
                registerAndResolveColor(prim.layer, ent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);

                prim.coords.reserve(ent.lwPts.size() * 4u);
                prim.coords.push_back(ent.lwPts[0].first);
                prim.coords.push_back(ent.lwPts[0].second);
                for (size_t k = 0; k + 1 < ent.lwPts.size(); ++k) {
                    const double b = (k < ent.lwBulges.size()) ? ent.lwBulges[k] : 0.0;
                    appendBulgeArcPoints(
                        ent.lwPts[k].first, ent.lwPts[k].second,
                        ent.lwPts[k + 1].first, ent.lwPts[k + 1].second,
                        b, prim.coords);
                }
                if (closed && ent.lwPts.size() >= 2) {
                    const size_t last = ent.lwPts.size() - 1;
                    const double b = (last < ent.lwBulges.size()) ? ent.lwBulges[last] : 0.0;
                    if (std::abs(b) > 1e-4) {
                        appendBulgeArcPoints(
                            ent.lwPts[last].first, ent.lwPts[last].second,
                            ent.lwPts[0].first, ent.lwPts[0].second,
                            b, prim.coords);
                        prim.closed = false; // Explicitly closed via arc segment
                    }
                }
                for (size_t k = 0; k + 1 < prim.coords.size(); k += 2) {
                    expandBounds(prim.coords[k], prim.coords[k + 1]);
                }
                pushPrimitive(std::move(prim));
            }
        } else if (ent.type == "CIRCLE" && ent.radius > 1e-9) {
            DxfEntityPrimitive prim;
            prim.kind = DxfEntityKind::Circle;
            prim.layer = ent.layer.empty() ? "0" : ent.layer;
            registerAndResolveColor(prim.layer, ent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);
            prim.coords = {ent.x1, ent.y1};
            prim.radius = ent.radius;
            expandBounds(ent.x1 - ent.radius, ent.y1 - ent.radius);
            expandBounds(ent.x1 + ent.radius, ent.y1 + ent.radius);
            pushPrimitive(std::move(prim));
        } else if (ent.type == "ARC" && ent.radius > 1e-9) {
            DxfEntityPrimitive prim;
            prim.kind = DxfEntityKind::Arc;
            prim.layer = ent.layer.empty() ? "0" : ent.layer;
            registerAndResolveColor(prim.layer, ent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);
            prim.coords = {ent.x1, ent.y1};
            prim.radius = ent.radius;
            prim.startAngleDeg = ent.startAng;
            prim.endAngleDeg = ent.endAng;

            // Exact arc bounding box sampling
            double a0 = std::fmod(ent.startAng, 360.0);
            if (a0 < 0) a0 += 360.0;
            double a1 = std::fmod(ent.endAng, 360.0);
            if (a1 < 0) a1 += 360.0;
            double sweep = a1 - a0;
            if (sweep <= 1e-6) sweep += 360.0;
            for (int s = 0; s <= 16; ++s) {
                const double rad = (a0 + sweep * (s / 16.0)) * (kPi / 180.0);
                expandBounds(ent.x1 + ent.radius * std::cos(rad), ent.y1 + ent.radius * std::sin(rad));
            }
            pushPrimitive(std::move(prim));
        } else if (ent.type == "TEXT" || ent.type == "MTEXT") {
            std::string fullRaw = ent.textAcc + ent.textTail;
            if (ent.type == "MTEXT") {
                fullRaw = cleanDxfMText(fullRaw);
            }
            while (!fullRaw.empty() && (fullRaw.back() == '\r' || fullRaw.back() == '\n' || fullRaw.back() == ' ')) {
                fullRaw.pop_back();
            }
            if (!fullRaw.empty()) {
                DxfEntityPrimitive prim;
                prim.kind = DxfEntityKind::Text;
                prim.layer = ent.layer.empty() ? "0" : ent.layer;
                registerAndResolveColor(prim.layer, ent.aciColor, prim.colorHex, prim.adaptiveDefaultColor);
                prim.coords = {ent.x1, ent.y1};
                prim.textHeight = (ent.height40 > 1e-6) ? ent.height40 : 2.5;
                if (!ent.hasRot50 && ent.type == "MTEXT" && (ent.hasX2 || ent.hasY2) &&
                    std::hypot(ent.x2, ent.y2) > 1e-6) {
                    prim.rotationDeg = std::atan2(ent.y2, ent.x2) * (180.0 / kPi);
                } else {
                    prim.rotationDeg = ent.rot50;
                }
                prim.text = std::move(fullRaw);
                expandBounds(ent.x1, ent.y1);
                expandBounds(
                    ent.x1 + prim.textHeight * static_cast<double>((std::min<size_t>)(prim.text.size(), 24u)) * 0.55,
                    ent.y1 + prim.textHeight);
                pushPrimitive(std::move(prim));
            }
        }
    };

    for (; pairIdx + 1 < lines.size(); pairIdx += 2) {
        int code = -1;
        if (!parseIntFast(lines[pairIdx], code)) {
            continue;
        }
        std::string_view valView = lines[pairIdx + 1];
        std::string_view trimmedVal = valView;
        while (!trimmedVal.empty() && std::isspace(static_cast<unsigned char>(trimmedVal.front()))) trimmedVal.remove_prefix(1);
        while (!trimmedVal.empty() && std::isspace(static_cast<unsigned char>(trimmedVal.back()))) trimmedVal.remove_suffix(1);

        if (code == 0) {
            const std::string upperToken = asciiToUpper(trimmedVal);
            if (currentSection == DxfSection::Tables && inLayerTableRecord) {
                if (!curTableLayerName.empty()) {
                    layerAciTable[curTableLayerName] = curTableLayerAci;
                }
                inLayerTableRecord = false;
            }
            if (hasActiveEnt) {
                flushEntity();
            }

            if (upperToken == "SECTION") {
                pendingSectionName = true;
                continue;
            }
            if (upperToken == "ENDSEC") {
                currentSection = DxfSection::None;
                pendingSectionName = false;
                continue;
            }
            if (upperToken == "EOF") {
                break;
            }

            if (currentSection == DxfSection::Tables) {
                if (upperToken == "LAYER") {
                    inLayerTableRecord = true;
                    curTableLayerName.clear();
                    curTableLayerAci = 7;
                }
                continue;
            }

            // Support both standard ENTITIES section and minimal headerless DXF snippets
            if (currentSection == DxfSection::Entities || currentSection == DxfSection::None) {
                if (upperToken == "LINE" || upperToken == "LWPOLYLINE" || upperToken == "POLYLINE" ||
                    upperToken == "VERTEX" || upperToken == "SEQEND" || upperToken == "CIRCLE" ||
                    upperToken == "ARC" || upperToken == "TEXT" || upperToken == "MTEXT" ||
                    upperToken == "SPLINE") {
                    ent = ActiveEntity{};
                    ent.type = upperToken;
                    hasActiveEnt = true;
                }
            }
            continue;
        }

        if (pendingSectionName && code == 2) {
            pendingSectionName = false;
            const std::string secName = asciiToUpper(trimmedVal);
            if (secName == "TABLES") currentSection = DxfSection::Tables;
            else if (secName == "ENTITIES") currentSection = DxfSection::Entities;
            else currentSection = DxfSection::Other;
            continue;
        }

        if (currentSection == DxfSection::Tables && inLayerTableRecord) {
            if (code == 2) {
                curTableLayerName = decodeDxfBytesToUtf8(trimmedVal, usedGbk);
            } else if (code == 62) {
                parseIntFast(trimmedVal, curTableLayerAci);
            }
            continue;
        }

        if (!hasActiveEnt) continue;

        switch (code) {
            case 8:
                ent.layer = decodeDxfBytesToUtf8(trimmedVal, usedGbk);
                break;
            case 62:
                parseIntFast(trimmedVal, ent.aciColor);
                break;
            case 70:
                parseIntFast(trimmedVal, ent.flags70);
                break;
            case 10: {
                double v = 0.0;
                if (parseDoubleFast(trimmedVal, v)) {
                    if (ent.type == "LWPOLYLINE" || ent.type == "SPLINE") {
                        if (ent.lwHasPendingX) {
                            ent.lwPts.emplace_back(ent.lwPendingX, 0.0);
                            ent.lwBulges.push_back(0.0);
                        }
                        ent.lwPendingX = v;
                        ent.lwHasPendingX = true;
                    } else {
                        ent.x1 = v;
                    }
                }
                break;
            }
            case 20: {
                double v = 0.0;
                if (parseDoubleFast(trimmedVal, v)) {
                    if (ent.type == "LWPOLYLINE" || ent.type == "SPLINE") {
                        const double px = ent.lwHasPendingX ? ent.lwPendingX : 0.0;
                        ent.lwPts.emplace_back(px, v);
                        ent.lwBulges.push_back(0.0);
                        ent.lwHasPendingX = false;
                    } else {
                        ent.y1 = v;
                    }
                }
                break;
            }
            case 42: {
                if (ent.type == "LWPOLYLINE") {
                    double b = 0.0;
                    if (parseDoubleFast(trimmedVal, b) && !ent.lwBulges.empty()) {
                        ent.lwBulges.back() = b;
                    }
                }
                break;
            }
            case 11:
                if (parseDoubleFast(trimmedVal, ent.x2)) ent.hasX2 = true;
                break;
            case 21:
                if (parseDoubleFast(trimmedVal, ent.y2)) ent.hasY2 = true;
                break;
            case 40:
                if (ent.type == "CIRCLE" || ent.type == "ARC") {
                    parseDoubleFast(trimmedVal, ent.radius);
                } else if (ent.type == "TEXT" || ent.type == "MTEXT") {
                    parseDoubleFast(trimmedVal, ent.height40);
                }
                break;
            case 50:
                if (ent.type == "ARC") {
                    parseDoubleFast(trimmedVal, ent.startAng);
                } else if (ent.type == "TEXT" || ent.type == "MTEXT") {
                    if (parseDoubleFast(trimmedVal, ent.rot50)) ent.hasRot50 = true;
                }
                break;
            case 51:
                if (ent.type == "ARC") {
                    parseDoubleFast(trimmedVal, ent.endAng);
                }
                break;
            case 3:
                if (ent.type == "MTEXT") {
                    ent.textAcc += decodeDxfBytesToUtf8(valView, usedGbk);
                }
                break;
            case 1:
                if (ent.type == "TEXT" || ent.type == "MTEXT") {
                    ent.textTail = decodeDxfBytesToUtf8(valView, usedGbk);
                }
                break;
            default:
                break;
        }
    }

    if (hasActiveEnt) {
        flushEntity();
    }

    if (result.entities.empty()) {
        return result;
    }

    if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX) || !std::isfinite(maxY)) {
        minX = 0.0; minY = 0.0; maxX = 100.0; maxY = 100.0;
    }
    if (maxX - minX < 1e-6) { minX -= 5.0; maxX += 5.0; }
    if (maxY - minY < 1e-6) { minY -= 5.0; maxY += 5.0; }

    result.ok = true;
    result.encoding = usedGbk ? "GBK" : "UTF-8";
    result.bounds = {minX, minY, maxX, maxY};
    return result;
}

// ── 6. 3D Mesh (.stl Binary/ASCII & .obj) Parser & Normalizer ───────────────

namespace {

struct RawTriangle {
    float v[9];
};

Mesh3dPreviewResult finalizeNormalizedMesh(
    const std::vector<RawTriangle>& allTris,
    size_t maxTriangles,
    bool isBinary,
    std::string formatLabel) {
    Mesh3dPreviewResult res;
    if (allTris.empty()) return res;

    double minX = (std::numeric_limits<double>::infinity)();
    double minY = (std::numeric_limits<double>::infinity)();
    double minZ = (std::numeric_limits<double>::infinity)();
    double maxX = -(std::numeric_limits<double>::infinity)();
    double maxY = -(std::numeric_limits<double>::infinity)();
    double maxZ = -(std::numeric_limits<double>::infinity)();

    for (const auto& t : allTris) {
        for (int k = 0; k < 9; k += 3) {
            const double x = t.v[k];
            const double y = t.v[k + 1];
            const double z = t.v[k + 2];
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            if (x < minX) minX = x;
            if (y < minY) minY = y;
            if (z < minZ) minZ = z;
            if (x > maxX) maxX = x;
            if (y > maxY) maxY = y;
            if (z > maxZ) maxZ = z;
        }
    }
    if (!std::isfinite(minX) || !std::isfinite(maxX)) return res;

    const double dimX = maxX - minX;
    const double dimY = maxY - minY;
    const double dimZ = maxZ - minZ;
    const double cx = 0.5 * (minX + maxX);
    const double cy = 0.5 * (minY + maxY);
    const double cz = 0.5 * (minZ + maxZ);
    const double maxSpan = (std::max)({dimX, dimY, dimZ, 1e-6});
    const double invHalfSpan = 2.0 / maxSpan;

    const size_t total = allTris.size();
    const size_t keep = (std::min)(total, (std::max<size_t>)(1u, maxTriangles));

    res.ok = true;
    res.isBinary = isBinary;
    res.formatLabel = std::move(formatLabel);
    res.totalTriangles = static_cast<uint32_t>(total);
    res.sampledTriangles = static_cast<uint32_t>(keep);
    res.truncated = (total > keep);
    res.dimensions = {dimX, dimY, dimZ};
    res.positions.resize(keep * 9u);

    for (size_t i = 0; i < keep; ++i) {
        const size_t srcIdx = (total > keep)
            ? static_cast<size_t>((static_cast<uint64_t>(i) * total) / keep)
            : i;
        const RawTriangle& tri = allTris[srcIdx];
        float* dst = res.positions.data() + i * 9u;
        for (int k = 0; k < 3; ++k) {
            const float nx = static_cast<float>((tri.v[k * 3 + 0] - cx) * invHalfSpan);
            const float ny = static_cast<float>((tri.v[k * 3 + 1] - cy) * invHalfSpan);
            const float nz = static_cast<float>((tri.v[k * 3 + 2] - cz) * invHalfSpan);
            // Round to 4 decimal places for compact IPC JSON serialization without visual loss
            dst[k * 3 + 0] = std::round(nx * 10000.0f) * 0.0001f;
            dst[k * 3 + 1] = std::round(ny * 10000.0f) * 0.0001f;
            dst[k * 3 + 2] = std::round(nz * 10000.0f) * 0.0001f;
        }
    }

    return res;
}

bool isLikelyBinaryStl(const std::vector<uint8_t>& raw) noexcept {
    if (raw.size() < 84) return false;
    const uint32_t triCount = readLe32(raw.data() + 80);
    if (triCount == 0 || triCount > 10000000u) return false;
    const uint64_t expectedBytes = 84ULL + static_cast<uint64_t>(triCount) * 50ULL;
    if (expectedBytes == raw.size()) {
        return true;
    }
    // Check if there are null bytes after header and expectedBytes fits inside buffer
    if (expectedBytes <= raw.size()) {
        const size_t checkEnd = (std::min<size_t>)(raw.size(), 512u);
        for (size_t i = 84; i < checkEnd; ++i) {
            if (raw[i] == 0) return true;
        }
    }
    return false;
}

} // namespace

Mesh3dPreviewResult DesignAndCadPreview::extract3dMeshPreview(
    const std::wstring& filePath,
    const std::string& lowerExt,
    size_t maxTriangles) {
    Mesh3dPreviewResult empty;
    std::vector<uint8_t> raw;
    if (!readFileZeroLock(filePath, kMaxMeshBytes, 0, raw) || raw.size() < 12) {
        return empty;
    }

    if (lowerExt == "stl") {
        if (isLikelyBinaryStl(raw)) {
            const uint32_t triCount = readLe32(raw.data() + 80);
            const size_t availTris = (std::min<size_t>)(triCount, (raw.size() - 84u) / 50u);
            std::vector<RawTriangle> tris;
            tris.reserve(availTris);
            for (size_t i = 0; i < availTris; ++i) {
                const uint8_t* rec = raw.data() + 84u + i * 50u;
                RawTriangle t{};
                // Skip 12-byte facet normal (rec + 0..11), read 3 vertices (rec + 12..47)
                std::memcpy(t.v, rec + 12, 36);
                bool valid = true;
                for (float val : t.v) {
                    if (!std::isfinite(val)) { valid = false; break; }
                }
                if (valid) tris.push_back(t);
            }
            return finalizeNormalizedMesh(tris, maxTriangles, true, "STL (Binary)");
        }

        // ASCII STL parser
        std::vector<RawTriangle> tris;
        RawTriangle cur{};
        int vIdx = 0;
        std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t nl = text.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? text.substr(pos)
                : text.substr(pos, nl - pos);
            pos = (nl == std::string_view::npos) ? text.size() : (nl + 1);

            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
            if (line.size() >= 7 &&
                (line.compare(0, 6, "vertex") == 0 ||
                 WinUtils::toLower(std::string(line.substr(0, 6))) == "vertex") &&
                std::isspace(static_cast<unsigned char>(line[6]))) {
                std::string_view rest = line.substr(7);
                double coords[3] = {};
                bool ok = true;
                for (int k = 0; k < 3; ++k) {
                    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
                    size_t tokEnd = 0;
                    while (tokEnd < rest.size() && !std::isspace(static_cast<unsigned char>(rest[tokEnd]))) ++tokEnd;
                    if (tokEnd == 0 || !parseDoubleFast(rest.substr(0, tokEnd), coords[k])) {
                        ok = false;
                        break;
                    }
                    rest.remove_prefix(tokEnd);
                }
                if (ok) {
                    cur.v[vIdx * 3 + 0] = static_cast<float>(coords[0]);
                    cur.v[vIdx * 3 + 1] = static_cast<float>(coords[1]);
                    cur.v[vIdx * 3 + 2] = static_cast<float>(coords[2]);
                    ++vIdx;
                    if (vIdx == 3) {
                        tris.push_back(cur);
                        vIdx = 0;
                        if (tris.size() >= 2000000u) break;
                    }
                }
            }
        }
        return finalizeNormalizedMesh(tris, maxTriangles, false, "STL (ASCII)");
    }

    if (lowerExt == "obj") {
        std::vector<std::array<float, 3>> vertices;
        vertices.reserve(8192);
        std::vector<RawTriangle> tris;
        tris.reserve(8192);

        std::string_view text(reinterpret_cast<const char*>(raw.data()), raw.size());
        size_t pos = 0;
        std::vector<size_t> faceVerts;
        faceVerts.reserve(8);

        while (pos < text.size()) {
            const size_t nl = text.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? text.substr(pos)
                : text.substr(pos, nl - pos);
            pos = (nl == std::string_view::npos) ? text.size() : (nl + 1);

            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
            if (line.size() < 3 || line.front() == '#') continue;

            if (line[0] == 'v' && std::isspace(static_cast<unsigned char>(line[1]))) {
                std::string_view rest = line.substr(2);
                double coords[3] = {};
                bool ok = true;
                for (int k = 0; k < 3; ++k) {
                    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
                    size_t tokEnd = 0;
                    while (tokEnd < rest.size() && !std::isspace(static_cast<unsigned char>(rest[tokEnd]))) ++tokEnd;
                    if (tokEnd == 0 || !parseDoubleFast(rest.substr(0, tokEnd), coords[k])) {
                        ok = false;
                        break;
                    }
                    rest.remove_prefix(tokEnd);
                }
                if (ok && vertices.size() < 4000000u) {
                    vertices.push_back({
                        static_cast<float>(coords[0]),
                        static_cast<float>(coords[1]),
                        static_cast<float>(coords[2])
                    });
                }
            } else if (line[0] == 'f' && std::isspace(static_cast<unsigned char>(line[1]))) {
                faceVerts.clear();
                std::string_view rest = line.substr(2);
                while (!rest.empty()) {
                    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.remove_prefix(1);
                    if (rest.empty() || rest.front() == '#') break;
                    size_t tokEnd = 0;
                    while (tokEnd < rest.size() && !std::isspace(static_cast<unsigned char>(rest[tokEnd]))) ++tokEnd;
                    std::string_view tok = rest.substr(0, tokEnd);
                    rest.remove_prefix(tokEnd);

                    const size_t slash = tok.find('/');
                    std::string_view vPart = (slash == std::string_view::npos) ? tok : tok.substr(0, slash);
                    int idx = 0;
                    if (parseIntFast(vPart, idx) && idx != 0) {
                        const ptrdiff_t resolved = (idx > 0)
                            ? static_cast<ptrdiff_t>(idx - 1)
                            : static_cast<ptrdiff_t>(vertices.size()) + static_cast<ptrdiff_t>(idx);
                        if (resolved >= 0 && static_cast<size_t>(resolved) < vertices.size()) {
                            faceVerts.push_back(static_cast<size_t>(resolved));
                        }
                    }
                }
                // Fan triangulation for triangles, quads, and N-gons
                if (faceVerts.size() >= 3) {
                    const auto& v0 = vertices[faceVerts[0]];
                    for (size_t k = 1; k + 1 < faceVerts.size() && tris.size() < 2000000u; ++k) {
                        const auto& v1 = vertices[faceVerts[k]];
                        const auto& v2 = vertices[faceVerts[k + 1]];
                        RawTriangle t{};
                        t.v[0] = v0[0]; t.v[1] = v0[1]; t.v[2] = v0[2];
                        t.v[3] = v1[0]; t.v[4] = v1[1]; t.v[5] = v1[2];
                        t.v[6] = v2[0]; t.v[7] = v2[1]; t.v[8] = v2[2];
                        tris.push_back(t);
                    }
                }
            }
        }
        return finalizeNormalizedMesh(tris, maxTriangles, false, "Wavefront OBJ");
    }

    return empty;
}

} // namespace tools3000::core
