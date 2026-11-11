#include "core/stats/StatsManager.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace tools3000::core {

nlohmann::json DailyStats::toJson() const {
    nlohmann::json j;
    j["totalKeys"] = totalKeys;
    j["leftClicks"] = leftClicks;
    j["rightClicks"] = rightClicks;
    j["mouseDistance"] = mouseDistance;
    j["scrolls"] = scrolls;
    nlohmann::json mapJson = nlohmann::json::object();
    for (const auto& [k, v] : keyMap) {
        mapJson[std::to_string(k)] = v;
    }
    j["keyMap"] = mapJson;
    return j;
}

DailyStats DailyStats::fromJson(const nlohmann::json& j) {
    DailyStats stats;
    stats.totalKeys = j.value("totalKeys", 0ULL);
    stats.leftClicks = j.value("leftClicks", 0ULL);
    stats.rightClicks = j.value("rightClicks", 0ULL);
    stats.mouseDistance = j.value("mouseDistance", 0.0);
    stats.scrolls = j.value("scrolls", 0ULL);
    if (j.contains("keyMap") && j["keyMap"].is_object()) {
        for (auto& [k, v] : j["keyMap"].items()) {
            try {
                const int key = std::stoi(k);
                if (key >= 0 && key <= 255 && v.is_number_unsigned()) {
                    stats.keyMap[key] = v.get<uint64_t>();
                }
            } catch (...) {
                // Skip corrupt individual entries while preserving the day.
            }
        }
    }
    return stats;
}

StatsManager& StatsManager::instance() {
    static StatsManager inst;
    return inst;
}

std::string StatsManager::getCurrentDateStr() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now{};
    localtime_s(&tm_now, &time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d");
    return oss.str();
}

void StatsManager::initialize() {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    {
        std::lock_guard lock(m_mutex);
        if (m_initialized.load(std::memory_order_acquire)) return;
        m_currentDate = getCurrentDateStr();
        loadFromFile();
        m_dirty = false;
        m_lastSaveTick = GetTickCount64();
        m_initialized.store(true, std::memory_order_release);
    }
    if (m_flushThread.joinable()) {
        m_flushRunning.store(false, std::memory_order_release);
        m_flushCv.notify_all();
        m_flushThread.join();
    }
    m_flushRunning.store(true, std::memory_order_release);
    try {
        m_flushThread = std::thread([this]() { flushLoop(); });
    } catch (const std::exception& e) {
        m_flushRunning.store(false, std::memory_order_release);
        LOG_ERROR("Failed to start stats flush thread: {}", e.what());
    }
    LOG_INFO("按键统计管理器初始化成功, 日期={}", m_currentDate);
}

void StatsManager::shutdown() {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    if (!m_initialized.load(std::memory_order_acquire) && !m_flushThread.joinable()) return;
    m_flushRunning.store(false, std::memory_order_release);
    m_flushCv.notify_all();
    if (m_flushThread.joinable()) m_flushThread.join();
    if (m_initialized.load(std::memory_order_acquire)) {
        flushPending(true);
        std::lock_guard lock(m_mutex);
        m_initialized.store(false, std::memory_order_release);
    }
    LOG_INFO("按键统计管理器已关闭");
}

void StatsManager::loadFromFile() {
    std::error_code ec;
    const auto dir = WinUtils::getAppDataDirectory() / L"stats";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("Failed to create stats directory: {}", ec.message());
        m_historyData = nlohmann::json::object();
        return;
    }
    const auto path = dir / L"stats.json";
    const auto legacyPath = WinUtils::getExeDirectory() / L"data" / L"stats.json";
    if (!std::filesystem::exists(path, ec) && std::filesystem::exists(legacyPath, ec)) {
        ec.clear();
        std::filesystem::copy_file(legacyPath, path,
                                   std::filesystem::copy_options::skip_existing, ec);
        if (ec) LOG_WARN("迁移旧统计文件失败: {}", ec.message());
    }
    ec.clear();
    if (!std::filesystem::exists(path, ec) || ec) {
        m_historyData = nlohmann::json::object();
        return;
    }
    try {
        std::ifstream ifs(path);
        m_historyData = nlohmann::json::parse(ifs);
        if (!m_historyData.is_object()) throw std::runtime_error("统计文件根节点不是对象");
        if (m_historyData.contains(m_currentDate)) {
            m_todayStats = DailyStats::fromJson(m_historyData[m_currentDate]);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("加载统计数据失败: {}", e.what());
        m_historyData = nlohmann::json::object();
    }
}

bool StatsManager::saveToFile() {
    std::error_code ec;
    auto dir = WinUtils::getAppDataDirectory() / L"stats";
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        LOG_ERROR("保存统计数据失败: {}", ec.message());
        return false;
    }
    auto path = dir / L"stats.json";
    auto tempPath = dir / L"stats.json.tmp";
    
    m_historyData[m_currentDate] = m_todayStats.toJson();
    
    try {
        std::ofstream ofs(tempPath, std::ios::binary | std::ios::trunc);
        ofs << m_historyData.dump(2);
        ofs.flush();
        if (!ofs) throw std::runtime_error("写入统计临时文件失败");
        ofs.close();
        if (!MoveFileExW(tempPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD moveErr = GetLastError();
            std::error_code rmEc;
            std::filesystem::remove(tempPath, rmEc);
            throw std::runtime_error("替换统计文件失败, error=" + std::to_string(moveErr));
        }
        m_dirty = false;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("保存统计数据失败: {}", e.what());
        return false;
    }
}

