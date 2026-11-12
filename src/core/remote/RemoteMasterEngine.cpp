#include "core/remote/RemoteMasterEngine.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/logger/Logger.h"
#include "core/events/EventBus.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/ipc/MessageBridge.h"

#include <imm.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>

#pragma comment(lib, "imm32.lib")

#ifndef IMC_SETOPENSTATUS
#define IMC_SETOPENSTATUS 0x0006
#endif

namespace tools3000::core {

namespace {

HKL findInstalledEnglishKeyboardLayout() noexcept {
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) return nullptr;
    std::vector<HKL> layouts(static_cast<size_t>(count), nullptr);
    const int fetched = GetKeyboardLayoutList(count, layouts.data());
    if (fetched <= 0) return nullptr;

    HKL fallbackEnglish = nullptr;
    for (int i = 0; i < fetched; ++i) {
        const HKL hkl = layouts[static_cast<size_t>(i)];
        const LANGID langId = LOWORD(HandleToUlong(hkl));
        if (langId == MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)) {
            return hkl;
        }
        if (!fallbackEnglish && PRIMARYLANGID(langId) == LANG_ENGLISH) {
            fallbackEnglish = hkl;
        }
    }
    return fallbackEnglish;
}

std::string toLowerAscii(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

std::wstring toLowerWide(std::wstring_view s) {
    std::wstring result;
    result.reserve(s.size());
    for (wchar_t c : s) {
        result.push_back(static_cast<wchar_t>(::towlower(c)));
    }
    return result;
}

std::string wstringToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

uint64_t packEmergencyShortcut(const ParsedEmergencyShortcut& s) noexcept {
    uint64_t packed = 0;
    if (s.valid) packed |= (1ULL << 0);
    if (s.ctrl)  packed |= (1ULL << 1);
    if (s.alt)   packed |= (1ULL << 2);
    if (s.shift) packed |= (1ULL << 3);
    if (s.win)   packed |= (1ULL << 4);
    packed |= (static_cast<uint64_t>(s.vk) << 8);
    return packed;
}

ParsedEmergencyShortcut unpackEmergencyShortcut(uint64_t packed) noexcept {
    ParsedEmergencyShortcut s;
    s.valid = (packed & (1ULL << 0)) != 0;
    s.ctrl  = (packed & (1ULL << 1)) != 0;
    s.alt   = (packed & (1ULL << 2)) != 0;
    s.shift = (packed & (1ULL << 3)) != 0;
    s.win   = (packed & (1ULL << 4)) != 0;
    s.vk    = static_cast<UINT>((packed >> 8) & 0xFFFFFFFFULL);
    return s;
}

} // namespace

nlohmann::json RemoteMasterSettings::toJson() const {
    return {
        {"enabled", enabled},
        {"hotkeyTunnelEnabled", hotkeyTunnelEnabled},
        {"emergencyFlushEnabled", emergencyFlushEnabled},
        {"doubleRightCtrlTrigger", doubleRightCtrlTrigger},
        {"imeSanitizerEnabled", imeSanitizerEnabled},
        {"emergencyShortcut", emergencyShortcut},
        {"targetProcesses", targetProcesses},
        {"targetClasses", targetClasses}
    };
}

RemoteMasterSettings RemoteMasterSettings::fromJson(const nlohmann::json& j) {
    RemoteMasterSettings s;
    if (j.contains("enabled") && j["enabled"].is_boolean()) s.enabled = j["enabled"].get<bool>();
    if (j.contains("hotkeyTunnelEnabled") && j["hotkeyTunnelEnabled"].is_boolean()) s.hotkeyTunnelEnabled = j["hotkeyTunnelEnabled"].get<bool>();
    if (j.contains("emergencyFlushEnabled") && j["emergencyFlushEnabled"].is_boolean()) s.emergencyFlushEnabled = j["emergencyFlushEnabled"].get<bool>();
    if (j.contains("doubleRightCtrlTrigger") && j["doubleRightCtrlTrigger"].is_boolean()) s.doubleRightCtrlTrigger = j["doubleRightCtrlTrigger"].get<bool>();
    if (j.contains("imeSanitizerEnabled") && j["imeSanitizerEnabled"].is_boolean()) s.imeSanitizerEnabled = j["imeSanitizerEnabled"].get<bool>();
    if (j.contains("emergencyShortcut") && j["emergencyShortcut"].is_string()) s.emergencyShortcut = j["emergencyShortcut"].get<std::string>();
    if (j.contains("targetProcesses") && j["targetProcesses"].is_array()) {
        s.targetProcesses.clear();
        for (const auto& item : j["targetProcesses"]) {
            if (item.is_string()) s.targetProcesses.push_back(item.get<std::string>());
        }
    }
    if (j.contains("targetClasses") && j["targetClasses"].is_array()) {
        s.targetClasses.clear();
        for (const auto& item : j["targetClasses"]) {
            if (item.is_string()) s.targetClasses.push_back(item.get<std::string>());
        }
    }
    return s;
}

RemoteMasterEngine::RemoteMasterEngine() {
    updateParsedEmergencyShortcut();
}

RemoteMasterEngine& RemoteMasterEngine::instance() {
    static RemoteMasterEngine inst;
    return inst;
}

bool RemoteMasterEngine::initialize() {
    {
        std::lock_guard lock(m_mutex);
        loadSettings();
    }

    // 注册 Windows 前台窗口变更通知钩子 (异步解耦，不阻塞用户消息)
    bool needHook = false;
    {
        std::lock_guard lock(m_mutex);
        needHook = (m_foregroundHook == nullptr);
    }
    if (needHook) {
        HWINEVENTHOOK hook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &RemoteMasterEngine::winEventProc,
            0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
        );
        if (hook) {
            HWINEVENTHOOK redundantHook = nullptr;
            {
                std::lock_guard lock(m_mutex);
                if (m_foregroundHook == nullptr) {
                    m_foregroundHook = hook;
                } else {
                    redundantHook = hook;
                }
            }
            if (redundantHook) {
                UnhookWinEvent(redundantHook);
            }
        } else {
            LOG_WARN("[RemoteMaster] Failed to install EVENT_SYSTEM_FOREGROUND hook, fallback to polling");
        }
    }

    // 立即侦测当前前台窗口状态
    const bool mockActive = m_mockActive.load(std::memory_order_acquire);
    HWND curFg = mockActive ? m_mockHwnd.load(std::memory_order_relaxed) : GetForegroundWindow();
    bool isRemote = false;
    bool isRawInput = false;
    std::string procName;
    if (curFg) {
        if (mockActive) {
            isRemote = m_mockIsRemote.load(std::memory_order_relaxed);
            std::lock_guard lock(m_mutex);
            procName = m_mockProcessName;
            isRawInput = isRawInputRemoteClient(procName);
        } else {
            isRemote = checkWindowIsRemote(curFg, &procName, &isRawInput);
        }
    }

    m_activeRemoteIsRawInput.store(isRemote && isRawInput, std::memory_order_relaxed);
    bool prevIsRemote = m_isRemoteForeground.exchange(isRemote, std::memory_order_relaxed);
    HWND prevHwnd = m_activeRemoteHwnd.exchange(isRemote ? curFg : nullptr, std::memory_order_relaxed);

    if (isRemote) {
        m_lastRemoteHwnd.store(curFg, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            m_activeRemoteProcess = procName;
            m_lastActiveRemoteHwnd = curFg;
        }
        if (!prevIsRemote || prevHwnd != curFg) {
            onRemoteForegroundGained(curFg);
        }
    } else {
        {
            std::lock_guard lock(m_mutex);
            m_activeRemoteProcess.clear();
        }
        if (prevIsRemote) {
            onRemoteForegroundLost(prevHwnd);
        }
    }

    LOG_INFO("[RemoteMaster] Initialized successfully");
    return true;
}

