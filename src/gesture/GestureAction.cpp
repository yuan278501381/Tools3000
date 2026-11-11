// ─────────────────────────────────────────────────────────────────────────────
// GestureAction.cpp — 手势动作执行与序列化
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureAction.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/GestureInputPolicy.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/lua/LuaEngine.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <thread>

namespace tools3000::gesture {

// ── KeyStroke ────────────────────────────────────────────────────────────────

KeyStroke KeyStroke::fromString(const std::string& str) {
    KeyStroke ks;
    std::string remaining = str;
    while (!remaining.empty() && remaining.front() == ' ') remaining.erase(remaining.begin());
    while (!remaining.empty() && remaining.back() == ' ') remaining.pop_back();

    auto startsWithIgnoreCase = [](std::string_view text, std::string_view prefix) noexcept {
        if (text.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            const unsigned char a = static_cast<unsigned char>(text[i]);
            const unsigned char b = static_cast<unsigned char>(prefix[i]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    };

    // 按前缀循环剥离任意顺序修饰键，保留末尾主键（包括 "+" 本身，如 "Ctrl++"）
    bool stripped = true;
    while (stripped && !remaining.empty()) {
        stripped = false;
        auto tryPrefix = [&](std::string_view prefix, uint8_t mod) {
            if (startsWithIgnoreCase(remaining, prefix)) {
                remaining.erase(0, prefix.size());
                ks.modifiers |= mod;
                stripped = true;
                return true;
            }
            return false;
        };
        if (tryPrefix("Ctrl+", MOD_CONTROL)) continue;
        if (tryPrefix("Control+", MOD_CONTROL)) continue;
        if (tryPrefix("Alt+", MOD_ALT)) continue;
        if (tryPrefix("Shift+", MOD_SHIFT)) continue;
        if (tryPrefix("Win+", MOD_WIN)) continue;
        if (tryPrefix("Meta+", MOD_WIN)) continue;
    }

    // 特殊键、标点 OEM 键与多媒体键映射表
    static const std::unordered_map<std::string, uint16_t> keyMap = {
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5},
        {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10},
        {"F11", VK_F11}, {"F12", VK_F12},
        {"Tab", VK_TAB}, {"Enter", VK_RETURN}, {"Return", VK_RETURN}, {"Space", VK_SPACE},
        {"Escape", VK_ESCAPE}, {"Esc", VK_ESCAPE},
        {"Backspace", VK_BACK}, {"Back", VK_BACK},
        {"Delete", VK_DELETE}, {"Del", VK_DELETE},
        {"Insert", VK_INSERT}, {"Ins", VK_INSERT},
        {"Home", VK_HOME}, {"End", VK_END},
        {"PageUp", VK_PRIOR}, {"PgUp", VK_PRIOR},
        {"PageDown", VK_NEXT}, {"PgDn", VK_NEXT},
        {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"MediaNext", VK_MEDIA_NEXT_TRACK}, {"MediaPrev", VK_MEDIA_PREV_TRACK},
        {"MediaPlay", VK_MEDIA_PLAY_PAUSE}, {"MediaPlayPause", VK_MEDIA_PLAY_PAUSE},
        {"VolumeMute", VK_VOLUME_MUTE}, {"VolumeUp", VK_VOLUME_UP}, {"VolumeDown", VK_VOLUME_DOWN},
        // OEM 标点符号与标准键名别名 (杜绝 ASCII 码被误强转为 VK_DELETE 等功能键)
        {".", VK_OEM_PERIOD}, {"Period", VK_OEM_PERIOD}, {"period", VK_OEM_PERIOD},
        {",", VK_OEM_COMMA}, {"Comma", VK_OEM_COMMA}, {"comma", VK_OEM_COMMA},
        {"-", VK_OEM_MINUS}, {"Minus", VK_OEM_MINUS}, {"minus", VK_OEM_MINUS},
        {"=", VK_OEM_PLUS}, {"Plus", VK_OEM_PLUS}, {"plus", VK_OEM_PLUS},
        {"Equal", VK_OEM_PLUS}, {"equal", VK_OEM_PLUS}, {"+", VK_OEM_PLUS},
        {";", VK_OEM_1}, {"Semicolon", VK_OEM_1}, {"semicolon", VK_OEM_1},
        {"/", VK_OEM_2}, {"Slash", VK_OEM_2}, {"slash", VK_OEM_2},
        {"`", VK_OEM_3}, {"Backquote", VK_OEM_3}, {"backquote", VK_OEM_3},
        {"Grave", VK_OEM_3}, {"grave", VK_OEM_3},
        {"[", VK_OEM_4}, {"BracketLeft", VK_OEM_4}, {"bracketleft", VK_OEM_4},
        {"\\", VK_OEM_5}, {"Backslash", VK_OEM_5}, {"backslash", VK_OEM_5},
        {"]", VK_OEM_6}, {"BracketRight", VK_OEM_6}, {"bracketright", VK_OEM_6},
        {"'", VK_OEM_7}, {"Quote", VK_OEM_7}, {"quote", VK_OEM_7},
        {"Apostrophe", VK_OEM_7}, {"apostrophe", VK_OEM_7},
    };

    auto it = keyMap.find(remaining);
    if (it != keyMap.end()) {
        ks.virtualKey = it->second;
    } else if (remaining.size() >= 3 && (remaining.starts_with("0x") || remaining.starts_with("0X"))) {
        try {
            ks.virtualKey = static_cast<uint16_t>(std::stoul(remaining.substr(2), nullptr, 16));
        } catch (...) {
            ks.virtualKey = 0;
        }
    } else if (remaining.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(remaining[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            ks.virtualKey = static_cast<uint16_t>(std::toupper(ch));
        } else {
            ks.virtualKey = 0;
        }
    }

    return ks;
}

std::string KeyStroke::toString() const {
    std::string result;
    if (modifiers & MOD_CONTROL) result += "Ctrl+";
    if (modifiers & MOD_ALT)     result += "Alt+";
    if (modifiers & MOD_SHIFT)   result += "Shift+";
    if (modifiers & MOD_WIN)     result += "Win+";

    // 虚拟键码 → 名称
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        result += static_cast<char>(virtualKey);
    } else if (virtualKey >= '0' && virtualKey <= '9') {
        result += static_cast<char>(virtualKey);
    } else if (virtualKey >= VK_F1 && virtualKey <= VK_F12) {
        result += "F" + std::to_string(virtualKey - VK_F1 + 1);
    } else {
        static const std::unordered_map<uint16_t, std::string> revMap = {
            {VK_TAB, "Tab"}, {VK_RETURN, "Enter"}, {VK_SPACE, "Space"},
            {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_DELETE, "Delete"},
            {VK_INSERT, "Insert"}, {VK_HOME, "Home"}, {VK_END, "End"},
            {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
            {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_UP, "Up"}, {VK_DOWN, "Down"},
            {VK_MEDIA_NEXT_TRACK, "MediaNext"}, {VK_MEDIA_PREV_TRACK, "MediaPrev"},
            {VK_MEDIA_PLAY_PAUSE, "MediaPlayPause"}, {VK_VOLUME_MUTE, "VolumeMute"},
            {VK_VOLUME_UP, "VolumeUp"}, {VK_VOLUME_DOWN, "VolumeDown"},
            {VK_OEM_PERIOD, "."}, {VK_OEM_COMMA, ","}, {VK_OEM_MINUS, "-"},
            {VK_OEM_PLUS, "="}, {VK_OEM_1, ";"}, {VK_OEM_2, "/"},
            {VK_OEM_3, "`"}, {VK_OEM_4, "["}, {VK_OEM_5, "\\"},
            {VK_OEM_6, "]"}, {VK_OEM_7, "'"},
        };
        if (auto it = revMap.find(virtualKey); it != revMap.end()) {
            result += it->second;
        } else {
            result += "0x" + std::format("{:02X}", virtualKey);
        }
    }
    return result;
}

namespace {

bool isGlobalKey(WORD vk) {
    return (vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE) ||
           (vk >= 0xB4 && vk <= 0xB7) ||
           (vk == VK_SNAPSHOT);
}

inline bool isGlobalAction(uint8_t modifiers, WORD vk) noexcept {
    if (isGlobalKey(vk)) return true;
    if (modifiers & MOD_WIN) return true;
    return false;
}

std::wstring windowClassName(HWND hwnd) noexcept {
    wchar_t cls[256] = {};
    if (hwnd) GetClassNameW(hwnd, cls, 256);
    return cls;
}

std::string describeWindow(HWND hwnd) {
    if (!hwnd) return "null";
    if (!IsWindow(hwnd)) {
        return std::format("0x{:X}(stale)", reinterpret_cast<uintptr_t>(hwnd));
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    return std::format("0x{:X} class={} exe={} pid={}",
                       reinterpret_cast<uintptr_t>(root),
                       tools3000::core::WinUtils::wstringToUtf8(windowClassName(root)),
                       tools3000::core::WinUtils::getProcessNameFromWindow(root),
                       pid);
}

bool isTools3000UiWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    return isTools3000UiClassName(windowClassName(root));
}

bool isGesturePassThroughWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;
    if (isGestureOverlayClassName(windowClassName(root))) return true;
    const LONG_PTR ex = GetWindowLongPtrW(root, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_TOPMOST) &&
        (ex & WS_EX_NOACTIVATE) && isTools3000UiWindow(root)) {
        return true;
    }
    return false;
}

HWND asLiveWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

void postCloseWindow(HWND hwnd) noexcept {
    HWND closeTarget = resolveCloseableWindow(hwnd);
    if (!closeTarget) return;
    PostMessageW(closeTarget, WM_SYSCOMMAND, SC_CLOSE, 0);
    PostMessageW(closeTarget, WM_CLOSE, 0, 0);
    LOG_INFO("关闭窗口已投递: {}", describeWindow(closeTarget));
}

/// 冷路径：投递后观察窗口是否已经消失。同线程窗靠 PeekMessage 收 WM_CLOSE，
/// 跨进程窗靠短睡眠等对方消息循环。仍在则返回 true（需要补按键）。
bool postedCloseStillPending(HWND hwnd, DWORD timeoutMs) noexcept {
    if (!hwnd) return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (windowStillAcceptsClose(IsWindow(hwnd) != FALSE, IsWindowVisible(hwnd) != FALSE)) {
        MSG msg{};
        while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!windowStillAcceptsClose(IsWindow(hwnd) != FALSE, IsWindowVisible(hwnd) != FALSE)) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool isWindowCloaked(HWND hwnd) noexcept {
    BOOL cloaked = FALSE;
    if (!hwnd) return false;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != FALSE;
}

bool windowContainsPoint(HWND hwnd, POINT pt) noexcept {
    RECT rc{};
    return hwnd && GetWindowRect(hwnd, &rc) && PtInRect(&rc, pt) != FALSE;
}

bool isAcceptedGestureHitWindow(HWND hwnd, POINT pt) noexcept {
    HWND root = asLiveWindow(hwnd);
    if (!root) return false;
    return gestureHitTestAcceptsWindow(
        IsWindowVisible(root) != FALSE,
        isGesturePassThroughWindow(root),
        isWindowCloaked(root),
        windowContainsPoint(root, pt));
}

struct EnumGestureHitCtx {
    POINT pt{};
    HWND result = nullptr;
};

BOOL CALLBACK enumGestureHitProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumGestureHitCtx*>(lp);
    if (!ctx) return FALSE;
    if (!isAcceptedGestureHitWindow(hwnd, ctx->pt)) return TRUE;
    ctx->result = asLiveWindow(hwnd);
    return FALSE;
}

HWND firstUsableGestureTarget(HWND a, HWND b, HWND c) noexcept {
    for (HWND h : {asLiveWindow(a), asLiveWindow(b), asLiveWindow(c)}) {
        if (h && !isGesturePassThroughWindow(h)) return h;
    }
    return nullptr;
}

struct ThreadInputAttach {
    DWORD self = 0;
    DWORD other = 0;
    bool attached = false;

    ThreadInputAttach(DWORD selfTid, DWORD otherTid) : self(selfTid), other(otherTid) {
        if (other && other != self) {
            attached = AttachThreadInput(self, other, TRUE) != FALSE;
        }
    }
    ~ThreadInputAttach() {
        if (attached) AttachThreadInput(self, other, FALSE);
    }
    ThreadInputAttach(const ThreadInputAttach&) = delete;
    ThreadInputAttach& operator=(const ThreadInputAttach&) = delete;
};

void pulseForegroundUnlock() noexcept {
    // 严禁发送裸 VK_MENU (Alt) KeyUp！发送裸 Alt KeyUp 会导致前台资源管理器 (CabinetWClass/Progman)
    // 立即激活系统菜单栏 SC_KEYMENU 模态循环，彻底锁死桌面文件拖拽 (DoDragDrop) 与正常点击。
    // 使用中立且无绑定的 VK_F24 Down + Up 脉冲满足 Windows 输入事件判定，既能解锁前台激活权限，
    // 又绝不会触发菜单模态或污染任何应用程序状态。
    INPUT inps[2]{};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = VK_F24;
    inps[0].ki.dwFlags = 0;
    inps[1].type = INPUT_KEYBOARD;
    inps[1].ki.wVk = VK_F24;
    inps[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inps, sizeof(INPUT));
}

bool activateTargetWindow(HWND targetHwnd, bool allowWait) noexcept {
    targetHwnd = asLiveWindow(targetHwnd);
    if (!targetHwnd || isGesturePassThroughWindow(targetHwnd)) {
        LOG_WARN("手势目标窗口不可用或属于覆盖层: {}", describeWindow(targetHwnd));
        return false;
    }

    AllowSetForegroundWindow(ASFW_ANY);
    LockSetForegroundWindow(LSFW_UNLOCK);

    HWND curFg = GetForegroundWindow();
    if (curFg == targetHwnd) return true;

    const DWORD selfTid = GetCurrentThreadId();
    const DWORD fgTid = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
    const DWORD targetTid = GetWindowThreadProcessId(targetHwnd, nullptr);
    ThreadInputAttach attachFg(selfTid, fgTid);
    ThreadInputAttach attachTarget(selfTid, targetTid);

    if (IsIconic(targetHwnd)) ShowWindow(targetHwnd, SW_RESTORE);
    BringWindowToTop(targetHwnd);
    SetForegroundWindow(targetHwnd);

    if (GetForegroundWindow() != targetHwnd) {
        pulseForegroundUnlock();
        SetForegroundWindow(targetHwnd);
        BringWindowToTop(targetHwnd);
    }

    if (allowWait && GetForegroundWindow() != targetHwnd) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
        while (std::chrono::steady_clock::now() < deadline) {
            SetForegroundWindow(targetHwnd);
            if (GetForegroundWindow() == targetHwnd) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    const HWND nowFg = GetForegroundWindow();
    const bool ok = (nowFg == targetHwnd);
    if (!ok) {
        wchar_t cls[256] = {};
        GetClassNameW(targetHwnd, cls, 256);
        LOG_WARN("手势目标窗口未能取得前台: hwnd=0x{:X}, class={}, fg=0x{:X}",
                 reinterpret_cast<uintptr_t>(targetHwnd),
                 tools3000::core::WinUtils::wstringToUtf8(cls),
                 reinterpret_cast<uintptr_t>(nowFg));
    }
    return ok;
}

} // namespace

void* resolveGestureKeyTarget(void* candidate, void* gestureStart, void* previousForeground) noexcept {
    HWND resolved = firstUsableGestureTarget(
        static_cast<HWND>(candidate),
        static_cast<HWND>(gestureStart),
        static_cast<HWND>(previousForeground));
    return resolved;
}

void* windowFromPointSkippingGestureOverlay(int x, int y) noexcept {
    POINT pt = {x, y};
    HWND quick = asLiveWindow(WindowFromPoint(pt));
    if (isAcceptedGestureHitWindow(quick, pt)) {
        return quick;
    }

    EnumGestureHitCtx ctx{};
    ctx.pt = pt;
    EnumWindows(enumGestureHitProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

bool gestureActionNeedsInputThread(ActionType type) noexcept {
    return type == ActionType::SendKeys || type == ActionType::BuiltinCommand;
}

static std::atomic<uint8_t> s_lastInjectedModifiers{0};
static std::atomic<DWORD> s_lastInjectedTick{0};

uint8_t getLastInjectedModifiers() noexcept {
    return s_lastInjectedModifiers.load(std::memory_order_relaxed);
}

DWORD getLastInjectedTick() noexcept {
    return s_lastInjectedTick.load(std::memory_order_relaxed);
}

void clearLastInjectedModifiers() noexcept {
    s_lastInjectedModifiers.store(0, std::memory_order_release);
    s_lastInjectedTick.store(0, std::memory_order_release);
}

void ensureModifierReleased(WORD vk) noexcept {
    if (!(GetAsyncKeyState(vk) & 0x8000)) {
        return;
    }

    // 若释放 Alt 键，必须先行注入中立的 VK_F24 脉冲，彻底阻断 Windows 激活 SC_KEYMENU 菜单模态并死锁拖拽
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        INPUT neutral[2]{};
        neutral[0].type = INPUT_KEYBOARD;
        neutral[0].ki.wVk = VK_F24;
        neutral[1].type = INPUT_KEYBOARD;
        neutral[1].ki.wVk = VK_F24;
        neutral[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, neutral, sizeof(INPUT));
    }

    INPUT inps[2]{};
    inps[0].type = INPUT_KEYBOARD;
    inps[0].ki.wVk = vk;
    inps[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    inps[0].ki.dwFlags = KEYEVENTF_KEYUP;

    inps[1] = inps[0];
    inps[1].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;

    if (vk == VK_LWIN || vk == VK_RWIN) {
        SendInput(2, inps, sizeof(INPUT));
    } else if (vk == VK_RMENU || vk == VK_RCONTROL) {
        SendInput(1, &inps[1], sizeof(INPUT));
    } else {
        SendInput(1, &inps[0], sizeof(INPUT));
    }
}

void ensureAllModifiersReleased(bool /*forceAll*/) noexcept {
    // 世界级安全规范：
    // 严禁向 Windows 系统队列盲目喷发裸 KEYUP（特别是 VK_MENU/Alt，会导致前台窗口立即激活系统菜单栏模态 SC_KEYMENU，锁死后续鼠标点击）。
    // 仅针对当前确实处于逻辑按下态 (GetAsyncKeyState & 0x8000) 的修饰键执行精准定向释放。
    std::vector<INPUT> inputs;
    inputs.reserve(8);

    auto addKeyUp = [&](WORD vk, bool extended = false) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inp.ki.dwFlags = KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(inp);
    };

    // 1. Win 键 (左 Win / 右 Win)
    if (GetAsyncKeyState(VK_LWIN) & 0x8000) {
        addKeyUp(VK_LWIN, true);
    }
    if (GetAsyncKeyState(VK_RWIN) & 0x8000) {
        addKeyUp(VK_RWIN, true);
    }

    // 2. Ctrl 键
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
        addKeyUp(VK_CONTROL, false);
    }

    // 3. Shift 键
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
        addKeyUp(VK_SHIFT, false);
    }

    // 4. Alt 键：仅在确认物理/逻辑按下时才补发。
    // 先行注入 VK_F24 脉冲中和系统菜单拦截，杜绝裸 Alt KeyUp 唤起 Windows Explorer 菜单栏或锁死拖拽
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
        INPUT neutralDown{};
        neutralDown.type = INPUT_KEYBOARD;
        neutralDown.ki.wVk = VK_F24;
        inputs.push_back(neutralDown);

        INPUT neutralUp{};
        neutralUp.type = INPUT_KEYBOARD;
        neutralUp.ki.wVk = VK_F24;
        neutralUp.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(neutralUp);

        addKeyUp(VK_MENU, false);
    }

    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
    clearLastInjectedModifiers();
}

