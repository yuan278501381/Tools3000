#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RadialMenuOverlay — 原生呼出轮盘
//
// 职责:
//   1. 使用 Direct2D 和 Layered Window 渲染绚丽的高清径向菜单
//   2. 追踪鼠标轨迹，实时高亮所在的扇区
//   3. 点击或松开鼠标时触发对应的 Command，点击中心或外部自动收起
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H
#define TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
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
    std::vector<RadialMenuItem> getItems() const;

    /// 在指定坐标中心显示轮盘
    void show(POINT centerPt);

    /// 隐藏轮盘
    void hide();

    /// 响应全局主题或强调色动态切换
    void reloadThemeColors();

    /// 插件卸载/退出时彻底销毁轮盘窗口、辅助宿主窗口与 Direct2D/GDI 资源
    void shutdown();

    /// 是否正在显示 (原子无锁读取，供全局低级鼠标钩子与触发角安全查询)
    bool isVisible() const noexcept { return m_visible.load(std::memory_order_acquire); }

    /// 当前悬停扇区索引 (-1 表示中心取消区或未命中扇区)
    int hoverIndex() const noexcept { return m_hoverIndex.load(std::memory_order_acquire); }

    /// 获取当前轮盘中心屏幕坐标 (线程安全快照)
    POINT centerPoint() const noexcept;

    /// 获取轮盘窗口句柄 (供测试与窗口识别使用)
    HWND hwnd() const noexcept { return m_hwnd; }

    /// 供单元测试直接对局部客户区坐标执行扇区命中判定
    int hitTestClientPoint(POINT clientPt) const noexcept { return hitTest(clientPt); }

    /// 判定屏幕坐标是否落在轮盘有效交互圆盘内 (含内圆与外圈扩展容差)
    bool isPointInsideRing(POINT screenPt) const noexcept;

    /// 由全局低级鼠标钩子转发鼠标动作，实现跨窗口悬停、外部点击收起与点击确认闭环
    /// 返回 true 表示本次按键由轮盘消费拦截，不应透传给底层窗口
    bool handleGlobalMouseMessage(WPARAM message, POINT screenPt) noexcept;

private:
    RadialMenuOverlay();
    ~RadialMenuOverlay();
    RadialMenuOverlay(const RadialMenuOverlay&) = delete;
    RadialMenuOverlay& operator=(const RadialMenuOverlay&) = delete;

    void registerWindowClass();
    bool createResourcesLocked();
    void discardResourcesLocked();
    void deleteBitmapLocked() noexcept;
    void render();
    void updateLayeredWindowLocked(HDC screenDc, HDC memoryDc, float popScale);

    int hitTest(POINT pt, bool clampOuterRing = false) const noexcept;
    void confirmSelectionAtClientPoint(POINT clientPt);
    void executeAction(int index);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
    mutable std::mutex m_stateMutex;
    HWND m_hwnd = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    HWND m_initialForegroundHwnd = nullptr;
    std::atomic<bool> m_visible{false};
    POINT m_centerPt = {0, 0};
    std::atomic<int> m_windowLeft{0};
    std::atomic<int> m_windowTop{0};
    std::atomic<int> m_hoverIndex{-1};
    std::atomic<bool> m_sawButtonDown{false};

    std::atomic<int> m_radiusOuter{150};
    std::atomic<int> m_radiusInner{40};
    std::atomic<int> m_windowSize{400};
    std::atomic<float> m_dpiScale{1.0f};

    // 动画状态
    UINT_PTR m_timerId = 0;
    std::atomic<DWORD> m_showStartTime{0};
    float m_popScale = 0.0f;           // 当前的弹出缩放比例 (0.0 -> 1.0)
    std::vector<float> m_hoverRadii;   // 每个扇区的额外半径扩展量，用于悬停平滑动画
    DWORD m_lastTickTime = 0;          // 上一帧时间，用于平滑更新

    mutable std::mutex m_itemsMutex;
    std::vector<RadialMenuItem> m_items;
    std::atomic<size_t> m_itemCount{0};

    // D2D Resources
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_hoverBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> m_ellipsisTrimmingSign;

    HBITMAP m_hBitmap = nullptr;
    void* m_bits = nullptr;
};

} // namespace tools3000::gesture

#endif // TOOLS3000_GESTURE_RADIALMENU_OVERLAY_H
