// ─────────────────────────────────────────────────────────────────────────────
// CaptureHistory.cpp — 截图历史记录管理器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureHistory.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"

#include <opencv2/imgproc.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace tools3000::capture {

// ─────────────────────────────────────────────────────────────────────────────
// HistoryEntry
// ─────────────────────────────────────────────────────────────────────────────

cv::Mat HistoryEntry::createThumbnail(const cv::Mat& source, int maxWidth) {
    if (source.empty()) return {};

    double scale = static_cast<double>(maxWidth) / source.cols;
    if (scale >= 1.0) return source.clone();  // 原图已够小

    cv::Mat thumb;
    cv::resize(source, thumb, cv::Size(), scale, scale, cv::INTER_AREA);
    return thumb;
}

// ─────────────────────────────────────────────────────────────────────────────
// CaptureHistory
// ─────────────────────────────────────────────────────────────────────────────

CaptureHistory& CaptureHistory::instance() {
    static CaptureHistory hist;
    return hist;
}

void CaptureHistory::push(const cv::Mat& image, const CaptureRegion& region,
                           const std::string& filePath) {
    if (image.empty()) {
        LOG_WARN("CaptureHistory: 忽略空图像");
        return;
    }

    HistoryEntry entry;
    entry.image = image.clone();
    entry.thumbnail = HistoryEntry::createThumbnail(image);
    entry.region = region;
    entry.filePath = filePath;

    // 生成 ISO 8601 时间戳
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
    localtime_s(&localTm, &time_t);
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%dT%H:%M:%S");
    entry.timestamp = oss.str();

    {
        std::lock_guard lock(m_mutex);
        m_totalBytes += entry.image.total() * entry.image.elemSize();
        m_totalBytes += entry.thumbnail.total() * entry.thumbnail.elemSize();
        m_entries.push_front(std::move(entry));

        // 同时限制条目数和内存；4K 截图不能无限挤占常驻内存。
        while (m_entries.size() > 1 &&
               (static_cast<int>(m_entries.size()) > m_maxSize || m_totalBytes > MAX_MEMORY_BYTES)) {
            const auto& oldest = m_entries.back();
            m_totalBytes -= oldest.image.total() * oldest.image.elemSize();
            m_totalBytes -= oldest.thumbnail.total() * oldest.thumbnail.elemSize();
            m_entries.pop_back();
        }
    }

    LOG_INFO("CaptureHistory: 推入新截图, 尺寸={}x{}, 当前历史数={}",
             image.cols, image.rows, count());
}

std::optional<HistoryEntry> CaptureHistory::get(int index) const {
    std::lock_guard lock(m_mutex);
    if (index < 0 || index >= static_cast<int>(m_entries.size())) {
        return std::nullopt;
    }
    return m_entries[index];
}

int CaptureHistory::count() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(m_entries.size());
}

void CaptureHistory::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
    m_totalBytes = 0;
    LOG_INFO("CaptureHistory: 已清空所有历史记录");
}

void CaptureHistory::setMaxSize(int maxSize) {
    std::lock_guard lock(m_mutex);
    m_maxSize = std::max(1, maxSize);
    while (static_cast<int>(m_entries.size()) > m_maxSize) {
        const auto& oldest = m_entries.back();
        m_totalBytes -= oldest.image.total() * oldest.image.elemSize();
        m_totalBytes -= oldest.thumbnail.total() * oldest.thumbnail.elemSize();
        m_entries.pop_back();
    }
    LOG_INFO("CaptureHistory: 设置最大历史数={}", m_maxSize);
}

int CaptureHistory::maxSize() const {
    std::lock_guard lock(m_mutex);
    return m_maxSize;
}

std::string CaptureHistory::toMetadataJson() const {
    std::lock_guard lock(m_mutex);
    nlohmann::json arr = nlohmann::json::array();

    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        const auto& e = m_entries[i];
        arr.push_back({
            {"index", i},
            {"timestamp", e.timestamp},
            {"width", e.region.width},
            {"height", e.region.height},
            {"filePath", e.filePath}
        });
    }

    return arr.dump();
}

}  // namespace tools3000::capture
