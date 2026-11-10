#include "MftParser.h"
#include "PinyinEngine.h"
#include "content/ContentSearchEngine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <cwctype>

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

    result.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.creationTime = fileTimeToUint64(data.ftCreationTime);
    result.lastWriteTime = fileTimeToUint64(data.ftLastWriteTime);
    result.fileSize = result.isDirectory
        ? 0
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

}  // namespace tools3000::service

MftParser::MftParser() : m_DriveLetter('C'), m_hVolume(INVALID_HANDLE_VALUE) {
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

void MftParser::UsnListenerLoop() {
    READ_USN_JOURNAL_DATA_V0 rujd = {0};
    const uint64_t initialUsn = m_CurrentUsn.load(std::memory_order_acquire);
    rujd.StartUsn = (initialUsn != 0) ? initialUsn : m_UsnJournalData.NextUsn;
    rujd.ReasonMask = 0xFFFFFFFF; // All reasons
    rujd.ReturnOnlyOnClose = FALSE;
    rujd.Timeout = 0;
    rujd.BytesToWaitFor = 0;
    rujd.UsnJournalID = m_UsnJournalData.UsnJournalID;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;

    while (m_IsListening.load(std::memory_order_relaxed)) {
        if (!DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &rujd, sizeof(rujd), buffer.data(), BUF_LEN, &bytesReturned, NULL)) {
            const DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED || err == ERROR_JOURNAL_NOT_ACTIVE) {
                if (QueryUsnJournal()) {
                    rujd.UsnJournalID = m_UsnJournalData.UsnJournalID;
                    rujd.StartUsn = m_UsnJournalData.NextUsn;
                    m_CurrentUsn.store(rujd.StartUsn, std::memory_order_release);
                    EnumerateFiles();
                }
            }
            Sleep(100);
            continue;
        }

        if (bytesReturned >= sizeof(USN)) {
            rujd.StartUsn = *reinterpret_cast<const USN*>(buffer.data());
            m_CurrentUsn.store(rujd.StartUsn, std::memory_order_release);
            m_UsnJournalData.NextUsn = rujd.StartUsn;
        }

        if (bytesReturned <= sizeof(USN)) {
            Sleep(50);
            continue;
        }

        DWORD dwRetBytes = bytesReturned - sizeof(USN);
        const BYTE* pRecordBytes = buffer.data() + sizeof(USN);

        if (dwRetBytes > 0) {
            std::unique_lock lock(m_MapMutex);
            bool changed = false;
            tools3000::service::ParsedUsnRecord parsed{};

            while (dwRetBytes > 0 && tools3000::service::parseUsnRecord(pRecordBytes, dwRetBytes, parsed)) {
                if (parsed.isValid) {
                    if (parsed.reason & USN_REASON_FILE_DELETE) {
                        changed = m_Store.erase(parsed.fileReferenceNumber) || changed;
                    } else if ((parsed.reason & USN_REASON_FILE_CREATE) ||
                               (parsed.reason & USN_REASON_RENAME_NEW_NAME)) {
                        if (!parsed.fileName.empty()) {
                            FileRecordInit record;
                            record.fileReferenceNumber = parsed.fileReferenceNumber;
                            record.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                            record.isDirectory = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                            record.fileAttributes = parsed.fileAttributes;
                            record.lastWriteTime = parsed.timeStamp;
                            record.fileName = std::wstring(parsed.fileName);

                            m_Store.upsert(record);
                            changed = true;
                        }
                    } else if (parsed.reason & USN_REASON_BASIC_INFO_CHANGE) {
                        const auto* existing = m_Store.find(parsed.fileReferenceNumber);
                        if (existing) {
                            FileRecord oldRec = m_Store.view(*existing);
                            FileRecordInit record;
                            record.fileReferenceNumber = parsed.fileReferenceNumber;
                            record.parentFileReferenceNumber = oldRec.parentFileReferenceNumber;
                            record.isDirectory = oldRec.isDirectory;
                            record.fileAttributes = parsed.fileAttributes;
                            record.fileSize = oldRec.fileSize;
                            record.creationTime = oldRec.creationTime;
                            record.lastWriteTime = parsed.timeStamp ? parsed.timeStamp : oldRec.lastWriteTime;
                            record.fileName = std::wstring(oldRec.fileName);

                            m_Store.upsert(record);
                            changed = true;
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
        Sleep(50); // Prevent 100% CPU on fast changes
    }
}

void MftParser::StartListening() {
    if (m_IsFallbackDirectoryWalk || m_hVolume == INVALID_HANDLE_VALUE) {
        return;
    }
    StopListening();
    m_IsListening = true;
    m_ListenerThread = std::make_unique<std::thread>(&MftParser::UsnListenerLoop, this);
}

void MftParser::StopListening() {
    m_IsListening = false;
    if (m_ListenerThread && m_ListenerThread->joinable()) {
        m_ListenerThread->join();
    }
    m_ListenerThread.reset();
}

bool MftParser::Initialize(char driveLetter) {
    resetStopRequest();
    m_DriveLetter = driveLetter;
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
    DWORD br;
    if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &m_UsnJournalData, sizeof(m_UsnJournalData), &br, NULL)) {
        // Try creating it
        CREATE_USN_JOURNAL_DATA cujd;
        cujd.MaximumSize = 0; // default
        cujd.AllocationDelta = 0; // default
        if (!DeviceIoControl(m_hVolume, FSCTL_CREATE_USN_JOURNAL, &cujd, sizeof(cujd), NULL, 0, &br, NULL)) {
            return false;
        }
        // Try query again
        if (!DeviceIoControl(m_hVolume, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &m_UsnJournalData, sizeof(m_UsnJournalData), &br, NULL)) {
            return false;
        }
    }
    return true;
}

void MftParser::EnumerateFilesViaDirectoryWalk(char driveLetter) {
    spdlog::info("Starting high-speed directory walk enumeration for drive {}: ...", driveLetter);
    std::wstring rootPath;
    rootPath += static_cast<wchar_t>(driveLetter);
    rootPath += L":\\";

    uint64_t nextId = 1;
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
            m_Store.upsert(item);
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
    spdlog::info("Directory walk enumeration completed. Indexed {} files on drive {}:", count, driveLetter);
}

void MftParser::EnumerateFiles() {
    if (m_StopRequested.load(std::memory_order_acquire)) return;
    {
        std::unique_lock lock(m_MapMutex);
        m_Store.clear();
    }
    if (m_IsFallbackDirectoryWalk || m_hVolume == INVALID_HANDLE_VALUE) {
        EnumerateFilesViaDirectoryWalk(m_DriveLetter);
        return;
    }

    spdlog::info("Starting MFT enumeration...");
    QueryUsnJournal();
    MFT_ENUM_DATA_V0 med;
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = m_UsnJournalData.NextUsn;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;
    int count = 0;

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
                FileRecordInit record;
                record.fileReferenceNumber = parsed.fileReferenceNumber;
                record.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                record.isDirectory = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                record.fileAttributes = parsed.fileAttributes;
                record.lastWriteTime = parsed.timeStamp;
                record.fileName = std::wstring(parsed.fileName);

                m_Store.upsert(record);
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
    {
        std::unique_lock lock(m_MapMutex);
        // Names repeat heavily across a volume, so interning pays for itself
        // during enumeration; afterwards only a trickle of USN updates arrives
        // and the lookup table is no longer worth its footprint.
        m_Store.releaseBuildScratch();
    }
    m_CurrentUsn.store(m_UsnJournalData.NextUsn, std::memory_order_release);
    m_IndexGeneration.fetch_add(1, std::memory_order_release);
    const uint64_t storeBytes = m_Store.approximateBytes();
    spdlog::info("Drive {}: MFT enumeration completed. Indexed {} files ({} MB records+names, {} B/file)",
                 m_DriveLetter, m_Store.size(), storeBytes / (1024 * 1024),
                 m_Store.size() ? storeBytes / m_Store.size() : 0);
}

std::vector<SearchResult> MftParser::Search(const std::wstring& query, int limit,
                                           const SearchExcludeOptions& excludeOpts,
                                           const CancellationCheck& isCancelled,
                                           const SearchParseOptions& parseOpts) {
    std::vector<SearchResult> results;
    if (query.empty() || (isCancelled && isCancelled())) return results;

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

    auto endsWithAsciiLower = [](std::wstring_view s, std::wstring_view lowerSuffix) noexcept -> bool {
        if (s.size() <= lowerSuffix.size()) return false;
        const size_t offset = s.size() - lowerSuffix.size();
        for (size_t i = 0; i < lowerSuffix.size(); ++i) {
            wchar_t ch = s[offset + i];
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
            if (ch != lowerSuffix[i]) return false;
        }
        return true;
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

    enum DirMetaFlags : uint8_t {
        kDirFlagRecycleBin       = 1u << 0,
        kDirFlagRecentDir        = 1u << 1,
        kDirFlagContentSysPruned = 1u << 2,
        kDirFlagExcludedPattern  = 1u << 3,
        kDirFlagBottomPriority   = 1u << 4,
        kDirFlagChatHistory      = 1u << 5,
        kDirFlagLowAppPriority   = 1u << 6,
        kDirFlagDevWorkspace     = 1u << 7,
    };

    struct DirScanMeta {
        uint64_t clauseMask = 0;
        int32_t folderPriority = 200;
        uint16_t depth = 0;
        uint8_t flags = 0;
        uint8_t specialKind = 0; // bit0..1: 1=windows, 2=office; bit7(0x80): desktop/documents/downloads
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

        DWORDLONG chain[64];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        uint64_t baseMask = rootClauseMask;

        while (cur != 0 && chainLen < 64) {
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
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                dirRec.parentFileReferenceNumber == cur || dirRec.parentFileReferenceNumber == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            cur = dirRec.parentFileReferenceNumber;
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

    auto getDirMeta = [&](DWORDLONG parentFrn,
                          DWORDLONG& lastParentFrn,
                          DirScanMeta& lastDirMeta,
                          std::unordered_map<DWORDLONG, DirScanMeta>& dirCache) -> const DirScanMeta& {
        if (parentFrn == lastParentFrn) {
            return lastDirMeta;
        }
        auto it = dirCache.find(parentFrn);
        if (it != dirCache.end()) {
            lastParentFrn = parentFrn;
            lastDirMeta = it->second;
            return lastDirMeta;
        }

        DWORDLONG chain[64];
        size_t chainLen = 0;
        DWORDLONG cur = parentFrn;
        DirScanMeta baseMeta{};
        baseMeta.clauseMask = rootClauseMask;
        baseMeta.folderPriority = baseDrivePriority;

        while (cur != 0 && chainLen < 64) {
            auto cachedIt = dirCache.find(cur);
            if (cachedIt != dirCache.end()) {
                baseMeta = cachedIt->second;
                break;
            }
            chain[chainLen++] = cur;
            const auto* node = m_Store.find(cur);
            if (!node) {
                break;
            }
            const FileRecord dirRec = m_Store.view(*node);
            if (dirRec.fileName.empty() || dirRec.fileName == L"." ||
                dirRec.parentFileReferenceNumber == cur || dirRec.parentFileReferenceNumber == 0) {
                if (dirRec.fileName.empty() || dirRec.fileName == L".") {
                    --chainLen;
                }
                break;
            }
            cur = dirRec.parentFileReferenceNumber;
        }

        for (size_t idx = chainLen; idx > 0; --idx) {
            const DWORDLONG stepFrn = chain[idx - 1];
            const auto* node = m_Store.find(stepFrn);
            if (node) {
                const FileRecord dirRec = m_Store.view(*node);
                const std::wstring_view name = dirRec.fileName;
                if (!name.empty() && name != L".") {
                    baseMeta.clauseMask |= expr.computeDirectorySegmentMask(
                        name, dirRec.pinyinInitials, dirRec.pinyinFull);
                    if (baseMeta.depth < 60000) {
                        baseMeta.depth++;
                    }
                    const uint8_t parentSpecial = static_cast<uint8_t>(baseMeta.specialKind & 0x03u);
                    baseMeta.specialKind = static_cast<uint8_t>(baseMeta.specialKind & 0x80u);
                    if (eqAsciiLower(name, L"windows")) {
                        baseMeta.specialKind |= 1u;
                    } else if (eqAsciiLower(name, L"office")) {
                        baseMeta.specialKind |= 2u;
                    }

                    if (eqAsciiLower(name, L"$recycle.bin")) {
                        baseMeta.flags |= (kDirFlagRecycleBin | kDirFlagContentSysPruned | kDirFlagBottomPriority);
                    } else if (eqAsciiLower(name, L"recent") && (parentSpecial == 1u || parentSpecial == 2u)) {
                        baseMeta.flags |= (kDirFlagRecentDir | kDirFlagBottomPriority);
                    } else if ((eqAsciiLower(name, L"winsxs") || eqAsciiLower(name, L"system32")) && parentSpecial == 1u) {
                        baseMeta.flags |= kDirFlagContentSysPruned;
                    }

                    if (!lowerExcludePatterns.empty() && (baseMeta.flags & kDirFlagExcludedPattern) == 0) {
                        for (const auto& lp : lowerExcludePatterns) {
                            if (lp.find(L'\\') == std::wstring::npos && lp.find(L'/') == std::wstring::npos) {
                                if (containsLower(name, lp)) {
                                    baseMeta.flags |= kDirFlagExcludedPattern;
                                    break;
                                }
                            }
                        }
                    }

                    if ((baseMeta.flags & kDirFlagBottomPriority) == 0) {
                        if (eqAsciiLower(name, L"npm-cache") || eqAsciiLower(name, L"pip") ||
                            eqAsciiLower(name, L"go-build") || eqAsciiLower(name, L".gradle") ||
                            eqAsciiLower(name, L"temp") || eqAsciiLower(name, L"tmp") ||
                            eqAsciiLower(name, L"cache") || eqAsciiLower(name, L"caches") ||
                            eqAsciiLower(name, L"inetcache") || eqAsciiLower(name, L"webcache") ||
                            eqAsciiLower(name, L"gpucache") || eqAsciiLower(name, L"code cache") ||
                            eqAsciiLower(name, L"shadercache") || eqAsciiLower(name, L"recent") ||
                            eqAsciiLower(name, L"prefetch") || eqAsciiLower(name, L"winsxs") ||
                            eqAsciiLower(name, L"windows") || eqAsciiLower(name, L"$recycle.bin") ||
                            eqAsciiLower(name, L"node_modules") || eqAsciiLower(name, L".git") ||
                            eqAsciiLower(name, L"cefcache") || eqAsciiLower(name, L"crashpad") ||
                            eqAsciiLower(name, L"coverage_report") || eqAsciiLower(name, L"__pycache__") ||
                            eqAsciiLower(name, L".vs")) {
                            baseMeta.flags |= kDirFlagBottomPriority;
                        } else if (eqAsciiLower(name, L"xwechat_files") || eqAsciiLower(name, L"xwechat") ||
                                   eqAsciiLower(name, L"wechat files") || eqAsciiLower(name, L"tencent files") ||
                                   eqAsciiLower(name, L"nt_qq") || eqAsciiLower(name, L"wxwork") ||
                                   eqAsciiLower(name, L"wecom") || eqAsciiLower(name, L"dingtalk") ||
                                   eqAsciiLower(name, L"feishu") || eqAsciiLower(name, L"lark") ||
                                   eqAsciiLower(name, L"telegram desktop") || eqAsciiLower(name, L"filestorage") ||
                                   eqAsciiLower(name, L"file_recv") || eqAsciiLower(name, L"filerecv") ||
                                   eqAsciiLower(name, L"msgattach")) {
                            baseMeta.flags |= kDirFlagChatHistory;
                        } else if (eqAsciiLower(name, L"appdata") || eqAsciiLower(name, L"program files") ||
                                   eqAsciiLower(name, L"programdata") || eqAsciiLower(name, L"program files (x86)")) {
                            baseMeta.flags |= kDirFlagLowAppPriority;
                        } else if (eqAsciiLower(name, L"desktop") || eqAsciiLower(name, L"documents") ||
                                   eqAsciiLower(name, L"downloads")) {
                            baseMeta.specialKind |= 0x80u;
                        } else if (eqAsciiLower(name, L"repo") || eqAsciiLower(name, L"repos") ||
                                   eqAsciiLower(name, L"workspace") || eqAsciiLower(name, L"projects") ||
                                   eqAsciiLower(name, L"code") || eqAsciiLower(name, L"src") ||
                                   eqAsciiLower(name, L"source") || eqAsciiLower(name, L"dev") ||
                                   eqAsciiLower(name, L"github")) {
                            baseMeta.flags |= kDirFlagDevWorkspace;
                        }
                    }
                }
            }

            int32_t prio = baseDrivePriority;
            if ((baseMeta.flags & kDirFlagBottomPriority) != 0) {
                prio = 1;
            } else if ((baseMeta.flags & kDirFlagChatHistory) != 0) {
                prio = 80;
            } else {
                if ((baseMeta.flags & kDirFlagLowAppPriority) != 0) {
                    prio = (std::min)(prio, 20);
                } else if ((baseMeta.specialKind & 0x80u) != 0) {
                    prio = (std::max)(prio, 2000);
                }
                if ((baseMeta.flags & kDirFlagDevWorkspace) != 0 && prio > 20) {
                    prio = (std::max)(prio, 3500);
                }
                if (prio > 20 && baseMeta.depth >= 1 && baseMeta.depth <= 3) {
                    prio += static_cast<int32_t>((4 - baseMeta.depth) * 300);
                }
            }
            baseMeta.folderPriority = prio;
            dirCache.emplace(stepFrn, baseMeta);
            if (idx == 1) {
                lastParentFrn = parentFrn;
                lastDirMeta = baseMeta;
            }
        }
        if (chainLen == 0) {
            dirCache.emplace(parentFrn, baseMeta);
            lastParentFrn = parentFrn;
            lastDirMeta = baseMeta;
        }
        return lastDirMeta;
    };

    auto resolveCandidatePriority = [&endsWithAsciiLower](const DirScanMeta& dirMeta, const FileRecord& fileRec) noexcept -> int {
        const std::wstring_view normName = fileRec.fileName;
        if (!fileRec.isDirectory && !normName.empty()) {
            if (normName.rfind(L"~$", 0) == 0 ||
                endsWithAsciiLower(normName, L".tmp") ||
                endsWithAsciiLower(normName, L".bak") ||
                endsWithAsciiLower(normName, L".temp") ||
                endsWithAsciiLower(normName, L".part") ||
                endsWithAsciiLower(normName, L".crdownload")) {
                return 1;
            }
            if (endsWithAsciiLower(normName, L".lnk") ||
                endsWithAsciiLower(normName, L".url") ||
                endsWithAsciiLower(normName, L".desklink")) {
                return (dirMeta.flags & kDirFlagBottomPriority) != 0 ? 1 : 10;
            }
        }
        if (fileRec.parentFileReferenceNumber == 0) {
            return 200;
        }
        return dirMeta.folderPriority;
    };

    auto testCandidate = [&](DWORDLONG id,
                             const FileRecord& record,
                             uint64_t dirClauseMask,
                             DWORDLONG& lastParentFrn,
                             DirScanMeta& lastDirMeta,
                             std::unordered_map<DWORDLONG, DirScanMeta>& localDirCache,
                             std::vector<HydratedStat>& statOut,
                             int& outPriority) {
        if (excludeOpts.excludeHidden && (record.fileAttributes & FILE_ATTRIBUTE_HIDDEN)) return false;
        if (excludeOpts.excludeSystem && (record.fileAttributes & FILE_ATTRIBUTE_SYSTEM)) return false;

        // 1. 若为全文内容搜索模式，提前剪枝目录与不支持的二进制/媒体格式
        if (expr.hasContentFilter()) {
            if (record.isDirectory) return false;
            size_t dotPos = record.fileName.rfind(L'.');
            if (dotPos == std::wstring::npos) return false;
            std::wstring_view ext = std::wstring_view(record.fileName).substr(dotPos + 1);
            if (!searchAllowedExts.contains(ext)) {
                return false;
            }
        }

        std::wstring lazyPath;
        bool hasLazyPath = false;
        auto getPath = [&]() -> const std::wstring& {
            if (!hasLazyPath) {
                lazyPath = buildFullPath(id);
                hasLazyPath = true;
            }
            return lazyPath;
        };

        // 2. 先执行可分离目录掩码加速的纯内存表达式比对 (99.99% 非匹配文件在此立即返回 false，0 次触发 getDirMeta / 0 次堆分配)
        if (expr.isFullySeparable()) {
            if (!expr.matchesSeparable(record, driveW, dirClauseMask)) {
                return false;
            }
        } else {
            SearchExpression::StatHydrateCallback onStat;
            if (record.fileSize == 0 && record.lastWriteTime == 0 && expr.requiresFullPath()) {
                onStat = [&statOut](uint64_t frn, uint64_t fSize, uint64_t cTime, uint64_t mTime) {
                    if (statOut.size() < 32768) {
                        statOut.push_back(HydratedStat{frn, fSize, cTime, mTime});
                    }
                };
            }
            if (!expr.matchesWithDirMask(record, driveW, dirClauseMask, getPath, onStat)) {
                return false;
            }
        }

        // 3. 仅对极少数真正命中搜索表达式的候选文件，计算完整目录元数据 (RecycleBin / Recent / 排除规则 / 优先级)
        const DirScanMeta& dirMeta = getDirMeta(record.parentFileReferenceNumber, lastParentFrn, lastDirMeta, localDirCache);
        if (expr.hasContentFilter() && (dirMeta.flags & kDirFlagContentSysPruned) != 0) {
            return false;
        }
        if (!queryExplicitRecycle) {
            if ((dirMeta.flags & kDirFlagRecycleBin) != 0 ||
                (record.isDirectory && eqAsciiLower(record.fileName, L"$recycle.bin"))) {
                return false;
            }
        }
        if (!queryExplicitRecent && (dirMeta.flags & kDirFlagRecentDir) != 0) {
            return false;
        }
        if (!lowerExcludePatterns.empty()) {
            if ((dirMeta.flags & kDirFlagExcludedPattern) != 0) {
                return false;
            }
            if (allExcludePatternsSingleSegment) {
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (containsLower(record.fileName, lowerPat)) {
                        return false;
                    }
                }
            } else {
                const std::wstring& p = getPath();
                const std::wstring lowerP = normalize(p);
                for (const auto& lowerPat : lowerExcludePatterns) {
                    if (lowerP.find(lowerPat) != std::wstring::npos ||
                        containsLower(record.fileName, lowerPat)) {
                        return false;
                    }
                }
            }
        }

        outPriority = resolveCandidatePriority(dirMeta, record);
        return true;
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

    // FileRecord is a view over the arena, so copying one is a handful of
    // pointers and never allocates.
    struct RankedCandidate {
        DWORDLONG id;
        FileRecord record;
        int rank;
        int folderPriority;
    };
    std::vector<RankedCandidate> ranked;
    std::vector<HydratedStat> hydratedStats;

    if (canNarrow) {
        std::vector<DWORDLONG> filtered;
        filtered.reserve(std::min<size_t>(candidates.size(), 65536));
        ranked.reserve(std::min<size_t>(candidates.size(), 65536));
        std::vector<FlatMaskSlot> flatMaskCache;
        DWORDLONG lastMaskParentFrn = UINT64_MAX;
        uint64_t lastDirClauseMask = 0;
        std::unordered_map<DWORDLONG, DirScanMeta> localDirCache;
        localDirCache.reserve(64);
        DWORDLONG lastParentFrn = UINT64_MAX;
        DirScanMeta lastDirMeta{};
        size_t checked = 0;
        for (const auto id : candidates) {
            if (((checked++ & 0x3FFu) == 0) && isCancelled && isCancelled()) return results;
            const auto* stored = m_Store.find(id);
            if (!stored) continue;
            const FileRecord record = m_Store.view(*stored);
            const uint64_t dirMask = getFastDirClauseMask(record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
            int fPriority = 0;
            if (testCandidate(id, record, dirMask, lastParentFrn, lastDirMeta, localDirCache, hydratedStats, fPriority)) {
                filtered.push_back(id);
                ranked.push_back({id, record, expr.calculateRank(record), fPriority});
            }
        }
        candidates = std::move(filtered);
    } else {
        const size_t totalRecords = m_Store.slotCount();
        const unsigned int hwThreads = std::thread::hardware_concurrency();
        // 针对单核/双核弱虚拟机自适应调优：当硬件核心数为 1 或 2 时，使用 1~2 线程，杜绝上下文切换风暴
        unsigned int numThreads = (hwThreads <= 2) ? (std::max)(1u, hwThreads) : (std::min)(hwThreads, 16u);
        if (totalRecords < 10000) numThreads = 1;

        std::vector<std::vector<DWORDLONG>> threadCandidates(numThreads);
        std::vector<std::vector<RankedCandidate>> threadRanked(numThreads);
        std::vector<std::vector<HydratedStat>> threadHydrated(numThreads);

        size_t chunkSize = (totalRecords + numThreads - 1) / numThreads;
        unsigned int activeThreads = 0;
        for (unsigned int t = 0; t < numThreads; ++t) {
            if (t * chunkSize < totalRecords) {
                ++activeThreads;
            }
        }

        const bool useOptimisticDirPreFilter = expr.isFullySeparable() && hasSeparableClauses;
        const uint64_t negMask = expr.getNegatedSeparableGroupMask();
        std::vector<std::vector<uint64_t>> threadDirMasks(activeThreads);
        uint64_t maximalDirMasks[8] = {rootClauseMask & ~negMask};
        size_t maximalDirMaskCount = 1;
        std::mutex barrierMutex;
        std::condition_variable barrierCv;
        unsigned int barrierArrived = 0;

        std::vector<std::thread> workers;
        workers.reserve(activeThreads);

        for (unsigned int t = 0; t < activeThreads; ++t) {
            size_t startIdx = t * chunkSize;
            size_t endIdx = std::min(startIdx + chunkSize, totalRecords);

            workers.emplace_back([&, t, startIdx, endIdx]() {
                std::vector<FlatMaskSlot> flatMaskCache;
                DWORDLONG lastMaskParentFrn = UINT64_MAX;
                uint64_t lastDirClauseMask = 0;

                if (useOptimisticDirPreFilter) {
                    try {
                        auto& localMasks = threadDirMasks[t];
                        for (size_t i = startIdx; i < endIdx; ++i) {
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
                    } catch (...) {
                    }
                    {
                        std::unique_lock<std::mutex> lk(barrierMutex);
                        if (++barrierArrived == activeThreads) {
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
                            barrierCv.notify_all();
                        } else {
                            barrierCv.wait(lk, [&]() { return barrierArrived == activeThreads; });
                        }
                    }
                }

                auto& localCand = threadCandidates[t];
                auto& localRank = threadRanked[t];
                auto& localStat = threadHydrated[t];
                localCand.reserve(256);
                localRank.reserve(256);
                std::unordered_map<DWORDLONG, DirScanMeta> localDirCache;
                localDirCache.reserve(64);
                DWORDLONG lastParentFrn = UINT64_MAX;
                DirScanMeta lastDirMeta{};

                for (size_t i = startIdx; i < endIdx; ++i) {
                    if (((i - startIdx) & 0x3FFu) == 0 && isCancelled && isCancelled()) break;
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
                    DWORDLONG id = record.fileReferenceNumber;
                    const uint64_t dirMask = getFastDirClauseMask(record.parentFileReferenceNumber, lastMaskParentFrn, lastDirClauseMask, flatMaskCache);
                    int fPriority = 0;
                    if (testCandidate(id, record, dirMask, lastParentFrn, lastDirMeta, localDirCache, localStat, fPriority)) {
                        localCand.push_back(id);
                        localRank.push_back({id, record, expr.calculateRank(record), fPriority});
                    }
                }
            });
        }

        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        if (isCancelled && isCancelled()) return results;

        size_t totalFound = 0;
        size_t totalHydrated = 0;
        for (unsigned int t = 0; t < numThreads; ++t) {
            totalFound += threadCandidates[t].size();
            totalHydrated += threadHydrated[t].size();
        }

        candidates.reserve(totalFound);
        ranked.reserve(totalFound);
        hydratedStats.reserve(totalHydrated);
        for (unsigned int t = 0; t < numThreads; ++t) {
            candidates.insert(candidates.end(),
                              std::make_move_iterator(threadCandidates[t].begin()),
                              std::make_move_iterator(threadCandidates[t].end()));
            ranked.insert(ranked.end(),
                          std::make_move_iterator(threadRanked[t].begin()),
                          std::make_move_iterator(threadRanked[t].end()));
            hydratedStats.insert(hydratedStats.end(),
                                 threadHydrated[t].begin(),
                                 threadHydrated[t].end());
        }
    }
    const auto toSemanticTier = [](int folderPriority) noexcept -> int {
        if (folderPriority <= 1) return 4;   // Temp / Cache / System Build
        if (folderPriority <= 10) return 3;  // .lnk / .url Shortcut
        if (folderPriority <= 20) return 2;  // AppData / ProgramData
        if (folderPriority <= 80) return 1;  // WeChat / QQ / IM Chat History
        return 0;                            // Normal User Directory (D:\Chosen, Desktop, Documents, etc.)
    };
    const auto compareRank = [&expr, &toSemanticTier](const auto& a, const auto& b) {
        const int tierA = toSemanticTier(a.folderPriority);
        const int tierB = toSemanticTier(b.folderPriority);
        if (tierA != tierB) return tierA < tierB;
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
    std::vector<DWORDLONG> resultIds;
    resultIds.reserve(resultCount);

    for (size_t i = 0; i < resultCount; ++i) {
        const auto& candidate = ranked[i];
        results.push_back({
            std::wstring(candidate.record.fileName),
            buildFullPath(candidate.id),
            candidate.record.isDirectory,
            candidate.record.fileSize,
            candidate.record.creationTime,
            candidate.record.lastWriteTime,
            candidate.rank,
            candidate.folderPriority
        });
        resultIds.push_back(candidate.id);
    }

    // Never hold the index lock across filesystem I/O. The result owns its
    // strings and can safely be enriched after concurrent USN updates resume.
    lock.unlock();

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

    auto resolveFolderNode = [&](DWORDLONG parentFrn,
                                 std::unordered_map<DWORDLONG, CachedFolderNode>& folderCache) -> const CachedFolderNode& {
        auto it = folderCache.find(parentFrn);
        if (it != folderCache.end()) {
            return it->second;
        }

        CachedFolderNode node;
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

        DWORDLONG cur = parentFrn;
        for (size_t depth = 0; depth < 128; ++depth) {
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

    std::vector<std::vector<ContentCandidate>> threadBuckets(numThreads);
    const size_t chunkSize = (totalSlots + numThreads - 1) / numThreads;
    std::vector<std::thread> workers;
    workers.reserve(numThreads > 1 ? numThreads : 0);

    auto scanRange = [&](unsigned int bucketIdx, size_t startIdx, size_t endIdx) {
        auto& localOut = threadBuckets[bucketIdx];
        localOut.reserve((std::min<size_t>)((endIdx - startIdx) / 32 + 64, 16384));
        std::unordered_map<DWORDLONG, CachedFolderNode> localFolderCache;
        localFolderCache.reserve(2048);

        for (size_t i = startIdx; i < endIdx; ++i) {
            if (((i - startIdx) & 0x3FFu) == 0 && isCancelled && isCancelled()) {
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

            if (hasFileFilter) {
                if (filterIsFullySeparable) {
                    if (!fileFilterExpr.matchesSeparable(record, driveW, dirNode.dirClauseMask)) {
                        continue;
                    }
                } else {
                    auto getFullPath = [&]() -> std::wstring {
                        return buildFullPathNow();
                    };
                    if (!fileFilterExpr.matchesWithLazyPath(record, driveW, getFullPath)) {
                        continue;
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
            localOut.push_back(ContentCandidate{
                .fileName = std::wstring(record.fileName),
                .fullPath = std::move(finalPath),
                .fileSize = record.fileSize,
                .creationTime = record.creationTime,
                .lastWriteTime = record.lastWriteTime,
                .priorityScore = (std::max)(1.0, dirNode.basePriority + extOffset)
            });
        }
    };

    if (numThreads == 1) {
        scanRange(0, 0, totalSlots);
    } else {
        for (unsigned int t = 0; t < numThreads; ++t) {
            const size_t startIdx = t * chunkSize;
            const size_t endIdx = (std::min)(startIdx + chunkSize, totalSlots);
            if (startIdx >= endIdx) break;
            workers.emplace_back(scanRange, t, startIdx, endIdx);
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }

    lock.unlock();
    if (isCancelled && isCancelled()) return collected;

    size_t totalFound = 0;
    for (const auto& b : threadBuckets) {
        totalFound += b.size();
    }
    collected.reserve(totalFound);
    for (auto& b : threadBuckets) {
        collected.insert(
            collected.end(),
            std::make_move_iterator(b.begin()),
            std::make_move_iterator(b.end())
        );
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
    const auto* stored = m_Store.find(fileReferenceNumber);
    if (!stored) return L"";

    std::vector<std::wstring_view> parts;
    parts.reserve(16);
    DWORDLONG current = fileReferenceNumber;
    for (size_t depth = 0; depth < 512; ++depth) {
        const auto* node = m_Store.find(current);
        if (!node) break;
        const FileRecord rec = m_Store.view(*node);
        if (!rec.fileName.empty() && rec.fileName != L".") {
            parts.push_back(rec.fileName);
        }
        if (rec.parentFileReferenceNumber == current || rec.parentFileReferenceNumber == 0) break;
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
    return (cur != 0) ? cur : m_UsnJournalData.NextUsn;
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

    std::unique_lock lock(m_MapMutex);
    m_Store.clear();
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
    m_Store.releaseBuildScratch();

    m_UsnJournalData.NextUsn = lastUsn;
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

    const auto startUsn = static_cast<USN>(fromUsn);
    if (fromUsn == 0 || startUsn < m_UsnJournalData.LowestValidUsn || startUsn > m_UsnJournalData.NextUsn) {
        spdlog::warn("USN journal on drive {} is out of range (fromUsn: {}, lowest: {}, next: {}), triggering full rebuild",
                     m_DriveLetter, fromUsn, m_UsnJournalData.LowestValidUsn, m_UsnJournalData.NextUsn);
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
    rujd.UsnJournalID = m_UsnJournalData.UsnJournalID;

    std::vector<BYTE> buffer(BUF_LEN);
    DWORD bytesReturned = 0;
    bool anyChanged = false;

    while (DeviceIoControl(m_hVolume, FSCTL_READ_USN_JOURNAL, &rujd, sizeof(rujd), buffer.data(), BUF_LEN, &bytesReturned, NULL)) {
        if (bytesReturned < sizeof(USN)) break;

        USN* pNextUsn = reinterpret_cast<USN*>(buffer.data());
        rujd.StartUsn = *pNextUsn;
        m_CurrentUsn.store(rujd.StartUsn, std::memory_order_release);
        m_UsnJournalData.NextUsn = rujd.StartUsn;

        if (bytesReturned == sizeof(USN)) break;

        DWORD dwRetBytes = bytesReturned - sizeof(USN);
        const BYTE* pRecordBytes = buffer.data() + sizeof(USN);

        if (dwRetBytes > 0) {
            std::unique_lock lock(m_MapMutex);
            tools3000::service::ParsedUsnRecord parsed{};
            while (dwRetBytes > 0 && tools3000::service::parseUsnRecord(pRecordBytes, dwRetBytes, parsed)) {
                if (parsed.isValid) {
                    if (parsed.reason & USN_REASON_FILE_DELETE) {
                        anyChanged = m_Store.erase(parsed.fileReferenceNumber) || anyChanged;
                    } else if ((parsed.reason & USN_REASON_FILE_CREATE) ||
                               (parsed.reason & USN_REASON_RENAME_NEW_NAME)) {
                        if (!parsed.fileName.empty()) {
                            FileRecordInit record;
                            record.fileReferenceNumber = parsed.fileReferenceNumber;
                            record.parentFileReferenceNumber = parsed.parentFileReferenceNumber;
                            record.isDirectory = (parsed.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                            record.fileAttributes = parsed.fileAttributes;
                            record.lastWriteTime = parsed.timeStamp;
                            record.fileName = std::wstring(parsed.fileName);

                            m_Store.upsert(record);
                            anyChanged = true;
                        }
                    } else if (parsed.reason & USN_REASON_BASIC_INFO_CHANGE) {
                        const auto* existing = m_Store.find(parsed.fileReferenceNumber);
                        if (existing) {
                            FileRecord oldRec = m_Store.view(*existing);
                            FileRecordInit record;
                            record.fileReferenceNumber = parsed.fileReferenceNumber;
                            record.parentFileReferenceNumber = oldRec.parentFileReferenceNumber;
                            record.isDirectory = oldRec.isDirectory;
                            record.fileAttributes = parsed.fileAttributes;
                            record.fileSize = oldRec.fileSize;
                            record.creationTime = oldRec.creationTime;
                            record.lastWriteTime = parsed.timeStamp ? parsed.timeStamp : oldRec.lastWriteTime;
                            record.fileName = std::wstring(oldRec.fileName);

                            m_Store.upsert(record);
                            anyChanged = true;
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
