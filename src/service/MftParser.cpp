#include "MftParser.h"
#include "PinyinEngine.h"
#include "QueryThreadPool.h"
#include "SensitivePathGuard.h"
#include "content/ContentSearchEngine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <cwctype>
#include <unordered_set>

#define BUF_LEN 65536


namespace {

uint64_t fileTimeToUint64(const FILETIME& value) noexcept {
    ULARGE_INTEGER raw{};
    raw.LowPart = value.dwLowDateTime;
    raw.HighPart = value.dwHighDateTime;
    return raw.QuadPart;
}

struct ScopedBackgroundIoPriority {
    bool active = false;
    ScopedBackgroundIoPriority() noexcept {
        // Windows 原生线程后台模式：同时降低 CPU 优先级、磁盘 I/O 优先级（IoPriorityVeryLow）
        // 与内存页优先级，确保机械硬盘（HDD）上后台扫描/水合绝不抢占前台与 WebView2 磁盘读写。
        active = (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE);
        if (!active) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
    }
    ~ScopedBackgroundIoPriority() noexcept {
        if (active) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
    }
    ScopedBackgroundIoPriority(const ScopedBackgroundIoPriority&) = delete;
    ScopedBackgroundIoPriority& operator=(const ScopedBackgroundIoPriority&) = delete;
};

struct VolumeStorageTopology {
    bool isRotational = false;
    bool hasDiskNumber = false;
    uint32_t diskNumber = 0;
};

VolumeStorageTopology detectVolumeStorageTopology(char driveLetter) noexcept {
    VolumeStorageTopology topo{};
    std::wstring volPath = L"\\\\.\\";
    volPath += static_cast<wchar_t>(driveLetter);
    volPath += L":";

    // 使用 0 访问权限（仅查询元数据）即可读取驱动器拓扑与寻道惩罚属性，非管理员也能工作
    HANDLE hVol = CreateFileW(
        volPath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return topo;
    }

    DWORD bytesReturned = 0;
    STORAGE_DEVICE_NUMBER devNum{};
    if (DeviceIoControl(
            hVol,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0,
            &devNum,
            sizeof(devNum),
            &bytesReturned,
            nullptr)) {
        topo.hasDiskNumber = true;
        topo.diskNumber = static_cast<uint32_t>(devNum.DeviceNumber);
    }

    auto querySeekPenalty = [](HANDLE hTarget, bool& outRotational) noexcept -> bool {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
        DWORD returned = 0;
        if (DeviceIoControl(
                hTarget,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                &desc,
                sizeof(desc),
                &returned,
                nullptr) &&
            returned >= sizeof(desc)) {
            outRotational = (desc.IncursSeekPenalty != FALSE);
            return true;
        }
        return false;
    };

    // 二级兜底：部分老式 SATA/IDE/RAID 或 Win10 LTSC 驱动未实现 SeekPenalty 属性，
    // 补充探测 TRIM 支持（机械硬盘不支持 TRIM -> TrimEnabled == FALSE）。
    auto queryTrimFallback = [](HANDLE hTarget, bool& outRotational) noexcept -> bool {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceTrimProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_TRIM_DESCRIPTOR desc{};
        DWORD returned = 0;
        if (DeviceIoControl(
                hTarget,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                &desc,
                sizeof(desc),
                &returned,
                nullptr) &&
            returned >= sizeof(desc)) {
            outRotational = (desc.TrimEnabled == FALSE);
            return true;
        }
        return false;
    };

    bool detected = querySeekPenalty(hVol, topo.isRotational);
    if (!detected) {
        detected = queryTrimFallback(hVol, topo.isRotational);
    }
    CloseHandle(hVol);

    if (!detected && topo.hasDiskNumber) {
        const std::wstring physPath = L"\\\\.\\PhysicalDrive" + std::to_wstring(topo.diskNumber);
        HANDLE hDisk = CreateFileW(
            physPath.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (hDisk != INVALID_HANDLE_VALUE) {
            if (!querySeekPenalty(hDisk, topo.isRotational)) {
                (void)queryTrimFallback(hDisk, topo.isRotational);
            }
            CloseHandle(hDisk);
        }
    }

    return topo;
}

struct ScopedScanPhaseGuard {
    std::atomic<uint8_t>& phase;
    ScopedScanPhaseGuard(std::atomic<uint8_t>& target, MftParser::ScanPhase initial) noexcept
        : phase(target) {
        phase.store(static_cast<uint8_t>(initial), std::memory_order_release);
    }
    ~ScopedScanPhaseGuard() noexcept {
        phase.store(static_cast<uint8_t>(MftParser::ScanPhase::Idle), std::memory_order_release);
    }
    ScopedScanPhaseGuard(const ScopedScanPhaseGuard&) = delete;
    ScopedScanPhaseGuard& operator=(const ScopedScanPhaseGuard&) = delete;
};

}  // namespace

namespace tools3000::service {

bool parseUsnRecord(const BYTE* pBytes, DWORD remainingBytes, ParsedUsnRecord& out) noexcept {
    if (!pBytes || remainingBytes < sizeof(USN_RECORD_COMMON_HEADER)) {
        return false;
    }
    const auto* header = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(pBytes);
    if (header->RecordLength < sizeof(USN_RECORD_COMMON_HEADER) || header->RecordLength > remainingBytes) {
        return false;
    }
    out.recordLength = header->RecordLength;
    out.majorVersion = header->MajorVersion;
    out.isValid = false;
    out.fileName = {};

    if (header->MajorVersion == 2) {
        if (header->RecordLength < sizeof(USN_RECORD_V2)) {
            return true;
        }
        const auto* v2 = reinterpret_cast<const USN_RECORD_V2*>(pBytes);
        out.fileReferenceNumber = v2->FileReferenceNumber;
        out.parentFileReferenceNumber = v2->ParentFileReferenceNumber;
        out.reason = v2->Reason;
        out.fileAttributes = v2->FileAttributes;
        out.timeStamp = static_cast<uint64_t>(v2->TimeStamp.QuadPart);
        if (v2->FileNameLength > 0 &&
            static_cast<DWORD>(v2->FileNameOffset) + v2->FileNameLength <= header->RecordLength) {
            out.fileName = std::wstring_view(
                reinterpret_cast<const wchar_t*>(pBytes + v2->FileNameOffset),
                v2->FileNameLength / sizeof(wchar_t));
        }
        out.isValid = true;
        return true;
    }

    if (header->MajorVersion == 3) {
        if (header->RecordLength < sizeof(USN_RECORD_V3)) {
            return true;
        }
        const auto* v3 = reinterpret_cast<const USN_RECORD_V3*>(pBytes);
        std::memcpy(&out.fileReferenceNumber, v3->FileReferenceNumber.Identifier, sizeof(uint64_t));
        std::memcpy(&out.parentFileReferenceNumber, v3->ParentFileReferenceNumber.Identifier, sizeof(uint64_t));
        out.reason = v3->Reason;
        out.fileAttributes = v3->FileAttributes;
        out.timeStamp = static_cast<uint64_t>(v3->TimeStamp.QuadPart);
        if (v3->FileNameLength > 0 &&
            static_cast<DWORD>(v3->FileNameOffset) + v3->FileNameLength <= header->RecordLength) {
            out.fileName = std::wstring_view(
                reinterpret_cast<const wchar_t*>(pBytes + v3->FileNameOffset),
                v3->FileNameLength / sizeof(wchar_t));
        }
        out.isValid = true;
        return true;
    }

    return true;
}

QueryThreadPool& QueryThreadPool::instance() {
    static QueryThreadPool pool;
    return pool;
}

QueryThreadPool::QueryThreadPool() {
    const unsigned int hw = std::thread::hardware_concurrency();
    m_workerCount = (hw <= 2)
        ? static_cast<size_t>((std::max)(1u, hw))
        : static_cast<size_t>((std::min)(hw, 16u));
}

QueryThreadPool::~QueryThreadPool() {
    shutdown();
}

void QueryThreadPool::ensureStarted() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_started || m_stopping) {
        return;
    }
    m_started = true;
    m_workers.reserve(m_workerCount);
    for (size_t i = 0; i < m_workerCount; ++i) {
        m_workers.emplace_back(&QueryThreadPool::workerLoop, this);
    }
}

