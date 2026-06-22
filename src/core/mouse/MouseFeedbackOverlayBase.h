#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MouseFeedbackOverlayBase.h — 硬件加速鼠标反馈可视化覆盖层基类
//
// 架构职责:
//   1. 统一管理 DirectComposition / Direct2D GPU 显存直通硬件上下文
//   2. 专用高优先级异步渲染线程 (std::jthread)，彻底杜绝主 UI 线程与输入钩子跨线程死锁
//   3. 原生集成 FocusAssistAvoidance 局部包围盒与全屏避让视口
//   4. 深度休眠契约：无鼠标交互或动效静止时等待事件 (0% CPU / 0% GPU)
//   5. 优雅容灾：硬件设备丢失 (DXGI_ERROR_DEVICE_REMOVED/RESET) 毫秒级自愈与重建
//   6. 线程亲和性保护：HWND 始终由渲染线程独占创建、更新与销毁，杜绝跨线程互锁
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_MOUSE_FEEDBACK_OVERLAY_BASE_H
#define TOOLS3000_CORE_MOUSE_FEEDBACK_OVERLAY_BASE_H

#include "core/utils/Export.h"
#include "core/mouse/MouseViewportManager.h"

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace tools3000::core {

class TOOLS3000CORE_API MouseFeedbackOverlayBase {
public:
    virtual ~MouseFeedbackOverlayBase();

    /// 初始化独立渲染管线与硬件覆盖层窗口
    bool initializeBase(HINSTANCE hInstance, const wchar_t* windowClassName, const wchar_t* helperOwnerName);

    /// 关闭并释放所有管线与显存资源 (安全消息泵送退出)
    void shutdownBase();

    /// 唤醒渲染线程刷新一帧
    void wakeRender() noexcept;

    /// 跨线程请求安全更新视口几何尺寸 (由渲染线程在其自身上下文中调用 SetWindowPos)
    void requestViewport(const MouseViewportRect& viewport) noexcept;

    /// 跨线程请求安全隐藏覆盖层 (由渲染线程自身执行 ShowWindow(SW_HIDE) 并收缩内存)
    void requestHide() noexcept;

    /// 注入按键前暂时让出 TOPMOST 状态，防止阻挡目标窗口置顶
    void yieldZOrderForInput() noexcept;

    /// 开始绘制时拉回 TOPMOST 状态
    void raiseZOrderForDraw() noexcept;

    /// 状态查询
    bool isCompositorReady() const noexcept;
    bool isVisible() const noexcept;
    HWND hwnd() const noexcept;

protected:
    MouseFeedbackOverlayBase();

    /// 纯虚接口：由子类实现单帧业务逻辑 (更新动效状态机、微粒运动等)
    virtual void onRenderTick() = 0;

    /// 纯虚接口：判断当前是否处于完全空闲状态 (Idle 时休眠，0% CPU/GPU)
    virtual bool isOverlayIdle() const = 0;

    /// 纯虚接口：在硬件设备丢失或表面重建时通知子类重建专属笔刷/纹理
    virtual void onRecreateDeviceResources() {}

    /// 纯虚接口：在硬件设备释放时通知子类释放设备相关资源
    virtual void onReleaseDeviceResources() {}

    /// 硬件合成上下文管理 (需在渲染线程内部调用)
    bool initCompositorLocked();
    void releaseCompositorLocked();
    bool ensureDcompSurfaceLocked(int width, int height);
    void releaseDcompSurfaceLocked();

    /// 视口与几何尺寸更新 (由渲染线程执行)
    void applyViewportLocked(const MouseViewportRect& viewport);

    /// Direct2D 显存直通绘制入口
    bool beginDrawLocked(ID2D1DeviceContext** outContext, POINT* surfaceOffset = nullptr);
    bool endDrawAndCommitLocked();

    /// 成员变量
    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    std::wstring m_windowClassName;
    std::wstring m_helperOwnerName;

    std::jthread m_renderThread;
    HANDLE m_wakeEvent = nullptr;

    std::atomic<bool> m_wakeRender{false};
    std::atomic<bool> m_hideRequested{false};
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_zOrderYieldRequested{false};
    std::atomic<bool> m_zOrderRaiseRequested{false};
    std::atomic<bool> m_viewportDirty{false};
    MouseViewportRect m_pendingViewport;

    // DirectComposition 硬件加速
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionSurface> m_dcompSurface;

    // Direct2D 显存设备与绘图上下文
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory1;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;

    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;
    bool m_compositorReady = false;
    MouseViewportRect m_currentViewport;

    mutable std::mutex m_baseMutex;
    void applyHideOverlayStateLocked();

private:
    static LRESULT CALLBACK baseWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void baseRenderLoop(std::stop_token st, HANDLE readyEvent);
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_MOUSE_FEEDBACK_OVERLAY_BASE_H
