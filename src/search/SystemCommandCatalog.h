#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SystemCommandCatalog.h — 进程内超轻量 Windows 极客系统命令静态白名单目录
//
// 核心设计原则:
//   1. 静态只读表 (< 16 KB)，< 0.05 ms 极速检索，0 外部服务依赖，严守 DEMAND_START；
//   2. 中英双语别名 + 拼音全拼/首字母 + 8 国语言 titleKey（输入 env/环境变量/hjbl 瞬达）；
//   3. 零信任安全执行：仅通过静态白名单 commandId 解析固定 executable + args，
//      严禁把前端传入的任意字符串直接交给 ShellExecuteW。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SEARCH_SYSTEMCOMMANDCATALOG_H
#define TOOLS3000_SEARCH_SYSTEMCOMMANDCATALOG_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::search {

struct SystemCommandSpec {
    const char* id;
    const char* titleKey;
    const char* defaultTitleEn;
    const char* defaultTitleZh;
    const char* commandHint;
    const char* aliases;
    const wchar_t* execFile;
    const wchar_t* execParams;
    const wchar_t* fallbackExecFile;
    const char* iconHint;
    bool supportsRunAs;
    bool isHostsEditor;
};

struct CommandQueryParseResult {
    bool isExplicitCommandQuery = false;
    std::string cleanQuery;
};

struct SystemCommandMatch {
    const SystemCommandSpec* spec = nullptr;
    int score = 0;
    bool isExactAlias = false;
};

class SystemCommandCatalog final {
public:
    /// 获取全部内置极客系统命令只读清单
    static std::span<const SystemCommandSpec> allCommands() noexcept;

    /// 按静态白名单 commandId 精确查找命令（非法或未知 ID 返回 nullptr）
    static const SystemCommandSpec* findById(std::string_view commandId) noexcept;

    /// 从 "cmd://<id>" URI 中安全提取 commandId（仅允许 [a-z0-9_-]，拒绝任何注入字符）
    static std::string extractCommandIdFromUri(std::string_view uri) noexcept;

    /// 解析显式 `cmd:` 前缀查询（大小写不敏感）
    static CommandQueryParseResult parseCommandQuery(std::string_view rawQuery);

    /// 判断常规查询是否适合混排系统命令/应用（自动排除路径、通配符及文件专用算子）
    static bool shouldMixLauncherInGeneralQuery(std::string_view rawQuery) noexcept;

    /// 离线检索匹配的系统命令（< 0.05 ms）
    static std::vector<SystemCommandMatch> searchCommands(
        std::string_view query,
        size_t limit = 20,
        bool allowEmptyQuery = false);

    /// 将匹配的系统命令序列化为标准 SearchResult JSON 数组
    static nlohmann::json searchCommandsToJson(
        std::string_view query,
        size_t limit = 20,
        bool allowEmptyQuery = false,
        bool preferChinese = true);

    /// 按静态白名单安全执行系统命令（未在白名单登记的 commandId 一律拒绝返回 false）
    static bool executeWhitelistedCommand(std::string_view commandId, bool asAdmin = false) noexcept;
};

}  // namespace tools3000::search

#endif  // TOOLS3000_SEARCH_SYSTEMCOMMANDCATALOG_H
