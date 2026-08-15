// ─────────────────────────────────────────────────────────────────────────────
// ScopeRule.cpp — 作用域规则引擎实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/ScopeRule.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <tlhelp32.h>
#include <unordered_map>
#include <list>

namespace tools3000::gesture {

namespace {

std::wstring wildcardToRegex(const std::wstring& pattern) {
    std::wstring result;
    result.reserve(pattern.size() * 2);
    for (const wchar_t ch : pattern) {
        if (ch == L'*') result += L".*";
        else if (ch == L'?') result += L'.';
        else {
            if (std::wstring_view(L".^$|()[]{}+\\").find(ch) != std::wstring_view::npos) {
                result += L'\\';
            }
            result += ch;
        }
    }
    return result;
}

std::optional<std::string> applyMatchedRuleEffect(const ScopeRule& rule) {
    if (rule.effect == RuleEffect::Disable) return std::nullopt;
    if (rule.effect == RuleEffect::UseProfile) return rule.profileName;
    return std::string{};
}

}  // namespace

// ── ScopeRule::compileRegexes ────────────────────────────────────────────────

void ScopeRule::compileRegexes() const {
    if (!windowClass.empty()) {
        classRegexAttempted = true;
        std::wstring wideClass = tools3000::core::WinUtils::utf8ToWstring(windowClass);
        try {
            if (matchMode == MatchMode::Wildcard) {
                compiledClassRegex.emplace(wildcardToRegex(wideClass), std::regex_constants::icase);
            } else if (matchMode == MatchMode::Regex) {
                compiledClassRegex.emplace(wideClass, std::regex_constants::icase);
            }
        } catch (const std::exception& error) {
            LOG_WARN("预编译作用域窗口类正则失败: {}", error.what());
            compiledClassRegex.reset();
        } catch (...) {
            compiledClassRegex.reset();
        }
    }

    if (!processName.empty()) {
        procRegexAttempted = true;
        std::wstring wideProcName = tools3000::core::WinUtils::utf8ToWstring(processName);
        try {
            if (matchMode == MatchMode::Wildcard) {
                compiledProcRegex.emplace(wildcardToRegex(wideProcName), std::regex_constants::icase);
            } else if (matchMode == MatchMode::Regex) {
                compiledProcRegex.emplace(wideProcName, std::regex_constants::icase);
            }
        } catch (const std::exception& error) {
            LOG_WARN("预编译作用域进程名正则失败: {}", error.what());
            compiledProcRegex.reset();
        } catch (...) {
            compiledProcRegex.reset();
        }
    }
}

// ── ScopeRule::matches ───────────────────────────────────────────────────────

bool ScopeRule::matches(HWND hwnd, const std::wstring& procName, const std::wstring& className) const {
    if (!enabled) return false;

    // 优先级 1: 窗口句柄精确匹配
    if (windowHandle != nullptr) {
        return hwnd == windowHandle;
    }

    // 优先级 2: 窗口类名匹配 (使用预编译缓存)
    if (!windowClass.empty()) {
        std::wstring wideClass = tools3000::core::WinUtils::utf8ToWstring(windowClass);
        switch (matchMode) {
            case MatchMode::Exact:
                if (className == wideClass) return true;
                break;
            case MatchMode::Wildcard:
            case MatchMode::Regex: {
                if (!classRegexAttempted) {
                    compileRegexes();
                }
                if (compiledClassRegex.has_value()) {
                    try {
                        if (std::regex_match(className, *compiledClassRegex)) return true;
                    } catch (...) {}
                }
                break;
            }
        }
    }

    // 优先级 3: 进程名匹配 (使用预编译缓存)
    if (!processName.empty() && !procName.empty()) {
        std::wstring wideProcName = tools3000::core::WinUtils::utf8ToWstring(processName);
        switch (matchMode) {
            case MatchMode::Exact: {
                std::wstring lowerProc = procName;
                std::wstring lowerTarget = wideProcName;
                std::transform(lowerProc.begin(), lowerProc.end(), lowerProc.begin(), ::towlower);
                std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::towlower);
                if (lowerProc == lowerTarget) return true;
                break;
            }
            case MatchMode::Wildcard:
            case MatchMode::Regex: {
                if (!procRegexAttempted) {
                    compileRegexes();
                }
                if (compiledProcRegex.has_value()) {
                    try {
                        if (std::regex_match(procName, *compiledProcRegex)) return true;
                    } catch (...) {}
                }
                break;
            }
        }
    }

    return false;
}

// ── ScopeRule JSON ───────────────────────────────────────────────────────────

nlohmann::json ScopeRule::toJson() const {
    return {
        {"id", id},
        {"name", name},
        {"enabled", enabled},
        {"processName", processName},
        {"windowClass", windowClass},
        {"matchMode", static_cast<int>(matchMode)},
        {"effect", static_cast<int>(effect)},
        {"profileName", profileName}
    };
}

