#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureTrailOverlay — 手势轨迹可视化覆盖层 (自建 HWND + 原生 DirectComposition 硬件合成)
//
// 职责:
//   1. 渲染线程自建并独立持有 HWND (m_hwnd 与 m_toastHwnd)，彻底根除跨线程 HWND 亲和性互锁
//   2. 激活原生 DirectComposition GPU 显存直通硬件合成 (IDCompositionDevice + DXGI Surface)
//   3. 彻底废除 GDI CreateDIBSection 软表面、CPU 像素计算 ensurePremultipliedAlpha 与 UpdateLayeredWindow
//   4. 0ms 按下即画，行云流水般的顺滑手绘流动感
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_TRAILOVERLAY_H
#define TOOLS3000_GESTURE_TRAILOVERLAY_H

#include "core/utils/SpscRingBuffer.h"
#include "core/utils/ThemeUtils.h"
#include "gesture/GesturePacer.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace tools3000::gesture {

/// 轨迹点
struct TrailPoint {
    float x;
    float y;
    DWORD timestamp;  // GetTickCount64 时间戳
};

/// 轨迹样式配置
struct TrailStyle {
    float lineWidth     = 3.0f;     // 线条宽度
    float outlineWidth  = 2.5f;     // 白色描边宽度，0 表示关闭
    float startOpacity  = 0.9f;     // 起点不透明度
    float fadeHoldMs    = 150.0f;   // 成功点亮高光与视觉暂留保持 150ms，确保高光脉冲与动作名称清晰醒目
    float fadeOutMs     = 180.0f;   // 随后 180ms 平滑自然淡出，总耗时约 330ms，干脆优雅
    uint32_t lineColor  = 0x7C3AED; // 线条颜色 (RGB, 紫色)
    uint32_t resultBg   = 0x000000; // 结果背景色
    float resultFontSize = 24.0f;   // 结果文字大小（会按窗口 DPI 缩放）
};

class GestureTrailOverlay {
public:
    static GestureTrailOverlay& instance();

    /// 初始化（在主线程调用，启动专用渲染管线）
    bool initialize(HINSTANCE hInstance);

    /// 关闭（销毁窗口和资源）
    void shutdown();

    /// 开始新的一次手势轨迹
    void beginTrail();

    /// 添加轨迹点
    void addPoint(float x, float y);

    /// 实时更新当前手势识别到的动作名称（按键回显风格）
    void setLiveAction(const std::string& actionText);

    /// 设置当前手势是否已命中识别动作（未命中时显示灰色，命中时显示主题/设置颜色）
    void setRecognized(bool recognized);

    /// 结束手势轨迹，显示识别结果（如 "← 后退"）
    void endTrail(const std::string& resultText = "");

    /// 创建 Direct2D 资源 (按需初始化)
    bool createD2DResources();

    /// 释放 D2D 资源与大尺寸表面 (深度释放内存)
    void releaseD2DResources();

    /// 清空画布 (提交全透明帧)
    void clearCanvas();

    /// 立即隐藏
    void hide();

    /// 注入按键前暂时让出 TOPMOST，避免覆盖层挡住目标窗口取得前台。
    void yieldZOrderForInput();

    /// 新一笔轨迹开始时把覆盖层拉回 TOPMOST 组。
    void raiseZOrderForDraw();

    /// 重新根据全局配置与主题加载画笔颜色
    void reloadThemeColors();

    /// 设置样式
    void setStyle(const TrailStyle& style);

    /// 是否正在显示
    bool isVisible() const { return m_visible.load(std::memory_order_relaxed); }

    /// DirectComposition 合成器是否就绪
    bool isCompositorReady() const { return m_compositorReady; }

    /// 获取高精度 QPC 节拍器引用 (供 F10 刷新率跟踪与外部诊断接入)
    GesturePacer& pacer() noexcept { return m_pacer; }
    const GesturePacer& pacer() const noexcept { return m_pacer; }

    /// 渲染循环当前是否处于活跃节拍/呈现状态 (F13 审计断言)
    bool isRenderLoopActive() const noexcept { return m_renderLoopActive.load(std::memory_order_relaxed); }

    /// 计算指定轨迹点集的外接脏矩形（含画笔线宽、柔光光晕与抗锯齿裕量）
    RECT computeTrailDirtyRect(
        const std::vector<TrailPoint>& points,
        int originX,
        int originY,
        int surfaceW,
        int surfaceH) const noexcept;

