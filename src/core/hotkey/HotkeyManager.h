#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HotkeyManager — 全局快捷键管理器
//
// 特性:
//   1. 统一注册/注销全局快捷键（RegisterHotKey）
//   2. 快捷键冲突检测（深度辨别内部插件冲突与外部系统/第三方应用冲突）
//   3. 快捷键 → 回调映射（支持 lambda / std::function）
//   4. 支持运行时动态修改快捷键绑定
//   5. 自动处理 WM_HOTKEY 消息分发
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_HOTKEY_HOTKEYMANAGER_H
#define TOOLS3000_CORE_HOTKEY_HOTKEYMANAGER_H

#include "core/utils/Export.h"

#include <windows.h>
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <atomic>
#include <optional>
#include <vector>

namespace tools3000::core {

/// 修饰键枚举（可组合使用）
enum class ModKey : UINT {
    None  = 0,
    Alt   = MOD_ALT,
    Ctrl  = MOD_CONTROL,
    Shift = MOD_SHIFT,
    Win   = MOD_WIN
};

inline ModKey operator|(ModKey a, ModKey b) {
    return static_cast<ModKey>(static_cast<UINT>(a) | static_cast<UINT>(b));
}

/// 快捷键定义
struct TOOLS3000CORE_API HotkeyDef {
    ModKey modifiers = ModKey::None;
    UINT virtualKey  = 0;            // VK_xxx 虚拟键码

    /// 人类可读的字符串表示 (如 "Ctrl+Shift+A")
    std::string toString() const;

    /// 从字符串解析 (如 "Ctrl+Shift+A")
    static std::optional<HotkeyDef> fromString(const std::string& str);

    /// 检查快捷键是否符合四级安全矩阵标准（杜绝打字劫持灾难）
    bool isSafe() const;

    bool operator==(const HotkeyDef& other) const {
        return modifiers == other.modifiers && virtualKey == other.virtualKey;
    }
};

using HotkeyCallback = std::function<void()>;

/// 快捷键冲突详情
struct HotkeyConflictInfo {
    bool hasConflict = false;
    std::string conflictType = "none";  // "none", "internal", "external", "unsafe"
    std::string conflictWith;           // 冲突的插件或提示说明
};

/// 已注册的快捷键信息
struct HotkeyEntry {
    int id = 0;                 // RegisterHotKey 的 ID
    HotkeyDef def;              // 快捷键定义
    std::string name;           // 人类可读名称 (如 "截图", "暂停手势")
    HotkeyCallback callback;    // 触发回调
    bool registered = false;    // false = 已禁用、会话外卸下，或当前组合键被占用
    bool armed = true;          // false = 会话外主动不占用系统热键
    bool conflict = false;      // 是否存在冲突
    std::string conflictType = "none"; // "none" | "internal" | "external" | "unsafe"
    std::string conflictWith;   // 冲突关联说明
    bool useHook = false;       // 是否由底层键盘钩子接管
    bool suspendedByFullscreen = false; // 是否因全屏游戏防打扰临时挂起卸下
};

class TOOLS3000CORE_API HotkeyManager {
public:
    static HotkeyManager& instance();

    /// 判断快捷键是否可由底层键盘钩子接管
    static bool canFallbackToHook(const HotkeyDef& def);

    /// 判断是否为单功能键（F1~F24, PrintScreen, Pause 等）
    static bool isSingleFunctionKey(const HotkeyDef& def);

    /// 初始化（需要传入消息窗口的 HWND）
    void initialize(HWND messageWindow);

    /// 关闭（注销所有快捷键）
    void shutdown();

    /// 注册快捷键
    /// @return true=注册成功, false=快捷键冲突或注册失败
    bool registerHotkey(const std::string& name, const HotkeyDef& def, HotkeyCallback callback);

    /// 注销快捷键
    void unregisterHotkey(const std::string& name);

    /// 修改已注册快捷键的绑定
    bool rebindHotkey(const std::string& name, const HotkeyDef& newDef);

    /// Disable a binding while retaining its name and callback so it can be
    /// re-enabled from settings without restarting the plugin.
    bool clearHotkey(const std::string& name);

    /// 会话外卸下/重新占用系统热键。卸下后设置页仍显示绑定，前台应用能收到该组合键。
    bool setHotkeyArmed(const std::string& name, bool armed);

    /// 检查快捷键是否已被占用
    bool isConflict(const HotkeyDef& def) const;

    /// 探测快捷键是否与内部插件或外部系统/第三方软件冲突
    HotkeyConflictInfo checkConflict(const HotkeyDef& def, const std::string& currentName = "") const;

    /// 处理 WM_HOTKEY 消息（在消息循环中调用）
    void handleHotkeyMessage(WPARAM wParam);

    /// 获取所有已注册快捷键（用于 UI 显示，附带冲突分析）
    std::vector<HotkeyEntry> getAllHotkeys() const;

    /// 设置快捷键分发暂停状态（例如用户在 UI 中录制按键时避免误触发）
    void setPaused(bool paused);
    bool isPaused() const;

    /// 全屏游戏智能静默：进入游戏时物理卸下 RegisterHotKey 与钩子热键（如将 F1 归还给 Dota 2），切出游戏后自动恢复
    void setFullscreenSuspended(bool suspended);
    bool isFullscreenSuspended() const;

private:
    HotkeyManager() = default;
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    int generateId();

    HWND m_hwnd = nullptr;
    std::atomic<DWORD> m_ownerThreadId{0};
    std::unordered_map<std::string, HotkeyEntry> m_hotkeys;  // name → entry
    std::unordered_map<int, std::string> m_idToName;         // id → name (反向查找)
    mutable std::mutex m_mutex;
    std::atomic<int> m_nextId{1};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_fullscreenSuspended{false};
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_HOTKEY_HOTKEYMANAGER_H
