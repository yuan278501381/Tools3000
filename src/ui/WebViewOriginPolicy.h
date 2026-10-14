#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <windows.h>
#include <nlohmann/json.hpp>

namespace tools3000::ui::web_security {

enum class Surface { Settings, Search, Tray, QuickLook };

// WebView bridge is a control plane, not a bulk-data channel.  Keep malformed
// renderer input from forcing unbounded UTF conversion / JSON allocations.
constexpr size_t MaxBridgeMessageBytes = 1024 * 1024;

inline bool isBridgeMessageSizeAcceptable(size_t byteCount) noexcept {
    return byteCount <= MaxBridgeMessageBytes;
}

inline bool startsWithInsensitive(std::wstring_view value, std::wstring_view prefix) noexcept {
    if (value.size() < prefix.size()) return false;
    return _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

inline bool isInternalVirtualHostUri(std::wstring_view uri) noexcept {
    std::wstring_view rest;
    if (startsWithInsensitive(uri, L"https://")) {
        rest = uri.substr(8);
    } else if (startsWithInsensitive(uri, L"http://")) {
        rest = uri.substr(7);
    } else {
        return false;
    }
    const size_t hostEnd = rest.find_first_of(L"/?#:");
    const std::wstring_view host = (hostEnd == std::wstring_view::npos) ? rest : rest.substr(0, hostEnd);
    if (host.empty()) return false;
    if (host.size() == 15 && _wcsnicmp(host.data(), L"tools3000.local", 15) == 0) {
        return true;
    }
    constexpr std::wstring_view suffix = L".tools3000.local";
    if (host.size() > suffix.size()) {
        const std::wstring_view tail = host.substr(host.size() - suffix.size());
        return _wcsnicmp(tail.data(), suffix.data(), suffix.size()) == 0;
    }
    return false;
}

inline bool isTrustedUri(std::wstring_view uri) noexcept {
    if (uri == L"https://tools3000.local" ||
        startsWithInsensitive(uri, L"https://tools3000.local/")) {
        return true;
    }
#ifdef _DEBUG
    // 开发构建才允许本机 Vite；Release 永远只接受打包的虚拟 HTTPS 源。
    if (startsWithInsensitive(uri, L"http://localhost:") ||
        startsWithInsensitive(uri, L"http://127.0.0.1:")) {
        return true;
    }
#endif
    return false;
}

inline bool isBridgeMethodAllowed(std::string_view message, Surface surface) {
    if (!isBridgeMessageSizeAcceptable(message.size())) {
        return false;
    }
    if (surface == Surface::Settings) return true;
    try {
        const auto request = nlohmann::json::parse(message);
        const std::string method = request.value("method", "");
        const auto starts = [&method](std::string_view prefix) {
            return method.starts_with(prefix);
        };
        bool allowed = false;
        switch (surface) {
            case Surface::Search:
                allowed = starts("search.") || starts("system.") ||
                          method == "capture.pinImageFile" ||
                          method == "general.getSettings";
                break;
            case Surface::Tray:
                allowed = starts("tray.") || starts("general.") || starts("plugins.") ||
                          starts("gesture.") || starts("app.") || starts("remote.") ||
                          method == "hotkey.getAll" ||
                          method == "capture.pinGroupControl" ||
                          method == "capture.pasteAsPin";
                break;
            case Surface::QuickLook:
                allowed = starts("quicklook.") || starts("general.") ||
                          method == "capture.pinImageFile";
                break;
            case Surface::Settings:
                allowed = true;
                break;
        }
        return allowed;
    } catch (...) {
        return false;
    }
}

}  // namespace tools3000::ui::web_security
