#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Logger — 统一日志系统
//
// 特性:
//   1. 专业日志级别: TRACE / DEBUG / INFO / WARN / ERROR / CRITICAL
//   2. 自动携带 TraceId —— 每条日志可追溯到触发它的用户操作
//   3. 多 Sink: 控制台(彩色) + 文件(按大小滚动) + MSVC 输出窗口
//   4. 高性能: 基于 spdlog 异步日志，不阻塞业务线程
//   5. 线程安全: 所有方法可从任意线程调用
//
// 使用方式:
//   LOG_INFO("截图完成, 尺寸={}x{}", width, height);
//   LOG_ERROR("钩子安装失败, errorCode={}", GetLastError());
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_LOGGER_LOGGER_H
#define TOOLS3000_CORE_LOGGER_LOGGER_H

#include "core/utils/Export.h"

#include <string>
#include <memory>
#include <filesystem>
#include <source_location>
#include <spdlog/spdlog.h>

#include "core/logger/WideUniversalRotatingFileSink.h"

namespace tools3000::core {

/// 日志语言类型 (0 锁原子无缝切换)
enum class LogLanguage : uint8_t {
    ZhCN = 0,
    EnUS = 1,
    ZhTW = 2,
    Other = 3
};

/// 日志配置
struct LoggerConfig {
    std::filesystem::path logDir;              // 日志文件目录（Windows 原生宽字符路径）
    std::string logFileName = "tools3000";     // 日志文件名前缀
    spdlog::level::level_enum consoleLevel = spdlog::level::info;   // 控制台日志级别
    spdlog::level::level_enum fileLevel    = spdlog::level::debug;  // 文件日志级别
    size_t maxFileSize  = 10 * 1024 * 1024;    // 单文件最大 10MB
    size_t maxFileCount = 5;                   // 保留最多 5 个文件（向后兼容）
    uint32_t retentionDays = 7;                // 历史归档保留天数（默认 7 天，0 为永久保留）
    size_t maxTotalSize = 50 * 1024 * 1024;    // 历史归档总容量上限（默认 50MB）
    uint32_t idleHandleTimeoutMs = 10000;      // 空闲句柄释放超时（毫秒，默认 10 秒；0 则禁用自动关闭）
    bool enableCompression = true;             // 历史归档是否在后台异步 gzip 压缩
    bool enableConsole  = true;                // 是否启用控制台输出
    bool enableMsvcSink = true;                // 是否启用 MSVC Output 窗口
};

class TOOLS3000CORE_API Logger {
public:
    /// 初始化日志系统（应在 main 入口处调用一次）
    static void initialize(const LoggerConfig& config);

    /// 关闭日志系统（刷写缓冲区）
    static void shutdown();

    /// 获取底层 spdlog logger 实例
    static std::shared_ptr<spdlog::logger>& instance();

    /// 设置全局日志级别
    static void setLevel(spdlog::level::level_enum level);

    /// 设置日志当前语言（跟随 /general/language，如 "zh-CN", "en-US", "auto"）
    static void setLanguage(const std::string& langCode);

    /// 获取当前日志语言（0 锁原子读取，O(1) 亚微秒级耗时）
    static LogLanguage getLanguage();

    /// 设置历史归档保留天数（0 表示永久保留）
    static void setRetentionDays(uint32_t days);

    /// 获取当前历史归档保留天数
    static uint32_t getRetentionDays();

    /// 触发立即执行一次历史日志自净与压缩
    static void triggerJanitor();

    /// 获取底层活动文件 Sink 实例
    static std::shared_ptr<WideUniversalRotatingFileSink> getFileSink();

    Logger() = delete;
};

}  // namespace tools3000::core

// ─────────────────────────────────────────────────────────────────────────────
// 便捷宏 —— 自动注入 TraceId + 语言动态选择
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/TraceId.h"

#include "core/logger/I18nLogCatalog.h"

#define LOG_DISPATCH_FMT(lvl, rawFmt, ...)                                                  \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            const char* locFmt = tools3000::core::getLocalizedLogFormatWithTrace(rawFmt, tools3000::core::Logger::getLanguage()); \
            if (locFmt) {                                                                   \
                tools3000::core::Logger::instance()->lvl(fmt::runtime(locFmt), tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->lvl("[{}] " rawFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

// 智能多语言自适应宏 (无锁查表翻译中英文，0 堆内存分配)
#define LOG_TRACE(fmt, ...)    LOG_DISPATCH_FMT(trace, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)    LOG_DISPATCH_FMT(debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)     LOG_DISPATCH_FMT(info, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)     LOG_DISPATCH_FMT(warn, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)    LOG_DISPATCH_FMT(error, fmt, ##__VA_ARGS__)
#define LOG_CRITICAL(fmt, ...) LOG_DISPATCH_FMT(critical, fmt, ##__VA_ARGS__)

// 国际化双语动态跟随宏 (自动随语言设置切换为中文/英文输出，100% 兼容 fmt12 编译期检查与零额外开销)
#define LOG_TRACE_L(zhFmt, enFmt, ...)                                                      \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->trace("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->trace("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define LOG_DEBUG_L(zhFmt, enFmt, ...)                                                      \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->debug("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->debug("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define LOG_INFO_L(zhFmt, enFmt, ...)                                                       \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->info("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->info("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define LOG_WARN_L(zhFmt, enFmt, ...)                                                       \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->warn("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->warn("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define LOG_ERROR_L(zhFmt, enFmt, ...)                                                      \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->error("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->error("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#define LOG_CRITICAL_L(zhFmt, enFmt, ...)                                                   \
    do {                                                                                    \
        if (tools3000::core::Logger::instance()) {                                               \
            if (tools3000::core::Logger::getLanguage() == tools3000::core::LogLanguage::EnUS) {      \
                tools3000::core::Logger::instance()->critical("[{}] " enFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            } else {                                                                        \
                tools3000::core::Logger::instance()->critical("[{}] " zhFmt, tools3000::core::TraceId::current(), ##__VA_ARGS__); \
            }                                                                               \
        }                                                                                   \
    } while (0)

#endif  // TOOLS3000_CORE_LOGGER_LOGGER_H
