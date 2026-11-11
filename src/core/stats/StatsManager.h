#ifndef TOOLS3000_CORE_STATS_STATSMANAGER_H
#define TOOLS3000_CORE_STATS_STATSMANAGER_H

#include "core/utils/Export.h"

#include <array>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>

namespace tools3000::core {

struct TOOLS3000CORE_API DailyStats {
    uint64_t totalKeys = 0;
    uint64_t leftClicks = 0;
    uint64_t rightClicks = 0;
    double mouseDistance = 0.0;
    uint64_t scrolls = 0;
    std::unordered_map<int, uint64_t> keyMap;

    nlohmann::json toJson() const;
    static DailyStats fromJson(const nlohmann::json& j);
};

class TOOLS3000CORE_API StatsManager {
public:
    static StatsManager& instance();

    void initialize();
    void shutdown();

    // 记录事件
    void recordKey(int virtualKey);
    void recordLeftClick();
    void recordRightClick();
    void recordScroll();
    void recordMouseDistance(double pixels);

    // 获取数据
    DailyStats getTodayStats();
    nlohmann::json getHistory(int days);
    nlohmann::json getTotalStats();
    void clearToday();

private:
    StatsManager() = default;
    ~StatsManager() = default;

    std::string getCurrentDateStr() const;
    void loadFromFile();
    bool saveToFile();
    void checkDateRollover(); // 检查是否跨天并清空内存或保存上一天
    void flushPending(bool forceSave);
    void flushLoop();

    std::mutex m_lifecycleMutex;
    std::mutex m_mutex;
    std::string m_currentDate;
    DailyStats m_todayStats;

    // 保存所有历史数据的 JSON 对象 (或者只加载当天，但由于要返回 history，我们把所有的都存一个大 json)
    nlohmann::json m_historyData;
    
    std::atomic<bool> m_initialized{false};
    uint64_t m_lastSaveTick = 0;
    bool m_dirty = false;

    // 系统低级钩子只做无锁原子累加；合并、跨日和磁盘 I/O 由后台线程处理。
    std::array<std::atomic<uint64_t>, 256> m_pendingKeys{};
    std::atomic<uint64_t> m_pendingTotalKeys{0};
    std::atomic<uint64_t> m_pendingLeftClicks{0};
    std::atomic<uint64_t> m_pendingRightClicks{0};
    std::atomic<uint64_t> m_pendingScrolls{0};
    std::atomic<double> m_pendingMouseDistance{0.0};
    std::atomic<bool> m_flushRunning{false};
    std::thread m_flushThread;
    std::mutex m_flushWaitMutex;
    std::condition_variable m_flushCv;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_STATS_STATSMANAGER_H
