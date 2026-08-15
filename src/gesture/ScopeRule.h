#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ScopeRule — 手势作用域规则引擎
//
// 决定在哪些窗口/进程中启用/禁用特定手势配置。
// 优先级: 窗口句柄 > 窗口类名 > 显式禁用规则 > 特殊系统目标(桌面/任务栏) > 进程名 > 全局默认
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_SCOPERULE_H
#define TOOLS3000_GESTURE_SCOPERULE_H

#include <windows.h>
#include <string>
#include <vector>
#include <optional>
#include <regex>
#include <list>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace tools3000::gesture {

/// 规则匹配模式
enum class MatchMode {
    Exact,      // 精确匹配
    Wildcard,   // 通配符 (*.exe)
    Regex       // 正则表达式
};

/// 规则作用效果
enum class RuleEffect {
    Enable,     // 在匹配的窗口中启用手势
    Disable,    // 在匹配的窗口中禁用手势
    UseProfile  // 使用指定的手势配置集
};

/// 单条作用域规则
struct ScopeRule {
    std::string id;              // 唯一标识
    std::string name;            // 人类可读名称 (如 "Chrome 浏览器", "VS Code")
    bool enabled = true;         // 规则是否启用

    // 匹配条件 (至少设置一个)
    std::string processName;     // 进程名 (如 "chrome.exe")
    std::string windowClass;     // 窗口类名 (如 "Chrome_WidgetWin_1")
    HWND windowHandle = nullptr; // 窗口句柄 (运行时动态捕获)
    MatchMode matchMode = MatchMode::Exact;

    // 效果
    RuleEffect effect = RuleEffect::Enable;
    std::string profileName;     // effect == UseProfile 时使用的配置集名称

    // 预编译正则与通配符缓存 (消除热路径动态编译开销)
    mutable std::optional<std::wregex> compiledClassRegex;
    mutable std::optional<std::wregex> compiledProcRegex;
    mutable bool classRegexAttempted = false;
    mutable bool procRegexAttempted = false;
    void compileRegexes() const;

    /// 检查此规则是否匹配给定的窗口
    bool matches(HWND hwnd, const std::wstring& processName, const std::wstring& className) const;

    /// 序列化
    nlohmann::json toJson() const;
    static ScopeRule fromJson(const nlohmann::json& j);
};

/// 作用域规则引擎
class ScopeRuleEngine {
public:
    /// 评估给定窗口应使用哪个手势配置
    /// @return profileName (空字符串表示使用全局默认配置, nullopt 表示手势被禁用)
    std::optional<std::string> evaluate(HWND hwnd) const;

    /// 纯上下文评估入口（支持无窗口句柄单元测试直接验证桌面/任务栏与自定义禁用优先级）
    std::optional<std::string> evaluateWindow(
        HWND hwnd,
        const std::wstring& processName,
        const std::wstring& className,
        bool isDesktop = false,
        bool isTaskbar = false) const;

    /// 添加规则
    void addRule(const ScopeRule& rule);

    /// 删除规则
    void removeRule(const std::string& ruleId);

    /// 批量添加规则
    void addRules(const std::vector<ScopeRule>& rules);

    /// 批量删除规则
    void removeRules(const std::vector<std::string>& ruleIds);

    /// 获取所有规则
    std::vector<ScopeRule> getRules() const;

    /// 清除所有规则
    void clearRules();

    /// 从 JSON 加载
    void loadFromJson(const nlohmann::json& j);

    /// 导出为 JSON
    nlohmann::json toJson() const;

private:
    std::vector<ScopeRule> m_rules;

    /// 获取窗口信息 (缓存友好)
    struct WindowInfo {
        HWND hwnd;
        std::wstring processName;
        std::wstring className;
    };

    WindowInfo getWindowInfo(HWND hwnd) const;
    std::optional<std::string> evaluateUnlocked(
        HWND hwnd,
        const std::wstring& className,
        bool isDesktop,
        bool isTaskbar,
        const std::wstring* knownProcessName) const;

    /// 清除所有缓存（规则变更时调用）
    void invalidateCache() const;

    // ── LRU 评估缓存 ─────────────────────────────────────────────────
    // 在鼠标钩子热路径上，前台窗口很少变化，缓存命中率 >95%
    struct CacheEntry {
        std::optional<std::string> result;
        std::list<uint64_t>::iterator lruIt;
    };
    static constexpr size_t MAX_CACHE_SIZE = 32;

    mutable std::unordered_map<uint64_t, CacheEntry> m_evaluateCache;
    mutable std::list<uint64_t> m_cacheLru;

    // PID → 进程名缓存
    mutable std::unordered_map<DWORD, std::wstring> m_processNameCache;
    mutable std::mutex m_mutex;
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_SCOPERULE_H
