#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureAction — 手势动作抽象
//
// 定义手势触发后执行什么动作:
//   - 发送快捷键 (SendInput)
//   - 运行 Lua 脚本
//   - 执行内置命令 (关闭窗口/最大化/最小化等)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREACTION_H
#define TOOLS3000_GESTURE_GESTUREACTION_H

#include <windows.h>
#include <string>
#include <variant>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

namespace tools3000::gesture {

/// 动作类型
enum class ActionType {
    SendKeys,       // 发送按键组合 (如 Ctrl+C)
    LuaScript,      // 执行 Lua 脚本
    BuiltinCommand, // 内置命令
    RunProgram      // 运行外部程序
};

/// 内置命令枚举
enum class BuiltinCommand {
    CloseWindow,              // 0: 关闭窗口
    CloseTab,                 // 1: 关闭标签页
    MaximizeWindow,           // 2: 最大化
    MinimizeWindow,           // 3: 最小化
    RestoreWindow,            // 4: 还原窗口
    ShowDesktop,              // 5: 显示桌面
    SwitchDesktop,            // 6: 切换虚拟桌面
    TaskView,                 // 7: 任务视图
    LockScreen,               // 8: 锁屏
    PauseGestures,            // 9: 暂停手势
    TakeScreenshot,           // 10: 截图
    StartRecording,           // 11: 开始录屏
    RestoreClosedTab,         // 12: 恢复最近关闭的标签页
    ToggleTopmost,            // 13: 窗口置顶切换
    ToggleWindowTransparency, // 14: 窗口透明度切换
    WebSearch,                // 15: 选中文本直接网页搜索
    ToggleSearch,             // 16: 切换搜索框
    ShowRadialMenu,           // 17: 显示呼出轮盘
    PasteAsPin,               // 18: 将剪贴板内容作为贴图显示
    MediaNext,                // 19: 下一曲 (全局多媒体)
    MediaPrev,                // 20: 上一曲 (全局多媒体)
    MediaPlayPause,           // 21: 播放 / 暂停 (全局多媒体)
    VolumeUp,                 // 22: 音量增加 (全局多媒体)
    VolumeDown,               // 23: 音量减小 (全局多媒体)
    VolumeMute,               // 24: 静音切换 (全局多媒体)
    PrevVirtualDesktop,       // 25: 上一个虚拟桌面 (Win+Ctrl+Left)
    NextVirtualDesktop,       // 26: 下一个虚拟桌面 (Win+Ctrl+Right)
    PrevDesktop = PrevVirtualDesktop,
    NextDesktop = NextVirtualDesktop,
    SwitchDesktopLeft = PrevVirtualDesktop,   // 上一个虚拟桌面别名
    SwitchDesktopRight = NextVirtualDesktop   // 下一个虚拟桌面别名
};

inline constexpr BuiltinCommand kMaxBuiltinCommand = BuiltinCommand::NextVirtualDesktop;

/// 统一校验 BuiltinCommand 整数索引是否合法 (0 ~ kMaxBuiltinCommand)
inline constexpr bool isValidBuiltinCommandIndex(int index) noexcept {
    return index >= static_cast<int>(BuiltinCommand::CloseWindow) &&
           index <= static_cast<int>(kMaxBuiltinCommand);
}

inline constexpr bool isValidBuiltinCommandValue(int value) noexcept {
    return isValidBuiltinCommandIndex(value);
}

/// 按键定义 (用于 SendKeys 类型)
struct KeyStroke {
    uint8_t modifiers = 0;   // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN 的组合
    uint16_t virtualKey = 0; // VK_xxx

    /// 从字符串解析 (如 "Ctrl+C")
    static KeyStroke fromString(const std::string& str);
    std::string toString() const;

    /// 通过 SendInput 合成这组按键 (按下修饰键 → 主键 → 逆序释放)
    /// 若提供了 targetWindow 且非全局多媒体键，则先将输入焦点精准切换至目标窗口。
    void send(void* targetWindow = nullptr) const;
};

/// 手势按键目标：跳过轨迹/Toast 覆盖层，保留设置窗等真实 Tools3000 窗口。
void* resolveGestureKeyTarget(void* candidate, void* gestureStart, void* previousForeground) noexcept;

/// 光标下的顶层窗口，穿透手势轨迹 / toast（它们是 TOPMOST，松手时正压在笔尖上）。
void* windowFromPointSkippingGestureOverlay(int x, int y) noexcept;

/// 动作类型是否必须在收到用户输入的 UI 线程上执行（SendKeys / 内置窗口命令）。
bool gestureActionNeedsInputThread(ActionType type) noexcept;

/// 强制确保系统全局修饰键（Win, Ctrl, Alt, Shift）完全释放，杜绝修饰键粘滞导致鼠标无法点击
/// forceAll = false 时仅释放通过 GetAsyncKeyState 处于逻辑按下态的修饰键；forceAll = true 时无条件补发全部 KEYUP
void ensureAllModifiersReleased(bool forceAll = false) noexcept;

/// 针对单个虚拟键强制补发 KEYUP 释放，杜绝特定按键粘滞
void ensureModifierReleased(WORD vk) noexcept;

/// 跨虚拟桌面切换专用分步确定性时序管线 (next = true 为向右/下一个桌面, false 为向左/上一个桌面)
void executeStagedDesktopSwitch(bool next) noexcept;

/// 获取最近一次由手势按键注入发送的修饰键掩码与时间戳 (用于看门狗精准自愈)
uint8_t getLastInjectedModifiers() noexcept;
DWORD getLastInjectedTick() noexcept;
void clearLastInjectedModifiers() noexcept;

/// 手势动作定义
struct GestureAction {
    ActionType type;
    std::string name;                // 人类可读名称 (如 "复制", "粘贴")
    std::string description;         // 描述

    // 根据 type 使用不同字段:
    KeyStroke keyStroke;             // type == SendKeys
    std::string luaScript;           // type == LuaScript (Lua 代码或脚本文件路径)
    std::vector<std::string> requestedPermissions; // type == LuaScript (声明的细粒度权限列表)
    BuiltinCommand builtinCmd;       // type == BuiltinCommand
    std::string programPath;         // type == RunProgram
    std::string programArgs;         // type == RunProgram

    /// 执行动作 (可传入鼠标下方的目标窗口句柄 HWND)
    void execute(void* targetWindow = nullptr) const;

    /// 序列化/反序列化
    nlohmann::json toJson() const;
    static GestureAction fromJson(const nlohmann::json& j);
};

/// 手势映射条目: 方向编码 → 动作
struct GestureMapping {
    std::string id;                  // 唯一标识 (若为空可由 gestureCode 生成)
    bool enabled = true;             // 单项手势启用/禁用
    bool instantExecute = false;     // 识别到方向时立即执行 (无需等待松开按键)
    bool silentToast = false;        // 执行时不显示手势名称卡片
    std::string gestureCode;         // 方向编码 (如 "L", "DR", "L-U-R")
    GestureAction action;

    nlohmann::json toJson() const;
    static GestureMapping fromJson(const nlohmann::json& j);
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTUREACTION_H
