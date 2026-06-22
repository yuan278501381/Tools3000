#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HotkeyPolicy — 全局热键占用与会话武装的纯判定
//
// RegisterHotKey 会把组合键从当前前台应用抢走。未在相关会话里时，
// 不应占用编辑器命令面板一类的通用和弦。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_HOTKEY_HOTKEYPOLICY_H
#define TOOLS3000_CORE_HOTKEY_HOTKEYPOLICY_H

#include <string_view>
#include <windows.h>

namespace tools3000::core {

enum class HotkeyArmScope : unsigned char {
    Always = 0,
    WhileRecording = 1,
};

inline HotkeyArmScope hotkeyArmScopeForName(std::string_view name) noexcept {
    return (name == "Record Pause" || name == "Record Snapshot")
               ? HotkeyArmScope::WhileRecording
               : HotkeyArmScope::Always;
}

inline bool hotkeyShouldBeArmed(HotkeyArmScope scope, bool recordingActive) noexcept {
    return scope == HotkeyArmScope::Always || recordingActive;
}

/// VS Code / Cursor / Antigravity 等编辑器的命令面板。
inline bool isEditorCommandPaletteChord(UINT modifiers, UINT virtualKey) noexcept {
    const UINT chord = modifiers & ~(static_cast<UINT>(MOD_NOREPEAT));
    return chord == (MOD_CONTROL | MOD_SHIFT) && virtualKey == 'P';
}

/// 有绑定且本应占用系统热键，但 RegisterHotKey 失败，才算外部冲突。
/// 会话外主动卸下的热键不是冲突。
inline bool hotkeyLooksExternallyConflicted(bool hasKey, bool armed, bool registered) noexcept {
    return hasKey && armed && !registered;
}

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_HOTKEY_HOTKEYPOLICY_H
