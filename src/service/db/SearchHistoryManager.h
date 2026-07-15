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

struct SearchHistoryItem {
    std::wstring search;
    uint32_t searchCount = 0;
    uint64_t lastSearchDate = 0; // Windows FILETIME
};

class SearchHistoryManager {
public:
    static SearchHistoryManager& instance();

    SearchHistoryManager();
    ~SearchHistoryManager();

    // 初始化搜索历史文件路径 (默认使用 %APPDATA%\Tools3000\Search History.csv)
    void init(const std::wstring& customCsvPath = L"");
    void initForSid(const std::wstring& sid);

    // 记录搜索表达式
    void recordSearch(const std::wstring& search);

    // 获取最近搜索列表 (按最近搜索时间及频次降序)
    std::vector<SearchHistoryItem> getRecentSearches(size_t limit = 20) const;

    // 移除单个搜索词
    bool removeSearch(const std::wstring& search);

    // 磁盘持久化与载入
    bool load();
    bool save();
    void clear();

    // 获取当前记录总数
    size_t size() const;
    std::wstring getFilePath() const;

private:
    std::wstring getNormalizedKey(const std::wstring& search) const;

    mutable std::recursive_mutex m_mutex;
    std::wstring m_csvPath;
    std::unordered_map<std::wstring, SearchHistoryItem> m_records;
    bool m_isDirty = false;
};

} // namespace tools3000::service::db