void StatsManager::checkDateRollover() {
    std::string today = getCurrentDateStr();
    if (today != m_currentDate) {
        m_historyData[m_currentDate] = m_todayStats.toJson();
        bool prevSaved = true;
        // 保存昨天的数据（若有变更）
        if (m_dirty) {
            prevSaved = saveToFile();
        }
        m_currentDate = today;
        m_todayStats = DailyStats(); // 新的一天重置内存
        // 如果文件中已经有今天的数据(比如异常退出再进)，尝试加载
        if (m_historyData.contains(m_currentDate)) {
            m_todayStats = DailyStats::fromJson(m_historyData[m_currentDate]);
        }
        m_dirty = !prevSaved;
    }
}

// 节流保存
void StatsManager::recordKey(int virtualKey) {
    m_pendingTotalKeys.fetch_add(1, std::memory_order_relaxed);
    if (virtualKey >= 0 && virtualKey < static_cast<int>(m_pendingKeys.size())) {
        m_pendingKeys[static_cast<size_t>(virtualKey)].fetch_add(1, std::memory_order_relaxed);
    }
}

void StatsManager::recordLeftClick() {
    m_pendingLeftClicks.fetch_add(1, std::memory_order_relaxed);
}

void StatsManager::recordRightClick() {
    m_pendingRightClicks.fetch_add(1, std::memory_order_relaxed);
}

void StatsManager::recordScroll() {
    m_pendingScrolls.fetch_add(1, std::memory_order_relaxed);
}

void StatsManager::recordMouseDistance(double pixels) {
    if (pixels > 0.0) m_pendingMouseDistance.fetch_add(pixels, std::memory_order_relaxed);
}

DailyStats StatsManager::getTodayStats() {
    flushPending(false);
    std::lock_guard lock(m_mutex);
    checkDateRollover();
    return m_todayStats;
}

nlohmann::json StatsManager::getHistory(int days) {
    days = std::clamp(days, 1, 3660);
    flushPending(false);
    std::lock_guard lock(m_mutex);
    checkDateRollover();
    
    // 为了防止还没 save 的 todayStats 没在 history 里面
    m_historyData[m_currentDate] = m_todayStats.toJson();

    nlohmann::json result = nlohmann::json::object();
    
    // 我们找出最近的 days 天。
    // 计算前推几天
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < days; ++i) {
        auto time_t_past = std::chrono::system_clock::to_time_t(now - std::chrono::hours(24 * i));
        std::tm tm_past{};
        localtime_s(&tm_past, &time_t_past);
        std::ostringstream oss;
        oss << std::put_time(&tm_past, "%Y-%m-%d");
        std::string dateStr = oss.str();
        
        if (m_historyData.contains(dateStr)) {
            result[dateStr] = m_historyData[dateStr];
        } else {
            result[dateStr] = DailyStats().toJson(); // 空数据
        }
    }
    
    return result;
}

nlohmann::json StatsManager::getTotalStats() {
    flushPending(false);
    std::lock_guard lock(m_mutex);
    checkDateRollover();
    
    // 确保今日数据也计入
    m_historyData[m_currentDate] = m_todayStats.toJson();

    uint64_t totalKeystrokes = 0;
    for (const auto& [dateStr, dailyJson] : m_historyData.items()) {
        totalKeystrokes += dailyJson.value("totalKeys", 0ULL);
    }
    
    return {
        {"totalKeystrokes", totalKeystrokes}
    };
}

void StatsManager::clearToday() {
    flushPending(false);
    std::lock_guard lock(m_mutex);
    m_todayStats = DailyStats();
    m_dirty = true;
    saveToFile();
    m_lastSaveTick = GetTickCount64();
}

void StatsManager::flushPending(bool forceSave) {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return;
    checkDateRollover();

    const uint64_t deltaKeys = m_pendingTotalKeys.exchange(0, std::memory_order_relaxed);
    const uint64_t deltaLeft = m_pendingLeftClicks.exchange(0, std::memory_order_relaxed);
    const uint64_t deltaRight = m_pendingRightClicks.exchange(0, std::memory_order_relaxed);
    const uint64_t deltaScrolls = m_pendingScrolls.exchange(0, std::memory_order_relaxed);
    const double deltaDist = m_pendingMouseDistance.exchange(0.0, std::memory_order_relaxed);

    m_todayStats.totalKeys += deltaKeys;
    m_todayStats.leftClicks += deltaLeft;
    m_todayStats.rightClicks += deltaRight;
    m_todayStats.scrolls += deltaScrolls;
    m_todayStats.mouseDistance += deltaDist;

    bool anyKeyDelta = false;
    for (size_t key = 0; key < m_pendingKeys.size(); ++key) {
        const uint64_t count = m_pendingKeys[key].exchange(0, std::memory_order_relaxed);
        if (count > 0) {
            m_todayStats.keyMap[static_cast<int>(key)] += count;
            anyKeyDelta = true;
        }
    }

    if (deltaKeys > 0 || deltaLeft > 0 || deltaRight > 0 ||
        deltaScrolls > 0 || deltaDist > 0.0 || anyKeyDelta) {
        m_dirty = true;
    }

    const uint64_t now = GetTickCount64();
    if (m_dirty && (forceSave || now - m_lastSaveTick >= 60000)) {
        saveToFile();
        m_lastSaveTick = now;
    }
}

void StatsManager::flushLoop() {
    std::unique_lock waitLock(m_flushWaitMutex);
    while (m_flushRunning.load(std::memory_order_acquire)) {
        m_flushCv.wait_for(waitLock, std::chrono::seconds(1),
                          [this]() { return !m_flushRunning.load(std::memory_order_acquire); });
        if (!m_flushRunning.load(std::memory_order_acquire)) break;
        waitLock.unlock();
        flushPending(false);
        waitLock.lock();
    }
}

} // namespace tools3000::core