void RemoteMasterEngine::shutdown() {
    HWINEVENTHOOK hook = nullptr;
    HWND activeHwnd = nullptr;
    bool wasImeSanitized = false;
    bool wasRemoteFg = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        hook = m_foregroundHook;
        m_foregroundHook = nullptr;
        activeHwnd = m_activeRemoteHwnd.load(std::memory_order_relaxed);
        wasImeSanitized = m_imeSanitized.load(std::memory_order_relaxed);
        wasRemoteFg = m_isRemoteForeground.load(std::memory_order_relaxed);
        m_isRemoteForeground.store(false, std::memory_order_relaxed);
        m_activeRemoteIsRawInput.store(false, std::memory_order_relaxed);
        m_activeRemoteHwnd.store(nullptr, std::memory_order_relaxed);
        m_lastRemoteHwnd.store(nullptr, std::memory_order_relaxed);
        m_pendingProbeHwnd.store(nullptr, std::memory_order_relaxed);
        m_activeRemoteProcess.clear();
        m_cachedForegroundHwnd = nullptr;
        m_cachedIsRemote = false;
        m_cachedIsRawInput = false;
        m_cachedProcessName.clear();
    }

    if (hook) {
        UnhookWinEvent(hook);
    }
    if (wasImeSanitized || wasRemoteFg) {
        onRemoteForegroundLost(activeHwnd);
    } else {
        HotkeyManager::instance().setRemoteSuspended(false);
    }

    // 冷路径退场释放内存
    tools3000::core::WinUtils::trimWorkingSet();
    LOG_INFO("[RemoteMaster] Shutdown completed and working set trimmed");
}

RemoteMasterSettings RemoteMasterEngine::getSettings() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void RemoteMasterEngine::updateSettings(const RemoteMasterSettings& settings) {
    bool becameDisabled = false;
    RemoteMasterSettings persisted;
    {
        std::lock_guard lock(m_mutex);
        becameDisabled = (m_settings.enabled && !settings.enabled) ||
                         (m_settings.imeSanitizerEnabled && !settings.imeSanitizerEnabled);
        m_settings = settings;
        updateParsedEmergencyShortcut();
        m_cachedForegroundHwnd = nullptr;
        m_cachedIsRemote = false;
        m_cachedIsRawInput = false;
        m_cachedProcessName.clear();
        persisted = m_settings;
    }
    saveSettings(persisted);
    if (!persisted.enabled) {
        const HWND prevActive = m_activeRemoteHwnd.exchange(nullptr, std::memory_order_relaxed);
        const bool wasRemote = m_isRemoteForeground.exchange(false, std::memory_order_relaxed);
        m_activeRemoteIsRawInput.store(false, std::memory_order_relaxed);
        m_winKeyDown.store(false, std::memory_order_relaxed);
        if (wasRemote || m_imeSanitized.load(std::memory_order_relaxed)) {
            onRemoteForegroundLost(prevActive);
        } else {
            HotkeyManager::instance().setRemoteSuspended(false);
        }
    } else {
        if (becameDisabled && m_imeSanitized.load(std::memory_order_relaxed)) {
            onRemoteForegroundLost(m_activeRemoteHwnd.load(std::memory_order_relaxed));
        }
        // 热加载刷新当前前台窗口匹配状态（支持用户在设置中增删 targetClasses / targetProcesses 后无需来回切窗即刻生效）
        if (!m_mockActive.load(std::memory_order_acquire)) {
            if (HWND curFg = GetForegroundWindow()) {
                std::string procName;
                bool rawInput = false;
                const bool isRemote = checkWindowIsRemote(curFg, &procName, &rawInput);
                m_activeRemoteIsRawInput.store(isRemote && rawInput, std::memory_order_relaxed);
                const bool prevRemote = m_isRemoteForeground.exchange(isRemote, std::memory_order_relaxed);
                const HWND prevHwnd = m_activeRemoteHwnd.exchange(isRemote ? curFg : nullptr, std::memory_order_relaxed);
                if (isRemote) {
                    m_lastRemoteHwnd.store(curFg, std::memory_order_release);
                    {
                        std::lock_guard lock(m_mutex);
                        m_activeRemoteProcess = procName;
                        m_lastActiveRemoteHwnd = curFg;
                    }
                    if (!prevRemote || prevHwnd != curFg) {
                        onRemoteForegroundGained(curFg);
                    }
                } else {
                    {
                        std::lock_guard lock(m_mutex);
                        m_activeRemoteProcess.clear();
                    }
                    if (prevRemote) {
                        onRemoteForegroundLost(prevHwnd);
                    }
                }
            }
        }
    }
    LOG_INFO("[RemoteMaster] Settings updated");
}

void RemoteMasterEngine::resetDefaults() {
    RemoteMasterSettings persisted;
    {
        std::lock_guard lock(m_mutex);
        m_settings = RemoteMasterSettings{};
        updateParsedEmergencyShortcut();
        m_cachedForegroundHwnd = nullptr;
        m_cachedIsRemote = false;
        m_cachedIsRawInput = false;
        m_cachedProcessName.clear();
        m_lastActiveRemoteHwnd = nullptr;
        m_lastRemoteHwnd.store(nullptr, std::memory_order_relaxed);
        m_pendingProbeHwnd.store(nullptr, std::memory_order_relaxed);
        persisted = m_settings;
    }
    saveSettings(persisted);
    if (m_imeSanitized.load(std::memory_order_relaxed) ||
        m_isRemoteForeground.load(std::memory_order_relaxed)) {
        onRemoteForegroundLost(m_activeRemoteHwnd.load(std::memory_order_relaxed));
    } else {
        HotkeyManager::instance().setRemoteSuspended(false);
    }
    m_activeRemoteIsRawInput.store(false, std::memory_order_relaxed);
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] Settings reset to defaults");
}

void RemoteMasterEngine::loadSettings() {
    auto j = ConfigManager::instance().get<nlohmann::json>("/remote_boost", nlohmann::json::object());
    if (j.is_object() && !j.empty()) {
        m_settings = RemoteMasterSettings::fromJson(j);
    }
    if (ConfigManager::instance().has("/remote/targetClasses")) {
        const auto legacyClasses = ConfigManager::instance().get<nlohmann::json>("/remote/targetClasses", nlohmann::json::array());
        if (legacyClasses.is_array() && (!j.is_object() || !j.contains("targetClasses"))) {
            m_settings.targetClasses.clear();
            for (const auto& item : legacyClasses) {
                if (item.is_string()) m_settings.targetClasses.push_back(item.get<std::string>());
            }
        }
    }
    if (ConfigManager::instance().has("/plugins/remote_boost/enabled")) {
        m_settings.enabled = ConfigManager::instance().get<bool>("/plugins/remote_boost/enabled", m_settings.enabled);
    }
    updateParsedEmergencyShortcut();
}