void QueryThreadPool::shutdown() noexcept {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_started && m_workers.empty()) {
            return;
        }
        m_stopping = true;
        toJoin.swap(m_workers);
        m_started = false;
    }
    m_workCv.notify_all();
    for (auto& w : toJoin) {
        if (w.joinable() && w.get_id() != std::this_thread::get_id()) {
            w.join();
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stopping = false;
    }
}

namespace {

thread_local int t_queryPoolTaskDepth = 0;

struct ScopedQueryPoolDepth {
    ScopedQueryPoolDepth() noexcept { ++t_queryPoolTaskDepth; }
    ~ScopedQueryPoolDepth() noexcept { --t_queryPoolTaskDepth; }
    ScopedQueryPoolDepth(const ScopedQueryPoolDepth&) = delete;
    ScopedQueryPoolDepth& operator=(const ScopedQueryPoolDepth&) = delete;
};

}  // namespace

void QueryThreadPool::finishOneItem(const std::shared_ptr<JobBatch>& batch) noexcept {
    if (batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard<std::mutex> doneLk(batch->doneMutex);
        batch->doneCv.notify_all();
    }
}

bool QueryThreadPool::stealOneTask() {
    std::shared_ptr<JobBatch> picked;
    size_t idx = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        while (!m_batches.empty()) {
            auto& front = m_batches.front();
            const size_t cand = front->nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (cand < front->count) {
                picked = front;
                idx = cand;
                if (cand + 1 >= front->count) {
                    m_batches.pop_front();
                }
                break;
            }
            m_batches.pop_front();
        }
    }
    if (!picked) {
        return false;
    }
    {
        ScopedQueryPoolDepth depthGuard;
        try {
            picked->fn(idx);
        } catch (...) {
        }
    }
    finishOneItem(picked);
    return true;
}

void QueryThreadPool::workerLoop() {
    while (true) {
        if (stealOneTask()) {
            continue;
        }
        std::unique_lock<std::mutex> lk(m_mutex);
        m_workCv.wait(lk, [this]() {
            return m_stopping || !m_batches.empty();
        });
        if (m_stopping && m_batches.empty()) {
            return;
        }
    }
}

void QueryThreadPool::parallelFor(size_t taskCount, const std::function<void(size_t)>& fn) {
    if (taskCount == 0 || !fn) {
        return;
    }
    // 嵌套在线程池任务内调用时严禁再次入队等待或跨批次窃取（否则持 shared_lock 线程
    // 窃取到外部请求的 unique_lock 任务会触发同线程 SRWLock 自死锁），直接同步完成。
    if (taskCount == 1 || m_workerCount <= 1 || t_queryPoolTaskDepth > 0) {
        ScopedQueryPoolDepth depthGuard;
        for (size_t i = 0; i < taskCount; ++i) {
            fn(i);
        }
        return;
    }

    ensureStarted();

    auto batch = std::make_shared<JobBatch>();
    batch->count = taskCount;
    batch->remaining.store(taskCount, std::memory_order_relaxed);
    batch->fn = fn;

    {
        std::unique_lock<std::mutex> lk(m_mutex);
        if (m_stopping || m_workers.empty()) {
            lk.unlock();
            ScopedQueryPoolDepth depthGuard;
            for (size_t i = 0; i < taskCount; ++i) {
                fn(i);
            }
            return;
        }
        m_batches.push_back(batch);
    }

    const size_t wakeCount = (std::min)(taskCount - 1, m_workerCount);
    if (wakeCount >= m_workerCount) {
        m_workCv.notify_all();
    } else {
        for (size_t i = 0; i < wakeCount; ++i) {
            m_workCv.notify_one();
        }
    }

    // 调用方只协作消费当前 batch 自身的分片，绝不跨批次窃取其他请求的任务，
    // 从根本上阻断跨查询锁层次反转与同线程 SRWLock 共享→独占自死锁。
    {
        ScopedQueryPoolDepth depthGuard;
        while (true) {
            const size_t myIdx = batch->nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (myIdx >= taskCount) {
                break;
            }
            try {
                batch->fn(myIdx);
            } catch (...) {
            }
            finishOneItem(batch);
        }
    }

    if (batch->remaining.load(std::memory_order_acquire) > 0) {
        std::unique_lock<std::mutex> doneLk(batch->doneMutex);
        batch->doneCv.wait(doneLk, [&batch]() {
            return batch->remaining.load(std::memory_order_acquire) == 0;
        });
    }
}

}  // namespace tools3000::service

MftParser::MftParser() : m_DriveLetter('C'), m_hVolume(INVALID_HANDLE_VALUE) {
    m_Store.setDriveLetter('C');
}

MftParser::~MftParser() {
    StopListening();
    if (m_hVolume != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hVolume);
    }
}

size_t MftParser::getFileCount() const {
    return m_FileCount.load(std::memory_order_acquire);
}

uint64_t MftParser::getApproximateIndexBytes() const {
    std::shared_lock lock(m_MapMutex);
    return m_Store.approximateBytes();
}

void MftParser::shrinkIndexCapacity() {
    std::unique_lock lock(m_MapMutex);
    m_Store.shrinkExcessCapacity();
    m_FileCount.store(m_Store.size(), std::memory_order_release);
}

bool MftParser::queryFrnDiskMetadata(
    uint64_t frn,
    const std::wstring& fallbackPath,
    uint64_t& outFileSize,
    uint64_t& outCreationTime,
    uint64_t& outLastWriteTime
) const noexcept {
    if (m_hVolume != INVALID_HANDLE_VALUE && !m_IsFallbackDirectoryWalk && frn != 0) {
        FILE_ID_DESCRIPTOR desc{};
        desc.dwSize = sizeof(desc);
        desc.Type = FileIdType;
        desc.FileId.QuadPart = static_cast<LONGLONG>(frn);
        HANDLE hFile = OpenFileById(
            m_hVolume,
            &desc,
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT
        );
        if (hFile != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info{};
            const BOOL ok = GetFileInformationByHandle(hFile, &info);
            CloseHandle(hFile);
            if (ok) {
                const bool isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                outFileSize = isDir
                    ? 0ULL
                    : ((static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                       static_cast<uint64_t>(info.nFileSizeLow));
                outCreationTime = fileTimeToUint64(info.ftCreationTime);
                outLastWriteTime = fileTimeToUint64(info.ftLastWriteTime);
                return true;
            }
        }
    }
    if (!fallbackPath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExW(fallbackPath.c_str(), GetFileExInfoStandard, &attr)) {
            const bool isDir = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            outFileSize = isDir
                ? 0ULL
                : ((static_cast<uint64_t>(attr.nFileSizeHigh) << 32) |
                   static_cast<uint64_t>(attr.nFileSizeLow));
            outCreationTime = fileTimeToUint64(attr.ftCreationTime);
            outLastWriteTime = fileTimeToUint64(attr.ftLastWriteTime);
            return true;
        }
    }
    return false;
}

