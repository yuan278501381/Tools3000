// ─────────────────────────────────────────────────────────────────────────────
// BuiltinCommands.cpp — 内置命令分发器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/BuiltinCommands.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/ipc/MessageBridge.h"

#include <windows.h>
#include <shellapi.h>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace tools3000::gesture {

std::string urlEncodeUtf8(std::string_view utf8) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (const unsigned char ch : utf8) {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(ch >> 4) & 0x0F]);
            encoded.push_back(kHex[ch & 0x0F]);
        }
    }
    return encoded;
}

std::wstring buildWebSearchUrl(std::wstring_view rawText) {
    size_t first = 0;
    while (first < rawText.size() && std::iswspace(rawText[first])) {
        ++first;
    }
    size_t last = rawText.size();
    while (last > first && std::iswspace(rawText[last - 1])) {
        --last;
    }
    const std::wstring_view trimmed = rawText.substr(first, last - first);
    if (trimmed.empty()) {
        return {};
    }

    const bool hasInternalWhitespace = [&]() noexcept {
        for (const wchar_t ch : trimmed) {
            if (std::iswspace(ch)) return true;
        }
        return false;
    }();

    if (!hasInternalWhitespace &&
        (_wcsnicmp(trimmed.data(), L"http://", 7) == 0 ||
         _wcsnicmp(trimmed.data(), L"https://", 8) == 0)) {
        return std::wstring(trimmed);
    }

    const std::string utf8Query = tools3000::core::WinUtils::wstringToUtf8(std::wstring(trimmed));
    const std::string encodedQuery = urlEncodeUtf8(utf8Query);
    return L"https://www.baidu.com/s?wd=" + tools3000::core::WinUtils::utf8ToWstring(encodedQuery);
}

BuiltinCommandDispatcher& BuiltinCommandDispatcher::instance() {
    static BuiltinCommandDispatcher inst;
    return inst;
}

void BuiltinCommandDispatcher::registerHandler(BuiltinCommand cmd, Handler handler) {
    std::unique_lock lock(m_mutex);
    m_handlers[cmd] = std::move(handler);
}

void BuiltinCommandDispatcher::clearHandlers() {
    decltype(m_handlers) handlers;
    {
        std::unique_lock lock(m_mutex);
        handlers.swap(m_handlers);
    }
    handlers.clear();
}

namespace {

/// 记录窗口在首次被手势切换半透明前是否原本已具备 WS_EX_LAYERED 属性
struct LayeredWindowStateRecord {
    bool originallyLayered = false;
    COLORREF originalColorKey = 0;
    DWORD originalFlags = 0;
};

std::mutex g_layeredStateMutex;
std::unordered_map<HWND, LayeredWindowStateRecord> g_layeredStateMap;

void pruneStaleLayeredRecordsLocked() {
    std::erase_if(g_layeredStateMap, [](const auto& kv) {
        return !kv.first || !IsWindow(kv.first);
    });
}

/// 合成一组按键 (复用 KeyStroke 的 SendInput 实现，可指定目标窗口)。
void sendCombo(uint8_t modifiers, uint16_t vk, void* targetWindow = nullptr) {
    KeyStroke ks;
    ks.modifiers = modifiers;
    ks.virtualKey = vk;
    ks.send(targetWindow);
}

/// 取得手势作用的目标窗口: 优先使用传入的 targetWindow，否则解析前台或光标下顶层窗口。
HWND resolveTargetWindow(void* targetWindowPtr) {
    HWND hwnd = static_cast<HWND>(resolveGestureKeyTarget(
        targetWindowPtr, GetForegroundWindow(), nullptr));
    if (hwnd) return hwnd;
    POINT pt;
    GetCursorPos(&pt);
    hwnd = static_cast<HWND>(windowFromPointSkippingGestureOverlay(pt.x, pt.y));
    hwnd = static_cast<HWND>(resolveGestureKeyTarget(hwnd, nullptr, nullptr));
    return hwnd;
}

void toggleTopmost(HWND hwnd) {
    if (!hwnd) return;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool isTopmost = (ex & WS_EX_TOPMOST) != 0;
    SetWindowPos(hwnd, isTopmost ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    LOG_DEBUG("窗口置顶切换: {} -> {}", isTopmost, !isTopmost);
}

void toggleTransparency(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    std::lock_guard lock(g_layeredStateMutex);
    pruneStaleLayeredRecordsLocked();

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) {
        g_layeredStateMap[hwnd] = LayeredWindowStateRecord{false, 0, 0};
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 180, LWA_ALPHA); // 约 70% 透明度
        LOG_DEBUG("窗口透明度切换: 开启 (70%)");
        return;
    }