void RemoteMasterEngine::saveSettings(const RemoteMasterSettings& snapshot) {
    ConfigManager::instance().mergePatch({
        {"remote_boost", snapshot.toJson()},
        {"plugins", {
            {"remote_boost", {{"enabled", snapshot.enabled}}},
            {"remote", {{"enabled", snapshot.enabled}}},
        }},
    }, "/remote_boost");
    MessageBridge::instance().pushEvent("remote:stateChanged", {{"enabled", snapshot.enabled}});
    MessageBridge::instance().pushEvent("plugins:changed", {{"id", "remote_boost"}, {"enabled", snapshot.enabled}});
}

void RemoteMasterEngine::syncAtomicSettingsLocked() noexcept {
    m_enabledAtomic.store(m_settings.enabled, std::memory_order_release);
    m_hotkeyTunnelAtomic.store(m_settings.hotkeyTunnelEnabled, std::memory_order_release);
    m_emergencyFlushAtomic.store(m_settings.emergencyFlushEnabled, std::memory_order_release);
    m_doubleRightCtrlAtomic.store(m_settings.doubleRightCtrlTrigger, std::memory_order_release);
    m_imeSanitizerAtomic.store(m_settings.imeSanitizerEnabled, std::memory_order_release);
    m_packedEmergencyShortcut.store(packEmergencyShortcut(m_parsedEmergencyShortcut),
                                    std::memory_order_release);
}

void RemoteMasterEngine::updateParsedEmergencyShortcut() {
    m_parsedEmergencyShortcut = parseEmergencyShortcut(m_settings.emergencyShortcut);
    syncAtomicSettingsLocked();
}

ParsedEmergencyShortcut RemoteMasterEngine::getParsedEmergencyShortcut() const {
    return unpackEmergencyShortcut(m_packedEmergencyShortcut.load(std::memory_order_acquire));
}

bool RemoteMasterEngine::isRemoteForeground() const {
    if (m_mockActive.load(std::memory_order_acquire)) {
        return m_mockIsRemote.load(std::memory_order_relaxed);
    }
    return m_isRemoteForeground.load(std::memory_order_relaxed);
}

HWND RemoteMasterEngine::getActiveRemoteHwnd() const {
    if (m_mockActive.load(std::memory_order_acquire)) {
        return m_mockHwnd.load(std::memory_order_relaxed);
    }
    return m_activeRemoteHwnd.load(std::memory_order_relaxed);
}

std::string RemoteMasterEngine::getActiveRemoteProcess() const {
    std::lock_guard lock(m_mutex);
    if (m_mockActive.load(std::memory_order_relaxed)) return m_mockProcessName;
    return m_activeRemoteProcess;
}

bool RemoteMasterEngine::isImeSanitized() const {
    return m_imeSanitized.load(std::memory_order_relaxed);
}

HKL RemoteMasterEngine::getSavedHkl() const {
    std::lock_guard lock(m_mutex);
    return m_savedHkl;
}

void RemoteMasterEngine::setMockForeground(HWND hwnd, bool isRemote, const std::string& processName) {
    {
        std::lock_guard lock(m_mutex);
        m_mockProcessName = processName;
    }
    m_mockHwnd.store(hwnd, std::memory_order_relaxed);
    m_mockIsRemote.store(isRemote, std::memory_order_relaxed);
    m_mockActive.store(true, std::memory_order_release);
    m_isRemoteForeground.store(isRemote, std::memory_order_relaxed);
    m_activeRemoteIsRawInput.store(isRemote && isRawInputRemoteClient(processName),
                                   std::memory_order_relaxed);
    m_activeRemoteHwnd.store(isRemote ? hwnd : nullptr, std::memory_order_relaxed);
    if (isRemote && hwnd) {
        m_lastRemoteHwnd.store(hwnd, std::memory_order_release);
    }
    if (isRemote) {
        onRemoteForegroundGained(hwnd);
    } else {
        onRemoteForegroundLost(hwnd);
    }
}

void RemoteMasterEngine::clearMockForeground() {
    const bool wasMockRemote = m_mockActive.load(std::memory_order_acquire) &&
                               m_mockIsRemote.load(std::memory_order_relaxed);
    const HWND prevMockHwnd = m_mockHwnd.load(std::memory_order_relaxed);
    if (wasMockRemote) {
        onRemoteForegroundLost(prevMockHwnd);
    } else {
        HotkeyManager::instance().setRemoteSuspended(false);
    }
    m_mockActive.store(false, std::memory_order_release);
    m_mockHwnd.store(nullptr, std::memory_order_relaxed);
    m_mockIsRemote.store(false, std::memory_order_relaxed);
    {
        std::lock_guard lock(m_mutex);
        m_mockProcessName.clear();
    }
    m_isRemoteForeground.store(false, std::memory_order_relaxed);
    m_activeRemoteIsRawInput.store(false, std::memory_order_relaxed);
    m_activeRemoteHwnd.store(nullptr, std::memory_order_relaxed);
    m_lastRemoteHwnd.store(nullptr, std::memory_order_relaxed);
}

