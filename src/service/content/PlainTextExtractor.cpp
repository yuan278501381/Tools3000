#include "PlainTextExtractor.h"
#include "ContentMatchUtils.h"
#include "ContentTextCache.h"
#include "RustContentKernel.h"
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
    if (!data || length == 0) return DetectedEncoding::UnknownBinary;
    const uint32_t raw = t3k_rs_detect_encoding(data, length, &outBomOffset);
    switch (raw) {
        case 1: return DetectedEncoding::Utf8;
        case 2: return DetectedEncoding::Utf16Le;
        case 3: return DetectedEncoding::Utf16Be;
        case 4: return DetectedEncoding::Gbk;
        case 5: return DetectedEncoding::MixedText;
        default: return DetectedEncoding::UnknownBinary;
    }
}

size_t rustFallbackDecodeLineCallback(
    const void* /*ctx*/,
    const uint8_t* bytesPtr,
    size_t bytesLen,
    uint32_t encodingRaw,
    uint16_t* outU16Ptr,
    size_t outU16Cap
) {
    if (!bytesPtr || bytesLen == 0 || !outU16Ptr || outU16Cap == 0) return 0;
    const char* data = reinterpret_cast<const char*>(bytesPtr);
    const int inLen = static_cast<int>(bytesLen);
    const int capLen = static_cast<int>(outU16Cap);
    wchar_t* outW = reinterpret_cast<wchar_t*>(outU16Ptr);

    if (encodingRaw == 1 || encodingRaw == 5) {
        const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, inLen, outW, capLen);
        if (written > 0) return static_cast<size_t>(written);
    }

    const UINT gbkCp = getGbkCodePage();
    int written = MultiByteToWideChar(gbkCp, 0, data, inLen, outW, capLen);
    if (written > 0) return static_cast<size_t>(written);

    if (CP_ACP != gbkCp) {
        written = MultiByteToWideChar(CP_ACP, 0, data, inLen, outW, capLen);
        if (written > 0) return static_cast<size_t>(written);
    }
    return 0;
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

struct PreparedTermBytePatterns {
    bool isAscii = true;
    std::string asciiBytes;
    std::vector<std::vector<uint8_t>> byteVariants;
    std::vector<T3kByteSlice> variantSlices;
};

