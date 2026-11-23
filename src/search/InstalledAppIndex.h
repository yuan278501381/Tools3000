#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// InstalledAppIndex.h — 进程内超轻量应用索引 + 统一 Frecency + 3 档融合排序器
//
// 核心设计原则:
//   1. 超轻量内存 (< 500 KB)，零后台轮询线程，按需懒加载 + 60s 节流刷新；
//   2. 100% 进程内离线运行，支持 `app:` 显式前缀，严守 DEMAND_START 不唤醒搜索服务；
//   3. 全链路 Unicode *W Win32 API + WinUtils::pathToUtf8，严禁 .string() 或 ANSI API；
//   4. 提供主进程统一 Frecency 计数持久化与 3 档可配置结果优先级融合排序器。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SEARCH_INSTALLEDAPPINDEX_H
#define TOOLS3000_SEARCH_INSTALLEDAPPINDEX_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tools3000::search {

struct AppQueryParseResult {
    bool isExplicitAppQuery = false;
    std::string cleanQuery;
};

struct InstalledAppEntry {
    std::string name;
    std::string path;
    std::string source;        // "start_menu" | "app_paths" | "portable"
    std::string nameLower;
    std::string stemLower;
    std::string acronymLower;  // 英文词首字母 + 汉字拼音首字母
    std::string pathLower;
};

enum class RankingProfile {
    BalancedFrecency = 0,  // 均衡智能（默认）：高分/精确启动项置顶 (Cap=3) + Frecency 自动提权
    FilesFirst = 1,        // 文件优先：本地文件最前，仅精确别名全匹配 (如 env) 置顶
    LauncherFirst = 2      // 启动器优先：所有命中的应用与系统命令处于最高梯队，文件紧随其后
};

RankingProfile parseRankingProfile(std::string_view raw) noexcept;
const char* rankingProfileToString(RankingProfile profile) noexcept;

class InstalledAppIndex final {
public:
    static InstalledAppIndex& instance();

    /// 解析显式 `app:` 前缀查询（大小写不敏感）
    static AppQueryParseResult parseAppQuery(std::string_view rawQuery);

    /// 为 UTF-8 名称生成零外部依赖的轻量化拼音/英文词首字母索引（支持 GB2312 汉字与英文词首）
    static std::string buildInitialsAcronym(std::string_view utf8Name);

    /// 更新用户自定义的便携应用目录列表（不会触发后台线程，仅在下次检索时按需刷新）
    void setPortableAppDirs(std::vector<std::string> dirs);

    /// 获取当前配置的便携应用目录
    std::vector<std::string> getPortableAppDirs() const;

    /// 离线检索本地已安装/便携应用，返回标准 SearchResult JSON 数组
    nlohmann::json searchLocalApps(
        std::string_view query,
        size_t limit = 30,
        bool allowEmptyQuery = false);

    /// 清空缓存释放内存（冷路径/退出时调用）
    void clearCache();

private:
    InstalledAppIndex() = default;

    void ensureFreshLocked();
    void scanKnownStartMenuFolders(
        std::vector<InstalledAppEntry>& out,
        std::unordered_map<std::string, size_t>& seenPaths,
        std::unordered_map<std::string, size_t>& seenStems);
    void scanAppPathsRegistry(
        std::vector<InstalledAppEntry>& out,
        std::unordered_map<std::string, size_t>& seenPaths,
        std::unordered_map<std::string, size_t>& seenStems);
    void scanPortableDirectories(
        const std::vector<std::string>& dirs,
        std::vector<InstalledAppEntry>& out,
        std::unordered_map<std::string, size_t>& seenPaths,
        std::unordered_map<std::string, size_t>& seenStems);

    mutable std::mutex m_mutex;
    std::vector<InstalledAppEntry> m_entries;
    std::vector<std::string> m_portableDirs;
    uint64_t m_lastRefreshTickMs = 0;
    bool m_dirty = true;
};

struct FrecencyRecord {
    uint32_t runCount = 0;
    int64_t lastRunEpochMs = 0;
};

class LauncherFrecencyStore final {
public:
    static LauncherFrecencyStore& instance();

    /// 归一化运行键（统一转小写与反斜杠，保留 cmd:// / http(s):// 等协议前缀）
    static std::string normalizeKey(std::string_view rawKey);

    /// 计算基于频次与近期衰减的 Frecency 分数
    static double computeScore(const FrecencyRecord& rec, int64_t nowEpochMs) noexcept;

    /// 记录一次运行并异步/轻量落盘（仅在数据变化时写盘）
    void recordRun(std::string_view rawKey, int64_t nowEpochMsOverride = 0);

    /// 查询单个键的历史记录
    FrecencyRecord lookup(std::string_view rawKey);

    /// 查询单个键当前的 Frecency 分数
    double scoreFor(std::string_view rawKey, int64_t nowEpochMsOverride = 0);

    /// 将当前 Frecency 与 runCount 注入 JSON 结果数组（保留两端较高 runCount）
    void applyToResults(nlohmann::json& resultsArray, int64_t nowEpochMsOverride = 0);

    /// 立即刷盘保存脏数据
    void flush();

    /// 清空内存缓存（不删除磁盘文件）
    void clearCache();

    /// 测试辅助：重定向持久化路径并重置内存状态
    void setStoragePathForTesting(const std::filesystem::path& path);
    void resetForTesting();

private:
    LauncherFrecencyStore() = default;

    void ensureLoadedLocked();
    bool persistLocked() const;
    std::filesystem::path resolveStoragePathLocked() const;
    void pruneLocked();

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, FrecencyRecord> m_records;
    std::filesystem::path m_customStoragePath;
    bool m_loaded = false;
    bool m_dirty = false;
};

struct FusionOptions {
    RankingProfile profile = RankingProfile::BalancedFrecency;
    size_t maxTotalResults = 100;
    size_t balancedTopLauncherCap = 3;
    size_t maxLeadFilesBeforeBookmarks = 3;
    int balancedScoreThreshold = 700;
    double balancedFrecencyThreshold = 200.0;
};

/// 3 档可配置结果优先级融合与去重引擎（纯函数，可直接用于防回退单元测试）
nlohmann::json fuseSearchResults(
    const nlohmann::json& filesJson,
    const nlohmann::json& appsJson,
    const nlohmann::json& commandsJson,
    const nlohmann::json& bookmarksJson,
    const FusionOptions& options = {});

}  // namespace tools3000::search

#endif  // TOOLS3000_SEARCH_INSTALLEDAPPINDEX_H