void executeStagedDesktopSwitch(bool next) noexcept {
    // 跨虚拟桌面切换专用分步确定性时序管线 (Win + Ctrl + Left/Right)
    // 解决 Windows DWM 跨桌面过渡动画丢弃瞬时组合键释放事件的底层物理缺陷
    static std::mutex s_switchMutex;
    static std::jthread s_switchWorker;

    std::lock_guard<std::mutex> lock(s_switchMutex);
    if (s_switchWorker.joinable()) {
        s_switchWorker.join();
    }
    s_switchWorker = std::jthread([next]() {
        const WORD arrowKey = next ? VK_RIGHT : VK_LEFT;

        auto sendKey = [](WORD vk, bool up, bool extended = false) {
            INPUT inp{};
            inp.type = INPUT_KEYBOARD;
            inp.ki.wVk = vk;
            inp.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            inp.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
            SendInput(1, &inp, sizeof(INPUT));
        };

        // 阶段 1: 按下修饰键与方向键
        sendKey(VK_LWIN, false, true);
        sendKey(VK_CONTROL, false, false);
        sendKey(arrowKey, false, true);

        // 阶段 2: 保持 25ms 确保 Windows Shell 稳健捕获该组合键
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        // 阶段 3: 释放方向键
        sendKey(arrowKey, true, true);

        // 阶段 4: 等待 35ms 跨桌面过渡动画建立
        std::this_thread::sleep_for(std::chrono::milliseconds(35));

        // 阶段 5: 逆序释放修饰键
        sendKey(VK_CONTROL, true, false);
        sendKey(VK_LWIN, true, true);

        // 阶段 6: 120ms 后 (DWM 动画完成)，轻量安全核查：若 Win/Ctrl 仍异常残留，精准补发释放
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
            sendKey(VK_LWIN, true, true);
            sendKey(VK_RWIN, true, true);
        }
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            sendKey(VK_CONTROL, true, false);
        }
    });
}

