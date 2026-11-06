#include "PlainTextExtractor.h"
#include "ContentMatchUtils.h"
#include "ContentTextCache.h"
#include "../SafeMappedRead.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

constexpr uint64_t MaxPlainTextFileBytes = 100ULL * 1024ULL * 1024ULL;
constexpr size_t SampleInspectionBytes = 8192;
constexpr double MaxRelaxedControlByteRatio = 0.02;
constexpr double MaxUtf16ControlUnitRatio = 0.03;
constexpr size_t MaxRtfExtractedChars = 15ULL * 1024ULL * 1024ULL;
constexpr size_t HugeLineByteThreshold = 16384;
constexpr size_t HugeLineWindowRadius = 384;
constexpr size_t MemchrChunkBytes = 4096;

enum class DetectedEncoding {
    UnknownBinary,
    Utf8,
    Utf16Le,
    Utf16Be,
    Gbk,
    MixedText
};

UINT getGbkCodePage() noexcept {
    static const UINT s_cp = IsValidCodePage(936) ? 936u : (IsValidCodePage(54936) ? 54936u : CP_ACP);
    return s_cp;
}

inline bool isIllegalControlByte(uint8_t b) noexcept {
    if (b == 0x00) return true;
    if (b < 0x20) {
        return (b != 0x09 && b != 0x0A && b != 0x0D && b != 0x0C && b != 0x08 && b != 0x1B);
    }
    return b == 0x7F;
}

