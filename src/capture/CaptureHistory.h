#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureHistory — 截图历史记录管理器
//
// 职责:
//   1. 环形缓冲区存储最近 N 次截图（默认 20 次）
//   2. 每次截图确认后自动推入历史
//   3. 支持历史回放浏览（通过 / 键在覆盖层中激活）
//   4. 线程安全：截图线程推入、UI 线程读取
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CAPTUREHISTORY_H
#define TOOLS3000_CAPTURE_CAPTUREHISTORY_H

#include "capture/ScreenCapture.h"

#include <opencv2/core.hpp>
#include <deque>
#include <mutex>
#include <string>
#include <chrono>
#include <optional>

namespace tools3000::capture {

/// 历史记录条目
struct HistoryEntry {
    cv::Mat image;                     // 截图图像（带标注的最终版本）
    cv::Mat thumbnail;                 // 缩略图（宽度 160px，用于预览）
    CaptureRegion region;              // 截取区域
    std::string timestamp;             // ISO 8601 时间戳
    std::string filePath;              // 保存路径（如有）

    /// 生成缩略图
    static cv::Mat createThumbnail(const cv::Mat& source, int maxWidth = 160);
};

/// 截图历史记录管理器
class CaptureHistory {
public:
    static CaptureHistory& instance();

    /// 将截图推入历史记录
    /// @param image 最终截图（带标注）
    /// @param region 截取区域
    /// @param filePath 保存路径（可为空）
    void push(const cv::Mat& image, const CaptureRegion& region,
              const std::string& filePath = "");

    /// 获取指定索引的历史记录（0 = 最新）
    /// @return 指针（可能为 null）
    std::optional<HistoryEntry> get(int index) const;

    /// 当前历史记录数量
    int count() const;

    /// 清空所有历史
    void clear();

    /// 设置最大历史记录数
    void setMaxSize(int maxSize);

    /// 获取最大历史记录数
    int maxSize() const;

    /// 获取所有条目的元数据（不含图像数据，用于 IPC 列表展示）
    /// 返回 JSON 数组: [{index, timestamp, width, height, filePath}, ...]
    std::string toMetadataJson() const;

private:
    CaptureHistory() = default;
    CaptureHistory(const CaptureHistory&) = delete;
    CaptureHistory& operator=(const CaptureHistory&) = delete;

    std::deque<HistoryEntry> m_entries;
    int m_maxSize = 20;
    size_t m_totalBytes = 0;
    static constexpr size_t MAX_MEMORY_BYTES = 128ull * 1024ull * 1024ull;
    mutable std::mutex m_mutex;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_CAPTUREHISTORY_H