// ── KeyStroke::send ──────────────────────────────────────────────────────────

namespace {

bool hasConflictingPhysicalModifiers(uint8_t targetMods) noexcept {
    if (!(targetMods & MOD_CONTROL) && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        return true;
    }
    if (!(targetMods & MOD_ALT) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        return true;
    }
    if (!(targetMods & MOD_SHIFT) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        return true;
    }
    if (!(targetMods & MOD_WIN) &&
        ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))) {
        return true;
    }
    return false;
}

void waitForPhysicalModifiersReleaseBounded(uint8_t targetMods, DWORD maxWaitMs = 120) noexcept {
    if (!hasConflictingPhysicalModifiers(targetMods)) {
        return;
    }
    constexpr DWORD kPollSliceMs = 4;
    const DWORD startTick = GetTickCount();
    while (hasConflictingPhysicalModifiers(targetMods) &&
           (GetTickCount() - startTick) < maxWaitMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollSliceMs));
    }
}

void sendKeyStrokeWorkerImpl(const KeyStroke& ks, void* targetWindowPtr) {
    if (ks.virtualKey == 0) {
        LOG_WARN("KeyStroke::send 被调用但 virtualKey 为空, 跳过");
        return;
    }

    HWND targetHwnd = asLiveWindow(static_cast<HWND>(targetWindowPtr));
    if (isGesturePassThroughWindow(targetHwnd)) {
        LOG_WARN("拒绝向手势覆盖层注入按键: keys={}, target={}",
                 ks.toString(), describeWindow(targetHwnd));
        targetHwnd = nullptr;
    }

    HWND fgBefore = GetForegroundWindow();
    if (!isGlobalAction(ks.modifiers, ks.virtualKey) && !targetHwnd) {
        POINT pt{};
        GetCursorPos(&pt);
        targetHwnd = static_cast<HWND>(resolveGestureKeyTarget(
            fgBefore, windowFromPointSkippingGestureOverlay(pt.x, pt.y), nullptr));
    }

    LOG_INFO("手势按键注入: keys={}, target={}, fg={}",
             ks.toString(), describeWindow(targetHwnd), describeWindow(fgBefore));

    if (!isGlobalAction(ks.modifiers, ks.virtualKey) && !targetHwnd) {
        LOG_WARN("无外部目标窗口，放弃按键注入: keys={}, fg={}",
                 ks.toString(), describeWindow(fgBefore));
        return;
    }

    uint8_t sendMods = ks.modifiers;
    uint16_t sendVk = ks.virtualKey;
    const std::wstring targetCls = windowClassName(targetHwnd);
    const bool isShellTarget = isSystemDesktopOrShellWindow(targetCls) ||
                               (targetHwnd && resolveCloseableWindow(targetHwnd) == nullptr);

    // 严禁向 Windows 桌面、任务栏或系统外壳注入关闭按键 (Alt+F4 / Ctrl+W)
    if (!isGlobalAction(ks.modifiers, ks.virtualKey) && targetHwnd && isShellTarget) {
        if (keyStrokeShouldPostClose(ks.modifiers, ks.virtualKey) ||
            keyStrokeIsCtrlW(ks.modifiers, ks.virtualKey)) {
            LOG_WARN("目标为 Windows 桌面或系统外壳，拦截关窗动作，放弃按键注入: keys={}, target={}",
                     ks.toString(), describeWindow(targetHwnd));
            return;
        }
    }

    bool closing = !isGlobalAction(ks.modifiers, ks.virtualKey) && targetHwnd &&
        keyStrokeShouldCloseWindow(ks.modifiers, ks.virtualKey, targetCls);
    if (closing) {
        HWND closeTarget = resolveCloseableWindow(targetHwnd);
        if (closeTarget) {
            std::string exe = tools3000::core::WinUtils::toLower(
                tools3000::core::WinUtils::getProcessNameFromWindow(closeTarget));
            if (exe == "explorer.exe") {
                std::wstring cls = tools3000::core::WinUtils::getWindowClassName(closeTarget);
                if (cls != L"CabinetWClass") {
                    LOG_WARN("拦截对 Windows Shell / 桌面窗口的关闭请求: hwnd=0x{:X} class={} exe={}",
                             reinterpret_cast<uintptr_t>(closeTarget),
                             tools3000::core::WinUtils::wstringToUtf8(cls), exe);
                    closeTarget = nullptr;
                }
            }
        }
        if (closeTarget) {
            targetHwnd = closeTarget;
            if (tools3000::core::WinUtils::isWindowHigherIntegrity(targetHwnd)) {
                LOG_WARN("目标窗口完整性更高，关闭可能被 UIPI 拦截: {}", describeWindow(targetHwnd));
            }
            postCloseWindow(targetHwnd);
            const bool stillPending = postedCloseStillPending(targetHwnd, kCloseObserveTimeoutMs);
            if (!closeShouldSendKeyFallback(true, stillPending)) {
                LOG_INFO("关闭已生效，不再补发 Alt+F4: {}", describeWindow(targetHwnd));
                return;
            }
            sendMods = MOD_ALT;
            sendVk = VK_F4;
            LOG_INFO("关闭投递后窗口仍在，补发 Alt+F4: {}", describeWindow(targetHwnd));
        } else {
            closing = false;
        }
    } else if (!isGlobalAction(ks.modifiers, ks.virtualKey) && targetHwnd &&
               tools3000::core::WinUtils::isWindowHigherIntegrity(targetHwnd)) {
        LOG_WARN("目标窗口完整性更高，按键注入可能被 UIPI 拦截: keys={}, target={}",
                 ks.toString(), describeWindow(targetHwnd));
    }

    // 非全局键先把输入焦点切到目标窗口，再原子发送。切不过则放弃，避免 Ctrl+W 打进前台的设置页。
    if (!isGlobalAction(sendMods, sendVk) && targetHwnd) {
        if (!activateTargetWindow(targetHwnd, /*allowWait=*/true)) {
            if (closing) {
                LOG_WARN("未能激活目标窗口，仅保留已投递的关闭消息: target={}",
                         describeWindow(targetHwnd));
                return;
            }
            LOG_WARN("未能激活目标窗口，放弃注入: keys={}, target={}, fg={}",
                     ks.toString(), describeWindow(targetHwnd), describeWindow(GetForegroundWindow()));
            return;
        }
    }

    // 在后台线程有界轮询等待冲突物理修饰键自然释放，严禁在 UI 主线程 sleep 阻塞消息泵
    waitForPhysicalModifiersReleaseBounded(sendMods, 120);

    // 注入前先确保无残留的粘滞修饰键干扰
    ensureAllModifiersReleased(false);

    // RAII 保护：确保无论以任何路径离开作用域，送出的修饰键得到兜底释放与核查
    struct ModifierReleaseGuard {
        uint8_t mods;
        ~ModifierReleaseGuard() {
            ensureAllModifiersReleased(false);
        }
    } guard{sendMods};

    // 2. 构造原子性 INPUT 数组: 按下修饰键 → 主键按下 → 主键释放 → 逆序释放修饰键
    // 必须在单次 SendInput 调用中原子性提交全部 Down 与 Up，绝不在中间插入 Sleep，彻底杜绝修饰键粘滞与幽灵按键问题
    std::vector<INPUT> inputs;
    inputs.reserve(10);

    auto addKey = [&](WORD vk, bool up) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        inp.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0);
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_LEFT || vk == VK_RIGHT ||
            vk == VK_UP || vk == VK_DOWN || vk == VK_DELETE || vk == VK_INSERT ||
            vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
            vk == VK_APPS) {
            inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        inputs.push_back(inp);
    };

    // 修饰键按下顺序
    static constexpr std::array<std::pair<uint8_t, WORD>, 4> kMods = {{
        {MOD_CONTROL, VK_CONTROL}, {MOD_ALT, VK_MENU},
        {MOD_SHIFT, VK_SHIFT},     {MOD_WIN, VK_LWIN},
    }};

    // 按下修饰键
    for (auto [mod, vk] : kMods) {
        if (sendMods & mod) addKey(vk, /*up=*/false);
    }
    // 按下主键
    addKey(sendVk, /*up=*/false);

    // 释放主键
    addKey(sendVk, /*up=*/true);
    // 逆序释放修饰键
    for (auto it = kMods.rbegin(); it != kMods.rend(); ++it) {
        if (sendMods & it->first) addKey(it->second, /*up=*/true);
    }

    // 原子性提交整组按键
    UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        LOG_WARN("SendInput 未完全发送: expected={}, sent={}, lastError={}",
                 inputs.size(), sent, GetLastError());
        ensureAllModifiersReleased(false);
    }
}

}  // namespace

