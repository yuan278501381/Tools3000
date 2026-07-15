#include "FileIndexStore.h"
#include "PinyinEngine.h"

#include <algorithm>

namespace tools3000::service {

namespace {

// Multiplier from splitmix64. NTFS file reference numbers carry a sequence
// number in their high bits and a dense record index in their low bits, so the
// raw value clusters badly under a power-of-two mask and needs mixing.
constexpr uint64_t kMixConstant = 0x9E3779B97F4A7C15ULL;

}  // namespace

uint64_t FileIndexStore::mixKey(uint64_t frn) noexcept {
    uint64_t x = frn * kMixConstant;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 32;
    return x;
}

void FileIndexStore::reserve(size_t records) {
    m_records.reserve(records);
    rehash(records * 2);
}

void FileIndexStore::clear() {
    m_arena.reset();
    m_records.clear();
    m_records.shrink_to_fit();
    m_pinyin.assign(1, PinyinEntry{});
    m_pinyin.shrink_to_fit();
    m_keys.clear();
    m_keys.shrink_to_fit();
    m_values.clear();
    m_values.shrink_to_fit();
    m_liveCount = 0;
    m_tombstones = 0;
}

size_t FileIndexStore::probeFor(uint64_t frn) const noexcept {
    const size_t mask = m_keys.size() - 1;
    size_t probe = static_cast<size_t>(mixKey(frn)) & mask;
    while (m_keys[probe] != EmptyKey && m_keys[probe] != frn) {
        probe = (probe + 1) & mask;
    }
    return probe;
}

void FileIndexStore::rehash(size_t minimumSlots) {
    size_t capacity = 1024;
    while (capacity < minimumSlots) capacity <<= 1;

    std::vector<uint64_t> keys(capacity, EmptyKey);
    std::vector<uint32_t> values(capacity, 0);
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

void FileIndexStore::insertKey(uint64_t frn, uint32_t slot) {
    // Linear probing degrades sharply past ~70% load.
    if ((m_liveCount + m_tombstones + 1) * 10 >= m_keys.size() * 7) {
        rehash(m_keys.size() * 2);
    }
    const size_t probe = probeFor(frn);
    m_keys[probe] = frn;
    m_values[probe] = slot;
}

void FileIndexStore::upsert(const FileRecordInit& init) {
    if (init.fileReferenceNumber == EmptyKey) return;
    if (m_keys.empty()) rehash(1024);

    const uint16_t nameLen = static_cast<uint16_t>(std::min<size_t>(init.fileName.size(), 0xFFFF));
    const std::wstring_view nameView = std::wstring_view{init.fileName}.substr(0, nameLen);
    const uint8_t newFlags = init.isDirectory ? StoredFileRecord::FlagDirectory : 0;

    // Fast path for USN metadata updates where FRN and fileName are unchanged:
    // reuse existing StringArena offset and pinyinSlot without growing m_pinyin.
    const size_t existingProbe = probeFor(init.fileReferenceNumber);
    if (m_keys[existingProbe] == init.fileReferenceNumber) {
        auto& existing = m_records[m_values[existingProbe]];
        if (!existing.isDeleted() &&
            existing.nameLength == nameLen &&
            m_arena.view(existing.nameOffset, existing.nameLength) == nameView) {
            existing.parentFrn = init.parentFileReferenceNumber;
            existing.fileSize = init.fileSize;
            existing.creationTime = init.creationTime;
            existing.lastWriteTime = init.lastWriteTime;
            existing.attributes = init.fileAttributes;
            existing.flags = newFlags;
            return;
        }
    }

    StoredFileRecord record{};
    record.frn = init.fileReferenceNumber;
    record.parentFrn = init.parentFileReferenceNumber;
    record.fileSize = init.fileSize;
    record.creationTime = init.creationTime;
    record.lastWriteTime = init.lastWriteTime;
    record.attributes = init.fileAttributes;
    record.nameLength = nameLen;
    record.flags = newFlags;
    record.nameOffset = m_arena.intern(nameView);

    // 仅对包含 CJK 汉字（含 Ext-A）的文件生成拼音旁路索引，非汉字文件零额外存储开销
    bool hasChinese = false;
    for (wchar_t ch : nameView) {
        const auto code = static_cast<uint16_t>(ch);
        if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3400 && code <= 0x4DBF)) {
            hasChinese = true;
            break;
        }
    }
    if (hasChinese) {
        const std::wstring initials = SearchExpression::normalize(PinyinEngine::GetInitials(init.fileName));
        const std::wstring full = SearchExpression::normalize(PinyinEngine::GetFullPinyin(init.fileName));
        PinyinEntry entry{};
        entry.initialsLength = static_cast<uint16_t>(std::min<size_t>(initials.size(), 0xFFFF));
        entry.fullLength = static_cast<uint16_t>(std::min<size_t>(full.size(), 0xFFFF));
        entry.initialsOffset = m_arena.intern(std::wstring_view{initials}.substr(0, entry.initialsLength));
        entry.fullOffset = m_arena.intern(std::wstring_view{full}.substr(0, entry.fullLength));
        m_pinyin.push_back(entry);
        record.pinyinSlot = static_cast<uint32_t>(m_pinyin.size() - 1);
    }

