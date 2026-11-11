// ─────────────────────────────────────────────────────────────────────────────
// ConfigManager.cpp — 配置管理器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace tools3000::core {

namespace {

constexpr uintmax_t MaxImportedConfigBytes = 4u * 1024u * 1024u;

bool hasParentTraversal(const std::filesystem::path& path) {
    return std::any_of(path.begin(), path.end(), [](const auto& component) {
        return component == L"..";
    });
}

std::optional<std::filesystem::path> validateTransferPath(
    const std::filesystem::path& input, bool importing) {
    if (input.empty() || !input.is_absolute() || hasParentTraversal(input) ||
        input.filename().empty() || WinUtils::toLower(input.extension().wstring()) != L".json") {
        return std::nullopt;
    }

    std::error_code ec;
    if (importing) {
        const auto resolved = std::filesystem::canonical(input, ec);
        if (ec || !std::filesystem::is_regular_file(resolved, ec) || ec) return std::nullopt;
        const auto size = std::filesystem::file_size(resolved, ec);
        if (ec || size > MaxImportedConfigBytes) return std::nullopt;
        return resolved;
    }

    const auto parent = std::filesystem::canonical(input.parent_path(), ec);
    if (ec || !std::filesystem::is_directory(parent, ec) || ec) return std::nullopt;
    const auto resolved = (parent / input.filename()).lexically_normal();
    if (resolved.parent_path() != parent) return std::nullopt;
    const bool targetExists = std::filesystem::exists(resolved, ec);
    if (ec) return std::nullopt;
    if (targetExists) {
        const bool targetIsDirectory = std::filesystem::is_directory(resolved, ec);
        if (ec || targetIsDirectory) return std::nullopt;
    }
    return resolved;
}

}  // namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

bool ConfigManager::initialize(const std::filesystem::path& configDir, const std::string& filename) {
    TraceId::Scope scope;

    if (m_watchRunning.load() || m_flushRunning.load()) {
        shutdown();
    }

    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        LOG_ERROR("无法创建配置目录: path={}, error={}", configDir.string(), ec.message());
        return false;
    }
    m_configFilePath = configDir / filename;
    {
        std::lock_guard ioLock(m_ioMutex);
        m_hasFileStamp = false;
        m_lastLoadedFileSize = 0;
    }
    {
        std::lock_guard flushLock(m_flushMutex);
        m_dirtySeq = 0;
        m_flushedSeq = 0;
        m_dirty = false;
    }
    {
        std::unique_lock lock(m_mutex);
        m_config = json();
    }

    LOG_INFO("配置管理器初始化, 配置文件路径={}", m_configFilePath.string());

    if (!load()) return false;

    // 启动 200ms 脏合并异步刷盘线程
    m_flushRunning.store(true, std::memory_order_release);
    try {
        m_flushThread = std::thread([this]() { flushWorkerLoop(); });
    } catch (const std::exception& e) {
        m_flushRunning.store(false, std::memory_order_release);
        LOG_ERROR("无法启动配置监控线程: {}", e.what());
        return false;
    }

    // 启动文件监控线程
    m_watchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_watchStopEvent) {
        m_flushRunning.store(false, std::memory_order_release);
        m_flushCv.notify_all();
        if (m_flushThread.joinable()) m_flushThread.join();
        LOG_ERROR("无法创建配置监控停止事件: error={}", GetLastError());
        return false;
    }
    m_watchRunning = true;
    try {
        m_watchThread = std::thread([this]() { watchFileChanges(); });
    } catch (const std::exception& e) {
        m_watchRunning = false;
        CloseHandle(m_watchStopEvent);
        m_watchStopEvent = nullptr;
        m_flushRunning.store(false, std::memory_order_release);
        m_flushCv.notify_all();
        if (m_flushThread.joinable()) m_flushThread.join();
        LOG_ERROR("无法启动配置监控线程: {}", e.what());
        return false;
    }
    return true;
}

