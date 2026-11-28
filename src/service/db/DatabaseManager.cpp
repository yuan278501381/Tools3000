#include "DatabaseManager.h"
#include "common/AtomicFile.h"
#include "../MftParser.h"
#include "../SafeMappedRead.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <limits>

namespace tools3000::service::db {

namespace {

struct ScopedHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ScopedHandle() {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
};

struct ScopedMappedView {
    const void* ptr = nullptr;
    ~ScopedMappedView() {
        if (ptr) {
            UnmapViewOfFile(ptr);
        }
    }
    ScopedMappedView() = default;
    explicit ScopedMappedView(const void* p) : ptr(p) {}
    ScopedMappedView(const ScopedMappedView&) = delete;
    ScopedMappedView& operator=(const ScopedMappedView&) = delete;
};

wchar_t toUpperDriveW(wchar_t ch) noexcept {
    return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)));
}

wchar_t toUpperDriveChar(char ch) noexcept {
    return static_cast<wchar_t>(
        std::towupper(static_cast<wint_t>(static_cast<unsigned char>(ch))));
}

std::wstring getDefaultDbPath() {
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\Tools3000";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\Tools3000.db";
    }
    return L".\\Tools3000.db";
}

bool writeAll(HANDLE file, const void* data, std::size_t bytes) {
    if (file == INVALID_HANDLE_VALUE || (bytes > 0 && data == nullptr)) return false;
    const auto* cursor = static_cast<const std::byte*>(data);
    constexpr std::size_t WriteChunkBytes = 16u * 1024u * 1024u;
    std::size_t remaining = bytes;
    while (remaining > 0) {
        const DWORD requested = static_cast<DWORD>((std::min)(remaining, WriteChunkBytes));
        DWORD written = 0;
        if (!WriteFile(file, cursor, requested, &written, nullptr) || written == 0) return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager s_mgr;
    return s_mgr;
}

DatabaseManager::DatabaseManager() {
    init();
}

DatabaseManager::~DatabaseManager() {
}

void DatabaseManager::init(const std::wstring& customDbPath) {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    if (!customDbPath.empty()) {
        m_dbPath = customDbPath;
    } else if (m_dbPath.empty()) {
        m_dbPath = getDefaultDbPath();
    }
}

std::wstring DatabaseManager::getDbPath() const {
    std::lock_guard<std::mutex> lock(m_pathMutex);
    return m_dbPath;
}

DbStats DatabaseManager::getStats() const {
    const std::wstring dbPath = getDbPath();
    DbStats stats;
    stats.dbPath = dbPath;
    if (dbPath.empty()) return stats;

    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(dbPath.c_str(), GetFileExInfoStandard, &attr)) {
        stats.exists = true;
        stats.fileSize = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;

        ULARGE_INTEGER uli;
        uli.LowPart = attr.ftLastWriteTime.dwLowDateTime;
        uli.HighPart = attr.ftLastWriteTime.dwHighDateTime;
        stats.timestamp = uli.QuadPart;

        // 快速读取头部的 totalRecords（不与 loadSnapshot/saveSnapshot 争抢 m_ioMutex，杜绝阻塞 getDbStats IPC）
        ScopedHandle hFile(CreateFileW(
            dbPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr));
        if (hFile.handle != INVALID_HANDLE_VALUE) {
            DbHeader header{};
            DWORD bytesRead = 0;
            if (ReadFile(hFile.handle, &header, sizeof(header), &bytesRead, nullptr) && bytesRead == sizeof(header)) {
                const bool validV2 = header.magic == 0x3230424459534145ULL && header.version == 2;
                const bool validV1 = header.magic == 0x3130424459534145ULL && header.version == 1;
                if (validV2 || validV1) {
                    stats.totalRecords = header.totalRecords;
                    stats.volumeCount = header.volumeCount;
                }
            }
        }
    }
    return stats;
}

