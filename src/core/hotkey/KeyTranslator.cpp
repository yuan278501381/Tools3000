#include "core/hotkey/KeyTranslator.h"
#include "core/utils/WinUtils.h"
#include <algorithm>
#include <unordered_map>

namespace tools3000::core {

namespace {

// 判断是否为动作控制功能键（Shift+该键视为动作组合快捷键，如 Shift+Tab, Shift+Delete）
inline bool isActionFunctionalKey(DWORD vk) {
    return (vk >= VK_F1 && vk <= VK_F24) ||
           vk == VK_TAB || vk == VK_RETURN || vk == VK_BACK ||
           vk == VK_DELETE || vk == VK_INSERT || vk == VK_HOME ||
           vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
           vk == VK_LEFT || vk == VK_UP || vk == VK_RIGHT || vk == VK_DOWN ||
           vk == VK_ESCAPE;
}

// 静态保底映射：美国标准键盘布局 (US QWERTY) 下的 Shift 上档标点与数字
const std::unordered_map<DWORD, std::string>& getUsShiftSymbolMap() {
    static const std::unordered_map<DWORD, std::string> s_map = {
        {'1', "!"},
        {'2', "@"},
        {'3', "#"},
        {'4', "$"},
        {'5', "%"},
        {'6', "^"},
        {'7', "&"},
        {'8', "*"},
        {'9', "("},
        {'0', ")"},
        {VK_OEM_MINUS, "_"},
        {VK_OEM_PLUS,  "+"},
        {VK_OEM_4,     "{"}, // [
        {VK_OEM_6,     "}"}, // ]
        {VK_OEM_5,     "|"}, // backslash
        {VK_OEM_1,     ":"}, // ;
        {VK_OEM_7,     "\""}, // '
        {VK_OEM_COMMA, "<"},
        {VK_OEM_PERIOD, ">"},
        {VK_OEM_2,     "?"}, // /
        {VK_OEM_3,     "~"}  // `
    };
    return s_map;
}

// 静态保底映射：无 Shift 下的标准 OEM 字符
const std::unordered_map<DWORD, std::string>& getUsNormalSymbolMap() {
    static const std::unordered_map<DWORD, std::string> s_map = {
        {VK_OEM_MINUS, "-"},
        {VK_OEM_PLUS,  "="},
        {VK_OEM_4,     "["},
        {VK_OEM_6,     "]"},
        {VK_OEM_5,     "\\"},
        {VK_OEM_1,     ";"},
        {VK_OEM_7,     "'"},
        {VK_OEM_COMMA, ","},
        {VK_OEM_PERIOD, "."},
        {VK_OEM_2,     "/"},
        {VK_OEM_3,     "`"}
    };
    return s_map;
}

} // anonymous namespace

bool KeyTranslator::isModifierKey(DWORD vkCode) {
    return vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL ||
           vkCode == VK_MENU    || vkCode == VK_LMENU    || vkCode == VK_RMENU ||
           vkCode == VK_SHIFT   || vkCode == VK_LSHIFT   || vkCode == VK_RSHIFT ||
           vkCode == VK_LWIN    || vkCode == VK_RWIN;
}

bool KeyTranslator::isFunctionalKey(DWORD vkCode) {
    return (vkCode >= VK_F1 && vkCode <= VK_F24) ||
           vkCode == VK_ESCAPE  || vkCode == VK_TAB      || vkCode == VK_RETURN ||
           vkCode == VK_BACK    || vkCode == VK_DELETE   || vkCode == VK_INSERT ||
           vkCode == VK_HOME    || vkCode == VK_END      || vkCode == VK_PRIOR  ||
           vkCode == VK_NEXT    || vkCode == VK_CAPITAL  || vkCode == VK_SNAPSHOT ||
           vkCode == VK_PAUSE   || vkCode == VK_SCROLL   || vkCode == VK_LEFT   ||
           vkCode == VK_UP      || vkCode == VK_RIGHT    || vkCode == VK_DOWN   ||
           vkCode == VK_SPACE   || vkCode == VK_NUMLOCK  || vkCode == VK_APPS ||
           vkCode == VK_SLEEP   ||
           (vkCode >= VK_BROWSER_BACK && vkCode <= VK_BROWSER_HOME) ||
           (vkCode >= VK_VOLUME_MUTE && vkCode <= VK_LAUNCH_APP2);
}

std::string KeyTranslator::translateKeyToUnicode(DWORD vkCode, UINT scanCode, bool hasShift, HKL hkl) {
    if (!hkl) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD tid = GetWindowThreadProcessId(fg, nullptr);
            hkl = GetKeyboardLayout(tid);
        }
        if (!hkl) {
            hkl = GetKeyboardLayout(0);
        }
    }

    if (scanCode == 0) {
        scanCode = MapVirtualKeyExW(vkCode, MAPVK_VK_TO_VSC, hkl);
    }

    // 构造临时键盘状态数组
    BYTE keyState[256] = {0};
    if (hasShift) {
        keyState[VK_SHIFT] = 0x80;
        keyState[VK_LSHIFT] = 0x80;
    }
    if (GetKeyState(VK_CAPITAL) & 0x0001) {
        keyState[VK_CAPITAL] = 0x01;
    }

    // 调用 ToUnicodeEx，关键参数 wFlags = 0x04：保证绝对不污染修改系统死键与输入法状态
    wchar_t buff[16] = {0};
    int ret = ToUnicodeEx(vkCode, scanCode, keyState, buff, 16, 0x04, hkl);
    if (ret > 0) {
        // 过滤不可见控制字符 (< 32 且非合法标点)
        if (buff[0] >= 32 && buff[0] != 127) {
            std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(std::wstring(buff, ret));
            if (!utf8.empty()) {
                return utf8;
            }
        }
    }

    // 动态转换失败或返回特殊字符时，采用静态保底映射表双保险
    if (hasShift) {
        const auto& shiftMap = getUsShiftSymbolMap();
        auto it = shiftMap.find(vkCode);
        if (it != shiftMap.end()) {
            return it->second;
        }
    } else {
        const auto& normMap = getUsNormalSymbolMap();
        auto it = normMap.find(vkCode);
        if (it != normMap.end()) {
            return it->second;
        }
    }

    return getStandardKeyName(vkCode, scanCode);
}