bool RemoteMasterEngine::isRemoteProcessOrClass(
    const std::wstring& processName,
    const std::wstring& className,
    const std::vector<std::string>& targetProcesses,
    const std::vector<std::string>& targetClasses
) {
    if (processName.empty() && className.empty()) return false;

    // 提取纯文件名（去除路径）并小写化
    std::wstring pLower = toLowerWide(processName);
    size_t lastSlash = pLower.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        pLower = pLower.substr(lastSlash + 1);
    }

    std::wstring cLower = toLowerWide(className);

    // 匹配进程名
    for (const auto& tp : targetProcesses) {
        std::string tpTrimmed = tp;
        while (!tpTrimmed.empty() && std::isspace(static_cast<unsigned char>(tpTrimmed.front()))) tpTrimmed.erase(tpTrimmed.begin());
        while (!tpTrimmed.empty() && std::isspace(static_cast<unsigned char>(tpTrimmed.back()))) tpTrimmed.pop_back();
        if (tpTrimmed.empty()) continue;
        std::string tpLower = toLowerAscii(tpTrimmed);
        std::wstring tpWLower(tpLower.begin(), tpLower.end());
        if (pLower == tpWLower ||
            (tpWLower.find(L'.') == std::wstring::npos && pLower == tpWLower + L".exe") ||
            (pLower.find(L'.') == std::wstring::npos && pLower + L".exe" == tpWLower)) {
            return true;
        }
    }

    // 匹配类名 (支持子串包含与精确匹配)
    for (const auto& tc : targetClasses) {
        std::string tcTrimmed = tc;
        while (!tcTrimmed.empty() && std::isspace(static_cast<unsigned char>(tcTrimmed.front()))) tcTrimmed.erase(tcTrimmed.begin());
        while (!tcTrimmed.empty() && std::isspace(static_cast<unsigned char>(tcTrimmed.back()))) tcTrimmed.pop_back();
        if (tcTrimmed.empty()) continue;
        std::string tcLower = toLowerAscii(tcTrimmed);
        std::wstring tcWLower(tcLower.begin(), tcLower.end());
        if (cLower == tcWLower || cLower.find(tcWLower) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

bool RemoteMasterEngine::isRawInputRemoteClient(
    std::string_view processName,
    std::wstring_view className
) noexcept {
    std::string pLower = toLowerAscii(processName);
    const size_t slash = pLower.find_last_of("\\/");
    if (slash != std::string::npos) {
        pLower = pLower.substr(slash + 1);
    }

    static constexpr std::string_view kRawInputProcs[] = {
        "parsec.exe", "parsecd.exe",
        "moonlight.exe",
        "vmware.exe", "vmware-vmx.exe", "vmware-remotemks.exe",
        "virtualboxvm.exe",
        "gameviewer.exe",
        "uuremote.exe",
        "parsec", "moonlight", "vmware", "virtualboxvm", "gameviewer", "uuremote"
    };
    for (const auto& candidate : kRawInputProcs) {
        if (pLower == candidate) {
            return true;
        }
    }

    if (!className.empty()) {
        const std::wstring cLower = toLowerWide(className);
        if (cLower == L"sdl_app" || cLower.find(L"mksWindow") != std::wstring::npos ||
            cLower.find(L"mkswindow") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

std::vector<INPUT> RemoteMasterEngine::buildEmergencyFlushInputs() {
    struct ModDef {
        WORD vk;
        WORD scanCode;
        bool isExtended;
    };

    static const ModDef kModifiers[8] = {
        { VK_LCONTROL, 0x1D, false },
        { VK_RCONTROL, 0x1D, true  },
        { VK_LMENU,    0x38, false },
        { VK_RMENU,    0x38, true  },
        { VK_LSHIFT,   0x2A, false },
        { VK_RSHIFT,   0x36, false },
        { VK_LWIN,     0x5B, true  },
        { VK_RWIN,     0x5C, true  }
    };

    std::vector<INPUT> inputs;
    inputs.reserve(11);

    // 8 组修饰键 KEYUP
    for (const auto& mod : kModifiers) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = mod.vk;
        inp.ki.wScan = mod.scanCode;
        inp.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE | (mod.isExtended ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(inp);
    }

    // 3 组鼠标键释放
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        inputs.push_back(inp);
    }
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        inputs.push_back(inp);
    }
    {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        inputs.push_back(inp);
    }

    return inputs;
}

ParsedEmergencyShortcut RemoteMasterEngine::parseEmergencyShortcut(const std::string& str) {
    ParsedEmergencyShortcut res;
    if (str.empty()) return res;

    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= str.size()) {
        const size_t end = str.find('+', start);
        std::string tok = str.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
        if (!tok.empty()) tokens.push_back(std::move(tok));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (tokens.empty()) return res;

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        std::string token = toLowerAscii(tokens[i]);
        if (token == "ctrl" || token == "control") res.ctrl = true;
        else if (token == "alt") res.alt = true;
        else if (token == "shift") res.shift = true;
        else if (token == "win" || token == "meta") res.win = true;
        else return res;
    }

    std::string keyToken = toLowerAscii(tokens.back());
    static const std::unordered_map<std::string, UINT> kSpecialKeyMap = {
        {"backspace", VK_BACK}, {"back", VK_BACK},
        {"delete", VK_DELETE}, {"del", VK_DELETE},
        {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"space", VK_SPACE}, {"tab", VK_TAB},
        {"return", VK_RETURN}, {"enter", VK_RETURN},
        {"home", VK_HOME}, {"end", VK_END},
        {"pageup", VK_PRIOR}, {"pgup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"pgdn", VK_NEXT},
        {"insert", VK_INSERT}, {"ins", VK_INSERT},
        {"pause", VK_PAUSE}, {"break", VK_PAUSE},
        {"printscreen", VK_SNAPSHOT},
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"num0", VK_NUMPAD0}, {"num1", VK_NUMPAD1}, {"num2", VK_NUMPAD2}, {"num3", VK_NUMPAD3},
        {"num4", VK_NUMPAD4}, {"num5", VK_NUMPAD5}, {"num6", VK_NUMPAD6}, {"num7", VK_NUMPAD7},
        {"num8", VK_NUMPAD8}, {"num9", VK_NUMPAD9},
        {"numpad0", VK_NUMPAD0}, {"numpad1", VK_NUMPAD1}, {"numpad2", VK_NUMPAD2}, {"numpad3", VK_NUMPAD3},
        {"numpad4", VK_NUMPAD4}, {"numpad5", VK_NUMPAD5}, {"numpad6", VK_NUMPAD6}, {"numpad7", VK_NUMPAD7},
        {"numpad8", VK_NUMPAD8}, {"numpad9", VK_NUMPAD9},
        {"num+", VK_ADD}, {"numadd", VK_ADD},
        {"num-", VK_SUBTRACT}, {"numsubtract", VK_SUBTRACT},
        {"num*", VK_MULTIPLY}, {"nummultiply", VK_MULTIPLY},
        {"num/", VK_DIVIDE}, {"numdivide", VK_DIVIDE},
        {"num.", VK_DECIMAL}, {"numdecimal", VK_DECIMAL},
        {"numenter", VK_RETURN},
        {"f1", VK_F1}, {"f2", VK_F2}, {"f3", VK_F3}, {"f4", VK_F4},
        {"f5", VK_F5}, {"f6", VK_F6}, {"f7", VK_F7}, {"f8", VK_F8},
        {"f9", VK_F9}, {"f10", VK_F10}, {"f11", VK_F11}, {"f12", VK_F12},
        {"f13", VK_F13}, {"f14", VK_F14}, {"f15", VK_F15}, {"f16", VK_F16},
        {"f17", VK_F17}, {"f18", VK_F18}, {"f19", VK_F19}, {"f20", VK_F20},
        {"f21", VK_F21}, {"f22", VK_F22}, {"f23", VK_F23}, {"f24", VK_F24},
    };

    auto it = kSpecialKeyMap.find(keyToken);
    if (it != kSpecialKeyMap.end()) {
        res.vk = it->second;
        res.valid = true;
    } else if (keyToken.size() == 1 && std::isalnum(static_cast<unsigned char>(keyToken[0]))) {
        res.vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(keyToken[0])));
        res.valid = true;
    } else if (keyToken.starts_with("0x")) {
        try {
            unsigned long val = std::stoul(keyToken.substr(2), nullptr, 16);
            if (val > 0 && val <= 0xFF) {
                res.vk = static_cast<UINT>(val);
                res.valid = true;
            }
        } catch (...) {}
    }

    if (res.valid && !(res.ctrl || res.alt || res.shift || res.win)) {
        res.valid = false;
        res.vk = 0;
    }

    return res;
}