bool isStrictUtf8(
    const uint8_t* data,
    size_t length,
    size_t& validMultiByteCount,
    size_t& illegalControlCount
) noexcept {
    size_t i = 0;
    validMultiByteCount = 0;
    illegalControlCount = 0;

    while (i < length) {
        const uint8_t c = data[i];
        if (c <= 0x7F) {
            if (isIllegalControlByte(c)) {
                ++illegalControlCount;
            }
            ++i;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= length) break;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            ++validMultiByteCount;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= length) break;
            const uint8_t c1 = data[i + 1];
            const uint8_t c2 = data[i + 2];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
            if (c == 0xE0 && c1 < 0xA0) return false;
            if (c == 0xED && c1 >= 0xA0) return false;
            ++validMultiByteCount;
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= length) break;
            const uint8_t c1 = data[i + 1];
            const uint8_t c2 = data[i + 2];
            const uint8_t c3 = data[i + 3];
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
            if (c == 0xF0 && c1 < 0x90) return false;
            if (c == 0xF4 && c1 > 0x8F) return false;
            ++validMultiByteCount;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool isStrictGbk(
    const uint8_t* data,
    size_t length,
    size_t& validGbkCount,
    size_t& illegalControlCount
) noexcept {
    size_t i = 0;
    validGbkCount = 0;
    illegalControlCount = 0;

    while (i < length) {
        const uint8_t c = data[i];
        if (c <= 0x7F) {
            if (isIllegalControlByte(c)) {
                ++illegalControlCount;
            }
            ++i;
        } else if (c >= 0x81 && c <= 0xFE) {
            if (i + 1 >= length) break;
            const uint8_t c2 = data[i + 1];
            if ((c2 >= 0x40 && c2 <= 0x7E) || (c2 >= 0x80 && c2 <= 0xFE)) {
                ++validGbkCount;
                i += 2;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

uint16_t readUtf16Unit(const uint8_t* data, size_t unitIndex, bool isBigEndian) noexcept {
    const size_t bytePos = unitIndex * 2;
    if (isBigEndian) {
        return (static_cast<uint16_t>(data[bytePos]) << 8) | static_cast<uint16_t>(data[bytePos + 1]);
    }
    return static_cast<uint16_t>(data[bytePos]) | (static_cast<uint16_t>(data[bytePos + 1]) << 8);
}

bool isUtf16Buffer(const uint8_t* data, size_t length, bool isBigEndian) noexcept {
    if (length < 2 || (length % 2 != 0)) return false;
    const size_t count = length / 2;
    size_t controlCount = 0;
    size_t validCharCount = 0;

    for (size_t i = 0; i < count; ++i) {
        const uint16_t code = readUtf16Unit(data, i, isBigEndian);
        if (code == 0x0000u) return false;

        // Allow valid UTF-16 surrogate pairs (0xD800..0xDBFF followed by 0xDC00..0xDFFF)
        if (code >= 0xD800u && code <= 0xDBFFu) {
            if (i + 1 == count) {
                // Sample truncated right after high surrogate
                break;
            }
            const uint16_t low = readUtf16Unit(data, i + 1, isBigEndian);
            if (low < 0xDC00u || low > 0xDFFFu) {
                return false;
            }
            validCharCount += 2;
            ++i;
            continue;
        }
        if (code >= 0xDC00u && code <= 0xDFFFu) {
            return false;
        }

        if (code < 0x20u) {
            if (code != 0x09u && code != 0x0Au && code != 0x0Du && code != 0x0Cu) {
                ++controlCount;
            }
        } else {
            ++validCharCount;
        }
    }

    if (count > 0 && (static_cast<double>(controlCount) / static_cast<double>(count) > MaxUtf16ControlUnitRatio)) {
        return false;
    }
    return validCharCount > 0;
}

DetectedEncoding detectEncoding(const uint8_t* data, size_t length, size_t& outBomOffset) noexcept {
    outBomOffset = 0;
    if (length == 0) return DetectedEncoding::UnknownBinary;

    // 1. Explicit BOM check
    if (length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        outBomOffset = 3;
        return DetectedEncoding::Utf8;
    }
    if (length >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        outBomOffset = 2;
        return DetectedEncoding::Utf16Le;
    }
    if (length >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        outBomOffset = 2;
        return DetectedEncoding::Utf16Be;
    }

    // 2. Sample inspection (first 8KB)
    size_t sampleLen = (std::min<size_t>)(length, SampleInspectionBytes);

    size_t nullCount = 0;
    size_t illegalControlCount = 0;
    for (size_t i = 0; i < sampleLen; ++i) {
        if (data[i] == 0x00) {
            ++nullCount;
        } else if (isIllegalControlByte(data[i])) {
            ++illegalControlCount;
        }
    }

    if (nullCount > 0) {
        // Align sample length to an even number of bytes if full file is even-sized UTF-16
        const size_t utf16SampleLen = (length % 2 == 0) ? (sampleLen & ~static_cast<size_t>(1)) : sampleLen;
        if (utf16SampleLen >= 2) {
            if (isUtf16Buffer(data, utf16SampleLen, false)) {
                return DetectedEncoding::Utf16Le;
            }
            if (isUtf16Buffer(data, utf16SampleLen, true)) {
                return DetectedEncoding::Utf16Be;
            }
        }
        return DetectedEncoding::UnknownBinary;
    }

    const double controlRatio = static_cast<double>(illegalControlCount) / static_cast<double>(sampleLen);
    if (controlRatio >= MaxRelaxedControlByteRatio) {
        return DetectedEncoding::UnknownBinary;
    }

    // 3. Strict UTF-8 check
    size_t utf8MultiBytes = 0;
    size_t utf8Illegal = 0;
    if (isStrictUtf8(data, sampleLen, utf8MultiBytes, utf8Illegal)) {
        return DetectedEncoding::Utf8;
    }

    // 4. Strict GBK / CP936 check
    size_t gbkMultiBytes = 0;
    size_t gbkIllegal = 0;
    if (isStrictGbk(data, sampleLen, gbkMultiBytes, gbkIllegal)) {
        return (gbkMultiBytes == 0) ? DetectedEncoding::Utf8 : DetectedEncoding::Gbk;
    }

    // 5. Relaxed mixed-encoding tolerance: first 8KB has zero NUL bytes and < 2% control bytes.
    // Decode line-by-line with UTF-8 first, falling back to CP936/ACP.
    return DetectedEncoding::MixedText;
}

std::wstring decodeLine(const char* data, size_t length, DetectedEncoding encoding) {
    if (length == 0) return {};

    // Try strict UTF-8 first for Utf8 and MixedText (and even Gbk if line happens to be valid UTF-8)
    if (encoding == DetectedEncoding::Utf8 || encoding == DetectedEncoding::MixedText) {
        const int wideLen = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), nullptr, 0);
        if (wideLen > 0) {
            std::wstring result(static_cast<size_t>(wideLen), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(length), result.data(), wideLen) > 0) {
                return result;
            }
        }
    }

    const UINT gbkCp = getGbkCodePage();
    int wideLen = MultiByteToWideChar(gbkCp, 0, data, static_cast<int>(length), nullptr, 0);
    if (wideLen > 0) {
        std::wstring result(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(gbkCp, 0, data, static_cast<int>(length), result.data(), wideLen);
        return result;
    }

    if (CP_ACP != gbkCp) {
        wideLen = MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(length), nullptr, 0);
        if (wideLen > 0) {
            std::wstring result(static_cast<size_t>(wideLen), L'\0');
            MultiByteToWideChar(CP_ACP, 0, data, static_cast<int>(length), result.data(), wideLen);
            return result;
        }
    }
    return {};
}

inline uint8_t asciiLowerByte(uint8_t ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<uint8_t>(ch + ('a' - 'A')) : ch;
}

size_t findSinglePatternByteOffset(
    const uint8_t* data,
    size_t len,
    std::wstring_view queryPattern,
    bool caseSensitive,
    DetectedEncoding enc
) {
    if (len == 0 || queryPattern.empty()) return std::string_view::npos;
    if (enc == DetectedEncoding::Utf16Le || enc == DetectedEncoding::Utf16Be) return 0;

    bool isAscii = true;
    std::string asciiQuery;
    asciiQuery.reserve(queryPattern.size());
    for (wchar_t wc : queryPattern) {
        if (wc < 128) {
            asciiQuery.push_back(static_cast<char>(wc));
        } else {
            isAscii = false;
            break;
        }
    }

    if (isAscii) {
        const size_t patLen = asciiQuery.size();
        if (len < patLen) return std::string_view::npos;
        const uint8_t firstRaw = static_cast<uint8_t>(asciiQuery[0]);
        const uint8_t firstLower = asciiLowerByte(firstRaw);
        const uint8_t firstUpper = (firstLower >= 'a' && firstLower <= 'z')
            ? static_cast<uint8_t>(firstLower - ('a' - 'A'))
            : firstLower;
        const size_t end = len - patLen + 1;

        const auto verifyTailAt = [&](size_t pos) noexcept -> bool {
            if (caseSensitive) {
                return patLen == 1 || std::memcmp(data + pos + 1, asciiQuery.data() + 1, patLen - 1) == 0;
            }
            for (size_t j = 1; j < patLen; ++j) {
                const uint8_t bj = data[pos + j];
                const uint8_t pj = static_cast<uint8_t>(asciiQuery[j]);
                if (bj != pj && asciiLowerByte(bj) != asciiLowerByte(pj)) {
                    return false;
                }
            }
            return true;
        };

        if (caseSensitive || firstLower == firstUpper) {
            const uint8_t targetFirst = caseSensitive ? firstRaw : firstLower;
            size_t pos = 0;
            while (pos < end) {
                const auto* hit = static_cast<const uint8_t*>(
                    std::memchr(data + pos, targetFirst, end - pos));
                if (!hit) return std::string_view::npos;
                const size_t hitIdx = static_cast<size_t>(hit - data);
                if (verifyTailAt(hitIdx)) {
                    return hitIdx;
                }
                pos = hitIdx + 1;
            }
            return std::string_view::npos;
        }

        size_t pos = 0;
        while (pos < end) {
            const size_t span = (std::min<size_t>)(MemchrChunkBytes, end - pos);
            const auto* hitLower = static_cast<const uint8_t*>(std::memchr(data + pos, firstLower, span));
            const auto* hitUpper = static_cast<const uint8_t*>(std::memchr(data + pos, firstUpper, span));
            if (!hitLower && !hitUpper) {
                pos += span;
                continue;
            }
            const uint8_t* bestHit = (!hitLower) ? hitUpper
                : ((!hitUpper) ? hitLower : ((hitLower < hitUpper) ? hitLower : hitUpper));
            const size_t hitIdx = static_cast<size_t>(bestHit - data);
            if (verifyTailAt(hitIdx)) {
                return hitIdx;
            }
            pos = hitIdx + 1;
        }
        return std::string_view::npos;
    }

    const auto searchPatternBytes = [&](std::wstring_view pat) -> size_t {
        size_t bestOffset = std::string_view::npos;
        const int utf8Len = WideCharToMultiByte(
            CP_UTF8, 0, pat.data(), static_cast<int>(pat.size()), nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::vector<uint8_t> utf8Pat(static_cast<size_t>(utf8Len));
            WideCharToMultiByte(
                CP_UTF8, 0, pat.data(), static_cast<int>(pat.size()),
                reinterpret_cast<char*>(utf8Pat.data()), utf8Len, nullptr, nullptr);
            const auto* it = std::search(data, data + len, utf8Pat.begin(), utf8Pat.end());
            if (it != data + len) {
                bestOffset = static_cast<size_t>(it - data);
            }
        }

        const UINT gbkCp = getGbkCodePage();
        const int gbkLen = WideCharToMultiByte(
            gbkCp, 0, pat.data(), static_cast<int>(pat.size()), nullptr, 0, nullptr, nullptr);
        if (gbkLen > 0) {
            std::vector<uint8_t> gbkPat(static_cast<size_t>(gbkLen));
            WideCharToMultiByte(
                gbkCp, 0, pat.data(), static_cast<int>(pat.size()),
                reinterpret_cast<char*>(gbkPat.data()), gbkLen, nullptr, nullptr);
            const auto* it = std::search(data, data + len, gbkPat.begin(), gbkPat.end());
            if (it != data + len) {
                const size_t off = static_cast<size_t>(it - data);
                if (bestOffset == std::string_view::npos || off < bestOffset) bestOffset = off;
            }
        }

        if (GetACP() != gbkCp && GetACP() != CP_UTF8) {
            const int acpLen = WideCharToMultiByte(
                CP_ACP, 0, pat.data(), static_cast<int>(pat.size()), nullptr, 0, nullptr, nullptr);
            if (acpLen > 0) {
                std::vector<uint8_t> acpPat(static_cast<size_t>(acpLen));
                WideCharToMultiByte(
                    CP_ACP, 0, pat.data(), static_cast<int>(pat.size()),
                    reinterpret_cast<char*>(acpPat.data()), acpLen, nullptr, nullptr);
                const auto* it = std::search(data, data + len, acpPat.begin(), acpPat.end());
                if (it != data + len) {
                    const size_t off = static_cast<size_t>(it - data);
                    if (bestOffset == std::string_view::npos || off < bestOffset) bestOffset = off;
                }
            }
        }
        return bestOffset;
    };

    size_t bestHit = searchPatternBytes(queryPattern);
    if (bestHit != std::string_view::npos && caseSensitive) return bestHit;

    if (!caseSensitive) {
        std::wstring lowerPat;
        lowerPat.reserve(queryPattern.size());
        for (wchar_t c : queryPattern) lowerPat.push_back(static_cast<wchar_t>(std::towlower(c)));
        if (lowerPat != queryPattern) {
            const size_t lowHit = searchPatternBytes(lowerPat);
            if (lowHit != std::string_view::npos && (bestHit == std::string_view::npos || lowHit < bestHit)) {
                bestHit = lowHit;
            }
        }

        std::wstring upperPat;
        upperPat.reserve(queryPattern.size());
        for (wchar_t c : queryPattern) upperPat.push_back(static_cast<wchar_t>(std::towupper(c)));
        if (upperPat != queryPattern && upperPat != lowerPat) {
            const size_t upHit = searchPatternBytes(upperPat);
            if (upHit != std::string_view::npos && (bestHit == std::string_view::npos || upHit < bestHit)) {
                bestHit = upHit;
            }
        }
    }

    return bestHit;
}

bool fastByteSinglePattern(
    const uint8_t* data,
    size_t len,
    std::wstring_view queryPattern,
    bool caseSensitive,
    DetectedEncoding enc
) {
    return findSinglePatternByteOffset(data, len, queryPattern, caseSensitive, enc) != std::string_view::npos;
}

bool fastBytePreFilterTerms(
    const uint8_t* data,
    size_t len,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    DetectedEncoding enc
) {
    if (len == 0 || queryTerms.empty()) return false;
    for (const auto& term : queryTerms) {
        if (term.empty()) continue;
        if (!fastByteSinglePattern(data, len, term, caseSensitive, enc)) {
            return false;
        }
    }
    return true;
}

void processHugeEncodedLine(
    uint32_t lineNumber,
    const uint8_t* lineBytes,
    size_t lineLen,
    DetectedEncoding enc,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    MultiTermDocumentMatcher& matcher
) {
    for (const auto& term : queryTerms) {
        if (term.empty()) continue;
        const size_t hitOffset = findSinglePatternByteOffset(lineBytes, lineLen, term, caseSensitive, enc);
        if (hitOffset == std::string_view::npos) continue;

        size_t winStart = (hitOffset > HugeLineWindowRadius) ? (hitOffset - HugeLineWindowRadius) : 0;
        size_t winEnd = (std::min<size_t>)(lineLen, hitOffset + term.size() * 4 + HugeLineWindowRadius);

        if (enc == DetectedEncoding::Utf8 || enc == DetectedEncoding::MixedText) {
            for (int step = 0; step < 3 && winStart < hitOffset && (lineBytes[winStart] & 0xC0u) == 0x80u; ++step) {
                ++winStart;
            }
            while (winEnd < lineLen && (lineBytes[winEnd] & 0xC0u) == 0x80u) {
                ++winEnd;
            }
        } else if (enc == DetectedEncoding::Gbk && winStart > 0) {
            for (size_t back = 0; back < 16 && winStart > 0; ++back) {
                if (lineBytes[winStart] < 0x80u) break;
                --winStart;
            }
        }

        std::wstring windowWide = decodeLine(
            reinterpret_cast<const char*>(lineBytes + winStart), winEnd - winStart, enc);
        if (!windowWide.empty()) {
            matcher.processLine(lineNumber, windowWide);
            if (matcher.canStopEarly()) {
                break;
            }
        }
    }
}

inline int rtfHexVal(char ch) noexcept {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool isRtfSkippedDestination(std::string_view word) noexcept {
    return word == "fonttbl" ||
           word == "colortbl" ||
           word == "stylesheet" ||
           word == "pict" ||
           word == "object" ||
           word == "info" ||
           word == "fldinst" ||
           word == "themedata" ||
           word == "colorschememapping" ||
           word == "latentstyles" ||
           word == "datastore" ||
           word == "xmlopen" ||
           word == "listtable" ||
           word == "listoverridetable" ||
           word == "rsidtbl" ||
           word == "generator";
}

bool hasRtfHeader(const uint8_t* data, size_t size) noexcept {
    if (!data || size < 5) return false;
    size_t pos = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        pos = 3;
    }
    while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n')) {
        ++pos;
    }
    return (pos + 5 <= size) && std::memcmp(data + pos, "{\\rtf", 5) == 0;
}

} // namespace