    if (m_keys[existingProbe] == init.fileReferenceNumber) {
        auto& existing = m_records[m_values[existingProbe]];
        if (existing.isDeleted()) {
            --m_tombstones;
            ++m_liveCount;
        }
        existing = record;
        return;
    }

    m_records.push_back(record);
    insertKey(init.fileReferenceNumber, static_cast<uint32_t>(m_records.size() - 1));
    ++m_liveCount;
}

bool FileIndexStore::updateMetadata(uint64_t frn, uint64_t fileSize,
                                    uint64_t creationTime, uint64_t lastWriteTime) noexcept {
    if (frn == EmptyKey || m_keys.empty()) return false;
    const size_t probe = probeFor(frn);
    if (m_keys[probe] != frn) return false;

    auto& record = m_records[m_values[probe]];
    if (record.isDeleted()) return false;

    record.fileSize = fileSize;
    if (creationTime != 0) record.creationTime = creationTime;
    if (lastWriteTime != 0) record.lastWriteTime = lastWriteTime;
    return true;
}

bool FileIndexStore::erase(uint64_t frn) {
    if (frn == EmptyKey || m_keys.empty()) return false;
    const size_t probe = probeFor(frn);
    if (m_keys[probe] != frn) return false;

    auto& record = m_records[m_values[probe]];
    if (record.isDeleted()) return false;
    record.flags |= StoredFileRecord::FlagDeleted;
    --m_liveCount;
    ++m_tombstones;
    return true;
}

const StoredFileRecord* FileIndexStore::find(uint64_t frn) const noexcept {
    if (frn == EmptyKey || m_keys.empty()) return nullptr;
    const size_t probe = probeFor(frn);
    if (m_keys[probe] != frn) return nullptr;
    const auto& record = m_records[m_values[probe]];
    return record.isDeleted() ? nullptr : &record;
}

FileRecord FileIndexStore::view(const StoredFileRecord& record) const noexcept {
    FileRecord out;
    out.fileReferenceNumber = record.frn;
    out.parentFileReferenceNumber = record.parentFrn;
    out.fileName = m_arena.view(record.nameOffset, record.nameLength);
    out.normalizedName = out.fileName;
    out.isDirectory = record.isDirectory();
    out.fileAttributes = record.attributes;
    out.fileSize = record.fileSize;
    out.creationTime = record.creationTime;
    out.lastWriteTime = record.lastWriteTime;

    if (record.pinyinSlot == 0) {
        out.pinyinInitials = out.fileName;
        out.pinyinFull = out.fileName;
    } else {
        const auto& entry = m_pinyin[record.pinyinSlot];
        out.pinyinInitials = m_arena.view(entry.initialsOffset, entry.initialsLength);
        out.pinyinFull = m_arena.view(entry.fullOffset, entry.fullLength);
    }
    return out;
}

void FileIndexStore::forEach(const std::function<void(const StoredFileRecord&)>& visit) const {
    for (const auto& record : m_records) {
        if (!record.isDeleted()) visit(record);
    }
}

void FileIndexStore::compactIfSparse() {
    if (m_tombstones == 0 || m_tombstones * 4 < m_records.size()) return;

    std::vector<StoredFileRecord> live;
    live.reserve(m_liveCount);
    for (const auto& record : m_records) {
        if (!record.isDeleted()) live.push_back(record);
    }
    m_records = std::move(live);
    m_tombstones = 0;

    size_t capacity = 1024;
    while (capacity < m_records.size() * 2) capacity <<= 1;
    m_keys.assign(capacity, EmptyKey);
    m_values.assign(capacity, 0);
    for (size_t i = 0; i < m_records.size(); ++i) {
        const size_t probe = probeFor(m_records[i].frn);
        m_keys[probe] = m_records[i].frn;
        m_values[probe] = static_cast<uint32_t>(i);
    }
}

uint64_t FileIndexStore::approximateBytes() const noexcept {
    return m_arena.approximateBytes() +
           static_cast<uint64_t>(m_records.capacity()) * sizeof(StoredFileRecord) +
           static_cast<uint64_t>(m_pinyin.capacity()) * sizeof(PinyinEntry) +
           static_cast<uint64_t>(m_keys.capacity()) * sizeof(uint64_t) +
           static_cast<uint64_t>(m_values.capacity()) * sizeof(uint32_t);
}

}  // namespace tools3000::service
