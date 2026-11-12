#include "MftParser.h"
#include "PinyinEngine.h"
#include "QueryThreadPool.h"
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

std::wstring normalize(std::wstring_view value) {
    return SearchExpression::normalize(value);
}

uint64_t fileTimeToUint64(const FILETIME& value) noexcept {
    ULARGE_INTEGER raw{};
    raw.LowPart = value.dwLowDateTime;
    raw.HighPart = value.dwHighDateTime;
    return raw.QuadPart;
}

enum class HydrateResult {
    Valid,
    RejectedRecycleBin,
    GhostNotFound
};

// FSCTL_ENUM_USN_DATA is deliberately fast, but its records do not contain a
// file's size or creation time; USN_RECORD::TimeStamp is also the journal event
// time rather than the authoritative last-write time. Hydrate only the small
// final result set so the index stays compact while every displayed property is
// accurate.
//
// Real-time verification: if GetFileAttributesExW indicates the file physically
// does not exist on disk (ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND), return
// GhostNotFound so the caller can drop the phantom node and prune it from the store.
HydrateResult hydrateResultProperties(SearchResult& result, bool allowRecycleBin = false, bool allowRecentShadow = false) {
    if (result.fullPath.empty()) {
        return HydrateResult::GhostNotFound;
    }
    if (!allowRecycleBin || !allowRecentShadow) {
        std::wstring lowerPath = normalize(result.fullPath);
        if (!allowRecycleBin &&
            (lowerPath.find(L"\\$recycle.bin") != std::wstring::npos ||
             lowerPath.find(L"/$recycle.bin") != std::wstring::npos)) {
            return HydrateResult::RejectedRecycleBin;
        }
        if (!allowRecentShadow &&
            (lowerPath.find(L"\\microsoft\\windows\\recent\\") != std::wstring::npos ||
             lowerPath.find(L"/microsoft/windows/recent/") != std::wstring::npos ||
             lowerPath.find(L"\\microsoft\\office\\recent\\") != std::wstring::npos ||
             lowerPath.find(L"/microsoft/office/recent/") != std::wstring::npos)) {
            return HydrateResult::RejectedRecycleBin;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(result.fullPath.c_str(), GetFileExInfoStandard, &data)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_INVALID_NAME || err == ERROR_BAD_PATHNAME ||
            err == ERROR_DIRECTORY || err == ERROR_NOT_READY ||
            err == ERROR_INVALID_DRIVE) {
            return HydrateResult::GhostNotFound;
        }
        return HydrateResult::Valid;
    }

    const uint64_t preservedDirSize = result.fileSize;
    result.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.creationTime = fileTimeToUint64(data.ftCreationTime);
    result.lastWriteTime = fileTimeToUint64(data.ftLastWriteTime);
    result.fileSize = result.isDirectory
        ? preservedDirSize
        : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return HydrateResult::Valid;
}