void KeyStroke::send(void* targetWindowPtr) const {
    if (virtualKey == 0) {
        LOG_WARN("KeyStroke::send 被调用但 virtualKey 为空, 跳过");
        return;
    }

    // 防阻塞守护：若调用方意外处于 UI 主线程，异步移交后台线程完成修饰键释放轮询与 SendInput，
    // 100% 杜绝在 UI 主线程执行 sleep_for 阻塞消息泵。
    const auto& dispatcher = tools3000::core::MainThreadDispatcher::instance();
    if (dispatcher.isInitialized() && dispatcher.isOwnerThread()) {
        const KeyStroke copy = *this;
        const std::string traceId = tools3000::core::TraceId::current();
        std::thread([copy, targetWindowPtr, traceId]() {
            if (!traceId.empty()) {
                tools3000::core::TraceId::setCurrent(traceId);
            }
            try {
                sendKeyStrokeWorkerImpl(copy, targetWindowPtr);
            } catch (const std::exception& e) {
                LOG_ERROR("Background KeyStroke::send failed: keys={}, error={}",
                          copy.toString(), e.what());
            } catch (...) {
                LOG_ERROR("Background KeyStroke::send unknown exception: keys={}",
                          copy.toString());
            }
        }).detach();
        return;
    }

    sendKeyStrokeWorkerImpl(*this, targetWindowPtr);
}

