// ─────────────────────────────────────────────────────────────────────────────
// CaptureHistory.cpp — 截图历史记录管理器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/CaptureHistory.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace tools3000::capture {

namespace {

size_t computeResidentEntryBytes(const HistoryEntry& e) noexcept {
    return e.compressedPng.size() +
           e.image.total() * e.image.elemSize() +
           e.thumbnail.total() * e.thumbnail.elemSize();
}

HistoryEntry materializeEntryCopy(const HistoryEntry& stored) {
    HistoryEntry out;
    out.id = stored.id;
    out.thumbnail = stored.thumbnail;
    out.region = stored.region;
    out.timestamp = stored.timestamp;
    out.filePath = stored.filePath;
    if (!stored.image.empty()) {
        out.image = stored.image.clone();
    } else if (!stored.compressedPng.empty()) {
        out.image = cv::imdecode(stored.compressedPng, cv::IMREAD_UNCHANGED);
    }
    return out;
}

}  // namespace

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
    entry.thumbnail = HistoryEntry::createThumbnail(image);
    entry.region = region;
    entry.filePath = filePath;
    if (entry.region.width <= 0) entry.region.width = image.cols;
    if (entry.region.height <= 0) entry.region.height = image.rows;

    // 仅常驻轻量 PNG 压缩字节流 + 160px 缩略图，告别常驻最多 20 张未压缩全尺寸原图（128MB+）
    bool encodedOk = false;
    try {
        encodedOk = cv::imencode(".png", image, entry.compressedPng,
                                 {cv::IMWRITE_PNG_COMPRESSION, 3});
    } catch (const cv::Exception&) {
        encodedOk = false;
    }
    if (!encodedOk || entry.compressedPng.empty()) {
        entry.compressedPng.clear();
        entry.image = image.clone();
    }

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
        entry.id = m_nextId++;
        m_totalBytes += computeResidentEntryBytes(entry);
        m_entries.push_front(std::move(entry));

        // 同时限制条目数和内存；4K 截图不能无限挤占常驻内存。
        while (m_entries.size() > 1 &&
               (static_cast<int>(m_entries.size()) > m_maxSize || m_totalBytes > MAX_MEMORY_BYTES)) {
            const auto& oldest = m_entries.back();
            const size_t oldestBytes = computeResidentEntryBytes(oldest);
            m_totalBytes = (m_totalBytes > oldestBytes) ? (m_totalBytes - oldestBytes) : 0;
            m_entries.pop_back();
        }
    }

    LOG_INFO("CaptureHistory: 推入新截图, 尺寸={}x{}, 当前历史数={}",
             image.cols, image.rows, count());
}

std::optional<HistoryEntry> CaptureHistory::get(int index) const {
    HistoryEntry snapshot;
    {
        std::lock_guard lock(m_mutex);
        if (index < 0 || index >= static_cast<int>(m_entries.size())) {
            return std::nullopt;
        }
        snapshot = m_entries[index];
    }
    return materializeEntryCopy(snapshot);
}

std::optional<HistoryEntry> CaptureHistory::getById(uint64_t id) const {
    if (id == 0) return std::nullopt;
    HistoryEntry snapshot;
    bool found = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& entry : m_entries) {
            if (entry.id == id) {
                snapshot = entry;
                found = true;
                break;
            }
        }
    }
    if (!found) return std::nullopt;
    return materializeEntryCopy(snapshot);
}

std::optional<cv::Mat> CaptureHistory::getThumbnail(int index) const {
    std::lock_guard lock(m_mutex);
    if (index < 0 || index >= static_cast<int>(m_entries.size())) {
        return std::nullopt;
    }
    if (m_entries[index].thumbnail.empty()) {
        return std::nullopt;
    }
    return m_entries[index].thumbnail.clone();
}

std::optional<cv::Mat> CaptureHistory::getThumbnailById(uint64_t id) const {
    if (id == 0) return std::nullopt;
    std::lock_guard lock(m_mutex);
    for (const auto& entry : m_entries) {
        if (entry.id == id) {
            if (entry.thumbnail.empty()) return std::nullopt;
            return entry.thumbnail.clone();
        }
    }
    return std::nullopt;
}

size_t CaptureHistory::totalMemoryBytes() const noexcept {
    std::lock_guard lock(m_mutex);
    return m_totalBytes;
}

int CaptureHistory::count() const {
    std::lock_guard lock(m_mutex);
    return static_cast<int>(m_entries.size());
}

bool CaptureHistory::remove(int index) {
    size_t remaining = 0;
    {
        std::lock_guard lock(m_mutex);
        if (index < 0 || index >= static_cast<int>(m_entries.size())) {
            return false;
        }
        const auto& target = m_entries[index];
        const size_t entryBytes = computeResidentEntryBytes(target);
        m_totalBytes = (m_totalBytes > entryBytes) ? (m_totalBytes - entryBytes) : 0;
        m_entries.erase(m_entries.begin() + index);
        remaining = m_entries.size();
    }
    LOG_INFO("CaptureHistory: 已移除历史记录 index={}, 剩余={}", index, remaining);
    return true;
}

bool CaptureHistory::removeById(uint64_t id) {
    if (id == 0) return false;
    size_t remaining = 0;
    {
        std::lock_guard lock(m_mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [id](const HistoryEntry& e) { return e.id == id; });
        if (it == m_entries.end()) {
            return false;
        }
        const size_t entryBytes = computeResidentEntryBytes(*it);
        m_totalBytes = (m_totalBytes > entryBytes) ? (m_totalBytes - entryBytes) : 0;
        m_entries.erase(it);
        remaining = m_entries.size();
    }
    LOG_INFO("CaptureHistory: 已按 ID 移除历史记录 id={}, 剩余={}", id, remaining);
    return true;
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
        const size_t oldestBytes = computeResidentEntryBytes(oldest);
        m_totalBytes = (m_totalBytes > oldestBytes) ? (m_totalBytes - oldestBytes) : 0;
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
            {"id", e.id},
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
