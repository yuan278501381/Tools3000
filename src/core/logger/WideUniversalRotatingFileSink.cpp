// ─────────────────────────────────────────────────────────────────────────────
// WideUniversalRotatingFileSink.cpp — 统一通用全共享轮转日志文件槽实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/logger/WideUniversalRotatingFileSink.h"
#include "core/utils/WinUtils.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

namespace tools3000::core {

namespace {

struct ArchiveFileInfo {
    std::filesystem::path fullPath;
    std::string dateStr; // YYYY-MM-DD
    int year = 0;
    int month = 0;
    int day = 0;
    uint32_t seq = 0;
    bool isGz = false;
    uintmax_t fileSize = 0;
};

// 尝试解析归档文件名格式: {logFileName}_{YYYY-MM-DD}_{seq:03d}.log[.gz]
std::optional<ArchiveFileInfo> parseArchiveFileName(
    const std::filesystem::path& path, const std::string& logFileName)
{
    const std::string name = WinUtils::wstringToUtf8(path.filename().wstring());
    const std::string prefix = logFileName + "_";
    if (!name.starts_with(prefix)) {
        return std::nullopt;
    }

    bool isGz = false;
    std::string withoutExt;
    if (name.ends_with(".log.gz")) {
        isGz = true;
        withoutExt = name.substr(0, name.size() - 7);
    } else if (name.ends_with(".log")) {
        isGz = false;
        withoutExt = name.substr(0, name.size() - 4);
    } else {
        return std::nullopt;
    }

    // withoutExt 格式必须为: {logFileName}_{YYYY-MM-DD}_{seq}
    const std::string rest = withoutExt.substr(prefix.size());
    if (rest.size() < 12) {
        return std::nullopt;
    }
    if (rest[4] != '-' || rest[7] != '-' || rest[10] != '_') {
        return std::nullopt;
    }

    int y = 0, m = 0, d = 0;
    try {
        y = std::stoi(rest.substr(0, 4));
        m = std::stoi(rest.substr(5, 2));
        d = std::stoi(rest.substr(8, 2));
    } catch (...) {
        return std::nullopt;
    }
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) {
        return std::nullopt;
    }

    uint32_t seq = 0;
    try {
        seq = static_cast<uint32_t>(std::stoul(rest.substr(11)));
    } catch (...) {
        return std::nullopt;
    }

    ArchiveFileInfo info;
    info.fullPath = path;
    info.dateStr = rest.substr(0, 10);
    info.year = y;
    info.month = m;
    info.day = d;
    info.seq = seq;
    info.isGz = isGz;

    std::error_code ec;
    info.fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        info.fileSize = 0;
    }
    return info;
}

// 计算两个日期相隔天数 (today - archiveDate)
int calculateDaysDiff(int todayY, int todayM, int todayD, int arcY, int arcM, int arcD) {
    try {
        const std::chrono::year_month_day todayYmd{
            std::chrono::year{todayY},
            std::chrono::month{static_cast<unsigned>(todayM)},
            std::chrono::day{static_cast<unsigned>(todayD)}
        };
        const std::chrono::year_month_day arcYmd{
            std::chrono::year{arcY},
            std::chrono::month{static_cast<unsigned>(arcM)},
            std::chrono::day{static_cast<unsigned>(arcD)}
        };
        if (todayYmd.ok() && arcYmd.ok()) {
            return static_cast<int>((std::chrono::sys_days{todayYmd} - std::chrono::sys_days{arcYmd}).count());
        }
    } catch (...) {}
    return 0;
}