    COLORREF colorKey = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    const BOOL queryOk = GetLayeredWindowAttributes(hwnd, &colorKey, &alpha, &flags);
    const auto recIt = g_layeredStateMap.find(hwnd);
    const bool hasAlphaUnder255 = queryOk && ((flags & LWA_ALPHA) != 0) && (alpha < 255);

    if (hasAlphaUnder255) {
        const bool wasOriginallyLayered = (recIt != g_layeredStateMap.end())
            ? recIt->second.originallyLayered
            : true;
        if (wasOriginallyLayered) {
            // 原生分层窗口：仅将 Alpha 恢复为 255，严禁剥离 WS_EX_LAYERED 破坏宿主渲染表面
            const DWORD restoreFlags = (recIt != g_layeredStateMap.end() && recIt->second.originalFlags != 0)
                ? (recIt->second.originalFlags | LWA_ALPHA)
                : ((flags != 0 ? flags : LWA_ALPHA) | LWA_ALPHA);
            const COLORREF restoreKey = (recIt != g_layeredStateMap.end())
                ? recIt->second.originalColorKey
                : colorKey;
            SetLayeredWindowAttributes(hwnd, restoreKey, 255, restoreFlags);
        } else {
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
            RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        if (recIt != g_layeredStateMap.end()) {
            g_layeredStateMap.erase(recIt);
        }
        LOG_DEBUG("窗口透明度切换: 关闭 (不透明)");
    } else {
        if (recIt == g_layeredStateMap.end()) {
            g_layeredStateMap[hwnd] = LayeredWindowStateRecord{true, colorKey, flags};
        }
        const DWORD applyFlags = (queryOk && flags != 0) ? (flags | LWA_ALPHA) : LWA_ALPHA;
        SetLayeredWindowAttributes(hwnd, queryOk ? colorKey : 0, 180, applyFlags);
        LOG_DEBUG("窗口透明度切换: 开启 (70%)");
    }
}

}  // namespace

bool BuiltinCommandDispatcher::hasHandler(BuiltinCommand cmd) const {
    std::shared_lock lock(m_mutex);
    return m_handlers.find(cmd) != m_handlers.end();
}

bool BuiltinCommandDispatcher::dispatchAppCommand(BuiltinCommand cmd) const {
    Handler handler;
    {
        std::shared_lock lock(m_mutex);
        if (const auto it = m_handlers.find(cmd); it != m_handlers.end()) {
            handler = it->second;
        }
    }
    if (handler) {
        handler();
        return true;
    }
    LOG_WARN("应用级内置命令未注册 Handler: cmd={}", static_cast<int>(cmd));
    return false;
}

