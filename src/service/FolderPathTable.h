#pragma once
#include "StringArena.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tools3000::service {

// Full paths for every directory on a volume, memoized so that turning a file
// record into a path stays a single lookup instead of a walk up the parent
// chain.
//
// The table gets rebuilt wholesale whenever the USN journal reports a change,
// so it owns a private arena that is simply reset each time rather than sharing
// the index arena, which must stay stable.
class FolderPathTable {
public:
    FolderPathTable() { reset(); }

    void reset() {
        m_arena.reset();
        m_keys.assign(1024, EmptyKey);
        m_values.assign(1024, 0);
        m_count = 0;
    }

    void set(uint64_t frn, std::wstring_view path) {
        if (frn == EmptyKey) return;
        if ((m_count + 1) * 10 >= m_keys.size() * 7) grow();
        const size_t probe = probeFor(frn);
        if (m_keys[probe] == EmptyKey) {
            m_keys[probe] = frn;
            ++m_count;
        }
        const uint32_t length = static_cast<uint32_t>(
            path.size() > 0xFFFF ? 0xFFFF : path.size());
        m_values[probe] = pack(m_arena.append(path.substr(0, length)), length);
    }

    std::wstring_view find(uint64_t frn) const noexcept {
        if (frn == EmptyKey) return {};
        const size_t probe = probeFor(frn);
        if (m_keys[probe] != frn) return {};
        return m_arena.view(offsetOf(m_values[probe]), lengthOf(m_values[probe]));
    }

    bool contains(uint64_t frn) const noexcept {
        if (frn == EmptyKey) return false;
        return m_keys[probeFor(frn)] == frn;
    }

    size_t size() const noexcept { return m_count; }

    uint64_t approximateBytes() const noexcept {
        return m_arena.approximateBytes() +
               static_cast<uint64_t>(m_keys.capacity()) * sizeof(uint64_t) +
               static_cast<uint64_t>(m_values.capacity()) * sizeof(uint64_t);
    }

private:
    static constexpr uint64_t EmptyKey = 0;

    static uint64_t pack(uint32_t offset, uint32_t length) noexcept {
        return (static_cast<uint64_t>(offset) << 16) | (length & 0xFFFFu);
    }
    static uint32_t offsetOf(uint64_t value) noexcept { return static_cast<uint32_t>(value >> 16); }
    static uint32_t lengthOf(uint64_t value) noexcept { return static_cast<uint32_t>(value & 0xFFFFu); }

    static uint64_t mixKey(uint64_t frn) noexcept {
        uint64_t x = frn * 0x9E3779B97F4A7C15ULL;
        x ^= x >> 29;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 32;
        return x;
    }

    size_t probeFor(uint64_t frn) const noexcept {
        const size_t mask = m_keys.size() - 1;
        size_t probe = static_cast<size_t>(mixKey(frn)) & mask;
        while (m_keys[probe] != EmptyKey && m_keys[probe] != frn) {
            probe = (probe + 1) & mask;
        }
        return probe;
    }

    void grow() {
        const size_t capacity = m_keys.size() * 2;
        std::vector<uint64_t> keys(capacity, EmptyKey);
        std::vector<uint64_t> values(capacity, 0);
        const size_t mask = capacity - 1;
        for (size_t i = 0; i < m_keys.size(); ++i) {
            if (m_keys[i] == EmptyKey) continue;
            size_t probe = static_cast<size_t>(mixKey(m_keys[i])) & mask;
            while (keys[probe] != EmptyKey) probe = (probe + 1) & mask;
            keys[probe] = m_keys[i];
            values[probe] = m_values[i];
        }
        m_keys = std::move(keys);
        m_values = std::move(values);
    }

    StringArena m_arena;
    std::vector<uint64_t> m_keys;
    std::vector<uint64_t> m_values;
    size_t m_count = 0;
};

}  // namespace tools3000::service
