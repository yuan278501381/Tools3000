#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace tools3000::service {

// Bulk storage for the filenames and pinyin initials owned by the file index.
//
// Dual-track storage (World-Class 1B):
//   - Pure ASCII strings (85%-95% of Windows paths + 100% of pinyin initials) are
//     packed into 1-byte `char` blocks (1 MiB/block), cutting RAM in half vs UTF-16.
//   - Non-ASCII strings (CJK / Japanese / Korean / Cyrillic / accented Latin) remain
//     native 2-byte UTF-16 `wchar_t` blocks (2 MiB/block) with ZERO 3-byte UTF-8 bloat.
// Blocks are never reallocated, so views handed out stay valid for the arena's lifetime.
class StringArena {
public:
    static constexpr uint32_t BlockShift = 18;
    static constexpr uint32_t BlockChars = 1u << BlockShift;  // 256Ki chars (256 KiB ASCII / 512 KiB UTF-16)
    static constexpr uint32_t BlockMask = BlockChars - 1;
    static constexpr uint32_t MaxBlocks = 1u << (32 - BlockShift); // 16384 blocks upper bound (4B total chars)

    // Interning packs the offset and length into one 64-bit slot, which caps the
    // length an interned string may have.
    static constexpr size_t MaxInternChars = 0xFFFF;

    static bool isAsciiOnly(std::wstring_view text) noexcept {
        for (const wchar_t ch : text) {
            if (static_cast<uint16_t>(ch) >= 0x80u) {
                return false;
            }
        }
        return true;
    }

    StringArena() { reset(); }

    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;
    StringArena(StringArena&&) noexcept = default;
    StringArena& operator=(StringArena&&) noexcept = default;

    // Copies wide text into the UTF-16 arena and returns its logical offset.
    uint32_t append(std::wstring_view text) {
        if (text.empty()) return 0;

        const size_t needed = text.size();
        if (needed > BlockChars) {
            if (m_blocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena logical address space exhausted");
            }
            // Oversized strings are side blocks. They receive their own logical
            // block index, but do not replace the current fixed-size append
            // block or discard its remaining capacity.
            m_blocks.push_back(std::make_unique<wchar_t[]>(needed));
            std::copy(text.begin(), text.end(), m_blocks.back().get());
            const uint32_t offset = static_cast<uint32_t>(m_blocks.size() - 1) << BlockShift;
            m_allocatedChars += needed;
            m_totalChars += needed;
            return offset;
        }

        if (m_blockUsed + needed > BlockChars) {
            if (m_blocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena logical address space exhausted");
            }
            m_blocks.push_back(std::make_unique<wchar_t[]>(BlockChars));
            m_activeBlockIndex = static_cast<uint32_t>(m_blocks.size() - 1);
            m_blockUsed = 0;
            m_allocatedChars += BlockChars;
        }

        const uint32_t blockIndex = m_activeBlockIndex;
        const uint32_t offset = (blockIndex << BlockShift) | m_blockUsed;
        std::copy(text.begin(), text.end(), m_blocks[blockIndex].get() + m_blockUsed);
        m_blockUsed += static_cast<uint32_t>(needed);
        m_totalChars += needed;
        return offset;
    }

    // Copies pure-ASCII text into the compact 1-byte arena and returns its logical offset.
    uint32_t appendAscii(std::string_view text) {
        if (text.empty()) return 0;
        ensureAsciiInitialized();

        const size_t needed = text.size();
        if (needed > BlockChars) {
            if (m_asciiBlocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena ASCII address space exhausted");
            }
            m_asciiBlocks.push_back(std::make_unique<char[]>(needed));
            std::copy(text.begin(), text.end(), m_asciiBlocks.back().get());
            const uint32_t offset = static_cast<uint32_t>(m_asciiBlocks.size() - 1) << BlockShift;
            m_allocatedAsciiChars += needed;
            m_totalAsciiChars += needed;
            return offset;
        }

        if (m_asciiBlockUsed + needed > BlockChars) {
            if (m_asciiBlocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena ASCII address space exhausted");
            }
            m_asciiBlocks.push_back(std::make_unique<char[]>(BlockChars));
            m_activeAsciiBlockIndex = static_cast<uint32_t>(m_asciiBlocks.size() - 1);
            m_asciiBlockUsed = 0;
            m_allocatedAsciiChars += BlockChars;
        }

        const uint32_t blockIndex = m_activeAsciiBlockIndex;
        const uint32_t offset = (blockIndex << BlockShift) | m_asciiBlockUsed;
        std::copy(text.begin(), text.end(), m_asciiBlocks[blockIndex].get() + m_asciiBlockUsed);
        m_asciiBlockUsed += static_cast<uint32_t>(needed);
        m_totalAsciiChars += needed;
        return offset;
    }

