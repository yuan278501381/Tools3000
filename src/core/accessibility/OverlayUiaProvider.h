#pragma once

// UI Automation entry point for custom layered HWNDs. Unlike ordinary Win32
// controls, Direct2D overlays have no child HWND tree, so their meaningful name
// and role must be supplied explicitly when an assistive technology sends
// WM_GETOBJECT/UiaRootObjectId.

#include <windows.h>

#include "core/utils/Export.h"

#include <string_view>
#include <string>
#include <vector>

namespace tools3000::core::accessibility {

enum class OverlayUiaRole {
    Status,
    Text,
    Pane,
};

enum class OverlayUiaActionRole {
    Button,
    Text,
};

struct OverlayUiaSemantics {
    std::wstring_view automationId;
    std::wstring_view helpText;
    OverlayUiaRole role = OverlayUiaRole::Text;
    bool politeLiveRegion = false;
};

/// A virtual actionable child of a Direct2D overlay. Bounds are physical screen
/// coordinates as required by UI Automation. Invoke is always marshalled by
/// PostMessage to the owning HWND; provider code never executes UI commands.
struct OverlayUiaAction {
    std::wstring automationId;
    std::wstring name;
    std::wstring helpText;
    std::wstring keyboardShortcut;
    RECT bounds{};
    bool enabled = true;
    bool selected = false;
    OverlayUiaActionRole role = OverlayUiaActionRole::Button;
    UINT invokeMessage = 0;
    WPARAM invokeWParam = 0;
};

/// Returns zero for messages other than a UIA root-object request. The returned
/// value can be passed straight from a window procedure without changing input
/// routing, focus, z-order, or DPI behavior.
TOOLS3000CORE_API LRESULT respondToOverlayUiaGetObject(
    HWND hwnd, WPARAM wParam, LPARAM lParam,
    const OverlayUiaSemantics& semantics) noexcept;

TOOLS3000CORE_API LRESULT respondToOverlayUiaGetObject(
    HWND hwnd, WPARAM wParam, LPARAM lParam,
    const OverlayUiaSemantics& semantics,
    std::vector<OverlayUiaAction> actions) noexcept;

/// Announces a visible live-region update without activating the HWND or
/// changing input focus. This is best-effort on older Windows builds.
TOOLS3000CORE_API void announceOverlayUia(HWND hwnd, const OverlayUiaSemantics& semantics,
                                     std::wstring_view text) noexcept;

/// Must be called from WM_NCDESTROY so clients cannot retain a provider for a
/// recycled HWND value after an overlay is destroyed.
TOOLS3000CORE_API void disconnectOverlayUiaProvider(HWND hwnd) noexcept;

}  // namespace tools3000::core::accessibility