ScopeRule ScopeRule::fromJson(const nlohmann::json& j) {
    ScopeRule rule;
    rule.id = j.value("id", "");
    rule.name = j.value("name", "");
    rule.enabled = j.value("enabled", true);
    rule.processName = j.value("processName", "");
    rule.windowClass = j.value("windowClass", "");
    const int matchMode = j.value("matchMode", 0);
    const int effect = j.value("effect", 0);
    rule.matchMode = static_cast<MatchMode>(std::clamp(matchMode, 0, 2));
    rule.effect = static_cast<RuleEffect>(std::clamp(effect, 0, 2));
    rule.profileName = j.value("profileName", "");
    rule.compileRegexes();
    return rule;
}

// ── ScopeRuleEngine ──────────────────────────────────────────────────────────

std::optional<std::string> ScopeRuleEngine::evaluateUnlocked(
    HWND hwnd,
    const std::wstring& className,
    bool isDesktop,
    bool isTaskbar,
    const std::wstring* knownProcessName) const {

    // 1. 窗口句柄规则（最高优先级，仅需 hwnd）
    if (hwnd != nullptr) {
        for (const auto& rule : m_rules) {
            if (rule.windowHandle != nullptr && rule.matches(hwnd, L"", className)) {
                return applyMatchedRuleEffect(rule);
            }
        }
    }

    // 2. 窗口类名规则（显式针对类名如 Progman/WorkerW/Shell_TrayWnd 的禁用或专属配置优先于内置特殊目标）
    if (!className.empty()) {
        for (const auto& rule : m_rules) {
            if (rule.windowHandle == nullptr && !rule.windowClass.empty() &&
                rule.matches(hwnd, L"", className)) {
                return applyMatchedRuleEffect(rule);
            }
        }
    }

    // 3. 若目标是桌面或任务栏：检查是否存在进程级显式禁用规则（如用户将 explorer.exe 加入禁用列表）
    if (isDesktop || isTaskbar) {
        bool hasProcessDisableRule = false;
        for (const auto& rule : m_rules) {
            if (rule.enabled && rule.effect == RuleEffect::Disable &&
                rule.windowHandle == nullptr && rule.windowClass.empty() &&
                !rule.processName.empty()) {
                hasProcessDisableRule = true;
                break;
            }
        }
        if (hasProcessDisableRule) {
            const std::wstring procName = knownProcessName
                ? *knownProcessName
                : (hwnd ? getWindowInfo(hwnd).processName : std::wstring{});
            if (!procName.empty()) {
                for (const auto& rule : m_rules) {
                    if (rule.enabled && rule.effect == RuleEffect::Disable &&
                        rule.windowHandle == nullptr && rule.windowClass.empty() &&
                        !rule.processName.empty() &&
                        rule.matches(hwnd, procName, className)) {
                        return std::nullopt;
                    }
                }
            }
        }
        return isDesktop ? std::optional<std::string>("special_desktop")
                         : std::optional<std::string>("special_taskbar");
    }

    // 4. 普通窗口的进程名规则：延迟按需解析进程名，无进程名规则时彻底杜绝 OpenProcess 开销
    bool hasProcessRule = false;
    for (const auto& rule : m_rules) {
        if (rule.enabled && rule.windowHandle == nullptr && rule.windowClass.empty() && !rule.processName.empty()) {
            hasProcessRule = true;
            break;
        }
    }

    if (hasProcessRule) {
        const std::wstring procName = knownProcessName
            ? *knownProcessName
            : (hwnd ? getWindowInfo(hwnd).processName : std::wstring{});
        if (!procName.empty()) {
            for (const auto& rule : m_rules) {
                if (rule.windowHandle == nullptr && rule.windowClass.empty() && !rule.processName.empty() &&
                    rule.matches(hwnd, procName, className)) {
                    return applyMatchedRuleEffect(rule);
                }
            }
        }
    }

    return std::string{};
}

std::optional<std::string> ScopeRuleEngine::evaluateWindow(
    HWND hwnd,
    const std::wstring& processName,
    const std::wstring& className,
    bool isDesktop,
    bool isTaskbar) const {
    std::lock_guard lock(m_mutex);
    return evaluateUnlocked(hwnd, className, isDesktop, isTaskbar, &processName);
}

