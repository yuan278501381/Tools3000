#pragma once
#include "SearchExpression.h"
#include "StringArena.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace tools3000::service {

// One indexed file, packed to 64 bytes.
//
// Names live in the store's StringArena and are referenced by offset. Pinyin is
// only produced for CJK names — a small minority of any real volume — so it sits
// in a side table addressed by `pinyinSlot` instead of costing every record two
// more offsets and lengths.
struct StoredFileRecord {
    uint64_t frn = 0;
    uint64_t parentFrn = 0;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
    uint32_t attributes = 0;
    uint32_t nameOffset = 0;
    uint32_t pinyinSlot = 0;  // 0 == no pinyin
    uint16_t nameLength = 0;
    uint8_t flags = 0;

    static constexpr uint8_t FlagDirectory = 1u << 0;
    static constexpr uint8_t FlagDeleted = 1u << 1;

    bool isDirectory() const noexcept { return (flags & FlagDirectory) != 0; }
    bool isDeleted() const noexcept { return (flags & FlagDeleted) != 0; }
};

// Backing store for a volume's file index.
//
// Records sit in one contiguous vector so a full scan streams linearly through
// memory, and lookups by file reference number go through a flat open-addressed
// table rather than a node-based map. Deletions tombstone in place and are
// reclaimed by compact() once they accumulate, which keeps USN updates O(1)
// without leaving the scan walking dead entries forever.
class FileIndexStore {
public:
    void reserve(size_t records);
    void clear();

    // Inserts or replaces the record for `init.fileReferenceNumber`.
    void upsert(const FileRecordInit& init);
    bool updateMetadata(uint64_t frn, uint64_t fileSize, uint64_t creationTime, uint64_t lastWriteTime) noexcept;
    bool erase(uint64_t frn);

    size_t size() const noexcept { return m_liveCount; }
    size_t slotCount() const noexcept { return m_records.size(); }
    bool empty() const noexcept { return m_liveCount == 0; }

    const StoredFileRecord* at(size_t slot) const noexcept {
        const auto& record = m_records[slot];
        return record.isDeleted() ? nullptr : &record;
    }

    const StoredFileRecord* find(uint64_t frn) const noexcept;

    FileRecord view(const StoredFileRecord& record) const noexcept;

    // Visits every live record. Used by snapshot export so that callers never
    // have to materialize a second copy of the whole index.
    void forEach(const std::function<void(const StoredFileRecord&)>& visit) const;

    // Drops tombstones when they exceed a quarter of the slots. Names left
    // behind in the arena are only reclaimed by a full clear(), which is what
    // a re-enumeration does anyway.
    void compactIfSparse();

    void releaseBuildScratch() { m_arena.releaseInterner(); }

    uint64_t approximateBytes() const noexcept;

private:
    struct PinyinEntry {
        uint32_t initialsOffset = 0;
        uint32_t fullOffset = 0;
        uint16_t initialsLength = 0;
        uint16_t fullLength = 0;
    };

    static constexpr uint64_t EmptyKey = 0;

    void rehash(size_t minimumSlots);
    void insertKey(uint64_t frn, uint32_t slot);
    size_t probeFor(uint64_t frn) const noexcept;
    static uint64_t mixKey(uint64_t frn) noexcept;

    StringArena m_arena;
    std::vector<StoredFileRecord> m_records;
    std::vector<PinyinEntry> m_pinyin{PinyinEntry{}};  // index 0 == "no pinyin"
    std::vector<uint64_t> m_keys;
    std::vector<uint32_t> m_values;
    size_t m_liveCount = 0;
    size_t m_tombstones = 0;
};

}  // namespace tools3000::service