void MftParser::UsnListenerLoop() {
    constexpr DWORD kDataModifyReasons =
        USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION;

    READ_USN_JOURNAL_DATA_V0 rujd = {0};
    {
        std::lock_guard<std::mutex> usnLock(m_UsnMutex);
        const uint64_t initialUsn = m_CurrentUsn.load(std::memory_order_acquire);
        rujd.StartUsn = (initialUsn != 0) ? static_cast<USN>(initialUsn) : m_UsnJournalData.NextUsn;
        rujd.UsnJournalID = m_UsnJournalData.UsnJournalID;
    }
    rujd.ReasonMask = 0xFFFFFFFF; // All reasons
    rujd.ReturnOnlyOnClose = FALSE;
    rujd.Timeout = 0;
    rujd.BytesToWaitFor = 0;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;

    while (m_IsListening.load(std::memory_order_relaxed)) {
        if (!DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &rujd, sizeof(rujd), buffer.data(), BUF_LEN, &bytesReturned, NULL)) {
            const DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED || err == ERROR_JOURNAL_NOT_ACTIVE) {
                if (QueryUsnJournal()) {
                    {
                        std::lock_guard<std::mutex> usnLock(m_UsnMutex);
                        rujd.UsnJournalID = m_UsnJournalData.UsnJournalID;
                        rujd.StartUsn = m_UsnJournalData.NextUsn;
                    }
                    m_CurrentUsn.store(static_cast<uint64_t>(rujd.StartUsn), std::memory_order_release);
                    EnumerateFiles();
                }
            }
            Sleep(100);
            continue;
        }

        if (bytesReturned >= sizeof(USN)) {
            rujd.StartUsn = *reinterpret_cast<const USN*>(buffer.data());
            m_CurrentUsn.store(static_cast<uint64_t>(rujd.StartUsn), std::memory_order_release);
            std::lock_guard<std::mutex> usnLock(m_UsnMutex);
            m_UsnJournalData.NextUsn = rujd.StartUsn;
        }

        if (bytesReturned <= sizeof(USN)) {
            Sleep(50);
            continue;
        }

        DWORD dwRetBytes = bytesReturned - sizeof(USN);
        const BYTE* pRecordBytes = buffer.data() + sizeof(USN);

        if (dwRetBytes > 0) {
            std::vector<std::pair<uint64_t, std::wstring>> pendingStatRefresh;
            std::unordered_set<uint64_t> queuedFrns;
            std::unordered_set<uint64_t> deletedInBatch;
            const bool needPathFallback = (m_hVolume == INVALID_HANDLE_VALUE || m_IsFallbackDirectoryWalk);

            {
                std::unique_lock lock(m_MapMutex);
                bool changed = false;
                tools3000::service::ParsedUsnRecord parsed{};

                while (dwRetBytes > 0 && tools3000::service::parseUsnRecord(pRecordBytes, dwRetBytes, parsed)) {
                    if (parsed.isValid) {
                        const uint64_t frn = parsed.fileReferenceNumber;
                        if (parsed.reason & USN_REASON_FILE_DELETE) {
                            changed = m_Store.erase(frn) || changed;
                            deletedInBatch.insert(frn);
                        } else if ((parsed.reason & USN_REASON_FILE_CREATE) ||
                                   (parsed.reason & USN_REASON_RENAME_NEW_NAME)) {
                            deletedInBatch.erase(frn);
                            if (!parsed.fileName.empty()) {
                                const bool isDir = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                                FileRecordInit record;
                                record.fileReferenceNumber = frn;
                                record.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                                record.isDirectory = isDir;
                                record.fileAttributes = parsed.fileAttributes;
                                record.lastWriteTime = parsed.timeStamp;
                                record.fileName = std::wstring(parsed.fileName);

                                // `MED-15`: Never blindly zero out `fileSize` / `creationTime` on rename
                                // (`USN_REASON_RENAME_NEW_NAME`) or follow-up close events on an existing FRN.
                                const auto* existing = m_Store.find(frn);
                                if (existing != nullptr) {
                                    const FileRecord oldRec = m_Store.view(*existing);
                                    if (!isDir) {
                                        record.fileSize = oldRec.fileSize;
                                    }
                                    record.creationTime = oldRec.creationTime;
                                    if (record.lastWriteTime == 0) {
                                        record.lastWriteTime = oldRec.lastWriteTime;
                                    }
                                }

                                m_Store.upsert(record);
                                changed = true;

                                if (!isDir &&
                                    ((parsed.reason & USN_REASON_RENAME_NEW_NAME) != 0 ||
                                     (parsed.reason & USN_REASON_CLOSE) != 0 ||
                                     (parsed.reason & kDataModifyReasons) != 0 ||
                                     record.fileSize == 0) &&
                                    !tools3000::service::SensitivePathGuard::isSensitiveCredentialFileName(parsed.fileName) &&
                                    queuedFrns.insert(frn).second) {
                                    const std::wstring fullPath = buildFullPath(frn);
                                    if (!tools3000::service::SensitivePathGuard::isSensitiveCredentialDirectoryPath(fullPath)) {
                                        pendingStatRefresh.emplace_back(
                                            frn,
                                            needPathFallback ? fullPath : std::wstring{});
                                    }
                                }
                            }
                        } else if ((parsed.reason & USN_REASON_BASIC_INFO_CHANGE) ||
                                   ((parsed.reason & USN_REASON_CLOSE) && (parsed.reason & kDataModifyReasons)) ||
                                   (parsed.reason & kDataModifyReasons)) {
                            const auto* existing = m_Store.find(frn);
                            if (existing != nullptr) {
                                const FileRecord oldRec = m_Store.view(*existing);
                                FileRecordInit record;
                                record.fileReferenceNumber = frn;
                                record.parentFileReferenceNumber = oldRec.parentFileReferenceNumber;
                                record.isDirectory = oldRec.isDirectory;
                                record.fileAttributes = parsed.fileAttributes ? parsed.fileAttributes : oldRec.fileAttributes;
                                record.fileSize = oldRec.fileSize;
                                record.creationTime = oldRec.creationTime;
                                record.lastWriteTime = parsed.timeStamp ? parsed.timeStamp : oldRec.lastWriteTime;
                                record.fileName = std::wstring(oldRec.fileName);

                                m_Store.upsert(record);
                                changed = true;

                                if (!oldRec.isDirectory &&
                                    ((parsed.reason & USN_REASON_CLOSE) != 0 ||
                                     (parsed.reason & kDataModifyReasons) != 0) &&
                                    !tools3000::service::SensitivePathGuard::isSensitiveCredentialFileName(oldRec.fileName) &&
                                    queuedFrns.insert(frn).second) {
                                    const std::wstring fullPath = buildFullPath(frn);
                                    if (!tools3000::service::SensitivePathGuard::isSensitiveCredentialDirectoryPath(fullPath)) {
                                        pendingStatRefresh.emplace_back(
                                            frn,
                                            needPathFallback ? fullPath : std::wstring{});
                                    }
                                }
                            }
                        }
                    }

                    if (parsed.recordLength == 0 || parsed.recordLength > dwRetBytes) {
                        break;
                    }
                    dwRetBytes -= parsed.recordLength;
                    pRecordBytes += parsed.recordLength;
                }

                if (changed) {
                    m_Store.compactIfSparse();
                    m_FileCount.store(m_Store.size(), std::memory_order_release);
                    m_IndexGeneration.fetch_add(1, std::memory_order_release);
                    std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
                    m_CachedQuery.clear();
                    m_CachedCandidates.clear();
                    m_CachedExcludePatterns.clear();
                }
            }

            // Refresh `fileSize`, `creationTime`, and `lastWriteTime` OUTSIDE `m_MapMutex` (`MED-15`)
            // so `size:` and `dm:` queries immediately see updated metadata after writes/renames.
            if (!pendingStatRefresh.empty()) {
                struct RefreshedMeta {
                    uint64_t frn = 0;
                    uint64_t fileSize = 0;
                    uint64_t creationTime = 0;
                    uint64_t lastWriteTime = 0;
                };
                std::vector<RefreshedMeta> refreshed;
                refreshed.reserve(pendingStatRefresh.size());
                for (const auto& [frn, fallbackPath] : pendingStatRefresh) {
                    if (deletedInBatch.find(frn) != deletedInBatch.end()) continue;
                    uint64_t fSize = 0;
                    uint64_t cTime = 0;
                    uint64_t mTime = 0;
                    if (queryFrnDiskMetadata(frn, fallbackPath, fSize, cTime, mTime)) {
                        refreshed.push_back(RefreshedMeta{frn, fSize, cTime, mTime});
                    }
                }
                if (!refreshed.empty()) {
                    std::unique_lock statLock(m_MapMutex);
                    for (const auto& item : refreshed) {
                        m_Store.updateMetadata(item.frn, item.fileSize, item.creationTime, item.lastWriteTime);
                    }
                    m_IndexGeneration.fetch_add(1, std::memory_order_release);
                    std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
                    m_CachedQuery.clear();
                    m_CachedCandidates.clear();
                    m_CachedExcludePatterns.clear();
                }
            }
        }
        Sleep(50); // Prevent 100% CPU on fast changes
    }
}

