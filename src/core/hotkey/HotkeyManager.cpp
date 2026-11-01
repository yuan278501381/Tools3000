// ─────────────────────────────────────────────────────────────────────────────
// HotkeyManager.cpp — 全局快捷键管理器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/HotkeyPolicy.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <unordered_map>

namespace tools3000::core {

namespace {

constexpr auto HotkeyDispatchTimeout = std::chrono::seconds(10);

bool isForeignHotkeyThread(DWORD ownerThreadId) noexcept {
    return ownerThreadId != 0 && ownerThreadId != GetCurrentThreadId();
}

template <typename Result, typename Operation>
Result dispatchToHotkeyThread(DWORD ownerThreadId,
                              const char* operationName,
                              Result failureResult,
                              Operation&& operation) {
    struct DispatchState {
        std::promise<Result> promise;
        std::atomic<bool> cancelled{false};
    };

    auto state = std::make_shared<DispatchState>();
    auto future = state->promise.get_future();
    const bool posted = MainThreadDispatcher::instance().post(
        [state, ownerThreadId, operationName,
         operation = std::forward<Operation>(operation), failureResult]() mutable {
            if (state->cancelled.load(std::memory_order_acquire)) return;
            if (GetCurrentThreadId() != ownerThreadId) {
                LOG_ERROR("快捷键操作派发到了错误线程: operation={}, expected={}, actual={}",
                          operationName, ownerThreadId, GetCurrentThreadId());
                state->promise.set_value(std::move(failureResult));
                return;
            }
            try {
                state->promise.set_value(operation());
            } catch (const std::exception& e) {
                LOG_ERROR("快捷键主线程操作异常: operation={}, error={}", operationName, e.what());
                state->promise.set_value(std::move(failureResult));
            } catch (...) {
                LOG_ERROR("快捷键主线程操作未知异常: operation={}", operationName);
                state->promise.set_value(std::move(failureResult));
            }
        });
    if (!posted) {
        LOG_ERROR("快捷键操作无法派发到窗口线程: operation={}", operationName);
        return failureResult;
    }
    if (future.wait_for(HotkeyDispatchTimeout) != std::future_status::ready) {
        state->cancelled.store(true, std::memory_order_release);
        LOG_ERROR("快捷键操作等待窗口线程超时: operation={}", operationName);
        return failureResult;
    }
    return future.get();
}

template <typename Operation>
void dispatchToHotkeyThread(DWORD ownerThreadId,
                            const char* operationName,
                            Operation&& operation) {
    (void)dispatchToHotkeyThread<bool>(
        ownerThreadId, operationName, false,
        [operation = std::forward<Operation>(operation)]() mutable {
            operation();
            return true;
        });
}

}  // namespace

// ── HotkeyDef 安全矩阵与序列化 ──────────────────────────────────────────────

bool HotkeyDef::isSafe() const {
    if (virtualKey == 0) return true;

    const auto mods = static_cast<UINT>(modifiers);
    const bool hasCtrl = (mods & MOD_CONTROL) != 0;
    const bool hasAlt  = (mods & MOD_ALT) != 0;
    const bool hasWin  = (mods & MOD_WIN) != 0;
    const bool hasCommandMod = hasCtrl || hasAlt || hasWin;

    // 凡带有 Ctrl、Alt 或 Win 的组合快捷键均绝对安全
    if (hasCommandMod) {
        return true;
    }

    // 无主要修饰键时：放行单键功能键 (F1~F24)
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) return true;

    // 放行独立控制键与小键盘独立算符
    if (virtualKey == VK_SNAPSHOT || virtualKey == VK_PAUSE || virtualKey == VK_SCROLL) return true;
    if (virtualKey == VK_MULTIPLY || virtualKey == VK_ADD || virtualKey == VK_SUBTRACT ||
        virtualKey == VK_DIVIDE || virtualKey == VK_DECIMAL) return true;

    // 放行媒体播放与音量按键
    if (virtualKey == VK_VOLUME_MUTE || virtualKey == VK_VOLUME_DOWN ||
        virtualKey == VK_VOLUME_UP || virtualKey == VK_MEDIA_NEXT_TRACK ||
        virtualKey == VK_MEDIA_PREV_TRACK || virtualKey == VK_MEDIA_STOP ||
        virtualKey == VK_MEDIA_PLAY_PAUSE) return true;

    // 严苛红线：严格禁止单字母 (A~Z)、单数字 (0~9)、单标点符号或单编辑键无修饰键注册为全局热键
    return false;
}

