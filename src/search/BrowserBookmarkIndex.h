#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BrowserBookmarkIndex.h — 零依赖离线浏览器书签极速索引（按 mtime 懒加载）
//
// 核心设计原则:
//   1. 零冲突语法：独立 `bm:` 前缀（绝不占用 `b ` 避免与文件名冲突），同时支持
//      常规搜索可控轻权重混排（`/search/includeBookmarks`，默认开启）；
//   2. 零常驻线程 / 零轮询 / 超低内存：仅在用户显式搜索时按文件 mtime 节流刷新
//      （10s stat 节流 + 单文件 8MB 上限 + 全局 8000 条去重封顶，常规 < 300 KB）；
//   3. 跨浏览器自动发现：原生支持 Microsoft Edge / Google Chrome / Brave /
//      Vivaldi / Chromium 多 Profile 本地书签文件，纯只读不锁文件；
//   4. 独立于全盘索引服务：纯 `bm:` 查询 < 0.5ms 瞬出，严守 DEMAND_START 契约。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SEARCH_BROWSERBOOKMARKINDEX_H
#define TOOLS3000_SEARCH_BROWSERBOOKMARKINDEX_H

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tools3000::search {

struct BookmarkEntry {
    std::string title;
    std::string url;
    std::string folderPath;
    std::string browserSource; // "Edge" / "Chrome" / "Brave" / "Vivaldi" / "Chromium"
    std::string titleLower;
    std::string urlLower;
    std::string folderLower;
    uint64_t dateAddedMicros = 0; // Chromium WebKit 1601 微秒时间戳，可转 FILETIME
};

struct BookmarkQueryParseResult {
    bool isExplicitBookmarkQuery = false;
    std::string cleanQuery;
};

class BrowserBookmarkIndex final {
public:
    static BrowserBookmarkIndex& instance();

    /// 解析书签前缀：`bm:` 或 `b:`（大小写不敏感，严格放行 `b:\...` / `b:/...` 盘符路径，不抢占 `b `）
    static BookmarkQueryParseResult parseBookmarkQuery(std::string_view rawQuery);

    /// 判断普通查询是否适合混排浏览器书签（自动排除显式文件系统操作符与驱动器路径）
    static bool shouldMixBookmarksInGeneralQuery(std::string_view rawQuery) noexcept;

    /// 判断 URL 是否为允许安全打开的 HTTP/HTTPS 网址
    static bool isSafeHttpUrl(std::string_view url) noexcept;

    /// 提取标准化小写 Origin（如 "https://zhuanlan.zhihu.com/"，不含路径与查询串）
    static std::string extractOriginLower(std::string_view url);

    /// 从 Chromium 系 Bookmarks JSON 内容解析书签条目（纯函数，支持离线单元测试）
    static void parseChromiumBookmarkJson(
        const nlohmann::json& root,
        std::string_view browserSource,
        std::vector<BookmarkEntry>& out,
        size_t maxEntries = 8000);

    /// 在给定书签集合中执行多词相关度检索并返回排序后的 SearchResult JSON 数组（纯函数，便于单测）
    static nlohmann::json searchEntriesToJson(
        const std::vector<BookmarkEntry>& entries,
        std::string_view query,
        size_t limit,
        int minScore = 1);

    /// 懒刷新本机书签缓存并检索，返回标准 SearchResult JSON 数组
    nlohmann::json searchLocalBookmarks(std::string_view query, size_t limit, int minScore = 1);

    /// 纯离线读取书签原网站 Favicon（优先本机浏览器 Favicons SQLite 库，未命中时回退对应浏览器 Windows 原生图标，0 网络请求）
    std::string getBookmarkIconBase64(std::string_view url, std::string_view browserSource = "");

    /// 显式释放内存缓存（用于插件卸载冷路径）
    void clearCache();

private:
    BrowserBookmarkIndex() = default;

    struct TrackedBookmarkSource {
        std::filesystem::path filePath;
        std::string browserName;
    };

    static std::vector<TrackedBookmarkSource> discoverLocalBookmarkFiles();
    void refreshIfNeededLocked();

    std::mutex m_mutex;
    std::vector<BookmarkEntry> m_entries;
    std::unordered_map<std::wstring, std::filesystem::file_time_type> m_fileMtimes;
    std::chrono::steady_clock::time_point m_lastCheckTime{};
    bool m_loadedOnce = false;

    std::mutex m_faviconMutex;
    std::unordered_map<std::string, std::string> m_faviconCache;
};

}  // namespace tools3000::search

#endif  // TOOLS3000_SEARCH_BROWSERBOOKMARKINDEX_H