void MftParser::BackgroundHydrateLoop() {
    ScopedBackgroundIoPriority bgIoGuard;

    auto shouldAbort = [this]() noexcept {
        return m_StopHydration.load(std::memory_order_acquire) ||
               m_StopRequested.load(std::memory_order_acquire) ||
               !m_IsListening.load(std::memory_order_acquire) ||
               m_hVolume == INVALID_HANDLE_VALUE;
    };
    if (shouldAbort()) {
        return;
    }

    std::vector<uint64_t> parentDirFrns;
    bool hydrateDriveRoot = false;
    {
        std::shared_lock readLock(m_MapMutex);
        std::unordered_set<uint64_t> seenParents;
        seenParents.reserve(8192);
        const size_t totalSlots = m_Store.slotCount();
        for (size_t i = 0; i < totalSlots; ++i) {
            if ((i & 0x3FFFu) == 0 && shouldAbort()) {
                return;
            }
            const auto* rec = m_Store.at(i);
            if (!rec || rec->isDirectory() || rec->isMetadataHydrated()) {
                continue;
            }
            if (rec->parentFrn == 0) {
                hydrateDriveRoot = true;
            } else if (seenParents.insert(rec->parentFrn).second) {
                const std::wstring dirPath = buildFullPath(rec->parentFrn);
                if (!tools3000::service::SensitivePathGuard::isSensitiveCredentialDirectoryPath(dirPath)) {
                    parentDirFrns.push_back(rec->parentFrn);
                }
            }
        }
    }

    if (parentDirFrns.empty() && !hydrateDriveRoot) {
        return;
    }

    struct HydratedBatchItem {
        uint64_t frn = 0;
        uint64_t fileSize = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
    };
    constexpr size_t kFlushBatchThreshold = 4096;
    std::vector<HydratedBatchItem> batch;
    batch.reserve(kFlushBatchThreshold + 256);
    std::vector<BYTE> dirBuffer(65536);

    auto flushHydratedBatch = [&]() {
        if (batch.empty() || shouldAbort()) {
            batch.clear();
            return;
        }
        {
            std::unique_lock writeLock(m_MapMutex);
            for (const auto& item : batch) {
                // 全盘后台水合不向内存侧表灌入数百万条 creationTime（搜索展示前排按需水合即可拿到精确创建时间）
                m_Store.updateMetadata(item.frn, item.fileSize, 0ULL, item.lastWriteTime);
            }
            m_IndexGeneration.fetch_add(1, std::memory_order_release);
        }
        batch.clear();
    };

    auto scanDirectoryHandle = [&](HANDLE hDir) {
        FILE_INFO_BY_HANDLE_CLASS infoClass = FileIdBothDirectoryRestartInfo;
        while (!shouldAbort() &&
               GetFileInformationByHandleEx(hDir, infoClass, dirBuffer.data(), static_cast<DWORD>(dirBuffer.size()))) {
            infoClass = FileIdBothDirectoryInfo;
            const BYTE* cursor = dirBuffer.data();
            while (cursor != nullptr) {
                const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(cursor);
                const bool isDir = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const uint64_t fileId = static_cast<uint64_t>(entry->FileId.QuadPart);
                if (!isDir && fileId != 0) {
                    batch.push_back(HydratedBatchItem{
                        fileId,
                        static_cast<uint64_t>(entry->EndOfFile.QuadPart),
                        static_cast<uint64_t>(entry->CreationTime.QuadPart),
                        static_cast<uint64_t>(entry->LastWriteTime.QuadPart),
                    });
                }
                if (entry->NextEntryOffset == 0) {
                    break;
                }
                cursor += entry->NextEntryOffset;
            }
            if (batch.size() >= kFlushBatchThreshold) {
                flushHydratedBatch();
            }
        }
    };

    if (hydrateDriveRoot && !shouldAbort()) {
        const std::wstring rootDir{static_cast<wchar_t>(m_DriveLetter), L':', L'\\'};
        HANDLE hRoot = CreateFileW(
            rootDir.c_str(),
            FILE_LIST_DIRECTORY | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        if (hRoot != INVALID_HANDLE_VALUE) {
            scanDirectoryHandle(hRoot);
            CloseHandle(hRoot);
        }
    }

    for (size_t idx = 0; idx < parentDirFrns.size(); ++idx) {
        if (shouldAbort()) {
            return;
        }
        FILE_ID_DESCRIPTOR desc{};
        desc.dwSize = sizeof(desc);
        desc.Type = FileIdType;
        desc.FileId.QuadPart = static_cast<LONGLONG>(parentDirFrns[idx]);
        HANDLE hDir = OpenFileById(
            m_hVolume,
            &desc,
            FILE_LIST_DIRECTORY | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (hDir != INVALID_HANDLE_VALUE) {
            scanDirectoryHandle(hDir);
            CloseHandle(hDir);
        }
        if ((idx & 0x3Fu) == 0x3Fu) {
            SwitchToThread();
        }
    }

    flushHydratedBatch();
}

void MftParser::StartListening() {
    if (m_IsFallbackDirectoryWalk || m_hVolume == INVALID_HANDLE_VALUE) {
        return;
    }
    StopListening();
    m_StopHydration.store(false, std::memory_order_release);
    m_IsListening.store(true, std::memory_order_release);
    m_ListenerThread = std::make_unique<std::thread>(&MftParser::UsnListenerLoop, this);
    // 机械硬盘（HDD）随机寻道仅 ~100 IOPS：跳过全盘数十万目录的后台随机预水合，
    // 搜索结果页已具备按需实时元数据水合，彻底杜绝机械盘持续 100% 磁盘占用与系统卡顿。
    if (m_IsRotationalDisk) {
        spdlog::info("Drive {}: rotational HDD detected (disk #{}); skipping background random directory hydration to protect disk I/O",
                     m_DriveLetter, m_PhysicalDiskNumber);
    } else {
        m_HydrationThread = std::make_unique<std::thread>(&MftParser::BackgroundHydrateLoop, this);
    }
}

void MftParser::StopListening() {
    m_IsListening.store(false, std::memory_order_release);
    m_StopHydration.store(true, std::memory_order_release);
    if (m_HydrationThread && m_HydrationThread->joinable()) {
        m_HydrationThread->join();
    }
    m_HydrationThread.reset();
    if (m_ListenerThread && m_ListenerThread->joinable()) {
        m_ListenerThread->join();
    }
    m_ListenerThread.reset();
}

bool MftParser::Initialize(char driveLetter) {
    resetStopRequest();
    m_InitialScanDone.store(false, std::memory_order_release);
    m_DriveLetter = driveLetter;
    m_Store.setDriveLetter(driveLetter);
    m_VolumeSerial = getVolumeSerialNumber();
    m_Initialized = true;
    const std::wstring root{static_cast<wchar_t>(driveLetter), L':', L'\\'};
    m_DriveType = GetDriveTypeW(root.c_str());
    {
        const auto topo = detectVolumeStorageTopology(driveLetter);
        m_IsRotationalDisk = topo.isRotational;
        m_HasPhysicalDiskNumber = topo.hasDiskNumber;
        m_PhysicalDiskNumber = topo.diskNumber;
        spdlog::info("Drive {}: storage topology detected (rotationalHdd={}, physicalDisk={})",
                     driveLetter,
                     m_IsRotationalDisk ? "true" : "false",
                     m_HasPhysicalDiskNumber ? std::to_string(m_PhysicalDiskNumber) : "unknown");
    }
    {
        wchar_t labelBuf[MAX_PATH + 1]{};
        if (GetVolumeInformationW(root.c_str(), labelBuf, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
            m_VolumeLabel.assign(labelBuf);
        } else {
            m_VolumeLabel.clear();
        }
    }

    std::wstring volumePath = L"\\\\.\\";
    volumePath += static_cast<wchar_t>(driveLetter);
    volumePath += L":";

    // 优先尝试以直接 NTFS 卷设备句柄打开以实现 100ms 级 MFT 全盘枚举
    m_hVolume = CreateFileW(volumePath.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);

    if (m_hVolume == INVALID_HANDLE_VALUE) {
        m_hVolume = CreateFileW(volumePath.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
    }

    if (m_hVolume == INVALID_HANDLE_VALUE || !QueryUsnJournal()) {
        if (m_hVolume != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hVolume);
            m_hVolume = INVALID_HANDLE_VALUE;
        }
        // 当以非管理员或便携模式运行时，自动启用极速目录树遍历作为优雅回退
        m_IsFallbackDirectoryWalk = true;
        spdlog::warn("Volume handle unavailable (Error: {}). Enabled DirectoryWalk fallback mode for drive {}:",
                     GetLastError(), driveLetter);
        return true;
    }

    m_IsFallbackDirectoryWalk = false;
    return true;
}

bool MftParser::QueryUsnJournal() {
    DWORD br = 0;
    USN_JOURNAL_DATA_V0 localData{};
    if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &localData, sizeof(localData), &br, NULL)) {
        // Try creating it
        CREATE_USN_JOURNAL_DATA cujd;
        cujd.MaximumSize = 0; // default
        cujd.AllocationDelta = 0; // default
        if (!DeviceIoControl(m_hVolume, FSCTL_CREATE_USN_JOURNAL, &cujd, sizeof(cujd), NULL, 0, &br, NULL)) {
            return false;
        }
        // Try query again
        if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &localData, sizeof(localData), &br, NULL)) {
            return false;
        }
    }
    std::lock_guard<std::mutex> usnLock(m_UsnMutex);
    m_UsnJournalData = localData;
    return true;
}

