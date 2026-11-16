#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinContextMenu.h — 贴图 Direct2D 微晶自绘右键复合菜单宿主与渲染引擎
//
// 职责:
//   1. 承载贴图右键复合面板：顶部 6 枚纯矢量图文快捷格 + 实时连续不透明度滑块 + 精简菜单列表；
//   2. 支持最多 3 层级联微晶子菜单（如 根菜单 -> 贴图分组 -> 切换本组颜色）、140ms 斜向悬停保护、
//      多显示器边缘自动翻转避让与超多分组滚轮滚动；
//   3. 支持完整键盘无障碍操控（Up / Down / Left / Right / Enter / Esc 及单键直达快捷键）；
//   4. 暴露离线位图渲染接口 renderPreviewBitmapForTest，支持自动化像素级视觉回归验证。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINCONTEXTMENU_H
#define TOOLS3000_CAPTURE_PINCONTEXTMENU_H

#include "capture/PinContextMenuModel.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <opencv2/core.hpp>
#include <array>
#include <functional>

namespace tools3000::capture {

class PinContextMenu {
public:
    static PinContextMenu& instance();

    /// 弹出完整微晶右键复合菜单并进入局部轻量消息循环，返回用户最终选择的指令
    /// @param ownerHwnd           所属贴图窗口句柄
    /// @param anchorScreenPt      鼠标右键触发的屏幕物理坐标
    /// @param snapshot            当前贴图与全局分组只读状态快照
    /// @param onLiveOpacityChange 滑块拖拽/滚轮微调期间的实时不透明度预览回调
    /// @param isOwnerAlive        局部循环期间校验宿主贴图是否仍然存活的谓词
    PinMenuCommand showContextMenu(HWND ownerHwnd,
                                   POINT anchorScreenPt,
                                   const PinContextMenuSnapshot& snapshot,
                                   std::function<void(float)> onLiveOpacityChange,
                                   std::function<bool()> isOwnerAlive = {});

    /// 弹出纯分组微晶菜单（供贴图左上角分组微标胶囊点击直接复用，视觉 100% 统一）
    PinMenuCommand showGroupMenu(HWND ownerHwnd,
                                 POINT anchorScreenPt,
                                 const PinContextMenuSnapshot& snapshot,
                                 std::function<bool()> isOwnerAlive = {});

    /// 主动收起当前打开的菜单（若处于展开态）
    void dismiss() noexcept;

    /// 插件卸载时释放全部窗口与 Direct2D / DirectWrite 资源
    void shutdown() noexcept;

    /// 将指定菜单数据离线渲染为 32-bit BGRA OpenCV 矩阵（供单元测试与高分屏像素级视觉校验使用）
    cv::Mat renderPreviewBitmap(const PinContextMenuData& menuData,
                                float dpiScale = 1.5f,
                                int hoveredHeaderBtn = -1,
                                int hoveredItemIdx = -1,
                                bool forceDarkTheme = true);

private:
    PinContextMenu() = default;
    ~PinContextMenu();
    PinContextMenu(const PinContextMenu&) = delete;
    PinContextMenu& operator=(const PinContextMenu&) = delete;

    struct PopupPlane {
        int level = 0;
        HWND hwnd = nullptr;
        bool visible = false;
        POINT windowPos{0, 0};

        bool hasHeaderBar = false;
        bool hasOpacitySlider = false;
        const std::vector<PinMenuItem>* items = nullptr;
        PinMenuPanelLayout layout{};

        int hoveredHeaderIndex = -1;
        int pressedHeaderIndex = -1;
        int hoveredItemIndex = -1;
        int pressedItemIndex = -1;
        int openSubmenuParentIndex = -1;
        bool hoverSliderThumb = false;
        bool hoverSliderPctPill = false;
        float scrollOffsetY = 0.0f;

        HDC memDc = nullptr;
        HBITMAP dib = nullptr;
        HGDIOBJ oldBmp = nullptr;
        int dibW = 0;
        int dibH = 0;

        void releaseSurface() noexcept;
    };

    PinMenuCommand runModalMenu(HWND ownerHwnd,
                                POINT anchorScreenPt,
                                const PinContextMenuData& rootData,
                                const PinContextMenuSnapshot& snapshot,
                                std::function<void(float)> onLiveOpacityChange,
                                std::function<bool()> isOwnerAlive);

    bool ensureGraphicsResources(float dpiScale, bool isChinese);
    bool ensurePlaneWindow(int level, HWND ownerHwnd);
    float measureTextWidth(const std::wstring& text, float fontPx);

    void openRootPlane(POINT anchorScreenPt, float maxWorkAreaH);
    void openChildPlane(int childLevel, int parentItemIndex, const std::vector<PinMenuItem>& submenuItems);
    void closePlanesFrom(int startLevel) noexcept;
    void hideAllPlanes() noexcept;

    void renderPlane(int level);
    void drawPlaneContents(ID2D1RenderTarget* rt,
                           const PopupPlane& plane,
                           bool isDark,
                           float dpiScale);

    bool isMenuFamilyHwnd(HWND hwnd) const noexcept;
    int findPlaneLevel(HWND hwnd) const noexcept;
    int deepestVisibleLevel() const noexcept;

    int hitTestHeaderButton(const PopupPlane& plane, float x, float y) const noexcept;
    bool hitTestSliderTrackArea(const PopupPlane& plane, float x, float y) const noexcept;
    bool hitTestSliderPctPill(const PopupPlane& plane, float x, float y) const noexcept;
    int hitTestMenuItem(const PopupPlane& plane, float x, float y) const noexcept;
    void ensureItemVisibleInScroll(PopupPlane& plane, int itemIndex) noexcept;
    void applyLiveOpacity(float newOpacity);

    LRESULT handlePlaneMessage(int level, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK planeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static bool s_rootClassRegistered;
    static bool s_subClassRegistered;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_dcTarget;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_headerLabelFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_itemLabelFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_itemLabelBoldFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_shortcutFmt;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFmt;
    float m_cachedFontScale = 0.0f;
    bool m_cachedFontChinese = true;

    std::array<PopupPlane, PinContextMenuMetrics::kMaxCascadeDepth> m_planes{};
    const PinContextMenuData* m_activeRootData = nullptr;
    PinContextMenuSnapshot m_activeSnapshot{};
    std::function<void(float)> m_onLiveOpacityChange;
    float m_dpiScale = 1.0f;
    float m_currentOpacity = 1.0f;
    RECT m_workArea{0, 0, 1920, 1080};

    bool m_running = false;
    bool m_draggingSlider = false;
    bool m_hasPointerDownInside = false;
    int m_pendingHoverLevel = -1;
    int m_pendingHoverItemIndex = -1;
    PinMenuCommand m_selectedCommand{};
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINCONTEXTMENU_H