void BuiltinCommandDispatcher::execute(BuiltinCommand cmd, void* targetWindowPtr) const {
    tools3000::core::TraceId::Scope scope;

    // 若注册了自定义 Handler (如应用级命令路由或测试 Mock 拦截)，优先通过 Handler 分发
    if (hasHandler(cmd)) {
        dispatchAppCommand(cmd);
        return;
    }

    switch (cmd) {
        // ── 窗口管理 (精准作用于鼠标下方目标窗口) ─────────────────────────
        case BuiltinCommand::CloseWindow: {
            sendCombo(MOD_ALT, VK_F4, targetWindowPtr);
            break;
        }
        case BuiltinCommand::MaximizeWindow: {
            HWND h = resolveTargetWindow(targetWindowPtr);
            if (h) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);  // 切换式
            break;
        }
        case BuiltinCommand::MinimizeWindow: {
            if (HWND h = resolveTargetWindow(targetWindowPtr)) ShowWindow(h, SW_MINIMIZE);
            break;
        }
        case BuiltinCommand::RestoreWindow: {
            if (HWND h = resolveTargetWindow(targetWindowPtr)) ShowWindow(h, SW_RESTORE);
            break;
        }
        case BuiltinCommand::ToggleTopmost: {
            toggleTopmost(resolveTargetWindow(targetWindowPtr));
            break;
        }
        case BuiltinCommand::ToggleWindowTransparency: {
            toggleTransparency(resolveTargetWindow(targetWindowPtr));
            break;
        }

        // ── 标签页 / 编辑 (优先作用于鼠标下方目标窗口) ─────────────────────
        case BuiltinCommand::CloseTab:           sendCombo(MOD_CONTROL, 'W', targetWindowPtr); break;
        case BuiltinCommand::RestoreClosedTab:   sendCombo(MOD_CONTROL | MOD_SHIFT, 'T', targetWindowPtr); break;
        case BuiltinCommand::WebSearch: {
            sendCombo(MOD_CONTROL, 'C', targetWindowPtr); // 发送 Ctrl+C 复制
            Sleep(50); // 等待剪贴板更新
            if (OpenClipboard(nullptr)) {
                std::wstring text;
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    const wchar_t* pszText = static_cast<const wchar_t*>(GlobalLock(hData));
                    if (pszText) {
                        text.assign(pszText);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();

                const std::wstring url = buildWebSearchUrl(text);
                if (!url.empty()) {
                    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            break;
        }

        // ── 全局多媒体控制 (全局广播，不切换窗口焦点) ─────────────────────
        case BuiltinCommand::MediaNext:          sendCombo(0, VK_MEDIA_NEXT_TRACK); break;
        case BuiltinCommand::MediaPrev:          sendCombo(0, VK_MEDIA_PREV_TRACK); break;
        case BuiltinCommand::MediaPlayPause:     sendCombo(0, VK_MEDIA_PLAY_PAUSE); break;
        case BuiltinCommand::VolumeUp:           sendCombo(0, VK_VOLUME_UP); break;
        case BuiltinCommand::VolumeDown:         sendCombo(0, VK_VOLUME_DOWN); break;
        case BuiltinCommand::VolumeMute:         sendCombo(0, VK_VOLUME_MUTE); break;

        // ── 系统 / 桌面 ──────────────────────────────────────────────────────
        case BuiltinCommand::ShowDesktop:        sendCombo(MOD_WIN, 'D'); break;
        case BuiltinCommand::TaskView:           sendCombo(MOD_WIN, VK_TAB); break;
        case BuiltinCommand::SwitchDesktop:      executeStagedDesktopSwitch(/*next=*/true); break;
        case BuiltinCommand::PrevVirtualDesktop: executeStagedDesktopSwitch(/*next=*/false); break;
        case BuiltinCommand::NextVirtualDesktop: executeStagedDesktopSwitch(/*next=*/true); break;
        case BuiltinCommand::LockScreen:         LockWorkStation(); break;

        // ── 应用级 (经回调路由, 保持单向依赖) ─────────────────────────────────
        case BuiltinCommand::PauseGestures:
        case BuiltinCommand::TakeScreenshot:
        case BuiltinCommand::StartRecording:
        case BuiltinCommand::ToggleSearch:
        case BuiltinCommand::ShowRadialMenu:
        case BuiltinCommand::PasteAsPin:
            dispatchAppCommand(cmd);
            break;
    }
}

}  // namespace tools3000::gesture