double computeExtensionSignalOffset(
    std::wstring_view extView,
    std::wstring_view normFileName,
    bool explicitLock
) noexcept {
    if (!explicitLock &&
        (normFileName == L"package-lock.json" ||
         normFileName == L"pnpm-lock.yaml" ||
         normFileName == L"yarn.lock" ||
         normFileName == L"cargo.lock" ||
         normFileName == L"composer.lock" ||
         normFileName == L"poetry.lock" ||
         normFileName == L"uv.lock")) {
        return -350.0;
    }

    if (extView.empty() || extView.size() > 16) return 0.0;
    wchar_t lowerExt[16];
    for (size_t i = 0; i < extView.size(); ++i) {
        lowerExt[i] = static_cast<wchar_t>(std::towlower(extView[i]));
    }
    const std::wstring_view ext(lowerExt, extView.size());

    if (!explicitLock && ext == L"lock") {
        return -350.0;
    }
    if (ext == L"md" || ext == L"markdown" || ext == L"txt" ||
        ext == L"docx" || ext == L"doc" || ext == L"xlsx" || ext == L"xls" ||
        ext == L"pptx" || ext == L"ppt" || ext == L"pdf" || ext == L"rtf" ||
        ext == L"sql" || ext == L"py" || ext == L"ts" || ext == L"tsx" ||
        ext == L"js" || ext == L"jsx" || ext == L"cpp" || ext == L"c" ||
        ext == L"h" || ext == L"hpp" || ext == L"cs" || ext == L"java" ||
        ext == L"go" || ext == L"rs" || ext == L"vue" || ext == L"svelte" ||
        ext == L"ch" || ext == L"pld" || ext == L"ps1" || ext == L"sh" || ext == L"lua") {
        return 90.0;
    }
    if (ext == L"yaml" || ext == L"yml" || ext == L"toml" || ext == L"ini" ||
        ext == L"conf" || ext == L"config" || ext == L"json" || ext == L"jsonc" ||
        ext == L"xml") {
        return 35.0;
    }
    if (ext == L"html" || ext == L"htm" || ext == L"log" || ext == L"csv" || ext == L"tsv") {
        return -60.0;
    }
    return 0.0;
}

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
    try {
        picked->fn(idx);
    } catch (...) {
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
    if (taskCount == 1 || m_workerCount <= 1) {
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

    // Caller participates immediately and steals queued tasks while waiting,
    // guaranteeing forward progress even under nested `parallelFor` invocations.
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

    while (batch->remaining.load(std::memory_order_acquire) > 0) {
        if (stealOneTask()) {
            continue;
        }
        std::unique_lock<std::mutex> doneLk(batch->doneMutex);
        if (batch->remaining.load(std::memory_order_acquire) == 0) {
            break;
        }
        batch->doneCv.wait_for(doneLk, std::chrono::microseconds(250), [&batch]() {
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
    std::shared_lock lock(m_MapMutex);
    return m_Store.size();
}

uint64_t MftParser::getApproximateIndexBytes() const {
    std::shared_lock lock(m_MapMutex);
    return m_Store.approximateBytes();
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
                                changed = true;

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

                if (changed) {
                    m_Store.compactIfSparse();
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

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
                parentDirFrns.push_back(rec->parentFrn);
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
                m_Store.updateMetadata(item.frn, item.fileSize, item.creationTime, item.lastWriteTime);
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
    m_HydrationThread = std::make_unique<std::thread>(&MftParser::BackgroundHydrateLoop, this);
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

    std::string volumePath = "\\\\.\\";
    volumePath += driveLetter;
    volumePath += ":";

    // 优先尝试以直接 NTFS 卷设备句柄打开以实现 100ms 级 MFT 全盘枚举
    m_hVolume = CreateFileA(volumePath.c_str(), 
                            GENERIC_READ | GENERIC_WRITE, 
                            FILE_SHARE_READ | FILE_SHARE_WRITE, 
                            NULL, 
                            OPEN_EXISTING, 
                            FILE_ATTRIBUTE_NORMAL, 
                            NULL);

    if (m_hVolume == INVALID_HANDLE_VALUE) {
        m_hVolume = CreateFileA(volumePath.c_str(), 
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
    {
        std::unique_lock lock(m_MapMutex);
        m_Store.recomputeHierarchyMetadata();
        m_Store.releaseBuildScratch();
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
    }
    if (m_IsFallbackDirectoryWalk || m_hVolume == INVALID_HANDLE_VALUE) {
        EnumerateFilesViaDirectoryWalk(m_DriveLetter);
        if (!m_StopRequested.load(std::memory_order_acquire)) {
            m_InitialScanDone.store(true, std::memory_order_release);
        }
        return;
    }

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

                m_Store.upsert(reusableRecord);
                count++;
            }

            if (parsed.recordLength == 0 || parsed.recordLength > dwRetBytes) {
                break;
            }
            dwRetBytes -= parsed.recordLength;
            pRecordBytes += parsed.recordLength;
        }
        med.StartFileReferenceNumber = *pUsn;
    }

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
        // `MED-14`: Read `m_Store.size()` and `m_Store.approximateBytes()` strictly BEFORE
        // releasing `m_MapMutex` so concurrent USN listener / snapshot readers never race.
        storeCount = m_Store.size();
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

std::vector<SearchResult> MftParser::Search(const std::wstring& query, int limit,
                                           const SearchExcludeOptions& excludeOpts,
                                           const CancellationCheck& isCancelled,
                                           const SearchParseOptions& parseOpts) {
    m_LastSearchPathBuildCount.store(0, std::memory_order_relaxed);
    std::vector<SearchResult> results;
    if (query.empty() || limit <= 0 || (isCancelled && isCancelled())) return results;

    const std::wstring lowerQuery = normalize(query);
    const std::wstring cacheQueryKey = parseOpts.caseSensitive ? query : lowerQuery;
    const auto expr = SearchExpression::parse(query, parseOpts);
    const bool queryExplicitRecycle = lowerQuery.find(L"recycle") != std::wstring::npos;
    const bool queryExplicitRecent = lowerQuery.find(L"recent") != std::wstring::npos ||
                                     lowerQuery.find(L"lnk") != std::wstring::npos;
    const auto searchAllowedExts = expr.hasContentFilter()
        ? tools3000::service::content::ContentSearchEngine::instance().buildAllowedExtensionSnapshot()
        : tools3000::service::content::AllowedExtensionSnapshot{};

    struct HydratedStat {
        uint64_t frn = 0;
        uint64_t fileSize = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
    };

    auto eqAsciiLower = [](std::wstring_view s, std::wstring_view lowerLit) noexcept -> bool {
        if (s.size() != lowerLit.size()) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            wchar_t ch = s[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            if (ch != lowerLit[i]) return false;
        }
        return true;
    };

    auto containsLower = [](std::wstring_view haystack, std::wstring_view lowerNeedle) noexcept -> bool {
        if (lowerNeedle.empty()) return true;
        if (haystack.size() < lowerNeedle.size()) return false;
        const size_t maxPos = haystack.size() - lowerNeedle.size();
        const wchar_t first = lowerNeedle[0];
        for (size_t i = 0; i <= maxPos; ++i) {
            wchar_t ch = haystack[i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            else if (ch >= 0x80 && (ch < 0x2000 || ch > 0x9FFF)) ch = static_cast<wchar_t>(std::towlower(ch));
            if (ch == first) {
                bool ok = true;
                for (size_t j = 1; j < lowerNeedle.size(); ++j) {
                    wchar_t cj = haystack[i + j];
                    if (cj >= L'A' && cj <= L'Z') cj = static_cast<wchar_t>(cj - L'A' + L'a');
                    else if (cj >= 0x80 && (cj < 0x2000 || cj > 0x9FFF)) cj = static_cast<wchar_t>(std::towlower(cj));
                    if (cj != lowerNeedle[j]) {
                        ok = false;
                        break;
                    }
                }
                if (ok) return true;
            }
        }
        return false;
    };

    std::vector<std::wstring> lowerExcludePatterns;
    bool allExcludePatternsSingleSegment = true;
    lowerExcludePatterns.reserve(excludeOpts.patterns.size());
    for (const auto& pat : excludeOpts.patterns) {
        if (pat.empty()) continue;
        std::wstring lp = normalize(pat);
        if (lp.find(L'\\') != std::wstring::npos || lp.find(L'/') != std::wstring::npos) {
            allExcludePatternsSingleSegment = false;
        }
        lowerExcludePatterns.push_back(std::move(lp));
    }

    struct DirExcludeCacheMeta {
        bool excluded = false;
        uint8_t dirClassFlags = 0;
        uint8_t dirStateBits = 0;
        int32_t folderPriority = 200;
    };

    const wchar_t driveW = static_cast<wchar_t>(m_DriveLetter);
    const uint64_t rootClauseMask = expr.computeDirectorySegmentMask(std::wstring_view(&driveW, 1));
    const int32_t baseDrivePriority = (m_DriveLetter != 'C' && m_DriveLetter != 'c') ? 800 : 200;

    std::shared_lock lock(m_MapMutex);
    const uint64_t generation = m_IndexGeneration.load(std::memory_order_acquire);

    const bool hasSeparableClauses = expr.hasSeparablePathGroups();
    struct FlatMaskSlot {
        DWORDLONG frn = UINT64_MAX;
        uint64_t mask = 0;
    };
    constexpr size_t kFlatMaskCacheSize = 4096;
    constexpr size_t kFlatMaskCacheMask = kFlatMaskCacheSize - 1;
    auto hashFrnSlot = [](DWORDLONG frn) noexcept -> size_t {
        uint64_t x = frn;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x) & kFlatMaskCacheMask;
    };

    // `LOW-07`: Allow deep directory ancestor chains up to 512 levels with explicit cycle
    // detection so deeply nested folders (>64 levels) never escape blacklist/exclusion rules.
    constexpr size_t kMaxAncestorChainDepth = 512;

    auto getFastDirClauseMask = [&](DWORDLONG parentFrn,
                                    DWORDLONG& lastMaskParentFrn,
                                    uint64_t& lastDirMask,
                                    std::vector<FlatMaskSlot>& maskCache) -> uint64_t {
        if (!hasSeparableClauses) {
            return 0ULL;
        }
        if (parentFrn == 0) {
            return rootClauseMask;
        }
        if (parentFrn == lastMaskParentFrn) {
            return lastDirMask;
        }
        if (maskCache.empty()) {
            maskCache.resize(kFlatMaskCacheSize);
        }
        const size_t topSlot = hashFrnSlot(parentFrn);
        if (maskCache[topSlot].frn == parentFrn) {
            lastMaskParentFrn = parentFrn;
            lastDirMask = maskCache[topSlot].mask;
            return lastDirMask;
        }

        DWORDLONG chain[kMaxAncestorChainDepth];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        uint64_t baseMask = rootClauseMask;

        while (cur != 0 && chainLen < kMaxAncestorChainDepth) {
            const size_t s = hashFrnSlot(cur);
            if (maskCache[s].frn == cur) {
                baseMask = maskCache[s].mask;
                break;
            }
            chain[chainLen++] = cur;
            const auto* node = m_Store.find(cur);
            if (!node) {
                break;
            }
            const FileRecord dirRec = m_Store.view(*node);
            const DWORDLONG nextParent = dirRec.parentFileReferenceNumber;
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                nextParent == cur || nextParent == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            if (std::find(chain, chain + chainLen, nextParent) != (chain + chainLen)) {
                break;
            }
            cur = nextParent;
        }

        for (size_t idx = chainLen; idx > 0; --idx) {
            const DWORDLONG stepFrn = chain[idx - 1];
            const auto* node = m_Store.find(stepFrn);
            if (node) {
                const FileRecord dirRec = m_Store.view(*node);
                if (!dirRec.fileName.empty() && dirRec.fileName != L".") {
                    baseMask |= expr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                }
            }
            maskCache[hashFrnSlot(stepFrn)] = FlatMaskSlot{stepFrn, baseMask};
        }
        lastMaskParentFrn = parentFrn;
        lastDirMask = baseMask;
        return baseMask;
    };

    auto isAncestorExcludedByPattern = [&](DWORDLONG parentFrn,
                                           DWORDLONG& lastExParentFrn,
                                           bool& lastExResult,
                                           std::unordered_map<DWORDLONG, bool>& exCache) -> bool {
        if (lowerExcludePatterns.empty() || parentFrn == 0) {
            return false;
        }
        if (parentFrn == lastExParentFrn) {
            return lastExResult;
        }
        auto it = exCache.find(parentFrn);
        if (it != exCache.end()) {
            lastExParentFrn = parentFrn;
            lastExResult = it->second;
            return lastExResult;
        }

        DWORDLONG chain[kMaxAncestorChainDepth];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        bool excluded = false;
        while (cur != 0 && chainLen < kMaxAncestorChainDepth) {
            auto cIt = exCache.find(cur);
            if (cIt != exCache.end()) {
                excluded = cIt->second;
                break;
            }
            chain[chainLen++] = cur;
            const auto* node = m_Store.find(cur);
            if (!node) break;
            const FileRecord dirRec = m_Store.view(*node);
            const DWORDLONG nextParent = dirRec.parentFileReferenceNumber;
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                nextParent == cur || nextParent == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            if (std::find(chain, chain + chainLen, nextParent) != (chain + chainLen)) {
                break;
            }
            cur = nextParent;
        }

        for (size_t idx = chainLen; idx > 0; --idx) {
            const DWORDLONG stepFrn = chain[idx - 1];
            if (!excluded) {
                const auto* node = m_Store.find(stepFrn);
                if (node) {
                    const FileRecord dirRec = m_Store.view(*node);
                    for (const auto& lp : lowerExcludePatterns) {
                        if (lp.find(L'\\') == std::wstring::npos && lp.find(L'/') == std::wstring::npos) {
                            if (containsLower(dirRec.fileName, lp)) {
                                excluded = true;
                                break;
                            }
                        }
                    }
                }
            }
            exCache.emplace(stepFrn, excluded);
        }
        lastExParentFrn = parentFrn;
        lastExResult = excluded;
        return excluded;
    };

    auto resolveFallbackTier = [&](const tools3000::service::StoredFileRecord& stored,
                                   const FileRecord& record,
                                   uint8_t& outDirClassFlags,
                                   uint8_t& outPathTier,
                                   int& outPriority) {
        if (stored.isTierComputed()) {
            outDirClassFlags = stored.dirClassFlags;
            outPathTier = stored.pathTier;
            outPriority = stored.folderPriority;
            return;
        }
        // Fallback when an incremental orphan node has not yet had hierarchy metadata computed.
        uint8_t parentClassFlags = 0;
        uint8_t parentStateBits = 0;
        int32_t parentPriority = baseDrivePriority;
        if (record.parentFileReferenceNumber != 0) {
            const auto* pNode = m_Store.find(record.parentFileReferenceNumber);
            if (pNode != nullptr) {
                parentClassFlags = pNode->dirClassFlags;
                parentStateBits = pNode->dirStateBits;
                parentPriority = pNode->folderPriority;
            }
        }
        outDirClassFlags = parentClassFlags;
        int32_t computedPrio = parentPriority;
        tools3000::service::computeLeafTierAndPriority(
            record.fileName,
            record.isDirectory,
            record.parentFileReferenceNumber,
            parentClassFlags,
            parentStateBits,
            parentPriority,
            outPathTier,
            computedPrio);
        outPriority = computedPrio;
    };

    enum class CandidateEvalResult : uint8_t {
        Rejected = 0,
        MatchedInMemory = 1,
        NeedsLockFreeStat = 2,
    };

    struct RankedCandidate {
        DWORDLONG id = 0;
        FileRecord record;
        int rank = 7;
        int folderPriority = 200;
        uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
    };

    struct PendingStatCandidate {
        DWORDLONG id = 0;
        uint64_t dirMask = 0;
        std::wstring fileName;
        std::wstring normalizedName;
        std::wstring pinyinInitials;
        std::wstring pinyinFull;
        std::wstring fullPath;
        bool isDirectory = false;
        uint32_t fileAttributes = 0;
        uint64_t fileSize = 0;
        uint64_t creationTime = 0;
        uint64_t lastWriteTime = 0;
        int folderPriority = 200;
        uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
    };

    auto testCandidate = [&](DWORDLONG id,
                             const tools3000::service::StoredFileRecord& stored,
                             const FileRecord& record,
                             uint64_t dirClauseMask,
                             DWORDLONG& lastExParentFrn,
                             bool& lastExResult,
                             std::unordered_map<DWORDLONG, bool>& localExCache,
                             std::wstring& outBuiltPath,
                             int& outPriority,
                             uint8_t& outPathTier) -> CandidateEvalResult {
        if (excludeOpts.excludeHidden && (record.fileAttributes & FILE_ATTRIBUTE_HIDDEN)) {
            return CandidateEvalResult::Rejected;
        }
        if (excludeOpts.excludeSystem && (record.fileAttributes & FILE_ATTRIBUTE_SYSTEM)) {
            return CandidateEvalResult::Rejected;
        }

        // 1. Prune directories and unsupported extensions early for content queries.
        if (expr.hasContentFilter()) {
            if (record.isDirectory) return CandidateEvalResult::Rejected;
            const size_t dotPos = record.fileName.rfind(L'.');
            if (dotPos == std::wstring::npos) return CandidateEvalResult::Rejected;
            const std::wstring_view ext = std::wstring_view(record.fileName).substr(dotPos + 1);
            if (!searchAllowedExts.contains(ext)) {
                return CandidateEvalResult::Rejected;
            }
        }

        bool hasLazyPath = false;
        auto getPath = [&]() -> const std::wstring& {
            if (!hasLazyPath) {
                outBuiltPath = buildFullPath(id);
                hasLazyPath = true;
            }
            return outBuiltPath;
        };

        // 2. Lock-free in-memory evaluation (`P-02` & `U-05`): ZERO disk I/O under `m_MapMutex`,
        // and slash path fragments (`A\B\C`) short-circuit via ancestor bitmask + leaf prefix.
        SearchExpression::InMemoryMatchStatus matchStatus = SearchExpression::InMemoryMatchStatus::Matched;
        if (expr.isFullySeparable()) {
            if (!expr.matchesSeparable(record, driveW, dirClauseMask)) {
                return CandidateEvalResult::Rejected;
            }
        } else {
            matchStatus = expr.evaluateInMemoryWithDirMask(
                record, driveW, dirClauseMask, stored.isMetadataHydrated(), getPath);
            if (matchStatus == SearchExpression::InMemoryMatchStatus::Rejected) {
                return CandidateEvalResult::Rejected;
            }
        }

        // 3. Consume precomputed `dirClassFlags`, `pathTier`, and `folderPriority` in O(1) (`P-02`).
        uint8_t dirClassFlags = 0;
        resolveFallbackTier(stored, record, dirClassFlags, outPathTier, outPriority);

        if (expr.hasContentFilter() &&
            (dirClassFlags & tools3000::service::kDirClassContentSysPruned) != 0) {
            return CandidateEvalResult::Rejected;
        }
        if (!queryExplicitRecycle) {
            if ((dirClassFlags & tools3000::service::kDirClassRecycleBin) != 0 ||
                (record.isDirectory && eqAsciiLower(record.fileName, L"$recycle.bin"))) {
                return CandidateEvalResult::Rejected;
            }
        }
        if (!queryExplicitRecent &&
            (dirClassFlags & tools3000::service::kDirClassRecentDir) != 0) {
            return CandidateEvalResult::Rejected;
        }
        if (!lowerExcludePatterns.empty()) {
            if (isAncestorExcludedByPattern(record.parentFileReferenceNumber, lastExParentFrn, lastExResult, localExCache)) {
                return CandidateEvalResult::Rejected;
            }
            if (allExcludePatternsSingleSegment) {
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (containsLower(record.fileName, lowerPat)) {
                        return CandidateEvalResult::Rejected;
                    }
                }
            } else {
                const std::wstring& p = getPath();
                const std::wstring lowerP = normalize(p);
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (lowerP.find(lowerPat) != std::wstring::npos ||
                        containsLower(record.fileName, lowerPat)) {
                        return CandidateEvalResult::Rejected;
                    }
                }
            }
        }

        if (matchStatus == SearchExpression::InMemoryMatchStatus::NeedsDiskStat) {
            (void)getPath();
            return CandidateEvalResult::NeedsLockFreeStat;
        }
        return CandidateEvalResult::MatchedInMemory;
    };

    auto effectiveSingleDriveTier = [](uint8_t rawTier, int rank) noexcept -> uint8_t {
        if (rawTier == tools3000::service::kPathTierAppLauncher && rank >= 7) {
            return tools3000::service::kPathTierGenericShortcut;
        }
        return rawTier;
    };

    std::vector<DWORDLONG> candidates;
    bool canNarrow = false;
    {
        std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
        if (generation == m_CachedGeneration && !m_CachedQuery.empty() &&
            !expr.requiresFullPath() &&
            excludeOpts.excludeHidden == m_CachedExcludeHidden &&
            excludeOpts.excludeSystem == m_CachedExcludeSystem &&
            excludeOpts.patterns == m_CachedExcludePatterns &&
            parseOpts == m_CachedParseOptions &&
            cacheQueryKey.rfind(m_CachedQuery, 0) == 0 &&
            cacheQueryKey.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos &&
            m_CachedQuery.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos) {
            candidates = m_CachedCandidates;
            canNarrow = true;
        }
    }

    std::vector<RankedCandidate> ranked;
    std::vector<PendingStatCandidate> pendingStatCandidates;
    std::vector<HydratedStat> hydratedStats;

    // `HIGH-14`: Check cancellation at high frequency (every 64 items for regex queries,
    // every 256 items otherwise) so `m_MapMutex` shared lock is promptly released.
    const size_t cancelMask = expr.hasRegexClause() ? 0x3Fu : 0xFFu;

    auto appendPendingStat = [](std::vector<PendingStatCandidate>& vec,
                                DWORDLONG id,
                                uint64_t dirMask,
                                const FileRecord& record,
                                std::wstring&& fullPath,
                                int fPriority,
                                uint8_t pTier) {
        if (vec.size() >= 65536) {
            return;
        }
        const bool sameInitials = (record.pinyinInitials.data() == record.fileName.data());
        const bool sameFull = (record.pinyinFull.data() == record.fileName.data());
        PendingStatCandidate item;
        item.id = id;
        item.dirMask = dirMask;
        item.fileName = std::wstring(record.fileName);
        item.normalizedName = std::wstring(record.normalizedName);
        if (!sameInitials) {
            item.pinyinInitials = std::wstring(record.pinyinInitials);
        }
        if (!sameFull) {
            item.pinyinFull = std::wstring(record.pinyinFull);
        }
        item.fullPath = std::move(fullPath);
        item.isDirectory = record.isDirectory;
        item.fileAttributes = record.fileAttributes;
        item.fileSize = record.fileSize;
        item.creationTime = record.creationTime;
        item.lastWriteTime = record.lastWriteTime;
        item.folderPriority = fPriority;
        item.pathTier = pTier;
        vec.push_back(std::move(item));
    };

    if (canNarrow) {
        std::vector<DWORDLONG> filtered;
        filtered.reserve(std::min<size_t>(candidates.size(), 65536));
        ranked.reserve(std::min<size_t>(candidates.size(), 65536));
        std::vector<FlatMaskSlot> flatMaskCache;
        DWORDLONG lastMaskParentFrn = UINT64_MAX;
        uint64_t lastDirClauseMask = 0;
        std::unordered_map<DWORDLONG, bool> localExCache;
        DWORDLONG lastExParentFrn = UINT64_MAX;
        bool lastExResult = false;
        std::wstring scratchPath;
        size_t checked = 0;
        for (const auto id : candidates) {
            if (((checked++ & cancelMask) == 0) && isCancelled && isCancelled()) return results;
            const auto* stored = m_Store.find(id);
            if (!stored) continue;
            try {
                const FileRecord record = m_Store.view(*stored);
                const uint64_t dirMask = getFastDirClauseMask(
                    record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                int fPriority = 0;
                uint8_t pTier = tools3000::service::kPathTierPrimaryUser;
                scratchPath.clear();
                const CandidateEvalResult eval = testCandidate(
                    id, *stored, record, dirMask, lastExParentFrn, lastExResult, localExCache, scratchPath, fPriority, pTier);
                if (eval == CandidateEvalResult::MatchedInMemory) {
                    filtered.push_back(id);
                    const int rnk = expr.calculateRank(record);
                    ranked.push_back({id, record, rnk, fPriority, effectiveSingleDriveTier(pTier, rnk)});
                } else if (eval == CandidateEvalResult::NeedsLockFreeStat) {
                    appendPendingStat(pendingStatCandidates, id, dirMask, record, std::move(scratchPath), fPriority, pTier);
                }
            } catch (...) {
            }
        }
        candidates = std::move(filtered);
    } else {
        const size_t totalRecords = m_Store.slotCount();
        const unsigned int hwThreads = std::thread::hardware_concurrency();
        unsigned int numThreads = (hwThreads <= 2) ? (std::max)(1u, hwThreads) : (std::min)(hwThreads, 16u);
        if (totalRecords < 10000) numThreads = 1;

        const size_t chunkSize = (totalRecords + numThreads - 1) / numThreads;
        unsigned int activeThreads = 0;
        for (unsigned int t = 0; t < numThreads; ++t) {
            if (t * chunkSize < totalRecords) {
                ++activeThreads;
            }
        }

        std::vector<std::vector<DWORDLONG>> threadCandidates(activeThreads);
        std::vector<std::vector<RankedCandidate>> threadRanked(activeThreads);
        std::vector<std::vector<PendingStatCandidate>> threadPendingStat(activeThreads);
        std::vector<std::vector<FlatMaskSlot>> threadFlatMaskCaches(activeThreads);

        const bool useOptimisticDirPreFilter = expr.isFullySeparable() && hasSeparableClauses;
        const uint64_t negMask = expr.getNegatedSeparableGroupMask();
        uint64_t maximalDirMasks[8] = {rootClauseMask & ~negMask};
        size_t maximalDirMaskCount = 1;

        // Pass 1 (optional): compute directory mask antichain across pooled workers without barriers.
        if (useOptimisticDirPreFilter && activeThreads > 0) {
            std::vector<std::vector<uint64_t>> threadDirMasks(activeThreads);
            tools3000::service::QueryThreadPool::instance().parallelFor(activeThreads, [&](size_t t) {
                const size_t startIdx = t * chunkSize;
                const size_t endIdx = (std::min)(startIdx + chunkSize, totalRecords);
                auto& flatMaskCache = threadFlatMaskCaches[t];
                DWORDLONG lastMaskParentFrn = UINT64_MAX;
                uint64_t lastDirClauseMask = 0;
                auto& localMasks = threadDirMasks[t];
                for (size_t i = startIdx; i < endIdx; ++i) {
                    if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) break;
                    const auto* stored = m_Store.at(i);
                    if (!stored || !stored->isDirectory()) continue;
                    const FileRecord dirRec = m_Store.view(*stored);
                    if (dirRec.fileName.empty() || dirRec.fileName == L".") continue;
                    const uint64_t segMask = expr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                    if (segMask != 0) {
                        const uint64_t parentMask = getFastDirClauseMask(
                            dirRec.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                        const uint64_t rawFullMask = parentMask | segMask;
                        if ((rawFullMask & negMask) == 0 && rawFullMask != 0 &&
                            std::find(localMasks.begin(), localMasks.end(), rawFullMask) == localMasks.end()) {
                            localMasks.push_back(rawFullMask);
                        }
                    }
                }
            });

            if (isCancelled && isCancelled()) return results;

            std::vector<uint64_t> allMasks;
            if ((rootClauseMask & negMask) == 0) {
                allMasks.push_back(rootClauseMask);
            }
            for (const auto& tm : threadDirMasks) {
                for (uint64_t m : tm) {
                    if (std::find(allMasks.begin(), allMasks.end(), m) == allMasks.end()) {
                        allMasks.push_back(m);
                    }
                }
            }
            std::vector<uint64_t> antichain;
            for (size_t i = 0; i < allMasks.size(); ++i) {
                bool dominated = false;
                for (size_t j = 0; j < allMasks.size(); ++j) {
                    if (i != j && (allMasks[i] & allMasks[j]) == allMasks[i] &&
                        allMasks[i] != allMasks[j]) {
                        dominated = true;
                        break;
                    }
                }
                if (!dominated) {
                    antichain.push_back(allMasks[i]);
                }
            }
            if (antichain.empty()) {
                maximalDirMasks[0] = rootClauseMask & ~negMask;
                maximalDirMaskCount = 1;
            } else if (antichain.size() <= 8) {
                maximalDirMaskCount = antichain.size();
                for (size_t k = 0; k < maximalDirMaskCount; ++k) {
                    maximalDirMasks[k] = antichain[k];
                }
            } else {
                uint64_t unionMask = 0;
                for (uint64_t m : antichain) unionMask |= m;
                maximalDirMasks[0] = unionMask;
                maximalDirMaskCount = 1;
            }
        }

        // Pass 2: scan index shards across reusable `QueryThreadPool` (`P-02`).
        if (activeThreads > 0) {
            tools3000::service::QueryThreadPool::instance().parallelFor(activeThreads, [&](size_t t) {
                const size_t startIdx = t * chunkSize;
                const size_t endIdx = (std::min)(startIdx + chunkSize, totalRecords);
                auto& flatMaskCache = threadFlatMaskCaches[t];
                DWORDLONG lastMaskParentFrn = UINT64_MAX;
                uint64_t lastDirClauseMask = 0;
                auto& localCand = threadCandidates[t];
                auto& localRank = threadRanked[t];
                auto& localPendingStat = threadPendingStat[t];
                localCand.reserve(256);
                localRank.reserve(256);
                std::unordered_map<DWORDLONG, bool> localExCache;
                DWORDLONG lastExParentFrn = UINT64_MAX;
                bool lastExResult = false;
                std::wstring scratchPath;

                for (size_t i = startIdx; i < endIdx; ++i) {
                    if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) break;
                    const auto* stored = m_Store.at(i);
                    if (!stored) continue;
                    const FileRecord record = m_Store.view(*stored);
                    if (useOptimisticDirPreFilter) {
                        bool possible = false;
                        for (size_t m = 0; m < maximalDirMaskCount; ++m) {
                            if (expr.matchesSeparable(record, driveW, maximalDirMasks[m])) {
                                possible = true;
                                break;
                            }
                        }
                        if (!possible) {
                            continue;
                        }
                    }
                    const DWORDLONG id = record.fileReferenceNumber;
                    const uint64_t dirMask = getFastDirClauseMask(
                        record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                    int fPriority = 0;
                    uint8_t pTier = tools3000::service::kPathTierPrimaryUser;
                    scratchPath.clear();
                    const CandidateEvalResult eval = testCandidate(
                        id, *stored, record, dirMask, lastExParentFrn, lastExResult, localExCache, scratchPath, fPriority, pTier);
                    if (eval == CandidateEvalResult::MatchedInMemory) {
                        localCand.push_back(id);
                        const int rnk = expr.calculateRank(record);
                        localRank.push_back({id, record, rnk, fPriority, effectiveSingleDriveTier(pTier, rnk)});
                    } else if (eval == CandidateEvalResult::NeedsLockFreeStat) {
                        appendPendingStat(localPendingStat, id, dirMask, record, std::move(scratchPath), fPriority, pTier);
                    }
                }
            });
        }

        if (isCancelled && isCancelled()) return results;

        size_t totalFound = 0;
        size_t totalPendingStat = 0;
        for (unsigned int t = 0; t < activeThreads; ++t) {
            totalFound += threadCandidates[t].size();
            totalPendingStat += threadPendingStat[t].size();
        }

        candidates.reserve(totalFound);
        ranked.reserve(totalFound);
        pendingStatCandidates.reserve(totalPendingStat);
        for (unsigned int t = 0; t < activeThreads; ++t) {
            candidates.insert(candidates.end(),
                              std::make_move_iterator(threadCandidates[t].begin()),
                              std::make_move_iterator(threadCandidates[t].end()));
            ranked.insert(ranked.end(),
                          std::make_move_iterator(threadRanked[t].begin()),
                          std::make_move_iterator(threadRanked[t].end()));
            pendingStatCandidates.insert(pendingStatCandidates.end(),
                                         std::make_move_iterator(threadPendingStat[t].begin()),
                                         std::make_move_iterator(threadPendingStat[t].end()));
        }
    }

    const auto compareRank = [&expr](const auto& a, const auto& b) {
        if (a.pathTier != b.pathTier) return a.pathTier < b.pathTier;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (expr.hasContentFilter()) {
            if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
            return a.record.lastWriteTime > b.record.lastWriteTime;
        }
        if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
        if (a.record.lastWriteTime != b.record.lastWriteTime)
            return a.record.lastWriteTime > b.record.lastWriteTime;
        if (a.record.fileName.size() != b.record.fileName.size())
            return a.record.fileName.size() < b.record.fileName.size();
        const int cmp = _wcsnicmp(
            a.record.fileName.data(),
            b.record.fileName.data(),
            a.record.fileName.size());
        if (cmp != 0) return cmp < 0;
        return a.record.fileName < b.record.fileName;
    };

    std::vector<DWORDLONG> resultIds;

    if (pendingStatCandidates.empty()) {
        if (isCancelled && isCancelled()) return results;
        const size_t resultCount = std::min(ranked.size(), static_cast<size_t>(limit));
        if (resultCount < ranked.size()) {
            std::partial_sort(ranked.begin(), ranked.begin() + resultCount, ranked.end(), compareRank);
        } else {
            std::sort(ranked.begin(), ranked.end(), compareRank);
        }

        {
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            if (!expr.requiresFullPath() &&
                cacheQueryKey.find_first_of(L"*?|/\\: \t\r\n") == std::wstring::npos) {
                m_CachedQuery = cacheQueryKey;
                m_CachedCandidates = std::move(candidates);
                m_CachedGeneration = generation;
                m_CachedExcludeHidden = excludeOpts.excludeHidden;
                m_CachedExcludeSystem = excludeOpts.excludeSystem;
                m_CachedExcludePatterns = excludeOpts.patterns;
                m_CachedParseOptions = parseOpts;
            } else {
                m_CachedQuery.clear();
                m_CachedCandidates.clear();
                m_CachedExcludePatterns.clear();
            }
        }

        results.reserve(resultCount);
        resultIds.reserve(resultCount);
        for (size_t i = 0; i < resultCount; ++i) {
            const auto& candidate = ranked[i];
            results.push_back(SearchResult{
                std::wstring(candidate.record.fileName),
                buildFullPath(candidate.id),
                candidate.record.isDirectory,
                candidate.record.fileSize,
                candidate.record.creationTime,
                candidate.record.lastWriteTime,
                candidate.rank,
                candidate.folderPriority,
                candidate.pathTier,
            });
            resultIds.push_back(candidate.id);
        }

        // Never hold the index lock across filesystem I/O.
        lock.unlock();
    } else {
        // Materialize in-memory matches before unlocking `m_MapMutex`, then resolve
        // unhydrated `size:` / `dm:` candidates in parallel strictly OUTSIDE `m_MapMutex` (`P-02`).
        struct OwnedRankedItem {
            DWORDLONG id = 0;
            std::wstring fileName;
            std::wstring fullPath;
            bool isDirectory = false;
            uint64_t fileSize = 0;
            uint64_t creationTime = 0;
            uint64_t lastWriteTime = 0;
            int rank = 7;
            int folderPriority = 200;
            uint8_t pathTier = tools3000::service::kPathTierPrimaryUser;
        };

        std::vector<OwnedRankedItem> ownedRanked;
        ownedRanked.reserve(ranked.size() + pendingStatCandidates.size());
        for (const auto& r : ranked) {
            ownedRanked.push_back(OwnedRankedItem{
                r.id,
                std::wstring(r.record.fileName),
                buildFullPath(r.id),
                r.record.isDirectory,
                r.record.fileSize,
                r.record.creationTime,
                r.record.lastWriteTime,
                r.rank,
                r.folderPriority,
                r.pathTier,
            });
        }
        {
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            m_CachedQuery.clear();
            m_CachedCandidates.clear();
            m_CachedExcludePatterns.clear();
        }

        // Release `m_MapMutex` BEFORE any disk stat calls so `UsnListenerLoop` never blocks!
        lock.unlock();

        if (isCancelled && isCancelled()) return results;

        const size_t pendingCount = pendingStatCandidates.size();
        const size_t statBatchSize = 64;
        const size_t numStatBatches = (pendingCount + statBatchSize - 1) / statBatchSize;
        std::vector<std::vector<OwnedRankedItem>> batchMatched(numStatBatches);
        std::vector<std::vector<HydratedStat>> batchHydrated(numStatBatches);

        tools3000::service::QueryThreadPool::instance().parallelFor(numStatBatches, [&](size_t bIdx) {
            const size_t start = bIdx * statBatchSize;
            const size_t end = (std::min)(start + statBatchSize, pendingCount);
            auto& localHits = batchMatched[bIdx];
            auto& localStats = batchHydrated[bIdx];
            for (size_t k = start; k < end; ++k) {
                if (isCancelled && isCancelled()) break;
                auto& item = pendingStatCandidates[k];
                uint64_t fSize = item.fileSize;
                uint64_t cTime = item.creationTime;
                uint64_t mTime = item.lastWriteTime;
                const bool statOk = queryFrnDiskMetadata(item.id, item.fullPath, fSize, cTime, mTime);
                if (statOk) {
                    item.fileSize = fSize;
                    item.creationTime = cTime;
                    item.lastWriteTime = mTime;
                    localStats.push_back(HydratedStat{item.id, fSize, cTime, mTime});
                } else if (isRealTimeValidationEnabled()) {
                    continue;
                }

                FileRecord viewRec{};
                viewRec.fileReferenceNumber = item.id;
                viewRec.fileName = item.fileName;
                viewRec.normalizedName = item.normalizedName;
                viewRec.pinyinInitials = item.pinyinInitials.empty()
                    ? std::wstring_view(item.fileName)
                    : std::wstring_view(item.pinyinInitials);
                viewRec.pinyinFull = item.pinyinFull.empty()
                    ? std::wstring_view(item.fileName)
                    : std::wstring_view(item.pinyinFull);
                viewRec.isDirectory = item.isDirectory;
                viewRec.fileAttributes = item.fileAttributes;
                viewRec.fileSize = item.fileSize;
                viewRec.creationTime = item.creationTime;
                viewRec.lastWriteTime = item.lastWriteTime;

                const auto status = expr.evaluateInMemoryWithDirMask(
                    viewRec, driveW, item.dirMask, /*isMetadataHydrated=*/true,
                    [&item]() { return item.fullPath; });
                if (status == SearchExpression::InMemoryMatchStatus::Matched) {
                    const int rnk = expr.calculateRank(viewRec);
                    localHits.push_back(OwnedRankedItem{
                        item.id,
                        std::move(item.fileName),
                        std::move(item.fullPath),
                        item.isDirectory,
                        item.fileSize,
                        item.creationTime,
                        item.lastWriteTime,
                        rnk,
                        item.folderPriority,
                        effectiveSingleDriveTier(item.pathTier, rnk),
                    });
                }
            }
        });

        if (isCancelled && isCancelled()) return results;

        for (size_t bIdx = 0; bIdx < numStatBatches; ++bIdx) {
            ownedRanked.insert(ownedRanked.end(),
                               std::make_move_iterator(batchMatched[bIdx].begin()),
                               std::make_move_iterator(batchMatched[bIdx].end()));
            hydratedStats.insert(hydratedStats.end(),
                                 batchHydrated[bIdx].begin(),
                                 batchHydrated[bIdx].end());
        }

        const auto compareOwned = [&expr](const OwnedRankedItem& a, const OwnedRankedItem& b) {
            if (a.pathTier != b.pathTier) return a.pathTier < b.pathTier;
            if (a.rank != b.rank) return a.rank < b.rank;
            if (expr.hasContentFilter()) {
                if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
                return a.lastWriteTime > b.lastWriteTime;
            }
            if (a.folderPriority != b.folderPriority) return a.folderPriority > b.folderPriority;
            if (a.lastWriteTime != b.lastWriteTime) return a.lastWriteTime > b.lastWriteTime;
            if (a.fileName.size() != b.fileName.size()) return a.fileName.size() < b.fileName.size();
            const int cmp = _wcsnicmp(a.fileName.data(), b.fileName.data(), a.fileName.size());
            if (cmp != 0) return cmp < 0;
            return a.fileName < b.fileName;
        };

        const size_t resultCount = std::min(ownedRanked.size(), static_cast<size_t>(limit));
        if (resultCount < ownedRanked.size()) {
            std::partial_sort(ownedRanked.begin(), ownedRanked.begin() + resultCount, ownedRanked.end(), compareOwned);
        } else {
            std::sort(ownedRanked.begin(), ownedRanked.end(), compareOwned);
        }

        results.reserve(resultCount);
        resultIds.reserve(resultCount);
        for (size_t i = 0; i < resultCount; ++i) {
            auto& item = ownedRanked[i];
            results.push_back(SearchResult{
                std::move(item.fileName),
                std::move(item.fullPath),
                item.isDirectory,
                item.fileSize,
                item.creationTime,
                item.lastWriteTime,
                item.rank,
                item.folderPriority,
                item.pathTier,
            });
            resultIds.push_back(item.id);
        }
    }

    std::vector<SearchResult> validatedResults;
    validatedResults.reserve(results.size());
    std::vector<DWORDLONG> ghostFrns;
    const bool validateDiskExistence = isRealTimeValidationEnabled();

    for (size_t i = 0; i < results.size(); ++i) {
        if (isCancelled && isCancelled()) {
            validatedResults.clear();
            break;
        }
        const bool neededStat = (results[i].fileSize == 0 && results[i].lastWriteTime == 0);
        const HydrateResult status = hydrateResultProperties(results[i], queryExplicitRecycle, queryExplicitRecent);
        if (status == HydrateResult::Valid) {
            if (neededStat && (results[i].fileSize != 0 || results[i].lastWriteTime != 0)) {
                hydratedStats.push_back(HydratedStat{
                    resultIds[i],
                    results[i].fileSize,
                    results[i].creationTime,
                    results[i].lastWriteTime
                });
            }
            validatedResults.push_back(std::move(results[i]));
        } else if (status == HydrateResult::GhostNotFound) {
            if (validateDiskExistence) {
                ghostFrns.push_back(resultIds[i]);
            } else {
                // In mock / in-memory test mode without physical files on disk, keep the mock result
                validatedResults.push_back(std::move(results[i]));
            }
        } else if (status == HydrateResult::RejectedRecycleBin) {
            // Recycled files must never be returned when not explicitly requested
            if (validateDiskExistence && !queryExplicitRecycle &&
                (results[i].fullPath.find(L"$Recycle.Bin") != std::wstring::npos ||
                 results[i].fullPath.find(L"$RECYCLE.BIN") != std::wstring::npos)) {
                ghostFrns.push_back(resultIds[i]);
            }
        }
    }

    if (!ghostFrns.empty() || !hydratedStats.empty()) {
        std::unique_lock writeLock(m_MapMutex);
        for (const auto& st : hydratedStats) {
            m_Store.updateMetadata(st.frn, st.fileSize, st.creationTime, st.lastWriteTime);
        }
        bool anyErased = false;
        for (DWORDLONG ghostId : ghostFrns) {
            anyErased = m_Store.erase(ghostId) || anyErased;
        }
        if (anyErased) {
            m_Store.compactIfSparse();
            m_IndexGeneration.fetch_add(1, std::memory_order_release);
            std::lock_guard<std::mutex> cacheLock(m_SearchCacheMutex);
            m_CachedQuery.clear();
            m_CachedCandidates.clear();
            m_CachedExcludePatterns.clear();
        }
    }
    return validatedResults;
}

std::vector<ContentCandidate> MftParser::CollectContentCandidates(
    const SearchExpression& fileFilterExpr,
    const ContentCandidateOptions& options,
    const CancellationCheck& isCancelled
) const {
    std::vector<ContentCandidate> collected;
    if (isCancelled && isCancelled()) return collected;

    constexpr uint64_t MaxContentCandidateBytes = 100ULL * 1024ULL * 1024ULL;
    const std::wstring& normQuery = fileFilterExpr.getNormalizedQuery();
    const bool explicitRecycle = normQuery.find(L"recycle") != std::wstring::npos;
    const bool explicitRecent = normQuery.find(L"recent") != std::wstring::npos ||
                                normQuery.find(L"lnk") != std::wstring::npos;
    const bool explicitWindows = normQuery.find(L"windows") != std::wstring::npos ||
                                 normQuery.find(L"winsxs") != std::wstring::npos ||
                                 normQuery.find(L"system32") != std::wstring::npos;
    const bool explicitSystemDirs = explicitWindows ||
                                    normQuery.find(L"appdata") != std::wstring::npos ||
                                    normQuery.find(L"programdata") != std::wstring::npos ||
                                    normQuery.find(L"program files") != std::wstring::npos ||
                                    normQuery.find(L"program") != std::wstring::npos;
    const bool explicitNodeModules = normQuery.find(L"node_modules") != std::wstring::npos ||
                                     normQuery.find(L"bower_components") != std::wstring::npos;
    const bool explicitVcs = normQuery.find(L".git") != std::wstring::npos ||
                             normQuery.find(L".svn") != std::wstring::npos;
    const bool explicitCache = normQuery.find(L"cache") != std::wstring::npos ||
                               normQuery.find(L"pycache") != std::wstring::npos ||
                               normQuery.find(L"gradle") != std::wstring::npos ||
                               normQuery.find(L"webview") != std::wstring::npos ||
                               normQuery.find(L"gemini") != std::wstring::npos ||
                               normQuery.find(L"cursor") != std::wstring::npos ||
                               normQuery.find(L"vscode") != std::wstring::npos;
    const bool explicitDeps = normQuery.find(L"venv") != std::wstring::npos ||
                              normQuery.find(L"site-packages") != std::wstring::npos ||
                              normQuery.find(L"cargo") != std::wstring::npos ||
                              normQuery.find(L"rustup") != std::wstring::npos ||
                              normQuery.find(L"nuget") != std::wstring::npos ||
                              normQuery.find(L"vcpkg") != std::wstring::npos ||
                              normQuery.find(L"packages") != std::wstring::npos;
    const bool explicitBuild = normQuery.find(L"target") != std::wstring::npos ||
                               normQuery.find(L"cmakefiles") != std::wstring::npos ||
                               normQuery.find(L"build") != std::wstring::npos ||
                               normQuery.find(L"dist") != std::wstring::npos ||
                               normQuery.find(L"release") != std::wstring::npos ||
                               normQuery.find(L"coverage") != std::wstring::npos ||
                               normQuery.find(L"obj") != std::wstring::npos;
    const bool explicitLock = normQuery.find(L"lock") != std::wstring::npos;

    std::vector<std::wstring> lowerExcludes;
    lowerExcludes.reserve(options.excludeOpts.patterns.size());
    for (const auto& pat : options.excludeOpts.patterns) {
        if (!pat.empty()) {
            lowerExcludes.push_back(normalize(pat));
        }
    }

    struct NormalizedDialogPriority {
        std::wstring pathLower;
        double score = 0.0;
    };
    std::vector<NormalizedDialogPriority> normalizedPriorities;
    normalizedPriorities.reserve(options.dialogPriorities.size());
    for (const auto& [rawPath, rawScore] : options.dialogPriorities) {
        std::wstring clean = normalize(rawPath);
        while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
            clean.pop_back();
        }
        if (!clean.empty()) {
            normalizedPriorities.push_back(NormalizedDialogPriority{std::move(clean), rawScore});
        }
    }

    const auto allowedExts =
        tools3000::service::content::ContentSearchEngine::instance().buildAllowedExtensionSnapshot(
            options.formatFilter);
    if (allowedExts.maxExtLength == 0) {
        return collected;
    }

    std::shared_lock lock(m_MapMutex);
    const size_t totalSlots = m_Store.slotCount();
    if (totalSlots == 0) return collected;

    struct CachedFolderNode {
        bool pruned = false;
        uint64_t dirClauseMask = 0;
        std::wstring fullDirPath;
        std::wstring lowerDirPath;
        double basePriority = 200.0;
    };

    const bool hasFileFilter = !fileFilterExpr.isEmpty();
    const bool filterHasSeparableGroups = hasFileFilter && fileFilterExpr.hasSeparablePathGroups();
    const bool filterIsFullySeparable = hasFileFilter && fileFilterExpr.isFullySeparable();

    const bool isNonSystemDrive = (m_DriveLetter != 'C' && m_DriveLetter != 'c');
    const wchar_t driveW = static_cast<wchar_t>(m_DriveLetter);
    const uint64_t rootContentClauseMask = filterHasSeparableGroups
        ? fileFilterExpr.computeDirectorySegmentMask(std::wstring_view(&driveW, 1))
        : 0ULL;

    auto resolveFolderNode = [&](DWORDLONG parentFrn,
                                 std::unordered_map<DWORDLONG, CachedFolderNode>& folderCache) -> const CachedFolderNode& {
        auto it = folderCache.find(parentFrn);
        if (it != folderCache.end()) {
            return it->second;
        }

        CachedFolderNode node;
        node.dirClauseMask = rootContentClauseMask;
        if (parentFrn == 0) {
            node.fullDirPath = {driveW, L':'};
            node.lowerDirPath = {static_cast<wchar_t>(std::towlower(driveW)), L':'};
            node.basePriority = isNonSystemDrive ? 2500.0 : 900.0;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        std::vector<std::wstring_view> segments;
        segments.reserve(16);
        bool prunedByAlwaysNoise = false;
        bool prunedByDevNoise = false;
        bool hasSystemProgramSeg = false;
        bool hasAppDataSeg = false;
        bool hasWorkspaceSeg = false;
        bool hasDesktopSeg = false;
        bool hasDocsSeg = false;
        bool hasDownloadsSeg = false;
        bool hasUsersSeg = false;
        bool hasChatSeg = false;
        bool hasLowValueSeg = false;
        bool hasSecondaryArtifactSeg = false;

        DWORDLONG visited[512];
        size_t visitedLen = 0;
        DWORDLONG cur = parentFrn;
        for (size_t depth = 0; depth < 512; ++depth) {
            if (visitedLen < 512) {
                visited[visitedLen++] = cur;
            }
            const auto* storedDir = m_Store.find(cur);
            if (!storedDir) break;
            const FileRecord dirRec = m_Store.view(*storedDir);
            if (!dirRec.fileName.empty() && dirRec.fileName != L".") {
                if (filterHasSeparableGroups) {
                    node.dirClauseMask |= fileFilterExpr.computeDirectorySegmentMask(
                        dirRec.fileName, dirRec.pinyinInitials, dirRec.pinyinFull);
                }
                segments.push_back(dirRec.fileName);
                const std::wstring segNorm = normalize(dirRec.fileName);
                if (!explicitRecycle &&
                    (segNorm == L"$recycle.bin" || segNorm == L"system volume information")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitWindows &&
                           (segNorm == L"winsxs" || segNorm == L"servicing" ||
                            segNorm == L"prefetch" || segNorm == L"driverstore")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitNodeModules &&
                           (segNorm == L"node_modules" || segNorm == L"bower_components" ||
                            segNorm == L"jspm_packages")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitVcs &&
                           (segNorm == L".git" || segNorm == L".svn" || segNorm == L".hg")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitCache &&
                           (segNorm == L"gpucache" || segNorm == L"code cache" ||
                            segNorm == L"shadercache" || segNorm == L"dawncache" ||
                            segNorm == L"dawnwebgpucache" || segNorm == L"inetcache" ||
                            segNorm == L"webcache" || segNorm == L"cefcache" ||
                            segNorm == L"browsercache" || segNorm == L"ebwebview" ||
                            segNorm == L"webview2" || segNorm == L"crashpad" ||
                            segNorm == L"__pycache__" || segNorm == L".pytest_cache" ||
                            segNorm == L".mypy_cache" || segNorm == L".ruff_cache" ||
                            segNorm == L".parcel-cache" || segNorm == L".vite" ||
                            segNorm == L".pnpm-store" || segNorm == L".yarn" ||
                            segNorm == L"npm-cache" || segNorm == L"go-build" ||
                            segNorm == L".gradle" || segNorm == L".gemini" ||
                            segNorm == L".cursor" || segNorm == L".vscode-server")) {
                    prunedByAlwaysNoise = true;
                } else if (!explicitDeps &&
                           (segNorm == L".venv" || segNorm == L"venv" ||
                            segNorm == L"site-packages" || segNorm == L".tox" ||
                            segNorm == L".nox" || segNorm == L".cargo" ||
                            segNorm == L".rustup" || segNorm == L".nuget" ||
                            segNorm == L"vcpkg" || segNorm == L"vcpkg_installed")) {
                    prunedByDevNoise = true;
                } else if (!explicitBuild &&
                           (segNorm == L"target" || segNorm == L"cmakefiles" ||
                            segNorm == L"obj" || segNorm == L"build-x64" ||
                            segNorm == L"build-arm64" || segNorm == L".next" ||
                            segNorm == L".nuxt" || segNorm == L".svelte-kit" ||
                            segNorm == L".turbo" || segNorm == L".output" ||
                            segNorm == L"coverage" || segNorm == L"playwright-report" ||
                            segNorm == L"test-results")) {
                    prunedByDevNoise = true;
                } else if (segNorm == L"temp" || segNorm == L"tmp" || segNorm == L".vs" ||
                           segNorm == L"cache" || segNorm == L"caches") {
                    hasLowValueSeg = true;
                } else if (!explicitBuild &&
                           (segNorm == L"dist" || segNorm == L"out" ||
                            segNorm == L"build" || segNorm == L".build" ||
                            segNorm == L"release" || segNorm == L"debug" ||
                            segNorm == L"vendor" || segNorm == L"third_party" ||
                            segNorm == L"thirdparty" || segNorm == L"3rdparty" ||
                            segNorm == L"sapb1help" || segNorm == L"javadoc" ||
                            segNorm == L"apidocs")) {
                    hasSecondaryArtifactSeg = true;
                } else if (segNorm == L"xwechat_files" || segNorm == L"xwechat" ||
                           segNorm == L"wechat files" || segNorm == L"tencent files" ||
                           segNorm == L"nt_qq" || segNorm == L"wxwork" || segNorm == L"wecom" ||
                           segNorm == L"dingtalk" || segNorm == L"feishu" || segNorm == L"lark" ||
                           segNorm == L"telegram desktop" || segNorm == L"filestorage" ||
                           segNorm == L"file_recv" || segNorm == L"filerecv" || segNorm == L"msgattach") {
                    hasChatSeg = true;
                } else if (segNorm == L"appdata") {
                    hasAppDataSeg = true;
                } else if (segNorm == L"programdata" || segNorm == L"program files" ||
                           segNorm == L"program files (x86)" || segNorm == L"windows") {
                    hasSystemProgramSeg = true;
                } else if (segNorm == L"desktop") {
                    hasDesktopSeg = true;
                } else if (segNorm == L"documents") {
                    hasDocsSeg = true;
                } else if (segNorm == L"downloads") {
                    hasDownloadsSeg = true;
                } else if (segNorm == L"users") {
                    hasUsersSeg = true;
                } else if (segNorm == L"repo" || segNorm == L"repos" || segNorm == L"workspace" ||
                           segNorm == L"projects" || segNorm == L"work" || segNorm == L"docs" ||
                           segNorm == L"code" || segNorm == L"src" || segNorm == L"source" ||
                           segNorm == L"dev" || segNorm == L"github") {
                    hasWorkspaceSeg = true;
                }
            }
            if (dirRec.parentFileReferenceNumber == cur || dirRec.parentFileReferenceNumber == 0) {
                break;
            }
            if (std::find(visited, visited + visitedLen, dirRec.parentFileReferenceNumber) !=
                (visited + visitedLen)) {
                break;
            }
            cur = dirRec.parentFileReferenceNumber;
        }

        node.fullDirPath.reserve(128);
        node.fullDirPath.push_back(driveW);
        node.fullDirPath.push_back(L':');
        for (auto rIt = segments.rbegin(); rIt != segments.rend(); ++rIt) {
            node.fullDirPath.push_back(L'\\');
            node.fullDirPath.append(*rIt);
        }
        node.lowerDirPath = normalize(node.fullDirPath);

        if (prunedByAlwaysNoise) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        double bestDialogBoost = 0.0;
        double multiHitBonus = 0.0;
        size_t matchedDialogCount = 0;
        for (const auto& dp : normalizedPriorities) {
            const bool isBoundaryPrefix =
                (node.lowerDirPath.size() == dp.pathLower.size() && node.lowerDirPath == dp.pathLower) ||
                (node.lowerDirPath.size() > dp.pathLower.size() &&
                 node.lowerDirPath.starts_with(dp.pathLower) &&
                 node.lowerDirPath[dp.pathLower.size()] == L'\\');
            if (isBoundaryPrefix) {
                ++matchedDialogCount;
                const double specificity =
                    static_cast<double>((std::min<size_t>)(dp.pathLower.size(), 120)) * 30.0;
                const double candidateBoost = dp.score + specificity;
                if (candidateBoost > bestDialogBoost) {
                    bestDialogBoost = candidateBoost;
                }
                if (matchedDialogCount > 1) {
                    multiHitBonus = (std::min)(1200.0, multiHitBonus + 400.0);
                }
            }
        }
        const bool explicitlyPinnedByDialog = (matchedDialogCount > 0);

        if (prunedByDevNoise) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        if (!explicitSystemDirs && !explicitlyPinnedByDialog) {
            if (hasSystemProgramSeg || (hasAppDataSeg && !hasChatSeg)) {
                node.pruned = true;
                return folderCache.emplace(parentFrn, std::move(node)).first->second;
            }
        }

        if (!explicitWindows &&
            (node.lowerDirPath.find(L"\\windows\\system32") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\windows\\syswow64") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\windows\\installer") != std::wstring::npos)) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }
        if (!explicitRecent &&
            (node.lowerDirPath.find(L"\\microsoft\\windows\\recent") != std::wstring::npos ||
             node.lowerDirPath.find(L"\\microsoft\\office\\recent") != std::wstring::npos)) {
            node.pruned = true;
            return folderCache.emplace(parentFrn, std::move(node)).first->second;
        }

        if (!lowerExcludes.empty()) {
            std::wstring dirWithTrail = node.lowerDirPath;
            dirWithTrail.push_back(L'\\');
            for (const auto& exPat : lowerExcludes) {
                if (dirWithTrail.find(exPat) != std::wstring::npos) {
                    node.pruned = true;
                    return folderCache.emplace(parentFrn, std::move(node)).first->second;
                }
            }
        }

        if (hasLowValueSeg) {
            node.basePriority = 1.0;
        } else if (hasSecondaryArtifactSeg) {
            node.basePriority = 45.0;
        } else if (hasChatSeg) {
            node.basePriority = 80.0;
        } else if (hasSystemProgramSeg || hasAppDataSeg) {
            node.basePriority = 20.0 + bestDialogBoost;
        } else {
            double score = 200.0;
            if (isNonSystemDrive) {
                score += 900.0;
            }
            const size_t slashDepth = segments.size();
            if (slashDepth <= 4) {
                score += static_cast<double>(5 - slashDepth) * 350.0;
            }
            if (hasWorkspaceSeg) {
                score += 3500.0;
            }
            if ((!options.desktopDirLower.empty() && node.lowerDirPath.starts_with(options.desktopDirLower)) ||
                hasDesktopSeg) {
                score += 2200.0;
            } else if ((!options.documentsDirLower.empty() && node.lowerDirPath.starts_with(options.documentsDirLower)) ||
                       hasDocsSeg) {
                score += 2000.0;
            } else if ((!options.downloadsDirLower.empty() && node.lowerDirPath.starts_with(options.downloadsDirLower)) ||
                       hasDownloadsSeg) {
                score += 1600.0;
            } else if ((!options.userProfileDirLower.empty() && node.lowerDirPath.starts_with(options.userProfileDirLower)) ||
                       hasUsersSeg) {
                score += 1000.0;
            }
            score += bestDialogBoost + multiHitBonus;
            node.basePriority = score;
        }

        return folderCache.emplace(parentFrn, std::move(node)).first->second;
    };

    const unsigned int hwThreads = std::thread::hardware_concurrency();
    unsigned int numThreads = (hwThreads <= 2) ? (std::max)(1u, hwThreads) : (std::min)(hwThreads, 12u);
    if (totalSlots < 16384) {
        numThreads = 1;
    }

    const size_t chunkSize = (totalSlots + numThreads - 1) / numThreads;
    unsigned int activeBuckets = 0;
    for (unsigned int t = 0; t < numThreads; ++t) {
        if (t * chunkSize < totalSlots) {
            ++activeBuckets;
        }
    }

    struct DeferredContentStatItem {
        uint64_t frn = 0;
        uint64_t dirMask = 0;
        std::wstring fileName;
        std::wstring normalizedName;
        std::wstring pinyinInitials;
        std::wstring pinyinFull;
        std::wstring fullPath;
        uint32_t fileAttributes = 0;
        ContentCandidate candidate;
    };

    std::vector<std::vector<ContentCandidate>> threadBuckets(activeBuckets);
    std::vector<std::vector<DeferredContentStatItem>> threadDeferredStats(activeBuckets);

    const size_t cancelMask = fileFilterExpr.hasRegexClause() ? 0x3Fu : 0xFFu;

    auto scanRange = [&](size_t bucketIdx, size_t startIdx, size_t endIdx) {
        try {
            auto& localOut = threadBuckets[bucketIdx];
            auto& localDeferred = threadDeferredStats[bucketIdx];
            localOut.reserve((std::min<size_t>)((endIdx - startIdx) / 32 + 64, 16384));
            std::unordered_map<DWORDLONG, CachedFolderNode> localFolderCache;
            localFolderCache.reserve(2048);

            for (size_t i = startIdx; i < endIdx; ++i) {
                if (((i - startIdx) & cancelMask) == 0 && isCancelled && isCancelled()) {
                    break;
                }
                const auto* stored = m_Store.at(i);
                if (!stored) continue;
                const FileRecord record = m_Store.view(*stored);
                if (record.isDirectory) continue;

                if (options.excludeOpts.excludeHidden && (record.fileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
                if (options.excludeOpts.excludeSystem && (record.fileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
                if (record.fileSize > MaxContentCandidateBytes) continue;

                const std::wstring_view normName = record.normalizedName;
                if (normName.size() >= 2 && normName[0] == L'~' && normName[1] == L'$') {
                    continue;
                }

                const size_t dotPos = record.fileName.rfind(L'.');
                if (dotPos == std::wstring_view::npos || dotPos + 1 >= record.fileName.size()) {
                    continue;
                }
                const std::wstring_view extView = record.fileName.substr(dotPos + 1);
                if (!allowedExts.contains(extView)) {
                    continue;
                }

                const CachedFolderNode& dirNode = resolveFolderNode(
                    record.parentFileReferenceNumber, localFolderCache);
                if (dirNode.pruned) {
                    continue;
                }

                std::wstring builtFullPath;
                auto buildFullPathNow = [&]() -> std::wstring& {
                    if (builtFullPath.empty()) {
                        builtFullPath.reserve(dirNode.fullDirPath.size() + 1 + record.fileName.size());
                        builtFullPath += dirNode.fullDirPath;
                        builtFullPath.push_back(L'\\');
                        builtFullPath += record.fileName;
                    }
                    return builtFullPath;
                };

                bool needsDeferredStat = false;
                if (hasFileFilter) {
                    if (filterIsFullySeparable) {
                        if (!fileFilterExpr.matchesSeparable(record, driveW, dirNode.dirClauseMask)) {
                            continue;
                        }
                    } else {
                        auto getFullPath = [&]() -> std::wstring {
                            return buildFullPathNow();
                        };
                        const auto status = fileFilterExpr.evaluateInMemoryWithDirMask(
                            record, driveW, dirNode.dirClauseMask, stored->isMetadataHydrated(), getFullPath);
                        if (status == SearchExpression::InMemoryMatchStatus::Rejected) {
                            continue;
                        }
                        if (status == SearchExpression::InMemoryMatchStatus::NeedsDiskStat) {
                            needsDeferredStat = true;
                        }
                    }
                }

                std::wstring finalPath = std::move(buildFullPathNow());
                if (!lowerExcludes.empty()) {
                    const std::wstring lowerFileName = normalize(record.fileName);
                    bool excludedByName = false;
                    for (const auto& exPat : lowerExcludes) {
                        if (lowerFileName.find(exPat) != std::wstring::npos) {
                            excludedByName = true;
                            break;
                        }
                    }
                    if (excludedByName) continue;
                }

                const double extOffset = computeExtensionSignalOffset(extView, normName, explicitLock);
                ContentCandidate cand{
                    .fileName = std::wstring(record.fileName),
                    .fullPath = std::move(finalPath),
                    .fileSize = record.fileSize,
                    .creationTime = record.creationTime,
                    .lastWriteTime = record.lastWriteTime,
                    .priorityScore = (std::max)(1.0, dirNode.basePriority + extOffset)
                };

                if (needsDeferredStat) {
                    if (localDeferred.size() < 32768) {
                        const bool sameInit = (record.pinyinInitials.data() == record.fileName.data());
                        const bool sameFull = (record.pinyinFull.data() == record.fileName.data());
                        localDeferred.push_back(DeferredContentStatItem{
                            record.fileReferenceNumber,
                            dirNode.dirClauseMask,
                            cand.fileName,
                            std::wstring(record.normalizedName),
                            sameInit ? std::wstring{} : std::wstring(record.pinyinInitials),
                            sameFull ? std::wstring{} : std::wstring(record.pinyinFull),
                            cand.fullPath,
                            record.fileAttributes,
                            std::move(cand),
                        });
                    }
                } else {
                    localOut.push_back(std::move(cand));
                }
            }
        } catch (...) {
        }
    };

    if (activeBuckets > 0) {
        tools3000::service::QueryThreadPool::instance().parallelFor(activeBuckets, [&](size_t t) {
            const size_t startIdx = t * chunkSize;
            const size_t endIdx = (std::min)(startIdx + chunkSize, totalSlots);
            scanRange(t, startIdx, endIdx);
        });
    }

    lock.unlock();
    if (isCancelled && isCancelled()) return collected;

    size_t totalFound = 0;
    size_t totalDeferred = 0;
    for (size_t b = 0; b < activeBuckets; ++b) {
        totalFound += threadBuckets[b].size();
        totalDeferred += threadDeferredStats[b].size();
    }
    collected.reserve(totalFound + totalDeferred);
    for (auto& b : threadBuckets) {
        collected.insert(
            collected.end(),
            std::make_move_iterator(b.begin()),
            std::make_move_iterator(b.end())
        );
    }

    if (totalDeferred > 0) {
        for (auto& db : threadDeferredStats) {
            for (auto& dItem : db) {
                if (isCancelled && isCancelled()) return {};
                uint64_t fSize = dItem.candidate.fileSize;
                uint64_t cTime = dItem.candidate.creationTime;
                uint64_t mTime = dItem.candidate.lastWriteTime;
                if (queryFrnDiskMetadata(dItem.frn, dItem.fullPath, fSize, cTime, mTime)) {
                    dItem.candidate.fileSize = fSize;
                    dItem.candidate.creationTime = cTime;
                    dItem.candidate.lastWriteTime = mTime;
                }
                if (dItem.candidate.fileSize > MaxContentCandidateBytes) {
                    continue;
                }
                FileRecord recView{};
                recView.fileReferenceNumber = dItem.frn;
                recView.fileName = dItem.fileName;
                recView.normalizedName = dItem.normalizedName;
                recView.pinyinInitials = dItem.pinyinInitials.empty()
                    ? std::wstring_view(dItem.fileName)
                    : std::wstring_view(dItem.pinyinInitials);
                recView.pinyinFull = dItem.pinyinFull.empty()
                    ? std::wstring_view(dItem.fileName)
                    : std::wstring_view(dItem.pinyinFull);
                recView.isDirectory = false;
                recView.fileAttributes = dItem.fileAttributes;
                recView.fileSize = dItem.candidate.fileSize;
                recView.creationTime = dItem.candidate.creationTime;
                recView.lastWriteTime = dItem.candidate.lastWriteTime;

                if (fileFilterExpr.evaluateInMemoryWithDirMask(
                        recView, driveW, dItem.dirMask, /*isMetadataHydrated=*/true,
                        [&dItem]() { return dItem.fullPath; }) ==
                    SearchExpression::InMemoryMatchStatus::Matched) {
                    collected.push_back(std::move(dItem.candidate));
                }
            }
        }
    }

    auto comparePriority = [](const ContentCandidate& a, const ContentCandidate& b) {
        if (a.priorityScore != b.priorityScore) return a.priorityScore > b.priorityScore;
        return a.lastWriteTime > b.lastWriteTime;
    };

    const size_t cap = options.maxCandidates > 0 ? options.maxCandidates : collected.size();
    if (collected.size() > cap) {
        std::nth_element(collected.begin(), collected.begin() + cap, collected.end(), comparePriority);
        collected.resize(cap);
    }
    std::sort(collected.begin(), collected.end(), comparePriority);
    return collected;
}

std::wstring MftParser::buildFullPath(DWORDLONG fileReferenceNumber) const {
    m_LastSearchPathBuildCount.fetch_add(1, std::memory_order_relaxed);
    const auto* stored = m_Store.find(fileReferenceNumber);
    if (!stored) return L"";

    std::vector<std::wstring_view> parts;
    parts.reserve(16);
    DWORDLONG visited[512];
    size_t visitedLen = 0;
    DWORDLONG current = fileReferenceNumber;
    for (size_t depth = 0; depth < 512; ++depth) {
        if (visitedLen < 512) {
            visited[visitedLen++] = current;
        }
        const auto* node = m_Store.find(current);
        if (!node) break;
        const FileRecord rec = m_Store.view(*node);
        if (!rec.fileName.empty() && rec.fileName != L".") {
            parts.push_back(rec.fileName);
        }
        if (rec.parentFileReferenceNumber == current || rec.parentFileReferenceNumber == 0) break;
        if (std::find(visited, visited + visitedLen, rec.parentFileReferenceNumber) !=
            (visited + visitedLen)) {
            break;
        }
        current = rec.parentFileReferenceNumber;
    }
    if (parts.empty()) return L"";

    std::wstring path;
    path.reserve(256);
    path += static_cast<wchar_t>(m_DriveLetter);
    path += L":\\";
    for (auto pit = parts.rbegin(); pit != parts.rend(); ++pit) {
        if (path.size() > 3 && path.back() != L'\\') path += L'\\';
        path += *pit;
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
        m_Store.forEach([&](const tools3000::service::StoredFileRecord& record) {
            visit(m_Store.view(record));
        });
    }
    outLastUsn = getCurrentUsn();
    outVolumeSerial = getVolumeSerialNumber();
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

    {
        std::unique_lock lock(m_MapMutex);
        m_Store.clear();
        m_Store.setDriveLetter(m_DriveLetter ? m_DriveLetter : 'C');
        m_Store.reserve(expectedRecords);

        FileRecordInit scratch;
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
            m_Store.upsert(scratch);
        }
        m_Store.recomputeHierarchyMetadata();
        m_Store.releaseBuildScratch();
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
            if (deletedInCatchUp.find(frn) != deletedInCatchUp.end()) continue;
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
