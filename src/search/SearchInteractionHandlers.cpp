#include "search/SearchInteractionHandlers.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/ipc/FileInteractionHandlers.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/PathOperations.h"
#include "core/utils/ShellContextMenuService.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace tools3000::search {
using tools3000::core::json;

namespace {

bool startsWithAsciiIgnoreCase(std::wstring_view text, std::wstring_view lowerPrefix) noexcept {
    if (text.size() < lowerPrefix.size()) return false;
    for (size_t i = 0; i < lowerPrefix.size(); ++i) {
        const wchar_t ch = (text[i] == L'/')
            ? L'\\'
            : ((text[i] >= L'A' && text[i] <= L'Z') ? static_cast<wchar_t>(text[i] + 32) : text[i]);
        if (ch != lowerPrefix[i]) return false;
    }
    return true;
}

bool isSafeAppsFolderUri(std::wstring_view wpath) noexcept {
    if (!startsWithAsciiIgnoreCase(wpath, L"shell:appsfolder")) {
        return false;
    }
    if (wpath.size() > 16 && wpath[16] != L'\\' && wpath[16] != L'/') {
        return false;
    }
    for (wchar_t ch : wpath) {
        if (ch < 0x20) return false;
    }
    return true;
}

bool isMissingLocalPathTarget(const std::wstring& wpath) noexcept {
    if (wpath.empty() || startsWithAsciiIgnoreCase(wpath, L"shell:")) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND ||
           err == ERROR_PATH_NOT_FOUND ||
           err == ERROR_INVALID_DRIVE ||
           err == ERROR_NOT_READY ||
           err == ERROR_BAD_NETPATH ||
           err == ERROR_BAD_NET_NAME ||
           err == ERROR_DEV_NOT_EXIST;
}

json makeFileNotFoundError() {
    return {
        {"success", false},
        {"errorCode", "FILE_NOT_FOUND"},
        {"error", "FILE_NOT_FOUND"}
    };
}

bool executeShellAppsUri(const std::wstring& uri, const wchar_t* verb) noexcept {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = verb;
    sei.lpFile = uri.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

std::string extractSearchRequestPath(const json& params) {
    return params.value("filepath", params.value("path", ""));
}

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

    bridge.registerHandler("search.openFile", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isSafeAppsFolderUri(wpath)) {
            return {{"success", executeShellAppsUri(wpath, nullptr)}};
        }
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        return {{"success", tools3000::core::WinUtils::openFile(wpath)}};
    });

    bridge.registerHandler("search.openFileAsAdmin", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isSafeAppsFolderUri(wpath)) {
            const bool ok = executeShellAppsUri(wpath, L"runas") || executeShellAppsUri(wpath, nullptr);
            return {{"success", ok}};
        }
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        return {{"success", tools3000::core::WinUtils::openFileAsAdmin(wpath)}};
    });

    bridge.registerHandler("search.openFolder", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isSafeAppsFolderUri(wpath)) {
            const std::wstring appsFolderRoot = L"shell:AppsFolder";
            return {{"success", executeShellAppsUri(appsFolderRoot, L"open")}};
        }
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        return {{"success", tools3000::core::WinUtils::openFolderAndSelectItem(wpath)}};
    });

    bridge.registerHandler("search.showFileProperties", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        return {{"success", tools3000::core::WinUtils::showFileProperties(wpath)}};
    });

    bridge.registerHandler("search.openWithNotepad", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        return {{"success", tools3000::core::WinUtils::openWithNotepad(wpath)}};
    });

    bridge.registerHandler("search.copyFile", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        std::error_code ec;
        const bool isGif = !std::filesystem::is_directory(wpath, ec) &&
                           tools3000::core::WinUtils::toLower(std::filesystem::path(wpath).extension().wstring()) == L".gif";
        const bool ok = isGif
            ? (tools3000::core::WinUtils::copyGifToClipboard(wpath) || tools3000::core::WinUtils::copyFileToClipboard(wpath))
            : tools3000::core::WinUtils::copyFileToClipboard(wpath);
        return {{"success", ok}};
    });

    bridge.registerHandler("search.renamePath", [](const json& params) -> json {
        const std::string oldPath = params.value("oldPath", params.value("path", ""));
        const std::string newName = params.value("newName", params.value("name", ""));
        if (oldPath.empty() || newName.empty()) {
            return {{"success", false}, {"error", "invalid parameters"}};
        }
        const std::wstring wOldPath = tools3000::core::WinUtils::utf8ToWstring(oldPath);
        if (isMissingLocalPathTarget(wOldPath)) {
            return makeFileNotFoundError();
        }
        const auto result = tools3000::core::renamePathWithinParent(
            wOldPath, tools3000::core::WinUtils::utf8ToWstring(newName));
        if (!result.success) return {{"success", false}, {"error", result.error}};
        return {
            {"success", true},
            {"newPath", tools3000::core::WinUtils::wstringToUtf8(result.newPath.wstring())},
            {"newName", newName}
        };
    });

    bridge.registerHandler("search.showShellContextMenu", [](const json& params) -> json {
        const std::string path = extractSearchRequestPath(params);
        if (path.empty()) return {{"success", false}, {"error", "path is empty"}};
        const std::wstring wpath = tools3000::core::WinUtils::utf8ToWstring(path);
        if (isMissingLocalPathTarget(wpath)) {
            return makeFileNotFoundError();
        }
        const bool started = tools3000::core::ShellContextMenuService::instance().showAsync(
            wpath, params.value("x", -1), params.value("y", -1),
            params.value("extended", false));
        return {{"success", started}, {"busy", !started}};
    });

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