    uint32_t appendAsciiFromWide(std::wstring_view text) {
        if (text.empty()) return 0;
        ensureAsciiInitialized();

        const size_t needed = text.size();
        if (needed > BlockChars) {
            if (m_asciiBlocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena ASCII address space exhausted");
            }
            auto buf = std::make_unique<char[]>(needed);
            for (size_t i = 0; i < needed; ++i) {
                buf[i] = static_cast<char>(text[i] & 0x7Fu);
            }
            m_asciiBlocks.push_back(std::move(buf));
            const uint32_t offset = static_cast<uint32_t>(m_asciiBlocks.size() - 1) << BlockShift;
            m_allocatedAsciiChars += needed;
            m_totalAsciiChars += needed;
            return offset;
        }

        if (m_asciiBlockUsed + needed > BlockChars) {
            if (m_asciiBlocks.size() >= MaxBlocks) {
                throw std::length_error("StringArena ASCII address space exhausted");
            }
            m_asciiBlocks.push_back(std::make_unique<char[]>(BlockChars));
            m_activeAsciiBlockIndex = static_cast<uint32_t>(m_asciiBlocks.size() - 1);
            m_asciiBlockUsed = 0;
            m_allocatedAsciiChars += BlockChars;
        }

        const uint32_t blockIndex = m_activeAsciiBlockIndex;
        const uint32_t offset = (blockIndex << BlockShift) | m_asciiBlockUsed;
        char* dst = m_asciiBlocks[blockIndex].get() + m_asciiBlockUsed;
        for (size_t i = 0; i < needed; ++i) {
            dst[i] = static_cast<char>(text[i] & 0x7Fu);
        }
        m_asciiBlockUsed += static_cast<uint32_t>(needed);
        m_totalAsciiChars += needed;
        return offset;
    }

    // Like append(), but reuses an identical wide string that is already stored.
    uint32_t intern(std::wstring_view text) {
        if (text.empty()) return 0;
        if (text.size() > MaxInternChars) return append(text);

        if (m_internUsed * 10 >= m_internSlots.size() * 7) growInternTable();

        const uint64_t hash = hashOf(text);
        size_t probe = static_cast<size_t>(hash) & (m_internSlots.size() - 1);
        while (true) {
            const uint64_t slot = m_internSlots[probe];
            if (slot == EmptySlot) {
                const uint32_t offset = append(text);
                m_internSlots[probe] = pack(offset, static_cast<uint32_t>(text.size()));
                ++m_internUsed;
                return offset;
            }
            if (view(offsetOf(slot), lengthOf(slot)) == text) {
                return offsetOf(slot);
            }
            probe = (probe + 1) & (m_internSlots.size() - 1);
        }
    }

    // Interns a pure-ASCII string into the 1-byte ASCII arena.
    uint32_t internAscii(std::string_view text) {
        if (text.empty()) return 0;
        if (text.size() > MaxInternChars) return appendAscii(text);
        ensureAsciiInitialized();

        if (m_asciiInternUsed * 10 >= m_asciiInternSlots.size() * 7) growAsciiInternTable();

        const uint64_t hash = hashOfAscii(text);
        size_t probe = static_cast<size_t>(hash) & (m_asciiInternSlots.size() - 1);
        while (true) {
            const uint64_t slot = m_asciiInternSlots[probe];
            if (slot == EmptySlot) {
                const uint32_t offset = appendAscii(text);
                m_asciiInternSlots[probe] = pack(offset, static_cast<uint32_t>(text.size()));
                ++m_asciiInternUsed;
                return offset;
            }
            if (viewAscii(offsetOf(slot), lengthOf(slot)) == text) {
                return offsetOf(slot);
            }
            probe = (probe + 1) & (m_asciiInternSlots.size() - 1);
        }
    }

    // Interns a verified pure-ASCII wide string into the 1-byte ASCII arena without intermediate allocation.
    uint32_t internAsciiFromWide(std::wstring_view text) {
        if (text.empty()) return 0;
        if (text.size() > MaxInternChars) return appendAsciiFromWide(text);
        ensureAsciiInitialized();

        if (m_asciiInternUsed * 10 >= m_asciiInternSlots.size() * 7) growAsciiInternTable();

        const uint64_t hash = hashOf(text);
        size_t probe = static_cast<size_t>(hash) & (m_asciiInternSlots.size() - 1);
        while (true) {
            const uint64_t slot = m_asciiInternSlots[probe];
            if (slot == EmptySlot) {
                const uint32_t offset = appendAsciiFromWide(text);
                m_asciiInternSlots[probe] = pack(offset, static_cast<uint32_t>(text.size()));
                ++m_asciiInternUsed;
                return offset;
            }
            if (equalsAsciiWide(viewAscii(offsetOf(slot), lengthOf(slot)), text)) {
                return offsetOf(slot);
            }
            probe = (probe + 1) & (m_asciiInternSlots.size() - 1);
        }
    }

