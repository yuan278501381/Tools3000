#pragma once
// -----------------------------------------------------------------------------
// TextExpanderManager.h -- 系统级文本扩展器引擎管理器
//
// 核心架构特性:
//   1. Opt-in 零常驻设计：默认关闭，关闭状态下不挂载任何系统钩子、不创建任何线程。
//   2. 独立轻量钩子线程 + 独立展开投递工作线程，WH_KEYBOARD_LL 回调零阻塞、零重资源分配。
//   3. 严格忽略 LLKHF_INJECTED 注入事件，彻底杜绝自触发递归循环。
//   4. 无痕剪贴板粘贴法附带全部三项 Windows 官方隐私排除标记，100% 不污染
//      Tools3000 剪贴板历史与 Windows Win+V，并在 140ms 后无痕还原原剪贴板。
//   5. 短小纯单行 ASCII 展开文本走 KEYEVENTF_UNICODE 零剪贴板极速直注路径。
// -----------------------------------------------------------------------------

#ifndef TOOLS3000_TEXT_EXPANDER_MANAGER_H
#define TOOLS3000_TEXT_EXPANDER_MANAGER_H

#include "text_expander/TextExpanderMatcher.h"
#include "text_expander/TextExpanderTypes.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::text_expander {

class TextExpanderManager {
public:
    static TextExpanderManager& instance();

    /// 初始化管理器：从 ConfigManager 与规则 JSON 加载状态；仅在 enabled==true 时挂载钩子
    void initialize();

    /// 安全停机：卸载所有钩子并回收后台线程
    void shutdown();

    /// 总开关控制（开启时即时挂钩，关闭时彻底卸载钩子与工作线程，回归零占用）
    bool setEnabled(bool enabled);
    bool isEnabled() const noexcept;

    /// 读取当前完整设置 JSON（供 IPC textExpander.getSettings）
    nlohmann::json getSettingsJson() const;

    /// 局部或全量更新设置（供 IPC textExpander.updateSettings）
    nlohmann::json updateSettings(const nlohmann::json& params);

    /// 获取全部短语规则 JSON 数组（供 IPC textExpander.listRules）
    nlohmann::json listRulesJson() const;

    /// 新增或更新单条规则（含长度与重复校验，供 IPC textExpander.upsertRule）
    nlohmann::json upsertRule(const nlohmann::json& params);

    /// 删除单条规则（供 IPC textExpander.deleteRule）
    nlohmann::json deleteRule(const std::string& id);

    /// 切换单条规则启用状态（供 IPC textExpander.toggleRule）
    nlohmann::json toggleRule(const std::string& id, bool enabled);

    /// 批量导入规则（支持 merge / replace，供 IPC textExpander.importRules）
    nlohmann::json importRules(const nlohmann::json& params);

    /// 导出规则 JSON 载荷（供 IPC textExpander.exportRules）
    nlohmann::json exportRulesJson() const;

    /// 可选注册规则命中回调（用于宿主额外观测或通知）
    void setOnRuleExpandedCallback(std::function<void(const std::string& ruleId, uint32_t hitCount)> cb);

    /// 测试辅助：指定隔离规则文件路径（严禁在生产路径调用）
    void setRulesFilePathForTesting(const std::filesystem::path& path);

private:
    TextExpanderManager() = default;
    ~TextExpanderManager();

    TextExpanderManager(const TextExpanderManager&) = delete;
    TextExpanderManager& operator=(const TextExpanderManager&) = delete;

    struct ExpansionJob {
        std::string ruleId;
        size_t eraseCount = 0;
        std::wstring replacementW;
        bool restoreClipboard = true;
        bool triggeredByDelimiter = false;
        bool keepDelimiter = true;
        wchar_t delimiterChar = L'\0';
        HWND targetFgHwnd = nullptr;
    };

    void loadSettingsFromConfigLocked();
    void saveSettingsToConfigLocked() const;
    void loadRulesFromDiskLocked();
    bool saveRulesToDiskLocked() const;
    void syncMatcherRulesLocked();

    void startRuntimeWorkers();
    void stopRuntimeWorkers();

    void hookThreadMain(HANDLE readyEvent, std::atomic<bool>& hookReadyOk);
    void expansionWorkerLoop();
    void executeExpansionJob(const ExpansionJob& job);

    bool isProcessExcluded(const std::string& lowerExeName) const;
    bool evaluateForegroundWindowExcluded(HWND fgHwnd);
    void handleLowLevelKeyDown(const KBDLLHOOKSTRUCT& data);
    void handleForegroundChanged();
    void recordRuleHitAndNotify(const std::string& ruleId);
    void pushStateChangedEvents(bool enabled) const;

    static LRESULT CALLBACK lowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static void CALLBACK winEventForegroundProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD dwEventThread,
        DWORD dwmsEventTime);

    static std::string generateRuleId();
    static bool readClipboardPlainText(std::wstring& outText, bool& outWasSensitive);
    static bool writeClipboardPlainTextPrivate(const std::wstring& text, DWORD& outSeqAfterWrite);
    static void sendBackspaces(size_t count);
    static void sendDirectUnicodeExpansion(size_t backspaces, std::wstring_view text, size_t moveLeftSteps);
    static void sendPasteShortcutForWindow(HWND targetFgHwnd);
    static void sendLeftArrows(size_t count);
    static void waitModifiersReleased(DWORD maxWaitMs);

    mutable std::mutex m_stateMutex;
    TextExpanderSettings m_settings;
    std::vector<TextExpanderRule> m_rules;
    std::filesystem::path m_rulesFilePath;
    bool m_initialized = false;

    mutable std::mutex m_matcherMutex;
    TextExpanderMatcher m_matcher;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_expanding{false};
    std::atomic<bool> m_mouseClicked{false};
    std::atomic<bool> m_foregroundDirty{true};
    std::atomic<uint64_t> m_policyEpoch{1};

    // 钩子线程私有状态（仅在钩子线程内读写，无锁高速路径）
    HWND m_lastCheckedHwnd = nullptr;
    uint64_t m_lastCheckedPolicyEpoch = 0;
    bool m_lastHwndExcluded = false;
    ULONGLONG m_lastKeyTickMs = 0;

    // 生命周期与后台线程管理
    mutable std::mutex m_lifecycleMutex;
    std::thread m_hookThread;
    std::atomic<DWORD> m_hookThreadId{0};
    std::atomic<HHOOK> m_kbHook{nullptr};
    uint64_t m_mouseActivitySubId = 0;

    std::thread m_workerThread;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<ExpansionJob> m_jobQueue;
    std::atomic<bool> m_workerRunning{false};

    mutable std::mutex m_callbackMutex;
    std::function<void(const std::string&, uint32_t)> m_onRuleExpanded;
};

}  // namespace tools3000::text_expander

#endif  // TOOLS3000_TEXT_EXPANDER_MANAGER_H
