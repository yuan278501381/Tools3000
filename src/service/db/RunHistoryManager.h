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

namespace tools3000::service::db {

struct RunRecord {
    std::wstring filename;
    uint32_t runCount = 0;
    uint64_t lastRunDate = 0; // Windows FILETIME
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