std::string KeyTranslator::getStandardKeyName(DWORD vkCode, UINT scanCode) {
    // 权威控制键与功能键标准命名（杜绝空格分词碎裂）
    switch (vkCode) {
        case VK_SPACE:    return "Space";
        case VK_RETURN:   return "Enter";
        case VK_ESCAPE:   return "Esc";
        case VK_TAB:      return "Tab";
        case VK_BACK:     return "Backspace";
        case VK_DELETE:   return "Delete";
        case VK_INSERT:   return "Insert";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "PageUp";     // 权威命名，不带空格
        case VK_NEXT:     return "PageDown";   // 权威命名，不带空格
        case VK_LEFT:     return "Left";
        case VK_RIGHT:    return "Right";
        case VK_UP:       return "Up";
        case VK_DOWN:     return "Down";
        case VK_CAPITAL:  return "CapsLock";   // 权威命名，不带空格
        case VK_SNAPSHOT: return "PrintScreen";
        case VK_SCROLL:   return "ScrollLock"; // 权威命名，不带空格
        case VK_PAUSE:    return "Pause";
        case VK_NUMLOCK:  return "NumLock";    // 权威命名，不带空格
        case VK_APPS:     return "Menu";
        case VK_SLEEP:    return "Sleep";

        // 媒体控制与浏览器导航扩展键（必须优先拦截，避免 MapVirtualKeyW 剥离 0xE0 扩展位误映射为字母 D/C/B/P/Q）
        case VK_VOLUME_MUTE:        return "VolumeMute";
        case VK_VOLUME_DOWN:        return "VolumeDown";
        case VK_VOLUME_UP:          return "VolumeUp";
        case VK_MEDIA_NEXT_TRACK:   return "MediaNext";
        case VK_MEDIA_PREV_TRACK:   return "MediaPrev";
        case VK_MEDIA_STOP:         return "MediaStop";
        case VK_MEDIA_PLAY_PAUSE:   return "PlayPause";
        case VK_BROWSER_BACK:       return "BrowserBack";
        case VK_BROWSER_FORWARD:    return "BrowserForward";
        case VK_BROWSER_REFRESH:    return "BrowserRefresh";
        case VK_BROWSER_STOP:       return "BrowserStop";
        case VK_BROWSER_SEARCH:     return "BrowserSearch";
        case VK_BROWSER_FAVORITES:  return "BrowserFavorites";
        case VK_BROWSER_HOME:       return "BrowserHome";
        case VK_LAUNCH_MAIL:        return "LaunchMail";
        case VK_LAUNCH_MEDIA_SELECT:return "LaunchMedia";
        case VK_LAUNCH_APP1:        return "LaunchApp1";
        case VK_LAUNCH_APP2:        return "LaunchApp2";

        // 小键盘运算键
        case VK_MULTIPLY: return "*";
        case VK_ADD:      return "+";
        case VK_SUBTRACT: return "-";
        case VK_DIVIDE:   return "/";
        case VK_DECIMAL:  return ".";

        // 小键盘数字键
        case VK_NUMPAD0:  return "Num0";
        case VK_NUMPAD1:  return "Num1";
        case VK_NUMPAD2:  return "Num2";
        case VK_NUMPAD3:  return "Num3";
        case VK_NUMPAD4:  return "Num4";
        case VK_NUMPAD5:  return "Num5";
        case VK_NUMPAD6:  return "Num6";
        case VK_NUMPAD7:  return "Num7";
        case VK_NUMPAD8:  return "Num8";
        case VK_NUMPAD9:  return "Num9";

        // 功能键 F1 ~ F24
        case VK_F1:  return "F1";  case VK_F2:  return "F2";  case VK_F3:  return "F3";  case VK_F4:  return "F4";
        case VK_F5:  return "F5";  case VK_F6:  return "F6";  case VK_F7:  return "F7";  case VK_F8:  return "F8";
        case VK_F9:  return "F9";  case VK_F10: return "F10"; case VK_F11: return "F11"; case VK_F12: return "F12";
        case VK_F13: return "F13"; case VK_F14: return "F14"; case VK_F15: return "F15"; case VK_F16: return "F16";
        case VK_F17: return "F17"; case VK_F18: return "F18"; case VK_F19: return "F19"; case VK_F20: return "F20";
        case VK_F21: return "F21"; case VK_F22: return "F22"; case VK_F23: return "F23"; case VK_F24: return "F24";

        default: break;
    }

    // 基础英文字母 A ~ Z
    if (vkCode >= 'A' && vkCode <= 'Z') {
        return std::string(1, static_cast<char>(vkCode));
    }
    // 基础主键盘数字 0 ~ 9
    if (vkCode >= '0' && vkCode <= '9') {
        return std::string(1, static_cast<char>(vkCode));
    }

    // 基础主键盘 OEM 标点符号
    const auto& normMap = getUsNormalSymbolMap();
    auto it = normMap.find(vkCode);
    if (it != normMap.end()) {
        return it->second;
    }

    // 其它特殊键通过驱动查询
    if (scanCode == 0) {
        scanCode = MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
    }
    switch (vkCode) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scanCode |= KF_EXTENDED;
            break;
    }

    char keyName[64] = {0};
    if (GetKeyNameTextA(scanCode << 16, keyName, sizeof(keyName)) > 0) {
        std::string name = keyName;
        // 清理由于驱动返回产生的多余空格，保证 token 独立完整性
        if (name == "Page Up")   return "PageUp";
        if (name == "Page Down") return "PageDown";
        if (name == "Caps Lock") return "CapsLock";
        if (name == "Num Lock")  return "NumLock";
        if (name == "Scroll Lock") return "ScrollLock";
        return name;
    }

    return "";
}

std::string KeyTranslator::formatCombo(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return "";
    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            result += " + ";
        }
        result += tokens[i];
    }
    return result;
}

KeycastKeyInfo KeyTranslator::resolveKeycastEvent(
    DWORD vkCode,
    UINT scanCode,
    bool hasCtrl,
    bool hasAlt,
    bool hasShift,
    bool hasWin,
    const std::string& filterMode,
    bool includeFunctionKeys,
    HKL hkl
) {
    KeycastKeyInfo info;

    if (isModifierKey(vkCode)) {
        // 单按修饰键自身由单独逻辑处理，此处不作为按键内容
        return info;
    }

    bool hasMainMod = hasCtrl || hasAlt || hasWin;
    bool isOnlyShift = hasShift && !hasMainMod;
    bool isActionFunc = isActionFunctionalKey(vkCode);
    bool isLetter = (vkCode >= 'A' && vkCode <= 'Z');
    bool isDigit = (vkCode >= '0' && vkCode <= '9');
    bool isOemPunctuation = (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_8) ||
                            (vkCode >= VK_OEM_PLUS && vkCode <= VK_OEM_3);

    std::string standardName = getStandardKeyName(vkCode, scanCode);

    // ─────────────────────────────────────────────────────────────────────────
    // 分支 1：存在主要命令修饰键 (Ctrl / Alt / Win)
    // ─────────────────────────────────────────────────────────────────────────
    if (hasMainMod) {
        info.isShortcut = true;
        info.shouldDisplay = true;

        if (hasCtrl)  info.tokens.push_back("Ctrl");
        if (hasWin)   info.tokens.push_back("Win");
        if (hasAlt)   info.tokens.push_back("Alt");
        if (hasShift) info.tokens.push_back("Shift");

        if (!standardName.empty()) {
            info.tokens.push_back(standardName);
        }

        info.rawKey = formatCombo(info.tokens);
        return info;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 分支 2：仅按住 Shift (isOnlyShift == true)
    // ─────────────────────────────────────────────────────────────────────────
    if (isOnlyShift) {
        // (A) 动作控制功能键（如 Shift + Tab, Shift + Enter, Shift + Delete, Shift + F10, Shift + 方向键）
        if (isActionFunc) {
            info.isShortcut = true;
            info.shouldDisplay = true;
            info.tokens.push_back("Shift");
            info.tokens.push_back(standardName);
            info.rawKey = formatCombo(info.tokens);
            return info;
        }

        const bool suppressShiftTyping =
            (filterMode == "smart_shortcuts" || filterMode == "shortcuts_only");

        // (B) 英文字母键 A ~ Z (产生大写输入)
        if (isLetter) {
            if (suppressShiftTyping) {
                // 智能打字过滤 / 仅快捷键：日常大写打字静默，绝不骚扰弹窗
                info.shouldDisplay = false;
            } else {
                // all_keys / with_single_modifiers 下【Shift 折叠】：直接输出大写字母单个键帽
                info.shouldDisplay = true;
                info.tokens.push_back(standardName);
                info.rawKey = standardName;
            }
            return info;
        }

        // (C) 数字与标点符号键（产生上档符号，例如 Shift+8 产生 *，Shift+= 产生 +，Shift+1 产生 !）
        if (isDigit || isOemPunctuation) {
            if (suppressShiftTyping) {
                // 【世界级体验】：标点符号输入属于纯打字！在智能快捷键与仅快捷键模式下静默，彻底消除误报！
                info.shouldDisplay = false;
            } else {
                // 【Shift 折叠核心】：将 Shift+8 翻译折叠为真实符号 [*]，输入什么显示什么！
                std::string actualSymbol = translateKeyToUnicode(vkCode, scanCode, true, hkl);
                if (actualSymbol.empty()) {
                    actualSymbol = standardName;
                }
                info.shouldDisplay = true;
                info.tokens.push_back(actualSymbol);
                info.rawKey = actualSymbol;
            }
            return info;
        }

        // 其它未分类按键：保底作为 Shift 组合
        if (suppressShiftTyping) {
            info.shouldDisplay = false;
        } else {
            info.shouldDisplay = true;
            info.tokens.push_back("Shift");
            info.tokens.push_back(standardName);
            info.rawKey = formatCombo(info.tokens);
        }
        return info;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 分支 3：无任何修饰键 (No Modifiers)
    // ─────────────────────────────────────────────────────────────────────────
    if (isFunctionalKey(vkCode)) {
        info.isShortcut = false;
        const bool isTypingCompanion = (vkCode == VK_SPACE || vkCode == VK_BACK ||
                                        vkCode == VK_CAPITAL || vkCode == VK_NUMLOCK ||
                                        vkCode == VK_SCROLL);
        if (filterMode == "all_keys" || filterMode == "with_single_modifiers") {
            info.shouldDisplay = true;
        } else if (isTypingCompanion) {
            // smart_shortcuts / shortcuts_only 下高频打字伴生键（空格选词、退格改字、锁定键）严格静默，防录屏拼音打字刷屏
            info.shouldDisplay = false;
        } else {
            // smart_shortcuts 模式下演示控制键（Enter/Esc/Tab/F1~F24/方向键等）由 includeFunctionKeys 控制
            info.shouldDisplay = (filterMode == "smart_shortcuts") && includeFunctionKeys;
        }
        info.tokens.push_back(standardName);
        info.rawKey = standardName;
        return info;
    }

    // 普通无修饰键打字输入（字母、数字、普通标点）
    if (filterMode == "all_keys" || filterMode == "with_single_modifiers") {
        info.shouldDisplay = true;
        std::string actualChar = translateKeyToUnicode(vkCode, scanCode, false, hkl);
        if (actualChar.empty()) {
            actualChar = standardName;
        }
        info.tokens.push_back(actualChar);
        info.rawKey = actualChar;
    } else {
        info.shouldDisplay = false; // smart_shortcuts 下纯打字静默
    }

    return info;
}

} // namespace tools3000::core
