#include "search/SearchInteractionHandlers.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/ipc/FileInteractionHandlers.h"
#include "core/ipc/MessageBridge.h"

#include <windows.h>
#include <algorithm>

namespace tools3000::search {
using tools3000::core::json;

namespace {

HWND currentProcessSearchWindow() {
    const DWORD currentProcessId = GetCurrentProcessId();
    HWND after = nullptr;
    while ((after = FindWindowExW(
                nullptr, after, L"Tools3000_SearchWindow", nullptr)) != nullptr) {
        DWORD ownerProcessId = 0;
        GetWindowThreadProcessId(after, &ownerProcessId);
        if (ownerProcessId == currentProcessId) return after;
    }
    return nullptr;
}

}  // namespace

void registerSearchInteractionHandlers(tools3000::core::MessageBridge& bridge) {
    tools3000::core::registerFileInteractionHandlers(bridge, "search", "path is empty", true);

    bridge.registerHandler("search.startDrag", [](const json&) -> json {
        const HWND hwnd = currentProcessSearchWindow();
        if (hwnd && IsWindow(hwnd)) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return {{"success", true}};
    });
    bridge.registerHandler("search.startResize", [](const json& params) -> json {
        const std::string edge = params.value("edge", params.value("direction", "bottom_right"));
        const HWND hwnd = currentProcessSearchWindow();
        if (hwnd && IsWindow(hwnd) && !IsZoomed(hwnd)) {
            SetForegroundWindow(hwnd);
            ReleaseCapture();
            WPARAM scSizeParam = 0;
            if (edge == "left" || edge == "w") scSizeParam = 0xF001;          // SC_SIZE + WMSZ_LEFT
            else if (edge == "right" || edge == "e") scSizeParam = 0xF002;    // SC_SIZE + WMSZ_RIGHT
            else if (edge == "top" || edge == "n") scSizeParam = 0xF003;      // SC_SIZE + WMSZ_TOP
            else if (edge == "top_left" || edge == "nw") scSizeParam = 0xF004; // SC_SIZE + WMSZ_TOPLEFT
            else if (edge == "top_right" || edge == "ne") scSizeParam = 0xF005;// SC_SIZE + WMSZ_TOPRIGHT
            else if (edge == "bottom" || edge == "s") scSizeParam = 0xF006;   // SC_SIZE + WMSZ_BOTTOM
            else if (edge == "bottom_left" || edge == "sw") scSizeParam = 0xF007;// SC_SIZE + WMSZ_BOTTOMLEFT
            else if (edge == "bottom_right" || edge == "se") scSizeParam = 0xF008;// SC_SIZE + WMSZ_BOTTOMRIGHT
            else return {{"success", false}, {"error", "invalid resize edge"}};

            POINT pt{};
            LPARAM lParam = 0;
            if (GetCursorPos(&pt)) {
                lParam = MAKELPARAM(pt.x, pt.y);
            }
            PostMessageW(hwnd, WM_SYSCOMMAND, scSizeParam, lParam);
        }
        return {{"success", true}};
    });
    bridge.registerHandler("search.resetPlacement", [](const json&) -> json {
        auto& config = tools3000::core::ConfigManager::instance();
        config.set<int>("/search/windowWidth", 760);
        config.set<int>("/search/windowHeight", 520);
        config.set<int>("/search/windowX", -99999);
        config.set<int>("/search/windowY", -99999);
        const HWND hwnd = currentProcessSearchWindow();
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_DISPLAYCHANGE, 0, 0);
        return {{"success", true}};
    });
    const auto applySearchWindowSize = [](const json& params, bool defaultCenter) -> json {
        if (!params.is_object()) {
            return {{"success", false}, {"error", "params must be an object"}};
        }
        int baseWidth = params.value("width", 760);
        int baseHeight = params.value("height", 520);
        const bool center = params.value("center", defaultCenter);
        baseWidth = (std::clamp)(baseWidth, 500, 2400);
        baseHeight = (std::clamp)(baseHeight, 400, 1600);

        auto& config = tools3000::core::ConfigManager::instance();
        config.set<int>("/search/windowWidth", baseWidth);
        config.set<int>("/search/windowHeight", baseHeight);
        if (center) {
            config.set<int>("/search/windowX", -99999);
            config.set<int>("/search/windowY", -99999);
        }
        const HWND hwnd = currentProcessSearchWindow();
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_DISPLAYCHANGE, 0, 0);
        return {{"success", true}, {"width", baseWidth}, {"height", baseHeight}};
    };
    if (!bridge.hasHandler("search.setWindowSize")) {
        bridge.registerHandler("search.setWindowSize", [applySearchWindowSize](const json& params) -> json {
            return applySearchWindowSize(params, false);
        });
    }
    bridge.registerHandler("search.applyWindowPreset", [applySearchWindowSize](const json& params) -> json {
        return applySearchWindowSize(params, true);
    });
    if (!bridge.hasHandler("search.getWindowSize")) {
        bridge.registerHandler("search.getWindowSize", [](const json&) -> json {
            auto& config = tools3000::core::ConfigManager::instance();
            const int width = config.get<int>("/search/windowWidth", 760);
            const int height = config.get<int>("/search/windowHeight", 520);
            return {{"success", true}, {"width", width}, {"height", height}};
        });
    }
    bridge.registerHandler("search.setPinned", [](const json& params) -> json {
        const bool pinned = params.value("pinned", false);
        const HWND hwnd = currentProcessSearchWindow();
        if (hwnd && IsWindow(hwnd)) {
            tools3000::core::MainThreadDispatcher::instance().post([hwnd, pinned]() {
                if (!IsWindow(hwnd)) return;
                if (pinned) {
                    SetPropW(hwnd, L"Tools3000_SearchPinned", reinterpret_cast<HANDLE>(1));
                    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                } else {
                    RemovePropW(hwnd, L"Tools3000_SearchPinned");
                    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            });
        }
        return {{"success", true}, {"pinned", pinned}};
    });
    bridge.registerHandler("search.isPinned", [](const json&) -> json {
        const HWND hwnd = currentProcessSearchWindow();
        return {{"pinned", hwnd && IsWindow(hwnd) &&
                           GetPropW(hwnd, L"Tools3000_SearchPinned") != nullptr}};
    });
}

}  // namespace tools3000::search
