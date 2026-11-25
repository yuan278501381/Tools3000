#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WindowImeGuard.h — 原生快捷键画布窗口三层纵深 IME 免疫护栏 (Tools3000Core 导出)
//
// 设计原则:
//   1. 严格窗口级作用域（ImmAssociateContextEx）：绝不篡改用户系统全局输入法语言或
//      中英文切换态；退出截图/贴图切回其他软件后输入法状态 100% 原封不动；
//   2. 非文字输入态默认卸载 IME 上下文（防 CJK IME 劫持 R/Shift+R/C/E 等单键快捷键
//      为 VK_PROCESSKEY 0xE5，并杜绝弹出孤立拼音候选框或误切输入法中英文）；
//   3. 显式进入文字输入态时毫秒级恢复默认 IME，退出前主动 CPS_CANCEL 清空残余草稿；
//   4. 兜底通过 ImmGetVirtualKey + lParam 硬件扫描码反解残余 VK_PROCESSKEY (0xE5)，
//      并在非输入态过滤 WM_IME_SETCONTEXT 视觉标志，构成三层纵深防护。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_WINDOWIMEGUARD_H
#define TOOLS3000_CORE_UTILS_WINDOWIMEGUARD_H

#include "core/utils/Export.h"

#include <windows.h>

#include <cstdint>

namespace tools3000::core {

class TOOLS3000CORE_API WindowImeGuard final {
public:
    WindowImeGuard() = default;

    /// 重置缓存态（窗口销毁或句柄重建时调用）
    void reset() noexcept;

    /// 按“当前是否处于显式文字输入态”幂等同步窗口级 IME 绑定态。
    /// - allowTextInput == false: 先取消残余组词串，再卸载窗口 IME 上下文；
    /// - allowTextInput == true:  恢复窗口默认系统 IME 上下文；
    /// - force == true:           忽略状态缓存强制重申（适用于 WM_SETFOCUS / WM_ACTIVATE / 窗口重显）。
    void sync(HWND hwnd, bool allowTextInput, bool force = false) noexcept;

    /// 查询当前缓存的窗口 IME 绑定态（-1=未初始化, 0=已卸载禁用, 1=已挂载启用）
    int8_t cachedState() const noexcept;

    /// 第二层兜底：非文字输入态下将残余 VK_PROCESSKEY (0xE5) 还原为真实物理虚拟键码。
    /// 若处于文字输入态或非 VK_PROCESSKEY，则原样返回 wParam。
    static WPARAM resolveShortcutVirtualKey(
        HWND hwnd,
        WPARAM wParam,
        LPARAM lParam,
        bool allowTextInput) noexcept;

    /// 纯静态扫描码反解函数（单一事实源，供离线单元测试零 HWND 精准校验）
    static WPARAM resolveVirtualKeyFromProcessKey(
        WPARAM wParam,
        LPARAM lParam,
        UINT immVirtualKey,
        bool allowTextInput) noexcept;

    /// 第三层视觉护栏：在非文字输入态下收敛 WM_IME_SETCONTEXT 浮窗显示标志，并消费异常泄漏的
    /// WM_IME_STARTCOMPOSITION / WM_IME_COMPOSITION / WM_IME_ENDCOMPOSITION / WM_IME_CHAR。
    /// 返回 true 表示消息已完全消费（调用方应直接返回 outResult）；
    /// 返回 false 表示调用方应继续路由（注意：WM_IME_SETCONTEXT 时 lParam 可能已被原地剥离 UI 标志）。
    static bool filterNonTextImeMessage(
        HWND hwnd,
        UINT msg,
        WPARAM wParam,
        LPARAM& lParam,
        bool allowTextInput,
        LRESULT& outResult) noexcept;

private:
    int8_t m_imeState = -1; // -1=未初始化, 0=已禁用卸载, 1=已启用挂载
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_UTILS_WINDOWIMEGUARD_H
