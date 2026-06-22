#ifndef TOOLS3000_CORE_HOTKEY_KEYTRANSLATOR_H
#define TOOLS3000_CORE_HOTKEY_KEYTRANSLATOR_H

#include "core/utils/Export.h"

#include <windows.h>
#include <string>
#include <vector>

namespace tools3000::core {

/**
 * @brief 按键屏幕回显结构化按键信息
 */
struct KeycastKeyInfo {
    std::vector<std::string> tokens; ///< 拆分后的独立键帽单元（例如 ["Ctrl", "+"] 或 ["*"] 或 ["PageUp"]）
    std::string rawKey;              ///< 唯一样本标识，用于连击与去重判定（例如 "Ctrl++" 或 "*" 或 "+"）
    bool isShortcut = false;         ///< 是否属于组合动作快捷键
    bool shouldDisplay = false;      ///< 在当前过滤模式下是否应当展示
};

/**
 * @brief 工业级按键字符翻译与回显意图裁决引擎
 */
class TOOLS3000CORE_API KeyTranslator {
public:
    /**
     * @brief 安全将虚拟键码转换为实际输出的 Unicode 文本字符
     * @note 内部强制使用 wFlags=0x04，绝不修改系统死键（Dead Key）状态机，对多语言输入法零污染
     */
    static std::string translateKeyToUnicode(DWORD vkCode, UINT scanCode, bool hasShift, HKL hkl = nullptr);

    /**
     * @brief 权威按键意图裁决与 Shift 折叠算法
     * @param vkCode 虚拟键码
     * @param scanCode 硬件扫描码
     * @param hasCtrl 是否按下 Ctrl
     * @param hasAlt 是否按下 Alt
     * @param hasShift 是否按下 Shift
     * @param hasWin 是否按下 Win
     * @param filterMode 过滤模式 ("smart_shortcuts", "with_single_modifiers", "all_keys")
     * @param includeFunctionKeys 是否包含单按功能键 (F1-F24, Esc, Tab, Enter等)
     * @param hkl 键盘布局句柄 (若为 nullptr 则自动嗅探前台激活窗口)
     */
    static KeycastKeyInfo resolveKeycastEvent(
        DWORD vkCode,
        UINT scanCode,
        bool hasCtrl,
        bool hasAlt,
        bool hasShift,
        bool hasWin,
        const std::string& filterMode,
        bool includeFunctionKeys,
        HKL hkl = nullptr
    );

    /**
     * @brief 获取权威标准键名（杜绝空格分词碎裂与加号吞噬）
     */
    static std::string getStandardKeyName(DWORD vkCode, UINT scanCode = 0);

    /**
     * @brief 格式化 Token 列表为标准回显字符串 (以 " + " 隔开)
     */
    static std::string formatCombo(const std::vector<std::string>& tokens);

    /**
     * @brief 判断是否为修饰键
     */
    static bool isModifierKey(DWORD vkCode);

    /**
     * @brief 判断是否为功能键/动作控制键
     */
    static bool isFunctionalKey(DWORD vkCode);
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_HOTKEY_KEYTRANSLATOR_H
