#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureRegionHistory.h — 截图历史与固定选区管理器 (PixPin R / Shift+R + 收藏固定选区标杆)
//
// 职责 (MECE 架构):
//   1. 线程安全地管理历史截图选区与用户置顶/命名的固定选区（物理虚拟桌面坐标 RECT，宽/高 >= 4）；
//   2. 未置顶历史遵循用户配置上限 (0..50，默认 10) 的 LRU 去重与置顶淘汰；已置顶固定选区永不自动淘汰；
//   3. 支持单会话双向回溯游标（resetCursor / stepOlder[R] / stepNewer[Shift+R]）及自定义快捷键解析；
//   4. 虚拟桌面边界安全裁剪：外接副屏拔除或分辨率变化时自动裁剪交集；若完全无有效交集，
//      自动平移回主屏幕中心并按可见边界安全限幅尺寸，彻底杜绝不可见幽灵选区；
//   5. 完整 CRUD 与快照序列化支持，供设置页「历史与固定选区管理」卡片及跨重启持久化消费。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CAPTUREREGIONHISTORY_H
#define TOOLS3000_CAPTURE_CAPTUREREGIONHISTORY_H

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::capture {

/// 单条截图选区实体（兼容自动记录的历史选区与用户收藏/命名的固定选区）
struct CaptureRegionEntry {
    std::string id;
    RECT rect{};
    std::string name;
    bool pinned = false;
    int64_t updatedAt = 0;

    constexpr LONG width() const noexcept { return rect.right - rect.left; }
    constexpr LONG height() const noexcept { return rect.bottom - rect.top; }
};

class CaptureRegionHistory {
public:
    static constexpr std::size_t kDefaultMaxEntries = 10;
    static constexpr std::size_t kMaxEntries = kDefaultMaxEntries;
    static constexpr std::size_t kMinConfigurableEntries = 0;
    static constexpr std::size_t kMaxConfigurableEntries = 50;
    static constexpr std::size_t kMaxPinnedEntries = 50;
    static constexpr LONG kMinDimension = 4;
    static constexpr int kUnsetCursor = -1;

    CaptureRegionHistory() = default;
    ~CaptureRegionHistory() = default;

    CaptureRegionHistory(const CaptureRegionHistory&) = delete;
    CaptureRegionHistory& operator=(const CaptureRegionHistory&) = delete;

    /// 全局共享单例（供 CaptureOverlay / CaptureInput / PluginEntry 跨会话复用）
    static CaptureRegionHistory& instance() {
        static CaptureRegionHistory inst;
        return inst;
    }

    /// 将用户配置的截图内历史选区快捷键名归一化为合法标准名（默认 "R"，空串表示关闭）
    static std::string normalizeCycleKeyName(std::string_view raw) {
        std::string s;
        s.reserve(raw.size());
        for (unsigned char ch : raw) {
            if (!std::isspace(ch)) {
                s.push_back(static_cast<char>(std::toupper(ch)));
            }
        }
        if (s.empty() || s == "NONE" || s == "OFF" || s == "DISABLED") {
            return "";
        }
        if (s == "R") return "R";
        if (s == "`" || s == "BACKQUOTE" || s == "TILDE") return "`";
        if (s == "F2") return "F2";
        if (s == "F4") return "F4";
        return "R";
    }

    /// 将配置的历史选区单键名映射为 Win32 虚拟键码（返回 0 表示关闭截图内单键触发）
    static WPARAM parseCycleKeyVk(std::string_view raw) noexcept {
        std::string s;
        for (unsigned char ch : raw) {
            if (!std::isspace(ch)) {
                s.push_back(static_cast<char>(std::toupper(ch)));
            }
        }
        if (s.empty() || s == "NONE" || s == "OFF" || s == "DISABLED") return 0;
        if (s == "R") return 'R';
        if (s == "`" || s == "BACKQUOTE" || s == "TILDE") return VK_OEM_3;
        if (s == "F2") return VK_F2;
        if (s == "F4") return VK_F4;
        return 'R';
    }

    /// 生成快捷键提示胶囊文案（如 L"R / Shift+R" 或 L"F2 / Shift+F2"；禁用时返回空串）
    static std::wstring formatCycleHintPair(std::string_view raw) {
        const std::string norm = normalizeCycleKeyName(raw);
        if (norm.empty()) return {};
        std::wstring keyW(norm.begin(), norm.end());
        return keyW + L" / Shift+" + keyW;
    }

    /// 规范化矩形，确保 left <= right 且 top <= bottom
    static constexpr RECT normalizeRect(const RECT& rc) noexcept {
        return RECT{
            (rc.left <= rc.right) ? rc.left : rc.right,
            (rc.top <= rc.bottom) ? rc.top : rc.bottom,
            (rc.left <= rc.right) ? rc.right : rc.left,
            (rc.top <= rc.bottom) ? rc.bottom : rc.top
        };
    }

    /// 判断矩形是否满足最小有效尺寸（宽 >= 4 且高 >= 4）
    static constexpr bool isValidRect(const RECT& rc) noexcept {
        const RECT norm = normalizeRect(rc);
        return (norm.right - norm.left) >= kMinDimension &&
               (norm.bottom - norm.top) >= kMinDimension;
    }

    /// 比较两个已规范化矩形是否完全相等
    static constexpr bool rectEquals(const RECT& a, const RECT& b) noexcept {
        return a.left == b.left &&
               a.top == b.top &&
               a.right == b.right &&
               a.bottom == b.bottom;
    }

    /// 按当前虚拟桌面边界安全裁剪历史矩形：
    ///   - 若与 virtualBounds 存在宽/高均 >= kMinDimension 的有效交集，直接返回裁剪后的交集；
    ///   - 若无交集或交集过窄（如外接副屏已拔除），则平移回 primaryBounds（若有效且在 virtualBounds 内）
    ///     或 virtualBounds 的几何中心，并按目标区域尺寸安全限幅。
    static bool sanitizeToBounds(
        const RECT& rawRect,
        const RECT& virtualBounds,
        RECT& outRect,
        const RECT* primaryBounds = nullptr) noexcept {
        const RECT vb = normalizeRect(virtualBounds);
        const LONG vbW = vb.right - vb.left;
        const LONG vbH = vb.bottom - vb.top;
        if (vbW < kMinDimension || vbH < kMinDimension) {
            return false;
        }

        const RECT r = normalizeRect(rawRect);
        const LONG rawW = r.right - r.left;
        const LONG rawH = r.bottom - r.top;
        if (rawW < kMinDimension || rawH < kMinDimension) {
            return false;
        }

        // 1. 优先计算与当前可见虚拟桌面的交集
        const LONG interLeft = (std::max)(r.left, vb.left);
        const LONG interTop = (std::max)(r.top, vb.top);
        const LONG interRight = (std::min)(r.right, vb.right);
        const LONG interBottom = (std::min)(r.bottom, vb.bottom);
        if ((interRight - interLeft) >= kMinDimension &&
            (interBottom - interTop) >= kMinDimension) {
            outRect = RECT{interLeft, interTop, interRight, interBottom};
            return true;
        }

        // 2. 与可见虚拟桌面无有效交集（如副屏拔除）：回落主屏中心并限幅尺寸
        RECT targetArea = vb;
        if (primaryBounds != nullptr) {
            const RECT pb = normalizeRect(*primaryBounds);
            const LONG pLeft = (std::max)(pb.left, vb.left);
            const LONG pTop = (std::max)(pb.top, vb.top);
            const LONG pRight = (std::min)(pb.right, vb.right);
            const LONG pBottom = (std::min)(pb.bottom, vb.bottom);
            if ((pRight - pLeft) >= kMinDimension && (pBottom - pTop) >= kMinDimension) {
                targetArea = RECT{pLeft, pTop, pRight, pBottom};
            }
        }

        const LONG targetW = targetArea.right - targetArea.left;
        const LONG targetH = targetArea.bottom - targetArea.top;
        const LONG clampedW = std::clamp(rawW, kMinDimension, targetW);
        const LONG clampedH = std::clamp(rawH, kMinDimension, targetH);

        LONG newLeft = targetArea.left + (targetW - clampedW) / 2;
        LONG newTop = targetArea.top + (targetH - clampedH) / 2;
        newLeft = std::clamp(newLeft, targetArea.left, targetArea.right - clampedW);
        newTop = std::clamp(newTop, targetArea.top, targetArea.bottom - clampedH);

        outRect = RECT{newLeft, newTop, newLeft + clampedW, newTop + clampedH};
        return true;
    }

    /// 动态配置未置顶历史选区最大条数（0..50；设为 0 时不再自动记录并清理未置顶条目，已置顶固定选区永久保留）
    void setMaxEntries(std::size_t limit) {
        const std::size_t clamped = std::clamp(limit, kMinConfigurableEntries, kMaxConfigurableEntries);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_maxEntries = clamped;
        trimUnpinnedLocked();
        clampCursorLocked();
    }

    std::size_t maxEntries() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_maxEntries;
    }

    /// 导出全部选区实体（按最近使用倒序，供设置页与持久化消费）
    std::vector<CaptureRegionEntry> entries() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::vector<CaptureRegionEntry>(m_entries.begin(), m_entries.end());
    }

    /// 兼容旧版：导出纯 RECT 快照
    std::vector<RECT> snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<RECT> out;
        out.reserve(m_entries.size());
        for (const auto& entry : m_entries) {
            out.push_back(entry.rect);
        }
        return out;
    }

    /// 从持久化完整实体恢复（自动去重、过滤非法矩形并按上限约束未置顶条目）
    void loadEntries(const std::vector<CaptureRegionEntry>& items) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_cursor = kUnsetCursor;
        std::size_t pinnedCount = 0;
        std::size_t unpinnedCount = 0;
        for (const auto& raw : items) {
            const RECT norm = normalizeRect(raw.rect);
            if (!isValidRect(norm)) continue;
            if (raw.pinned && pinnedCount >= kMaxPinnedEntries) continue;
            if (!raw.pinned && unpinnedCount >= m_maxEntries) continue;
            bool dup = false;
            for (const auto& existing : m_entries) {
                if (rectEquals(existing.rect, norm) && existing.name == raw.name) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            CaptureRegionEntry clean = raw;
            clean.rect = norm;
            clean.name = sanitizeName(clean.name);
            if (clean.id.empty()) {
                clean.id = generateIdLocked();
            }
            if (clean.updatedAt <= 0) {
                clean.updatedAt = nowEpochMs();
            }
            m_entries.push_back(std::move(clean));
            if (raw.pinned) ++pinnedCount;
            else ++unpinnedCount;
        }
    }

    /// 兼容旧版：从纯 RECT 快照恢复
    void loadSnapshot(const std::vector<RECT>& rects) {
        std::vector<CaptureRegionEntry> converted;
        converted.reserve(rects.size());
        const int64_t now = nowEpochMs();
        for (std::size_t i = 0; i < rects.size(); ++i) {
            CaptureRegionEntry entry{};
            entry.rect = rects[i];
            entry.updatedAt = now - static_cast<int64_t>(i);
            converted.push_back(std::move(entry));
        }
        loadEntries(converted);
    }

    /// 写入一条新的虚拟屏幕坐标选区：
    ///   - 宽或高 < 4 时拒绝写入返回 false；
    ///   - 若历史中已存在完全相同矩形，则保留其名称与置顶标记并提升至最新顶部；
    ///   - 若为新矩形且 m_maxEntries == 0，则拒绝新增未置顶记录（不破坏已置顶固定选区）；
    ///   - 超过 m_maxEntries 条未置顶记录时仅淘汰尾部最旧未置顶条目；
    ///   - 成功记录后将游标重置为尚未回溯态。
    bool record(const RECT& screenRect) {
        const RECT norm = normalizeRect(screenRect);
        if (!isValidRect(norm)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const int64_t now = nowEpochMs();

        if (!m_entries.empty() && rectEquals(m_entries.front().rect, norm)) {
            m_entries.front().updatedAt = now;
            m_cursor = kUnsetCursor;
            return true;
        }

        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (rectEquals(it->rect, norm)) {
                CaptureRegionEntry promoted = std::move(*it);
                promoted.updatedAt = now;
                m_entries.erase(it);
                m_entries.push_front(std::move(promoted));
                m_cursor = kUnsetCursor;
                return true;
            }
        }

        if (m_maxEntries == 0) {
            m_cursor = kUnsetCursor;
            return false;
        }

        CaptureRegionEntry created{};
        created.id = generateIdLocked();
        created.rect = norm;
        created.pinned = false;
        created.updatedAt = now;
        m_entries.push_front(std::move(created));
        trimUnpinnedLocked();
        m_cursor = kUnsetCursor;
        return true;
    }

    /// 新增或更新自定义/固定选区（供设置页「历史与固定选区管理」CRUD 使用）
    std::string upsertCustomRegion(
        const RECT& rawRect,
        const std::string& name,
        bool pinned,
        const std::string& existingId = {}) {
        const RECT norm = normalizeRect(rawRect);
        if (!isValidRect(norm)) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string cleanName = sanitizeName(name);
        const int64_t now = nowEpochMs();

        if (!existingId.empty()) {
            for (auto& entry : m_entries) {
                if (entry.id == existingId) {
                    entry.rect = norm;
                    entry.name = cleanName;
                    entry.pinned = pinned;
                    entry.updatedAt = now;
                    trimUnpinnedLocked();
                    clampCursorLocked();
                    return entry.id;
                }
            }
        }

        if (pinned && countPinnedLocked() >= kMaxPinnedEntries) {
            return {};
        }
        if (!pinned && m_maxEntries == 0) {
            return {};
        }

        CaptureRegionEntry created{};
        created.id = generateIdLocked();
        created.rect = norm;
        created.name = cleanName;
        created.pinned = pinned;
        created.updatedAt = now;
        const std::string outId = created.id;
        m_entries.push_front(std::move(created));
        trimUnpinnedLocked();
        m_cursor = kUnsetCursor;
        return outId;
    }

    bool setPinned(const std::string& id, bool pinned) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.id == id) {
                if (pinned && !entry.pinned && countPinnedLocked() >= kMaxPinnedEntries) {
                    return false;
                }
                entry.pinned = pinned;
                entry.updatedAt = nowEpochMs();
                trimUnpinnedLocked();
                clampCursorLocked();
                return true;
            }
        }
        return false;
    }

    bool rename(const std::string& id, const std::string& newName) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& entry : m_entries) {
            if (entry.id == id) {
                entry.name = sanitizeName(newName);
                entry.updatedAt = nowEpochMs();
                return true;
            }
        }
        return false;
    }

    bool removeById(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->id == id) {
                m_entries.erase(it);
                clampCursorLocked();
                return true;
            }
        }
        return false;
    }

    /// 仅清空未置顶的自动历史记录，完整保留用户收藏的固定选区
    std::size_t clearUnpinned() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t before = m_entries.size();
        m_entries.erase(
            std::remove_if(m_entries.begin(), m_entries.end(),
                           [](const CaptureRegionEntry& e) { return !e.pinned; }),
            m_entries.end());
        clampCursorLocked();
        return before - m_entries.size();
    }

    bool findById(const std::string& id, CaptureRegionEntry& outEntry) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& entry : m_entries) {
            if (entry.id == id) {
                outEntry = entry;
                return true;
            }
        }
        return false;
    }

    /// 每次新截图会话开启时重置浏览游标为「尚未回溯」
    void resetCursor() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cursor = kUnsetCursor;
    }

    /// 向更早历史步进（对应 R）：
    ///   - 首次调用返回最新一条（索引 0）；
    ///   - 继续调用依次回溯更早记录，抵达最旧一条后保持停留在最旧一条。
    bool stepOlder(
        const RECT& virtualBounds,
        RECT& outRect,
        const RECT* primaryBounds = nullptr,
        std::string* outName = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.empty()) {
            return false;
        }

        int nextCursor = 0;
        if (m_cursor < 0) {
            nextCursor = 0;
        } else if (static_cast<std::size_t>(m_cursor + 1) < m_entries.size()) {
            nextCursor = m_cursor + 1;
        } else {
            nextCursor = static_cast<int>(m_entries.size() - 1);
        }

        const auto& entry = m_entries[static_cast<std::size_t>(nextCursor)];
        RECT sanitized{};
        if (!sanitizeToBounds(entry.rect, virtualBounds, sanitized, primaryBounds)) {
            return false;
        }

        m_cursor = nextCursor;
        outRect = sanitized;
        if (outName) *outName = entry.name;
        return true;
    }

    /// 向较新历史步进（对应 Shift+R）：
    ///   - 若当前位于较旧记录，则向前翻回较新一条；
    ///   - 若已在最新一条或尚未回溯，则保持在最新一条（索引 0）。
    bool stepNewer(
        const RECT& virtualBounds,
        RECT& outRect,
        const RECT* primaryBounds = nullptr,
        std::string* outName = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.empty()) {
            return false;
        }

        int nextCursor = 0;
        if (m_cursor <= 0) {
            nextCursor = 0;
        } else {
            nextCursor = m_cursor - 1;
        }

        const auto& entry = m_entries[static_cast<std::size_t>(nextCursor)];
        RECT sanitized{};
        if (!sanitizeToBounds(entry.rect, virtualBounds, sanitized, primaryBounds)) {
            return false;
        }

        m_cursor = nextCursor;
        outRect = sanitized;
        if (outName) *outName = entry.name;
        return true;
    }

    bool hasHistory() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_entries.empty();
    }

    bool empty() const {
        return !hasHistory();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    std::size_t pinnedCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return countPinnedLocked();
    }

    /// 当前游标索引（-1 表示尚未回溯，0 表示最新一条）
    int cursorIndex() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cursor;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_cursor = kUnsetCursor;
    }

