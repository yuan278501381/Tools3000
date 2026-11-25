#pragma once
#include "SearchExpression.h"
#include "StringArena.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
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

// One indexed file, tightly packed to exactly 40 bytes (World-Class 1B: -37.5% vs 64B).
//
// - Pure ASCII names live in `StringArena`'s 1-byte ASCII track (`FlagAsciiName`);
//   CJK / multilingual names live in native 2-byte UTF-16 blocks with ZERO 3-byte UTF-8 bloat.
// - `fileSize32` stores exact sizes up to 4 GiB-2 inline; >= 4 GiB overflows cleanly
//   into `m_largeSizes` (`kLargeSizeSentinel == UINT32_MAX`), keeping exact 64-bit accuracy.
// - `lastWriteSec` stores 1-second Unix timestamps (1970..2106, 0 == unset), halving
//   timestamp footprint while preserving `dm:` filters and recent-first ranking.
// - `creationTime` is kept in a cold bounded side-map (zero RAM on cold MFT scan).
struct StoredFileRecord {
    static constexpr uint32_t kLargeSizeSentinel = UINT32_MAX;

    static constexpr uint16_t kNameLenMask = 0x03FFu;  // up to 1023 chars (4x NTFS MAX_PATH)
    static constexpr uint16_t kFlagsShift = 10u;

    static constexpr uint8_t FlagDirectory        = 1u << 0;
    static constexpr uint8_t FlagDeleted          = 1u << 1;
    static constexpr uint8_t FlagMetadataHydrated = 1u << 2;
    static constexpr uint8_t FlagTierComputed     = 1u << 3;
    static constexpr uint8_t FlagAsciiName        = 1u << 4;
    static constexpr uint8_t FlagHasCreationTime  = 1u << 5;

    static constexpr uint16_t kPrioMask = 0x1FFFu;     // 0..8191 (max folderPriority ~5700)
    static constexpr uint16_t kTierShift = 13u;        // 3 bits: 0..7

    uint64_t frn = 0;
    uint64_t parentFrn = 0;
    uint32_t nameOffset = 0;
    uint32_t pinyinSlot = 0;       // 0 == no pinyin
    uint32_t fileSize32 = 0;       // < UINT32_MAX inline; UINT32_MAX => m_largeSizes[frn]
    uint32_t lastWriteSec = 0;     // Unix seconds since 1970-01-01 (0 == unset)
    uint16_t attributes16 = 0;     // standard Win32 FILE_ATTRIBUTE_* bits (0x0000..0xFFFF)
    uint16_t nameLenAndFlags = 0;  // low 10 bits: nameLength; high 6 bits: flags
    uint16_t prioAndTier = (static_cast<uint16_t>(kPathTierPrimaryUser) << kTierShift) | 200u;
    uint8_t  dirClassFlags = 0;
    uint8_t  dirStateBits = 0;

    uint16_t nameLength() const noexcept { return static_cast<uint16_t>(nameLenAndFlags & kNameLenMask); }
    void setNameLength(uint16_t len) noexcept {
        const uint16_t clamped = (len > kNameLenMask) ? kNameLenMask : len;
        nameLenAndFlags = static_cast<uint16_t>((nameLenAndFlags & ~kNameLenMask) | clamped);
    }

    uint8_t flags() const noexcept { return static_cast<uint8_t>(nameLenAndFlags >> kFlagsShift); }
    void setFlags(uint8_t f) noexcept {
        nameLenAndFlags = static_cast<uint16_t>((nameLenAndFlags & kNameLenMask) | (static_cast<uint16_t>(f & 0x3Fu) << kFlagsShift));
    }
    void addFlags(uint8_t f) noexcept { setFlags(static_cast<uint8_t>(flags() | f)); }
    void clearFlags(uint8_t f) noexcept { setFlags(static_cast<uint8_t>(flags() & ~f)); }

    uint8_t pathTier() const noexcept { return static_cast<uint8_t>((prioAndTier >> kTierShift) & 0x07u); }
    void setPathTier(uint8_t tier) noexcept {
        const uint16_t t = static_cast<uint16_t>((tier <= 7u ? tier : 7u) << kTierShift);
        prioAndTier = static_cast<uint16_t>((prioAndTier & kPrioMask) | t);
    }

    int32_t folderPriority() const noexcept { return static_cast<int32_t>(prioAndTier & kPrioMask); }
    void setFolderPriority(int32_t prio) noexcept {
        const uint16_t clamped = (prio <= 0) ? 0u : (prio > static_cast<int32_t>(kPrioMask) ? kPrioMask : static_cast<uint16_t>(prio));
        prioAndTier = static_cast<uint16_t>((prioAndTier & ~kPrioMask) | clamped);
    }

    bool isDirectory() const noexcept { return (flags() & FlagDirectory) != 0; }
    bool isDeleted() const noexcept { return (flags() & FlagDeleted) != 0; }
    bool isMetadataHydrated() const noexcept { return (flags() & FlagMetadataHydrated) != 0; }
    bool isTierComputed() const noexcept { return (flags() & FlagTierComputed) != 0; }
    bool isAsciiName() const noexcept { return (flags() & FlagAsciiName) != 0; }
    bool hasCreationTime() const noexcept { return (flags() & FlagHasCreationTime) != 0; }
};

static_assert(sizeof(StoredFileRecord) == 40, "StoredFileRecord must remain 40 bytes");

// Per-thread reusable widen scratch buffer so ASCII names widen in ~3ns on L1 stack/heap with zero per-record allocation.
struct FileViewScratch {
    std::wstring nameBuf;
    std::wstring pinyinBuf;