bool DatabaseManager::saveSnapshot(const std::vector<MftParser*>& parsers) {
    std::lock_guard<std::mutex> ioLock(m_ioMutex);
    const std::wstring dbPath = getDbPath();
    if (dbPath.empty() || parsers.empty()) return false;

    // `MED-14`: Never persist a partial/in-progress index snapshot before every active
    // volume parser has finished its initial full scan (or loaded a valid snapshot).
    bool hasValidParser = false;
    for (auto* parser : parsers) {
        if (!parser) continue;
        hasValidParser = true;
        if (!parser->isInitialScanDone()) {
            return false;
        }
    }
    if (!hasValidParser) return false;

    // 落盘快照前强制合并各卷冷态待处理 USN 变更，确保快照内容与推进后的 lastUsn 严格一致
    for (auto* parser : parsers) {
        if (parser) {
            parser->flushPendingUsnChanges();
        }
    }

    // Reparse-point defense (`CRIT-05`): verify parent chain and target file are non-reparse.
    const size_t lastSlash = dbPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        const std::wstring parentDir = dbPath.substr(0, lastSlash);
        if (!parentDir.empty() && !tools3000::common::ensureDirectorySafeNoReparse(parentDir)) {
            return false;
        }
    }
    if (tools3000::common::pathContainsReparsePoint(dbPath)) {
        return false;
    }

    static std::atomic<uint64_t> s_tmpSequence{0};
    const uint64_t seq = s_tmpSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::wstring tmpPath = dbPath + L"." + std::to_wstring(GetCurrentProcessId()) +
                                 L"_" + std::to_wstring(GetTickCount64()) +
                                 L"_" + std::to_wstring(seq) + L".tmp";
    if (tools3000::common::pathContainsReparsePoint(tmpPath)) {
        return false;
    }
    HANDLE hFile = CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (GetFileType(hFile) != FILE_TYPE_DISK) {
        CloseHandle(hFile);
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION tmpInfo{};
    if (!GetFileInformationByHandle(hFile, &tmpInfo) ||
        (tmpInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (tmpInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(hFile);
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    DbHeader header{};
    header.magic = 0x3230424459534145ULL; // 'EASYDB02'
    header.version = 2;
    header.volumeCount = 0;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    header.timestamp = uli.QuadPart;
    header.totalRecords = 0;

    // 先写入占位的 Header
    const auto failSave = [&]() {
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
        DeleteFileW(tmpPath.c_str());
        return false;
    };
    if (!writeAll(hFile, &header, sizeof(header))) return failSave();

    uint64_t totalRecordsAll = 0;

    for (auto* parser : parsers) {
        if (!parser) continue;

        const bool isHdd = parser->isRotationalDisk();
        uint64_t volumeRecords = 0;
        const bool volumeOk = parser->withSnapshotLock(
            [&](const tools3000::service::FileIndexStore& store,
                uint64_t lastUsn,
                uint32_t volumeSerial) -> bool {
                // Pass 1: 零堆分配统计记录数与宽字符池总字节数（300 万条耗时 < 8ms）
                uint32_t recCount = 0;
                uint64_t totalWChars = 0;
                const size_t slots = store.slotCount();
                for (size_t i = 0; i < slots; ++i) {
                    const auto* rec = store.at(i);
                    if (!rec) continue;
                    const uint16_t nLen = rec->nameLength();
                    if (recCount == (std::numeric_limits<uint32_t>::max)() ||
                        totalWChars > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)() / sizeof(wchar_t)) - nLen) {
                        return false;
                    }
                    ++recCount;
                    totalWChars += nLen;
                }

                DbVolumeHeader volHeader{};
                volHeader.driveLetter = toUpperDriveChar(parser->getDriveLetter());
                volHeader.volumeSerial = volumeSerial;
                volHeader.lastUsn = lastUsn;
                volHeader.recordCount = recCount;
                volHeader.stringPoolBytes = static_cast<uint32_t>(totalWChars * sizeof(wchar_t));

                if (!writeAll(hFile, &volHeader, sizeof(volHeader))) {
                    return false;
                }

                // Pass 2: 固定 2 MiB 分块流式写出 StringPool（消除 100+ MB 整卷字符串池峰值）
                static constexpr size_t kStringChunkWChars = 1024u * 1024u;  // 2 MiB
                // 机械盘后台快照平滑限速：每写 4 MiB 让出 12ms，避免 5400/7200rpm 机械盘队列飙到 90%+ 卡顿
                static constexpr uint64_t kHddYieldIntervalBytes = 4ULL * 1024ULL * 1024ULL;
                static constexpr DWORD kHddYieldSleepMs = 12;
                std::wstring stringChunk;
                stringChunk.reserve(kStringChunkWChars + 1024u);
                uint64_t hddWrittenSinceYield = 0;

                auto flushStringChunk = [&]() -> bool {
                    if (stringChunk.empty()) return true;
                    const size_t bytes = stringChunk.size() * sizeof(wchar_t);
                    if (!writeAll(hFile, stringChunk.data(), bytes)) return false;
                    stringChunk.clear();
                    if (isHdd) {
                        hddWrittenSinceYield += bytes;
                        if (hddWrittenSinceYield >= kHddYieldIntervalBytes) {
                            hddWrittenSinceYield = 0;
                            Sleep(kHddYieldSleepMs);
                        }
                    }
                    return true;
                };

                for (size_t i = 0; i < slots; ++i) {
                    const auto* rec = store.at(i);
                    if (!rec) continue;
                    store.appendNameTo(*rec, stringChunk);
                    if (stringChunk.size() >= kStringChunkWChars) {
                        if (!flushStringChunk()) return false;
                    }
                }
                if (!flushStringChunk()) return false;
                stringChunk.shrink_to_fit();

                // Pass 3: 固定 ~2 MiB 分块流式写出 DbRecordPod（消除 140+ MB 整卷 POD 峰值）
                constexpr size_t kPodChunkCount = 43690u;  // ~2 MiB
                std::vector<DbRecordPod> podChunk;
                podChunk.reserve(kPodChunkCount);
                uint32_t runningNameOffset = 0;

                auto flushPodChunk = [&]() -> bool {
                    if (podChunk.empty()) return true;
                    const size_t bytes = podChunk.size() * sizeof(DbRecordPod);
                    if (!writeAll(hFile, podChunk.data(), bytes)) return false;
                    podChunk.clear();
                    if (isHdd) {
                        hddWrittenSinceYield += bytes;
                        if (hddWrittenSinceYield >= kHddYieldIntervalBytes) {
                            hddWrittenSinceYield = 0;
                            Sleep(kHddYieldSleepMs);
                        }
                    }
                    return true;
                };

                for (size_t i = 0; i < slots; ++i) {
                    const auto* rec = store.at(i);
                    if (!rec) continue;
                    DbRecordPod pod{};
                    pod.frn = rec->frn;
                    pod.parentFrn = rec->parentFrn;
                    pod.attributes = static_cast<uint32_t>(rec->attributes16);
                    pod.isDirectory = rec->isDirectory() ? 1 : 0;
                    pod.fileSize = store.fileSizeOf(*rec);
                    pod.creationTime = store.creationTimeOf(*rec);
                    pod.lastWriteTime = store.lastWriteTimeOf(*rec);
                    pod.nameOffset = runningNameOffset;
                    pod.nameLen = rec->nameLength();
                    runningNameOffset += pod.nameLen;
                    podChunk.push_back(pod);
                    if (podChunk.size() >= kPodChunkCount) {
                        if (!flushPodChunk()) return false;
                    }
                }
                if (!flushPodChunk()) return false;

                volumeRecords = recCount;
                return true;
            });

        if (!volumeOk) {
            return failSave();
        }
        totalRecordsAll += volumeRecords;
        ++header.volumeCount;
    }

    // 回填更新 Header.totalRecords
    header.totalRecords = totalRecordsAll;
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(hFile, start, nullptr, FILE_BEGIN) ||
        !writeAll(hFile, &header, sizeof(header)) ||
        !FlushFileBuffers(hFile)) {
        return failSave();
    }
    if (!CloseHandle(hFile)) {
        hFile = INVALID_HANDLE_VALUE;
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    hFile = INVALID_HANDLE_VALUE;

    // 原子替换
    if (!MoveFileExW(tmpPath.c_str(), dbPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

bool DatabaseManager::loadSnapshot(std::vector<MftParser*>& parsers) {
    const std::wstring dbPath = getDbPath();
    if (dbPath.empty() || parsers.empty()) return false;

    std::vector<std::pair<MftParser*, uint64_t>> pendingCatchUps;
    bool anyVolumeLoaded = false;

    {
        std::lock_guard<std::mutex> ioLock(m_ioMutex);
        ScopedHandle hFile(CreateFileW(
            dbPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        ));

        if (hFile.handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(hFile.handle, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(DbHeader))) {
            return false;
        }
        if (static_cast<uint64_t>(fileSize.QuadPart) > (std::numeric_limits<size_t>::max)()) {
            return false;
        }

        ScopedHandle hMapping(CreateFileMappingW(hFile.handle, nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!hMapping.handle) {
            return false;
        }

        const size_t totalBytes = static_cast<size_t>(fileSize.QuadPart);
        ScopedMappedView mappedView(MapViewOfFile(hMapping.handle, FILE_MAP_READ, 0, 0, totalBytes));
        const auto* pData = static_cast<const uint8_t*>(mappedView.ptr);
        if (!pData) {
            return false;
        }

        DbHeader header{};
        if (!tools3000::service::safeMappedMemcpy(&header, pData, sizeof(header))) {
            return false;
        }

        // 校验魔数与版本
        const bool validV2 = header.magic == 0x3230424459534145ULL && header.version == 2;
        const bool validV1 = header.magic == 0x3130424459534145ULL && header.version == 1;
        if (!validV2 && !validV1) {
            return false;
        }

        size_t cursor = sizeof(DbHeader);
        pendingCatchUps.reserve(header.volumeCount);

        for (uint32_t v = 0; v < header.volumeCount; ++v) {
            if (cursor > totalBytes || sizeof(DbVolumeHeader) > totalBytes - cursor) break;
            DbVolumeHeader volume{};
            if (!tools3000::service::safeMappedMemcpy(&volume, pData + cursor, sizeof(volume))) {
                break;
            }
            cursor += sizeof(DbVolumeHeader);

            const size_t stringPoolBytes = volume.stringPoolBytes;
            if (stringPoolBytes % sizeof(wchar_t) != 0 ||
                cursor > totalBytes || stringPoolBytes > totalBytes - cursor) {
                break;
            }
            const size_t afterStrings = cursor + stringPoolBytes;
            const size_t availableForPods = totalBytes - afterStrings;
            if (volume.recordCount > availableForPods / sizeof(DbRecordPod)) break;
            const size_t podsBytes = static_cast<size_t>(volume.recordCount) * sizeof(DbRecordPod);

            const uint8_t* pStringBytes = pData + cursor;
            cursor += stringPoolBytes;

            const auto* pPodBytes = pData + cursor;
            cursor += podsBytes;

            const size_t totalWChars = stringPoolBytes / sizeof(wchar_t);

            // 查找对应盘符的 MftParser
            const wchar_t drive = toUpperDriveW(volume.driveLetter);
            MftParser* targetParser = nullptr;
            for (auto* parser : parsers) {
                if (parser && toUpperDriveChar(parser->getDriveLetter()) == drive) {
                    targetParser = parser;
                    break;
                }
            }

            if (!targetParser) continue;

            // 单趟流式读取并内联边界校验，消除 300 万级记录下的冗余双趟内存扫描，冷启动提速 2x+
            uint32_t podCursor = 0;
            bool pageReadFault = false;

            auto produce = [&](FileRecordInit& r) {
                if (podCursor >= volume.recordCount) return false;
                DbRecordPod pod{};
                if (!tools3000::service::safeMappedMemcpy(
                        &pod,
                        pPodBytes + static_cast<size_t>(podCursor++) * sizeof(DbRecordPod),
                        sizeof(pod))) {
                    pageReadFault = true;
                    return false;
                }
                if (pod.nameOffset > totalWChars || pod.nameLen > totalWChars - pod.nameOffset) {
                    pageReadFault = true;
                    return false;
                }
                r.fileReferenceNumber = pod.frn;
                r.parentFileReferenceNumber = pod.parentFrn;
                r.fileAttributes = pod.attributes;
                r.isDirectory = (pod.isDirectory != 0);
                r.fileSize = pod.fileSize;
                r.creationTime = pod.creationTime;
                r.lastWriteTime = pod.lastWriteTime;

                r.fileName.resize(pod.nameLen);
                if (pod.nameLen > 0) {
                    if (!tools3000::service::safeMappedMemcpy(
                            r.fileName.data(),
                            pStringBytes + static_cast<size_t>(pod.nameOffset) * sizeof(wchar_t),
                            static_cast<size_t>(pod.nameLen) * sizeof(wchar_t))) {
                        pageReadFault = true;
                        return false;
                    }
                }
                return true;
            };

            if (targetParser->importSnapshot(produce, volume.recordCount, volume.lastUsn, volume.volumeSerial) &&
                !pageReadFault) {
                pendingCatchUps.emplace_back(targetParser, volume.lastUsn);
                anyVolumeLoaded = true;
            }
        }
    }

    // 在释放文件映射句柄与 m_ioMutex 之后再执行各卷 USN 增量追赶，避免锁持有跨卷 I/O
    for (const auto& [parser, lastUsn] : pendingCatchUps) {
        if (parser) {
            parser->catchUpUsnJournal(lastUsn);
            parser->shrinkIndexCapacity();
        }
    }

    return anyVolumeLoaded;
}

} // namespace tools3000::service::db