    std::wstring_view view(uint32_t offset, uint32_t length) const noexcept {
        if (length == 0) return {};
        return {m_blocks[offset >> BlockShift].get() + (offset & BlockMask), length};
    }

    std::string_view viewAscii(uint32_t offset, uint32_t length) const noexcept {
        if (length == 0 || m_asciiBlocks.empty()) return {};
        return {m_asciiBlocks[offset >> BlockShift].get() + (offset & BlockMask), length};
    }

    bool equalsName(uint32_t offset, uint32_t length, bool isAscii, std::wstring_view rhs) const noexcept {
        if (length != rhs.size()) return false;
        if (length == 0) return true;
        if (isAscii) {
            return equalsAsciiWide(viewAscii(offset, length), rhs);
        }
        return view(offset, length) == rhs;
    }

    std::wstring_view viewWidened(uint32_t offset, uint32_t length, bool isAscii, std::wstring& scratch) const {
        if (length == 0) return {};
        if (!isAscii) {
            return view(offset, length);
        }
        const std::string_view ascii = viewAscii(offset, length);
        scratch.resize(ascii.size());
        wchar_t* dst = scratch.data();
        const char* src = ascii.data();
        for (size_t i = 0; i < ascii.size(); ++i) {
            dst[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        }
        return std::wstring_view(dst, ascii.size());
    }

    void appendNameTo(uint32_t offset, uint32_t length, bool isAscii, std::wstring& dst) const {
        if (length == 0) return;
        if (!isAscii) {
            const auto wv = view(offset, length);
            dst.append(wv.data(), wv.size());
            return;
        }
        const auto av = viewAscii(offset, length);
        const size_t prev = dst.size();
        dst.resize(prev + av.size());
        wchar_t* out = dst.data() + prev;
        const char* src = av.data();
        for (size_t i = 0; i < av.size(); ++i) {
            out[i] = static_cast<wchar_t>(static_cast<unsigned char>(src[i]));
        }
    }

    int compareNamesIgnoreCase(
        uint32_t offA, uint32_t lenA, bool asciiA,
        uint32_t offB, uint32_t lenB, bool asciiB
    ) const noexcept {
        const uint32_t common = (lenA < lenB) ? lenA : lenB;
        auto fold = [](wchar_t ch) noexcept -> wchar_t {
            if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch + 32);
            return ch;
        };
        if (asciiA && asciiB) {
            const auto a = viewAscii(offA, lenA);
            const auto b = viewAscii(offB, lenB);
            for (uint32_t i = 0; i < common; ++i) {
                unsigned char ca = static_cast<unsigned char>(a[i]);
                unsigned char cb = static_cast<unsigned char>(b[i]);
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb + 32);
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
        } else if (!asciiA && !asciiB) {
            const auto a = view(offA, lenA);
            const auto b = view(offB, lenB);
            for (uint32_t i = 0; i < common; ++i) {
                const wchar_t ca = fold(a[i]);
                const wchar_t cb = fold(b[i]);
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
        } else {
            for (uint32_t i = 0; i < common; ++i) {
                const wchar_t ca = fold(asciiA
                    ? static_cast<wchar_t>(static_cast<unsigned char>(viewAscii(offA, lenA)[i]))
                    : view(offA, lenA)[i]);
                const wchar_t cb = fold(asciiB
                    ? static_cast<wchar_t>(static_cast<unsigned char>(viewAscii(offB, lenB)[i]))
                    : view(offB, lenB)[i]);
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
        }
        if (lenA != lenB) return (lenA < lenB) ? -1 : 1;
        return 0;
    }

    int compareNamesExact(
        uint32_t offA, uint32_t lenA, bool asciiA,
        uint32_t offB, uint32_t lenB, bool asciiB
    ) const noexcept {
        const uint32_t common = (lenA < lenB) ? lenA : lenB;
        if (asciiA && asciiB) {
            const auto a = viewAscii(offA, lenA);
            const auto b = viewAscii(offB, lenB);
            for (uint32_t i = 0; i < common; ++i) {
                const unsigned char ca = static_cast<unsigned char>(a[i]);
                const unsigned char cb = static_cast<unsigned char>(b[i]);
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
        } else if (!asciiA && !asciiB) {
            const auto a = view(offA, lenA);
            const auto b = view(offB, lenB);
            for (uint32_t i = 0; i < common; ++i) {
                if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
            }
        } else {
            for (uint32_t i = 0; i < common; ++i) {
                const wchar_t ca = asciiA
                    ? static_cast<wchar_t>(static_cast<unsigned char>(viewAscii(offA, lenA)[i]))
                    : view(offA, lenA)[i];
                const wchar_t cb = asciiB
                    ? static_cast<wchar_t>(static_cast<unsigned char>(viewAscii(offB, lenB)[i]))
                    : view(offB, lenB)[i];
                if (ca != cb) return (ca < cb) ? -1 : 1;
            }
        }
        if (lenA != lenB) return (lenA < lenB) ? -1 : 1;
        return 0;
    }

    void reset() {
        m_blocks.clear();
        m_blocks.push_back(std::make_unique<wchar_t[]>(BlockChars));
        m_activeBlockIndex = 0;
        // Offset 0 is reserved for the empty string so that a zero offset can
        // double as "absent" in the record layout and in the intern table.
        m_blocks.back()[0] = L'\0';
        m_blockUsed = 1;
        m_allocatedChars = BlockChars;
        m_totalChars = 0;
        m_internSlots.assign(1024, EmptySlot);
        m_internUsed = 0;

        m_asciiBlocks.clear();
        m_asciiBlocks.shrink_to_fit();
        m_activeAsciiBlockIndex = 0;
        m_asciiBlockUsed = 0;
        m_allocatedAsciiChars = 0;
        m_totalAsciiChars = 0;
        m_asciiInternSlots.clear();
        m_asciiInternSlots.shrink_to_fit();
        m_asciiInternUsed = 0;
    }

    // Pre-sizes the intern hash tables before bulk snapshot/MFT import to avoid repeated rehashes.
    void reserveInternTable(size_t expectedUniqueStrings) {
        // Most filenames are pure ASCII; split budget ~80% ASCII / ~20% wide.
        const size_t expectedWide = (std::max<size_t>)(512, expectedUniqueStrings / 5);
        const size_t desiredWide = (expectedWide + 1) * 2;
        size_t capWide = m_internSlots.empty() ? 1024 : m_internSlots.size();
        while (capWide < desiredWide) capWide <<= 1;
        if (capWide > m_internSlots.size()) {
            std::vector<uint64_t> larger(capWide, EmptySlot);
            for (const uint64_t slot : m_internSlots) {
                if (slot == EmptySlot) continue;
                const uint64_t hash = hashOf(view(offsetOf(slot), lengthOf(slot)));
                size_t probe = static_cast<size_t>(hash) & (larger.size() - 1);
                while (larger[probe] != EmptySlot) probe = (probe + 1) & (larger.size() - 1);
                larger[probe] = slot;
            }
            m_internSlots = std::move(larger);
        }

        ensureAsciiInitialized();
        const size_t desiredAscii = (expectedUniqueStrings + 1) * 2;
        size_t capAscii = m_asciiInternSlots.empty() ? 1024 : m_asciiInternSlots.size();
        while (capAscii < desiredAscii) capAscii <<= 1;
        if (capAscii > m_asciiInternSlots.size()) {
            std::vector<uint64_t> larger(capAscii, EmptySlot);
            for (const uint64_t slot : m_asciiInternSlots) {
                if (slot == EmptySlot) continue;
                const uint64_t hash = hashOfAscii(viewAscii(offsetOf(slot), lengthOf(slot)));
                size_t probe = static_cast<size_t>(hash) & (larger.size() - 1);
                while (larger[probe] != EmptySlot) probe = (probe + 1) & (larger.size() - 1);
                larger[probe] = slot;
            }
            m_asciiInternSlots = std::move(larger);
        }
    }

    // Drops the interning tables once bulk enumeration is done.
    void releaseInterner() {
        m_internSlots.assign(1024, EmptySlot);
        m_internUsed = 0;
        m_internSlots.shrink_to_fit();

        if (!m_asciiInternSlots.empty()) {
            m_asciiInternSlots.assign(1024, EmptySlot);
            m_asciiInternUsed = 0;
            m_asciiInternSlots.shrink_to_fit();
        }
    }

    uint64_t storedChars() const noexcept { return m_totalChars + m_totalAsciiChars; }
    uint64_t storedAsciiChars() const noexcept { return m_totalAsciiChars; }
    uint64_t storedWideChars() const noexcept { return m_totalChars; }
    size_t uniqueStrings() const noexcept { return m_internUsed + m_asciiInternUsed; }

    uint64_t approximateBytes() const noexcept {
        return m_allocatedChars * sizeof(wchar_t) +
               m_allocatedAsciiChars * sizeof(char) +
               static_cast<uint64_t>(m_internSlots.size()) * sizeof(uint64_t) +
               static_cast<uint64_t>(m_asciiInternSlots.size()) * sizeof(uint64_t);
    }

private:
    static constexpr uint64_t EmptySlot = 0;

    static uint64_t pack(uint32_t offset, uint32_t length) noexcept {
        return (static_cast<uint64_t>(offset) << 16) | (length & 0xFFFFu);
    }
    static uint32_t offsetOf(uint64_t slot) noexcept { return static_cast<uint32_t>(slot >> 16); }
    static uint32_t lengthOf(uint64_t slot) noexcept { return static_cast<uint32_t>(slot & 0xFFFFu); }

    static bool equalsAsciiWide(std::string_view ascii, std::wstring_view wide) noexcept {
        if (ascii.size() != wide.size()) return false;
        for (size_t i = 0; i < ascii.size(); ++i) {
            if (static_cast<uint16_t>(wide[i]) != static_cast<unsigned char>(ascii[i])) {
                return false;
            }
        }
        return true;
    }

    static uint64_t hashOf(std::wstring_view text) noexcept {
        uint64_t hash = 1469598103934665603ULL;  // FNV-1a
        for (const wchar_t ch : text) {
            hash ^= static_cast<uint64_t>(static_cast<uint16_t>(ch));
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static uint64_t hashOfAscii(std::string_view text) noexcept {
        uint64_t hash = 1469598103934665603ULL;  // FNV-1a (identical to hashOf for ASCII code points!)
        for (const char ch : text) {
            hash ^= static_cast<uint64_t>(static_cast<unsigned char>(ch));
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void ensureAsciiInitialized() {
        if (m_asciiBlocks.empty()) {
            m_asciiBlocks.push_back(std::make_unique<char[]>(BlockChars));
            m_activeAsciiBlockIndex = 0;
            m_asciiBlocks.back()[0] = '\0';
            m_asciiBlockUsed = 1;
            m_allocatedAsciiChars = BlockChars;
        }
        if (m_asciiInternSlots.empty()) {
            m_asciiInternSlots.assign(1024, EmptySlot);
            m_asciiInternUsed = 0;
        }
    }

    void growInternTable() {
        std::vector<uint64_t> larger(m_internSlots.size() * 2, EmptySlot);
        for (const uint64_t slot : m_internSlots) {
            if (slot == EmptySlot) continue;
            const uint64_t hash = hashOf(view(offsetOf(slot), lengthOf(slot)));
            size_t probe = static_cast<size_t>(hash) & (larger.size() - 1);
            while (larger[probe] != EmptySlot) probe = (probe + 1) & (larger.size() - 1);
            larger[probe] = slot;
        }
        m_internSlots = std::move(larger);
    }

    void growAsciiInternTable() {
        std::vector<uint64_t> larger(m_asciiInternSlots.size() * 2, EmptySlot);
        for (const uint64_t slot : m_asciiInternSlots) {
            if (slot == EmptySlot) continue;
            const uint64_t hash = hashOfAscii(viewAscii(offsetOf(slot), lengthOf(slot)));
            size_t probe = static_cast<size_t>(hash) & (larger.size() - 1);
            while (larger[probe] != EmptySlot) probe = (probe + 1) & (larger.size() - 1);
            larger[probe] = slot;
        }
        m_asciiInternSlots = std::move(larger);
    }

    std::vector<std::unique_ptr<wchar_t[]>> m_blocks;
    std::vector<uint64_t> m_internSlots;
    uint32_t m_activeBlockIndex = 0;
    uint32_t m_blockUsed = 0;
    size_t m_internUsed = 0;
    uint64_t m_totalChars = 0;
    uint64_t m_allocatedChars = 0;

    std::vector<std::unique_ptr<char[]>> m_asciiBlocks;
    std::vector<uint64_t> m_asciiInternSlots;
    uint32_t m_activeAsciiBlockIndex = 0;
    uint32_t m_asciiBlockUsed = 0;
    size_t m_asciiInternUsed = 0;
    uint64_t m_totalAsciiChars = 0;
    uint64_t m_allocatedAsciiChars = 0;
};

}  // namespace tools3000::service
