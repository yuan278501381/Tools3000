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

uint32_t FileIndexStore::fileTimeToUnixSec(uint64_t ft) noexcept {
    if (ft == 0) return 0;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    constexpr uint64_t kTicksPerSec = 10000000ULL;
    if (ft <= kEpochDiff100ns) return 1u;
    const uint64_t sec = (ft - kEpochDiff100ns) / kTicksPerSec;
    if (sec == 0) return 1u;
    return static_cast<uint32_t>((std::min<uint64_t>)(sec, UINT32_MAX));
}

uint64_t FileIndexStore::unixSecToFileTime(uint32_t sec) noexcept {
    if (sec == 0) return 0;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    constexpr uint64_t kTicksPerSec = 10000000ULL;
    return kEpochDiff100ns + static_cast<uint64_t>(sec) * kTicksPerSec;
}

uint64_t FileIndexStore::fileSizeOf(const StoredFileRecord& record) const noexcept {
    if (record.fileSize32 != StoredFileRecord::kLargeSizeSentinel) {
        return static_cast<uint64_t>(record.fileSize32);
    }
    const auto it = m_largeSizes.find(record.frn);
    return (it != m_largeSizes.end()) ? it->second : 0ULL;
}

uint64_t FileIndexStore::creationTimeOf(const StoredFileRecord& record) const noexcept {
    if (!record.hasCreationTime()) return 0ULL;
    const auto it = m_creationTimes.find(record.frn);
    return (it != m_creationTimes.end()) ? unixSecToFileTime(it->second) : 0ULL;
}

void FileIndexStore::setFileSize(StoredFileRecord& record, uint64_t size) {
    if (size < StoredFileRecord::kLargeSizeSentinel) {
        if (record.fileSize32 == StoredFileRecord::kLargeSizeSentinel) {
            m_largeSizes.erase(record.frn);
        }
        record.fileSize32 = static_cast<uint32_t>(size);
    } else {
        record.fileSize32 = StoredFileRecord::kLargeSizeSentinel;
        m_largeSizes[record.frn] = size;
    }
}

void FileIndexStore::setCreationTime(StoredFileRecord& record, uint64_t creationTime) {
    if (creationTime == 0) {
        if (record.hasCreationTime()) {
            m_creationTimes.erase(record.frn);
            record.clearFlags(StoredFileRecord::FlagHasCreationTime);
        }
        return;
    }
    if (record.hasCreationTime() || m_creationTimes.size() < kMaxCreationTimeCacheEntries) {
        m_creationTimes[record.frn] = fileTimeToUnixSec(creationTime);
        record.addFlags(StoredFileRecord::FlagHasCreationTime);
    }
}

uint32_t FileIndexStore::internName(std::wstring_view nameView, bool& outIsAscii) {
    if (nameView.empty()) {
        outIsAscii = true;
        return 0;
    }
    outIsAscii = StringArena::isAsciiOnly(nameView);
    return outIsAscii
        ? m_arena.internAsciiFromWide(nameView)
        : m_arena.intern(nameView);
}

