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
    uint8_t pathTier = tools3000::service::kPathTierUncomputed;
    uint64_t lastAccessTime = 0;
    uint32_t fileAttributes = 0;
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

/// 紧凑冷态待合并 USN 增量事件（仅保存真正改变索引的创建/删除/重命名/修改字段，几十 KB 即可缓存数千条变更）
struct PendingUsnEvent {
    uint64_t fileReferenceNumber = 0;
    uint64_t parentFileReferenceNumber = 0;
    DWORD reason = 0;
    DWORD fileAttributes = 0;
    uint64_t timeStamp = 0;
    std::wstring fileName;
};

bool parseUsnRecord(const BYTE* pBytes, DWORD remainingBytes, ParsedUsnRecord& out) noexcept;

}  // namespace tools3000::service

class MftParser {
public:
    MftParser();
    ~MftParser();

    bool Initialize(char driveLetter);
    void EnumerateFiles();
    void requestStop() noexcept {
        m_StopRequested.store(true, std::memory_order_release);
        m_StopHydration.store(true, std::memory_order_release);
    }
    void resetStopRequest() noexcept {
        m_StopRequested.store(false, std::memory_order_release);
        m_StopHydration.store(false, std::memory_order_release);
    }

    // USN Journal Monitoring
    void StartListening();
    void StopListening();

    // 冷态 USN 增量延迟合并缓冲（避免服务闲置修剪后，后台高频写盘瞬间将 180MB 主索引页全部 Fault 回工作集）
    static constexpr size_t kDefaultPendingUsnFlushLimit = 4096u;
    static constexpr uint64_t kDefaultPendingUsnMaxAgeMs = 15ULL * 60ULL * 1000ULL;

    void setColdUsnBatchingEnabled(bool enabled) noexcept {
        m_ColdUsnBatching.store(enabled, std::memory_order_release);
    }
    bool isColdUsnBatchingEnabled() const noexcept {
        return m_ColdUsnBatching.load(std::memory_order_acquire);
    }
    void setPendingUsnFlushPolicy(size_t maxEntries, uint64_t maxAgeMs) noexcept {
        m_PendingUsnFlushLimit.store(maxEntries > 0 ? maxEntries : 1u, std::memory_order_release);
        m_PendingUsnMaxAgeMs.store(maxAgeMs, std::memory_order_release);
    }
    size_t pendingUsnChangeCount() const noexcept {
        return m_PendingUsnCount.load(std::memory_order_acquire);
    }
    uint64_t totalPendingUsnFlushes() const noexcept {
        return m_PendingUsnFlushCount.load(std::memory_order_acquire);
    }
    /// 毫秒级批量合并待处理 USN 队列至主索引（热路径 0 锁原子预检；保证严格事件因果顺序与锁层次无死锁）
    size_t flushPendingUsnChanges();
    /// 安全兜底：若待合并队列最老条目已达到 maxAgeMs，自动触发批量合并
    bool flushPendingUsnChangesIfExpired(uint64_t nowTickMs = 0);
    /// 统一 USN 事件批处理入口：冷态入队攒批（达上限或超龄自动合并），热态即时合并
    void processUsnEventBatch(std::vector<tools3000::service::PendingUsnEvent>&& events, uint64_t nowTickMs = 0);

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

    // 多趟流式快照访问：在单次 m_MapMutex 共享锁内执行回调，使 DatabaseManager::saveSnapshot
    // 以固定 4 MiB 缓冲直接分块落盘，彻底消除 300 万文件下 150-250 MB 临时堆峰值。
    using StoreSnapshotCallback = std::function<bool(
        const tools3000::service::FileIndexStore& store,
        uint64_t lastUsn,
        uint32_t volumeSerial)>;
    bool withSnapshotLock(const StoreSnapshotCallback& fn) const;

    // 导入同理走拉取式回调：填充 out 并返回 true 表示还有下一条。调用方可以复用
    // 同一个 out，于是整个恢复过程只存在一份记录的临时字符串。
    using SnapshotProducer = std::function<bool(FileRecordInit& out)>;
    bool importSnapshot(const SnapshotProducer& next, size_t expectedRecords,
                        uint64_t lastUsn, uint32_t volumeSerial);
    bool importSnapshot(std::vector<FileRecordInit>&& records, uint64_t lastUsn, uint32_t volumeSerial);
    bool catchUpUsnJournal(uint64_t fromUsn);
    bool consumeRescannedDuringCatchUp() noexcept {
        return m_RescannedDuringCatchUp.exchange(false, std::memory_order_acq_rel);
    }
    bool isInitialScanDone() const noexcept {
        return m_InitialScanDone.load(std::memory_order_acquire);
    }

    enum class ScanPhase : uint8_t {
        Idle = 0,
        Scanning = 1,
        BuildingHierarchy = 2,
        Syncing = 3,
        LoadingSnapshot = 4,
    };