LPARAM RemoteMasterEngine::makeKeyLParam(
    DWORD scanCode,
    bool isExtended,
    bool isKeyUp,
    bool isAltDown,
    bool isRepeat
) {
    LPARAM lp = 1; // repeat count = 1
    lp |= static_cast<LPARAM>((scanCode & 0xFFULL) << 16);
    if (isExtended) lp |= static_cast<LPARAM>(1ULL << 24);
    if (isAltDown)  lp |= static_cast<LPARAM>(1ULL << 29);
    if (isRepeat)   lp |= static_cast<LPARAM>(1ULL << 30);
    if (isKeyUp) {
        lp |= static_cast<LPARAM>(1ULL << 30); // previous key state was down
        lp |= static_cast<LPARAM>(1ULL << 31); // transition state (being released)
    }
    return lp;
}

bool RemoteMasterEngine::checkWindowIsRemote(HWND hwnd, std::string* outProcessName, bool* outIsRawInput) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    // 快速命中缓存
    {
        std::lock_guard lock(m_mutex);
        if (hwnd == m_cachedForegroundHwnd) {
            if (outProcessName) *outProcessName = m_cachedProcessName;
            if (outIsRawInput) *outIsRawInput = m_cachedIsRawInput;
            return m_cachedIsRemote;
        }
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        std::lock_guard lock(m_mutex);
        m_cachedForegroundHwnd = hwnd;
        m_cachedIsRemote = false;
        m_cachedIsRawInput = false;
        m_cachedProcessName.clear();
        return false;
    }

    std::wstring processImage;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t pathBuf[1024] = {0};
        DWORD size = 1024;
        if (QueryFullProcessImageNameW(hProcess, 0, pathBuf, &size)) {
            processImage = pathBuf;
        }
        CloseHandle(hProcess);
    }

    wchar_t classBuf[256] = {0};
    GetClassNameW(hwnd, classBuf, 256);

    std::vector<std::string> targets;
    std::vector<std::string> classes;
    {
        std::lock_guard lock(m_mutex);
        targets = m_settings.targetProcesses;
        classes = m_settings.targetClasses;
    }

    bool matched = isRemoteProcessOrClass(processImage, classBuf, targets, classes);

    // 提取纯进程名用于显示（若受限无法读取进程名但类名命中，则回退为类名）
    std::string procUtf8;
    if (!processImage.empty()) {
        std::wstring pOnly = processImage;
        size_t slash = pOnly.find_last_of(L"\\/");
        if (slash != std::wstring::npos) pOnly = pOnly.substr(slash + 1);
        procUtf8 = wstringToUtf8(pOnly);
    } else if (matched && classBuf[0] != L'\0') {
        procUtf8 = wstringToUtf8(classBuf);
    }

    const bool rawInputClient = matched && isRawInputRemoteClient(procUtf8, classBuf);

    {
        std::lock_guard lock(m_mutex);
        m_cachedForegroundHwnd = hwnd;
        m_cachedIsRemote = matched;
        m_cachedIsRawInput = rawInputClient;
        m_cachedProcessName = procUtf8;
    }

    if (outProcessName) *outProcessName = procUtf8;
    if (outIsRawInput) *outIsRawInput = rawInputClient;
    return matched;
}

void RemoteMasterEngine::onRemoteForegroundGained(HWND hwnd) {
    if (!m_enabledAtomic.load(std::memory_order_acquire)) return;

    // 复位输入热键与连按状态机，确保进入远控前台时干净无状态污染
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    // 临时让出本地全局热键（如 F1/F3/Alt+Space/QuickLook），确保按键无截胡送达远端主机
    HotkeyManager::instance().setRemoteSuspended(true);

    if (!m_imeSanitizerAtomic.load(std::memory_order_acquire)) return;

    if (!m_imeSanitized.load(std::memory_order_relaxed)) {
        DWORD threadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
        HKL currentHkl = threadId ? GetKeyboardLayout(threadId) : nullptr;
        if (!currentHkl) {
            currentHkl = GetKeyboardLayout(0);
        }
        {
            std::lock_guard lock(m_mutex);
            m_savedHkl = currentHkl;
        }
    }

    // 优先切换至用户系统已安装的英文键盘布局，避免在纯中文系统上强行 LoadKeyboardLayoutW("00000409") 污染语言栏；
    // 若系统未安装独立英文键盘布局，则通过默认 IME 窗口非阻塞关闭中文转换状态。
    const HKL hklEng = findInstalledEnglishKeyboardLayout();
    bool closedImeViaControl = false;
    if (hwnd && IsWindow(hwnd)) {
        if (hklEng) {
            PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hklEng));
        }
        HWND imeWnd = ImmGetDefaultIMEWnd(hwnd);
        if (imeWnd && IsWindow(imeWnd)) {
            PostMessageW(imeWnd, WM_IME_CONTROL, IMC_SETOPENSTATUS, 0);
            closedImeViaControl = (hklEng == nullptr);
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_imeClosedViaControl = closedImeViaControl;
    }
    m_imeSanitized.store(true, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] IME Sanitizer: Switched input layout to pure US English (0409)");
}

void RemoteMasterEngine::onRemoteForegroundLost(HWND hwnd) {
    // 恢复本地全局热键占用
    HotkeyManager::instance().setRemoteSuspended(false);

    // 关键状态重置：离开远控前台时，必须无条件复位 Win 键与右 Ctrl 状态机
    // 若离开前台时 Win 键处于按下态，主动向原远控窗口派发 KEYUP，消除远端 Win 键卡死粘滞隐患
    if (m_winKeyDown.exchange(false, std::memory_order_relaxed)) {
        if (hwnd && IsWindow(hwnd) && !m_activeRemoteIsRawInput.load(std::memory_order_relaxed)) {
            LPARAM lp = makeKeyLParam(0x5B, true, true, false);
            PostMessageW(hwnd, WM_KEYUP, VK_LWIN, lp);
        }
    }
    m_activeRemoteIsRawInput.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    if (!m_imeSanitized.load(std::memory_order_relaxed)) return;

    HKL hklToRestore = nullptr;
    bool reopenIme = false;
    {
        std::lock_guard lock(m_mutex);
        hklToRestore = m_savedHkl;
        m_savedHkl = nullptr;
        reopenIme = m_imeClosedViaControl;
        m_imeClosedViaControl = false;
    }

    auto restoreLayoutForWindow = [&](HWND targetWnd) {
        if (!targetWnd || !IsWindow(targetWnd)) return;
        if (hklToRestore) {
            PostMessageW(targetWnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(hklToRestore));
        }
        if (reopenIme && hklToRestore &&
            PRIMARYLANGID(LOWORD(HandleToUlong(hklToRestore))) != LANG_ENGLISH) {
            HWND imeWnd = ImmGetDefaultIMEWnd(targetWnd);
            if (imeWnd && IsWindow(imeWnd)) {
                PostMessageW(imeWnd, WM_IME_CONTROL, IMC_SETOPENSTATUS, 1);
            }
        }
    };

    restoreLayoutForWindow(hwnd);
    const HWND currentFg = GetForegroundWindow();
    if (currentFg && currentFg != hwnd) {
        restoreLayoutForWindow(currentFg);
    }
    m_imeSanitized.store(false, std::memory_order_relaxed);
    LOG_INFO("[RemoteMaster] IME Sanitizer: Restored previous input layout");
}

