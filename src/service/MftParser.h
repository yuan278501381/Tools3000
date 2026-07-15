#pragma once
#include "FileIndexStore.h"
#include "SearchExpression.h"
#include "content/IContentExtractor.h"
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <utility>

struct SearchResult {
    std::wstring fileName;
    std::wstring fullPath;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
    int matchRank = 7;
    int folderPriority = 200;
};

struct SearchExcludeOptions {
    std::vector<std::wstring> patterns;
    bool excludeHidden = false;
    bool excludeSystem = false;
};

struct ContentCandidate {
    std::wstring fileName;
    std::wstring fullPath;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
    double priorityScore = 0.0;
};

struct ContentCandidateOptions {
    SearchExcludeOptions excludeOpts;
    const tools3000::service::content::ContentFormatFilter* formatFilter = nullptr;
    std::vector<std::pair<std::wstring, double>> dialogPriorities;
    std::wstring userProfileDirLower;
    std::wstring desktopDirLower;
    std::wstring documentsDirLower;
    std::wstring downloadsDirLower;
    size_t maxCandidates = 300000;
};

namespace tools3000::service {

struct ParsedUsnRecord {
    uint64_t fileReferenceNumber = 0;
    uint64_t parentFileReferenceNumber = 0;
    DWORD reason = 0;
    DWORD fileAttributes = 0;
    uint64_t timeStamp = 0;
    std::wstring_view fileName;
    DWORD recordLength = 0;
    WORD majorVersion = 0;
    bool isValid = false;
};

bool parseUsnRecord(const BYTE* pBytes, DWORD remainingBytes, ParsedUsnRecord& out) noexcept;

}  // namespace tools3000::service

class MftParser {
public:
    MftParser();
    ~MftParser();

    bool Initialize(char driveLetter);
    void EnumerateFiles();
    void requestStop() noexcept { m_StopRequested.store(true, std::memory_order_release); }
    void resetStopRequest() noexcept { m_StopRequested.store(false, std::memory_order_release); }

    // USN Journal Monitoring
    void StartListening();
    void StopListening();

    // Quick search
    using CancellationCheck = std::function<bool()>;
    std::vector<SearchResult> Search(const std::wstring& query, int limit = 100,
                                     const SearchExcludeOptions& excludeOpts = {},
                                     const CancellationCheck& isCancelled = {},
                                     const SearchParseOptions& parseOpts = {});

    // Zero-disk-I/O in-memory candidate collection dedicated to full-text content search
    std::vector<ContentCandidate> CollectContentCandidates(
        const SearchExpression& fileFilterExpr,
        const ContentCandidateOptions& options = {},
        const CancellationCheck& isCancelled = {}
    ) const;

    // 快照导出与载入接口
    //
    // 导出为流式访问：全盘索引有数百万条记录，物化成第二份 vector 会让保存快照
    // 的瞬间内存翻倍，因此改由调用方逐条消费。
    using SnapshotVisitor = std::function<void(const FileRecord&)>;
    void exportSnapshot(const SnapshotVisitor& visit, uint64_t& outLastUsn, uint32_t& outVolumeSerial) const;

    // 导入同理走拉取式回调：填充 out 并返回 true 表示还有下一条。调用方可以复用
    // 同一个 out，于是整个恢复过程只存在一份记录的临时字符串。
    using SnapshotProducer = std::function<bool(FileRecordInit& out)>;
    bool importSnapshot(const SnapshotProducer& next, size_t expectedRecords,
                        uint64_t lastUsn, uint32_t volumeSerial);
    bool importSnapshot(std::vector<FileRecordInit>&& records, uint64_t lastUsn, uint32_t volumeSerial);
    bool catchUpUsnJournal(uint64_t fromUsn);

    char getDriveLetter() const { return m_DriveLetter; }
    UINT getDriveType() const { return m_DriveType; }
    uint64_t getCurrentUsn() const;
    uint32_t getVolumeSerialNumber() const;
    size_t getFileCount() const;
    uint64_t getApproximateIndexBytes() const;
    void setRealTimeValidation(bool enable) noexcept { m_ValidateExistence.store(enable, std::memory_order_release); }
    bool isRealTimeValidationEnabled() const noexcept {
        return m_ValidateExistence.load(std::memory_order_acquire) ||
               (m_IsListening.load(std::memory_order_acquire) && m_hVolume != INVALID_HANDLE_VALUE);
    }

private:
    char m_DriveLetter = 0;
    UINT m_DriveType = DRIVE_UNKNOWN;
    HANDLE m_hVolume = INVALID_HANDLE_VALUE;
    USN_JOURNAL_DATA_V0 m_UsnJournalData{};
    std::atomic<uint64_t> m_CurrentUsn{0};
    std::atomic<bool> m_ValidateExistence{false};

    // Listener Thread
    std::atomic<bool> m_IsListening{false};
    std::atomic<bool> m_StopRequested{false};
    std::unique_ptr<std::thread> m_ListenerThread;
    void UsnListenerLoop();

    // Memory structures
    mutable std::shared_mutex m_MapMutex;
    tools3000::service::FileIndexStore m_Store;
    std::atomic<uint64_t> m_IndexGeneration{0};
    std::mutex m_SearchCacheMutex;
    std::wstring m_CachedQuery;
    std::vector<DWORDLONG> m_CachedCandidates;
    uint64_t m_CachedGeneration = 0;
    bool m_CachedExcludeHidden = false;
    bool m_CachedExcludeSystem = false;
    std::vector<std::wstring> m_CachedExcludePatterns;
    SearchParseOptions m_CachedParseOptions{};

    bool QueryUsnJournal();
    void EnumerateFilesViaDirectoryWalk(char driveLetter);
    std::wstring buildFullPath(DWORDLONG fileReferenceNumber) const;

    bool m_IsFallbackDirectoryWalk{false};
    bool m_Initialized{false};
    uint32_t m_VolumeSerial{0};
};