// 使用 zlib gzip 流式压缩指定文件为 .gz
bool compressFileGzip(const std::filesystem::path& srcPath, const std::filesystem::path& destGzPath) {
    HANDLE hSrc = CreateFileW(
        srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) {
        return false;
    }

    gzFile gzOut = gzopen_w(destGzPath.c_str(), "wb6");
    if (!gzOut) {
        CloseHandle(hSrc);
        return false;
    }

    constexpr DWORD kChunkSize = 64 * 1024;
    std::vector<char> buffer(kChunkSize);
    DWORD bytesRead = 0;
    bool success = true;

    while (ReadFile(hSrc, buffer.data(), kChunkSize, &bytesRead, nullptr) && bytesRead > 0) {
        const int written = gzwrite(gzOut, buffer.data(), bytesRead);
        if (written <= 0) {
            success = false;
            break;
        }
    }

    CloseHandle(hSrc);
    if (gzclose(gzOut) != Z_OK) {
        success = false;
    }

    if (!success) {
        std::error_code ec;
        std::filesystem::remove(destGzPath, ec);
    }
    return success;
}

}  // namespace

WideUniversalRotatingFileSink::WideUniversalRotatingFileSink(WideUniversalSinkConfig config)
    : m_config(std::move(config)) {
    if (m_config.maxFileSize == 0) {
        m_config.maxFileSize = 10 * 1024 * 1024;
    }
    if (m_config.logFileName.empty()) {
        m_config.logFileName = "tools3000";
    }
    m_retentionDays.store(m_config.retentionDays);
    m_maxTotalSize.store(m_config.maxTotalSize);
    m_idleHandleTimeoutMs.store(m_config.idleHandleTimeoutMs);
    m_enableCompression.store(m_config.enableCompression);
    m_activePath = m_config.logDir / WinUtils::utf8ToWstring(m_config.logFileName + ".log");

    openHandleLocked(false);

    // 检查磁盘上已存在的活动文件并恢复其日期
    if (m_file != INVALID_HANDLE_VALUE && m_currentFileSize > 0) {
        FILETIME ftWrite{};
        if (GetFileTime(m_file, nullptr, nullptr, &ftWrite)) {
            FILETIME localFt{};
            SYSTEMTIME st{};
            if (FileTimeToLocalFileTime(&ftWrite, &localFt) && FileTimeToSystemTime(&localFt, &st)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
                m_activeFileDay = buf;
            }
        }
    }
    if (m_activeFileDay.empty()) {
        m_activeFileDay = getCurrentDayString();
    }

    // 启动后台管理与异步压缩线程
    m_workerThread = std::thread(&WideUniversalRotatingFileSink::workerLoop, this);

    // 启动时触发一次后台自净扫描
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_janitorPending = true;
        m_cv.notify_one();
    }
}

WideUniversalRotatingFileSink::WideUniversalRotatingFileSink(
    std::filesystem::path path, size_t maxFileSize, size_t maxFileCount)
{
    m_config.logDir = path.parent_path();
    std::string stem = WinUtils::wstringToUtf8(path.stem().wstring());
    // 去掉可能的 .log 后缀
    if (stem.ends_with(".log")) {
        stem = stem.substr(0, stem.size() - 4);
    }
    m_config.logFileName = stem.empty() ? "tools3000" : stem;
    m_config.maxFileSize = (std::max)(size_t{1}, maxFileSize);
    m_config.retentionDays = 7;
    m_config.maxTotalSize = (std::max)(m_config.maxFileSize * (std::max)(size_t{1}, maxFileCount), size_t{50 * 1024 * 1024});
    m_config.idleHandleTimeoutMs = 10000;
    m_config.enableCompression = true;
    m_retentionDays.store(m_config.retentionDays);
    m_maxTotalSize.store(m_config.maxTotalSize);
    m_idleHandleTimeoutMs.store(m_config.idleHandleTimeoutMs);
    m_enableCompression.store(m_config.enableCompression);

    m_activePath = m_config.logDir / WinUtils::utf8ToWstring(m_config.logFileName + ".log");

    openHandleLocked(false);
    m_activeFileDay = getCurrentDayString();
    m_workerThread = std::thread(&WideUniversalRotatingFileSink::workerLoop, this);
}

