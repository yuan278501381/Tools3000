/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#include "PathMemoryManager.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include <chrono>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace tools3000::dialog {

namespace {

std::string getKnownFolderUtf8(int csidl) {
    wchar_t path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, path))) {
        return tools3000::core::WinUtils::wstringToUtf8(path);
    }
    return "";
}

bool stringEqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) == std::tolower(static_cast<unsigned char>(cb));
    });
}

std::string normalizePathUtf8(const std::string& value) {
    if (value.empty()) return {};

    std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(value);
    while (wide.size() >= 2 &&
           ((wide.front() == L'"' && wide.back() == L'"') ||
            (wide.front() == L'\'' && wide.back() == L'\''))) {
        wide = wide.substr(1, wide.size() - 2);
    }
    if (wide.empty()) return {};

    std::filesystem::path path(wide);
    path = path.lexically_normal();
    return tools3000::core::WinUtils::wstringToUtf8(path.native());
}

bool isExistingDirectory(const std::filesystem::path& path) {
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void pushRecentPath(std::vector<std::string>& recentPaths, const std::string& folderPath) {
    recentPaths.erase(
        std::remove_if(recentPaths.begin(), recentPaths.end(), [&](const std::string& item) {
            return stringEqualsIgnoreCase(item, folderPath);
        }),
        recentPaths.end());
    recentPaths.insert(recentPaths.begin(), folderPath);
    if (recentPaths.size() > 20) recentPaths.resize(20);
}

} // namespace

PathMemoryManager& PathMemoryManager::instance() {
    static PathMemoryManager s_instance;
    return s_instance;
}

PathMemoryManager::PathMemoryManager() {
    loadConfig();
}

void PathMemoryManager::loadConfig() {
    std::lock_guard lock(m_mutex);
    auto& cfg = tools3000::core::ConfigManager::instance();

    if (cfg.has("/plugins/dialogenhancer/enabled")) {
        m_enabled = cfg.get<bool>("/plugins/dialogenhancer/enabled", true);
    } else {
        m_enabled = cfg.get<bool>("/dialog/enabled", true);
    }
    m_perAppMemory = cfg.get<bool>("/dialog/perAppMemory", true);
    m_quickSwitch = cfg.get<bool>("/dialog/quickSwitch", true);
    m_ribbonEnabled = cfg.get<bool>("/dialog/ribbonEnabled", true);
    m_ribbonPosition = cfg.get<std::string>("/dialog/ribbonPosition", "top-right");

    std::vector<std::string> defaultFavs;
    std::string desktop = getKnownFolderUtf8(CSIDL_DESKTOPDIRECTORY);
    std::string profile = getKnownFolderUtf8(CSIDL_PROFILE);
    std::string downloads = profile.empty() ? std::string{} : profile + "\\Downloads";
    std::string docs = getKnownFolderUtf8(CSIDL_MYDOCUMENTS);
    if (!desktop.empty()) defaultFavs.push_back(desktop);
    if (!downloads.empty()) defaultFavs.push_back(downloads);
    if (!docs.empty() && defaultFavs.size() < 3) defaultFavs.push_back(docs);

    m_favorites = cfg.get<std::vector<std::string>>("/dialog/favorites", defaultFavs);
    m_recentPaths = cfg.get<std::vector<std::string>>("/dialog/recentPaths", {});
    m_blacklist = cfg.get<std::vector<std::string>>("/dialog/blacklist", {});

    // 加载 AppMemories map
    m_appMemories.clear();
    auto memJson = cfg.get<nlohmann::json>("/dialog/appMemories", nlohmann::json::object());
    if (memJson.is_object()) {
        for (auto& [proc, item] : memJson.items()) {
            if (item.is_object()) {
                AppMemoryItem mem;
                const std::string key = canonicalProcessKey(proc);
                if (key.empty()) continue;
                mem.processName = item.value("processName", proc);
                mem.lastPath = item.value("path", "");
                mem.fixedWorkspace = item.value("fixedWorkspace", "");
                mem.isFixed = item.value("isFixed", false);
                mem.lastUsedTimestamp = item.value("timestamp", 0ULL);
                if (!mem.lastPath.empty() || !mem.fixedWorkspace.empty()) {
                    m_appMemories[key] = mem;
                }
            }
        }
    }
}

void PathMemoryManager::saveConfig() {
    nlohmann::json dialogJson;
    {
        std::lock_guard lock(m_mutex);
        nlohmann::json memJson = nlohmann::json::object();
        for (const auto& [key, item] : m_appMemories) {
            memJson[key] = {
                {"processName", item.processName},
                {"path", item.lastPath},
                {"fixedWorkspace", item.fixedWorkspace},
                {"isFixed", item.isFixed},
                {"timestamp", item.lastUsedTimestamp}
            };
        }
        dialogJson = {
            {"enabled", m_enabled},
            {"perAppMemory", m_perAppMemory},
            {"quickSwitch", m_quickSwitch},
            {"ribbonEnabled", m_ribbonEnabled},
            {"ribbonPosition", m_ribbonPosition},
            {"favorites", m_favorites},
            {"recentPaths", m_recentPaths},
            {"blacklist", m_blacklist},
            {"appMemories", std::move(memJson)}
        };
    }

    // DialogEnhancer owns the complete /dialog subtree. Replacing that subtree
    // atomically removes legacy differently-cased EXE keys (for example
    // Code.exe/code.exe) that JSON merge-patch would otherwise retain forever.
    tools3000::core::ConfigManager::instance().set("/dialog", std::move(dialogJson));
    tools3000::core::ConfigManager::instance().set<bool>("/plugins/dialogenhancer/enabled", m_enabled);
    tools3000::core::ConfigManager::instance().set<bool>("/plugins/dialog_enhancer/enabled", m_enabled);
}

