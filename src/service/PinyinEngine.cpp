#include "PinyinEngine.h"
#include "UnicodePinyinData.h"

#include <cwctype>

namespace {

constexpr wchar_t asciiLowerWide(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
}

constexpr char asciiLowerByte(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z')
        ? static_cast<char>((ch - L'A') + 'a')
        : static_cast<char>(ch & 0x7F);
}

constexpr bool isHighSurrogate(wchar_t ch) noexcept {
    return ch >= static_cast<wchar_t>(0xD800) && ch <= static_cast<wchar_t>(0xDBFF);
}

constexpr bool isLowSurrogate(wchar_t ch) noexcept {
    return ch >= static_cast<wchar_t>(0xDC00) && ch <= static_cast<wchar_t>(0xDFFF);
}

inline wchar_t foldNonAsciiBmpChar(wchar_t ch) noexcept {
    if (ch >= 0xFF21 && ch <= 0xFF3A) {
        return static_cast<wchar_t>(ch + 0x20);
    }
    return static_cast<wchar_t>(std::towlower(ch));
}

}  // namespace

const char* PinyinEngine::LookupSyllableAscii(wchar_t ch) noexcept {
    if (ch <= 0x7F) {
        return nullptr;
    }
    const uint16_t u = static_cast<uint16_t>(ch);
    if (kUnicodePinyinTableSize == 0 ||
        u < kUnicodePinyinTable[0].unicode ||
        u > kUnicodePinyinTable[kUnicodePinyinTableSize - 1].unicode) {
        return nullptr;
    }

    size_t left = 0;
    size_t right = kUnicodePinyinTableSize - 1;
    while (left <= right) {
        const size_t mid = left + (right - left) / 2;
        const uint16_t midCode = kUnicodePinyinTable[mid].unicode;
        if (midCode == u) {
            return kPinyinSyllables[kUnicodePinyinTable[mid].syllableId];
        }
        if (midCode < u) {
            left = mid + 1;
        } else {
            if (mid == 0) break;
            right = mid - 1;
        }
    }
    return nullptr;
}

wchar_t PinyinEngine::GetFirstLetter(wchar_t ch) noexcept {
    if (ch <= 0x7F) {
        return asciiLowerWide(ch);
    }
    const char* py = LookupSyllableAscii(ch);
    if (py != nullptr && py[0] != '\0') {
        return static_cast<wchar_t>(static_cast<unsigned char>(py[0]));
    }
    return ch;
}

std::wstring PinyinEngine::GetCharPinyin(wchar_t ch) {
    if (ch <= 0x7F) {
        return std::wstring(1, asciiLowerWide(ch));
    }
    const char* py = LookupSyllableAscii(ch);
    if (py != nullptr) {
        std::wstring res;
        res.reserve(6);
        while (*py) {
            res.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*py++)));
        }
        return res;
    }
    return std::wstring(1, ch);
}

std::wstring PinyinEngine::GetInitials(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (isHighSurrogate(ch) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            result.push_back(ch);
            result.push_back(text[++i]);
            continue;
        }
        result.push_back(GetFirstLetter(ch));
    }
    return result;
}

std::wstring PinyinEngine::GetFullPinyin(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size() * 4);
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (isHighSurrogate(ch) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            result.push_back(ch);
            result.push_back(text[++i]);
            continue;
        }
        if (ch <= 0x7F) {
            result.push_back(asciiLowerWide(ch));
            continue;
        }
        const char* py = LookupSyllableAscii(ch);
        if (py != nullptr) {
            while (*py) {
                result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*py++)));
            }
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

void PinyinEngine::BuildNormalizedPinyin(
    std::wstring_view text,
    std::string& scratchAsciiInitials,
    std::string& scratchAsciiFull,
    std::wstring& outInitials,
    std::wstring& outFull
) {
    scratchAsciiInitials.clear();
    scratchAsciiFull.clear();
    outInitials.clear();
    outFull.clear();
    if (text.empty()) {
        return;
    }

    if (scratchAsciiInitials.capacity() < text.size()) {
        scratchAsciiInitials.reserve(text.size());
    }
    if (scratchAsciiFull.capacity() < text.size() * 4) {
        scratchAsciiFull.reserve(text.size() * 4);
    }

    // Fast path: write pure 1-byte ASCII into compact scratch buffers until any non-CJK non-ASCII symbol appears.
    bool pureAsciiOutput = true;
    size_t splitIdx = 0;
    for (; splitIdx < text.size(); ++splitIdx) {
        const wchar_t ch = text[splitIdx];
        if (ch <= 0x7F) {
            const char low = asciiLowerByte(ch);
            scratchAsciiInitials.push_back(low);
            scratchAsciiFull.push_back(low);
            continue;
        }
        if (isHighSurrogate(ch) && splitIdx + 1 < text.size() && isLowSurrogate(text[splitIdx + 1])) {
            pureAsciiOutput = false;
            break;
        }
        const char* py = LookupSyllableAscii(ch);
        if (py != nullptr && py[0] != '\0') {
            scratchAsciiInitials.push_back(py[0]);
            scratchAsciiFull.append(py);
        } else {
            pureAsciiOutput = false;
            break;
        }
    }

    outInitials.reserve(pureAsciiOutput ? scratchAsciiInitials.size() : text.size());
    for (char c : scratchAsciiInitials) {
        outInitials.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }

    outFull.reserve(pureAsciiOutput ? scratchAsciiFull.size() : (scratchAsciiFull.size() + (text.size() - splitIdx) * 4));
    for (char c : scratchAsciiFull) {
        outFull.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }

    if (pureAsciiOutput) {
        return;
    }

    for (size_t i = splitIdx; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch <= 0x7F) {
            const wchar_t low = asciiLowerWide(ch);
            outInitials.push_back(low);
            outFull.push_back(low);
            continue;
        }
        if (isHighSurrogate(ch) && i + 1 < text.size() && isLowSurrogate(text[i + 1])) {
            const wchar_t lo = text[++i];
            outInitials.push_back(ch);
            outInitials.push_back(lo);
            outFull.push_back(ch);
            outFull.push_back(lo);
            continue;
        }
        const char* py = LookupSyllableAscii(ch);
        if (py != nullptr && py[0] != '\0') {
            outInitials.push_back(static_cast<wchar_t>(static_cast<unsigned char>(py[0])));
            while (*py) {
                outFull.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*py++)));
            }
        } else {
            const wchar_t folded = foldNonAsciiBmpChar(ch);
            outInitials.push_back(folded);
            outFull.push_back(folded);
        }
    }
}