void MftParser::EnumerateFilesViaDirectoryWalk(char driveLetter) {
    ScopedBackgroundIoPriority bgIoGuard;
    ScopedScanPhaseGuard phaseGuard(m_ScanPhase, ScanPhase::Scanning);
    m_ScannedRecords.store(0, std::memory_order_release);
    m_EstimatedRecords.store(0, std::memory_order_release);
    spdlog::info("Starting high-speed directory walk enumeration for drive {}: ...", driveLetter);
    std::wstring rootPath;
    rootPath += static_cast<wchar_t>(driveLetter);
    rootPath += L":\\";

    uint64_t nextId = 1;
    {
        std::unique_lock lock(m_MapMutex);
        m_Store.reserve(65536);
    }
    std::vector<std::pair<std::wstring, uint64_t>> dirsToScan;
    dirsToScan.reserve(8192);
    dirsToScan.push_back({rootPath, 0});

    int count = 0;
    std::vector<FileRecordInit> batch;
    batch.reserve(2048);

    auto flushBatch = [&]() {
        if (batch.empty()) return;
        std::unique_lock lock(m_MapMutex);
        for (const auto& item : batch) {
            m_Store.upsert(item, /*metadataHydrated=*/true);
        }
        m_FileCount.store(m_Store.size(), std::memory_order_release);
        m_ScannedRecords.store(static_cast<uint64_t>(count), std::memory_order_release);
        batch.clear();
        m_IndexGeneration.fetch_add(1, std::memory_order_release);
    };

    while (!dirsToScan.empty() && !m_StopRequested.load(std::memory_order_acquire)) {
        auto [currentDir, parentId] = std::move(dirsToScan.back());
        dirsToScan.pop_back();

        std::wstring searchPattern = currentDir;
        if (searchPattern.back() != L'\\') searchPattern += L'\\';
        searchPattern += L'*';

        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &findData,
            FindExSearchNameMatch,
            nullptr,
            FIND_FIRST_EX_LARGE_FETCH
        );

        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (m_StopRequested.load(std::memory_order_acquire)) break;
            if (findData.cFileName[0] == L'.' && 
                (findData.cFileName[1] == L'\0' || (findData.cFileName[1] == L'.' && findData.cFileName[2] == L'\0'))) {
                continue;
            }

            // 跳过 NTFS 重解析点/符号链接，防止目录循环与死锁
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            const bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const uint64_t fileId = nextId++;

            FileRecordInit record;
            record.fileReferenceNumber = fileId;
            record.parentFileReferenceNumber = parentId;
            record.isDirectory = isDir;
            record.fileAttributes = findData.dwFileAttributes;
            record.fileSize = isDir ? 0 : ((static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow);
            record.creationTime = (static_cast<uint64_t>(findData.ftCreationTime.dwHighDateTime) << 32) | findData.ftCreationTime.dwLowDateTime;
            record.lastWriteTime = (static_cast<uint64_t>(findData.ftLastWriteTime.dwHighDateTime) << 32) | findData.ftLastWriteTime.dwLowDateTime;
            record.fileName = findData.cFileName;

            if (isDir) {
                std::wstring subDir = currentDir;
                if (subDir.back() != L'\\') subDir += L'\\';
                subDir += findData.cFileName;
                dirsToScan.push_back({std::move(subDir), fileId});
            }

            batch.push_back(std::move(record));
            count++;

            if (batch.size() >= 2048) {
                flushBatch();
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    flushBatch();
    if (m_StopRequested.load(std::memory_order_acquire)) {
        spdlog::info("Directory walk enumeration cancelled on drive {}:", driveLetter);
        return;
    }
    m_ScanPhase.store(static_cast<uint8_t>(ScanPhase::BuildingHierarchy), std::memory_order_release);
    {
        std::unique_lock lock(m_MapMutex);
        m_Store.recomputeHierarchyMetadata();
        m_Store.releaseBuildScratch();
        m_Store.shrinkExcessCapacity();
    }
    spdlog::info("Directory walk enumeration completed. Indexed {} files on drive {}:", count, driveLetter);
}

void MftParser::EnumerateFiles() {
    if (m_StopRequested.load(std::memory_order_acquire)) return;
    m_InitialScanDone.store(false, std::memory_order_release);
    {
        std::unique_lock lock(m_MapMutex);
        m_Store.clear();
        m_Store.setDriveLetter(m_DriveLetter);
        m_FileCount.store(0, std::memory_order_release);
    }
    if (m_IsFallbackDirectoryWalk || m_hVolume == INVALID_HANDLE_VALUE) {
        EnumerateFilesViaDirectoryWalk(m_DriveLetter);
        if (!m_StopRequested.load(std::memory_order_acquire)) {
            m_InitialScanDone.store(true, std::memory_order_release);
        }
        return;
    }

    ScopedScanPhaseGuard phaseGuard(m_ScanPhase, ScanPhase::Scanning);
    m_ScannedRecords.store(0, std::memory_order_release);
    spdlog::info("Starting MFT enumeration...");
    {
        NTFS_VOLUME_DATA_BUFFER ntfsVolData{};
        DWORD volBytesReturned = 0;
        size_t estimatedRecords = 131072;
        if (DeviceIoControl(
                m_hVolume,
                FSCTL_GET_NTFS_VOLUME_DATA,
                nullptr,
                0,
                &ntfsVolData,
                sizeof(ntfsVolData),
                &volBytesReturned,
                nullptr) &&
            ntfsVolData.BytesPerFileRecordSegment > 0 &&
            ntfsVolData.MftValidDataLength.QuadPart > 0) {
            const uint64_t rawSlots =
                static_cast<uint64_t>(ntfsVolData.MftValidDataLength.QuadPart) /
                static_cast<uint64_t>(ntfsVolData.BytesPerFileRecordSegment);
            estimatedRecords = static_cast<size_t>(
                std::clamp<uint64_t>(rawSlots, 65536ULL, 8000000ULL));
        }
        m_EstimatedRecords.store(static_cast<uint64_t>(estimatedRecords), std::memory_order_release);
        std::unique_lock lock(m_MapMutex);
        m_Store.reserve(estimatedRecords);
    }
    QueryUsnJournal();
    USN enumHighUsn = 0;
    {
        std::lock_guard<std::mutex> usnLock(m_UsnMutex);
        enumHighUsn = m_UsnJournalData.NextUsn;
    }
    MFT_ENUM_DATA_V0 med;
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = enumHighUsn;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;
    int count = 0;
    FileRecordInit reusableRecord;
    reusableRecord.fileName.reserve(128);

    while (!m_StopRequested.load(std::memory_order_acquire) &&
           DeviceIoControl(m_hVolume, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buffer.data(), BUF_LEN, &bytesReturned, NULL)) {
        if (bytesReturned <= sizeof(USN)) break;
        DWORD dwRetBytes = bytesReturned - sizeof(USN);
        USN* pUsn = reinterpret_cast<USN*>(buffer.data());
        const BYTE* pRecordBytes = buffer.data() + sizeof(USN);
        
        std::unique_lock lock(m_MapMutex);
        tools3000::service::ParsedUsnRecord parsed{};
        while (!m_StopRequested.load(std::memory_order_relaxed) &&
               dwRetBytes > 0 &&
               tools3000::service::parseUsnRecord(pRecordBytes, dwRetBytes, parsed)) {
            if (parsed.isValid && !parsed.fileName.empty()) {
                reusableRecord.fileReferenceNumber = parsed.fileReferenceNumber;
                reusableRecord.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                reusableRecord.isDirectory = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                reusableRecord.fileAttributes = parsed.fileAttributes;
                reusableRecord.fileSize = 0;
                reusableRecord.creationTime = 0;
                reusableRecord.lastWriteTime = parsed.timeStamp;
                reusableRecord.fileName.assign(parsed.fileName.data(), parsed.fileName.size());

                // 冷扫描结束后第 1152 行统一运行 recomputeHierarchyMetadata()，此处走 bulkImport=true 跳过冗余逐条层级计算
                m_Store.upsert(reusableRecord, /*metadataHydrated=*/false, /*bulkImport=*/true);
                count++;
            }

            if (parsed.recordLength == 0 || parsed.recordLength > dwRetBytes) {
                break;
            }
            dwRetBytes -= parsed.recordLength;
            pRecordBytes += parsed.recordLength;
        }
        m_FileCount.store(m_Store.size(), std::memory_order_release);
        m_ScannedRecords.store(static_cast<uint64_t>(count), std::memory_order_release);
        med.StartFileReferenceNumber = *pUsn;
    }

    m_ScanPhase.store(static_cast<uint8_t>(ScanPhase::BuildingHierarchy), std::memory_order_release);
    size_t storeCount = 0;
    uint64_t storeBytes = 0;
    const bool completedFullScan = !m_StopRequested.load(std::memory_order_acquire);
    {
        std::unique_lock lock(m_MapMutex);
        // Precompute full directory hierarchy & pathTier bitfields once after out-of-order MFT scan (`P-02`).
        m_Store.recomputeHierarchyMetadata();
        // Names repeat heavily across a volume, so interning pays for itself
        // during enumeration; afterwards only a trickle of USN updates arrives
        // and the lookup table is no longer worth its footprint.
        m_Store.releaseBuildScratch();
        m_Store.shrinkExcessCapacity();
        // `MED-14`: Read `m_Store.size()` and `m_Store.approximateBytes()` strictly BEFORE
        // releasing `m_MapMutex` so concurrent USN listener / snapshot readers never race.
        storeCount = m_Store.size();
        m_FileCount.store(storeCount, std::memory_order_release);
        storeBytes = m_Store.approximateBytes();
        if (completedFullScan) {
            m_InitialScanDone.store(true, std::memory_order_release);
        }
    }
    if (completedFullScan) {
        m_CurrentUsn.store(static_cast<uint64_t>(enumHighUsn), std::memory_order_release);
    }
    m_IndexGeneration.fetch_add(1, std::memory_order_release);
    spdlog::info("Drive {}: MFT enumeration completed. Indexed {} files ({} MB records+names, {} B/file)",
                 m_DriveLetter, storeCount, storeBytes / (1024 * 1024),
                 storeCount ? storeBytes / storeCount : 0);
}


std::wstring MftParser::buildFullPath(DWORDLONG fileReferenceNumber) const {
    m_LastSearchPathBuildCount.fetch_add(1, std::memory_order_relaxed);
    const auto* stored = m_Store.find(fileReferenceNumber);
    if (!stored) return L"";

    const tools3000::service::StoredFileRecord* chain[512];
    size_t chainLen = 0;
    DWORDLONG visited[512];
    size_t visitedLen = 0;
    DWORDLONG current = fileReferenceNumber;
    for (size_t depth = 0; depth < 512; ++depth) {
        if (visitedLen < 512) {
            visited[visitedLen++] = current;
        }
        const auto* node = m_Store.find(current);
        if (!node) break;
        const uint16_t nLen = node->nameLength();
        bool isDot = false;
        if (nLen == 1) {
            tools3000::service::FileViewScratch dotScratch;
            const auto rec = m_Store.view(*node, dotScratch);
            isDot = (rec.fileName == L".");
        }
        if (nLen > 0 && !isDot) {
            chain[chainLen++] = node;
        }
        if (node->parentFrn == current || node->parentFrn == 0) break;
        if (std::find(visited, visited + visitedLen, node->parentFrn) !=
            (visited + visitedLen)) {
            break;
        }
        current = node->parentFrn;
    }
    if (chainLen == 0) return L"";

    size_t estimatedLen = 3;
    for (size_t i = 0; i < chainLen; ++i) {
        estimatedLen += chain[i]->nameLength() + 1;
    }
    std::wstring path;
    path.reserve(estimatedLen);
    path += static_cast<wchar_t>(m_DriveLetter);
    path += L":\\";
    for (size_t i = chainLen; i > 0; --i) {
        if (path.size() > 3 && path.back() != L'\\') path += L'\\';
        m_Store.appendNameTo(*chain[i - 1], path);
    }
    return path;
}

uint64_t MftParser::getCurrentUsn() const {
    const uint64_t cur = m_CurrentUsn.load(std::memory_order_acquire);
    if (cur != 0) {
        return cur;
    }
    std::lock_guard<std::mutex> usnLock(m_UsnMutex);
    return static_cast<uint64_t>(m_UsnJournalData.NextUsn);
}

uint32_t MftParser::getVolumeSerialNumber() const {
    std::wstring root;
    root += static_cast<wchar_t>(m_DriveLetter);
    root += L":\\";
    DWORD serial = 0;
    GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
    return serial;
}

void MftParser::exportSnapshot(const SnapshotVisitor& visit, uint64_t& outLastUsn, uint32_t& outVolumeSerial) const {
    std::shared_lock lock(m_MapMutex);
    if (visit) {
        tools3000::service::FileViewScratch scratch;
        m_Store.forEach([&](const tools3000::service::StoredFileRecord& record) {
            visit(m_Store.view(record, scratch));
        });
    }
    outLastUsn = getCurrentUsn();
    outVolumeSerial = getVolumeSerialNumber();
}

bool MftParser::withSnapshotLock(const StoreSnapshotCallback& fn) const {
    if (!fn) return false;
    std::shared_lock lock(m_MapMutex);
    return fn(m_Store, getCurrentUsn(), getVolumeSerialNumber());
}

bool MftParser::importSnapshot(const SnapshotProducer& next, size_t expectedRecords,
                               uint64_t lastUsn, uint32_t volumeSerial) {
    if (m_Initialized && volumeSerial != 0) {
        const uint32_t currentSerial = (m_VolumeSerial != 0) ? m_VolumeSerial : getVolumeSerialNumber();
        if (currentSerial != 0 && currentSerial != volumeSerial) {
            spdlog::warn("Drive {}: snapshot volume serial (0x{:X}) does not match current volume (0x{:X}), discarding snapshot",
                         m_DriveLetter, volumeSerial, currentSerial);
            return false;
        }
    }
    if (!next) return false;

    ScopedScanPhaseGuard phaseGuard(m_ScanPhase, ScanPhase::LoadingSnapshot);
    m_ScannedRecords.store(0, std::memory_order_release);
    m_EstimatedRecords.store(static_cast<uint64_t>(expectedRecords), std::memory_order_release);

    {
        std::unique_lock lock(m_MapMutex);
        m_Store.clear();
        m_Store.setDriveLetter(m_DriveLetter ? m_DriveLetter : 'C');
        m_Store.reserve(expectedRecords);

        FileRecordInit scratch;
        uint64_t imported = 0;
        while (true) {
            scratch.fileName.clear();
            scratch.fileReferenceNumber = 0;
            scratch.parentFileReferenceNumber = 0;
            scratch.isDirectory = false;
            scratch.fileAttributes = 0;
            scratch.fileSize = 0;
            scratch.creationTime = 0;
            scratch.lastWriteTime = 0;
            if (!next(scratch)) break;
            const bool hasPersistedMeta = (scratch.fileSize > 0 || scratch.creationTime != 0);
            m_Store.upsert(scratch, /*metadataHydrated=*/hasPersistedMeta, /*bulkImport=*/true);
            if ((++imported & 0x1FFFu) == 0) {
                m_ScannedRecords.store(imported, std::memory_order_release);
            }
        }
        m_ScannedRecords.store(imported, std::memory_order_release);
        m_ScanPhase.store(static_cast<uint8_t>(ScanPhase::BuildingHierarchy), std::memory_order_release);
        m_Store.recomputeHierarchyMetadata();
        m_Store.releaseBuildScratch();
        m_Store.shrinkExcessCapacity();
        m_FileCount.store(m_Store.size(), std::memory_order_release);
        m_InitialScanDone.store(true, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> usnLock(m_UsnMutex);
        m_UsnJournalData.NextUsn = static_cast<USN>(lastUsn);
    }
    m_CurrentUsn.store(lastUsn, std::memory_order_release);
    m_IndexGeneration.fetch_add(1, std::memory_order_release);
    return true;
}

bool MftParser::importSnapshot(std::vector<FileRecordInit>&& records, uint64_t lastUsn, uint32_t volumeSerial) {
    size_t cursor = 0;
    const size_t total = records.size();
    return importSnapshot(
        [&](FileRecordInit& out) {
            if (cursor >= total) return false;
            out = std::move(records[cursor++]);
            return true;
        },
        total, lastUsn, volumeSerial);
}

bool MftParser::catchUpUsnJournal(uint64_t fromUsn) {
    if (m_hVolume == INVALID_HANDLE_VALUE || m_IsFallbackDirectoryWalk) {
        return false;
    }
    ScopedScanPhaseGuard phaseGuard(m_ScanPhase, ScanPhase::Syncing);

    if (!QueryUsnJournal()) {
        return false;
    }

    constexpr DWORD kDataModifyReasons =
        USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION;

    USN lowestValidUsn = 0;
    USN nextUsn = 0;
    DWORDLONG usnJournalId = 0;
    {
        std::lock_guard<std::mutex> usnLock(m_UsnMutex);
        lowestValidUsn = m_UsnJournalData.LowestValidUsn;
        nextUsn = m_UsnJournalData.NextUsn;
        usnJournalId = m_UsnJournalData.UsnJournalID;
    }

    const auto startUsn = static_cast<USN>(fromUsn);
    if (fromUsn == 0 || startUsn < lowestValidUsn || startUsn > nextUsn) {
        spdlog::warn("USN journal on drive {} is out of range (fromUsn: {}, lowest: {}, next: {}), triggering full rebuild",
                     m_DriveLetter, fromUsn, lowestValidUsn, nextUsn);
        EnumerateFiles();
        m_RescannedDuringCatchUp.store(true, std::memory_order_release);
        return true;
    }

    // Fast-path threshold: replaying >4 MB of accumulated USN journal records (~45k build/temp mutations)
    // via per-record parent hierarchy updates is slower than a sequential MFT scan (~6s for 2.5M files).
    constexpr uint64_t kMaxIncrementalUsnSpanBytes = 4ULL * 1024ULL * 1024ULL;
    constexpr uint64_t kPersistCursorDeltaBytes = 1ULL * 1024ULL * 1024ULL;
    const uint64_t usnSpanBytes = (nextUsn > startUsn)
        ? static_cast<uint64_t>(nextUsn - startUsn)
        : 0ULL;
    if (usnSpanBytes > kMaxIncrementalUsnSpanBytes) {
        spdlog::info(
            "USN journal backlog on drive {}: is {} MB (> 4 MB), performing fast full MFT rescan instead of slow journal replay",
            m_DriveLetter,
            usnSpanBytes / (1024ULL * 1024ULL));
        EnumerateFiles();
        m_RescannedDuringCatchUp.store(true, std::memory_order_release);
        return true;
    }
    if (usnSpanBytes >= kPersistCursorDeltaBytes) {
        m_RescannedDuringCatchUp.store(true, std::memory_order_release);
    }

    READ_USN_JOURNAL_DATA_V0 rujd = {0};
    rujd.StartUsn = fromUsn;
    rujd.ReasonMask = 0xFFFFFFFF;
    rujd.ReturnOnlyOnClose = FALSE;
    rujd.Timeout = 0;
    rujd.BytesToWaitFor = 0;
    rujd.UsnJournalID = usnJournalId;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;
    bool anyChanged = false;
    std::vector<std::pair<uint64_t, std::wstring>> pendingStatRefresh;
    std::unordered_set<uint64_t> queuedFrns;
    std::unordered_set<uint64_t> deletedInCatchUp;
    const bool needPathFallback = (m_hVolume == INVALID_HANDLE_VALUE || m_IsFallbackDirectoryWalk);

    while (DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &rujd, sizeof(rujd), buffer.data(), BUF_LEN, &bytesReturned, NULL)) {
        if (bytesReturned < sizeof(USN)) break;

        USN* pNextUsn = reinterpret_cast<USN*>(buffer.data());
        rujd.StartUsn = *pNextUsn;
        m_CurrentUsn.store(static_cast<uint64_t>(rujd.StartUsn), std::memory_order_release);
        {
            std::lock_guard<std::mutex> usnLock(m_UsnMutex);
            m_UsnJournalData.NextUsn = rujd.StartUsn;
        }

        if (bytesReturned == sizeof(USN)) break;

        DWORD dwRetBytes = bytesReturned - sizeof(USN);
        const BYTE* pRecordBytes = buffer.data() + sizeof(USN);

        if (dwRetBytes > 0) {
            std::unique_lock lock(m_MapMutex);
            tools3000::service::ParsedUsnRecord parsed{};
            while (dwRetBytes > 0 && tools3000::service::parseUsnRecord(pRecordBytes, dwRetBytes, parsed)) {
                if (parsed.isValid) {
                    const uint64_t frn = parsed.fileReferenceNumber;
                    if (parsed.reason & USN_REASON_FILE_DELETE) {
                        anyChanged = m_Store.erase(frn) || anyChanged;
                        deletedInCatchUp.insert(frn);
                    } else if ((parsed.reason & USN_REASON_FILE_CREATE) ||
                               (parsed.reason & USN_REASON_RENAME_NEW_NAME)) {
                        deletedInCatchUp.erase(frn);
                        if (!parsed.fileName.empty()) {
                            const bool isDir = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                            FileRecordInit record;
                            record.fileReferenceNumber = frn;
                            record.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                            record.isDirectory = isDir;
                            record.fileAttributes = parsed.fileAttributes;
                            record.lastWriteTime = parsed.timeStamp;
                            record.fileName = std::wstring(parsed.fileName);

                            // `MED-15`: Preserve existing `fileSize` and `creationTime` across renames
                            // and queue lock-free metadata refresh for modified/renamed files.
                            const auto* existing = m_Store.find(frn);
                            if (existing != nullptr) {
                                const FileRecord oldRec = m_Store.view(*existing);
                                if (!isDir) {
                                    record.fileSize = oldRec.fileSize;
                                }
                                record.creationTime = oldRec.creationTime;
                                if (record.lastWriteTime == 0) {
                                    record.lastWriteTime = oldRec.lastWriteTime;
                                }
                            }

                            m_Store.upsert(record);
                            anyChanged = true;

                            if (!isDir &&
                                ((parsed.reason & USN_REASON_RENAME_NEW_NAME) != 0 ||
                                 (parsed.reason & USN_REASON_CLOSE) != 0 ||
                                 (parsed.reason & kDataModifyReasons) != 0 ||
                                 record.fileSize == 0) &&
                                queuedFrns.insert(frn).second) {
                                pendingStatRefresh.emplace_back(
                                    frn,
                                    needPathFallback ? buildFullPath(frn) : std::wstring{});
                            }
                        }
                    } else if ((parsed.reason & USN_REASON_BASIC_INFO_CHANGE) ||
                               ((parsed.reason & USN_REASON_CLOSE) && (parsed.reason & kDataModifyReasons)) ||
                               (parsed.reason & kDataModifyReasons)) {
                        const auto* existing = m_Store.find(frn);
                        if (existing != nullptr) {
                            const FileRecord oldRec = m_Store.view(*existing);
                            FileRecordInit record;
                            record.fileReferenceNumber = frn;
                            record.parentFileReferenceNumber = oldRec.parentFileReferenceNumber;
                            record.isDirectory = oldRec.isDirectory;
                            record.fileAttributes = parsed.fileAttributes ? parsed.fileAttributes : oldRec.fileAttributes;
                            record.fileSize = oldRec.fileSize;
                            record.creationTime = oldRec.creationTime;
                            record.lastWriteTime = parsed.timeStamp ? parsed.timeStamp : oldRec.lastWriteTime;
                            record.fileName = std::wstring(oldRec.fileName);

                            m_Store.upsert(record);
                            anyChanged = true;

                            if (!oldRec.isDirectory &&
                                ((parsed.reason & USN_REASON_CLOSE) != 0 ||
                                 (parsed.reason & kDataModifyReasons) != 0) &&
                                queuedFrns.insert(frn).second) {
                                pendingStatRefresh.emplace_back(
                                    frn,
                                    needPathFallback ? buildFullPath(frn) : std::wstring{});
                            }
                        }
                    }
                }

                if (parsed.recordLength == 0 || parsed.recordLength > dwRetBytes) {
                    break;
                }
                dwRetBytes -= parsed.recordLength;
                pRecordBytes += parsed.recordLength;
            }
            m_FileCount.store(m_Store.size(), std::memory_order_release);
        }
    }

    const DWORD ioErr = GetLastError();
    if (ioErr == ERROR_JOURNAL_ENTRY_DELETED || ioErr == ERROR_JOURNAL_NOT_ACTIVE) {
        spdlog::warn("USN journal entries deleted on drive {}:, re-enumerating files", m_DriveLetter);
        EnumerateFiles();
        m_RescannedDuringCatchUp.store(true, std::memory_order_release);
        return true;
    }

    // Refresh disk metadata OUTSIDE `m_MapMutex` (`MED-15`).
    // Bound synchronous per-file `OpenFileById` refreshes during startup USN catchup (`kMaxSyncCatchUpStatRefresh`)
    // so tens of thousands of build/unpack changes accumulated while the service was stopped never stall
    // `loadSnapshot` for minutes; excess entries are marked dirty (`!FlagMetadataHydrated`) so `size:`/`dm:`
    // queries stat on demand and `BackgroundHydrateLoop` batch-hydrates directories asynchronously.
    if (!pendingStatRefresh.empty()) {
        constexpr size_t kMaxSyncCatchUpStatRefresh = 256;
        struct RefreshedMeta {
            uint64_t frn = 0;
            uint64_t fileSize = 0;
            uint64_t creationTime = 0;
            uint64_t lastWriteTime = 0;
        };
        const size_t totalPending = pendingStatRefresh.size();
        const size_t syncStartIdx = (totalPending > kMaxSyncCatchUpStatRefresh)
            ? (totalPending - kMaxSyncCatchUpStatRefresh)
            : 0;
        std::vector<RefreshedMeta> refreshed;
        refreshed.reserve(totalPending - syncStartIdx);
        for (size_t idx = syncStartIdx; idx < totalPending; ++idx) {
            const auto& [frn, fallbackPath] = pendingStatRefresh[idx];
            if (deletedInCatchUp.find(frn) != deletedInCatchUp.end()) continue;
            uint64_t fSize = 0;
            uint64_t cTime = 0;
            uint64_t mTime = 0;
            if (queryFrnDiskMetadata(frn, fallbackPath, fSize, cTime, mTime)) {
                refreshed.push_back(RefreshedMeta{frn, fSize, cTime, mTime});
            }
        }
        if (!refreshed.empty() || syncStartIdx > 0) {
            std::unique_lock statLock(m_MapMutex);
            for (size_t idx = 0; idx < syncStartIdx; ++idx) {
                const uint64_t deferredFrn = pendingStatRefresh[idx].first;
                if (deletedInCatchUp.find(deferredFrn) == deletedInCatchUp.end()) {
                    m_Store.markMetadataDirty(deferredFrn);
                }
            }
            for (const auto& item : refreshed) {
                m_Store.updateMetadata(item.frn, item.fileSize, item.creationTime, item.lastWriteTime);
            }
            anyChanged = true;
        }
    }

    if (anyChanged) {
        std::unique_lock lock(m_MapMutex);
        m_Store.compactIfSparse();
        m_IndexGeneration.fetch_add(1, std::memory_order_release);
        std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
        m_CachedQuery.clear();
        m_CachedCandidates.clear();
        m_CachedExcludePatterns.clear();
    }

    return true;
}
