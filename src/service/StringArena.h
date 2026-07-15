#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace tools3000::service {

// Bulk storage for the wide strings owned by the file index.
//
// A std::wstring costs 32 bytes of control block plus a heap allocation (and its
// allocator header) for anything past the small-string buffer. Multiplied across
// several strings per record and millions of records, that bookkeeping dwarfs
// the text itself, so the index stores names here and addresses them by a 32-bit
// offset instead. Blocks are never reallocated, so views handed out stay valid
// for as long as the arena lives.
class StringArena {
public:
    static constexpr uint32_t BlockShift = 20;
    static constexpr uint32_t BlockChars = 1u << BlockShift;  // 1Mi wchar_t == 2 MiB
    static constexpr uint32_t BlockMask = BlockChars - 1;
    static constexpr uint32_t MaxBlocks = 1u << (32 - BlockShift); // 4096 块 == 8 GiB 宽字符上界

    // Interning packs the offset and length into one 64-bit slot, which caps the
    // length an interned string may have.
    static constexpr size_t MaxInternChars = 0xFFFF;

    StringArena() { reset(); }

    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;
    StringArena(StringArena&&) noexcept = default;
    StringArena& operator=(StringArena&&) noexcept = default;

    // Copies text into the arena and returns its logical offset.
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

    // Like append(), but reuses an identical string that is already stored.
    // File trees repeat names heavily (package.json, index.js, __init__.py,
    // thousands of copies under node_modules), so interning collapses a large
    // share of the arena at the cost of one open-addressed lookup table.
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

    std::wstring_view view(uint32_t offset, uint32_t length) const noexcept {
        if (length == 0) return {};
        return {m_blocks[offset >> BlockShift].get() + (offset & BlockMask), length};
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
    }

    // Drops the interning table. Worth calling once bulk enumeration is done and
    // only a trickle of incremental names remains.
    void releaseInterner() {
        m_internSlots.assign(1024, EmptySlot);
        m_internUsed = 0;
        m_internSlots.shrink_to_fit();
    }

    uint64_t storedChars() const noexcept { return m_totalChars; }
    size_t uniqueStrings() const noexcept { return m_internUsed; }

    uint64_t approximateBytes() const noexcept {
        return m_allocatedChars * sizeof(wchar_t) +
               static_cast<uint64_t>(m_internSlots.size()) * sizeof(uint64_t);
    }

private:
    static constexpr uint64_t EmptySlot = 0;

    static uint64_t pack(uint32_t offset, uint32_t length) noexcept {
        return (static_cast<uint64_t>(offset) << 16) | (length & 0xFFFFu);
    }
    static uint32_t offsetOf(uint64_t slot) noexcept { return static_cast<uint32_t>(slot >> 16); }
    static uint32_t lengthOf(uint64_t slot) noexcept { return static_cast<uint32_t>(slot & 0xFFFFu); }

    static uint64_t hashOf(std::wstring_view text) noexcept {
        uint64_t hash = 1469598103934665603ULL;  // FNV-1a
        for (const wchar_t ch : text) {
            hash ^= static_cast<uint64_t>(static_cast<uint16_t>(ch));
            hash *= 1099511628211ULL;
        }
        return hash;
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

    std::vector<std::unique_ptr<wchar_t[]>> m_blocks;
    std::vector<uint64_t> m_internSlots;
    uint32_t m_activeBlockIndex = 0;
    uint32_t m_blockUsed = 0;
    size_t m_internUsed = 0;
    uint64_t m_totalChars = 0;
    uint64_t m_allocatedChars = 0;
};

}  // namespace tools3000::service