std::optional<std::string> ScopeRuleEngine::evaluate(HWND hwnd) const {
    std::lock_guard lock(m_mutex);
    // ── LRU 缓存：按 (PID, className, hwndHash) 缓存评估结果 ──────────
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto className = tools3000::core::WinUtils::getWindowClassName(hwnd);
    const bool isDesktop = tools3000::core::WinUtils::isDesktopWindow(hwnd);
    const bool isTaskbar = !isDesktop && tools3000::core::WinUtils::isTaskbarWindow(hwnd);

    const bool hasHandleRule = std::any_of(m_rules.begin(), m_rules.end(), [](const ScopeRule& r) {
        return r.enabled && r.windowHandle != nullptr;
    });
    uint64_t cacheKey = (static_cast<uint64_t>(pid) << 32) ^
                        (std::hash<std::wstring>{}(className) & 0xFFFFFFFFULL);
    if (isDesktop) cacheKey ^= 0xD35C709FULL;
    if (isTaskbar) cacheKey ^= 0x7A5CBA21ULL;
    if (hasHandleRule) {
        cacheKey ^= reinterpret_cast<uintptr_t>(hwnd) * 0x9E3779B97F4A7C15ULL;
    }

    {
        auto it = m_evaluateCache.find(cacheKey);
        if (it != m_evaluateCache.end()) {
            m_cacheLru.splice(m_cacheLru.begin(), m_cacheLru, it->second.lruIt);
            return it->second.result;
        }
    }

    const std::optional<std::string> result = evaluateUnlocked(
        hwnd, className, isDesktop, isTaskbar, nullptr);

    // 写入 LRU 缓存
    if (m_cacheLru.size() >= MAX_CACHE_SIZE) {
        auto evictKey = m_cacheLru.back();
        m_cacheLru.pop_back();
        m_evaluateCache.erase(evictKey);
    }
    m_cacheLru.push_front(cacheKey);
    m_evaluateCache[cacheKey] = {result, m_cacheLru.begin()};

    return result;
}

void ScopeRuleEngine::addRule(const ScopeRule& rule) {
    std::lock_guard lock(m_mutex);
    rule.compileRegexes();
    m_rules.push_back(rule);
    invalidateCache();  // 规则变更时清除缓存
    LOG_DEBUG("添加作用域规则: id={}, name={}, process={}, class={}",
              rule.id, rule.name, rule.processName, rule.windowClass);
}

void ScopeRuleEngine::removeRule(const std::string& ruleId) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_rules, [&](const ScopeRule& r) { return r.id == ruleId; });
    invalidateCache();  // 规则变更时清除缓存
}

void ScopeRuleEngine::addRules(const std::vector<ScopeRule>& rules) {
    std::lock_guard lock(m_mutex);
    for (const auto& rule : rules) {
        rule.compileRegexes();
        m_rules.push_back(rule);
    }
    invalidateCache();
}

void ScopeRuleEngine::removeRules(const std::vector<std::string>& ruleIds) {
    std::lock_guard lock(m_mutex);
    std::erase_if(m_rules, [&](const ScopeRule& rule) {
        return std::find(ruleIds.begin(), ruleIds.end(), rule.id) != ruleIds.end();
    });
    invalidateCache();
}

void ScopeRuleEngine::loadFromJson(const nlohmann::json& j) {
    std::lock_guard lock(m_mutex);
    m_rules.clear();
    if (j.is_array()) {
        for (const auto& item : j) {
            m_rules.push_back(ScopeRule::fromJson(item));
        }
    }
    invalidateCache();  // 规则变更时清除缓存
    LOG_INFO("作用域规则已加载, 规则数量={}", m_rules.size());
}

nlohmann::json ScopeRuleEngine::toJson() const {
    std::lock_guard lock(m_mutex);
    nlohmann::json j = nlohmann::json::array();
    for (const auto& rule : m_rules) {
        j.push_back(rule.toJson());
    }
    return j;
}

std::vector<ScopeRule> ScopeRuleEngine::getRules() const {
    std::lock_guard lock(m_mutex);
    return m_rules;
}

void ScopeRuleEngine::clearRules() {
    std::lock_guard lock(m_mutex);
    m_rules.clear();
    invalidateCache();
}

ScopeRuleEngine::WindowInfo ScopeRuleEngine::getWindowInfo(HWND hwnd) const {
    WindowInfo info;
    info.hwnd = hwnd;
    info.className = tools3000::core::WinUtils::getWindowClassName(hwnd);

    // PID → 进程名 LRU 缓存（避免在热路径上重复调用 OpenProcess + QueryFullProcessImageName）
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    auto it = m_processNameCache.find(pid);
    if (it != m_processNameCache.end()) {
        info.processName = it->second;
    } else {
        info.processName = tools3000::core::WinUtils::processNameFromPid(pid);
        // 缓存上限 64 个进程名
        if (m_processNameCache.size() >= 64) {
            m_processNameCache.clear();  // 简单策略：满了全清
        }
        m_processNameCache[pid] = info.processName;
    }

    return info;
}

void ScopeRuleEngine::invalidateCache() const {
    m_evaluateCache.clear();
    m_cacheLru.clear();
    m_processNameCache.clear();
    LOG_TRACE("ScopeRuleEngine: 缓存已失效");
}

}  // namespace tools3000::gesture