void RemoteMasterEngine::postKeyToTarget(HWND fg, UINT msg, DWORD vk, DWORD scanCode, bool isExtended, bool isAltDown) {
    if (!fg) return;

    HWND target = fg;
    DWORD threadId = GetWindowThreadProcessId(fg, nullptr);
    if (threadId) {
        GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
        if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus && IsWindow(gti.hwndFocus)) {
            target = gti.hwndFocus;
        }
    }

    DWORD sc = scanCode;
    if (sc == 0) {
        sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }

    LPARAM lParam = makeKeyLParam(sc, isExtended, (msg == WM_KEYUP || msg == WM_SYSKEYUP), isAltDown);
    PostMessageW(target, msg, vk, lParam);
}

void RemoteMasterEngine::flushModifiers(HWND targetHwnd, bool manualRescue) {
    // 1. 无条件复位连击状态机，防止时间戳残留
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    if (m_mockActive.load(std::memory_order_acquire)) {
        if (!manualRescue && !m_mockIsRemote.load(std::memory_order_relaxed)) {
            LOG_DEBUG("[RemoteMaster] 无有效远控目标窗口，安全跳过急救冲刷");
            return;
        }
        // 单测 Mock 远控前台模式：完成状态机复位并发布通知，避免调用真实硬件 SendInput 干扰开发环境
        EventBus::instance().publish(ShowToastEvent{
            isUiLanguageChinese() ? L"已释放卡住的修饰键与鼠标按键"
                                  : L"Released stuck modifier keys and mouse buttons"
        });
        LOG_INFO("[RemoteMaster] [Mock] Emergency flush executed: state reset completed");
        return;
    }

    HWND target = nullptr;
    if (manualRescue) {
        // 手动急救（来自设置页或托盘右键菜单）：当前前台窗口必然为宿主 UI，不得因此直接 return；
        // 冲刷本机物理修饰键，同时对仍存活的活跃或最近远控窗口投递释放消息，不抢夺焦点。
        HWND candidate = targetHwnd ? targetHwnd : m_activeRemoteHwnd.load(std::memory_order_relaxed);
        if (!candidate || !IsWindow(candidate)) {
            candidate = m_lastRemoteHwnd.load(std::memory_order_acquire);
        }
        if (candidate && IsWindow(candidate)) {
            DWORD candPid = 0;
            GetWindowThreadProcessId(candidate, &candPid);
            if (candPid != 0 && candPid != GetCurrentProcessId()) {
                target = candidate;
            }
        }
    } else {
        // 自动/快捷键急救：仅当前台窗口本就是活跃远控会话窗口时才执行冲刷，绝不回退后台历史窗口、绝不调用 SetForegroundWindow 抢夺焦点
        HWND fg = GetForegroundWindow();
        HWND activeRemote = m_activeRemoteHwnd.load(std::memory_order_relaxed);
        bool fgIsRemote = fg && IsWindow(fg) &&
                          ((activeRemote && fg == activeRemote) || checkWindowIsRemote(fg));
        if (!fgIsRemote || (targetHwnd && targetHwnd != fg)) {
            LOG_DEBUG("[RemoteMaster] 无有效远控目标窗口，安全跳过急救冲刷");
            return;
        }
        target = fg;
    }

    const bool leftDown   = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rightDown  = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool middleDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

    // 注入中立 VK_F24 脉冲，彻底中和后续释放 Alt (VK_LMENU/VK_RMENU) 触发的 SC_KEYMENU 菜单模态死锁
    INPUT neutral[2]{};
    neutral[0].type = INPUT_KEYBOARD;
    neutral[0].ki.wVk = VK_F24;
    neutral[1].type = INPUT_KEYBOARD;
    neutral[1].ki.wVk = VK_F24;
    neutral[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, neutral, sizeof(INPUT));

    // 1. 原子化 SendInput 发送 8 组修饰键 KEYUP 及实际处于物理按下态的鼠标键释放脉冲（杜绝未按右键时误弹右键菜单）
    auto allInputs = buildEmergencyFlushInputs();
    std::vector<INPUT> activeInputs;
    activeInputs.reserve(allInputs.size());
    for (const auto& inp : allInputs) {
        if (inp.type == INPUT_MOUSE) {
            if ((inp.mi.dwFlags & MOUSEEVENTF_LEFTUP) != 0 && !leftDown) continue;
            if ((inp.mi.dwFlags & MOUSEEVENTF_RIGHTUP) != 0 && !rightDown) continue;
            if ((inp.mi.dwFlags & MOUSEEVENTF_MIDDLEUP) != 0 && !middleDown) continue;
        }
        activeInputs.push_back(inp);
    }
    if (!activeInputs.empty()) {
        SendInput(static_cast<UINT>(activeInputs.size()), activeInputs.data(), sizeof(INPUT));
    }

    // 2. 向目标远控画布精准直投 8 组修饰键的 WM_KEYUP / WM_SYSKEYUP
    if (target && IsWindow(target)) {
        HWND canvas = target;
        DWORD threadId = GetWindowThreadProcessId(target, nullptr);
        if (threadId) {
            GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
            if (GetGUIThreadInfo(threadId, &gti) && gti.hwndFocus && IsWindow(gti.hwndFocus)) {
                canvas = gti.hwndFocus;
            }
        }

        static const struct {
            UINT msg;
            DWORD vk;
            DWORD scan;
            bool ext;
            bool alt;
        } kDirectMsgs[8] = {
            { WM_KEYUP,    VK_LCONTROL, 0x1D, false, false },
            { WM_KEYUP,    VK_RCONTROL, 0x1D, true,  false },
            { WM_SYSKEYUP, VK_LMENU,    0x38, false, true  },
            { WM_SYSKEYUP, VK_RMENU,    0x38, true,  true  },
            { WM_KEYUP,    VK_LSHIFT,   0x2A, false, false },
            { WM_KEYUP,    VK_RSHIFT,   0x36, false, false },
            { WM_KEYUP,    VK_LWIN,     0x5B, true,  false },
            { WM_KEYUP,    VK_RWIN,     0x5C, true,  false }
        };

        for (const auto& dm : kDirectMsgs) {
            LPARAM lp = makeKeyLParam(dm.scan, dm.ext, true, dm.alt);
            PostMessageW(canvas, dm.msg, dm.vk, lp);
            if (canvas != target) {
                PostMessageW(target, dm.msg, dm.vk, lp);
            }
        }

        // 仅当鼠标按键真实处于物理按下态时才投递弹起消息，彻底杜绝无故触发上下文右键菜单
        if (leftDown || rightDown || middleDown) {
            POINT pt{0, 0};
            GetCursorPos(&pt);
            POINT ptCanvas = pt;
            ScreenToClient(canvas, &ptCanvas);
            LPARAM lpCanvas = MAKELPARAM(ptCanvas.x, ptCanvas.y);

            if (leftDown)   PostMessageW(canvas, WM_LBUTTONUP, 0, lpCanvas);
            if (rightDown)  PostMessageW(canvas, WM_RBUTTONUP, 0, lpCanvas);
            if (middleDown) PostMessageW(canvas, WM_MBUTTONUP, 0, lpCanvas);
            if (canvas != target) {
                POINT ptTarget = pt;
                ScreenToClient(target, &ptTarget);
                LPARAM lpTarget = MAKELPARAM(ptTarget.x, ptTarget.y);
                if (leftDown)   PostMessageW(target, WM_LBUTTONUP, 0, lpTarget);
                if (rightDown)  PostMessageW(target, WM_RBUTTONUP, 0, lpTarget);
                if (middleDown) PostMessageW(target, WM_MBUTTONUP, 0, lpTarget);
            }
        }
    }

    // 3. 复位状态机
    m_winKeyDown.store(false, std::memory_order_relaxed);
    m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
    m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);

    // 4. 触发全局轻量通知反馈
    EventBus::instance().publish(ShowToastEvent{
        isUiLanguageChinese() ? L"已释放卡住的修饰键与鼠标按键"
                              : L"Released stuck modifier keys and mouse buttons"
    });
    LOG_INFO("[RemoteMaster] Emergency flush executed: 8 modifier keys and 3 mouse buttons released");
}

bool RemoteMasterEngine::onLowLevelKeyboardEvent(const KBDLLHOOKSTRUCT& data, WPARAM wParam) {
    if (!m_enabledAtomic.load(std::memory_order_acquire)) return false;

    DWORD vk = data.vkCode;
    bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool mockActive = m_mockActive.load(std::memory_order_acquire);

    // ── 引擎 2: 远程修饰键卡死急救 (快捷热键触发，100% 无锁) ──────────────
    if (m_emergencyFlushAtomic.load(std::memory_order_relaxed)) {
        bool inRemote = mockActive ? m_mockIsRemote.load(std::memory_order_relaxed) :
                        (m_isRemoteForeground.load(std::memory_order_relaxed) ||
                        (m_activeRemoteHwnd.load(std::memory_order_relaxed) != nullptr));
        if (!inRemote) {
            m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
        } else {
            // 双击右 Ctrl 触发急救 (400ms 内连按两次释放，兼容不同键盘驱动模式)
            if (m_doubleRightCtrlAtomic.load(std::memory_order_relaxed)) {
                bool isRightCtrl = (vk == VK_RCONTROL) ||
                                   (vk == VK_CONTROL && ((data.flags & LLKHF_EXTENDED) != 0));
                if (isRightCtrl) {
                    if (isKeyUp) {
                        ULONGLONG now = GetTickCount64();
                        ULONGLONG last = m_lastRightCtrlUpTime.load(std::memory_order_relaxed);
                        bool otherKey = m_otherKeyPressedSinceRightCtrl.load(std::memory_order_relaxed);

                        if (last > 0 && (now - last) <= 400 && !otherKey) {
                            // 异步任务投递至主线程下一轮消息循环，绝不在 WH_KEYBOARD_LL 栈帧内同步直调业务闭包与 SendInput
                            auto target = m_activeRemoteHwnd.load(std::memory_order_relaxed);
                            if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                                tools3000::core::MainThreadDispatcher::instance().postDeferred([this, target]() {
                                    flushModifiers(target, false);
                                });
                            } else if (mockActive) {
                                flushModifiers(target, false);
                            }
                            m_lastRightCtrlUpTime.store(0, std::memory_order_relaxed);
                            m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
                            return true; // 拦截并消费第二次弹起的 Right Ctrl
                        } else {
                            m_lastRightCtrlUpTime.store(now, std::memory_order_relaxed);
                            m_otherKeyPressedSinceRightCtrl.store(false, std::memory_order_relaxed);
                        }
                    }
                } else if (isKeyDown) {
                    m_otherKeyPressedSinceRightCtrl.store(true, std::memory_order_relaxed);
                }
            }

            // 专用急救快捷键 (单次 64 位原子读取，绝不在 1000Hz 钩子中加锁)
            const ParsedEmergencyShortcut targetShortcut =
                unpackEmergencyShortcut(m_packedEmergencyShortcut.load(std::memory_order_acquire));
            if (targetShortcut.valid && isKeyDown && vk == targetShortcut.vk) {
                bool hasCtrl = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
                               (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL);
                bool hasAlt = ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                              ((data.flags & LLKHF_ALTDOWN) != 0) ||
                              (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU);
                bool hasShift = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
                                (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT);
                bool hasWin = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                              ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0) ||
                              m_winKeyDown.load(std::memory_order_relaxed) ||
                              (vk == VK_LWIN || vk == VK_RWIN);

                if (hasCtrl == targetShortcut.ctrl &&
                    hasAlt == targetShortcut.alt &&
                    hasShift == targetShortcut.shift &&
                    hasWin == targetShortcut.win) {
                    auto target = m_activeRemoteHwnd.load(std::memory_order_relaxed);
                    if (tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                        tools3000::core::MainThreadDispatcher::instance().postDeferred([this, target]() {
                            flushModifiers(target, false);
                        });
                    } else if (mockActive) {
                        flushModifiers(target, false);
                    }
                    return true; // 拦截并消费急救按键
                }
            }
        }
    }

    // ── 引擎 1: 沉浸式系统热键直通 (Immersive Remote Hotkey Tunnel) ────────
    if (!m_hotkeyTunnelAtomic.load(std::memory_order_relaxed)) return false;

    // 真实运行环境下无条件放行触控板驱动合成手势 (如精密触控板四指横滑切换桌面) 与软件注入按键
    if (!mockActive && (data.flags & LLKHF_INJECTED) != 0) {
        return false;
    }

    bool isRemote = mockActive ? m_mockIsRemote.load(std::memory_order_relaxed)
                               : m_isRemoteForeground.load(std::memory_order_relaxed);
    if (!isRemote) {
        m_winKeyDown.store(false, std::memory_order_relaxed);
        return false;
    }

    HWND fg = mockActive ? m_mockHwnd.load(std::memory_order_relaxed)
                         : m_activeRemoteHwnd.load(std::memory_order_relaxed);
    if (!mockActive) {
        HWND actualFg = GetForegroundWindow();
        if (!actualFg) {
            m_isRemoteForeground.store(false, std::memory_order_relaxed);
            m_winKeyDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (actualFg != fg) {
            // 严禁在 WH_KEYBOARD_LL 内同步加锁执行 checkWindowIsRemote / OpenProcess；
            // 先立即保守复位远控前台标志，再异步投递到主线程下一轮消息循环探测新窗口并更新原子缓存。
            m_isRemoteForeground.store(false, std::memory_order_relaxed);
            m_winKeyDown.store(false, std::memory_order_relaxed);
            if (m_pendingProbeHwnd.exchange(actualFg, std::memory_order_acq_rel) != actualFg &&
                tools3000::core::MainThreadDispatcher::instance().isInitialized()) {
                tools3000::core::MainThreadDispatcher::instance().postDeferred([this, actualFg]() {
                    HWND expected = actualFg;
                    m_pendingProbeHwnd.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
                    if (m_mockActive.load(std::memory_order_acquire) ||
                        !m_enabledAtomic.load(std::memory_order_acquire)) {
                        return;
                    }
                    const HWND curFg = GetForegroundWindow();
                    if (!curFg) return;
                    std::string procName;
                    bool rawInput = false;
                    const bool remote = checkWindowIsRemote(curFg, &procName, &rawInput);
                    m_activeRemoteIsRawInput.store(remote && rawInput, std::memory_order_relaxed);
                    const bool prevRemote = m_isRemoteForeground.exchange(remote, std::memory_order_relaxed);
                    const HWND prevHwnd = m_activeRemoteHwnd.exchange(remote ? curFg : nullptr, std::memory_order_relaxed);
                    if (remote) {
                        m_lastRemoteHwnd.store(curFg, std::memory_order_release);
                        {
                            std::lock_guard lock(m_mutex);
                            m_activeRemoteProcess = procName;
                            m_lastActiveRemoteHwnd = curFg;
                        }
                        if (!prevRemote || prevHwnd != curFg) {
                            onRemoteForegroundGained(curFg);
                        }
                    } else {
                        {
                            std::lock_guard lock(m_mutex);
                            m_activeRemoteProcess.clear();
                        }
                        if (prevRemote) {
                            onRemoteForegroundLost(prevHwnd);
                        }
                    }
                });
            }
            return false;
        }
        fg = actualFg;
    } else if (!fg) {
        fg = GetForegroundWindow();
    }
    if (!fg) return false;

    // Parsec / Moonlight / VMware / VirtualBox 等客户端使用底层 Raw Input 或自有 WH_KEYBOARD_LL 钩子捕获系统组合键；
    // 若在此处拦截钩子链并改发窗口队列 PostMessageW，会导致其原生钩子与 Raw Input 被截断，引发 Alt+Tab / Win 死键。
    if (m_activeRemoteIsRawInput.load(std::memory_order_relaxed)) {
        m_winKeyDown.store(false, std::memory_order_relaxed);
        return false;
    }

    // 1. Win 键单独按下与弹起 (VK_LWIN / VK_RWIN)
    if (vk == VK_LWIN || vk == VK_RWIN) {
        if (isKeyDown) {
            m_winKeyDown.store(true, std::memory_order_relaxed);
            postKeyToTarget(fg, WM_KEYDOWN, vk, data.scanCode, true);
            return true; // 拦截，防止弹出本地开始菜单
        } else if (isKeyUp) {
            m_winKeyDown.store(false, std::memory_order_relaxed);
            postKeyToTarget(fg, WM_KEYUP, vk, data.scanCode, true);
            return true; // 拦截，防止弹出本地开始菜单
        }
    }

    // 2. Win 键按住期间的组合快捷键 (Win+R, Win+E, Win+D, Win+X, Win+A, Win+S, Win+1..9 等)
    bool physicalWinDown = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                           ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    if (!mockActive && m_winKeyDown.load(std::memory_order_relaxed) && !physicalWinDown) {
        // 物理 Win 键已松开（如焦点切换或远控软件自身钩子吞噬了 KEYUP），自动自愈复位粘滞状态
        m_winKeyDown.store(false, std::memory_order_relaxed);
    }
    bool hasWin = m_winKeyDown.load(std::memory_order_relaxed) || physicalWinDown;

    if (hasWin) {
        // 放行 Windows 本地虚拟桌面切换快捷键 (Win + Ctrl + Left / Right / D / F4)，配合四指横滑无缝切回本地桌面
        if (!mockActive && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) &&
            (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_F4 || vk == 'D')) {
            if (m_winKeyDown.exchange(false, std::memory_order_relaxed)) {
                postKeyToTarget(fg, WM_KEYUP, VK_LWIN, 0x5B, true);
            }
            return false;
        }
        if (isKeyDown) {
            postKeyToTarget(fg, WM_KEYDOWN, vk, data.scanCode, (data.flags & LLKHF_EXTENDED) != 0);
            return true; // 拦截本地运行、资源管理器、显示桌面等弹出
        } else if (isKeyUp) {
            postKeyToTarget(fg, WM_KEYUP, vk, data.scanCode, (data.flags & LLKHF_EXTENDED) != 0);
            return true;
        }
    }

    // 3. Alt + Tab 直通远端
    if (vk == VK_TAB) {
        bool hasAlt = ((data.flags & LLKHF_ALTDOWN) != 0) ||
                      ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) ||
                      (wParam == WM_SYSKEYDOWN) || (wParam == WM_SYSKEYUP);
        if (hasAlt) {
            if (isKeyDown) {
                postKeyToTarget(fg, WM_SYSKEYDOWN, VK_TAB, data.scanCode, false, true);
                return true; // 拦截本地任务切换器
            } else if (isKeyUp) {
                postKeyToTarget(fg, WM_SYSKEYUP, VK_TAB, data.scanCode, false, true);
                return true;
            }
        }
    }

    // 4. Ctrl + Shift + Esc / Ctrl + Esc 直通远端
    if (vk == VK_ESCAPE) {
        bool hasCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool hasShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool hasAlt = ((data.flags & LLKHF_ALTDOWN) != 0) || ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

        // Ctrl + Shift + Esc: 远端任务管理器 (拦截本地任务管理器)
        if (hasCtrl && hasShift) {
            if (isKeyDown) {
                postKeyToTarget(fg, WM_KEYDOWN, VK_ESCAPE, data.scanCode, false);
                return true;
            } else if (isKeyUp) {
                postKeyToTarget(fg, WM_KEYUP, VK_ESCAPE, data.scanCode, false);
                return true;
            }
        }
        // Ctrl + Esc: 远端开始菜单 (拦截本地开始菜单弹出)
        if (hasCtrl && !hasShift && !hasAlt) {
            if (isKeyDown) {
                postKeyToTarget(fg, WM_KEYDOWN, VK_ESCAPE, data.scanCode, false);
                return true;
            } else if (isKeyUp) {
                postKeyToTarget(fg, WM_KEYUP, VK_ESCAPE, data.scanCode, false);
                return true;
            }
        }
    }

    return false;
}