PlainTextExtractor::PlainTextExtractor() {
    const wchar_t* exts[] = {
        // C/C++/C#
        L"c", L"cpp", L"cc", L"cxx", L"h", L"hpp", L"hxx", L"inl", L"rc", L"def", L"cs",
        // Modern Systems & Compilers
        L"rs", L"go", L"zig", L"v", L"nim", L"odin",
        // JVM & Mobile
        L"java", L"kt", L"kts", L"scala", L"groovy", L"dart", L"swift", L"m", L"mm",
        // Web / Fullstack / Modern Frameworks
        L"js", L"jsx", L"mjs", L"cjs", L"ts", L"tsx", L"vue", L"svelte", L"astro",
        L"html", L"htm", L"css", L"scss", L"sass", L"less",
        // Schema & IDL / Data Definitions
        L"proto", L"graphql", L"gql", L"thrift", L"prisma", L"schema", L"avsc", L"dbml",
        // Data & Config
        L"json", L"jsonc", L"json5", L"xml", L"xaml", L"yaml", L"yml", L"toml",
        L"ini", L"cfg", L"conf", L"config", L"properties", L"env", L"reg", L"lock", L"plist", L"prefs",
        // Python/Ruby/PHP/Lua/Shell/PowerShell/Automation
        L"py", L"pyw", L"rb", L"php", L"pl", L"pm", L"lua", L"sh", L"bash", L"zsh",
        L"ps1", L"psm1", L"psd1", L"bat", L"cmd", L"vbs", L"ahk", L"au3",
        // SQL & Enterprise
        L"sql", L"prc", L"fnc", L"trg", L"pks", L"pkb", L"pls", L"ch", L"pld",
        // Plain text & Docs & Markup
        L"txt", L"md", L"markdown", L"log", L"csv", L"tsv", L"tex", L"bib", L"rst", L"adoc", L"rtf", L"diff", L"patch", L"org",
        // Assembly & Shaders
        L"asm", L"s", L"glsl", L"hlsl", L"vert", L"frag", L"geom", L"comp", L"shader", L"wgsl"
    };

    for (const auto* ext : exts) {
        m_supportedExts.insert(ext);
    }
}

