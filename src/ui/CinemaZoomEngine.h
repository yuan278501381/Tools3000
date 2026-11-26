#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CinemaZoomEngine — 双击 Ctrl 电影级平滑交互放大引擎 (Issue #34)
//
// 架构特性:
//   1. Windows Magnification API (MagSetFullscreenTransform) 动态加载：
//      硬件合成级无损全屏缩放，放大期间全程支持真实鼠标点击、拖拽与键盘打字。
//   2. 3 通道解析临界阻尼物理弹簧 (zoom, centerX, centerY) + DwmFlush VSync 驱动：
//      220ms 丝滑推近、180ms 利落拉回、0 过冲不眩晕；闲置期自动 MagUninitialize 零占用。
//   3. 中心柔和死区跟焦 + 边界钳制：
//      光标微调点击时画面稳如泰山，越过死区平滑平移跟焦，多显示器坐标安全兼容。
//   4. 活跃期支持 Ctrl+滚轮 / Ctrl+± 无级调节倍率 (1.25x ~ 4.0x)，普通滚轮与按键全透传。
//   5. 五重防卡死复位铁甲：Esc/双击 Ctrl 退出、锁屏/休眠/显示器拓扑变更复位、
//      程序退出强制复位、SetUnhandledExceptionFilter 崩溃前无锁紧急复位 1.0x。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_CINEMAZOOMENGINE_H
#define TOOLS3000_UI_CINEMAZOOMENGINE_H

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace tools3000::ui {

struct CinemaZoomConfig {
    float targetZoom = 2.0f;         // 默认放大倍率 (1.25 ~ 4.0)
    bool followCursor = true;        // 放大期间镜头平滑跟随光标
    bool wheelAdjust = true;         // 放大期间允许 Ctrl+滚轮 无级调节倍率
};

class CinemaZoomEngine {
public:
    struct SpringState {
        double pos = 0.0;
        double vel = 0.0;
    };

    struct ViewportOrigin {
        int x = 0;
        int y = 0;
    };

    static CinemaZoomEngine& instance();

    /// 纯静态数学算法与策略辅助（零 Win32 副作用，支持离线单元测试）
    static float clampZoomLevel(float level) noexcept;
    static float stepZoomWheel(float currentTarget, int wheelDelta) noexcept;
    static SpringState stepCriticallyDampedSpring(
        const SpringState& state, double target, double omega, double dtSec) noexcept;
    static ViewportOrigin computeZoomViewportOrigin(
        double focusX, double focusY, float zoomLevel,
        int primaryW, int primaryH, const RECT& clampBounds) noexcept;
    static std::pair<double, double> applyCursorDeadzone(
        double currentCenterX, double currentCenterY,
        double cursorX, double cursorY,
        float zoomLevel, int primaryW, int primaryH,
        const RECT& clampBounds, double deadzoneRatio = 0.24) noexcept;

    /// 切换电影级平滑放大：闲置或正在拉回时立即推近；正在推近或放大中时平滑拉回
    /// @return true 表示切换后进入放大态，false 表示进入拉回/关闭态或系统不支持 Magnification API
    bool toggle(POINT anchorPt, const CinemaZoomConfig& config);

    /// 显式启动/更新放大
    bool activate(POINT anchorPt, const CinemaZoomConfig& config);

    /// 退出放大：immediate=false 丝滑拉回 1.0x；immediate=true 立即硬复位（锁屏/休眠/二次 Esc）
    void dismiss(bool immediate = false);

    /// 动态同步设置（不强制触发放大；仅在放大期间即时生效）
    void updateConfig(const CinemaZoomConfig& config);

    /// 放大活跃期间通过滚轮或快捷键调节目标倍率
    void adjustZoomByWheel(int wheelDelta);
    void adjustZoomByStep(float delta);
    void resetZoomToConfigured();

    /// 当前是否处于推近/放大/拉回动画中（单次原子读，钩子热路径安全）
    bool isActive() const noexcept;

    /// 当前是否处于放大态且允许拦截 Ctrl+滚轮（钩子热路径单次原子读，<1ns）
    bool isWheelAdjustActive() const noexcept;

    /// 绑定放大完全退出到 1.0x 后的回调（用于同步收起伴随光环）
    void setOnSessionEndedCallback(std::function<void()> cb);

    /// 显示器分辨率/DPI 变更、锁屏、休眠或退出时的紧急硬复位
    void handleSystemInterruption(const char* reason);

    /// 关闭引擎、复位 1.0x 并安全汇合专用动画线程
    void shutdown();

private:
    CinemaZoomEngine() = default;
    ~CinemaZoomEngine();

    CinemaZoomEngine(const CinemaZoomEngine&) = delete;
    CinemaZoomEngine& operator=(const CinemaZoomEngine&) = delete;

    enum class Phase : uint8_t {
        Idle = 0,
        ZoomingIn = 1,
        Active = 2,
        ZoomingOut = 3
    };

    bool ensureMagnificationApiLoaded() noexcept;
    void installCrashGuardOnce() noexcept;
    void workerLoop(POINT anchorPt, std::string traceId);
    static LONG WINAPI crashResetFilter(EXCEPTION_POINTERS* ep);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::jthread m_workerThread;
    bool m_workerRunning = false;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_wheelAdjustActive{false};
    std::atomic<bool> m_immediateReset{false};
    std::atomic<uint8_t> m_phase{static_cast<uint8_t>(Phase::Idle)};

    CinemaZoomConfig m_config{};
    float m_configuredBaseZoom = 2.0f;
    float m_targetZoom = 2.0f;
    POINT m_anchorPt{0, 0};

    std::function<void()> m_onSessionEnded;
    std::mutex m_callbackMutex;

    // Magnification.dll 动态函数指针（按需加载，不硬链接导入表）
    using MagInitializeFn = BOOL(WINAPI*)();
    using MagUninitializeFn = BOOL(WINAPI*)();
    using MagSetFullscreenTransformFn = BOOL(WINAPI*)(float, int, int);

    HMODULE m_magModule = nullptr;
    MagInitializeFn m_magInitialize = nullptr;
    MagUninitializeFn m_magUninitialize = nullptr;
    MagSetFullscreenTransformFn m_magSetFullscreenTransform = nullptr;
    bool m_magLoadAttempted = false;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_CINEMAZOOMENGINE_H