// ── GestureAction::execute ───────────────────────────────────────────────────

void GestureAction::execute(void* targetWindowPtr) const {
    tools3000::core::TraceId::Scope scope;

    switch (type) {
        case ActionType::SendKeys: {
            LOG_DEBUG("执行手势动作: SendKeys, keys={}", keyStroke.toString());
            // 若为虚拟桌面切换组合键 (Win+Ctrl+Left 或 Win+Ctrl+Right)，自动路由至确定性分步时序管线
            if (keyStroke.modifiers == (MOD_WIN | MOD_CONTROL) &&
                (keyStroke.virtualKey == VK_LEFT || keyStroke.virtualKey == VK_RIGHT)) {
                executeStagedDesktopSwitch(keyStroke.virtualKey == VK_RIGHT);
            } else {
                keyStroke.send(targetWindowPtr);
            }
            break;
        }

        case ActionType::LuaScript: {
            LOG_DEBUG("执行手势动作: LuaScript, name={}, script={}", name, luaScript.substr(0, std::min(luaScript.size(), (size_t)100)));
            tools3000::core::ScriptContext ctx;
            // 权限绑定到名称和内容；编辑同名脚本后必须重新授权，不能继承旧代码权限。
            ctx.scriptId = "gesture:" + name + ":" +
                           std::to_string(std::hash<std::string>{}(luaScript));
            ctx.scriptName = name.empty() ? "自定义手势脚本" : name;
            ctx.requestedPerms = requestedPermissions.empty()
                ? tools3000::core::LuaPermission::Safe
                : tools3000::core::parseLuaPermissions(requestedPermissions);
            ctx.interactive = true;
            tools3000::core::LuaEngine::instance().authorizeAndExecute(luaScript, ctx);
            break;
        }

        case ActionType::BuiltinCommand: {
            LOG_DEBUG("执行手势动作: BuiltinCommand, cmd={}", static_cast<int>(builtinCmd));
            BuiltinCommandDispatcher::instance().execute(builtinCmd, targetWindowPtr);
            break;
        }

        case ActionType::RunProgram: {
            LOG_DEBUG("执行手势动作: RunProgram, path={}", programPath);
            const std::wstring wPath = tools3000::core::WinUtils::utf8ToWstring(programPath);
            const std::wstring wArgs = tools3000::core::WinUtils::utf8ToWstring(programArgs);
            ShellExecuteW(nullptr, L"open", wPath.c_str(),
                          wArgs.empty() ? nullptr : wArgs.c_str(),
                          nullptr, SW_SHOWNORMAL);
            break;
        }
    }
}