bool PlainTextExtractor::canHandle(std::wstring_view extension) const {
    const std::wstring lowerExt = ContentFormatFilter::normalizeExt(extension);
    if (lowerExt.empty()) return false;
    return m_supportedExts.find(lowerExt) != m_supportedExts.end();
}

bool PlainTextExtractor::extractRtfFromMemory(
    const uint8_t* data,
    size_t size,
    std::wstring& outText,
    const CancellationCheck& isCancelled
) {
    outText.clear();
    if (!data || size == 0) return false;

    struct RtfGroupState {
        int ucCount = 1;
        bool skipDest = false;
        bool justOpened = false;
    };

    UINT defaultCodePage = getGbkCodePage();
    std::vector<RtfGroupState> stack;
    stack.reserve(32);
    stack.push_back(RtfGroupState{1, false, false});

    std::string pendingMbBytes;
    pendingMbBytes.reserve(64);

    auto flushPendingMbBytes = [&]() {
        if (pendingMbBytes.empty()) return;
        if (!stack.back().skipDest && outText.size() < MaxRtfExtractedChars) {
            const UINT cp = IsValidCodePage(defaultCodePage) ? defaultCodePage : getGbkCodePage();
            const int wlen = MultiByteToWideChar(
                cp, 0, pendingMbBytes.data(), static_cast<int>(pendingMbBytes.size()), nullptr, 0);
            if (wlen > 0) {
                std::wstring wide(static_cast<size_t>(wlen), L'\0');
                MultiByteToWideChar(
                    cp, 0, pendingMbBytes.data(), static_cast<int>(pendingMbBytes.size()), wide.data(), wlen);
                const size_t allow = (std::min)(wide.size(), MaxRtfExtractedChars - outText.size());
                outText.append(wide.data(), allow);
            }
        }
        pendingMbBytes.clear();
    };

    auto skipUnicodeReplacementUnits = [&](size_t& idx, int toSkip) {
        int skipped = 0;
        while (skipped < toSkip && idx < size) {
            const char c = static_cast<char>(data[idx]);
            if (c == '\r' || c == '\n') {
                ++idx;
                continue;
            }
            if (c == '{' || c == '}') {
                break;
            }
            if (c == '\\') {
                if (idx + 3 < size && data[idx + 1] == '\'' &&
                    rtfHexVal(static_cast<char>(data[idx + 2])) >= 0 &&
                    rtfHexVal(static_cast<char>(data[idx + 3])) >= 0) {
                    idx += 4;
                    ++skipped;
                    continue;
                }
                // Another control word (like \u-9165 for low surrogate) is NOT a replacement char unless it's \'hh
                break;
            }
            ++idx;
            ++skipped;
        }
    };

    size_t i = 0;
    while (i < size && outText.size() < MaxRtfExtractedChars) {
        if ((i & 0xFFFFu) == 0 && isCancelled && isCancelled()) {
            outText.clear();
            return false;
        }

        const char ch = static_cast<char>(data[i]);
        if (ch == '{') {
            flushPendingMbBytes();
            RtfGroupState child = stack.back();
            child.justOpened = true;
            stack.push_back(child);
            ++i;
            continue;
        }
        if (ch == '}') {
            flushPendingMbBytes();
            if (stack.size() > 1) {
                stack.pop_back();
            }
            ++i;
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            // Unescaped CR/LF in RTF is formatting whitespace, not document newline
            ++i;
            continue;
        }

        if (ch == '\\') {
            ++i;
            if (i >= size) break;
            const char next = static_cast<char>(data[i]);

            // Ignorable destination marker {\*\dest ...}
            if (next == '*') {
                flushPendingMbBytes();
                if (stack.back().justOpened) {
                    stack.back().skipDest = true;
                }
                ++i;
                continue;
            }

            // Hex byte escape \'hh
            if (next == '\'') {
                if (i + 2 < size) {
                    const int hi = rtfHexVal(static_cast<char>(data[i + 1]));
                    const int lo = rtfHexVal(static_cast<char>(data[i + 2]));
                    if (hi >= 0 && lo >= 0) {
                        if (!stack.back().skipDest) {
                            pendingMbBytes.push_back(static_cast<char>((hi << 4) | lo));
                        }
                        stack.back().justOpened = false;
                        i += 3;
                        continue;
                    }
                }
                ++i;
                continue;
            }

            // Escaped literal symbols \{ \} \\ \~ \_ \n \r
            if (!std::isalpha(static_cast<unsigned char>(next))) {
                flushPendingMbBytes();
                stack.back().justOpened = false;
                if (!stack.back().skipDest) {
                    if (next == '\\' || next == '{' || next == '}') {
                        outText.push_back(static_cast<wchar_t>(next));
                    } else if (next == '~') {
                        outText.push_back(L' ');
                    } else if (next == '_') {
                        outText.push_back(L'-');
                    } else if (next == '\n' || next == '\r') {
                        outText.push_back(L'\n');
                    }
                }
                ++i;
                continue;
            }

            // Alphabetic control word: \word[[-]N][ ]
            flushPendingMbBytes();
            const size_t wordStart = i;
            while (i < size && std::isalpha(static_cast<unsigned char>(data[i]))) {
                ++i;
            }
            const std::string_view word(reinterpret_cast<const char*>(data + wordStart), i - wordStart);

            bool hasNumber = false;
            int32_t numValue = 0;
            int32_t sign = 1;
            if (i < size && (data[i] == '-' || std::isdigit(data[i]))) {
                hasNumber = true;
                if (data[i] == '-') {
                    sign = -1;
                    ++i;
                }
                int64_t acc = 0;
                while (i < size && std::isdigit(data[i])) {
                    acc = acc * 10 + (data[i] - '0');
                    if (acc > 0x7FFFFFFFLL) acc = 0x7FFFFFFFLL;
                    ++i;
                }
                numValue = static_cast<int32_t>(acc * sign);
            }

            // Consume single optional space delimiter after control word
            if (i < size && data[i] == ' ') {
                ++i;
            }

            const bool atGroupHead = stack.back().justOpened;
            stack.back().justOpened = false;

            if (atGroupHead && isRtfSkippedDestination(word)) {
                stack.back().skipDest = true;
                continue;
            }

            if (word == "ansicpg" && hasNumber && numValue > 0) {
                const UINT candidateCp = static_cast<UINT>(numValue);
                if (IsValidCodePage(candidateCp)) {
                    defaultCodePage = candidateCp;
                }
                continue;
            }
            if (word == "uc" && hasNumber && numValue >= 0 && numValue <= 16) {
                stack.back().ucCount = static_cast<int>(numValue);
                continue;
            }
            if (word == "bin" && hasNumber && numValue > 0) {
                const size_t skipBytes = static_cast<size_t>(numValue);
                i = (i + skipBytes <= size) ? (i + skipBytes) : size;
                continue;
            }

            if (word == "u" && hasNumber) {
                if (!stack.back().skipDest) {
                    uint16_t u16 = 0;
                    if (numValue < 0) {
                        u16 = static_cast<uint16_t>(static_cast<int16_t>(numValue));
                    } else if (numValue <= 0xFFFF) {
                        u16 = static_cast<uint16_t>(numValue);
                    } else if (numValue <= 0x10FFFF) {
                        const uint32_t cp = static_cast<uint32_t>(numValue) - 0x10000u;
                        outText.push_back(static_cast<wchar_t>(0xD800u + ((cp >> 10) & 0x3FFu)));
                        u16 = static_cast<uint16_t>(0xDC00u + (cp & 0x3FFu));
                    }
                    if (u16 != 0 && outText.size() < MaxRtfExtractedChars) {
                        outText.push_back(static_cast<wchar_t>(u16));
                    }
                }
                skipUnicodeReplacementUnits(i, stack.back().ucCount);
                continue;
            }

            if (!stack.back().skipDest) {
                if (word == "par" || word == "pard" || word == "line" ||
                    word == "cell" || word == "row" || word == "sect" || word == "page") {
                    if (word != "pard" || (!outText.empty() && outText.back() != L'\n')) {
                        if (outText.empty() || outText.back() != L'\n') {
                            outText.push_back(L'\n');
                        }
                    }
                } else if (word == "tab") {
                    outText.push_back(L'\t');
                } else if (word == "emdash") {
                    outText.push_back(0x2014);
                } else if (word == "endash") {
                    outText.push_back(0x2013);
                } else if (word == "lquote") {
                    outText.push_back(0x2018);
                } else if (word == "rquote") {
                    outText.push_back(0x2019);
                } else if (word == "ldblquote") {
                    outText.push_back(0x201C);
                } else if (word == "rdblquote") {
                    outText.push_back(0x201D);
                } else if (word == "bullet") {
                    outText.push_back(0x2022);
                }
            }
            continue;
        }

        // Plain byte inside current group
        stack.back().justOpened = false;
        if (!stack.back().skipDest) {
            if (static_cast<uint8_t>(ch) >= 0x80u) {
                pendingMbBytes.push_back(ch);
            } else {
                flushPendingMbBytes();
                if (static_cast<uint8_t>(ch) >= 0x20u || ch == '\t') {
                    outText.push_back(static_cast<wchar_t>(static_cast<uint8_t>(ch)));
                }
            }
        }
        ++i;
    }

    flushPendingMbBytes();
    return !outText.empty();
}

bool PlainTextExtractor::searchContent(
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

bool PlainTextExtractor::searchContentTerms(
    const std::wstring& filePath,
    const std::vector<std::wstring>& queryTerms,
    bool caseSensitive,
    std::vector<ContentSnippet>& outSnippets,
    size_t maxSnippetsPerFile,
    const CancellationCheck& isCancelled
) {
    MultiTermDocumentMatcher matcher(queryTerms, caseSensitive, maxSnippetsPerFile);
    if (!matcher.hasValidTerms() || (isCancelled && isCancelled())) {
        return false;
    }

    std::wstring lowerExt;
    const size_t dotPos = filePath.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        lowerExt = ContentFormatFilter::normalizeExt(std::wstring_view(filePath).substr(dotPos + 1));
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
    if (hFile == INVALID_HANDLE_VALUE) return false;

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } fileCloser{hFile};

    const FileCacheStamp stamp = queryFileCacheStampByHandle(hFile);
    if (!stamp.valid || stamp.fileSize == 0 || stamp.fileSize > MaxPlainTextFileBytes) {
        return false;
    }

    // Fast cache check for .rtf rich text documents before memory-mapping
    if (lowerExt == L"rtf") {
        std::wstring cachedRtf;
        if (ContentTextCache::instance().tryGet(filePath, stamp.fileSize, stamp.lastWriteTime, cachedRtf)) {
            if (cachedRtf.empty()) return false;
            return matchDocumentTextAndBuildSnippets(
                cachedRtf, queryTerms, caseSensitive, outSnippets, maxSnippetsPerFile, isCancelled);
        }
    }

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

    bool isRtf = (lowerExt == L"rtf");
    if (!isRtf) {
        const bool headerOk = runWithMappedViewGuard([&]() -> bool {
            isRtf = hasRtfHeader(pData, mapSize);
            return true;
        });
        if (!headerOk) return false;
    }

    // Rich Text Format (.rtf or "{\rtf" signature) branch
    if (isRtf) {
        std::wstring rtfText;
        if (lowerExt != L"rtf" &&
            ContentTextCache::instance().tryGet(filePath, stamp.fileSize, stamp.lastWriteTime, rtfText)) {
            if (rtfText.empty()) return false;
            return matchDocumentTextAndBuildSnippets(
                rtfText, queryTerms, caseSensitive, outSnippets, maxSnippetsPerFile, isCancelled);
        }
        const bool rtfGuardOk = runWithMappedViewGuard([&]() -> bool {
            return extractRtfFromMemory(pData, mapSize, rtfText, isCancelled);
        });
        if (!rtfGuardOk) {
            rtfText.clear();
        }
        if (isCancelled && isCancelled()) {
            return false;
        }
        ContentTextCache::instance().put(filePath, stamp.fileSize, stamp.lastWriteTime, rtfText);
        if (rtfText.empty()) {
            return false;
        }
        return matchDocumentTextAndBuildSnippets(
            rtfText, queryTerms, caseSensitive, outSnippets, maxSnippetsPerFile, isCancelled);
    }

    const bool textScanOk = runWithMappedViewGuard([&]() -> bool {
        size_t bomOffset = 0;
        const DetectedEncoding enc = detectEncoding(pData, mapSize, bomOffset);
        if (enc == DetectedEncoding::UnknownBinary) {
            return false;
        }

        if (!fastBytePreFilterTerms(pData + bomOffset, mapSize - bomOffset, queryTerms, caseSensitive, enc)) {
            return false;
        }

        uint32_t lineNumber = 1;
        if (enc == DetectedEncoding::Utf16Le || enc == DetectedEncoding::Utf16Be) {
            const bool isBe = (enc == DetectedEncoding::Utf16Be);
            const uint8_t* body = pData + bomOffset;
            const size_t wCount = (mapSize - bomOffset) / sizeof(wchar_t);
            size_t lineStart = 0;

            for (size_t i = 0; i < wCount; ++i) {
                if (((i & 0x3FFFFu) == 0) && isCancelled && isCancelled()) {
                    return false;
                }
                const wchar_t ch = static_cast<wchar_t>(readUtf16Unit(body, i, isBe));
                if (ch == L'\n' || i == wCount - 1) {
                    if (((lineNumber & 0x3FFu) == 0) && isCancelled && isCancelled()) {
                        return false;
                    }

                    const size_t lineEnd = (ch == L'\n') ? i : (i + 1);
                    std::wstring line;
                    line.reserve(lineEnd - lineStart);
                    for (size_t k = lineStart; k < lineEnd; ++k) {
                        line.push_back(static_cast<wchar_t>(readUtf16Unit(body, k, isBe)));
                    }

                    matcher.processLine(lineNumber, line);
                    if (matcher.canStopEarly()) {
                        break;
                    }

                    lineStart = i + 1;
                    ++lineNumber;
                }
            }
        } else {
            size_t lineStart = bomOffset;
            for (size_t i = bomOffset; i < mapSize; ++i) {
                if ((((i - bomOffset) & 0x3FFFFu) == 0) && isCancelled && isCancelled()) {
                    return false;
                }
                if (pData[i] == '\n' || i == mapSize - 1) {
                    if (((lineNumber & 0x3FFu) == 0) && isCancelled && isCancelled()) {
                        return false;
                    }

                    const size_t lineEnd = (pData[i] == '\n') ? i : (i + 1);
                    const size_t lineLen = lineEnd - lineStart;
                    if (lineLen > HugeLineByteThreshold) {
                        processHugeEncodedLine(
                            lineNumber,
                            pData + lineStart,
                            lineLen,
                            enc,
                            queryTerms,
                            caseSensitive,
                            matcher
                        );
                    } else {
                        std::wstring line = decodeLine(
                            reinterpret_cast<const char*>(pData + lineStart), lineLen, enc);
                        matcher.processLine(lineNumber, line);
                    }
                    if (matcher.canStopEarly()) {
                        break;
                    }

                    lineStart = i + 1;
                    ++lineNumber;
                }
            }
        }
        return true;
    });

    if (!textScanOk) {
        outSnippets.clear();
        return false;
    }

    return matcher.finalize(outSnippets);
}

} // namespace tools3000::service::content
