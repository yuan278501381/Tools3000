// ─────────────────────────────────────────────────────────────────────────────
// WindowImeGuard.cpp — 原生快捷键画布窗口三层纵深 IME 免疫护栏实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/WindowImeGuard.h"

#include <imm.h>

namespace tools3000::core {

namespace {

WPARAM normalizeModifierVk(UINT vk) noexcept {
    switch (vk) {
        case VK_LSHIFT:
        case VK_RSHIFT:
            return VK_SHIFT;
        case VK_LCONTROL:
        case VK_RCONTROL:
            return VK_CONTROL;
        case VK_LMENU:
        case VK_RMENU:
            return VK_MENU;
        default:
            return static_cast<WPARAM>(vk);
    }
}

void cancelActiveCompositionIfAny(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return;
    HIMC hImc = ImmGetContext(hwnd);
    if (!hImc) return;
    ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    ImmReleaseContext(hwnd, hImc);
}

} // namespace

void WindowImeGuard::reset() noexcept {
    m_imeState = -1;
}

int8_t WindowImeGuard::cachedState() const noexcept {
    return m_imeState;
}

void WindowImeGuard::sync(HWND hwnd, bool allowTextInput, bool force) noexcept {
    if (!hwnd || !IsWindow(hwnd)) {
        m_imeState = -1;
        return;
    }

    const int8_t targetState = allowTextInput ? 1 : 0;
    if (!force && m_imeState == targetState) {
        return;
    }

    if (!allowTextInput) {
        // 先取消未上屏拼音/假名残余草稿，避免下次点开文字输入框吐出旧拼音
        cancelActiveCompositionIfAny(hwnd);
        // 仅解除当前窗口的 IME 上下文关联，严禁触碰系统全局输入法中英文切换状态
        ImmAssociateContextEx(hwnd, nullptr, 0);
    } else {
        // 恢复当前窗口的默认系统 IME 上下文，保障 CJK 输入法正常组词上屏
        ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
    }

    m_imeState = targetState;
}

WPARAM WindowImeGuard::resolveVirtualKeyFromProcessKey(
    WPARAM wParam,
    LPARAM lParam,
    UINT immVirtualKey,
    bool allowTextInput) noexcept {
    if (allowTextInput || wParam != VK_PROCESSKEY) {
        return wParam;
    }

    if (immVirtualKey != 0 && immVirtualKey != VK_PROCESSKEY) {
        return normalizeModifierVk(immVirtualKey);
    }

    const UINT scanCode = static_cast<UINT>((static_cast<uintptr_t>(lParam) >> 16) & 0xFFu);
    if (scanCode == 0) {
        return wParam;
    }

    const bool isExtended = (static_cast<uintptr_t>(lParam) & (static_cast<uintptr_t>(1) << 24)) != 0;
    UINT mapped = 0;
    if (isExtended) {
        mapped = MapVirtualKeyW(0xE000u | scanCode, MAPVK_VSC_TO_VK_EX);
    }
    if (mapped == 0) {
        mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
    }
    if (mapped == 0) {
        mapped = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK);
    }
    if (mapped != 0 && mapped != VK_PROCESSKEY) {
        return normalizeModifierVk(mapped);
    }
    return wParam;
}

WPARAM WindowImeGuard::resolveShortcutVirtualKey(
    HWND hwnd,
    WPARAM wParam,
    LPARAM lParam,
    bool allowTextInput) noexcept {
    if (allowTextInput || wParam != VK_PROCESSKEY) {
        return wParam;
    }
    const UINT immVk = (hwnd && IsWindow(hwnd)) ? ImmGetVirtualKey(hwnd) : 0u;
    return resolveVirtualKeyFromProcessKey(wParam, lParam, immVk, allowTextInput);
}

bool WindowImeGuard::filterNonTextImeMessage(
    HWND hwnd,
    UINT msg,
    WPARAM /*wParam*/,
    LPARAM& lParam,
    bool allowTextInput,
    LRESULT& outResult) noexcept {
    if (allowTextInput) {
        return false;
    }

    switch (msg) {
        case WM_IME_SETCONTEXT:
            // 剥离组合窗与候选窗 UI 展示位，保留消息交由 DefWindowProcW 完成标准激活链
            lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW | ISC_SHOWUIALLCANDIDATEWINDOW);
            return false;
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
            cancelActiveCompositionIfAny(hwnd);
            outResult = 0;
            return true;
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
            outResult = 0;
            return true;
        default:
            return false;
    }
}

} // namespace tools3000::core
