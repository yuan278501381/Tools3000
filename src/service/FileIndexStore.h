#pragma once
#include "SearchExpression.h"
#include "StringArena.h"

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace tools3000::service {

// Precomputed semantic path ranking tiers (`P-02` & `U-04`).
enum PathTier : uint8_t {
    kPathTierAppLauncher     = 0,  // Start Menu\Programs, User/Public Desktop .lnk/.url/.appref-ms, AppsFolder UWP
    kPathTierPrimaryUser     = 1,  // Normal user workspaces & documents (Projects, Documents, Downloads, etc.)
    kPathTierChatHistory     = 2,  // WeChat / QQ / Feishu / DingTalk chat copies
    kPathTierSystemAppData   = 3,  // AppData / ProgramData / Program Files / Windows
    kPathTierGenericShortcut = 4,  // Non-launcher .lnk / .url / .desklink
    kPathTierTempNoise       = 5,  // Temp / Cache / Recent / $Recycle.Bin / node_modules / .git / ~$ / .tmp
    kPathTierUncomputed      = 0xFF,
};

enum DirClassFlags : uint8_t {
    kDirClassRecycleBin       = 1u << 0,
    kDirClassRecentDir        = 1u << 1,
    kDirClassContentSysPruned = 1u << 2,
    kDirClassBottomPriority   = 1u << 3,
    kDirClassChatHistory      = 1u << 4,
    kDirClassLowAppPriority   = 1u << 5,
    kDirClassDevWorkspace     = 1u << 6,
    kDirClassAppLauncherDir   = 1u << 7,
};

enum DirStateBits : uint8_t {
    kDirStateParentWindows = 1u << 0,
    kDirStateParentOffice  = 1u << 1,
    kDirStateSawUsers      = 1u << 2,
    kDirStateInStartMenu   = 1u << 3,
    kDirStateInPrograms    = 1u << 4,
    kDirStateUserDesktop   = 1u << 5,
    kDirStateUserShellDir  = 1u << 6,
};

// One indexed file, packed to exactly 64 bytes (1 cache line).
//
// Names live in the store's StringArena and are referenced by offset. Pinyin is
// only produced for CJK names so it sits in a side table addressed by `pinyinSlot`.
// Precomputed `pathTier`, `dirClassFlags`, `dirStateBits`, `treeDepth`, and
// `folderPriority` occupy the trailing 9 bytes that were previously struct padding,
// keeping `sizeof(StoredFileRecord) == 64` with zero memory overhead.
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
    uint8_t pathTier = kPathTierPrimaryUser;
    uint8_t dirClassFlags = 0;
    uint8_t dirStateBits = 0;
    uint16_t treeDepth = 0;
    int32_t folderPriority = 200;

    static constexpr uint8_t FlagDirectory        = 1u << 0;
    static constexpr uint8_t FlagDeleted          = 1u << 1;
    static constexpr uint8_t FlagMetadataHydrated = 1u << 2;
    static constexpr uint8_t FlagTierComputed     = 1u << 3;

    bool isDirectory() const noexcept { return (flags & FlagDirectory) != 0; }
    bool isDeleted() const noexcept { return (flags & FlagDeleted) != 0; }
    bool isMetadataHydrated() const noexcept { return (flags & FlagMetadataHydrated) != 0; }
    bool isTierComputed() const noexcept { return (flags & FlagTierComputed) != 0; }
};

static_assert(sizeof(StoredFileRecord) == 64, "StoredFileRecord must remain 64 bytes");

// Computes leaf `(pathTier, folderPriority)` from parent directory state and leaf name.
void computeLeafTierAndPriority(
    std::wstring_view leafName,
    bool isDirectory,
    uint64_t parentFrn,
    uint8_t parentDirClassFlags,
    uint8_t parentDirStateBits,
    int32_t parentFolderPriority,
    uint8_t& outPathTier,
    int32_t& outFolderPriority
) noexcept;