std::string HotkeyDef::toString() const {
    if (virtualKey == 0) return {};
    std::string result;
    auto mods = static_cast<UINT>(modifiers);
    if (mods & MOD_CONTROL) result += "Ctrl+";
    if (mods & MOD_ALT)     result += "Alt+";
    if (mods & MOD_SHIFT)   result += "Shift+";
    if (mods & MOD_WIN)     result += "Win+";

    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        result += "F" + std::to_string(virtualKey - VK_F1 + 1);
    } else if ((virtualKey >= 'A' && virtualKey <= 'Z') ||
               (virtualKey >= '0' && virtualKey <= '9')) {
        result += static_cast<char>(virtualKey);
    } else if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9) {
        result += "Num" + std::to_string(virtualKey - VK_NUMPAD0);
    } else {
        switch (virtualKey) {
            case VK_OEM_3:      result += "`"; break;
            case VK_OEM_MINUS:  result += "-"; break;
            case VK_OEM_PLUS:   result += "="; break;
            case VK_OEM_4:      result += "["; break;
            case VK_OEM_6:      result += "]"; break;
            case VK_OEM_5:      result += "\\"; break;
            case VK_OEM_1:      result += ";"; break;
            case VK_OEM_7:      result += "'"; break;
            case VK_OEM_COMMA:  result += ","; break;
            case VK_OEM_PERIOD: result += "."; break;
            case VK_OEM_2:      result += "/"; break;

            case VK_MULTIPLY:   result += "Num*"; break;
            case VK_ADD:        result += "Num+"; break;
            case VK_SUBTRACT:   result += "Num-"; break;
            case VK_DECIMAL:    result += "Num."; break;
            case VK_DIVIDE:     result += "Num/"; break;

            case VK_SPACE:      result += "Space"; break;
            case VK_RETURN:     result += "Enter"; break;
            case VK_ESCAPE:     result += "Escape"; break;
            case VK_TAB:        result += "Tab"; break;
            case VK_DELETE:     result += "Delete"; break;
            case VK_INSERT:     result += "Insert"; break;
            case VK_HOME:       result += "Home"; break;
            case VK_END:        result += "End"; break;
            case VK_PRIOR:      result += "PageUp"; break;
            case VK_NEXT:       result += "PageDown"; break;
            case VK_UP:         result += "Up"; break;
            case VK_DOWN:       result += "Down"; break;
            case VK_LEFT:       result += "Left"; break;
            case VK_RIGHT:      result += "Right"; break;
            case VK_SNAPSHOT:   result += "PrintScreen"; break;
            case VK_BACK:       result += "Backspace"; break;
            case VK_CAPITAL:    result += "CapsLock"; break;
            case VK_SCROLL:     result += "ScrollLock"; break;
            case VK_NUMLOCK:    result += "NumLock"; break;
            case VK_PAUSE:      result += "Pause"; break;
            case VK_APPS:       result += "Apps"; break;

            case VK_VOLUME_MUTE:       result += "VolumeMute"; break;
            case VK_VOLUME_DOWN:       result += "VolumeDown"; break;
            case VK_VOLUME_UP:         result += "VolumeUp"; break;
            case VK_MEDIA_NEXT_TRACK:  result += "MediaNext"; break;
            case VK_MEDIA_PREV_TRACK:  result += "MediaPrev"; break;
            case VK_MEDIA_STOP:        result += "MediaStop"; break;
            case VK_MEDIA_PLAY_PAUSE:  result += "MediaPlayPause"; break;

            default:
                result += "0x" + std::format("{:02X}", virtualKey);
                break;
        }
    }

    return result;
}

std::optional<HotkeyDef> HotkeyDef::fromString(const std::string& str) {
    HotkeyDef def;
    if (str.empty()) return std::nullopt;

    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    };

    std::string s = str;
    trim(s);
    if (s.empty()) return std::nullopt;

    // 解析前缀修饰键 (例如 "Ctrl+", "Alt+", "Shift+", "Win+")，彻底解决 Num+ 与 + 分词碎裂并兼容空白字符
    auto consumeModifier = [&](const std::string& name, ModKey mod) -> bool {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!lower.starts_with(name)) return false;
        size_t pos = name.size();
        while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos]))) {
            pos++;
        }
        if (pos < lower.size() && lower[pos] == '+') {
            pos++; // 消费 '+'
            while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos]))) {
                pos++;
            }
            def.modifiers = def.modifiers | mod;
            s = s.substr(pos);
            return true;
        }
        return false;
    };

    while (true) {
        if (consumeModifier("ctrl", ModKey::Ctrl) ||
            consumeModifier("control", ModKey::Ctrl) ||
            consumeModifier("alt", ModKey::Alt) ||
            consumeModifier("shift", ModKey::Shift) ||
            consumeModifier("win", ModKey::Win) ||
            consumeModifier("meta", ModKey::Win)) {
            continue;
        }
        break;
    }

    trim(s);
    if (s.empty()) return std::nullopt;

    // 1. 特殊命名键表
    static const std::unordered_map<std::string, UINT> s_namedKeys = {
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16},
        {"F17", VK_F17}, {"F18", VK_F18}, {"F19", VK_F19}, {"F20", VK_F20},
        {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},

        {"Space", VK_SPACE}, {"Enter", VK_RETURN}, {"Return", VK_RETURN},
        {"Escape", VK_ESCAPE}, {"Esc", VK_ESCAPE},
        {"Tab", VK_TAB}, {"Delete", VK_DELETE}, {"Del", VK_DELETE},
        {"Insert", VK_INSERT}, {"Ins", VK_INSERT},
        {"Home", VK_HOME}, {"End", VK_END},
        {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT},
        {"PgUp", VK_PRIOR}, {"PgDn", VK_NEXT},
        {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
        {"PrintScreen", VK_SNAPSHOT}, {"PrtScn", VK_SNAPSHOT}, {"Snapshot", VK_SNAPSHOT},
        {"Backspace", VK_BACK}, {"Back", VK_BACK},
        {"CapsLock", VK_CAPITAL}, {"ScrollLock", VK_SCROLL},
        {"NumLock", VK_NUMLOCK}, {"Pause", VK_PAUSE},
        {"Apps", VK_APPS}, {"ContextMenu", VK_APPS},

        // 小键盘
        {"Num0", VK_NUMPAD0}, {"Num1", VK_NUMPAD1}, {"Num2", VK_NUMPAD2},
        {"Num3", VK_NUMPAD3}, {"Num4", VK_NUMPAD4}, {"Num5", VK_NUMPAD5},
        {"Num6", VK_NUMPAD6}, {"Num7", VK_NUMPAD7}, {"Num8", VK_NUMPAD8},
        {"Num9", VK_NUMPAD9},
        {"Num+", VK_ADD}, {"Num-", VK_SUBTRACT}, {"Num*", VK_MULTIPLY},
        {"Num/", VK_DIVIDE}, {"Num.", VK_DECIMAL}, {"NumEnter", VK_RETURN},

        // 多媒体按键
        {"VolumeMute", VK_VOLUME_MUTE}, {"Mute", VK_VOLUME_MUTE},
        {"VolumeDown", VK_VOLUME_DOWN}, {"VolumeUp", VK_VOLUME_UP},
        {"MediaNext", VK_MEDIA_NEXT_TRACK}, {"MediaNextTrack", VK_MEDIA_NEXT_TRACK},
        {"MediaPrev", VK_MEDIA_PREV_TRACK}, {"MediaPrevTrack", VK_MEDIA_PREV_TRACK}, {"MediaPreviousTrack", VK_MEDIA_PREV_TRACK},
        {"MediaStop", VK_MEDIA_STOP}, {"MediaPlayPause", VK_MEDIA_PLAY_PAUSE},

        // OEM 符号键
        {"`", VK_OEM_3}, {"~", VK_OEM_3},
        {"-", VK_OEM_MINUS}, {"_", VK_OEM_MINUS},
        {"=", VK_OEM_PLUS}, {"+", VK_OEM_PLUS},
        {"[", VK_OEM_4}, {"{", VK_OEM_4},
        {"]", VK_OEM_6}, {"}", VK_OEM_6},
        {"\\", VK_OEM_5}, {"|", VK_OEM_5},
        {";", VK_OEM_1}, {":", VK_OEM_1},
        {"'", VK_OEM_7}, {"\"", VK_OEM_7},
        {",", VK_OEM_COMMA}, {"<", VK_OEM_COMMA},
        {".", VK_OEM_PERIOD}, {">", VK_OEM_PERIOD},
        {"/", VK_OEM_2}, {"?", VK_OEM_2}
    };

    auto it = s_namedKeys.find(s);
    if (it != s_namedKeys.end()) {
        def.virtualKey = it->second;
        return def;
    }

    // 2. 忽略大小写的命名匹配 (例如 "pagedown", "f1", "space")
    std::string sLower = s;
    std::transform(sLower.begin(), sLower.end(), sLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& [k, v] : s_namedKeys) {
        std::string kLower = k;
        std::transform(kLower.begin(), kLower.end(), kLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (sLower == kLower) {
            def.virtualKey = v;
            return def;
        }
    }

    // 3. 单字母 (A~Z) 与 单数字 (0~9)
    if (s.size() == 1 && std::isalnum(static_cast<unsigned char>(s[0]))) {
        def.virtualKey = static_cast<UINT>(std::toupper(static_cast<unsigned char>(s[0])));
        return def;
    }

    // 4. 十六进制 0xXX 形式
    if (s.starts_with("0x") || s.starts_with("0X")) {
        try {
            const auto value = std::stoul(s.substr(2), nullptr, 16);
            if (value == 0 || value > 0xFF) return std::nullopt;
            def.virtualKey = static_cast<UINT>(value);
            return def;
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

// ── HotkeyManager ────────────────────────────────────────────────────────────

HotkeyManager& HotkeyManager::instance() {
    static HotkeyManager inst;
    return inst;
}

bool HotkeyManager::canFallbackToHook(const HotkeyDef& def) {
    return def.isSafe();
}

bool HotkeyManager::isSingleFunctionKey(const HotkeyDef& def) {
    if (def.modifiers != ModKey::None) return false;
    const UINT vk = def.virtualKey;
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    if (vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_SCROLL) return true;
    if (vk == VK_MULTIPLY || vk == VK_ADD || vk == VK_SUBTRACT ||
        vk == VK_DIVIDE || vk == VK_DECIMAL) return true;
    if (vk == VK_VOLUME_MUTE || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_UP ||
        vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK ||
        vk == VK_MEDIA_STOP || vk == VK_MEDIA_PLAY_PAUSE) return true;
    return false;
}

void HotkeyManager::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
    KeyboardHook::instance().setPaused(paused);
}

bool HotkeyManager::isPaused() const {
    return m_paused.load(std::memory_order_relaxed);
}

void HotkeyManager::setFullscreenSuspended(bool suspended) {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        dispatchToHotkeyThread(ownerThreadId, "setFullscreenSuspended",
                               [this, suspended]() { setFullscreenSuspended(suspended); });
        return;
    }

    KeyboardHook::instance().setFullscreenSuspended(suspended);

    std::lock_guard lock(m_mutex);
    const bool wasSuspended = m_fullscreenSuspended.exchange(suspended, std::memory_order_acq_rel);
    if (wasSuspended == suspended) {
        return;
    }

    if (suspended) {
        for (auto& [name, entry] : m_hotkeys) {
            if (entry.registered) {
                if (entry.useHook) {
                    KeyboardHook::instance().unregisterHookHotkey(entry.id);
                } else if (m_hwnd) {
                    UnregisterHotKey(m_hwnd, entry.id);
                }
                m_idToName.erase(entry.id);
                entry.registered = false;
                entry.useHook = false;
                entry.suspendedByFullscreen = true;
            }
        }
        LOG_INFO("全屏游戏免打扰已激活：全局快捷键已临时卸下并交还前台游戏");
    } else {
        for (auto& [name, entry] : m_hotkeys) {
            if (!entry.armed || entry.def.virtualKey == 0) {
                entry.suspendedByFullscreen = false;
                continue;
            }
            if (entry.registered) {
                entry.suspendedByFullscreen = false;
                continue;
            }
            if (!entry.suspendedByFullscreen) {
                continue;
            }
            entry.suspendedByFullscreen = false;
            bool ok = false;
            bool useHook = false;
            if (m_hwnd && IsWindow(m_hwnd)) {
                UINT mods = static_cast<UINT>(entry.def.modifiers) | MOD_NOREPEAT;
                ok = (RegisterHotKey(m_hwnd, entry.id, mods, entry.def.virtualKey) != FALSE);
            }
            if (!ok && canFallbackToHook(entry.def)) {
                KeyboardHook::instance().registerHookHotkey(entry.id, entry.def, name, entry.callback);
                KeyboardHook::instance().install();
                ok = true;
                useHook = true;
            }
            entry.registered = ok;
            entry.useHook = useHook;
            if (ok) {
                m_idToName[entry.id] = name;
            }
        }
        LOG_INFO("全屏游戏免打扰已解除：全局快捷键已恢复占用");
    }
}

bool HotkeyManager::isFullscreenSuspended() const {
    return m_fullscreenSuspended.load(std::memory_order_acquire);
}

void HotkeyManager::initialize(HWND messageWindow) {
    m_hwnd = messageWindow;
    DWORD ownerThreadId = 0;
    if (messageWindow) ownerThreadId = GetWindowThreadProcessId(messageWindow, nullptr);
    if (ownerThreadId == 0) ownerThreadId = GetCurrentThreadId();
    m_ownerThreadId.store(ownerThreadId, std::memory_order_release);
    LOG_INFO("快捷键管理器已初始化");
}

void HotkeyManager::shutdown() {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        dispatchToHotkeyThread(ownerThreadId, "shutdown", [this]() { shutdown(); });
        return;
    }
    std::lock_guard lock(m_mutex);
    m_fullscreenSuspended.store(false, std::memory_order_release);
    KeyboardHook::instance().setFullscreenSuspended(false);
    for (const auto& [name, entry] : m_hotkeys) {
        if (entry.registered) {
            if (entry.useHook) {
                KeyboardHook::instance().unregisterHookHotkey(entry.id);
            } else if (m_hwnd) {
                UnregisterHotKey(m_hwnd, entry.id);
            }
        }
        LOG_DEBUG("注销快捷键: name={}, def={}", name, entry.def.toString());
    }
    m_hotkeys.clear();
    m_idToName.clear();
    m_hwnd = nullptr;
    m_ownerThreadId.store(0, std::memory_order_release);
    LOG_INFO("快捷键管理器已关闭, 所有快捷键已注销");
}

bool HotkeyManager::registerHotkey(const std::string& name, const HotkeyDef& def, HotkeyCallback callback) {
    TraceId::Scope scope;
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        return dispatchToHotkeyThread<bool>(
            ownerThreadId, "registerHotkey", false,
            [this, name, def, callback = std::move(callback)]() mutable {
                return registerHotkey(name, def, std::move(callback));
            });
    }
    std::lock_guard lock(m_mutex);

    // 0. 安全矩阵校验：严禁无修饰键的单字母、单数字、单标点符号注册为全局热键
    if (!def.isSafe()) {
        LOG_WARN("拒绝注册不安全的全局快捷键: name={}, def={}", name, def.toString());
        return false;
    }

    // 检查名称冲突
    if (m_hotkeys.contains(name)) {
        LOG_WARN("快捷键名称已存在, 将先注销旧绑定: name={}", name);
        if (m_hotkeys[name].registered) {
            if (m_hotkeys[name].useHook) {
                KeyboardHook::instance().unregisterHookHotkey(m_hotkeys[name].id);
            } else if (m_hwnd) {
                UnregisterHotKey(m_hwnd, m_hotkeys[name].id);
            }
        }
        m_idToName.erase(m_hotkeys[name].id);
        m_hotkeys.erase(name);
    }

    int id = generateId();
    if (def.virtualKey == 0) {
        m_hotkeys.emplace(name, HotkeyEntry{id, def, name, std::move(callback), false, true, false, "none", "", false, false});
        LOG_INFO("快捷键已禁用: name={}", name);
        return true;
    }

    if (m_fullscreenSuspended.load(std::memory_order_relaxed)) {
        m_hotkeys.emplace(name, HotkeyEntry{id, def, name, std::move(callback), false, true, false, "none", "", false, true});
        return true;
    }

    bool registered = false;
    bool useHook = false;

    // 1. 优先调用 Win32 原生 RegisterHotKey（修饰键组合优先）
    if (m_hwnd && IsWindow(m_hwnd)) {
        UINT mods = static_cast<UINT>(def.modifiers) | MOD_NOREPEAT;
        registered = (RegisterHotKey(m_hwnd, id, mods, def.virtualKey) != FALSE);
    }

    // 2. 自动降级与兜底：单按功能键（如 F1/F3 等系统抢占键）或 RegisterHotKey 失败时，由底层键盘钩子接管
    if (!registered && canFallbackToHook(def)) {
        KeyboardHook::instance().registerHookHotkey(id, def, name, callback);
        KeyboardHook::instance().install();
        registered = true;
        useHook = true;
        LOG_INFO("快捷键由底层键盘钩子引擎接管注册成功: name={}, def={}", name, def.toString());
    }

    HotkeyEntry entry{id, def, name, std::move(callback), registered, true, false, "none", "", useHook, false};
    m_hotkeys[name] = std::move(entry);

    if (!registered) {
        DWORD err = GetLastError();
        LOG_ERROR("注册快捷键失败: name={}, def={}, error={}", name, def.toString(), err);
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            LOG_ERROR("快捷键 {} 已被其他程序占用", def.toString());
        }
        return false;
    }

    m_idToName[id] = name;
    if (!useHook) {
        LOG_INFO("注册快捷键成功: name={}, def={}, id={}", name, def.toString(), id);
    }
    return true;
}

void HotkeyManager::unregisterHotkey(const std::string& name) {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        dispatchToHotkeyThread(ownerThreadId, "unregisterHotkey",
                               [this, name]() { unregisterHotkey(name); });
        return;
    }
    std::lock_guard lock(m_mutex);
    auto it = m_hotkeys.find(name);
    if (it == m_hotkeys.end()) {
        LOG_WARN("尝试注销不存在的快捷键: name={}", name);
        return;
    }
    if (it->second.registered) {
        if (it->second.useHook) {
            KeyboardHook::instance().unregisterHookHotkey(it->second.id);
        } else if (m_hwnd) {
            UnregisterHotKey(m_hwnd, it->second.id);
        }
    }
    m_idToName.erase(it->second.id);
    m_hotkeys.erase(it);
    LOG_INFO("已注销快捷键: name={}", name);
}

bool HotkeyManager::rebindHotkey(const std::string& name, const HotkeyDef& newDef) {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        return dispatchToHotkeyThread<bool>(
            ownerThreadId, "rebindHotkey", false,
            [this, name, newDef]() { return rebindHotkey(name, newDef); });
    }
    if (newDef.virtualKey == 0) return clearHotkey(name);
    if (!newDef.isSafe()) {
        LOG_WARN("重绑定拒绝不安全的快捷键: name={}, def={}", name, newDef.toString());
        return false;
    }

    std::lock_guard lock(m_mutex);
    auto it = m_hotkeys.find(name);
    if (it == m_hotkeys.end()) {
        LOG_WARN("尝试重绑定不存在的快捷键: name={}", name);
        return false;
    }

    const auto oldDef = it->second.def;
    const bool oldRegistered = it->second.registered;
    const bool oldUseHook = it->second.useHook;
    const bool armed = it->second.armed;

    if (oldRegistered) {
        if (oldUseHook) {
            KeyboardHook::instance().unregisterHookHotkey(it->second.id);
        } else if (m_hwnd) {
            UnregisterHotKey(m_hwnd, it->second.id);
        }
        m_idToName.erase(it->second.id);
        it->second.registered = false;
        it->second.useHook = false;
    }

    if (!armed) {
        it->second.def = newDef;
        it->second.registered = false;
        it->second.useHook = false;
        it->second.suspendedByFullscreen = false;
        LOG_INFO("快捷键重绑定成功（会话外不占用）: name={}, {} → {}",
                 name, oldDef.toString(), newDef.toString());
        return true;
    }

    if (m_fullscreenSuspended.load(std::memory_order_relaxed)) {
        it->second.def = newDef;
        it->second.registered = false;
        it->second.useHook = false;
        it->second.suspendedByFullscreen = true;
        LOG_INFO("快捷键重绑定成功: name={}, {} → {}", name, oldDef.toString(), newDef.toString());
        return true;
    }

    // 注册新的
    bool registered = false;
    bool useHook = false;
    if (m_hwnd && IsWindow(m_hwnd)) {
        UINT mods = static_cast<UINT>(newDef.modifiers) | MOD_NOREPEAT;
        registered = (RegisterHotKey(m_hwnd, it->second.id, mods, newDef.virtualKey) != FALSE);
    }

    if (!registered && canFallbackToHook(newDef)) {
        KeyboardHook::instance().registerHookHotkey(it->second.id, newDef, name, it->second.callback);
        KeyboardHook::instance().install();
        registered = true;
        useHook = true;
        LOG_INFO("快捷键由底层键盘钩子引擎接管重绑定成功: name={}, def={}", name, newDef.toString());
    }

    if (!registered) {
        LOG_ERROR("重绑定快捷键失败: name={}, newDef={}", name, newDef.toString());
        it->second.registered = false;
        it->second.useHook = false;
        // 回滚旧绑定
        if (oldRegistered && oldDef.virtualKey != 0) {
            if (oldUseHook) {
                KeyboardHook::instance().registerHookHotkey(it->second.id, oldDef, name, it->second.callback);
                KeyboardHook::instance().install();
                it->second.registered = true;
                it->second.useHook = true;
                m_idToName[it->second.id] = name;
            } else if (m_hwnd) {
                UINT oldMods = static_cast<UINT>(oldDef.modifiers) | MOD_NOREPEAT;
                if (RegisterHotKey(m_hwnd, it->second.id, oldMods, oldDef.virtualKey)) {
                    it->second.registered = true;
                    it->second.useHook = false;
                    m_idToName[it->second.id] = name;
                }
            }
        }
        return false;
    }

    LOG_INFO("快捷键重绑定成功: name={}, {} → {}", name, oldDef.toString(), newDef.toString());
    it->second.def = newDef;
    it->second.registered = true;
    it->second.useHook = useHook;
    it->second.suspendedByFullscreen = false;
    m_idToName[it->second.id] = name;
    return true;
}

bool HotkeyManager::clearHotkey(const std::string& name) {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        return dispatchToHotkeyThread<bool>(
            ownerThreadId, "clearHotkey", false,
            [this, name]() { return clearHotkey(name); });
    }
    std::lock_guard lock(m_mutex);
    auto it = m_hotkeys.find(name);
    if (it == m_hotkeys.end()) return false;
    if (it->second.registered) {
        if (it->second.useHook) {
            KeyboardHook::instance().unregisterHookHotkey(it->second.id);
        } else if (m_hwnd) {
            UnregisterHotKey(m_hwnd, it->second.id);
        }
    }
    m_idToName.erase(it->second.id);
    it->second.def = {};
    it->second.registered = false;
    it->second.useHook = false;
    it->second.suspendedByFullscreen = false;
    LOG_INFO("快捷键已禁用: name={}", name);
    return true;
}

bool HotkeyManager::setHotkeyArmed(const std::string& name, bool armed) {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        return dispatchToHotkeyThread<bool>(
            ownerThreadId, "setHotkeyArmed", false,
            [this, name, armed]() { return setHotkeyArmed(name, armed); });
    }
    std::lock_guard lock(m_mutex);
    auto it = m_hotkeys.find(name);
    if (it == m_hotkeys.end()) {
        LOG_WARN("尝试武装不存在的快捷键: name={}", name);
        return false;
    }

    it->second.armed = armed;
    if (it->second.def.virtualKey == 0) {
        it->second.registered = false;
        it->second.useHook = false;
        it->second.suspendedByFullscreen = false;
        return true;
    }

    if (!armed) {
        it->second.suspendedByFullscreen = false;
        if (it->second.registered) {
            if (it->second.useHook) {
                KeyboardHook::instance().unregisterHookHotkey(it->second.id);
            } else if (m_hwnd) {
                UnregisterHotKey(m_hwnd, it->second.id);
            }
            m_idToName.erase(it->second.id);
            it->second.registered = false;
            it->second.useHook = false;
            LOG_INFO("快捷键已卸下，交还给前台应用: name={}, def={}", name, it->second.def.toString());
        }
        return true;
    }

    if (m_fullscreenSuspended.load(std::memory_order_relaxed)) {
        it->second.suspendedByFullscreen = true;
        return true;
    }

    if (it->second.registered) return true;

    bool ok = false;
    bool useHook = false;
    if (m_hwnd && IsWindow(m_hwnd)) {
        UINT mods = static_cast<UINT>(it->second.def.modifiers) | MOD_NOREPEAT;
        ok = (RegisterHotKey(m_hwnd, it->second.id, mods, it->second.def.virtualKey) != FALSE);
    }
    if (!ok && canFallbackToHook(it->second.def)) {
        KeyboardHook::instance().registerHookHotkey(it->second.id, it->second.def, name, it->second.callback);
        KeyboardHook::instance().install();
        ok = true;
        useHook = true;
    }

    it->second.registered = ok;
    it->second.useHook = useHook;
    it->second.suspendedByFullscreen = false;
    if (ok) {
        m_idToName[it->second.id] = name;
        LOG_INFO("快捷键已重新占用: name={}, def={}", name, it->second.def.toString());
    } else {
        LOG_ERROR("重新占用快捷键失败: name={}, def={}, error={}",
                  name, it->second.def.toString(), GetLastError());
    }
    return ok;
}

bool HotkeyManager::isConflict(const HotkeyDef& def) const {
    std::lock_guard lock(m_mutex);
    for (const auto& [name, entry] : m_hotkeys) {
        if ((entry.registered || entry.suspendedByFullscreen) && def.virtualKey != 0 && entry.def == def) return true;
    }
    return false;
}

HotkeyConflictInfo HotkeyManager::checkConflict(const HotkeyDef& def, const std::string& currentName) const {
    const DWORD ownerThreadId = m_ownerThreadId.load(std::memory_order_acquire);
    if (isForeignHotkeyThread(ownerThreadId)) {
        return dispatchToHotkeyThread<HotkeyConflictInfo>(
            ownerThreadId, "checkConflict",
            HotkeyConflictInfo{true, "unavailable", "无法在窗口线程检查快捷键冲突"},
            [this, def, currentName]() { return checkConflict(def, currentName); });
    }
    if (def.virtualKey == 0) {
        return {false, "none", ""};
    }

    if (!def.isSafe()) {
        return {true, "unsafe", "字母与符号键请搭配 Ctrl、Alt 或 Win 使用"};
    }

    std::lock_guard lock(m_mutex);

    // 1. 优先检查内部插件与功能冲突
    for (const auto& [name, entry] : m_hotkeys) {
        if (name != currentName && entry.def.virtualKey != 0 && entry.def == def) {
            return {true, "internal", "与 [" + name + "] 冲突"};
        }
    }

    // 2. 针对单功能键（F1~F24、PrintScreen 等），底层键盘钩子具备 100% 抢占与兜底能力，无外部冲突
    if (isSingleFunctionKey(def)) {
        return {false, "none", ""};
    }

    // 3. 检查系统/第三方外部软件冲突 (通过试探性注册探针)
    if (m_hwnd && IsWindow(m_hwnd)) {
        UINT mods = static_cast<UINT>(def.modifiers) | MOD_NOREPEAT;
        constexpr int PROBE_HOTKEY_ID = 0xBEEF;
        if (!RegisterHotKey(m_hwnd, PROBE_HOTKEY_ID, mods, def.virtualKey)) {
            DWORD err = GetLastError();
            if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
                return {true, "external", "已被第三方应用程序或系统快捷键占用"};
            }
        } else {
            UnregisterHotKey(m_hwnd, PROBE_HOTKEY_ID);
        }
    }

    return {false, "none", ""};
}