uint16_t FileIndexStore::estimateParentDepth(uint64_t parentFrn) const noexcept {
    uint16_t depth = 0;
    uint64_t cur = parentFrn;
    // Only depths 1..3 affect folderPriority bonus; capping walk at 8 is O(1) and cycle-safe.
    while (cur != 0 && depth < 8) {
        const StoredFileRecord* p = find(cur);
        if (!p || !p->isDirectory()) break;
        ++depth;
        if (p->parentFrn == 0 || p->parentFrn == cur) break;
        cur = p->parentFrn;
    }
    return depth;
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
    m_pinyin.reserve((std::max<size_t>)(256, records / 12));
    m_arena.reserveInternTable((std::max<size_t>)(512, records / 2));
    rehash(minSlotsForLive(records));
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
    m_largeSizes.clear();
    m_creationTimes.clear();
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
    const size_t liveFloor = minSlotsForLive(m_liveCount);
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
        const size_t desiredMin = ((m_liveCount + 1) * 10 >= m_slots.size() * 7)
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
            record.clearFlags(StoredFileRecord::FlagTierComputed);
            record.setPathTier(kPathTierPrimaryUser);
            record.setFolderPriority(basePrio);
            return;
        }
        parentClass = parent->dirClassFlags;
        parentState = parent->dirStateBits;
        parentDepth = estimateParentDepth(record.parentFrn);
        parentPrio = parent->folderPriority();
    }

    int32_t outPrio = basePrio;
    uint8_t outTier = kPathTierPrimaryUser;
    if (record.isDirectory()) {
        uint16_t outDepth = 0;
        stepDirectoryState(
            nameView,
            m_driveLetter,
            parentClass,
            parentState,
            parentDepth,
            record.dirClassFlags,
            record.dirStateBits,
            outDepth,
            outPrio,
            outTier);
    } else {
        record.dirClassFlags = parentClass;
        record.dirStateBits = parentState;
        computeLeafTierAndPriority(
            nameView,
            false,
            record.parentFrn,
            parentClass,
            parentState,
            parentPrio,
            outTier,
            outPrio);
    }
    record.setFolderPriority(outPrio);
    record.setPathTier(outTier);
    record.addFlags(StoredFileRecord::FlagTierComputed);
}

void FileIndexStore::propagateAncestorsSizeChange(
    uint64_t startParentFrn,
    uint64_t oldSize,
    uint64_t newSize
) noexcept {
    if (oldSize == newSize || startParentFrn == 0) {
        return;
    }
    const bool isIncrease = (newSize > oldSize);
    const uint64_t delta = isIncrease ? (newSize - oldSize) : (oldSize - newSize);

    constexpr size_t kMaxChainDepth = 512;
    uint32_t visited[kMaxChainDepth];
    size_t visitedLen = 0;

    uint64_t curFrn = startParentFrn;
    while (curFrn != 0 && visitedLen < kMaxChainDepth) {
        const size_t probe = findProbeIndex(curFrn);
        if (probe == kNoSlotIndex) break;
        const uint32_t recIdx = m_slots[probe];
        bool cycle = false;
        for (size_t k = 0; k < visitedLen; ++k) {
            if (visited[k] == recIdx) {
                cycle = true;
                break;
            }
        }
        if (cycle) break;
        visited[visitedLen++] = recIdx;

        auto& dirRec = m_records[recIdx];
        if (dirRec.isDeleted() || !dirRec.isDirectory()) break;

        const uint64_t curDirSize = fileSizeOf(dirRec);
        const uint64_t nextDirSize = isIncrease
            ? ((UINT64_MAX - curDirSize < delta) ? UINT64_MAX : (curDirSize + delta))
            : ((curDirSize > delta) ? (curDirSize - delta) : 0ULL);
        setFileSize(dirRec, nextDirSize);

        if (dirRec.parentFrn == 0 || dirRec.parentFrn == dirRec.frn) break;
        curFrn = dirRec.parentFrn;
    }
}

