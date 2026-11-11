#include "FileIndexStore.h"
#include "PinyinEngine.h"

#include <algorithm>
#include <cwctype>

namespace tools3000::service {

namespace {

// Multiplier from splitmix64. NTFS file reference numbers carry a sequence
// number in their high bits and a dense record index in their low bits, so the
// raw value clusters badly under a power-of-two mask and needs mixing.
constexpr uint64_t kMixConstant = 0x9E3779B97F4A7C15ULL;

inline wchar_t foldAsciiLower(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return ch;
}

inline bool eqAsciiLower(std::wstring_view s, std::wstring_view lowerLit) noexcept {
    if (s.size() != lowerLit.size()) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (foldAsciiLower(s[i]) != lowerLit[i]) return false;
    }
    return true;
}

inline bool endsWithAsciiLower(std::wstring_view s, std::wstring_view lowerSuffix) noexcept {
    if (s.size() <= lowerSuffix.size()) return false;
    const size_t offset = s.size() - lowerSuffix.size();
    for (size_t i = 0; i < lowerSuffix.size(); ++i) {
        if (foldAsciiLower(s[offset + i]) != lowerSuffix[i]) return false;
    }
    return true;
}

inline bool containsUninstallKeyword(std::wstring_view name) noexcept {
    if (name.find(L"卸载") != std::wstring_view::npos) return true;
    constexpr std::wstring_view kUninst = L"uninstall";
    if (name.size() < kUninst.size()) return false;
    const size_t limit = name.size() - kUninst.size();
    for (size_t i = 0; i <= limit; ++i) {
        bool ok = true;
        for (size_t j = 0; j < kUninst.size(); ++j) {
            if (foldAsciiLower(name[i + j]) != kUninst[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

inline int32_t driveBasePriority(char driveLetter) noexcept {
    return (driveLetter != 'C' && driveLetter != 'c') ? 800 : 200;
}

void stepDirectoryState(
    std::wstring_view dirName,
    char driveLetter,
    uint8_t parentClassFlags,
    uint8_t parentStateBits,
    uint16_t parentDepth,
    uint8_t& outClassFlags,
    uint8_t& outStateBits,
    uint16_t& outDepth,
    int32_t& outFolderPriority,
    uint8_t& outPathTier
) noexcept {
    const int32_t basePrio = driveBasePriority(driveLetter);
    if (dirName.empty() || dirName == L".") {
        outClassFlags = parentClassFlags;
        outStateBits = parentStateBits;
        outDepth = parentDepth;
        outFolderPriority = basePrio;
        outPathTier = kPathTierPrimaryUser;
        return;
    }

    const uint16_t depth = (parentDepth < 60000u) ? static_cast<uint16_t>(parentDepth + 1u) : parentDepth;
    const bool parentWasWin = (parentStateBits & kDirStateParentWindows) != 0;
    const bool parentWasOffice = (parentStateBits & kDirStateParentOffice) != 0;

    uint8_t state = static_cast<uint8_t>(parentStateBits & ~(kDirStateParentWindows | kDirStateParentOffice));
    uint8_t cls = parentClassFlags;

    if (eqAsciiLower(dirName, L"windows")) {
        state |= kDirStateParentWindows;
    } else if (eqAsciiLower(dirName, L"office")) {
        state |= kDirStateParentOffice;
    }

    if (eqAsciiLower(dirName, L"$recycle.bin")) {
        cls |= (kDirClassRecycleBin | kDirClassContentSysPruned | kDirClassBottomPriority);
        cls = static_cast<uint8_t>(cls & ~kDirClassAppLauncherDir);
    } else if (eqAsciiLower(dirName, L"recent") && (parentWasWin || parentWasOffice)) {
        cls |= (kDirClassRecentDir | kDirClassBottomPriority);
        cls = static_cast<uint8_t>(cls & ~kDirClassAppLauncherDir);
    } else if ((eqAsciiLower(dirName, L"winsxs") || eqAsciiLower(dirName, L"system32")) && parentWasWin) {
        cls |= kDirClassContentSysPruned;
    }

    if (eqAsciiLower(dirName, L"users")) {
        state |= kDirStateSawUsers;
    } else if (eqAsciiLower(dirName, L"start menu")) {
        if ((cls & (kDirClassRecycleBin | kDirClassRecentDir | kDirClassBottomPriority)) == 0) {
            state |= kDirStateInStartMenu;
            cls |= kDirClassAppLauncherDir;
            cls = static_cast<uint8_t>(cls & ~kDirClassLowAppPriority);
        }
    } else if (eqAsciiLower(dirName, L"programs") && (state & kDirStateInStartMenu) != 0) {
        if ((cls & (kDirClassRecycleBin | kDirClassRecentDir | kDirClassBottomPriority)) == 0) {
            state |= kDirStateInPrograms;
            cls |= kDirClassAppLauncherDir;
        }
    }

    if (eqAsciiLower(dirName, L"npm-cache") || eqAsciiLower(dirName, L"pip") ||
        eqAsciiLower(dirName, L"go-build") || eqAsciiLower(dirName, L".gradle") ||
        eqAsciiLower(dirName, L"temp") || eqAsciiLower(dirName, L"tmp") ||
        eqAsciiLower(dirName, L"cache") || eqAsciiLower(dirName, L"caches") ||
        eqAsciiLower(dirName, L"inetcache") || eqAsciiLower(dirName, L"webcache") ||
        eqAsciiLower(dirName, L"gpucache") || eqAsciiLower(dirName, L"code cache") ||
        eqAsciiLower(dirName, L"shadercache") || eqAsciiLower(dirName, L"recent") ||
        eqAsciiLower(dirName, L"prefetch") || eqAsciiLower(dirName, L"winsxs") ||
        eqAsciiLower(dirName, L"$recycle.bin") || eqAsciiLower(dirName, L"node_modules") ||
        eqAsciiLower(dirName, L".git") || eqAsciiLower(dirName, L"cefcache") ||
        eqAsciiLower(dirName, L"crashpad") || eqAsciiLower(dirName, L"coverage_report") ||
        eqAsciiLower(dirName, L"__pycache__") || eqAsciiLower(dirName, L".vs")) {
        cls |= kDirClassBottomPriority;
        cls = static_cast<uint8_t>(cls & ~kDirClassAppLauncherDir);
    } else if ((cls & kDirClassBottomPriority) == 0) {
        if (eqAsciiLower(dirName, L"xwechat_files") || eqAsciiLower(dirName, L"xwechat") ||
            eqAsciiLower(dirName, L"wechat files") || eqAsciiLower(dirName, L"tencent files") ||
            eqAsciiLower(dirName, L"nt_qq") || eqAsciiLower(dirName, L"wxwork") ||
            eqAsciiLower(dirName, L"wecom") || eqAsciiLower(dirName, L"dingtalk") ||
            eqAsciiLower(dirName, L"feishu") || eqAsciiLower(dirName, L"lark") ||
            eqAsciiLower(dirName, L"telegram desktop") || eqAsciiLower(dirName, L"filestorage") ||
            eqAsciiLower(dirName, L"file_recv") || eqAsciiLower(dirName, L"filerecv") ||
            eqAsciiLower(dirName, L"msgattach")) {
            cls |= kDirClassChatHistory;
        } else if (eqAsciiLower(dirName, L"appdata") || eqAsciiLower(dirName, L"program files") ||
                   eqAsciiLower(dirName, L"programdata") || eqAsciiLower(dirName, L"program files (x86)") ||
                   eqAsciiLower(dirName, L"windows")) {
            if ((state & kDirStateInStartMenu) == 0) {
                cls |= kDirClassLowAppPriority;
            }
        } else if (eqAsciiLower(dirName, L"desktop")) {
            state |= kDirStateUserShellDir;
            if ((state & kDirStateSawUsers) != 0 &&
                (cls & (kDirClassBottomPriority | kDirClassChatHistory | kDirClassLowAppPriority)) == 0) {
                state |= kDirStateUserDesktop;
                cls |= kDirClassAppLauncherDir;
            }
        } else if (eqAsciiLower(dirName, L"documents") || eqAsciiLower(dirName, L"downloads")) {
            state |= kDirStateUserShellDir;
        } else if (eqAsciiLower(dirName, L"repo") || eqAsciiLower(dirName, L"repos") ||
                   eqAsciiLower(dirName, L"workspace") || eqAsciiLower(dirName, L"projects") ||
                   eqAsciiLower(dirName, L"code") || eqAsciiLower(dirName, L"src") ||
                   eqAsciiLower(dirName, L"source") || eqAsciiLower(dirName, L"dev") ||
                   eqAsciiLower(dirName, L"github")) {
            cls |= kDirClassDevWorkspace;
        }
    }

    int32_t prio = basePrio;
    uint8_t tier = kPathTierPrimaryUser;
    if ((cls & kDirClassBottomPriority) != 0) {
        prio = 1;
        tier = kPathTierTempNoise;
    } else if ((cls & kDirClassChatHistory) != 0) {
        prio = 80;
        tier = kPathTierChatHistory;
    } else if ((cls & kDirClassLowAppPriority) != 0) {
        prio = (std::min)(prio, 20);
        tier = kPathTierSystemAppData;
    } else {
        if ((state & kDirStateUserShellDir) != 0) {
            prio = (std::max)(prio, 2000);
        }
        if ((cls & kDirClassDevWorkspace) != 0) {
            prio = (std::max)(prio, 3500);
        }
        if (depth >= 1 && depth <= 3) {
            prio += static_cast<int32_t>((4 - depth) * 300);
        }
        tier = kPathTierPrimaryUser;
    }

    outClassFlags = cls;
    outStateBits = state;
    outDepth = depth;
    outFolderPriority = prio;
    outPathTier = tier;
}

}  // namespace

void computeLeafTierAndPriority(
    std::wstring_view leafName,
    bool isDirectory,
    uint64_t parentFrn,
    uint8_t parentDirClassFlags,
    uint8_t parentDirStateBits,
    int32_t parentFolderPriority,
    uint8_t& outPathTier,
    int32_t& outFolderPriority
) noexcept {
    if (!isDirectory && !leafName.empty()) {
        if (leafName.rfind(L"~$", 0) == 0 ||
            endsWithAsciiLower(leafName, L".tmp") ||
            endsWithAsciiLower(leafName, L".bak") ||
            endsWithAsciiLower(leafName, L".temp") ||
            endsWithAsciiLower(leafName, L".part") ||
            endsWithAsciiLower(leafName, L".crdownload")) {
            outPathTier = kPathTierTempNoise;
            outFolderPriority = 1;
            return;
        }

        const bool isAppShortcutExt =
            endsWithAsciiLower(leafName, L".lnk") ||
            endsWithAsciiLower(leafName, L".url") ||
            endsWithAsciiLower(leafName, L".appref-ms");
        const bool isDeskLink = endsWithAsciiLower(leafName, L".desklink");

        if (isAppShortcutExt || isDeskLink) {
            if ((parentDirClassFlags & (kDirClassBottomPriority | kDirClassRecentDir | kDirClassRecycleBin)) != 0) {
                outPathTier = kPathTierTempNoise;
                outFolderPriority = 1;
                return;
            }
            if (isAppShortcutExt && (parentDirClassFlags & kDirClassAppLauncherDir) != 0) {
                if (containsUninstallKeyword(leafName)) {
                    outPathTier = kPathTierSystemAppData;
                    outFolderPriority = 50;
                    return;
                }
                outPathTier = kPathTierAppLauncher;
                if ((parentDirStateBits & kDirStateInPrograms) != 0) {
                    outFolderPriority = 4800;
                } else if ((parentDirStateBits & kDirStateInStartMenu) != 0) {
                    outFolderPriority = 4600;
                } else {
                    outFolderPriority = 4500;
                }
                return;
            }
            outPathTier = kPathTierGenericShortcut;
            outFolderPriority = 10;
            return;
        }
    }

    if ((parentDirClassFlags & kDirClassBottomPriority) != 0) {
        outPathTier = kPathTierTempNoise;
        outFolderPriority = 1;
        return;
    }
    if ((parentDirClassFlags & kDirClassChatHistory) != 0) {
        outPathTier = kPathTierChatHistory;
        outFolderPriority = 80;
        return;
    }
    if ((parentDirClassFlags & kDirClassLowAppPriority) != 0) {
        outPathTier = kPathTierSystemAppData;
        outFolderPriority = 20;
        return;
    }

    outPathTier = kPathTierPrimaryUser;
    outFolderPriority = (parentFrn == 0) ? 200 : parentFolderPriority;
}

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
    m_freePinyinSlots.clear();
    m_freePinyinSlots.shrink_to_fit();
    m_slots.clear();
    m_slots.shrink_to_fit();
    m_liveCount = 0;
    m_tombstones = 0;
}

size_t FileIndexStore::findProbeIndex(uint64_t frn) const noexcept {
    if (m_slots.empty() || frn == EmptyKey) return kNoSlotIndex;
    const size_t mask = m_slots.size() - 1;
    size_t probe = static_cast<size_t>(mixKey(frn)) & mask;
    for (size_t steps = 0; steps < m_slots.size(); ++steps) {
        const uint32_t recIdx = m_slots[probe];
        if (recIdx == kEmptySlot) {
            return kNoSlotIndex;
        }
        if (recIdx != kTombstoneSlot) {
            const auto& rec = m_records[recIdx];
            if (!rec.isDeleted() && rec.frn == frn) {
                return probe;
            }
        }
        probe = (probe + 1) & mask;
    }
    return kNoSlotIndex;
}

size_t FileIndexStore::probeForUpsert(uint64_t frn, size_t& outFirstTombstone) const noexcept {
    outFirstTombstone = kNoSlotIndex;
    const size_t mask = m_slots.size() - 1;
    size_t probe = static_cast<size_t>(mixKey(frn)) & mask;
    for (size_t steps = 0; steps < m_slots.size(); ++steps) {
        const uint32_t recIdx = m_slots[probe];
        if (recIdx == kEmptySlot) {
            return probe;
        }
        if (recIdx == kTombstoneSlot) {
            if (outFirstTombstone == kNoSlotIndex) {
                outFirstTombstone = probe;
            }
        } else {
            const auto& rec = m_records[recIdx];
            if (!rec.isDeleted() && rec.frn == frn) {
                return probe;
            }
        }
        probe = (probe + 1) & mask;
    }
    return outFirstTombstone != kNoSlotIndex ? outFirstTombstone : 0;
}

void FileIndexStore::rehash(size_t minimumSlots) {
    // Size strictly from live records + requested minimum so tombstone-triggered
    // rebuilds purge tombstones in place instead of doubling `m_slots` forever (`P-01`).
    const size_t liveFloor = (m_liveCount + 1) * 2;
    const size_t targetMin = (std::max)(minimumSlots, liveFloor);
    size_t capacity = 1024;
    while (capacity < targetMin) capacity <<= 1;

    std::vector<uint32_t> newSlots(capacity, kEmptySlot);
    const size_t mask = capacity - 1;

    for (uint32_t recIdx : m_slots) {
        if (recIdx == kEmptySlot || recIdx == kTombstoneSlot) continue;
        if (recIdx >= m_records.size()) continue;
        const auto& rec = m_records[recIdx];
        if (rec.isDeleted() || rec.frn == EmptyKey) continue;
        size_t probe = static_cast<size_t>(mixKey(rec.frn)) & mask;
        while (newSlots[probe] != kEmptySlot) {
            probe = (probe + 1) & mask;
        }
        newSlots[probe] = recIdx;
    }

    m_slots = std::move(newSlots);
    m_tombstones = 0;
}

void FileIndexStore::insertNewSlot(uint64_t frn, uint32_t recordSlot) {
    // Linear probing degrades past ~70% occupied (live + tombstones) slots.
    if ((m_liveCount + m_tombstones + 1) * 10 >= m_slots.size() * 7) {
        const size_t desiredMin = ((m_liveCount + 1) * 10 >= m_slots.size() * 5)
            ? (m_slots.size() * 2)
            : m_slots.size();
        rehash(desiredMin);
    }

    size_t firstTombstone = kNoSlotIndex;
    const size_t probe = probeForUpsert(frn, firstTombstone);
    const size_t target = (firstTombstone != kNoSlotIndex) ? firstTombstone : probe;
    if (m_slots[target] == kTombstoneSlot && m_tombstones > 0) {
        --m_tombstones;
    }
    m_slots[target] = recordSlot;
}

uint32_t FileIndexStore::allocatePinyinSlot(const PinyinEntry& entry) {
    if (!m_freePinyinSlots.empty()) {
        const uint32_t slot = m_freePinyinSlots.back();
        m_freePinyinSlots.pop_back();
        m_pinyin[slot] = entry;
        return slot;
    }
    m_pinyin.push_back(entry);
    return static_cast<uint32_t>(m_pinyin.size() - 1);
}

void FileIndexStore::releasePinyinSlot(uint32_t& slot) noexcept {
    if (slot != 0 && slot < m_pinyin.size()) {
        m_pinyin[slot] = PinyinEntry{};
        m_freePinyinSlots.push_back(slot);
    }
    slot = 0;
}

void FileIndexStore::applyIncrementalTier(StoredFileRecord& record, std::wstring_view nameView) noexcept {
    const int32_t basePrio = driveBasePriority(m_driveLetter);
    uint8_t parentClass = 0;
    uint8_t parentState = 0;
    uint16_t parentDepth = 0;
    int32_t parentPrio = basePrio;

    if (record.parentFrn != 0 && record.parentFrn != record.frn) {
        const StoredFileRecord* parent = find(record.parentFrn);
        if (parent == nullptr || !parent->isTierComputed()) {
            record.flags = static_cast<uint8_t>(record.flags & ~StoredFileRecord::FlagTierComputed);
            record.pathTier = kPathTierPrimaryUser;
            record.folderPriority = basePrio;
            return;
        }
        parentClass = parent->dirClassFlags;
        parentState = parent->dirStateBits;
        parentDepth = parent->treeDepth;
        parentPrio = parent->folderPriority;
    }

    if (record.isDirectory()) {
        stepDirectoryState(
            nameView,
            m_driveLetter,
            parentClass,
            parentState,
            parentDepth,
            record.dirClassFlags,
            record.dirStateBits,
            record.treeDepth,
            record.folderPriority,
            record.pathTier);
    } else {
        record.dirClassFlags = parentClass;
        record.dirStateBits = parentState;
        record.treeDepth = parentDepth;
        computeLeafTierAndPriority(
            nameView,
            false,
            record.parentFrn,
            parentClass,
            parentState,
            parentPrio,
            record.pathTier,
            record.folderPriority);
    }
    record.flags |= StoredFileRecord::FlagTierComputed;
}

void FileIndexStore::upsert(const FileRecordInit& init, bool metadataHydrated) {
    if (init.fileReferenceNumber == EmptyKey) return;
    if (m_slots.empty()) rehash(1024);

    const uint16_t nameLen = static_cast<uint16_t>(std::min<size_t>(init.fileName.size(), 0xFFFF));
    const std::wstring_view nameView = std::wstring_view{init.fileName}.substr(0, nameLen);
    uint8_t newFlags = init.isDirectory ? StoredFileRecord::FlagDirectory : 0;
    if (metadataHydrated || init.fileSize > 0 || init.creationTime > 0) {
        newFlags |= StoredFileRecord::FlagMetadataHydrated;
    }

    size_t firstTombstone = kNoSlotIndex;
    const size_t existingProbe = probeForUpsert(init.fileReferenceNumber, firstTombstone);
    const uint32_t existingRecSlot = m_slots[existingProbe];
    const bool hasExistingLive =
        existingRecSlot != kEmptySlot &&
        existingRecSlot != kTombstoneSlot &&
        !m_records[existingRecSlot].isDeleted() &&
        m_records[existingRecSlot].frn == init.fileReferenceNumber;

    // Fast path for USN metadata updates where FRN and fileName are unchanged:
    // reuse existing StringArena offset and pinyinSlot without growing `m_pinyin`.
    if (hasExistingLive) {
        auto& existing = m_records[existingRecSlot];
        if (existing.nameLength == nameLen &&
            m_arena.view(existing.nameOffset, existing.nameLength) == nameView) {
            const bool parentChanged = (existing.parentFrn != init.parentFileReferenceNumber);
            const bool dirFlagChanged = (existing.isDirectory() != init.isDirectory);
            const uint8_t preservedHydrated =
                static_cast<uint8_t>(existing.flags & StoredFileRecord::FlagMetadataHydrated);
            const uint8_t preservedTier =
                (!parentChanged && !dirFlagChanged)
                    ? static_cast<uint8_t>(existing.flags & StoredFileRecord::FlagTierComputed)
                    : 0u;
            existing.parentFrn = init.parentFileReferenceNumber;
            existing.fileSize = init.fileSize;
            existing.creationTime = init.creationTime;
            existing.lastWriteTime = init.lastWriteTime;
            existing.attributes = init.fileAttributes;
            existing.flags = static_cast<uint8_t>(newFlags | preservedHydrated | preservedTier);
            if (parentChanged || dirFlagChanged || !existing.isTierComputed()) {
                applyIncrementalTier(existing, nameView);
                if (existing.isDirectory() && (parentChanged || dirFlagChanged)) {
                    recomputeHierarchyMetadata();
                }
            }
            return;
        }
    }

    // Check CJK presence for side-table pinyin indexing.
    bool hasChinese = false;
    for (wchar_t ch : nameView) {
        const auto code = static_cast<uint16_t>(ch);
        if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3400 && code <= 0x4DBF)) {
            hasChinese = true;
            break;
        }
    }

    PinyinEntry newPinyin{};
    if (hasChinese) {
        const std::wstring initials = SearchExpression::normalize(PinyinEngine::GetInitials(init.fileName));
        const std::wstring full = SearchExpression::normalize(PinyinEngine::GetFullPinyin(init.fileName));
        newPinyin.initialsLength = static_cast<uint16_t>(std::min<size_t>(initials.size(), 0xFFFF));
        newPinyin.fullLength = static_cast<uint16_t>(std::min<size_t>(full.size(), 0xFFFF));
        newPinyin.initialsOffset = m_arena.intern(std::wstring_view{initials}.substr(0, newPinyin.initialsLength));
        newPinyin.fullOffset = m_arena.intern(std::wstring_view{full}.substr(0, newPinyin.fullLength));
    }

    if (hasExistingLive) {
        auto& existing = m_records[existingRecSlot];
        const bool wasDirectory = existing.isDirectory();
        const uint8_t preservedHydrated =
            static_cast<uint8_t>(existing.flags & StoredFileRecord::FlagMetadataHydrated);

        // `P-01`: Reuse existing `pinyinSlot` in-place on CJK -> CJK rename;
        // recycle old `pinyinSlot` into free-list on CJK -> ASCII rename!
        uint32_t updatedPinyinSlot = existing.pinyinSlot;
        if (hasChinese) {
            if (updatedPinyinSlot != 0 && updatedPinyinSlot < m_pinyin.size()) {
                m_pinyin[updatedPinyinSlot] = newPinyin;
            } else {
                updatedPinyinSlot = allocatePinyinSlot(newPinyin);
            }
        } else if (updatedPinyinSlot != 0) {
            releasePinyinSlot(updatedPinyinSlot);
        }

        existing.parentFrn = init.parentFileReferenceNumber;
        existing.fileSize = init.fileSize;
        existing.creationTime = init.creationTime;
        existing.lastWriteTime = init.lastWriteTime;
        existing.attributes = init.fileAttributes;
        existing.nameLength = nameLen;
        existing.nameOffset = m_arena.intern(nameView);
        existing.pinyinSlot = updatedPinyinSlot;
        existing.flags = static_cast<uint8_t>(newFlags | preservedHydrated);
        applyIncrementalTier(existing, nameView);
        if (wasDirectory || existing.isDirectory()) {
            recomputeHierarchyMetadata();
        }
        return;
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
    if (hasChinese) {
        record.pinyinSlot = allocatePinyinSlot(newPinyin);
    }
    applyIncrementalTier(record, nameView);

    m_records.push_back(record);
    insertNewSlot(init.fileReferenceNumber, static_cast<uint32_t>(m_records.size() - 1));
    ++m_liveCount;
}

bool FileIndexStore::updateMetadata(uint64_t frn, uint64_t fileSize,
                                    uint64_t creationTime, uint64_t lastWriteTime) noexcept {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return false;

    auto& record = m_records[m_slots[probe]];
    if (record.isDeleted()) return false;

    record.fileSize = fileSize;
    if (creationTime != 0) record.creationTime = creationTime;
    if (lastWriteTime != 0) record.lastWriteTime = lastWriteTime;
    record.flags |= StoredFileRecord::FlagMetadataHydrated;
    return true;
}

bool FileIndexStore::erase(uint64_t frn) {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return false;

    auto& record = m_records[m_slots[probe]];
    if (record.isDeleted()) return false;

    if (record.pinyinSlot != 0) {
        releasePinyinSlot(record.pinyinSlot);
    }
    record.flags |= StoredFileRecord::FlagDeleted;
    m_slots[probe] = kTombstoneSlot;
    --m_liveCount;
    ++m_tombstones;
    return true;
}

const StoredFileRecord* FileIndexStore::find(uint64_t frn) const noexcept {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return nullptr;
    const auto& record = m_records[m_slots[probe]];
    return record.isDeleted() ? nullptr : &record;
}

void FileIndexStore::recomputeHierarchyMetadata() {
    if (m_records.empty()) return;

    const int32_t basePrio = driveBasePriority(m_driveLetter);
    for (auto& rec : m_records) {
        if (!rec.isDeleted()) {
            rec.flags = static_cast<uint8_t>(rec.flags & ~StoredFileRecord::FlagTierComputed);
        }
    }

    constexpr size_t kMaxChainDepth = 512;
    uint32_t chain[kMaxChainDepth];

    // Pass 1: Memoized top-down evaluation of all live directory records.
    for (size_t i = 0; i < m_records.size(); ++i) {
        auto& startRec = m_records[i];
        if (startRec.isDeleted() || !startRec.isDirectory() || startRec.isTierComputed()) {
            continue;
        }

        size_t chainLen = 0;
        uint32_t curIdx = static_cast<uint32_t>(i);
        uint8_t curClass = 0;
        uint8_t curState = 0;
        uint16_t curDepth = 0;

        while (chainLen < kMaxChainDepth) {
            auto& curRec = m_records[curIdx];
            if (curRec.isTierComputed()) {
                curClass = curRec.dirClassFlags;
                curState = curRec.dirStateBits;
                curDepth = curRec.treeDepth;
                break;
            }
            chain[chainLen++] = curIdx;
            if (curRec.parentFrn == 0 || curRec.parentFrn == curRec.frn) {
                break;
            }
            const size_t parentProbe = findProbeIndex(curRec.parentFrn);
            if (parentProbe == kNoSlotIndex) {
                break;
            }
            const uint32_t parentIdx = m_slots[parentProbe];
            bool cycle = false;
            for (size_t k = 0; k < chainLen; ++k) {
                if (chain[k] == parentIdx) {
                    cycle = true;
                    break;
                }
            }
            if (cycle) break;
            curIdx = parentIdx;
        }

        for (size_t step = chainLen; step > 0; --step) {
            auto& dirRec = m_records[chain[step - 1]];
            const std::wstring_view dirName = m_arena.view(dirRec.nameOffset, dirRec.nameLength);
            int32_t dirPrio = basePrio;
            uint8_t dirTier = kPathTierPrimaryUser;
            stepDirectoryState(
                dirName,
                m_driveLetter,
                curClass,
                curState,
                curDepth,
                dirRec.dirClassFlags,
                dirRec.dirStateBits,
                dirRec.treeDepth,
                dirPrio,
                dirTier);
            dirRec.folderPriority = dirPrio;
            dirRec.pathTier = dirTier;
            dirRec.flags |= StoredFileRecord::FlagTierComputed;

            curClass = dirRec.dirClassFlags;
            curState = dirRec.dirStateBits;
            curDepth = dirRec.treeDepth;
        }
    }

    // Pass 2: O(1) evaluation of all live file records from their already-computed parent directory.
    uint64_t cachedParentFrn = UINT64_MAX;
    uint8_t cachedParentClass = 0;
    uint8_t cachedParentState = 0;
    uint16_t cachedParentDepth = 0;
    int32_t cachedParentPrio = basePrio;

    for (auto& rec : m_records) {
        if (rec.isDeleted() || rec.isDirectory()) continue;
        if (rec.parentFrn == 0 || rec.parentFrn == rec.frn) {
            rec.dirClassFlags = 0;
            rec.dirStateBits = 0;
            rec.treeDepth = 0;
            computeLeafTierAndPriority(
                m_arena.view(rec.nameOffset, rec.nameLength),
                false,
                0,
                0,
                0,
                basePrio,
                rec.pathTier,
                rec.folderPriority);
            rec.flags |= StoredFileRecord::FlagTierComputed;
            continue;
        }

        if (rec.parentFrn != cachedParentFrn) {
            cachedParentFrn = rec.parentFrn;
            const StoredFileRecord* parent = find(rec.parentFrn);
            if (parent != nullptr) {
                cachedParentClass = parent->dirClassFlags;
                cachedParentState = parent->dirStateBits;
                cachedParentDepth = parent->treeDepth;
                cachedParentPrio = parent->folderPriority;
            } else {
                cachedParentClass = 0;
                cachedParentState = 0;
                cachedParentDepth = 0;
                cachedParentPrio = basePrio;
            }
        }

        rec.dirClassFlags = cachedParentClass;
        rec.dirStateBits = cachedParentState;
        rec.treeDepth = cachedParentDepth;
        computeLeafTierAndPriority(
            m_arena.view(rec.nameOffset, rec.nameLength),
            false,
            rec.parentFrn,
            cachedParentClass,
            cachedParentState,
            cachedParentPrio,
            rec.pathTier,
            rec.folderPriority);
        rec.flags |= StoredFileRecord::FlagTierComputed;
    }
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
    const size_t deadRecords = (m_records.size() > m_liveCount) ? (m_records.size() - m_liveCount) : 0;
    if (deadRecords == 0 || deadRecords * 4 < m_records.size()) return;

    std::vector<StoredFileRecord> live;
    live.reserve(m_liveCount);
    for (const auto& record : m_records) {
        if (!record.isDeleted()) live.push_back(record);
    }
    m_records = std::move(live);
    m_tombstones = 0;

    size_t capacity = 1024;
    while (capacity < (m_records.size() + 1) * 2) capacity <<= 1;
    m_slots.assign(capacity, kEmptySlot);
    const size_t mask = capacity - 1;
    for (size_t i = 0; i < m_records.size(); ++i) {
        size_t probe = static_cast<size_t>(mixKey(m_records[i].frn)) & mask;
        while (m_slots[probe] != kEmptySlot) {
            probe = (probe + 1) & mask;
        }
        m_slots[probe] = static_cast<uint32_t>(i);
    }
}

uint64_t FileIndexStore::approximateBytes() const noexcept {
    return m_arena.approximateBytes() +
           static_cast<uint64_t>(m_records.capacity()) * sizeof(StoredFileRecord) +
           static_cast<uint64_t>(m_pinyin.capacity()) * sizeof(PinyinEntry) +
           static_cast<uint64_t>(m_freePinyinSlots.capacity()) * sizeof(uint32_t) +
           static_cast<uint64_t>(m_slots.capacity()) * sizeof(uint32_t);
}

}  // namespace tools3000::service