bool PathMemoryManager::isEnabled() const {
    std::lock_guard lock(m_mutex);
    return m_enabled;
}

bool PathMemoryManager::isPerAppMemoryEnabled() const {
    std::lock_guard lock(m_mutex);
    return m_perAppMemory;
}

bool PathMemoryManager::isQuickSwitchEnabled() const {
    std::lock_guard lock(m_mutex);
    return m_quickSwitch;
}

bool PathMemoryManager::isRibbonEnabled() const {
    std::lock_guard lock(m_mutex);
    return m_ribbonEnabled;
}

std::string PathMemoryManager::getRibbonPosition() const {
    std::lock_guard lock(m_mutex);
    return m_ribbonPosition;
}

void PathMemoryManager::setEnabled(bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_enabled = enabled;
    }
    saveConfig();
}

void PathMemoryManager::setPerAppMemoryEnabled(bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_perAppMemory = enabled;
    }
    saveConfig();
}

void PathMemoryManager::setQuickSwitchEnabled(bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_quickSwitch = enabled;
    }
    saveConfig();
}

void PathMemoryManager::setRibbonEnabled(bool enabled) {
    {
        std::lock_guard lock(m_mutex);
        m_ribbonEnabled = enabled;
    }
    saveConfig();
}

void PathMemoryManager::setRibbonPosition(const std::string& pos) {
    {
        std::lock_guard lock(m_mutex);
        m_ribbonPosition = pos;
    }
    saveConfig();
}

void PathMemoryManager::recordAppPath(const std::string& processName, const std::string& folderPath) {
    if (processName.empty() || folderPath.empty()) return;
    if (isProcessBlacklisted(processName)) return;

    const std::string key = canonicalProcessKey(processName);
    const std::string normalizedFolder = normalizePathUtf8(folderPath);
    if (key.empty() || normalizedFolder.empty()) return;

    {
        std::lock_guard lock(m_mutex);
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        auto& item = m_appMemories[key];
        item.processName = processName;
        item.lastPath = normalizedFolder;
        item.lastUsedTimestamp = now;
        pushRecentPath(m_recentPaths, normalizedFolder);
    }
    saveConfig();
}

void PathMemoryManager::recordAppSelection(const std::string& processName,
                                           const std::string& selectedPath,
                                           const std::string& currentFolder) {
    const std::string directory = directoryForSelection(selectedPath, currentFolder);
    if (!directory.empty()) recordAppPath(processName, directory);
}

std::string PathMemoryManager::getAppPath(const std::string& processName) const {
    if (processName.empty()) return "";
    const std::string key = canonicalProcessKey(processName);
    std::lock_guard lock(m_mutex);
    auto it = m_appMemories.find(key);
    if (it != m_appMemories.end()) {
        return it->second.lastPath;
    }
    return "";
}

std::string PathMemoryManager::getEffectiveAppPath(const std::string& processName) const {
    if (processName.empty()) return "";
    const std::string key = canonicalProcessKey(processName);
    std::lock_guard lock(m_mutex);
    auto it = m_appMemories.find(key);
    if (it != m_appMemories.end()) {
        if (it->second.isFixed && !it->second.fixedWorkspace.empty()) {
            return it->second.fixedWorkspace;
        }

        return it->second.lastPath;
    }
    return "";
}

void PathMemoryManager::setAppFixedWorkspace(const std::string& processName, const std::string& workspacePath, bool isFixed) {
    if (processName.empty()) return;
    const std::string key = canonicalProcessKey(processName);
    {
        std::lock_guard lock(m_mutex);
        auto& item = m_appMemories[key];
        item.processName = processName;
        item.fixedWorkspace = workspacePath;
        item.isFixed = isFixed;
        if (item.lastPath.empty()) item.lastPath = workspacePath;
    }
    saveConfig();
}

void PathMemoryManager::removeAppMemory(const std::string& processName) {
    const std::string key = canonicalProcessKey(processName);
    {
        std::lock_guard lock(m_mutex);
        m_appMemories.erase(key);
    }
    saveConfig();
}

void PathMemoryManager::clearAppMemories() {
    {
        std::lock_guard lock(m_mutex);
        m_appMemories.clear();
    }
    saveConfig();
}

std::vector<AppMemoryItem> PathMemoryManager::getAllAppMemories() const {
    std::lock_guard lock(m_mutex);
    std::vector<AppMemoryItem> list;
    list.reserve(m_appMemories.size());
    for (const auto& [_, item] : m_appMemories) {
        list.push_back(item);
    }
    std::sort(list.begin(), list.end(), [](const AppMemoryItem& a, const AppMemoryItem& b) {
        return a.lastUsedTimestamp > b.lastUsedTimestamp;
    });
    return list;
}

void PathMemoryManager::recordRecentPath(const std::string& folderPath) {
    const std::string normalizedFolder = normalizePathUtf8(folderPath);
    if (normalizedFolder.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        if (!m_recentPaths.empty() && stringEqualsIgnoreCase(m_recentPaths.front(), normalizedFolder)) {
            return;
        }
        pushRecentPath(m_recentPaths, normalizedFolder);
    }
    saveConfig();
}

std::vector<std::string> PathMemoryManager::getRecentPaths(size_t maxCount) const {
    std::lock_guard lock(m_mutex);
    if (m_recentPaths.size() <= maxCount) {
        return m_recentPaths;
    }
    return std::vector<std::string>(m_recentPaths.begin(), m_recentPaths.begin() + maxCount);
}

void PathMemoryManager::clearRecentPaths() {
    {
        std::lock_guard lock(m_mutex);
        m_recentPaths.clear();
    }
    saveConfig();
}

std::vector<std::string> PathMemoryManager::getFavorites() const {
    std::lock_guard lock(m_mutex);
    return m_favorites;
}

void PathMemoryManager::setFavorites(const std::vector<std::string>& favorites) {
    {
        std::lock_guard lock(m_mutex);
        m_favorites = favorites;
    }
    saveConfig();
}

void PathMemoryManager::addFavorite(const std::string& folderPath) {
    if (folderPath.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        if (std::find(m_favorites.begin(), m_favorites.end(), folderPath) == m_favorites.end()) {
            m_favorites.push_back(folderPath);
        }
    }
    saveConfig();
}

void PathMemoryManager::removeFavorite(const std::string& folderPath) {
    {
        std::lock_guard lock(m_mutex);
        auto it = std::remove(m_favorites.begin(), m_favorites.end(), folderPath);
        m_favorites.erase(it, m_favorites.end());
    }
    saveConfig();
}

bool PathMemoryManager::isProcessBlacklisted(const std::string& processName) const {
    if (processName.empty()) return false;
    std::lock_guard lock(m_mutex);
    for (const auto& item : m_blacklist) {
        if (stringEqualsIgnoreCase(processName, item)) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> PathMemoryManager::getBlacklist() const {
    std::lock_guard lock(m_mutex);
    return m_blacklist;
}

void PathMemoryManager::setBlacklist(const std::vector<std::string>& blacklist) {
    {
        std::lock_guard lock(m_mutex);
        m_blacklist = blacklist;
    }
    saveConfig();
}

std::string PathMemoryManager::canonicalProcessKey(const std::string& processName) {
    if (processName.empty()) return {};

    std::string key = processName;
    const size_t slash = key.find_last_of("\\/");
    if (slash != std::string::npos) key.erase(0, slash + 1);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return key;
}

std::string PathMemoryManager::directoryForSelection(const std::string& selectedPath,
                                                     const std::string& currentFolder) {
    if (selectedPath.empty()) {
        if (!currentFolder.empty()) {
            std::filesystem::path current(
                tools3000::core::WinUtils::utf8ToWstring(currentFolder));
            if (isExistingDirectory(current)) {
                return tools3000::core::WinUtils::wstringToUtf8(current.lexically_normal().native());
            }
        }
        return {};
    }

    std::filesystem::path selected(
        tools3000::core::WinUtils::utf8ToWstring(selectedPath));
    if (selected.empty()) return {};

    std::filesystem::path current;
    if (!currentFolder.empty()) {
        current = std::filesystem::path(
            tools3000::core::WinUtils::utf8ToWstring(currentFolder)).lexically_normal();
    }
    if (selected.is_relative()) {
        if (current.empty()) return {};
        selected = current / selected;
    }
    selected = selected.lexically_normal();

    DWORD attrs = GetFileAttributesW(selected.c_str());
    std::filesystem::path directory;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // A folder picker confirms a child shown inside currentFolder. The
            // useful recall location is that containing folder so the next
            // dialog opens at the same choice level, not inside the old child.
            const std::filesystem::path parent = selected.parent_path().lexically_normal();
            const bool selectedChildOfCurrent = !current.empty() &&
                _wcsicmp(parent.c_str(), current.c_str()) == 0;
            directory = selectedChildOfCurrent ? current : selected;
        } else {
            directory = selected.parent_path();
        }
    } else {
        // 保存对话框里的目标文件在确认前通常尚不存在；只要父目录真实存在，
        // 就把它视为“文件路径”，记住父目录。绝不凭扩展名或本地化文案猜测。
        const std::filesystem::path parent = selected.parent_path();
        if (isExistingDirectory(parent)) {
            directory = parent;
        } else if (!current.empty() && isExistingDirectory(current)) {
            directory = current;
        }
    }

    if (directory.empty()) return {};
    return tools3000::core::WinUtils::wstringToUtf8(directory.lexically_normal().native());
}

} // namespace tools3000::dialog