WideUniversalRotatingFileSink::~WideUniversalRotatingFileSink() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_stopWorker = true;
        m_cv.notify_all();
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
        closeHandleLocked();
    }
}

void WideUniversalRotatingFileSink::setRetentionDays(uint32_t days) {
    m_retentionDays.store(days);
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    m_config.retentionDays = days;
    std::lock_guard<std::mutex> wlock(m_workerMutex);
    m_janitorPending = true;
    m_cv.notify_one();
}

uint32_t WideUniversalRotatingFileSink::getRetentionDays() const {
    return m_retentionDays.load();
}

void WideUniversalRotatingFileSink::setMaxTotalSize(size_t maxTotalSize) {
    m_maxTotalSize.store(maxTotalSize);
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    m_config.maxTotalSize = maxTotalSize;
    std::lock_guard<std::mutex> wlock(m_workerMutex);
    m_janitorPending = true;
    m_cv.notify_one();
}

size_t WideUniversalRotatingFileSink::getMaxTotalSize() const {
    return m_maxTotalSize.load();
}

void WideUniversalRotatingFileSink::setIdleTimeoutMs(uint32_t timeoutMs) {
    m_idleHandleTimeoutMs.store(timeoutMs);
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    m_config.idleHandleTimeoutMs = timeoutMs;
}

uint32_t WideUniversalRotatingFileSink::getIdleTimeoutMs() const {
    return m_idleHandleTimeoutMs.load();
}

void WideUniversalRotatingFileSink::setCompressionEnabled(bool enable) {
    m_enableCompression.store(enable);
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    m_config.enableCompression = enable;
    if (enable) {
        std::lock_guard<std::mutex> wlock(m_workerMutex);
        m_janitorPending = true;
        m_cv.notify_one();
    }
}

bool WideUniversalRotatingFileSink::isCompressionEnabled() const {
    return m_enableCompression.load();
}

bool WideUniversalRotatingFileSink::isFileHandleOpen() const {
    return m_fileHandleOpen.load();
}

std::filesystem::path WideUniversalRotatingFileSink::getActiveLogPath() const {
    return m_activePath;
}

void WideUniversalRotatingFileSink::triggerJanitorSync() {
    if (m_config.enableCompression) {
        compressPendingFilesSync();
    }
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    cleanOldFilesLockedOrSync();
}

void WideUniversalRotatingFileSink::forceRotate() {
    std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
    rotateLocked();
}

std::string WideUniversalRotatingFileSink::getActiveDay() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_activeFileDay;
}

void WideUniversalRotatingFileSink::setSimulatedDayForTesting(const std::string& dayStr) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_simulatedDay = dayStr;
}

std::string WideUniversalRotatingFileSink::getCurrentDayString() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_simulatedDay.empty()) {
        return m_simulatedDay;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return std::string(buf);
}

void WideUniversalRotatingFileSink::openHandleLocked(bool truncate) {
    std::error_code ec;
    std::filesystem::create_directories(m_config.logDir, ec);

    DWORD desiredAccess = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    DWORD disposition = OPEN_ALWAYS;
    if (truncate) {
        desiredAccess = GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        disposition = CREATE_ALWAYS;
    }

    m_file = CreateFileW(
        m_activePath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        disposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (m_file == INVALID_HANDLE_VALUE) {
        m_currentFileSize = 0;
        m_fileHandleOpen.store(false);
        return;
    }

    m_fileHandleOpen.store(true);
    LARGE_INTEGER li{};
    if (GetFileSizeEx(m_file, &li)) {
        m_currentFileSize = static_cast<size_t>((std::max)(LONGLONG{0}, li.QuadPart));
    } else {
        m_currentFileSize = 0;
    }
    m_lastWriteTime = std::chrono::steady_clock::now();
}

void WideUniversalRotatingFileSink::closeHandleLocked() noexcept {
    if (m_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_file);
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
        m_fileHandleOpen.store(false);
    }
}