// ── JSON 序列化 ──────────────────────────────────────────────────────────────

nlohmann::json GestureAction::toJson() const {
    nlohmann::json j;
    j["type"] = static_cast<int>(type);
    j["name"] = name;
    j["description"] = description;

    switch (type) {
        case ActionType::SendKeys:
            j["keyStroke"] = keyStroke.toString();
            break;
        case ActionType::LuaScript:
            j["luaScript"] = luaScript;
            if (!requestedPermissions.empty()) {
                j["permissions"] = requestedPermissions;
            }
            break;
        case ActionType::BuiltinCommand:
            j["builtinCmd"] = static_cast<int>(builtinCmd);
            break;
        case ActionType::RunProgram:
            j["programPath"] = programPath;
            j["programArgs"] = programArgs;
            break;
    }
    return j;
}

GestureAction GestureAction::fromJson(const nlohmann::json& j) {
    GestureAction action;
    action.type = ActionType::SendKeys;
    action.builtinCmd = BuiltinCommand::CloseWindow;
    if (!j.is_object()) {
        return action;
    }

    int rawType = 0;
    if (j.contains("type") && j["type"].is_number_integer()) {
        rawType = j["type"].get<int>();
    }
    rawType = std::clamp(
        rawType,
        static_cast<int>(ActionType::SendKeys),
        static_cast<int>(ActionType::RunProgram));
    action.type = static_cast<ActionType>(rawType);

    if (j.contains("name") && j["name"].is_string()) {
        action.name = j["name"].get<std::string>();
    }
    if (j.contains("description") && j["description"].is_string()) {
        action.description = j["description"].get<std::string>();
    }

    switch (action.type) {
        case ActionType::SendKeys:
            if (j.contains("keyStroke") && j["keyStroke"].is_string()) {
                action.keyStroke = KeyStroke::fromString(j["keyStroke"].get<std::string>());
            }
            break;
        case ActionType::LuaScript:
            if (j.contains("luaScript") && j["luaScript"].is_string()) {
                action.luaScript = j["luaScript"].get<std::string>();
            }
            if (j.contains("permissions") && j["permissions"].is_array()) {
                action.requestedPermissions.clear();
                for (const auto& perm : j["permissions"]) {
                    if (perm.is_string()) {
                        action.requestedPermissions.push_back(perm.get<std::string>());
                    }
                }
            }
            break;
        case ActionType::BuiltinCommand: {
            int rawCmd = 0;
            if (j.contains("builtinCmd") && j["builtinCmd"].is_number_integer()) {
                rawCmd = j["builtinCmd"].get<int>();
            }
            rawCmd = std::clamp(
                rawCmd,
                static_cast<int>(BuiltinCommand::CloseWindow),
                static_cast<int>(kMaxBuiltinCommand));
            action.builtinCmd = static_cast<BuiltinCommand>(rawCmd);
            break;
        }
        case ActionType::RunProgram:
            if (j.contains("programPath") && j["programPath"].is_string()) {
                action.programPath = j["programPath"].get<std::string>();
            }
            if (j.contains("programArgs") && j["programArgs"].is_string()) {
                action.programArgs = j["programArgs"].get<std::string>();
            }
            break;
    }
    return action;
}

nlohmann::json GestureMapping::toJson() const {
    nlohmann::json j;
    if (!id.empty()) j["id"] = id;
    j["enabled"] = enabled;
    j["instantExecute"] = instantExecute;
    j["silentToast"] = silentToast;
    j["gestureCode"] = gestureCode;
    j["action"] = action.toJson();
    return j;
}

GestureMapping GestureMapping::fromJson(const nlohmann::json& j) {
    GestureMapping mapping;
    if (!j.is_object()) {
        return mapping;
    }
    if (j.contains("id") && j["id"].is_string()) {
        mapping.id = j["id"].get<std::string>();
    }
    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        mapping.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("instantExecute") && j["instantExecute"].is_boolean()) {
        mapping.instantExecute = j["instantExecute"].get<bool>();
    }
    if (j.contains("silentToast") && j["silentToast"].is_boolean()) {
        mapping.silentToast = j["silentToast"].get<bool>();
    }
    if (j.contains("gestureCode") && j["gestureCode"].is_string()) {
        mapping.gestureCode = j["gestureCode"].get<std::string>();
    }
    if (j.contains("action") && j["action"].is_object()) {
        mapping.action = GestureAction::fromJson(j["action"]);
    }
    return mapping;
}

}  // namespace tools3000::gesture
