#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinContextMenuModel.h — 贴图微晶自绘右键复合菜单纯数据模型与多屏布局计算中枢
//
// 职责:
//   1. 定义贴图微晶右键面板（顶部 6 枚快捷图文栏 + 实时不透明度滑块 + 精简分层列表 + 级联子菜单）
//      的强类型命令枚举、状态快照契约与节点树模型；
//   2. 纯函数构建场景感知菜单树（单张贴图自动隐去全局批处理、局部选字自动置顶复制、非默认状态微标回显）；
//   3. 纯几何计算面板尺寸、滑块坐标映射、多显示器工作区边缘自动翻转避让与超长分组菜单滚动视口；
//   4. 零 HWND / GDI 副作用，支持在单元测试中 100% 确定性验证全部交互与几何边界。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINCONTEXTMENUMODEL_H
#define TOOLS3000_CAPTURE_PINCONTEXTMENUMODEL_H

#include "capture/CaptureVectorIcons.h"

#include <windows.h>
#include <d2d1.h>
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace tools3000::capture {

/// 微晶右键菜单排版与交互几何常量 (SSOT，杜绝魔术字面量)
namespace PinContextMenuMetrics {
    inline constexpr int kHeaderActionCount = 6;
    inline constexpr int kMaxCascadeDepth = 3;

    inline constexpr float kShadowMarginDip = 10.0f;
    inline constexpr float kPanelCornerRadiusDip = 12.0f;
    inline constexpr float kPanelPadXDip = 7.0f;
    inline constexpr float kPanelPadYDip = 7.0f;

    inline constexpr float kHeaderBtnHeightDip = 50.0f;
    inline constexpr float kHeaderBtnGapDip = 4.0f;
    inline constexpr float kHeaderBtnRadiusDip = 8.0f;
    inline constexpr float kHeaderIconSizeDip = 24.0f;

    inline constexpr float kSliderRowHeightDip = 36.0f;
    inline constexpr float kSliderTrackHeightDip = 5.0f;
    inline constexpr float kSliderThumbRadiusDip = 6.8f;
    inline constexpr float kSliderThumbActiveRadiusDip = 7.8f;
    inline constexpr float kSliderPctPillWidthDip = 42.0f;
    inline constexpr float kSliderWheelStep = 0.05f;
    inline constexpr float kSliderMagneticSnapTolerance = 0.014f;

    inline constexpr float kItemRowHeightDip = 32.0f;
    inline constexpr float kItemCornerRadiusDip = 7.0f;
    inline constexpr float kItemIconLeadWidthDip = 28.0f;
    inline constexpr float kItemIconSizeDip = 21.0f;
    inline constexpr float kItemChevronWidthDip = 18.0f;
    inline constexpr float kItemTextGapDip = 8.0f;
    inline constexpr float kItemRightPadDip = 10.0f;
    inline constexpr float kStatusBadgePadXDip = 6.0f;
    inline constexpr float kStatusBadgeHeightDip = 18.0f;

    inline constexpr float kSeparatorHeightDip = 9.0f;
    inline constexpr float kMinRootPanelWidthDip = 312.0f;
    inline constexpr float kMinSubPanelWidthDip = 232.0f;
    inline constexpr float kMaxPanelWidthDip = 420.0f;

    inline constexpr float kSubmenuOverlapDip = 3.0f;
    inline constexpr float kScreenEdgeMarginDip = 6.0f;
    inline constexpr float kScrollStepDip = 32.0f;

    inline constexpr float kMinUiFontPx = 12.0f;
    inline constexpr float kHeaderLabelFontDip = 12.0f;
    inline constexpr float kItemLabelFontDip = 12.6f;
    inline constexpr float kShortcutFontDip = 12.0f;
    inline constexpr float kBadgeFontDip = 12.0f;

    inline constexpr UINT kSubmenuHoverTimerId = 101;
    inline constexpr UINT kSubmenuHoverDelayMs = 140;
} // namespace PinContextMenuMetrics

/// 贴图右键菜单强类型动作枚举
enum class PinMenuAction {
    None = 0,
    CopyImage,
    CopySelectedText,
    CopyNoteText,
    CopyAllText,
    EditMarkup,
    ExtractTextDialog,
    TranslateInPlace,
    RetranslateInPlace,
    ToggleTranslationOverlay,
    CopyTranslatedText,
    ToggleTextSelectionMode,
    SelectAllText,
    SaveImage,
    ResetScale,
    RotateRight90,
    RotateLeft90,
    FlipHorizontal,
    FlipVertical,
    ToggleInvert,
    ToggleGrayscale,
    ToggleFold,
    ToggleClickThrough,
    ArrangeAllPins,
    ToggleHideAllPins,
    CloseOtherPins,
    CloseAllPins,
    ClosePin,

    // 贴图分组与按组过滤动作家族
    GroupAssign,            // param = groupId (1..32)
    GroupCreateAndAssign,
    GroupRemoveCurrent,
    GroupAutoGroupUngrouped,
    GroupViewAll,
    GroupViewOnly,          // param = groupId (1..32)
    GroupArrangeCurrent,
    GroupDissolveCurrent,
    GroupCloseCurrent,
    GroupRenameCurrent,
    GroupSetColorPreset,    // param = presetSlot (1..N)
    GroupResetMeta,
};

/// 强类型菜单指令载荷
struct PinMenuCommand {
    PinMenuAction action = PinMenuAction::None;
    int param = 0;

    constexpr bool isValid() const noexcept { return action != PinMenuAction::None; }
    constexpr bool operator==(const PinMenuCommand& other) const noexcept {
        return action == other.action && param == other.param;
    }
};

/// 贴图只读状态快照（输入给纯函数构建器）
struct PinContextMenuSnapshot {
    bool isChinese = true;
    bool isFolded = false;
    bool isInverted = false;
    bool isGrayscale = false;
    bool isClickThrough = false;
    bool isTextSelectionMode = false;
    bool hasSelectedText = false;
    bool hasPinnedSourceText = false;
    bool hasTranslation = false;
    bool isShowingTranslation = false;
    bool isTranslationInFlight = false;
    float scale = 1.0f;
    float opacity = 1.0f;

    int currentGroupId = 0;
    int activeGroupFilter = 0;
    int nextAvailableGroupId = 1;
    size_t totalPinCount = 1;
    size_t ungroupedPinCount = 1;
    bool allPinsHidden = false;
    bool currentGroupHasCustomization = false;
    std::wstring currentGroupShortLabel;
    D2D1_COLOR_F currentGroupAccentColor{0.23f, 0.51f, 0.96f, 1.0f};

    struct GroupEntryInfo {
        int groupId = 0;
        size_t count = 0;
        bool showInAssignList = false;
        bool showInViewList = false;
        std::wstring shortLabel;
        std::wstring fullLabel;
        D2D1_COLOR_F accentColor{0.23f, 0.51f, 0.96f, 1.0f};
    };
    std::vector<GroupEntryInfo> groups;

    struct ColorPresetInfo {
        int slotIndex = 0; // 1-based
        std::wstring name;
        D2D1_COLOR_F color{0.23f, 0.51f, 0.96f, 1.0f};
        bool isCurrent = false;
    };
    std::vector<ColorPresetInfo> currentGroupColorPresets;
};

/// 顶栏快捷图标格模型
struct PinMenuHeaderButton {
    CaptureIconId icon = CaptureIconId::None;
    std::wstring shortLabel;
    PinMenuCommand primaryCommand{};
    PinMenuCommand shiftCommand{};
    bool isActive = false;
};

/// 列表菜单项节点（支持递归级联子菜单）
struct PinMenuItem {
    bool isSeparator = false;
    CaptureIconId icon = CaptureIconId::None;
    bool hasColorDot = false;
    D2D1_COLOR_F dotColor{0.0f, 0.0f, 0.0f, 0.0f};
    std::wstring label;
    std::wstring statusBadge;
    std::wstring shortcutText;
    bool isChecked = false;
    bool isDanger = false;
    PinMenuCommand command{};
    std::vector<PinMenuItem> submenu;

    bool hasSubmenu() const noexcept { return !submenu.empty(); }
    bool isSelectable() const noexcept { return !isSeparator && (command.isValid() || hasSubmenu()); }
};

/// 完整菜单面板数据模型
struct PinContextMenuData {
    bool hasHeaderBar = false;
    std::array<PinMenuHeaderButton, PinContextMenuMetrics::kHeaderActionCount> headerButtons{};
    bool hasOpacitySlider = false;
    std::wstring opacityLabel;
    float initialOpacity = 1.0f;
    std::vector<PinMenuItem> items;
};

/// 单层弹出面板几何计算结果（窗口局部物理像素坐标系）
struct PinMenuPanelLayout {
    SIZE windowSize{0, 0};
    D2D1_RECT_F panelRect{};
    bool hasHeaderBar = false;
    std::array<D2D1_RECT_F, PinContextMenuMetrics::kHeaderActionCount> headerButtonRects{};
    bool hasOpacitySlider = false;
    D2D1_RECT_F sliderRowRect{};
    D2D1_RECT_F sliderLabelRect{};
    D2D1_RECT_F sliderTrackRect{};
    D2D1_RECT_F sliderPctPillRect{};
    D2D1_RECT_F itemsViewportRect{};
    std::vector<D2D1_RECT_F> itemRects; // 未施加 scrollOffsetY 的基准内容坐标
    float totalItemsContentHeight = 0.0f;
    float maxScrollOffsetY = 0.0f;
};

/// 文本测量回调签名（输入文本与字号 px，返回向上取整像素宽度）
using PinMenuTextMeasureFn = std::function<float(const std::wstring& text, float fontPx)>;

class PinContextMenuBuilder {
public:
    /// 构建完整右键复合菜单（顶栏 6 枚快捷图标 + 实时不透明度滑块 + 精简列表 + 级联子菜单）
    static PinContextMenuData buildRootMenu(const PinContextMenuSnapshot& snap);

    /// 构建纯分组弹出菜单（供贴图左上角分组胶囊左键单击复用同一微晶渲染基座）
    static PinContextMenuData buildGroupOnlyMenu(const PinContextMenuSnapshot& snap);

    /// 解析菜单展开期间的直接键盘快捷键（如 E / M / T / I / G / R / L / H / V / 0 / Ctrl+C / Ctrl+S / Ctrl+T）
    static PinMenuCommand resolveDirectKeyShortcut(WPARAM vk,
                                                   bool ctrl,
                                                   bool shift,
                                                   bool alt,
                                                   const PinContextMenuSnapshot& snap) noexcept;
};

class PinContextMenuLayoutEngine {
public:
    /// 计算单层弹出面板（根面板或任意子菜单面板）的精确像素布局与滚动上限
    static PinMenuPanelLayout computePanelLayout(bool hasHeaderBar,
                                                 const std::array<PinMenuHeaderButton, PinContextMenuMetrics::kHeaderActionCount>* headerButtons,
                                                 bool hasOpacitySlider,
                                                 const std::wstring& opacityLabel,
                                                 const std::vector<PinMenuItem>& items,
                                                 float dpiScale,
                                                 float maxWorkAreaHeightPx,
                                                 const PinMenuTextMeasureFn& measureText);

    /// 计算根菜单窗口的屏幕左上角坐标（含边缘不足时自动向左/向上翻转并钳制在工作区内）
    static POINT placeRootWindow(POINT cursorScreenPt,
                                 SIZE windowSize,
                                 const RECT& workArea,
                                 float shadowMarginPx) noexcept;

    /// 计算级联子菜单窗口的屏幕左上角坐标（优先贴合父面板右侧，右侧空间不足时自动翻转至父面板左侧）
    static POINT placeSubmenuWindow(const RECT& parentPanelScreenRect,
                                    float anchorItemTopScreenY,
                                    SIZE childWindowSize,
                                    const RECT& workArea,
                                    float shadowMarginPx,
                                    float overlapPx) noexcept;

    /// 将滑轨上的鼠标 X 坐标映射为 [0.10, 1.00] 的归一化不透明度（含 50% / 75% / 100% 轻磁吸）
    static float opacityFromSliderX(float clientX, const D2D1_RECT_F& trackRect) noexcept;

    /// 根据归一化不透明度计算滑轨圆钮中心 X 坐标
    static float sliderXFromOpacity(float opacity, const D2D1_RECT_F& trackRect) noexcept;

    /// 在可选菜单项中循环查找上/下一个可交互索引（键盘 Up / Down 导航）
    static int findNextSelectableIndex(const std::vector<PinMenuItem>& items,
                                       int currentIndex,
                                       int stepDirection) noexcept;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINCONTEXTMENUMODEL_H
