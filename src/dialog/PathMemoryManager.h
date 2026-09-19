/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace tools3000::dialog {

struct AppMemoryItem {
    std::string processName;
    std::string lastPath;
    std::string fixedWorkspace;
    bool isFixed{false};
    uint64_t lastUsedTimestamp{0};
};

class PathMemoryManager {
public:
    static PathMemoryManager& instance();

    // 加载与保存配置
    void loadConfig();
    void saveConfig();

    // 基础开关
    bool isEnabled() const;
    void setEnabled(bool enabled);

    bool isPerAppMemoryEnabled() const;
    void setPerAppMemoryEnabled(bool enabled);

    bool isQuickSwitchEnabled() const;
    void setQuickSwitchEnabled(bool enabled);

    bool isRibbonEnabled() const;
    void setRibbonEnabled(bool enabled);

    std::string getRibbonPosition() const;
    void setRibbonPosition(const std::string& pos);

    // 应用程序专属记忆与固定母工作区
    void recordAppPath(const std::string& processName, const std::string& folderPath);
    void recordAppSelection(const std::string& processName,
                            const std::string& selectedPath,
                            const std::string& currentFolder = {});
    std::string getAppPath(const std::string& processName) const;
    std::string getEffectiveAppPath(const std::string& processName) const;
    void setAppFixedWorkspace(const std::string& processName, const std::string& workspacePath, bool isFixed);
    void removeAppMemory(const std::string& processName);
    void clearAppMemories();
    std::vector<AppMemoryItem> getAllAppMemories() const;

    // 全局最近使用文件夹 (MRU)
    void recordRecentPath(const std::string& folderPath);
    std::vector<std::string> getRecentPaths(size_t maxCount = 10) const;
    void clearRecentPaths();

    // 常用工作区收藏 (Favorites)
    std::vector<std::string> getFavorites() const;
    void setFavorites(const std::vector<std::string>& favorites);
    void addFavorite(const std::string& folderPath);
    void removeFavorite(const std::string& folderPath);

    // 进程黑名单 (Blacklist)
    bool isProcessBlacklisted(const std::string& processName) const;
    std::vector<std::string> getBlacklist() const;
    void setBlacklist(const std::vector<std::string>& blacklist);

    // 将文件/文件夹选择结果统一转换为后续对话框应进入的目录。
    // - 选择当前目录的子文件夹：保留其所在层级，便于下次选择同级项
    // - 选择当前目录本身：保留当前目录
    // - 文件选择/保存文件名：取所在目录
    // - 相对文件名：相对 currentFolder 解析
    static std::string directoryForSelection(const std::string& selectedPath,
                                             const std::string& currentFolder = {});

private:
    PathMemoryManager();
    ~PathMemoryManager() = default;

    static std::string canonicalProcessKey(const std::string& processName);

    mutable std::mutex m_mutex;
    bool m_enabled{true};
    bool m_perAppMemory{true};
    bool m_quickSwitch{true};
    bool m_ribbonEnabled{true};
    std::string m_ribbonPosition{"top-right"};

    std::unordered_map<std::string, AppMemoryItem> m_appMemories;
    std::vector<std::string> m_recentPaths;
    std::vector<std::string> m_favorites;
    std::vector<std::string> m_blacklist;
};

} // namespace tools3000::dialog
