#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// LuaEngine — Lua 脚本运行时
//
// 职责:
//   1. 管理 Lua (sol2) 虚拟机状态
//   2. 注入 C++ API 供 Lua 脚本调用 (easy.* 命名空间，对齐需求文档)
//   3. 安全地执行脚本字符串/文件，捕获异常
//   4. 轻量沙箱: 不开放 io 库，os 仅暴露白名单函数
//
// 线程模型:
//   sol::state 非线程安全。手势线程与设置界面线程都可能触发脚本执行，
//   因此所有 execute* 入口用一把互斥锁串行化。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_LUAENGINE_H
#define TOOLS3000_CORE_LUAENGINE_H

#include "core/utils/Export.h"

#include <sol/sol.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tools3000::core {

/// 细粒度 Lua 脚本权限控制枚举
enum class LuaPermission : uint32_t {
    None      = 0,
    Log       = 1 << 0,
    Url       = 1 << 1,
    Safe      = Log | Url,                                  // 绝对无害的纯净只读子集 (不包含任何窗口/设备/剪贴板控制)
    Window    = 1 << 2,                                     // 敏感权限：窗口控制 (最小化/最大化/关闭)
    Ui        = 1 << 3,                                     // 敏感权限：弹窗与通知交互
    Keyboard  = 1 << 4,                                     // 敏感权限：键盘按键模拟
    Mouse     = 1 << 5,                                     // 敏感权限：鼠标点击与移动模拟
    Clipboard = 1 << 6,                                     // 敏感权限：剪贴板内容读写
    Screen    = 1 << 7,                                     // 敏感权限：屏幕像素探测
    Shell     = 1 << 8,                                     // 极危权限：调用外部程序
    Fs        = 1 << 9,                                     // 极危权限：本地文件读写
    Http      = 1 << 10,                                    // 极危权限：网络 HTTP 请求
    Standard  = Safe | Window | Ui | Keyboard | Mouse | Clipboard | Screen,
    All       = 0xFFFFFFFF
};

inline constexpr LuaPermission operator|(LuaPermission a, LuaPermission b) noexcept {
    return static_cast<LuaPermission>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr LuaPermission operator&(LuaPermission a, LuaPermission b) noexcept {
    return static_cast<LuaPermission>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr bool hasPermission(LuaPermission set, LuaPermission required) noexcept {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(required)) == static_cast<uint32_t>(required);
}

/// 权限字符串解析与转换
TOOLS3000CORE_API LuaPermission parseLuaPermissions(const std::vector<std::string>& permStrings);
TOOLS3000CORE_API std::vector<std::string> formatLuaPermissions(LuaPermission perms);

/// 脚本执行上下文与用户授权元数据
struct ScriptContext {
    std::string scriptId;                                  // 脚本唯一标识 (如 "gesture:close_tab", "plugin:demo")
    std::string scriptName;                                // 可读名称
    LuaPermission requestedPerms = LuaPermission::Safe;    // 申请的细粒度权限
    bool interactive = true;                               // 未预授权时是否允许弹出原生模态确认弹窗
};

class TOOLS3000CORE_API LuaEngine {
public:
    static LuaEngine& instance();

    /// 初始化虚拟机并绑定 API。可重复调用 (重新构建干净的状态)。
    bool initialize();

    /// 释放虚拟机。
    void shutdown();

    /// 执行一段 Lua 源码。支持细粒度权限控制、超时限制与取消令牌。成功返回 true；失败记录错误日志并返回 false。
    bool executeScript(const std::string& script,
                       LuaPermission permissions = LuaPermission::Safe,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                       std::atomic<bool>* cancelToken = nullptr);

    /// 执行一段 Lua 源码，带完整的真实用户授权决策流 (User Authorization Flow)。
    bool authorizeAndExecute(const std::string& script,
                             const ScriptContext& context,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                             std::atomic<bool>* cancelToken = nullptr);

    /// 执行磁盘上的 Lua 脚本文件 (UTF-8 路径)。支持细粒度权限控制、超时限制与取消令牌。
    bool executeFile(const std::string& utf8Path,
                     LuaPermission permissions = LuaPermission::Safe,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                     std::atomic<bool>* cancelToken = nullptr);

    /// 用户授权管理接口
    bool isScriptAuthorized(const std::string& scriptId, LuaPermission requiredPerms);
    void grantPermissions(const std::string& scriptId, LuaPermission perms);
    void revokePermissions(const std::string& scriptId);
    void revokeAllPermissions();

private:
    LuaEngine() = default;
    ~LuaEngine() = default;
    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    // 在锁内执行已加载/编译好的可调用对象，统一错误处理与安全超时限制。
    bool runProtected(const std::function<sol::protected_function_result()>& fn,
                      const char* context,
                      LuaPermission permissions = LuaPermission::Standard,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
                      std::atomic<bool>* cancelToken = nullptr);

    // ── API 绑定 (按命名空间拆分，便于维护) ─────────────────────────────────
    void bindApi();
    void bindLog(sol::table& easy);
    void bindKeyboard(sol::table& easy);
    void bindMouse(sol::table& easy);
    void bindClipboard(sol::table& easy);
    void bindShell(sol::table& easy);
    void bindWindow(sol::table& easy);
    void bindScreen(sol::table& easy);
    void bindFs(sol::table& easy);
    void bindHttp(sol::table& easy);
    void bindUi(sol::table& easy);
    void bindUrl(sol::table& easy);

    std::unique_ptr<sol::state> m_lua;
    std::mutex m_mutex;  // 串行化所有脚本执行与权限授权
    std::unordered_map<std::string, LuaPermission> m_authorizedScripts;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_LUAENGINE_H
