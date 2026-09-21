// ─────────────────────────────────────────────────────────────────────────────
// Logger.cpp — 统一日志系统实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/logger/Logger.h"
#include "core/logger/WideUniversalRotatingFileSink.h"
#include "core/utils/WinUtils.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/async.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <windows.h>

namespace tools3000::core {

static std::shared_ptr<spdlog::logger> s_logger;
static std::shared_ptr<WideUniversalRotatingFileSink> s_universalFileSink;
static std::atomic<uint8_t> s_logLanguage{static_cast<uint8_t>(LogLanguage::ZhCN)};

void Logger::initialize(const LoggerConfig& config) {
    try {
        // ── 初始化异步线程池 ─────────────────────────────────────────────
        spdlog::init_thread_pool(8192, 1);  // 队列大小 8192, 1 个后台线程

        // ── 构建多 Sink ─────────────────────────────────────────────────
        std::vector<spdlog::sink_ptr> sinks;

        // Sink 1: 控制台 (彩色)
        if (config.enableConsole) {
            auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            consoleSink->set_level(config.consoleLevel);
            consoleSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [tid:%t] %v");
            sinks.push_back(consoleSink);
        }

        // Sink 2: 文件 (WideUniversalRotatingFileSink 双轮驱动与三维配额管理)
        const std::filesystem::path logDir = config.logDir.empty()
            ? WinUtils::getLogDirectory()
            : config.logDir;

        std::filesystem::create_directories(logDir);

        WideUniversalSinkConfig universalConfig;
        universalConfig.logDir = logDir;
        universalConfig.logFileName = config.logFileName;
        universalConfig.maxFileSize = config.maxFileSize;
        universalConfig.retentionDays = config.retentionDays;
        universalConfig.maxTotalSize = config.maxTotalSize;
        universalConfig.idleHandleTimeoutMs = config.idleHandleTimeoutMs;
        universalConfig.enableCompression = config.enableCompression;

        s_universalFileSink = std::make_shared<WideUniversalRotatingFileSink>(universalConfig);
        s_universalFileSink->set_level(config.fileLevel);
        s_universalFileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [tid:%t] %v");
        sinks.push_back(s_universalFileSink);

        // Sink 3: MSVC 输出窗口 (仅 Debug 构建)
#ifdef _DEBUG
        if (config.enableMsvcSink) {
            auto msvcSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
            msvcSink->set_level(spdlog::level::debug);
            sinks.push_back(msvcSink);
        }
#endif

        // ── 创建异步 Logger ─────────────────────────────────────────────
        s_logger = std::make_shared<spdlog::async_logger>(
            "tools3000",
            sinks.begin(), sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block
        );
        s_logger->set_level(spdlog::level::trace);  // 总开关: 允许所有级别，由各 Sink 过滤
        s_logger->flush_on(spdlog::level::info);   // INFO 及以上立即刷盘
        spdlog::flush_every(std::chrono::seconds(1));

        spdlog::register_logger(s_logger);
        spdlog::set_default_logger(s_logger);

        LOG_INFO("日志系统初始化完成, 日志目录={}", WinUtils::wstringToUtf8(logDir.wstring()));

    } catch (const spdlog::spdlog_ex& ex) {
        // 日志系统初始化失败时，用 OutputDebugString 输出错误
        OutputDebugStringA(("Logger initialization failed: " + std::string(ex.what()) + "\n").c_str());
    }
}

void Logger::shutdown() {
    if (s_logger) {
        LOG_INFO("日志系统正在关闭...");
        s_logger->flush();
    }
    s_logger.reset();
    s_universalFileSink.reset();
    spdlog::shutdown();

    // 关键防崩溃保护：spdlog::shutdown() 会将 default_logger_ 置空。
    // 若后续模块（例如测试套件或静态对象析构）调用 spdlog::warn/info，
    // 解引用空指针将引发 SEH 0xc0000005 崩溃。此处挂载一个轻量无状态的控制台 fallback logger。
    try {
        auto fallbackSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fallbackLogger = std::make_shared<spdlog::logger>("", std::move(fallbackSink));
        spdlog::set_default_logger(fallbackLogger);
    } catch (...) {
    }
}

std::shared_ptr<spdlog::logger>& Logger::instance() {
    return s_logger;
}

void Logger::setLevel(spdlog::level::level_enum level) {
    if (s_logger) {
        s_logger->set_level(level);
        LOG_INFO_L("日志级别已切换为: {}", "Log level switched to: {}", spdlog::level::to_string_view(level));
    }
}

void Logger::setRetentionDays(uint32_t days) {
    if (s_universalFileSink) {
        s_universalFileSink->setRetentionDays(days);
        s_universalFileSink->triggerJanitorSync();
        LOG_INFO_L("日志保留天数已切换为: {} 天", "Log retention days switched to: {} days", days);
    }
}

uint32_t Logger::getRetentionDays() {
    if (s_universalFileSink) {
        return s_universalFileSink->getRetentionDays();
    }
    return 7;
}

void Logger::triggerJanitor() {
    if (s_universalFileSink) {
        s_universalFileSink->triggerJanitorSync();
    }
}

std::shared_ptr<WideUniversalRotatingFileSink> Logger::getFileSink() {
    return s_universalFileSink;
}

void Logger::setLanguage(const std::string& langCode) {
    if (langCode == "en-US" || langCode == "en") {
        s_logLanguage.store(static_cast<uint8_t>(LogLanguage::EnUS), std::memory_order_relaxed);
    } else if (langCode == "zh-TW" || langCode == "zh-HK") {
        s_logLanguage.store(static_cast<uint8_t>(LogLanguage::ZhTW), std::memory_order_relaxed);
    } else if (langCode == "auto") {
        LANGID langId = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(langId) == LANG_ENGLISH) {
            s_logLanguage.store(static_cast<uint8_t>(LogLanguage::EnUS), std::memory_order_relaxed);
        } else if (PRIMARYLANGID(langId) == LANG_CHINESE) {
            if (SUBLANGID(langId) == SUBLANG_CHINESE_TRADITIONAL || SUBLANGID(langId) == SUBLANG_CHINESE_HONGKONG) {
                s_logLanguage.store(static_cast<uint8_t>(LogLanguage::ZhTW), std::memory_order_relaxed);
            } else {
                s_logLanguage.store(static_cast<uint8_t>(LogLanguage::ZhCN), std::memory_order_relaxed);
            }
        } else {
            s_logLanguage.store(static_cast<uint8_t>(LogLanguage::EnUS), std::memory_order_relaxed);
        }
    } else {
        s_logLanguage.store(static_cast<uint8_t>(LogLanguage::ZhCN), std::memory_order_relaxed);
    }
}

LogLanguage Logger::getLanguage() {
    return static_cast<LogLanguage>(s_logLanguage.load(std::memory_order_relaxed));
}

}  // namespace tools3000::core