    /// 获取累积手势脏矩形 (供测试与外部诊断断言)
    RECT accumulatedStrokeDirtyRect() const noexcept { return m_accumulatedStrokeDirtyRect; }

private:
    GestureTrailOverlay() = default;
    ~GestureTrailOverlay() = default;
    GestureTrailOverlay(const GestureTrailOverlay&) = delete;
    GestureTrailOverlay& operator=(const GestureTrailOverlay&) = delete;

    /// 创建 Layered Window (在渲染线程内部调用)
    bool createOverlayWindow(HINSTANCE hInstance);
    bool updateTextFormat(float dpiScale);

    /// 渲染一帧。成功提交到屏幕后返回 true。
    bool render();

    /// 按轨迹包围盒调整分层窗口与硬件表面，支持原子离屏双缓冲无闪烁渲染
    bool fitSurface(int left, int top, int right, int bottom,
                    const std::vector<TrailPoint>& points,
                    bool isRecognized,
                    bool& alreadyRendered);
    bool recreateBitmapLocked(int x, int y, int width, int height);
    bool presentLayeredLocked(HWND hwnd, HDC memDC, int x, int y, int width, int height);
    void clearCanvasLocked();

    // ── DirectComposition GPU 显存直通硬件合成管线 ──
    bool ensureCompositorLocked();
    /// 预分配全虚拟屏 DirectComposition 双缓冲硬件表面 (0 SetWindowPos, 0 CreateSurface)
    bool preallocateTrailSurfacesLocked(int width, int height);
    /// 响应系统显示器拓扑变更 (WM_DISPLAYCHANGE 冷路径)
    void handleDisplayChange();
    void releaseCompositorSurfacesLocked();
    void releaseCompositorLocked();

    bool presentToastLocked(const std::string& resultText, bool recognized, bool excessive,
                            int toastCenterX, int toastCenterY, float toastScale,
                            float pulseIntensity = 0.0f);
    void hideToastWindow();

    struct GpuSurfaceContext {
        Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
        Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> renderTarget;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> lineBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glowBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outlineBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> headCoreBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> excDotBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> flashBrush;

        void reset() {
            flashBrush.Reset();
            excDotBrush.Reset();
            textBrush.Reset();
            borderBrush.Reset();
            bgBrush.Reset();
            headCoreBrush.Reset();
            outlineBrush.Reset();
            glowBrush.Reset();
            lineBrush.Reset();
            renderTarget.Reset();
            dxgiSurface.Reset();
            surface.Reset();
        }
    };

    bool renderTrailToSurfaceLocked(
        GpuSurfaceContext& ctx,
        const std::vector<TrailPoint>& points,
        bool isRecognized,
        float fadeAlpha,
        int originX,
        int originY,
        const RECT* pDirtyRect = nullptr);

    bool renderToastToSurfaceLocked(
        GpuSurfaceContext& ctx,
        const std::string& resultText,
        bool isRecognized,
        bool excessive,
        int toastW,
        int toastH,
        float toastScale,
        float fadeAlpha,
        float pulseIntensity = 0.0f);

    /// 硬件级更新 Visual 不透明度并提交 DWM，0 次 CPU/GPU 重绘
    void applyVisualOpacityLocked(float alpha);
    void resetVisualOpacityLocked();

    /// 统一图元渲染核心 (供 DComp 与 GDI 降级管线复用)
    void drawTrailGeometryDirect(
        ID2D1RenderTarget* rt,
        ID2D1SolidColorBrush* lineBrush,
        ID2D1SolidColorBrush* glowBrush,
        ID2D1SolidColorBrush* outlineBrush,
        ID2D1SolidColorBrush* headCoreBrush,
        const std::vector<TrailPoint>& points,
        bool isRecognized,
        float fadeAlpha,
        int originX,
        int originY);

    void drawTrailGeometry(ID2D1RenderTarget* rt,
                           const std::vector<TrailPoint>& points,
                           bool isRecognized,
                           float fadeAlpha);
    void drawToastContentDirect(
        ID2D1RenderTarget* rt,
        GpuSurfaceContext& ctx,
        const std::string& resultText,
        bool isRecognized,
        bool excessive,
        int toastW,
        int toastH,
        float toastScale,
        float fadeAlpha,
        float pulseIntensity = 0.0f);

    /// 根据当前配置重建 D2D 画笔。调用方必须已持有 m_renderMutex，且渲染目标有效。
    void applyThemeColorsLocked();

    /// 在生命周期退场时执行真正的隐藏与资源释放，禁止从鼠标钩子调用
    void applyHideOverlayState();

    /// 释放 D2D 资源。调用方必须已持有 m_renderMutex。
    void releaseD2DResourcesLocked();