void CALLBACK RemoteMasterEngine::winEventProc(
    HWINEVENTHOOK /*hWinEventHook*/,
    DWORD event,
    HWND hwnd,
    LONG /*idObject*/,
    LONG /*idChild*/,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/
) {
    if (event != EVENT_SYSTEM_FOREGROUND || !hwnd) return;

    auto& engine = RemoteMasterEngine::instance();
    if (!engine.m_enabledAtomic.load(std::memory_order_acquire)) return;

    std::string procName;
    bool rawInput = false;
    bool isRemote = engine.checkWindowIsRemote(hwnd, &procName, &rawInput);

    engine.m_activeRemoteIsRawInput.store(isRemote && rawInput, std::memory_order_relaxed);
    bool prevIsRemote = engine.m_isRemoteForeground.exchange(isRemote, std::memory_order_relaxed);
    HWND prevHwnd = engine.m_activeRemoteHwnd.exchange(isRemote ? hwnd : nullptr, std::memory_order_relaxed);

    if (isRemote) {
        engine.m_lastRemoteHwnd.store(hwnd, std::memory_order_release);
        {
            std::lock_guard lock(engine.m_mutex);
            engine.m_activeRemoteProcess = procName;
            engine.m_lastActiveRemoteHwnd = hwnd;
        }
        if (!prevIsRemote || prevHwnd != hwnd) {
            engine.onRemoteForegroundGained(hwnd);
        }
    } else {
        {
            std::lock_guard lock(engine.m_mutex);
            engine.m_activeRemoteProcess.clear();
        }
        if (prevIsRemote) {
            engine.onRemoteForegroundLost(prevHwnd);
        }
    }
}

} // namespace tools3000::core