void FileIndexStore::upsert(const FileRecordInit& init, bool metadataHydrated, bool bulkImport) {
    if (init.fileReferenceNumber == EmptyKey) return;
    if (m_slots.empty()) rehash(1024);

    const uint16_t nameLen = static_cast<uint16_t>(
        (std::min<size_t>)(init.fileName.size(), StoredFileRecord::kNameLenMask));
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
        if (existing.nameLength() == nameLen &&
            m_arena.equalsName(existing.nameOffset, existing.nameLength(), existing.isAsciiName(), nameView)) {
            const uint64_t oldParentFrn = existing.parentFrn;
            const uint64_t oldSize = fileSizeOf(existing);
            const bool parentChanged = (oldParentFrn != init.parentFileReferenceNumber);
            const bool dirFlagChanged = (existing.isDirectory() != init.isDirectory);
            const uint8_t preservedHydrated =
                static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagMetadataHydrated);
            const uint8_t preservedAscii =
                static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagAsciiName);
            const uint8_t preservedCreation =
                static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagHasCreationTime);
            const uint8_t preservedTier =
                (!parentChanged && !dirFlagChanged)
                    ? static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagTierComputed)
                    : 0u;

            existing.parentFrn = init.parentFileReferenceNumber;
            const uint64_t nextSize = (init.isDirectory && !dirFlagChanged && init.fileSize == 0)
                ? oldSize
                : init.fileSize;
            setFileSize(existing, nextSize);
            existing.lastWriteSec = fileTimeToUnixSec(init.lastWriteTime);
            existing.attributes16 = static_cast<uint16_t>(init.fileAttributes & 0xFFFFu);
            existing.setFlags(static_cast<uint8_t>(
                newFlags | preservedHydrated | preservedAscii | preservedCreation | preservedTier));
            setCreationTime(existing, init.creationTime);

            if (!init.isDirectory && !dirFlagChanged) {
                if (parentChanged) {
                    propagateAncestorsSizeChange(oldParentFrn, oldSize, 0);
                    propagateAncestorsSizeChange(existing.parentFrn, 0, nextSize);
                } else if (oldSize != nextSize) {
                    propagateAncestorsSizeChange(existing.parentFrn, oldSize, nextSize);
                }
            }

            if (parentChanged || dirFlagChanged || !existing.isTierComputed()) {
                applyIncrementalTier(existing, nameView);
                if (existing.isDirectory() && (parentChanged || dirFlagChanged)) {
                    recomputeHierarchyMetadata();
                } else if (dirFlagChanged) {
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
        PinyinEngine::BuildNormalizedInitials(
            nameView,
            m_pinyinAsciiInitialsScratch,
            m_pinyinWideInitialsScratch
        );
        newPinyin.initialsLength = static_cast<uint16_t>(
            (std::min<size_t>)(m_pinyinWideInitialsScratch.size(), 0xFFFF));
        const std::wstring_view pyView =
            std::wstring_view{m_pinyinWideInitialsScratch}.substr(0, newPinyin.initialsLength);
        if (StringArena::isAsciiOnly(pyView)) {
            newPinyin.isAscii = 1;
            newPinyin.initialsOffset = m_arena.internAsciiFromWide(pyView);
        } else {
            newPinyin.isAscii = 0;
            newPinyin.initialsOffset = m_arena.intern(pyView);
        }
    }

    bool nameIsAscii = true;
    const uint32_t internedNameOffset = internName(nameView, nameIsAscii);
    if (nameIsAscii) {
        newFlags |= StoredFileRecord::FlagAsciiName;
    }

    if (hasExistingLive) {
        auto& existing = m_records[existingRecSlot];
        const bool wasDirectory = existing.isDirectory();
        const uint64_t oldParentFrn = existing.parentFrn;
        const uint64_t oldSize = fileSizeOf(existing);
        const bool parentChanged = (oldParentFrn != init.parentFileReferenceNumber);
        const bool dirFlagChanged = (wasDirectory != init.isDirectory);
        const uint8_t preservedHydrated =
            static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagMetadataHydrated);
        const uint8_t preservedCreation =
            static_cast<uint8_t>(existing.flags() & StoredFileRecord::FlagHasCreationTime);

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
        const uint64_t nextSize = (init.isDirectory && !dirFlagChanged && init.fileSize == 0)
            ? oldSize
            : init.fileSize;
        setFileSize(existing, nextSize);
        existing.lastWriteSec = fileTimeToUnixSec(init.lastWriteTime);
        existing.attributes16 = static_cast<uint16_t>(init.fileAttributes & 0xFFFFu);
        existing.setNameLength(nameLen);
        existing.nameOffset = internedNameOffset;
        existing.pinyinSlot = updatedPinyinSlot;
        existing.setFlags(static_cast<uint8_t>(newFlags | preservedHydrated | preservedCreation));
        setCreationTime(existing, init.creationTime);

        if (!init.isDirectory && !dirFlagChanged) {
            if (parentChanged) {
                propagateAncestorsSizeChange(oldParentFrn, oldSize, 0);
                propagateAncestorsSizeChange(existing.parentFrn, 0, nextSize);
            } else if (oldSize != nextSize) {
                propagateAncestorsSizeChange(existing.parentFrn, oldSize, nextSize);
            }
        }

        applyIncrementalTier(existing, nameView);
        if (wasDirectory || existing.isDirectory()) {
            recomputeHierarchyMetadata();
        }
        return;
    }

    StoredFileRecord record{};
    record.frn = init.fileReferenceNumber;
    record.parentFrn = init.parentFileReferenceNumber;
    record.lastWriteSec = fileTimeToUnixSec(init.lastWriteTime);
    record.attributes16 = static_cast<uint16_t>(init.fileAttributes & 0xFFFFu);
    record.setNameLength(nameLen);
    record.setFlags(newFlags);
    record.nameOffset = internedNameOffset;
    setFileSize(record, init.fileSize);
    setCreationTime(record, init.creationTime);
    if (hasChinese) {
        record.pinyinSlot = allocatePinyinSlot(newPinyin);
    }
    if (!bulkImport) {
        applyIncrementalTier(record, nameView);
    }

    m_records.push_back(record);
    insertNewSlot(init.fileReferenceNumber, static_cast<uint32_t>(m_records.size() - 1));
    ++m_liveCount;

    if (!bulkImport && !record.isDirectory() && init.fileSize > 0) {
        propagateAncestorsSizeChange(record.parentFrn, 0, init.fileSize);
    }
}