uint32_t WideUniversalRotatingFileSink::getNextSequenceForDateLocked(const std::string& dateStr) const {
    uint32_t maxSeq = 0;
    std::error_code ec;
    if (!std::filesystem::exists(m_config.logDir, ec)) {
        return 1;
    }

    for (const auto& entry : std::filesystem::directory_iterator(m_config.logDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto info = parseArchiveFileName(entry.path(), m_config.logFileName);
        if (info && info->dateStr == dateStr) {
            maxSeq = (std::max)(maxSeq, info->seq);
        }
    }
    return maxSeq + 1;
}

void WideUniversalRotatingFileSink::rotateLocked() {
    closeHandleLocked();

    std::error_code ec;
    bool archiveCreated = false;
    std::filesystem::path archivePath;

    if (std::filesystem::exists(m_activePath, ec) && m_currentFileSize > 0) {
        const std::string dateStr = m_activeFileDay.empty() ? getCurrentDayString() : m_activeFileDay;
        const uint32_t seq = getNextSequenceForDateLocked(dateStr);

        char seqBuf[16];
        snprintf(seqBuf, sizeof(seqBuf), "%03u", seq);
        const std::string archiveName = m_config.logFileName + "_" + dateStr + "_" + seqBuf + ".log";
        archivePath = m_config.logDir / WinUtils::utf8ToWstring(archiveName);

        if (MoveFileExW(m_activePath.c_str(), archivePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            archiveCreated = true;
        } else {
            // 降级容错：若外部只读或共享占用导致 MoveFileEx 失败 (ERROR_SHARING_VIOLATION)，
            // 尝试复制后截断，保障历史日志不丢失且当前活跃日志安全接续
            if (CopyFileW(m_activePath.c_str(), archivePath.c_str(), FALSE)) {
                archiveCreated = true;
                HANDLE hTrunc = CreateFileW(
                    m_activePath.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    TRUNCATE_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr
                );
                if (hTrunc != INVALID_HANDLE_VALUE) {
                    CloseHandle(hTrunc);
                }
            }
        }

        if (archiveCreated) {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_config.enableCompression) {
                m_compressionQueue.push_back(archivePath);
            }
            m_janitorPending = true;
            m_cv.notify_one();
        }
    }

    m_activeFileDay = getCurrentDayString();
    openHandleLocked(true);
    m_currentFileSize = 0;
}

void WideUniversalRotatingFileSink::sink_it_(const spdlog::details::log_msg& message) {
    spdlog::memory_buf_t formatted;
    base_sink<std::mutex>::formatter_->format(message, formatted);

    // 1. 外部删除或重命名探测自愈：若磁盘上原文件被移走，脱钩旧句柄就地重新创建
    if (m_file != INVALID_HANDLE_VALUE) {
        const DWORD attrs = GetFileAttributesW(m_activePath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            CloseHandle(m_file);
            m_file = INVALID_HANDLE_VALUE;
            m_fileHandleOpen.store(false);
            m_currentFileSize = 0;
        }
    }
    if (m_file == INVALID_HANDLE_VALUE) {
        openHandleLocked(false);
        if (m_file == INVALID_HANDLE_VALUE) {
            return;
        }
    }

    // 2. 双轮驱动混合滚动引擎判定
    // (a) 时钟驱动：跨天自然日变更 (00:00:00)
    const std::string currentDay = getCurrentDayString();
    if (!m_activeFileDay.empty() && currentDay != m_activeFileDay) {
        if (m_currentFileSize > 0) {
            rotateLocked();
        } else {
            m_activeFileDay = currentDay;
        }
    } else if (m_activeFileDay.empty()) {
        m_activeFileDay = currentDay;
    }
    // (b) 体积驱动：当前体积 + 待写批次超出上限
    else if (m_currentFileSize > 0 && m_currentFileSize + formatted.size() > m_config.maxFileSize) {
        rotateLocked();
    }

    // 3. 稳健写入流 (带错误自愈重试，0 崩溃)
    if (m_file != INVALID_HANDLE_VALUE) {
        size_t offset = 0;
        while (offset < formatted.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(
                formatted.size() - offset,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(m_file, formatted.data() + offset, chunk, &written, nullptr) || written == 0) {
                // 自愈重试机制：重连句柄后再试一次
                closeHandleLocked();
                openHandleLocked(false);
                if (m_file != INVALID_HANDLE_VALUE) {
                    DWORD retryWritten = 0;
                    if (WriteFile(m_file, formatted.data() + offset, chunk, &retryWritten, nullptr) && retryWritten > 0) {
                        offset += retryWritten;
                        m_currentFileSize += retryWritten;
                    }
                }
                break;
            }
            offset += written;
            m_currentFileSize += written;
        }
        m_lastWriteTime = std::chrono::steady_clock::now();
    }
}

void WideUniversalRotatingFileSink::flush_() {
    if (m_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(m_file);
    }
}

void WideUniversalRotatingFileSink::cleanOldFilesLockedOrSync() {
    std::error_code ec;
    if (!std::filesystem::exists(m_config.logDir, ec)) {
        return;
    }

    // 收集所有归档文件
    std::vector<ArchiveFileInfo> archives;
    for (const auto& entry : std::filesystem::directory_iterator(m_config.logDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto info = parseArchiveFileName(entry.path(), m_config.logFileName);
        if (info) {
            archives.push_back(*info);
        }
    }

    // 当前日期解析 (无魔术常量，动态从本地时间初始化)
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int todayY = static_cast<int>(st.wYear);
    int todayM = static_cast<int>(st.wMonth);
    int todayD = static_cast<int>(st.wDay);
    const std::string curDay = getCurrentDayString();
    if (curDay.size() >= 10) {
        try {
            todayY = std::stoi(curDay.substr(0, 4));
            todayM = std::stoi(curDay.substr(5, 2));
            todayD = std::stoi(curDay.substr(8, 2));
        } catch (...) {}
    }

    // 维度 1: 保留天数过期清理 (0 为永久保留)
    const uint32_t retention = m_retentionDays.load();
    std::vector<ArchiveFileInfo> keptArchives;
    for (const auto& arc : archives) {
        bool expired = false;
        if (retention > 0) {
            const int diffDays = calculateDaysDiff(todayY, todayM, todayD, arc.year, arc.month, arc.day);
            if (diffDays > static_cast<int>(retention)) {
                expired = true;
            }
        }

        if (expired) {
            std::filesystem::remove(arc.fullPath, ec);
        } else {
            keptArchives.push_back(arc);
        }
    }

    // 维度 2: 历史总容量上限配额修剪 (级联淘汰最老归档)
    uintmax_t totalBytes = m_currentFileSize;
    if (totalBytes == 0 && std::filesystem::exists(m_activePath, ec)) {
        totalBytes = std::filesystem::file_size(m_activePath, ec);
    }
    for (const auto& arc : keptArchives) {
        totalBytes += arc.fileSize;
    }

    const size_t maxTotal = m_maxTotalSize.load();
    if (totalBytes > maxTotal) {
        // 按时间从老到新排序 (旧日期优先；同日期序号小者优先；同序号未压缩者优先)
        std::sort(keptArchives.begin(), keptArchives.end(), [](const ArchiveFileInfo& a, const ArchiveFileInfo& b) {
            if (a.year != b.year) return a.year < b.year;
            if (a.month != b.month) return a.month < b.month;
            if (a.day != b.day) return a.day < b.day;
            if (a.seq != b.seq) return a.seq < b.seq;
            if (a.isGz != b.isGz) return !a.isGz;
            return a.fullPath.wstring() < b.fullPath.wstring();
        });

        for (const auto& arc : keptArchives) {
            if (totalBytes <= maxTotal) break;
            if (std::filesystem::remove(arc.fullPath, ec)) {
                if (totalBytes > arc.fileSize) {
                    totalBytes -= arc.fileSize;
                } else {
                    totalBytes = 0;
                }
            }
        }
    }
}

void WideUniversalRotatingFileSink::compressPendingFilesSync() {
    std::vector<std::filesystem::path> uncompressed;
    {
        std::lock_guard<std::mutex> wlock(m_workerMutex);
        uncompressed.insert(uncompressed.end(), m_compressionQueue.begin(), m_compressionQueue.end());
        m_compressionQueue.clear();
    }
    std::error_code ec;
    if (std::filesystem::exists(m_config.logDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(m_config.logDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto info = parseArchiveFileName(entry.path(), m_config.logFileName);
            if (info && !info->isGz) {
                uncompressed.push_back(entry.path());
            }
        }
    }
    std::sort(uncompressed.begin(), uncompressed.end());
    uncompressed.erase(std::unique(uncompressed.begin(), uncompressed.end()), uncompressed.end());

    std::lock_guard<std::mutex> compLock(m_compressionMutex);
    for (const auto& path : uncompressed) {
        if (std::filesystem::exists(path, ec)) {
            auto gzPath = path;
            gzPath += L".gz";
            if (compressFileGzip(path, gzPath)) {
                std::filesystem::remove(path, ec);
            }
        }
    }
}

void WideUniversalRotatingFileSink::workerLoop() {
    while (true) {
        std::vector<std::filesystem::path> toCompress;
        bool doJanitor = false;
        const uint32_t idleMs = m_idleHandleTimeoutMs.load();
        const auto waitMs = idleMs > 0 ? (std::min)(uint32_t{200}, (std::max)(uint32_t{25}, idleMs / 3)) : uint32_t{200};
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(waitMs), [this] {
                return m_stopWorker || !m_compressionQueue.empty() || m_janitorPending;
            });
            if (m_stopWorker && m_compressionQueue.empty()) {
                break;
            }
            toCompress.swap(m_compressionQueue);
            doJanitor = m_janitorPending;
            m_janitorPending = false;
        }

        // 1. 在专用压缩互斥锁保护下执行后台耗时 gzip 压缩，0 影响写入线程性能，0 并发冲突
        if (!toCompress.empty() && m_config.enableCompression) {
            std::lock_guard<std::mutex> compLock(m_compressionMutex);
            for (const auto& path : toCompress) {
                std::error_code ec;
                if (std::filesystem::exists(path, ec)) {
                    auto gzPath = path;
                    gzPath += L".gz";
                    if (compressFileGzip(path, gzPath)) {
                        std::filesystem::remove(path, ec);
                    }
                }
            }
        }

        // 2. 空闲超时自动释放物理文件句柄 (Lease / Idle Handle Auto-Close)
        if (idleMs > 0) {
            std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
            if (m_file != INVALID_HANDLE_VALUE) {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - m_lastWriteTime).count();
                if (elapsedMs >= static_cast<int64_t>(idleMs)) {
                    closeHandleLocked();
                }
            }
        }

        // 3. 时钟驱动：跨天自然日变更 (00:00:00) 周期主动轮转 (Chronological Midnight Trigger)
        {
            const std::string curDay = getCurrentDayString();
            std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
            if (!m_activeFileDay.empty() && curDay != m_activeFileDay) {
                if (m_currentFileSize > 0) {
                    rotateLocked();
                } else {
                    m_activeFileDay = curDay;
                }
            }
        }

        // 4. 定期或事件触发自净配额检查
        if (doJanitor) {
            std::lock_guard<std::mutex> lock(base_sink<std::mutex>::mutex_);
            cleanOldFilesLockedOrSync();
        }
    }
}

}  // namespace tools3000::core