void HotkeyManager::handleHotkeyMessage(WPARAM wParam) {
    if (m_paused.load(std::memory_order_relaxed) ||
        m_fullscreenSuspended.load(std::memory_order_relaxed)) {
        LOG_DEBUG("快捷键已暂停触发 (录制模式中)");
        return;
    }
    int id = static_cast<int>(wParam);
    std::string name;
    HotkeyCallback callback;

    {
        std::lock_guard lock(m_mutex);
        auto it = m_idToName.find(id);
        if (it == m_idToName.end()) return;

        name = it->second;
        auto entryIt = m_hotkeys.find(name);
        if (entryIt == m_hotkeys.end()) return;

        callback = entryIt->second.callback;
    }

    LOG_DEBUG("快捷键触发: name={}", name);

    if (callback) {
        TraceId::Scope scope;
        try {
            callback();
        } catch (const std::exception& e) {
            LOG_ERROR("快捷键回调异常: name={}, error={}", name, e.what());
        }
    }
}

std::vector<HotkeyEntry> HotkeyManager::getAllHotkeys() const {
    std::lock_guard lock(m_mutex);
    std::vector<HotkeyEntry> result;
    result.reserve(m_hotkeys.size());

    // 统计各快捷键出现的次数以判断内部冲突
    std::unordered_map<std::string, std::vector<std::string>> keyUsage;
    for (const auto& [name, entry] : m_hotkeys) {
        if (entry.def.virtualKey != 0) {
            keyUsage[entry.def.toString()].push_back(name);
        }
    }

    for (const auto& [name, entry] : m_hotkeys) {
        HotkeyEntry item = entry;
        if (item.suspendedByFullscreen) {
            item.registered = true;
        }
        if (item.def.virtualKey == 0) {
            item.conflict = false;
            item.conflictType = "none";
            item.conflictWith = "";
        } else {
            const auto& users = keyUsage[item.def.toString()];
            if (users.size() > 1) {
                // 内部冲突 (多个插件绑定了同一热键)
                item.conflict = true;
                item.conflictType = "internal";
                std::string others;
                for (const auto& u : users) {
                    if (u != name) {
                        if (!others.empty()) others += ", ";
                        others += u;
                    }
                }
                item.conflictWith = "与插件/功能 [" + others + "] 冲突";
            } else if (!item.suspendedByFullscreen &&
                       hotkeyLooksExternallyConflicted(
                           item.def.virtualKey != 0, item.armed, item.registered)) {
                // 外部冲突 (被其它第三方软件占用)
                item.conflict = true;
                item.conflictType = "external";
                item.conflictWith = "已被第三方软件或系统快捷键占用";
            } else {
                item.conflict = false;
                item.conflictType = "none";
                item.conflictWith = "";
            }
        }
        result.push_back(item);
    }
    return result;
}

int HotkeyManager::generateId() {
    int id = m_nextId.fetch_add(1);
    if (id == 0xBEEF) {
        id = m_nextId.fetch_add(1);
    }
    return id;
}

}  // namespace tools3000::core