private:
    static int64_t nowEpochMs() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string sanitizeName(std::string_view raw) {
        std::size_t start = 0;
        while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start]))) ++start;
        std::size_t end = raw.size();
        while (end > start && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
        std::string out(raw.substr(start, end - start));
        constexpr std::size_t kMaxNameBytes = 96;
        if (out.size() > kMaxNameBytes) {
            out.resize(kMaxNameBytes);
            // 避免截断多字节 UTF-8 字符尾部续字节
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u) {
                out.pop_back();
            }
        }
        return out;
    }

    std::string generateIdLocked() {
        ++m_idSeq;
        return "reg_" + std::to_string(nowEpochMs()) + "_" + std::to_string(m_idSeq);
    }

    std::size_t countPinnedLocked() const noexcept {
        std::size_t n = 0;
        for (const auto& entry : m_entries) {
            if (entry.pinned) ++n;
        }
        return n;
    }

    void trimUnpinnedLocked() {
        std::size_t unpinned = 0;
        for (const auto& entry : m_entries) {
            if (!entry.pinned) ++unpinned;
        }
        while (unpinned > m_maxEntries) {
            bool erased = false;
            for (auto it = m_entries.end(); it != m_entries.begin();) {
                --it;
                if (!it->pinned) {
                    m_entries.erase(it);
                    --unpinned;
                    erased = true;
                    break;
                }
            }
            if (!erased) break;
        }
    }

    void clampCursorLocked() noexcept {
        if (m_entries.empty()) {
            m_cursor = kUnsetCursor;
        } else if (m_cursor >= static_cast<int>(m_entries.size())) {
            m_cursor = static_cast<int>(m_entries.size()) - 1;
        }
    }

    mutable std::mutex m_mutex;
    std::deque<CaptureRegionEntry> m_entries;
    std::size_t m_maxEntries = kDefaultMaxEntries;
    uint64_t m_idSeq = 0;
    int m_cursor = kUnsetCursor;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CAPTUREREGIONHISTORY_H