bool FileIndexStore::updateMetadata(uint64_t frn, uint64_t fileSize,
                                    uint64_t creationTime, uint64_t lastWriteTime) noexcept {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return false;

    auto& record = m_records[m_slots[probe]];
    if (record.isDeleted()) return false;

    if (!record.isDirectory()) {
        const uint64_t oldSize = fileSizeOf(record);
        if (oldSize != fileSize) {
            propagateAncestorsSizeChange(record.parentFrn, oldSize, fileSize);
            setFileSize(record, fileSize);
        }
    }
    if (creationTime != 0) setCreationTime(record, creationTime);
    if (lastWriteTime != 0) record.lastWriteSec = fileTimeToUnixSec(lastWriteTime);
    record.addFlags(StoredFileRecord::FlagMetadataHydrated);
    return true;
}

bool FileIndexStore::markMetadataDirty(uint64_t frn) noexcept {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return false;

    auto& record = m_records[m_slots[probe]];
    if (record.isDeleted() || record.isDirectory()) return false;

    record.clearFlags(StoredFileRecord::FlagMetadataHydrated);
    return true;
}

bool FileIndexStore::erase(uint64_t frn) {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return false;

    auto& record = m_records[m_slots[probe]];
    if (record.isDeleted()) return false;

    const uint64_t oldSize = fileSizeOf(record);
    if (oldSize > 0) {
        propagateAncestorsSizeChange(record.parentFrn, oldSize, 0);
    }
    if (record.fileSize32 == StoredFileRecord::kLargeSizeSentinel) {
        m_largeSizes.erase(record.frn);
        record.fileSize32 = 0;
    }
    if (record.hasCreationTime()) {
        m_creationTimes.erase(record.frn);
        record.clearFlags(StoredFileRecord::FlagHasCreationTime);
    }
    if (record.pinyinSlot != 0) {
        releasePinyinSlot(record.pinyinSlot);
    }
    record.addFlags(StoredFileRecord::FlagDeleted);
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

uint32_t FileIndexStore::findSlotIndex(uint64_t frn) const noexcept {
    const size_t probe = findProbeIndex(frn);
    if (probe == kNoSlotIndex) return kEmptySlot;
    const uint32_t recIdx = m_slots[probe];
    return m_records[recIdx].isDeleted() ? kEmptySlot : recIdx;
}

void FileIndexStore::recomputeHierarchyMetadata() {
    if (m_records.empty()) return;

    const int32_t basePrio = driveBasePriority(m_driveLetter);
    for (auto& rec : m_records) {
        if (!rec.isDeleted()) {
            rec.clearFlags(StoredFileRecord::FlagTierComputed);
            if (rec.isDirectory()) {
                setFileSize(rec, 0);
            }
        }
    }

    // Transient per-slot depth table (freed immediately on return; 0 bytes resident RAM).
    std::vector<uint16_t> treeDepths(m_records.size(), 0);
    FileViewScratch scratch;

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
                curDepth = treeDepths[curIdx];
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
            const uint32_t dirIdx = chain[step - 1];
            auto& dirRec = m_records[dirIdx];
            const std::wstring_view dirName = m_arena.viewWidened(
                dirRec.nameOffset, dirRec.nameLength(), dirRec.isAsciiName(), scratch.nameBuf);
            int32_t dirPrio = basePrio;
            uint8_t dirTier = kPathTierPrimaryUser;
            uint16_t outDepth = 0;
            stepDirectoryState(
                dirName,
                m_driveLetter,
                curClass,
                curState,
                curDepth,
                dirRec.dirClassFlags,
                dirRec.dirStateBits,
                outDepth,
                dirPrio,
                dirTier);
            treeDepths[dirIdx] = outDepth;
            dirRec.setFolderPriority(dirPrio);
            dirRec.setPathTier(dirTier);
            dirRec.addFlags(StoredFileRecord::FlagTierComputed);

            curClass = dirRec.dirClassFlags;
            curState = dirRec.dirStateBits;
            curDepth = outDepth;
        }
    }

    // Pass 2: O(1) evaluation of all live file records + direct-parent size accumulation.
    uint64_t cachedParentFrn = UINT64_MAX;
    uint32_t cachedParentSlot = kEmptySlot;
    uint8_t cachedParentClass = 0;
    uint8_t cachedParentState = 0;
    int32_t cachedParentPrio = basePrio;

    for (auto& rec : m_records) {
        if (rec.isDeleted() || rec.isDirectory()) continue;
        const std::wstring_view leafName = m_arena.viewWidened(
            rec.nameOffset, rec.nameLength(), rec.isAsciiName(), scratch.nameBuf);
        if (rec.parentFrn == 0 || rec.parentFrn == rec.frn) {
            rec.dirClassFlags = 0;
            rec.dirStateBits = 0;
            uint8_t leafTier = kPathTierPrimaryUser;
            int32_t leafPrio = basePrio;
            computeLeafTierAndPriority(
                leafName,
                false,
                0,
                0,
                0,
                basePrio,
                leafTier,
                leafPrio);
            rec.setPathTier(leafTier);
            rec.setFolderPriority(leafPrio);
            rec.addFlags(StoredFileRecord::FlagTierComputed);
            continue;
        }

        if (rec.parentFrn != cachedParentFrn) {
            cachedParentFrn = rec.parentFrn;
            const size_t parentProbe = findProbeIndex(rec.parentFrn);
            if (parentProbe != kNoSlotIndex) {
                const uint32_t pIdx = m_slots[parentProbe];
                const auto& parent = m_records[pIdx];
                if (!parent.isDeleted() && parent.isDirectory()) {
                    cachedParentSlot = pIdx;
                    cachedParentClass = parent.dirClassFlags;
                    cachedParentState = parent.dirStateBits;
                    cachedParentPrio = parent.folderPriority();
                } else {
                    cachedParentSlot = kEmptySlot;
                    cachedParentClass = 0;
                    cachedParentState = 0;
                    cachedParentPrio = basePrio;
                }
            } else {
                cachedParentSlot = kEmptySlot;
                cachedParentClass = 0;
                cachedParentState = 0;
                cachedParentPrio = basePrio;
            }
        }

        rec.dirClassFlags = cachedParentClass;
        rec.dirStateBits = cachedParentState;
        uint8_t leafTier = kPathTierPrimaryUser;
        int32_t leafPrio = basePrio;
        computeLeafTierAndPriority(
            leafName,
            false,
            rec.parentFrn,
            cachedParentClass,
            cachedParentState,
            cachedParentPrio,
            leafTier,
            leafPrio);
        rec.setPathTier(leafTier);
        rec.setFolderPriority(leafPrio);
        rec.addFlags(StoredFileRecord::FlagTierComputed);

        const uint64_t recSize = fileSizeOf(rec);
        if (recSize > 0 && cachedParentSlot != kEmptySlot) {
            auto& directParent = m_records[cachedParentSlot];
            const uint64_t parentSize = fileSizeOf(directParent);
            const uint64_t nextParentSize = (UINT64_MAX - parentSize < recSize)
                ? UINT64_MAX
                : (parentSize + recSize);
            setFileSize(directParent, nextParentSize);
        }
    }

    // Pass 3: Single bottom-up pass over directories (ordered deepest -> shallowest) so directory
    // size roll-up runs in O(numDirectories) instead of O(numFiles * treeDepth) random hash walks.
    uint16_t maxDirDepth = 0;
    size_t dirCount = 0;
    for (size_t i = 0; i < m_records.size(); ++i) {
        const auto& rec = m_records[i];
        if (!rec.isDeleted() && rec.isDirectory()) {
            ++dirCount;
            if (treeDepths[i] > maxDirDepth) maxDirDepth = treeDepths[i];
        }
    }
    if (dirCount > 0) {
        const size_t bucketCount = static_cast<size_t>((std::min<uint16_t>)(maxDirDepth, kMaxChainDepth)) + 1;
        std::vector<size_t> counts(bucketCount, 0);
        for (size_t i = 0; i < m_records.size(); ++i) {
            const auto& rec = m_records[i];
            if (!rec.isDeleted() && rec.isDirectory()) {
                const size_t d = (std::min<size_t>)(treeDepths[i], bucketCount - 1);
                ++counts[d];
            }
        }
        std::vector<size_t> offsets(bucketCount + 1, 0);
        for (size_t d = 0; d < bucketCount; ++d) {
            offsets[d + 1] = offsets[d] + counts[d];
        }
        std::vector<uint32_t> dirsByDepth(dirCount);
        std::vector<size_t> writePos = offsets;
        for (size_t i = 0; i < m_records.size(); ++i) {
            const auto& rec = m_records[i];
            if (!rec.isDeleted() && rec.isDirectory()) {
                const size_t d = (std::min<size_t>)(treeDepths[i], bucketCount - 1);
                dirsByDepth[writePos[d]++] = static_cast<uint32_t>(i);
            }
        }
        for (size_t d = bucketCount; d > 0; --d) {
            for (size_t pos = offsets[d - 1]; pos < offsets[d]; ++pos) {
                const uint32_t childIdx = dirsByDepth[pos];
                const auto& childDir = m_records[childIdx];
                const uint64_t childSize = fileSizeOf(childDir);
                if (childSize == 0 || childDir.parentFrn == 0 || childDir.parentFrn == childDir.frn) {
                    continue;
                }
                const size_t pProbe = findProbeIndex(childDir.parentFrn);
                if (pProbe == kNoSlotIndex) continue;
                const uint32_t parentIdx = m_slots[pProbe];
                auto& parentDir = m_records[parentIdx];
                if (parentDir.isDeleted() || !parentDir.isDirectory() || treeDepths[parentIdx] >= treeDepths[childIdx]) {
                    continue;
                }
                const uint64_t parentSize = fileSizeOf(parentDir);
                const uint64_t nextParentSize = (UINT64_MAX - parentSize < childSize)
                    ? UINT64_MAX
                    : (parentSize + childSize);
                setFileSize(parentDir, nextParentSize);
            }
        }
    }
}