    /// 确保 D2D 笔触样式就绪（设备无关资源，双重保险防平整裁切）
    bool ensureStrokeStyleLocked();

    /// 专用高优先级异步渲染循环 (独立持有 HWND 并驱动 DirectComposition)
    void renderLoop(std::stop_token stopToken, HANDLE readyEvent);

    /// 窗口过程
    static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE m_hInstance = nullptr;
    HWND m_helperOwnerHwnd = nullptr;
    HWND m_hwnd = nullptr;
    TrailStyle m_style;
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_fading{false};
    std::atomic<bool> m_hideRequested{false};
    std::atomic<bool> m_wantVisible{false};
    std::atomic<bool> m_strokeSurfaceLive{false};
    std::atomic<bool> m_dismissPrevious{false};
    std::atomic<bool> m_wakeRender{false};
    std::atomic<uint64_t> m_trailEpoch{0};
    std::atomic<uint64_t> m_hideEpoch{0};
    GesturePacer m_pacer;
    uint64_t m_fadeEpoch = 0;
    std::atomic<float> m_fadeAlpha{1.0f};
    std::atomic<float> m_pulseIntensity{0.0f};
    DWORD m_fadeStartTick = 0;
    std::atomic<bool> m_fadeClockStarted{false};
    std::atomic<bool> m_themeDirty{true};
    std::atomic<bool> m_zOrderYielded{false};
    std::atomic<bool> m_zOrderYieldRequested{false};
    std::atomic<bool> m_zOrderRaiseRequested{false};

    // F13: 渲染循环活跃状态标志（原子无锁），用于 1000Hz 输入唤醒合并与节流
    std::atomic<bool> m_renderLoopActive{false};

    // 专用异步渲染引擎
    std::jthread m_renderThread;
    HANDLE m_wakeEvent = nullptr;
    std::atomic<bool> m_renderRequested{false};
    void wakeRender() noexcept;

    // 虚拟屏幕原点
    int m_originX = 0;
    int m_originY = 0;
    int m_virtualX = 0;
    int m_virtualY = 0;
    int m_virtualW = 0;
    int m_virtualH = 0;
    std::atomic<DWORD> m_lastWakeTick{0};

    // 轨迹数据与无锁 SPSC 环形队列缓冲
    tools3000::core::SpscRingBuffer<TrailPoint, 2048> m_pointQueue;
    std::mutex m_trailMutex;
    std::vector<TrailPoint> m_points;
    std::string m_resultText;

    // 缓存平滑贝塞尔曲线 PathGeometry 以提升渲染性能
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> m_smoothPathGeometry;

    // Direct2D 资源与并发安全渲染锁
    std::mutex m_renderMutex;

    uint64_t m_loggedPresentEpoch = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    HDC m_memoryDC = nullptr;
    HBITMAP m_memoryBitmap = nullptr;
    HBITMAP m_oldBitmap = nullptr;
    void* m_memoryBits = nullptr;
    int m_memoryPitch = 0;
    int m_width = 0;
    int m_height = 0;

    // DirectComposition 硬件合成设备与视口表面
    bool m_compositorReady = false;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_trailDcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_rootDcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_trailDcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> m_trailEffectGroup;
    GpuSurfaceContext m_frontCtx;
    Microsoft::WRL::ComPtr<IDCompositionSurface> m_trailDcompSurface;
    int m_trailDcompW = 0;
    int m_trailDcompH = 0;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_toastDcompVisual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> m_toastEffectGroup;
    GpuSurfaceContext m_toastFrontCtx;
    GpuSurfaceContext m_toastBackCtx;
    Microsoft::WRL::ComPtr<IDCompositionSurface> m_toastDcompSurface;
    int m_toastDcompW = 0;
    int m_toastDcompH = 0;
    int m_toastOriginX = 0;
    int m_toastOriginY = 0;
    int m_lastToastOriginX = -9999;
    int m_lastToastOriginY = -9999;
    int m_lastToastW = 0;
    int m_lastToastH = 0;
    float m_dpiScale = 1.0f;
    float m_textScale = 0.0f;

    // 累积手势包围盒 (Cumulative Bounding Box Pipeline)
    RECT m_accumulatedStrokeDirtyRect{ 0, 0, 0, 0 };

    bool m_isLightTheme = false;
    bool m_isDarkTheme = true;
    tools3000::core::AccentColorRGB m_cachedTrailRgb{ 0.231f, 0.510f, 0.965f };
    
    std::atomic<bool> m_isRecognized{false};

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_lineBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_glowBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_greyLineBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_greyGlowBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_headCoreBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_outlineBrush;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_strokeStyle;
};

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_TRAILOVERLAY_H
