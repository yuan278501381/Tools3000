#pragma once

// Click-through layered HWND 没有普通控件树。先通过窗口名称和 MSAA 事件将
// 关键状态暴露给系统宿主 provider；复杂交互（例如截图工具栏按钮）可在此
// 基础上逐步替换为完整 UIA provider，且不会影响渲染或抢占键盘焦点。

#include <windows.h>
#include <string>
#include <string_view>

#include "core/accessibility/OverlayUiaProvider.h"

namespace tools3000::core::accessibility {

inline void announceOverlay(HWND hwnd, std::wstring_view name) noexcept {
    if (!hwnd) return;
    SetWindowTextW(hwnd, std::wstring(name).c_str());
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_SHOW, hwnd, OBJID_CLIENT, CHILDID_SELF);
    announceOverlayUia(hwnd, {L"Tools3000.OverlayAnnouncement", name,
                             OverlayUiaRole::Status, true}, name);
}

inline void hideOverlay(HWND hwnd) noexcept {
    if (hwnd) NotifyWinEvent(EVENT_OBJECT_HIDE, hwnd, OBJID_CLIENT, CHILDID_SELF);
}

}  // namespace tools3000::core::accessibility