FileRecord FileIndexStore::view(const StoredFileRecord& record, FileViewScratch& scratch) const {
    FileRecord out;
    out.fileReferenceNumber = record.frn;
    out.parentFileReferenceNumber = record.parentFrn;
    out.fileName = m_arena.viewWidened(
        record.nameOffset, record.nameLength(), record.isAsciiName(), scratch.nameBuf);
    out.normalizedName = out.fileName;
    out.isDirectory = record.isDirectory();
    out.fileAttributes = record.attributes16;
    out.fileSize = fileSizeOf(record);
    out.creationTime = creationTimeOf(record);
    out.lastWriteTime = lastWriteTimeOf(record);

    if (record.pinyinSlot == 0) {
        out.pinyinInitials = out.fileName;
        out.pinyinFull = out.fileName;
    } else {
        const auto& entry = m_pinyin[record.pinyinSlot];
        out.pinyinInitials = m_arena.viewWidened(
            entry.initialsOffset, entry.initialsLength, entry.isAscii != 0, scratch.pinyinBuf);
        out.pinyinFull = {};
    }
    return out;
}

FileRecord FileIndexStore::view(const StoredFileRecord& record) const {
    thread_local FileViewScratch tlsScratch;
    return view(record, tlsScratch);
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

    if (m_records.empty()) {
        m_largeSizes.clear();
        m_creationTimes.clear();
    }

    size_t capacity = 1024;
    const size_t minSlots = minSlotsForLive(m_records.size());
    while (capacity < minSlots) capacity <<= 1;
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

void FileIndexStore::shrinkExcessCapacity() {
    compactIfSparse();
    releaseBuildScratch();

    m_records.shrink_to_fit();
    m_pinyin.shrink_to_fit();
    m_freePinyinSlots.shrink_to_fit();

    if (m_liveCount == 0) {
        m_slots.clear();
        m_slots.shrink_to_fit();
        m_largeSizes.clear();
        m_creationTimes.clear();
        m_tombstones = 0;
        return;
    }

    size_t targetCap = 1024;
    const size_t minSlots = minSlotsForLive(m_liveCount);
    while (targetCap < minSlots) {
        targetCap <<= 1;
    }

    if (m_slots.size() > targetCap * 2) {
        std::vector<uint32_t> newSlots(targetCap, kEmptySlot);
        const size_t mask = targetCap - 1;
        for (size_t i = 0; i < m_records.size(); ++i) {
            const auto& rec = m_records[i];
            if (rec.isDeleted() || rec.frn == EmptyKey) continue;
            size_t probe = static_cast<size_t>(mixKey(rec.frn)) & mask;
            while (newSlots[probe] != kEmptySlot) {
                probe = (probe + 1) & mask;
            }
            newSlots[probe] = static_cast<uint32_t>(i);
        }
        m_slots = std::move(newSlots);
        m_slots.shrink_to_fit();
        m_tombstones = 0;
    } else if (m_slots.capacity() > m_slots.size()) {
        m_slots.shrink_to_fit();
    }
}

uint64_t FileIndexStore::approximateBytes() const noexcept {
    return m_arena.approximateBytes() +
           static_cast<uint64_t>(m_records.capacity()) * sizeof(StoredFileRecord) +
           static_cast<uint64_t>(m_pinyin.capacity()) * sizeof(PinyinEntry) +
           static_cast<uint64_t>(m_freePinyinSlots.capacity()) * sizeof(uint32_t) +
           static_cast<uint64_t>(m_slots.capacity()) * sizeof(uint32_t) +
           static_cast<uint64_t>(m_largeSizes.size()) * 48ULL +
           static_cast<uint64_t>(m_creationTimes.size()) * 40ULL;
}

}  // namespace tools3000::service
