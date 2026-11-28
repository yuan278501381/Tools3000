#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MouseFeedbackOverlayBase.h — 硬件加速鼠标反馈可视化覆盖层基类 (世界级跨系统零掉帧架构)
//
// 架构职责:
//   1. 三级 GPU 呈现降级链：
//      - Tier 1 (首选): DXGI CreateSwapChainForComposition (FLIP_DISCARD + FRAME_LATENCY_WAITABLE_OBJECT) + WS_EX_NOREDIRECTIONBITMAP
//      - Tier 2 (老 WDDM / 虚拟机驱动): FLIP_SEQUENTIAL SwapChain + DwmFlush
//      - Tier 3 (极端环境兜底): 高水位滞回复用 IDCompositionSurface
//      - D3D11 HARDWARE 自动无感降级 WARP 软件光栅化
//   2. 三态智能节拍调度：
//      - Idle / StaticHolding: INFINITE 事件挂起 (0.00% CPU / 0% GPU)
//      - Animating: 动态 LoadLibraryW("avrt.dll") MMCSS + DwmFlush/FrameLatencyWaitable + QPC 防空转校验 + 高精度 Waitable Timer (Win10 1607 / Server 2016 自动降级标准 WaitableTimer + 活跃期动态 timeBeginPeriod(1)) + RDP/WARP 60FPS 封顶
//   3. 专用高优先级异步渲染线程 (std::jthread)，独立 m_wakeEvent (输入唤醒) 与 m_controlEvent (控制立即打断)
//   4. 优雅容灾：GPU 设备丢失 (DEVICE_REMOVED/RESET/D2DERR_RECREATE_TARGET) 毫秒级自愈 + 熔断保护
//   5. 实证帧率遥测 (OverlayFrameMetrics)：精准统计 P50/P95/P99 帧间隔、丢帧率、SetWindowPos 次数与表面重建次数
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

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace tools3000::core {

enum class OverlayPresentTier : uint8_t {
    None = 0,
    SwapChainWaitable = 1,
    SwapChainSequential = 2,
    DcompSurfaceFallback = 3,
};

struct OverlayFrameMetrics {
    uint64_t framesPresented = 0;
    uint64_t surfaceRebuilds = 0;
    uint64_t setWindowPosCalls = 0;
    uint64_t deviceResets = 0;
    OverlayPresentTier presentTier = OverlayPresentTier::None;
    bool usingWarp = false;
    bool remoteSession = false;
    double targetHz = 60.0;
    double p50FrameMs = 0.0;
    double p95FrameMs = 0.0;
    double p99FrameMs = 0.0;
    double dropRatePercent = 0.0;
};

class TOOLS3000CORE_API MouseFeedbackOverlayBase {
public:
    virtual ~MouseFeedbackOverlayBase();

    /// 初始化独立渲染管线与硬件覆盖层窗口（lazyCompositor=true 时冷启动不预建 D3D11/D2D1 设备，首次绘制按需初始化且闲置 60s 自动回收）
    bool initializeBase(HINSTANCE hInstance, const wchar_t* windowClassName, const wchar_t* helperOwnerName,
                        bool lazyCompositor = false);

    /// 关闭并释放所有管线与显存资源 (安全消息泵送退出)
    void shutdownBase();

    /// 唤醒渲染线程刷新一帧 (仅用于 Idle / StaticHolding 唤醒，绝不打断 Animating 中的 VSync 节拍)
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
    bool hasPresentSurface() const noexcept;
    HWND hwnd() const noexcept;
    OverlayPresentTier presentTier() const noexcept;
    int maxTextureDimension() const noexcept;
    bool isSoftwareWarp() const noexcept;

    /// 默认滞后表面释放（3s）与 Compositor 释放（60s）常量，以及单测专用滞后释放覆盖接口
    static constexpr uint32_t kDefaultLazySurfaceIdleReleaseMs = 3000u;
    static constexpr uint32_t kDefaultLazyCompositorIdleReleaseMs = 60000u;
    void setLazySurfaceIdleReleaseMsForTest(uint32_t releaseMs) noexcept;
    uint32_t lazySurfaceIdleReleaseMs() const noexcept;

    /// 实证帧率与呈现遥测
    OverlayFrameMetrics getFrameMetrics() const;
    void resetFrameMetrics();

    /// 静态工具：查询当前显示器刷新率（支持 DWM 组合定时 + EnumDisplaySettingsW 兜底，RDP/WARP 自动封顶 60Hz）
    static double queryEffectiveRefreshHz(bool remoteSession, bool usingWarp) noexcept;

    /// 静态工具：基于 QPC 单调微秒时钟与预期帧周期判断是否触发了瞬时返回（防 RDP / 锁屏 / 虚拟显卡 100% CPU 暴走）
    static int64_t computeAntiSpinRemainingUs(int64_t elapsedUs, int64_t targetPeriodUs) noexcept;

    /// 静态工具：判定等待 FrameLatencyWaitable 后是否仍需调用 DwmFlush（防止双 VSync 叠加将 60/144Hz 腰斩为半帧率）
    static bool shouldInvokeDwmFlushAfterWaitable(int64_t waitedUs, int64_t sinceLastFrameUs, int64_t targetPeriodUs) noexcept;

protected:
    MouseFeedbackOverlayBase();

    /// 纯虚接口：由子类实现单帧业务逻辑 (更新动效状态机、微粒运动等)
    virtual void onRenderTick() = 0;

    /// 纯虚接口：判断当前是否处于完全空闲状态 (Idle 时休眠，0% CPU/GPU)
    virtual bool isOverlayIdle() const = 0;

    /// 虚拟接口：判断当前是否需要按 VSync 连续帧刷新（常亮静止态返回 false 可直接挂起 0% CPU，待鼠标移动再唤醒）
    virtual bool needsContinuousAnimation() const;

    /// 虚拟接口：在硬件设备丢失或重新初始化完成时通知子类重建专属笔刷/纹理
    virtual void onRecreateDeviceResources();

    /// 虚拟接口：在硬件设备释放时通知子类释放设备相关资源
    virtual void onReleaseDeviceResources();

    /// 硬件合成上下文管理 (需在渲染线程内部调用)
    bool initCompositorLocked();
    void releaseCompositorLocked();
    void releaseIdleCompositor();
    bool ensurePresentSurfaceLocked(int width, int height);
    bool ensureDcompSurfaceLocked(int width, int height);
    void releaseDcompSurfaceLocked();

    /// 视口与几何尺寸更新 (由渲染线程执行；几何无变更且已可见时 0 SetWindowPos 调用)
    void applyViewportLocked(const MouseViewportRect& viewport);
    void ensureViewportBeforeDrawLocked(const MouseViewportRect& viewport);

    /// 统一 Direct2D 显存直通绘制入口（自动适配 Tier 1/2 SwapChain 与 Tier 3 DCompSurface，含设备丢失熔断自愈）
    bool beginFrameLocked(int width, int height, ID2D1DeviceContext** outContext, POINT* surfaceOffset = nullptr);
    bool endFrameAndPresentLocked();

    /// 兼容旧接口：转发至统一帧管线
    bool beginDrawLocked(ID2D1DeviceContext** outContext, POINT* surfaceOffset = nullptr);
    bool endDrawAndCommitLocked();

    /// 成员变量
    HINSTANCE m_hInstance = nullptr;
    std::atomic<HWND> m_hwnd{nullptr};
    HWND m_helperOwnerHwnd = nullptr;
    std::wstring m_windowClassName;
    std::wstring m_helperOwnerName;

    std::jthread m_renderThread;
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_controlEvent = nullptr;

    std::atomic<bool> m_wakeRender{false};
    std::atomic<bool> m_hideRequested{false};
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_zOrderYieldRequested{false};
    std::atomic<bool> m_zOrderRaiseRequested{false};
    std::atomic<bool> m_viewportDirty{false};
    MouseViewportRect m_pendingViewport;

    // DirectComposition / DXGI SwapChain 硬件加速
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<IDXGIFactory2> m_dxgiFactory2;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    HANDLE m_frameLatencyWaitable = nullptr;
    UINT m_swapChainFlags = 0;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionSurface> m_dcompSurface;

    // Direct2D 显存设备与绘图上下文
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory1;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_frameTargetBitmap;

    int m_surfaceWidth = 0;
    int m_surfaceHeight = 0;
    int m_requestedWidth = 0;
    int m_requestedHeight = 0;
    int m_maxTextureDim = 16384;
    bool m_usingWarp = false;
    bool m_usingNoRedirectionBitmap = false;
    bool m_dcompNeedsCommit = false;
    bool m_visualContentDetached = false;
    bool m_frameInProgress = false;
    bool m_lazyCompositor = false;
    std::chrono::steady_clock::time_point m_idleSince{};
    std::atomic<int64_t> m_hiddenSinceMs{0};
    std::atomic<uint32_t> m_lazySurfaceIdleReleaseMs{kDefaultLazySurfaceIdleReleaseMs};
    std::chrono::steady_clock::time_point m_oversizedSince{};
    std::chrono::steady_clock::time_point m_resetWindowStart{};
    int m_recentDeviceResets = 0;
    std::chrono::steady_clock::time_point m_circuitBreakerUntil{};

    std::atomic<OverlayPresentTier> m_presentTier{OverlayPresentTier::None};
    std::atomic<bool> m_compositorReady{false};
    std::atomic<bool> m_hasDcompSurface{false};
    std::atomic<bool> m_hasLiveSurface{false};
    MouseViewportRect m_currentViewport;

    mutable std::mutex m_baseMutex;
    void applyHideOverlayStateLocked();

    // 帧遥测统计（环形缓冲记录最近 512 帧帧间隔微秒）
    mutable std::mutex m_metricsMutex;
    static constexpr size_t kFrameSampleCapacity = 512;
    std::array<int64_t, kFrameSampleCapacity> m_frameDeltasUs{};
    size_t m_frameSampleCount = 0;
    size_t m_frameSampleHead = 0;
    int64_t m_lastPresentedQpcUs = 0;
    uint64_t m_framesPresentedCount = 0;
    uint64_t m_surfaceRebuildCount = 0;
    uint64_t m_setWindowPosCount = 0;
    uint64_t m_deviceResetCount = 0;
    double m_lastMeasuredTargetHz = 60.0;

private:
    static LRESULT CALLBACK baseWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void baseRenderLoop(std::stop_token st, HANDLE readyEvent);
    bool createSwapChainForSizeLocked(int width, int height);
    bool createFallbackDcompSurfaceLocked(int width, int height);
    void recordFramePresentedLocked(double targetHz) noexcept;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_MOUSE_FEEDBACK_OVERLAY_BASE_H