    FileViewScratch() {
        nameBuf.reserve(256);
        pinyinBuf.reserve(64);
    }
};

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
// Records sit in one contiguous vector (`40 B/record`) so a full scan streams linearly through
// memory, and lookups by file reference number go through a single flat open-addressed
// `m_slots` table (`kEmptySlot` / `kTombstoneSlot` sentinels, <= 70% load factor).
class FileIndexStore {
public:
    static constexpr uint32_t kEmptySlot = UINT32_MAX;
    static constexpr uint32_t kTombstoneSlot = UINT32_MAX - 1;
    static constexpr size_t kMaxCreationTimeCacheEntries = 65536;

    static uint32_t fileTimeToUnixSec(uint64_t ft) noexcept;
    static uint64_t unixSecToFileTime(uint32_t sec) noexcept;

    void setDriveLetter(char driveLetter) noexcept { m_driveLetter = driveLetter; }
    char driveLetter() const noexcept { return m_driveLetter; }

    void reserve(size_t records);
    void clear();

    // Inserts or replaces the record for `init.fileReferenceNumber`.
    // When `bulkImport` is true, skips per-record incremental tier/size-propagation since
    // `recomputeHierarchyMetadata()` will run once after the batch completes.
    void upsert(const FileRecordInit& init, bool metadataHydrated = false, bool bulkImport = false);
    bool updateMetadata(uint64_t frn, uint64_t fileSize, uint64_t creationTime, uint64_t lastWriteTime) noexcept;
    bool markMetadataDirty(uint64_t frn) noexcept;
    bool erase(uint64_t frn);

    // Finalizes `pathTier`, `dirClassFlags`, `dirStateBits`, and `folderPriority`
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
    uint32_t findSlotIndex(uint64_t frn) const noexcept;

    uint64_t fileSizeOf(const StoredFileRecord& record) const noexcept;
    uint64_t lastWriteTimeOf(const StoredFileRecord& record) const noexcept {
        return unixSecToFileTime(record.lastWriteSec);
    }
    uint64_t creationTimeOf(const StoredFileRecord& record) const noexcept;

    // Zero-allocation hot-path view using caller's per-thread scratch buffer.
    FileRecord view(const StoredFileRecord& record, FileViewScratch& scratch) const;

    // Convenience single-record view backed by a thread_local scratch buffer.
    FileRecord view(const StoredFileRecord& record) const;

    void appendNameTo(const StoredFileRecord& record, std::wstring& dst) const {
        m_arena.appendNameTo(record.nameOffset, record.nameLength(), record.isAsciiName(), dst);
    }

    std::wstring copyFileName(const StoredFileRecord& record) const {
        std::wstring out;
        out.reserve(record.nameLength());
        appendNameTo(record, out);
        return out;
    }

    int compareNamesIgnoreCase(const StoredFileRecord& a, const StoredFileRecord& b) const noexcept {
        return m_arena.compareNamesIgnoreCase(
            a.nameOffset, a.nameLength(), a.isAsciiName(),
            b.nameOffset, b.nameLength(), b.isAsciiName());
    }

    int compareNamesExact(const StoredFileRecord& a, const StoredFileRecord& b) const noexcept {
        return m_arena.compareNamesExact(
            a.nameOffset, a.nameLength(), a.isAsciiName(),
            b.nameOffset, b.nameLength(), b.isAsciiName());
    }

    // Visits every live record. Used by snapshot export so that callers never
    // have to materialize a second copy of the whole index.
    void forEach(const std::function<void(const StoredFileRecord&)>& visit) const;

    // Drops dead records when they exceed a quarter of `m_records`.
    void compactIfSparse();

    // Releases excess vector capacity and downsizes an over-provisioned hash table
    // without losing any live records or lookup integrity.
    void shrinkExcessCapacity();

    void releaseBuildScratch() {
        m_arena.releaseInterner();
        m_pinyinAsciiInitialsScratch.clear();
        m_pinyinAsciiInitialsScratch.shrink_to_fit();
        m_pinyinWideInitialsScratch.clear();
        m_pinyinWideInitialsScratch.shrink_to_fit();
    }

    uint64_t approximateBytes() const noexcept;

private:
    struct PinyinEntry {
        uint32_t initialsOffset = 0;
        uint16_t initialsLength = 0;
        uint8_t  isAscii = 1;
        uint8_t  reserved = 0;
    };

    static constexpr uint64_t EmptyKey = 0;
    static constexpr size_t kNoSlotIndex = static_cast<size_t>(-1);

    static size_t minSlotsForLive(size_t liveCount) noexcept {
        // <= 70% load factor: keeps average linear-probe length ~1.8 while avoiding premature 2x doubling.
        return ((liveCount + 1) * 10 + 6) / 7;
    }

    void setFileSize(StoredFileRecord& record, uint64_t size);
    void setCreationTime(StoredFileRecord& record, uint64_t creationTime);
    uint32_t internName(std::wstring_view nameView, bool& outIsAscii);
    uint16_t estimateParentDepth(uint64_t parentFrn) const noexcept;

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
    std::unordered_map<uint64_t, uint64_t> m_largeSizes;    // only files/dirs >= 4 GiB
    std::unordered_map<uint64_t, uint32_t> m_creationTimes; // cold bounded cache (<= 64K entries)
    std::string m_pinyinAsciiInitialsScratch;
    std::wstring m_pinyinWideInitialsScratch;
    size_t m_liveCount = 0;
    size_t m_tombstones = 0;
    char m_driveLetter = 'C';
};

}  // namespace tools3000::service
