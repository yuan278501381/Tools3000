#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
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

    static void computeBloomBits(std::wstring_view path, uint64_t& outLenBit, uint64_t& outBloomBits) noexcept {
        const size_t n = path.size();
        outLenBit = 1ULL << (n & 63);
        if (n == 0) {
            outBloomBits = 1ULL;
            return;
        }
        const auto foldChar = [](wchar_t ch) noexcept -> uint64_t {
            if (ch == L'/') return static_cast<uint64_t>(L'\\');
            if (ch >= L'A' && ch <= L'Z') return static_cast<uint64_t>(ch + (L'a' - L'A'));
            return static_cast<uint64_t>(ch);
        };
        const uint64_t cLast = foldChar(path[n - 1]);
        const uint64_t cMid = foldChar(path[n >= 6 ? (n - 6) : 0]);
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
            key.push_back(ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch)));
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
    static RunHistoryManager& instance();

    RunHistoryManager();
    ~RunHistoryManager();

    // 初始化历史文件路径 (默认使用 %APPDATA%\Tools3000\Run History.csv 或当前执行目录)
    void init(const std::wstring& customCsvPath = L"");
    void initForSid(const std::wstring& sid);

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
    void clear();

    // 获取当前记录总数
    size_t size() const;
    std::wstring getFilePath() const;

    // 获取当前系统 FILETIME
    static uint64_t getCurrentFileTime();

private:
    std::wstring getNormalizedKey(const std::wstring& path) const;

    mutable std::recursive_mutex m_mutex;
    std::wstring m_csvPath;
    std::unordered_map<std::wstring, RunRecord> m_records;
    bool m_isDirty = false;
};

} // namespace tools3000::service::db