    ScanPhase getScanPhase() const noexcept {
        return static_cast<ScanPhase>(m_ScanPhase.load(std::memory_order_acquire));
    }
    uint64_t getScannedRecords() const noexcept {
        return m_ScannedRecords.load(std::memory_order_acquire);
    }
    uint64_t getEstimatedRecords() const noexcept {
        return m_EstimatedRecords.load(std::memory_order_acquire);
    }
    const std::wstring& getVolumeLabel() const noexcept {
        return m_VolumeLabel;
    }

    char getDriveLetter() const { return m_DriveLetter; }
    UINT getDriveType() const { return m_DriveType; }
    bool isRotationalDisk() const noexcept { return m_IsRotationalDisk; }
    uint32_t getPhysicalDiskNumber() const noexcept { return m_PhysicalDiskNumber; }
    bool hasPhysicalDiskNumber() const noexcept { return m_HasPhysicalDiskNumber; }
    uint64_t getCurrentUsn() const;
    uint32_t getVolumeSerialNumber() const;
    size_t getFileCount() const;
    uint64_t getIndexGeneration() const noexcept {
        return m_IndexGeneration.load(std::memory_order_acquire);
    }
    uint64_t getApproximateIndexBytes() const;
    void releaseQueryCaches();
    void shrinkIndexCapacity();
    size_t getLastSearchPathBuildCount() const noexcept {
        return m_LastSearchPathBuildCount.load(std::memory_order_relaxed);
    }
    void setRealTimeValidation(bool enable) noexcept { m_ValidateExistence.store(enable, std::memory_order_release); }
    bool isRealTimeValidationEnabled() const noexcept {
        return m_ValidateExistence.load(std::memory_order_acquire) ||
               (m_IsListening.load(std::memory_order_acquire) && m_hVolume != INVALID_HANDLE_VALUE);
    }

private:
    char m_DriveLetter = 0;
    UINT m_DriveType = DRIVE_UNKNOWN;
    HANDLE m_hVolume = INVALID_HANDLE_VALUE;
    mutable std::mutex m_UsnMutex;
    USN_JOURNAL_DATA_V0 m_UsnJournalData{};
    std::atomic<uint64_t> m_CurrentUsn{0};
    std::atomic<bool> m_ValidateExistence{false};
    std::atomic<bool> m_InitialScanDone{false};

    // Listener & Low-priority Background Hydration Threads
    std::atomic<bool> m_IsListening{false};
    std::atomic<bool> m_StopRequested{false};
    std::atomic<bool> m_StopHydration{false};
    std::unique_ptr<std::thread> m_ListenerThread;
    std::unique_ptr<std::thread> m_HydrationThread;
    void UsnListenerLoop();
    void BackgroundHydrateLoop();

    // Memory structures
    mutable std::shared_mutex m_MapMutex;
    mutable std::atomic<size_t> m_LastSearchPathBuildCount{0};
    tools3000::service::FileIndexStore m_Store;
    std::atomic<size_t> m_FileCount{0};
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
    void clearPendingUsnQueue();
    size_t applyUsnEventBatchLocked(const std::vector<tools3000::service::PendingUsnEvent>& events);
    std::wstring buildFullPath(DWORDLONG fileReferenceNumber) const;

    // 冷态 USN 攒批队列（锁顺序铁律：m_UsnApplyMutex -> m_PendingUsnMutex(短持 swap 即放) -> m_MapMutex -> m_SearchCacheMutex）
    std::atomic<bool> m_ColdUsnBatching{false};
    std::atomic<size_t> m_PendingUsnFlushLimit{kDefaultPendingUsnFlushLimit};
    std::atomic<uint64_t> m_PendingUsnMaxAgeMs{kDefaultPendingUsnMaxAgeMs};
    std::atomic<size_t> m_PendingUsnCount{0};
    std::atomic<uint64_t> m_PendingUsnFlushCount{0};
    mutable std::mutex m_UsnApplyMutex;
    mutable std::mutex m_PendingUsnMutex;
    std::vector<tools3000::service::PendingUsnEvent> m_PendingUsnQueue;
    uint64_t m_PendingUsnFirstQueuedTick{0};
    bool queryFrnDiskMetadata(
        uint64_t frn,
        const std::wstring& fallbackPath,
        uint64_t& outFileSize,
        uint64_t& outCreationTime,
        uint64_t& outLastWriteTime
    ) const noexcept;

    bool m_IsFallbackDirectoryWalk{false};
    bool m_Initialized{false};
    bool m_IsRotationalDisk{false};
    bool m_HasPhysicalDiskNumber{false};
    uint32_t m_PhysicalDiskNumber{0};
    std::atomic<bool> m_RescannedDuringCatchUp{false};
    uint32_t m_VolumeSerial{0};
    std::wstring m_VolumeLabel;
    std::atomic<uint8_t> m_ScanPhase{static_cast<uint8_t>(ScanPhase::Idle)};
    std::atomic<uint64_t> m_ScannedRecords{0};
    std::atomic<uint64_t> m_EstimatedRecords{0};
};