void ConfigManager::shutdown() {
    if (!m_watchRunning.load() && !m_watchThread.joinable() &&
        !m_flushRunning.load() && !m_flushThread.joinable()) {
        if (hasPendingDirty()) {
            (void)flushPendingDirty();
        }
        return;
    }

    LOG_INFO("配置管理器正在关闭...");

    // 停止后台合并刷盘线程
    m_flushRunning.store(false, std::memory_order_release);
    m_flushCv.notify_all();
    if (m_flushThread.joinable()) {
        m_flushThread.join();
    }

    // 停止文件监控
    m_watchRunning = false;
    if (m_watchStopEvent) {
        SetEvent(m_watchStopEvent);
    }
    if (m_watchThread.joinable()) {
        m_watchThread.join();
    }
    if (m_watchStopEvent) {
        CloseHandle(m_watchStopEvent);
        m_watchStopEvent = nullptr;
    }

    if (!flush()) LOG_ERROR("配置管理器关闭时保存失败");
}

bool ConfigManager::load(bool* changed) {
    std::lock_guard ioLock(m_ioMutex);
    json loaded = json::object();
    bool shouldCreate = false;
    try {
        if (!std::filesystem::exists(m_configFilePath)) {
            LOG_INFO("配置文件不存在, 将使用默认配置并创建文件: {}", m_configFilePath.string());
            shouldCreate = true;
        }

        std::ifstream file(m_configFilePath);
        if (file.is_open()) {
            loaded = json::parse(file, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
            if (!loaded.is_object()) {
                throw std::runtime_error("配置根节点必须是 JSON 对象");
            }
            LOG_INFO("配置文件加载成功, 键数量={}", loaded.size());
        }
    } catch (const std::exception& e) {
        bool hasValidConfig = false;
        {
            std::shared_lock lock(m_mutex);
            hasValidConfig = m_config.is_object();
        }
        if (hasValidConfig) {
            LOG_ERROR("配置文件解析失败: {}, 保留最后一次有效配置", e.what());
            return false;
        }
        LOG_ERROR("配置文件解析失败: {}, 首次启动使用默认配置", e.what());
        if (std::filesystem::exists(m_configFilePath)) {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER stamp{};
            stamp.LowPart = now.dwLowDateTime;
            stamp.HighPart = now.dwHighDateTime;
            auto backupPath = m_configFilePath;
            backupPath += L".corrupt." + std::to_wstring(stamp.QuadPart);
            std::error_code backupError;
            std::filesystem::copy_file(m_configFilePath, backupPath,
                                       std::filesystem::copy_options::none, backupError);
            if (backupError) {
                LOG_ERROR("损坏配置备份失败: {}", backupError.message());
            } else {
                LOG_WARN("损坏配置已保留到: {}", backupPath.string());
            }
        }
        shouldCreate = true;
    }

    if (shouldCreate && !writeSnapshotLocked(loaded)) return false;
    if (!shouldCreate) {
        std::error_code stampEc;
        const auto wt = std::filesystem::last_write_time(m_configFilePath, stampEc);
        if (!stampEc) {
            const auto sz = std::filesystem::file_size(m_configFilePath, stampEc);
            if (!stampEc) {
                m_lastLoadedWriteTime = wt;
                m_lastLoadedFileSize = sz;
                m_hasFileStamp = true;
            }
        }
    }
    {
        std::unique_lock lock(m_mutex);
        if (changed) *changed = m_config != loaded;
        m_config = std::move(loaded);
    }
    return true;
}

bool ConfigManager::save() const {
    std::lock_guard ioLock(m_ioMutex);
    json snapshot;
    {
        std::shared_lock lock(m_mutex);
        snapshot = m_config;
    }
    return writeSnapshotLocked(snapshot);
}

bool ConfigManager::writeSnapshotLocked(const json& snapshot) const {
    try {
        const std::string payload = snapshot.dump(2);
        if (WinUtils::atomicWriteFileWithFlush(m_configFilePath.wstring(), payload)) {
            m_diskWriteCount.fetch_add(1, std::memory_order_relaxed);
            std::error_code stampEc;
            const auto wt = std::filesystem::last_write_time(m_configFilePath, stampEc);
            if (!stampEc) {
                const auto sz = std::filesystem::file_size(m_configFilePath, stampEc);
                if (!stampEc) {
                    m_lastLoadedWriteTime = wt;
                    m_lastLoadedFileSize = sz;
                    m_hasFileStamp = true;
                }
            }
            LOG_TRACE("配置已通过硬件原子刷盘保存到文件: {}", m_configFilePath.string());
            return true;
        } else {
            LOG_ERROR("配置原子硬件刷盘失败: {}", m_configFilePath.string());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("配置保存失败: {}", e.what());
    }
    return false;
}

void ConfigManager::markDirtyAndScheduleFlush() {
    if (!m_flushRunning.load(std::memory_order_acquire)) {
        // 未启动后台合并线程时（如 shutdown 后独立调用），直接同步落盘保证零数据丢失
        (void)save();
        return;
    }
    {
        std::lock_guard flushLock(m_flushMutex);
        ++m_dirtySeq;
        if (!m_dirty) {
            m_dirty = true;
            m_dirtyDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        }
    }
    m_flushCv.notify_one();
}

bool ConfigManager::hasPendingDirty() const noexcept {
    std::lock_guard flushLock(m_flushMutex);
    return m_dirty || (m_flushedSeq < m_dirtySeq);
}

bool ConfigManager::flushPendingDirty() {
    uint64_t targetSeq = 0;
    {
        std::lock_guard flushLock(m_flushMutex);
        if (!m_dirty && m_flushedSeq >= m_dirtySeq) {
            return true;
        }
        targetSeq = m_dirtySeq;
    }

    std::lock_guard ioLock(m_ioMutex);
    uint64_t snapSeq = 0;
    json snapshot;
    {
        std::lock_guard flushLock(m_flushMutex);
        if (m_flushedSeq >= targetSeq && !m_dirty) {
            return true;
        }
        snapSeq = m_dirtySeq;
    }
    {
        std::shared_lock cfgLock(m_mutex);
        snapshot = m_config;
    }

    const bool ok = writeSnapshotLocked(snapshot);
    if (ok) {
        std::lock_guard flushLock(m_flushMutex);
        if (snapSeq > m_flushedSeq) {
            m_flushedSeq = snapSeq;
        }
        if (m_flushedSeq >= m_dirtySeq) {
            m_dirty = false;
        }
    }
    return ok;
}

bool ConfigManager::flush() {
    if (hasPendingDirty()) {
        return flushPendingDirty();
    }
    std::lock_guard ioLock(m_ioMutex);
    if (m_configFilePath.empty()) return false;
    std::error_code ec;
    if (std::filesystem::exists(m_configFilePath, ec) && !ec) {
        return true;
    }
    json snapshot;
    {
        std::shared_lock cfgLock(m_mutex);
        snapshot = m_config;
    }
    return writeSnapshotLocked(snapshot);
}

uint64_t ConfigManager::diskWriteCount() const noexcept {
    return m_diskWriteCount.load(std::memory_order_relaxed);
}

void ConfigManager::flushWorkerLoop() {
    for (;;) {
        {
            std::unique_lock flushLock(m_flushMutex);
            m_flushCv.wait(flushLock, [this]() {
                return !m_flushRunning.load(std::memory_order_acquire) || m_dirty;
            });
            if (!m_flushRunning.load(std::memory_order_acquire) && !m_dirty) {
                break;
            }
            if (m_dirty && m_flushRunning.load(std::memory_order_acquire)) {
                const auto deadline = m_dirtyDeadline;
                m_flushCv.wait_until(flushLock, deadline, [this]() {
                    return !m_flushRunning.load(std::memory_order_acquire);
                });
            }
        }
        (void)flushPendingDirty();
        if (!m_flushRunning.load(std::memory_order_acquire)) {
            break;
        }
    }
}

bool ConfigManager::setJsonValue(const std::string& key, json value) {
    if (m_configFilePath.empty()) {
        return false;
    }
    {
        std::unique_lock lock(m_mutex);
        try {
            if (!m_config.is_object()) {
                m_config = json::object();
            }
            m_config[json::json_pointer(key)] = std::move(value);
        } catch (const std::exception& e) {
            LOG_WARN("设置配置项失败: key={}, error={}", key, e.what());
            return false;
        }
    }
    markDirtyAndScheduleFlush();
    notifyChange(key);
    return true;
}

bool ConfigManager::mergePatch(const json& patch, const std::string& notificationKey) {
    if (!patch.is_object()) {
        LOG_WARN("配置合并失败: patch 根节点不是对象");
        return false;
    }
    if (m_configFilePath.empty()) {
        return false;
    }
    {
        std::unique_lock lock(m_mutex);
        try {
            if (!m_config.is_object()) {
                m_config = json::object();
            }
            std::string existingPipeToken;
            auto tokenPtr = json::json_pointer("/search/pipeToken");
            if (m_config.contains(tokenPtr) && m_config[tokenPtr].is_string()) {
                existingPipeToken = m_config[tokenPtr].get<std::string>();
            }

            m_config.merge_patch(patch);

            if (!existingPipeToken.empty()) {
                m_config["search"]["pipeToken"] = existingPipeToken;
            }
        } catch (const std::exception& e) {
            LOG_WARN("配置合并失败: {}", e.what());
            return false;
        }
    }
    markDirtyAndScheduleFlush();
    notifyChange(notificationKey);
    return true;
}

bool ConfigManager::has(const std::string& key) const {
    std::shared_lock lock(m_mutex);
    try {
        auto ptr = json::json_pointer(key);
        return m_config.contains(ptr);
    } catch (...) {
        return false;
    }
}

bool ConfigManager::remove(const std::string& key) {
    if (m_configFilePath.empty()) {
        return false;
    }
    bool changed = false;
    {
        std::unique_lock lock(m_mutex);
        try {
            const std::string normalizedKey =
                (!key.empty() && key.front() != '/') ? ("/" + key) : key;
            auto ptr = json::json_pointer(normalizedKey);
            if (ptr.empty()) {
                return false;
            }
            if (m_config.contains(ptr)) {
                auto parentPtr = ptr.parent_pointer();
                const std::string& childToken = ptr.back();
                auto& parentNode = m_config.at(parentPtr);
                if (parentNode.is_object()) {
                    changed = parentNode.erase(childToken) > 0;
                } else if (parentNode.is_array()) {
                    const bool validIdx =
                        !childToken.empty() &&
                        (childToken == "0" || childToken.front() != '0') &&
                        std::all_of(childToken.begin(), childToken.end(),
                                    [](unsigned char c) { return c >= '0' && c <= '9'; });
                    if (validIdx) {
                        const size_t idx = std::stoull(childToken);
                        if (idx < parentNode.size()) {
                            parentNode.erase(parentNode.begin() + static_cast<std::ptrdiff_t>(idx));
                            changed = true;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("删除配置项失败: key={}, error={}", key, e.what());
            return false;
        }
    }
    if (!changed) return true;
    markDirtyAndScheduleFlush();
    notifyChange(key);
    return true;
}

std::string ConfigManager::toJsonString(int indent) const {
    std::shared_lock lock(m_mutex);
    return m_config.dump(indent);
}

bool ConfigManager::fromJsonString(const std::string& jsonStr) {
    TraceId::Scope scope;
    try {
        auto incoming = json::parse(jsonStr);
        if (!incoming.is_object()) {
            throw std::invalid_argument("配置根节点必须是 JSON 对象");
        }
        const bool ok = mergePatch(incoming);
        if (ok) LOG_INFO("配置已从 JSON 字符串批量更新");
        return ok;
    } catch (const std::exception& e) {
        LOG_ERROR("JSON 字符串解析失败: {}", e.what());
        return false;
    }
}

bool ConfigManager::reset() {
    {
        std::lock_guard ioLock(m_ioMutex);
        json empty = json::object();
        std::string preservedPipeToken;
        {
            std::shared_lock lock(m_mutex);
            try {
                auto tokenPtr = json::json_pointer("/search/pipeToken");
                if (m_config.contains(tokenPtr) && m_config[tokenPtr].is_string()) {
                    preservedPipeToken = m_config[tokenPtr].get<std::string>();
                }
            } catch (...) {}
        }
        if (!preservedPipeToken.empty()) {
            empty["search"]["pipeToken"] = preservedPipeToken;
        }
        if (!writeSnapshotLocked(empty)) return false;
        {
            std::unique_lock lock(m_mutex);
            m_config = std::move(empty);
        }
        {
            std::lock_guard flushLock(m_flushMutex);
            ++m_dirtySeq;
            m_flushedSeq = m_dirtySeq;
            m_dirty = false;
        }
    }
    notifyChange("*");
    LOG_INFO("配置已重置");
    return true;
}

size_t ConfigManager::onChange(ConfigChangeCallback callback) {
    if (!callback) return 0;
    size_t id = m_nextCallbackId.fetch_add(1);
    if (id == 0) {
        id = m_nextCallbackId.fetch_add(1);
    }
    std::lock_guard lock(m_callbackMutex);
    m_callbacks.emplace_back(id, std::move(callback));
    return id;
}

void ConfigManager::removeOnChange(size_t callbackId) {
    if (callbackId == 0) return;
    std::vector<std::pair<size_t, ConfigChangeCallback>> removed;
    {
        std::lock_guard lock(m_callbackMutex);
        auto it = m_callbacks.begin();
        while (it != m_callbacks.end()) {
            if (it->first == callbackId) {
                removed.push_back(std::move(*it));
                it = m_callbacks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void ConfigManager::clearCallbacks() {
    std::vector<std::pair<size_t, ConfigChangeCallback>> callbacks;
    {
        std::lock_guard lock(m_callbackMutex);
        callbacks.swap(m_callbacks);
    }
}

void ConfigManager::notifyChange(const std::string& key) {
    std::vector<std::pair<size_t, ConfigChangeCallback>> callbacks;
    {
        std::lock_guard lock(m_callbackMutex);
        callbacks = m_callbacks;
    }
    for (const auto& [id, callback] : callbacks) {
        try {
            callback(key);
        } catch (const std::exception& e) {
            LOG_WARN("配置变更回调执行异常: callbackId={}, error={}", id, e.what());
        }
    }
}

bool ConfigManager::exportTo(const std::filesystem::path& filePath) const {
    TraceId::Scope scope;
    try {
        const auto validatedPath = validateTransferPath(filePath, false);
        if (!validatedPath) {
            LOG_WARN("拒绝不安全的配置导出路径: {}", WinUtils::wstringToUtf8(filePath.wstring()));
            return false;
        }
        const auto& safePath = *validatedPath;
        json snapshot;
        {
            std::shared_lock lock(m_mutex);
            snapshot = m_config;
        }
        auto tempPath = safePath;
        tempPath += L".tmp";
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG_ERROR("无法打开配置导出临时文件: {}", tempPath.string());
            return false;
        }
        file << snapshot.dump(2);
        file.flush();
        if (!file.good()) throw std::runtime_error("写入配置导出文件失败");
        file.close();
        if (!MoveFileExW(tempPath.c_str(), safePath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            throw std::runtime_error("替换配置导出文件失败, error=" + std::to_string(error));
        }
        LOG_INFO("配置已导出到: {}", WinUtils::wstringToUtf8(safePath.wstring()));
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("配置导出失败: {}", e.what());
    }
    return false;
}

bool ConfigManager::importFrom(const std::filesystem::path& filePath) {
    TraceId::Scope scope;
    try {
        const auto validatedPath = validateTransferPath(filePath, true);
        if (!validatedPath) {
            LOG_WARN("拒绝不安全的配置导入路径: {}", WinUtils::wstringToUtf8(filePath.wstring()));
            return false;
        }
        std::ifstream file(*validatedPath);
        if (file.is_open()) {
            auto incoming = json::parse(file);
            if (!incoming.is_object()) {
                throw std::runtime_error("导入配置根节点必须是 JSON 对象");
            }
            if (!mergePatch(incoming)) return false;
            LOG_INFO("配置已从文件导入(合并模式): {}",
                     WinUtils::wstringToUtf8(validatedPath->wstring()));
            return true;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("配置导入失败: {}", e.what());
    }
    return false;
}

void ConfigManager::watchFileChanges() {
    auto dir = m_configFilePath.parent_path();
    auto fileName = m_configFilePath.filename().wstring();

    HANDLE hDir = CreateFileW(
        dir.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (hDir == INVALID_HANDLE_VALUE) {
        LOG_WARN("无法监控配置文件目录: {}", dir.string());
        return;
    }

    LOG_DEBUG("配置文件监控已启动: {}", dir.string());

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        LOG_WARN("无法创建配置文件监控事件: error={}", GetLastError());
        CloseHandle(hDir);
        return;
    }

    alignas(DWORD) char buffer[4096];
    HANDLE waitHandles[] = {overlapped.hEvent, m_watchStopEvent};

    while (m_watchRunning) {
        DWORD bytesReturned = 0;
        ResetEvent(overlapped.hEvent);

        BOOL result = ReadDirectoryChangesW(
            hDir,
            buffer, sizeof(buffer),
            FALSE,  // 不监控子目录
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned,
            &overlapped,
            nullptr
        );

        if (!result && GetLastError() != ERROR_IO_PENDING) {
            LOG_WARN("读取配置目录变更失败: error={}", GetLastError());
            break;
        }

        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0) {
            // 文件变更事件
            if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
                bool configChanged = false;
                if (bytesReturned == 0) {
                    // ReadDirectoryChangesW 通知缓冲区溢出，保守触发重检
                    configChanged = true;
                } else {
                    constexpr size_t headerSize = offsetof(FILE_NOTIFY_INFORMATION, FileName);
                    size_t offset = 0;
                    while (offset < bytesReturned) {
                        if (bytesReturned - offset < headerSize) {
                            configChanged = true;
                            break;
                        }
                        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                            buffer + offset);
                        const size_t maxNameBytes = bytesReturned - offset - headerSize;
                        if (info->FileNameLength > maxNameBytes) {
                            configChanged = true;
                            break;
                        }
                        std::wstring changedFile(info->FileName,
                                                 info->FileNameLength / sizeof(wchar_t));
                        if (_wcsicmp(changedFile.c_str(), fileName.c_str()) == 0) {
                            configChanged = true;
                        }
                        if (info->NextEntryOffset == 0) break;
                        if (info->NextEntryOffset < headerSize ||
                            info->NextEntryOffset > bytesReturned - offset ||
                            (info->NextEntryOffset % alignof(DWORD)) != 0) {
                            configChanged = true;
                            break;
                        }
                        offset += info->NextEntryOffset;
                    }
                }

                if (configChanged) {
                    // 可中断防抖：避免编辑器的多步替换产生中间状态，同时保证关闭迅速。
                    if (WaitForSingleObject(m_watchStopEvent, 150) == WAIT_OBJECT_0) break;
                    bool selfTriggered = false;
                    {
                        std::lock_guard ioLock(m_ioMutex);
                        if (m_hasFileStamp) {
                            std::error_code stampEc;
                            const auto wt = std::filesystem::last_write_time(m_configFilePath, stampEc);
                            if (!stampEc) {
                                const auto sz = std::filesystem::file_size(m_configFilePath, stampEc);
                                if (!stampEc && wt == m_lastLoadedWriteTime && sz == m_lastLoadedFileSize) {
                                    selfTriggered = true;
                                }
                            }
                        }
                    }
                    if (!selfTriggered && !hasPendingDirty()) {
                        LOG_INFO("检测到配置文件变更, 正在热加载...");
                        bool changed = false;
                        if (load(&changed) && changed) notifyChange("*");
                    }
                }
            }
        } else {
            // 收到停止信号
            CancelIoEx(hDir, &overlapped);
            break;
        }
    }

    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
    LOG_DEBUG("配置文件监控已停止");
}

ConfigManager::~ConfigManager() {
    if (m_watchRunning || m_watchThread.joinable() ||
        m_flushRunning || m_flushThread.joinable() || hasPendingDirty()) {
        shutdown();
    }
}

}  // namespace tools3000::core