// Backing store for a volume's file index.
//
// Records sit in one contiguous vector so a full scan streams linearly through
// memory, and lookups by file reference number go through a single flat open-addressed
// `m_slots` table (`kEmptySlot` / `kTombstoneSlot` sentinels, reading `m_records[slot].frn`
// directly without a parallel `m_keys` vector, saving ~50.3 MB on 2.75M files).
class FileIndexStore {
public:
    static constexpr uint32_t kEmptySlot = UINT32_MAX;
    static constexpr uint32_t kTombstoneSlot = UINT32_MAX - 1;

    void setDriveLetter(char driveLetter) noexcept { m_driveLetter = driveLetter; }
    char driveLetter() const noexcept { return m_driveLetter; }

    void reserve(size_t records);
    void clear();

    // Inserts or replaces the record for `init.fileReferenceNumber`.
    void upsert(const FileRecordInit& init, bool metadataHydrated = false);
    bool updateMetadata(uint64_t frn, uint64_t fileSize, uint64_t creationTime, uint64_t lastWriteTime) noexcept;
    bool erase(uint64_t frn);

    // Finalizes `pathTier`, `dirClassFlags`, `dirStateBits`, `treeDepth`, and `folderPriority`
    // across all records after out-of-order MFT enumeration or snapshot import.
    void recomputeHierarchyMetadata();

    size_t size() const noexcept { return m_liveCount; }
    size_t slotCount() const noexcept { return m_records.size(); }
    size_t hashCapacity() const noexcept { return m_slots.size(); }
    size_t tombstoneCount() const noexcept { return m_tombstones; }
    size_t pinyinEntryCount() const noexcept { return m_pinyin.size(); }
    size_t freePinyinSlotCount() const noexcept { return m_freePinyinSlots.size(); }
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

    // Drops dead records when they exceed a quarter of `m_records`.
    void compactIfSparse();

    void releaseBuildScratch() {
        m_arena.releaseInterner();
        m_pinyinAsciiInitialsScratch.clear();
        m_pinyinAsciiInitialsScratch.shrink_to_fit();
        m_pinyinAsciiFullScratch.clear();
        m_pinyinAsciiFullScratch.shrink_to_fit();
        m_pinyinWideInitialsScratch.clear();
        m_pinyinWideInitialsScratch.shrink_to_fit();
        m_pinyinWideFullScratch.clear();
        m_pinyinWideFullScratch.shrink_to_fit();
    }

    uint64_t approximateBytes() const noexcept;

private:
    struct PinyinEntry {
        uint32_t initialsOffset = 0;
        uint32_t fullOffset = 0;
        uint16_t initialsLength = 0;
        uint16_t fullLength = 0;
    };

    static constexpr uint64_t EmptyKey = 0;
    static constexpr size_t kNoSlotIndex = static_cast<size_t>(-1);

    void rehash(size_t minimumSlots);
    void insertNewSlot(uint64_t frn, uint32_t recordSlot);
    size_t findProbeIndex(uint64_t frn) const noexcept;
    size_t probeForUpsert(uint64_t frn, size_t& outFirstTombstone) const noexcept;
    uint32_t allocatePinyinSlot(const PinyinEntry& entry);
    void releasePinyinSlot(uint32_t& slot) noexcept;
    void applyIncrementalTier(StoredFileRecord& record, std::wstring_view nameView) noexcept;
    void propagateAncestorsSizeChange(uint64_t startParentFrn, uint64_t oldSize, uint64_t newSize) noexcept;
    static uint64_t mixKey(uint64_t frn) noexcept;

    StringArena m_arena;
    std::vector<StoredFileRecord> m_records;
    std::vector<PinyinEntry> m_pinyin{PinyinEntry{}};  // index 0 == "no pinyin"
    std::vector<uint32_t> m_freePinyinSlots;
    std::vector<uint32_t> m_slots;
    std::string m_pinyinAsciiInitialsScratch;
    std::string m_pinyinAsciiFullScratch;
    std::wstring m_pinyinWideInitialsScratch;
    std::wstring m_pinyinWideFullScratch;
    size_t m_liveCount = 0;
    size_t m_tombstones = 0;
    char m_driveLetter = 'C';
};

}  // namespace tools3000::service
