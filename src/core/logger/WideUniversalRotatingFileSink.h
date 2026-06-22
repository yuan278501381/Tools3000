#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WideUniversalRotatingFileSink.h — 统一通用全共享轮转日志文件槽
//
// 架构特性：
//   1. 恒定单一事实源 (Canonical Active Log):
//      活动日志文件恒定命名为 {logFileName}.log，历史归档生成带 ISO 日期与序号的独立文件
//   2. 双轮驱动混合滚动引擎 (Hybrid Rotation Engine):
//      - 体积触发: 文件大小 + 缓冲区 > maxFileSize (默认 10MB) 时滚动，当天序号递增
//      - 时钟触发: 跨过午夜零点 (00:00:00，自然日变更) 时滚动，新一天序号归位 001
//   3. 句柄治理与全共享模式 (Handle Governance & Idle Auto-Release):
//      - 始终以 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE 打开
//      - 空闲超时 (默认 10 秒) 后台线程自动 FlushFileBuffers 并 CloseHandle 进入 0 句柄状态
//      - 下一次写入到达时秒级按需重开 (OPEN_ALWAYS / FILE_APPEND_DATA)
//   4. 外部重命名与删除自愈 (Self-Healing & Zero Crash):
//      - 外部删除/移动自动检测并在原地无缝重建 {logFileName}.log，0 崩溃 0 抛出
//   5. 三维物理配额与自净 (3D Quota Janitor & Background Compression):
//      - 单文件上限 (默认 10MB)
//      - 保留天数上限 (默认 7 天，0 为永久保留，与前端设置页无锁热更联动)
//      - 历史归档总容量上限 (默认 50MB，按时间级联修剪)
//      - 历史归档后台异步 gzip (.log.gz) 压缩，节省 90% 物理磁盘空间
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_LOGGER_WIDEUNIVERSALROTATINGFILESINK_H
#define TOOLS3000_CORE_LOGGER_WIDEUNIVERSALROTATINGFILESINK_H

#include "core/utils/Export.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tools3000::core {

/// 通用滚动文件槽物理配置
struct TOOLS3000CORE_API WideUniversalSinkConfig {
    std::filesystem::path logDir;              // 日志目录（Windows 原生宽字符路径）
    std::string logFileName = "tools3000";     // 活跃文件名主干 (如 "tools3000" -> "tools3000.log")
    size_t maxFileSize = 10 * 1024 * 1024;     // 单文件体积触发上限 (默认 10MB)
    uint32_t retentionDays = 7;                // 保留天数上限 (默认 7 天，0 为永久保留)
    size_t maxTotalSize = 50 * 1024 * 1024;    // 历史归档总容量上限 (默认 50MB)
    uint32_t idleHandleTimeoutMs = 10000;      // 空闲句柄释放超时 (毫秒，默认 10000 即 10 秒；0 则不自动关闭)
    bool enableCompression = true;             // 历史归档是否在后台异步 gzip 压缩
};

class TOOLS3000CORE_API WideUniversalRotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit WideUniversalRotatingFileSink(WideUniversalSinkConfig config);
    WideUniversalRotatingFileSink(std::filesystem::path path, size_t maxFileSize, size_t maxFileCount);
    ~WideUniversalRotatingFileSink() override;

    WideUniversalRotatingFileSink(const WideUniversalRotatingFileSink&) = delete;
    WideUniversalRotatingFileSink& operator=(const WideUniversalRotatingFileSink&) = delete;

    /// 动态设置归档保留天数 (0 为永久保留)
    void setRetentionDays(uint32_t days);
    uint32_t getRetentionDays() const;

    /// 动态设置历史总容量上限
    void setMaxTotalSize(size_t maxTotalSize);
    size_t getMaxTotalSize() const;

    /// 动态设置空闲超时 (毫秒)
    void setIdleTimeoutMs(uint32_t timeoutMs);
    uint32_t getIdleTimeoutMs() const;

    /// 动态设置历史归档是否启用压缩
    void setCompressionEnabled(bool enable);
    bool isCompressionEnabled() const;

    /// 物理文件句柄当前是否打开
    bool isFileHandleOpen() const;

    /// 获取当前活动日志文件的完整路径
    std::filesystem::path getActiveLogPath() const;

    /// 触发立即执行一次配额自净与未压缩归档压缩 (同步执行，供单测或显式维护调用)
    void triggerJanitorSync();

    /// 强制触发一次就地滚动并归档
    void forceRotate();

    /// 获取当前活跃日志记录的日期标识 (YYYY-MM-DD)
    std::string getActiveDay() const;

    /// 模拟活跃日期（专用于单元测试快速验证跨天滚动与归档逻辑）
    void setSimulatedDayForTesting(const std::string& dayStr);

protected:
    void sink_it_(const spdlog::details::log_msg& message) override;
    void flush_() override;

private:
    void openHandleLocked(bool truncate);
    void closeHandleLocked() noexcept;
    void rotateLocked();
    void cleanOldFilesLockedOrSync();
    void compressPendingFilesSync();
    void workerLoop();
    std::string getCurrentDayString() const;
    uint32_t getNextSequenceForDateLocked(const std::string& dateStr) const;

    WideUniversalSinkConfig m_config;
    std::filesystem::path m_activePath;
    HANDLE m_file{INVALID_HANDLE_VALUE};
    size_t m_currentFileSize{0};
    std::string m_activeFileDay;
    std::chrono::steady_clock::time_point m_lastWriteTime{};
    std::string m_simulatedDay;

    mutable std::mutex m_stateMutex;
    std::atomic<uint32_t> m_retentionDays{7};
    std::atomic<size_t> m_maxTotalSize{50 * 1024 * 1024};
    std::atomic<uint32_t> m_idleHandleTimeoutMs{10000};
    std::atomic<bool> m_fileHandleOpen{false};
    std::atomic<bool> m_enableCompression{true};

    // 后台清理与压缩线程
    std::thread m_workerThread;
    std::condition_variable m_cv;
    std::mutex m_workerMutex;
    std::mutex m_compressionMutex;
    bool m_stopWorker{false};
    std::vector<std::filesystem::path> m_compressionQueue;
    bool m_janitorPending{false};
};

/// 别名保持向后兼容
using WideRotatingFileSink = WideUniversalRotatingFileSink;

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_LOGGER_WIDEUNIVERSALROTATINGFILESINK_H