void buildPreparedTermBytePatterns(
    std::wstring_view queryPattern,
    bool caseSensitive,
    PreparedTermBytePatterns& out
) {
    out.isAscii = true;
    out.asciiBytes.clear();
    out.byteVariants.clear();
    out.variantSlices.clear();
    if (queryPattern.empty()) return;

    out.asciiBytes.reserve(queryPattern.size());
    for (wchar_t wc : queryPattern) {
        if (wc < 128) {
            out.asciiBytes.push_back(static_cast<char>(wc));
        } else {
            out.isAscii = false;
            break;
        }
    }
    if (out.isAscii) {
        return;
    }

    auto appendEncodedVariantsFor = [&](std::wstring_view pat) {
        const auto addCodePageBytes = [&](UINT cp) {
            const int len = WideCharToMultiByte(
                cp, 0, pat.data(), static_cast<int>(pat.size()), nullptr, 0, nullptr, nullptr);
            if (len <= 0) return;
            std::vector<uint8_t> buf(static_cast<size_t>(len));
            WideCharToMultiByte(
                cp, 0, pat.data(), static_cast<int>(pat.size()),
                reinterpret_cast<char*>(buf.data()), len, nullptr, nullptr);
            for (const auto& existing : out.byteVariants) {
                if (existing == buf) return;
            }
            out.byteVariants.push_back(std::move(buf));
        };

        addCodePageBytes(CP_UTF8);
        const UINT gbkCp = getGbkCodePage();
        addCodePageBytes(gbkCp);
        if (GetACP() != gbkCp && GetACP() != CP_UTF8) {
            addCodePageBytes(CP_ACP);
        }
    };

    appendEncodedVariantsFor(queryPattern);
    if (!caseSensitive) {
        std::wstring lowerPat;
        lowerPat.reserve(queryPattern.size());
        for (wchar_t c : queryPattern) lowerPat.push_back(static_cast<wchar_t>(std::towlower(c)));
        if (lowerPat != queryPattern) {
            appendEncodedVariantsFor(lowerPat);
        }

        std::wstring upperPat;
        upperPat.reserve(queryPattern.size());
        for (wchar_t c : queryPattern) upperPat.push_back(static_cast<wchar_t>(std::towupper(c)));
        if (upperPat != queryPattern && upperPat != lowerPat) {
            appendEncodedVariantsFor(upperPat);
        }
    }

    out.variantSlices.reserve(out.byteVariants.size());
    for (const auto& v : out.byteVariants) {
        out.variantSlices.push_back(T3kByteSlice{v.data(), v.size()});
    }
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

    PreparedTermBytePatterns prep;
    buildPreparedTermBytePatterns(queryPattern, caseSensitive, prep);

    if (prep.isAscii) {
        const size_t hit = t3k_rs_find_ascii_pattern(
            data,
            len,
            reinterpret_cast<const uint8_t*>(prep.asciiBytes.data()),
            prep.asciiBytes.size(),
            caseSensitive ? 1 : 0
        );
        return (hit == SIZE_MAX) ? std::string_view::npos : hit;
    }

    size_t bestHit = std::string_view::npos;
    for (const auto& v : prep.byteVariants) {
        const size_t hit = t3k_rs_find_exact_bytes(data, len, v.data(), v.size());
        if (hit != SIZE_MAX) {
            if (bestHit == std::string_view::npos || hit < bestHit) {
                bestHit = hit;
            }
            if (caseSensitive) break;
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

    outSnippets.clear();
    std::vector<T3kUtf16Slice> u16Terms;
    std::vector<PreparedTermBytePatterns> preparedBytes;
    std::vector<T3kByteTermVariants> byteTermViews;
    u16Terms.reserve(queryTerms.size());
    preparedBytes.reserve(queryTerms.size());
    byteTermViews.reserve(queryTerms.size());

    for (const auto& t : queryTerms) {
        if (t.empty()) continue;
        u16Terms.push_back(T3kUtf16Slice{
            reinterpret_cast<const uint16_t*>(t.data()),
            t.size()
        });
        PreparedTermBytePatterns prep;
        buildPreparedTermBytePatterns(t, caseSensitive, prep);
        preparedBytes.push_back(std::move(prep));
    }
    if (u16Terms.empty()) {
        return false;
    }
    for (const auto& prep : preparedBytes) {
        byteTermViews.push_back(T3kByteTermVariants{
            .is_ascii = prep.isAscii ? static_cast<uint8_t>(1) : static_cast<uint8_t>(0),
            .ascii_ptr = prep.isAscii ? reinterpret_cast<const uint8_t*>(prep.asciiBytes.data()) : nullptr,
            .ascii_len = prep.isAscii ? prep.asciiBytes.size() : 0,
            .variants_ptr = prep.isAscii ? nullptr : prep.variantSlices.data(),
            .variants_count = prep.isAscii ? 0 : prep.variantSlices.size()
        });
    }

    bool rustMatched = false;
    const bool textScanOk = runWithMappedViewGuard([&]() -> bool {
        size_t bomOffset = 0;
        const DetectedEncoding enc = detectEncoding(pData, mapSize, bomOffset);
        if (enc == DetectedEncoding::UnknownBinary || bomOffset >= mapSize) {
            return false;
        }

        if (!fastBytePreFilterTerms(pData + bomOffset, mapSize - bomOffset, queryTerms, caseSensitive, enc)) {
            return false;
        }

        const uint8_t matched = t3k_rs_scan_mapped_text(
            pData + bomOffset,
            mapSize - bomOffset,
            static_cast<uint32_t>(enc),
            u16Terms.data(),
            byteTermViews.data(),
            u16Terms.size(),
            caseSensitive ? 1 : 0,
            maxSnippetsPerFile,
            isCancelled ? &rustCancelBridgeCallback : nullptr,
            isCancelled ? &isCancelled : nullptr,
            &rustFallbackDecodeLineCallback,
            nullptr,
            &rustEmitSnippetBridgeCallback,
            &outSnippets
        );
        rustMatched = (matched != 0);
        return true;
    });

    if (!textScanOk || !rustMatched) {
        outSnippets.clear();
        return false;
    }

    return !outSnippets.empty();
}

} // namespace tools3000::service::content
