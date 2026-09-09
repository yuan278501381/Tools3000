#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinGroupManager.h — 贴图分组元数据、自定义名称/宝石色调色板与过滤契约单一事实源 (SSOT)
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINGROUPMANAGER_H
#define TOOLS3000_CAPTURE_PINGROUPMANAGER_H

#include <d2d1.h>
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace tools3000::capture {

namespace PinGroupConstants {
    /// 分组编号契约：0 表示未分组，支持动态扩展至 1..32 组
    inline constexpr int kUngroupedId = 0;
    inline constexpr int kMinGroupId = 1;
    inline constexpr int kDefaultQuickGroupCount = 6;
    inline constexpr int kMaxShortcutGroupId = 9;
    inline constexpr int kMaxGroupId = 32;
    inline constexpr int kPaletteColorCount = 12;
    inline constexpr size_t kMaxCustomGroupNameChars = 24;

    /// 视图过滤常量：-1 表示显示全部贴图，0 表示仅显示未分组，1..32 表示仅显示对应分组
    inline constexpr int kFilterAllGroups = -1;
} // namespace PinGroupConstants

struct PinGroupSummaryItem {
    int groupId = 0;
    size_t count = 0;
};

class PinGroupPalette {
public:
    static constexpr int normalizeGroupId(int groupId) noexcept {
        if (groupId < PinGroupConstants::kMinGroupId || groupId > PinGroupConstants::kMaxGroupId) {
            return PinGroupConstants::kUngroupedId;
        }
        return groupId;
    }

    static constexpr int normalizeFilter(int filter) noexcept {
        if (filter == PinGroupConstants::kFilterAllGroups) {
            return PinGroupConstants::kFilterAllGroups;
        }
        if (filter == PinGroupConstants::kUngroupedId) {
            return PinGroupConstants::kUngroupedId;
        }
        if (filter >= PinGroupConstants::kMinGroupId && filter <= PinGroupConstants::kMaxGroupId) {
            return filter;
        }
        return PinGroupConstants::kFilterAllGroups;
    }

    static constexpr int paletteSlot(int groupId) noexcept {
        const int norm = normalizeGroupId(groupId);
        if (norm == PinGroupConstants::kUngroupedId) {
            return 0;
        }
        return ((norm - 1) % PinGroupConstants::kPaletteColorCount) + 1;
    }

    /// 规范化十六进制颜色字符串（支持 "#RRGGBB" / "RRGGBB" / "#RGB"，空串代表恢复默认，非法返回 nullopt）
    static std::optional<std::string> normalizeHexColor(std::string_view raw);

    /// 规范化自定义分组名称（去除首尾空白与控制字符，截断至 kMaxCustomGroupNameChars）
    static std::wstring normalizeCustomName(std::wstring_view raw);

    /// 规范化导览栏/内联重命名输入（空白或等同于默认短名 "组 N"/"GN" 时返回空串以恢复默认组名）
    static std::wstring normalizeInlineRenameInput(int groupId, std::wstring_view raw);

    /// 内置 12 色高辨识度宝石色相环预设（供 UI 色板与右键快捷换色统一引用）
    static const std::array<const char*, PinGroupConstants::kPaletteColorCount>& presetPaletteHexes() noexcept;
    static const wchar_t* presetColorName(int slot, bool chinese) noexcept;
    static const char* defaultHex(int groupId) noexcept;
    static D2D1_COLOR_F defaultColor(int groupId, float alpha = 1.0f) noexcept;

    /// 当前生效的分组强调色与色值（优先返回用户自定义颜色，未设置时回退内置宝石色）
    static D2D1_COLOR_F accentColor(int groupId, float alpha = 1.0f);
    static std::string accentHex(int groupId);

    /// 当前生效的短标签与完整标签（优先展示用户自定义分组名称，保留组号快捷键锚点）
    static std::wstring shortLabel(int groupId, bool chinese);
    static std::wstring fullLabel(int groupId, bool chinese);
    static const wchar_t* colorName(int groupId, bool chinese) noexcept;

    /// 自定义分组名称与颜色查询 / 持久化管理（绑定 ConfigManager `/capture/pinGroups`）
    static void ensureLoadedFromConfig();
    static void reloadFromConfig();
    static std::wstring customName(int groupId);
    static std::string customColorHex(int groupId);
    static bool hasCustomization(int groupId);

    static bool setGroupName(int groupId, std::wstring_view name, bool persist = true);
    static bool setGroupColor(int groupId, std::string_view colorHex, bool persist = true);
    static bool setGroupCustomization(
        int groupId,
        std::optional<std::wstring_view> name,
        std::optional<std::string_view> colorHex,
        bool persist = true);
    static void resetGroupCustomization(int groupId, bool persist = true);
    static void resetAllCustomizations(bool persist = true);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINGROUPMANAGER_H
