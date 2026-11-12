#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cwctype>

namespace tools3000::service::db {

struct RunRecord {
    std::wstring filename;
    uint32_t runCount = 0;
    uint64_t lastRunDate = 0; // Windows FILETIME
};

struct RunEntryStats {
    uint32_t runCount = 0;
    double frecencyScore = 0.0;
};

struct RunHistorySnapshot {
    std::unordered_map<std::wstring, RunEntryStats> entries;
    uint64_t lenMask = 0;
    uint64_t bloomMask = 0;

    bool empty() const noexcept { return entries.empty(); }

    static wchar_t foldPathChar(wchar_t ch) noexcept {
        if (ch == L'/') {
            return L'\\';
        }
        if (ch < 0x80) {
            return (ch >= L'A' && ch <= L'Z')
                ? static_cast<wchar_t>(ch + (L'a' - L'A'))
                : ch;
        }
        if (ch >= 0x3000 && ch <= 0x9FFF) {
            return ch;
        }
        wchar_t folded = ch;
        CharLowerBuffW(&folded, 1);
        return folded;
    }

    static void computeBloomBits(std::wstring_view path, uint64_t& outLenBit, uint64_t& outBloomBits) noexcept {
        const size_t n = path.size();
        outLenBit = 1ULL << (n & 63);
        if (n == 0) {
            outBloomBits = 1ULL;
            return;
        }
        const uint64_t cLast = static_cast<uint64_t>(foldPathChar(path[n - 1]));
        const uint64_t cMid = static_cast<uint64_t>(foldPathChar(path[n >= 6 ? (n - 6) : 0]));
        const uint64_t b1 = 1ULL << (((n * 131ULL) ^ cLast) & 63ULL);
        const uint64_t b2 = 1ULL << (((n * 31ULL) ^ (cMid * 17ULL)) & 63ULL);
        outBloomBits = b1 | b2;
    }

    RunEntryStats lookup(std::wstring_view fullPath) const {
        if (entries.empty()) return {};
        uint64_t lenBit = 0;
        uint64_t bloomBits = 0;
        computeBloomBits(fullPath, lenBit, bloomBits);
        if ((lenMask & lenBit) == 0 || (bloomMask & bloomBits) != bloomBits) {
            return {};
        }
        std::wstring key;
        key.reserve(fullPath.size());
        for (wchar_t ch : fullPath) {
            key.push_back(foldPathChar(ch));
        }
        auto it = entries.find(key);
        return (it != entries.end()) ? it->second : RunEntryStats{};
    }

    uint32_t getRunCount(std::wstring_view fullPath) const {
        return lookup(fullPath).runCount;
    }
};

class RunHistoryManager {
public:
    static constexpr size_t MaxRunHistoryEntries = 10000;
    static constexpr uint32_t FlushDebounceMs = 350;
    static constexpr uint32_t MaxFlushDelayMs = 1500;

    static RunHistoryManager& instance();

    RunHistoryManager();
    ~RunHistoryManager();

    // 初始化历史文件路径 (默认使用本地目录或测试指定路径)
    void init(const std::wstring& customCsvPath = L"");
    void initForSid(const std::wstring& sid);

    // 绑定已通过命名管道内核鉴权的客户端令牌，定位并隔离至该客户端真实的
    // %LOCALAPPDATA%\Tools3000\service_user_data\Run History.csv，并在客户端低权限令牌模拟上下文中读写。
    bool bindClientUserContext(
        HANDLE clientToken,
        bool isSameUserProcessSid,
        const std::wstring& clientSid
    );

    // 记录运行/打开事件
    void recordRun(const std::wstring& filename);

    // 查询指定文件的运行次数与最后运行时间
    uint32_t getRunCount(const std::wstring& filename) const;
    uint64_t getLastRunDate(const std::wstring& filename) const;

    // Frecency 智能相关度评分 (结合频次与时效)
    double calculateFrecencyScore(const std::wstring& filename, uint64_t currentFileTime = 0) const;

    // 捕获只读无锁快照（单次加锁 + Bloom 预筛，消除万级结果集循环中重复加锁与字符串归一化分配）
    RunHistorySnapshot captureSnapshot(uint64_t currentFileTime = 0) const;

    // 获取高频运行列表 (按 Frecency 降序)
    std::vector<RunRecord> getTopRuns(size_t limit = 50) const;

    // 磁盘持久化与载入
    bool load();
    bool save();
    bool flush();
    void clear();

    void setMaxEntries(size_t maxEntries);
    size_t maxEntries() const;

    // 获取当前记录总数
    size_t size() const;
    std::wstring getFilePath() const;

    // 获取当前系统 FILETIME
    static uint64_t getCurrentFileTime();

private:
    std::wstring getNormalizedKey(const std::wstring& path) const;
    void closeImpersonationTokenUnlocked() noexcept;
    void evictOverflowUnlocked(uint64_t nowFt);
    void scheduleAsyncFlush();
    void stopFlushWorker() noexcept;
    void asyncFlushWorkerLoop();

    mutable std::recursive_mutex m_mutex;
    std::wstring m_csvPath;
    std::wstring m_boundClientSid;
    HANDLE m_impersonationToken = nullptr;
    bool m_requiresImpersonation = false;
    std::unordered_map<std::wstring, RunRecord> m_records;
    size_t m_maxEntries = MaxRunHistoryEntries;
    bool m_isDirty = false;

    std::mutex m_flushMutex;
    std::condition_variable m_flushCv;
    std::thread m_flushThread;
    std::chrono::steady_clock::time_point m_firstDirtyTick{};
    std::chrono::steady_clock::time_point m_lastDirtyTick{};
    bool m_flushPending = false;
    bool m_flushWorkerRunning = false;
    bool m_stopFlushWorker = false;
};

} // namespace tools3000::service::db
