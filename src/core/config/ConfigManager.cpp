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

    if (m_watchRunning.load()) {
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
        std::lock_guard lock(m_mutex);
        m_config = json();
    }

    LOG_INFO("配置管理器初始化, 配置文件路径={}", m_configFilePath.string());

    if (!load()) return false;

    // 启动文件监控线程
    m_watchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_watchStopEvent) {
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
        LOG_ERROR("无法启动配置监控线程: {}", e.what());
        return false;
    }
    return true;
}

void ConfigManager::shutdown() {
    if (!m_watchRunning.load() && !m_watchThread.joinable()) {
        return;
    }

    LOG_INFO("配置管理器正在关闭...");

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

    if (!save()) LOG_ERROR("配置管理器关闭时保存失败");
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
            std::lock_guard lock(m_mutex);
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
    {
        std::lock_guard lock(m_mutex);
        if (changed) *changed = m_config != loaded;
        m_config = std::move(loaded);
    }
    return true;
}

bool ConfigManager::save() const {
    std::lock_guard ioLock(m_ioMutex);
    json snapshot;
    {
        std::lock_guard lock(m_mutex);
        snapshot = m_config;
    }
    return writeSnapshotLocked(snapshot);
}

bool ConfigManager::writeSnapshotLocked(const json& snapshot) const {
    try {
        const std::string payload = snapshot.dump(2);
        if (WinUtils::atomicWriteFileWithFlush(m_configFilePath.wstring(), payload)) {
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

bool ConfigManager::setJsonValue(const std::string& key, json value) {
    json next;
    {
        std::lock_guard ioLock(m_ioMutex);
        {
            std::lock_guard lock(m_mutex);
            next = m_config;
        }
        try {
            next[json::json_pointer(key)] = std::move(value);
        } catch (const std::exception& e) {
            LOG_WARN("设置配置项失败: key={}, error={}", key, e.what());
            return false;
        }
        if (!writeSnapshotLocked(next)) return false;
        {
            std::lock_guard lock(m_mutex);
            m_config = std::move(next);
        }
    }
    notifyChange(key);
    return true;
}

bool ConfigManager::mergePatch(const json& patch, const std::string& notificationKey) {
    if (!patch.is_object()) {
        LOG_WARN("配置合并失败: patch 根节点不是对象");
        return false;
    }
    json next;
    {
        std::lock_guard ioLock(m_ioMutex);
        {
            std::lock_guard lock(m_mutex);
            next = m_config;
        }
        try {
            std::string existingPipeToken;
            auto tokenPtr = json::json_pointer("/search/pipeToken");
            if (next.contains(tokenPtr) && next[tokenPtr].is_string()) {
                existingPipeToken = next[tokenPtr].get<std::string>();
            }

            next.merge_patch(patch);

            if (!existingPipeToken.empty()) {
                next["search"]["pipeToken"] = existingPipeToken;
            }
        } catch (const std::exception& e) {
            LOG_WARN("配置合并失败: {}", e.what());
            return false;
        }
        if (!writeSnapshotLocked(next)) return false;
        {
            std::lock_guard lock(m_mutex);
            m_config = std::move(next);
        }
    }
    notifyChange(notificationKey);
    return true;
}

bool ConfigManager::has(const std::string& key) const {
    std::lock_guard lock(m_mutex);
    try {
        auto ptr = json::json_pointer(key);
        return m_config.contains(ptr);
    } catch (...) {
        return false;
    }
}

bool ConfigManager::remove(const std::string& key) {
    bool changed = false;
    json next;
    {
        std::lock_guard ioLock(m_ioMutex);
        {
            std::lock_guard lock(m_mutex);
            next = m_config;
        }
        try {
            // nlohmann::json 不直接支持 erase with pointer，需要手动导航
            // 这里简化处理：使用 JSON Patch
            auto ptr = json::json_pointer(key);
            if (next.contains(ptr)) {
                // 从路径中提取父路径和键名
                auto keyStr = key;
                auto lastSlash = keyStr.rfind('/');
                if (lastSlash != std::string::npos && lastSlash > 0) {
                    auto parentPath = keyStr.substr(0, lastSlash);
                    auto childKey = keyStr.substr(lastSlash + 1);
                    auto parentPtr = json::json_pointer(parentPath);
                    next.at(parentPtr).erase(childKey);
                    changed = true;
                } else if (lastSlash == 0) {
                    // 根级别的 key
                    auto childKey = keyStr.substr(1);
                    changed = next.erase(childKey) > 0;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("删除配置项失败: key={}, error={}", key, e.what());
            return false;
        }
        if (!changed) return true;
        if (!writeSnapshotLocked(next)) return false;
        {
            std::lock_guard lock(m_mutex);
            m_config = std::move(next);
        }
    }
    notifyChange(key);
    return true;
}

std::string ConfigManager::toJsonString(int indent) const {
    std::lock_guard lock(m_mutex);
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
            std::lock_guard lock(m_mutex);
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
            std::lock_guard lock(m_mutex);
            m_config = std::move(empty);
        }
    }
    notifyChange("*");
    LOG_INFO("配置已重置");
    return true;
}

size_t ConfigManager::onChange(ConfigChangeCallback callback) {
    size_t id = m_nextCallbackId.fetch_add(1);
    std::lock_guard lock(m_mutex);
    m_callbacks.emplace_back(id, std::move(callback));
    return id;
}

void ConfigManager::removeOnChange(size_t callbackId) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_callbacks, [callbackId](const auto& pair) {
        return pair.first == callbackId;
    });
}

void ConfigManager::notifyChange(const std::string& key) {
    std::vector<std::pair<size_t, ConfigChangeCallback>> callbacks;
    {
        std::lock_guard lock(m_mutex);
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
            std::lock_guard lock(m_mutex);
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
            if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE) && bytesReturned > 0) {
                bool configChanged = false;
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
                while (info) {
                    std::wstring changedFile(info->FileName,
                                             info->FileNameLength / sizeof(wchar_t));
                    if (_wcsicmp(changedFile.c_str(), fileName.c_str()) == 0) configChanged = true;
                    if (info->NextEntryOffset == 0) break;
                    info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                        reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
                }

                if (configChanged) {
                    // 可中断防抖：避免编辑器的多步替换产生中间状态，同时保证关闭迅速。
                    if (WaitForSingleObject(m_watchStopEvent, 150) == WAIT_OBJECT_0) break;
                    LOG_INFO("检测到配置文件变更, 正在热加载...");
                    bool changed = false;
                    if (load(&changed) && changed) notifyChange("*");
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
    if (m_watchRunning || m_watchThread.joinable()) {
        shutdown();
    }
}

}  // namespace tools3000::core
