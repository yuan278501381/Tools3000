#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RadialMenuOverlay — 原生呼出轮盘
//
// 职责:
//   1. 使用 Direct2D 和 Layered Window 渲染绚丽的高清径向菜单
//   2. 追踪鼠标轨迹，高亮所在的扇区
//   3. 松开鼠标时触发对应的 Command
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H
#define TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace tools3000::gesture {

struct RadialMenuItem {
    std::string label;
    std::string command;
    // 后续可扩展 icon 路径
};

class RadialMenuOverlay {
public:
    static RadialMenuOverlay& instance();

    /// 注册菜单项 (按顺时针方向，从顶部开始)
    void setItems(const std::vector<RadialMenuItem>& items);
    const std::vector<RadialMenuItem>& getItems() const { return m_items; }

    /// 在指定坐标中心显示轮盘
    void show(POINT centerPt);

    /// 隐藏轮盘
    void hide();

    /// 是否正在显示
    bool isVisible() const { return m_visible; }

private:
    RadialMenuOverlay();
    ~RadialMenuOverlay();
    RadialMenuOverlay(const RadialMenuOverlay&) = delete;
    RadialMenuOverlay& operator=(const RadialMenuOverlay&) = delete;

    void registerWindowClass();
    bool createResources();
    void discardResources();
    void render();
    void updateLayeredWindow(HDC screenDc, HDC memoryDc);

    int hitTest(POINT pt);
    void executeAction(int index);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    bool m_visible = false;
    POINT m_centerPt = {0, 0};
    int m_hoverIndex = -1;
    
    int m_radiusOuter = 150;
    int m_radiusInner = 40;
    int m_windowSize = 400;
    float m_dpiScale = 1.0f;

    // 动画状态
    UINT_PTR m_timerId = 0;
    DWORD m_showStartTime = 0;
    float m_popScale = 0.0f;           // 当前的弹出缩放比例 (0.0 -> 1.0)
    std::vector<float> m_hoverRadii;   // 每个扇区的额外半径扩展量，用于悬停平滑动画
    DWORD m_lastTickTime = 0;          // 上一帧时间，用于平滑更新

    std::vector<RadialMenuItem> m_items;

    // D2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;

    HBITMAP m_hBitmap = nullptr;
    void* m_bits = nullptr;
};

} // namespace tools3000::gesture

#endif // TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H
