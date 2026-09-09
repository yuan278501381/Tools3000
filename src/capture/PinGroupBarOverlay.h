#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinGroupBarOverlay.h — 屏幕顶部雪瓷白微晶贴图分组切换与编组导览栏
//
// 职责:
//   1. 当存在已分组贴图或处于按组过滤视图时，在主屏顶部居中展示轻盈雪瓷白分组胶囊栏；
//   2. 支持一键按组切换查看 (全部 / 组 1~32 / 未分组)、原位直接编辑分组名称、动态新建贴图组、
//      一键编组未分组贴图、网格整理当前组与解散当前组；
//   3. 常态严格采用 WS_EX_NOACTIVATE + UpdateLayeredWindow Per-Pixel Alpha 合成零焦点抢夺；
//      原地重命名时挂载轻量 Owned 原生编辑宿主窗，完美兼容中文 IME 候选框对齐与全套编辑热键。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINGROUPBAROVERLAY_H
#define TOOLS3000_CAPTURE_PINGROUPBAROVERLAY_H

#include "capture/PinGroupManager.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::capture {

class PinGroupBarOverlay {
public:
    static PinGroupBarOverlay& instance();

    /// 根据当前所有贴图的分组统计与激活过滤态同步更新或隐藏顶部导览栏
    void refresh(size_t totalCount,
                 size_t ungroupedCount,
                 const std::array<size_t, PinGroupConstants::kMaxGroupId + 1>& groupCounts,
                 const std::array<bool, PinGroupConstants::kMaxGroupId + 1>& createdGroups,
                 int activeFilter,
                 bool allHidden);

    /// 彻底隐藏并销毁窗口资源
    void shutdown();

    bool isVisible() const noexcept { return m_hwnd != nullptr && IsWindowVisible(m_hwnd) != FALSE; }

    /// 在顶部导览栏目标分组胶囊原位开启内联重命名输入框
    bool beginInlineRename(int groupId);

    /// 提交或取消当前原位重命名（save=true 落盘同步四端，save=false 放弃修改）
    void commitInlineRename(bool save);

    bool isInlineEditing() const noexcept {
        return m_editingGroupId >= PinGroupConstants::kMinGroupId &&
               m_editControlHwnd != nullptr &&
               IsWindow(m_editControlHwnd) != FALSE;
    }
    int editingGroupId() const noexcept { return m_editingGroupId; }
    HWND inlineEditControlHwnd() const noexcept { return m_editControlHwnd; }

    /// 规范化导览栏原位重命名输入（空白或等同于默认短名 "组 N"/"GN" 时返回空串以恢复默认名）
    static std::wstring normalizeInlineRenameInput(int groupId, std::wstring_view rawInput);

private:
    PinGroupBarOverlay() = default;
    ~PinGroupBarOverlay();
    PinGroupBarOverlay(const PinGroupBarOverlay&) = delete;
    PinGroupBarOverlay& operator=(const PinGroupBarOverlay&) = delete;

    enum class ChipKind {
        FilterGroup,
        CreateNewGroup,
        AutoGroupUngrouped,
        RenameActiveGroup,
        ArrangeActiveGroup,
        DissolveActiveGroup,
    };

    struct BarChip {
        ChipKind kind = ChipKind::FilterGroup;
        int groupParam = PinGroupConstants::kFilterAllGroups;
        std::wstring label;
        bool hasDot = false;
        D2D1_COLOR_F dotColor{};
        bool isActive = false;
        bool isSeparatorBefore = false;
        D2D1_RECT_F rect{};
    };

    bool ensureWindow();
    bool ensureInlineEditorPopup();
    bool ensureResources(float scale);
    void updateInlineEditorFont(float scale);
    void repositionInlineEditor();
    void destroyInlineEditorWindow() noexcept;
    bool isGroupCurrentlyDisplayed(int groupId) const noexcept;
    bool tryConsumeGroupDoubleClick(POINT screenPt, int hitGroupId);
    float measureChipText(const std::wstring& text, float scale);
    void rebuildAndRender(int retryDepth = 0);
    void handleChipClick(const BarChip& chip);
    void handleGroupContextMenu(int groupId, POINT screenPt);

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editControlSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HWND m_editPopupHwnd = nullptr;
    HWND m_editControlHwnd = nullptr;
    WNDPROC m_origEditWndProc = nullptr;
    HFONT m_editFont = nullptr;
    float m_editFontScale = 0.0f;
    int m_editingGroupId = 0;
    RECT m_editingScreenRect{};
    bool m_closingInlineEdit = false;

    ULONGLONG m_lastGroupClickTick = 0;
    POINT m_lastGroupClickScreenPt{};
    int m_lastClickedGroupId = 0;

    float m_dpiScale = 1.0f;
    size_t m_totalCount = 0;
    size_t m_ungroupedCount = 0;
    std::array<size_t, PinGroupConstants::kMaxGroupId + 1> m_groupCounts{};
    std::array<bool, PinGroupConstants::kMaxGroupId + 1> m_createdGroups{};
    int m_activeFilter = PinGroupConstants::kFilterAllGroups;
    int m_hoveredChipIndex = -1;
    std::vector<BarChip> m_chips;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_dcTarget;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    float m_cachedFontScale = 0.0f;
    bool m_cachedChinese = true;

    static bool s_classRegistered;
    static bool s_editPopupClassRegistered;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINGROUPBAROVERLAY_H
