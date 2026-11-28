// ─────────────────────────────────────────────────────────────────────────────
// MouseFeedbackOverlayBase.cpp — 硬件加速鼠标反馈可视化覆盖层基类实现 (世界级跨系统零掉帧架构)
// ─────────────────────────────────────────────────────────────────────────────

#include "core/mouse/MouseFeedbackOverlayBase.h"
#include "core/mouse/MouseStreamCore.h"
#include "core/utils/WinUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/logger/Logger.h"

#include <dwmapi.h>
#include <dxgi1_3.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace tools3000::core {

namespace {

/// 动态加载 avrt.dll MMCSS（零静态依赖，Windows Server / 精简版缺失 avrt.dll 时优雅静默跳过）
class ScopedMmcssBoost final {
public:
    void enter() noexcept {
        if (m_handle != nullptr) return;
        ensureAvrtLoaded();
        if (s_setFn) {
            DWORD taskIdx = 0;
            m_handle = s_setFn(L"Games", &taskIdx);
            if (!m_handle) {
                taskIdx = 0;
                m_handle = s_setFn(L"DisplayPostProcessing", &taskIdx);
            }
        }
    }

    void leave() noexcept {
        if (m_handle != nullptr && s_revertFn) {
            s_revertFn(m_handle);
        }
        m_handle = nullptr;
    }

    ~ScopedMmcssBoost() noexcept { leave(); }

private:
    using SetMmThreadFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using RevertMmThreadFn = BOOL(WINAPI*)(HANDLE);

    static void ensureAvrtLoaded() noexcept {
        static std::once_flag s_once;
        std::call_once(s_once, []() noexcept {
            HMODULE mod = LoadLibraryW(L"avrt.dll");
            if (mod) {
                s_setFn = reinterpret_cast<SetMmThreadFn>(GetProcAddress(mod, "AvSetMmThreadCharacteristicsW"));
                s_revertFn = reinterpret_cast<RevertMmThreadFn>(GetProcAddress(mod, "AvRevertMmThreadCharacteristics"));
            }
        });
    }

    HANDLE m_handle = nullptr;
    inline static SetMmThreadFn s_setFn = nullptr;
    inline static RevertMmThreadFn s_revertFn = nullptr;
};

/// 跨系统高精度等待定时器（Win10 1803+ 自动使用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 零全局时钟污染；
/// Win10 1607 / Windows Server 2016 自动降级标准 WaitableTimer + 仅活跃期动态配对 timeBeginPeriod(1)/timeEndPeriod(1)）
class AdaptiveFrameTimer final {
public:
    AdaptiveFrameTimer() noexcept {
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (m_timer) {
            m_highResNative = true;
        } else {
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            m_highResNative = false;
        }
    }

    ~AdaptiveFrameTimer() noexcept {
        setActivePeriodBoost(false);
        if (m_timer) {
            CloseHandle(m_timer);
            m_timer = nullptr;
        }
    }

    void setActivePeriodBoost(bool active) noexcept {
        if (m_highResNative) return;
        if (active && !m_periodBoosted) {
            if (timeBeginPeriod(1) == TIMERR_NOERROR) {
                m_periodBoosted = true;
            }
        } else if (!active && m_periodBoosted) {
            timeEndPeriod(1);
            m_periodBoosted = false;
        }
    }

    /// 精确睡眠 remainingUs 微秒，期间可被 controlEvent 或消息队列唤醒
    void waitRemainingUs(int64_t remainingUs, HANDLE controlEvent) noexcept {
        if (remainingUs <= 200) return;
        if (m_timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -(remainingUs * 10LL); // 100ns 相对时间单位
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE handles[2] = {m_timer, controlEvent};
                const DWORD count = controlEvent ? 2u : 1u;
                const DWORD timeoutMs = static_cast<DWORD>(std::clamp<int64_t>((remainingUs / 1000LL) + 8LL, 2LL, 100LL));
                MsgWaitForMultipleObjectsEx(count, handles, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                CancelWaitableTimer(m_timer);
                return;
            }
        }
        const DWORD fallbackMs = static_cast<DWORD>(std::clamp<int64_t>(remainingUs / 1000LL, 1LL, 33LL));
        if (controlEvent) {
            MsgWaitForMultipleObjectsEx(1, &controlEvent, fallbackMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            Sleep(fallbackMs);
        }
    }

private:
    HANDLE m_timer = nullptr;
    bool m_highResNative = false;
    bool m_periodBoosted = false;
};

int quantizeSurfaceDim(int value, int step) noexcept {
    if (value <= 0) return step;
    return ((value + step - 1) / step) * step;
}

} // namespace

MouseFeedbackOverlayBase::MouseFeedbackOverlayBase() = default;

MouseFeedbackOverlayBase::~MouseFeedbackOverlayBase() {
    shutdownBase();
}

bool MouseFeedbackOverlayBase::isCompositorReady() const noexcept {
    return m_compositorReady.load(std::memory_order_acquire);
}

bool MouseFeedbackOverlayBase::isVisible() const noexcept {
    return m_visible.load(std::memory_order_relaxed);
}

bool MouseFeedbackOverlayBase::hasPresentSurface() const noexcept {
    return m_hasLiveSurface.load(std::memory_order_acquire);
}

void MouseFeedbackOverlayBase::setLazySurfaceIdleReleaseMsForTest(uint32_t releaseMs) noexcept {
    m_lazySurfaceIdleReleaseMs.store(releaseMs, std::memory_order_release);
}

uint32_t MouseFeedbackOverlayBase::lazySurfaceIdleReleaseMs() const noexcept {
    return m_lazySurfaceIdleReleaseMs.load(std::memory_order_acquire);
}

HWND MouseFeedbackOverlayBase::hwnd() const noexcept {
    return m_hwnd.load(std::memory_order_acquire);
}

OverlayPresentTier MouseFeedbackOverlayBase::presentTier() const noexcept {
    return m_presentTier.load(std::memory_order_acquire);
}

int MouseFeedbackOverlayBase::maxTextureDimension() const noexcept {
    return m_maxTextureDim;
}

bool MouseFeedbackOverlayBase::isSoftwareWarp() const noexcept {
    return m_usingWarp;
}

bool MouseFeedbackOverlayBase::needsContinuousAnimation() const {
    return !isOverlayIdle();
}

void MouseFeedbackOverlayBase::onRecreateDeviceResources() {}

void MouseFeedbackOverlayBase::onReleaseDeviceResources() {}

double MouseFeedbackOverlayBase::queryEffectiveRefreshHz(bool remoteSession, bool usingWarp) noexcept {
    if (remoteSession || usingWarp) {
        return 60.0;
    }

    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) &&
        ti.rateCompose.uiDenominator > 0 && ti.rateCompose.uiNumerator > 0) {
        const double hz = static_cast<double>(ti.rateCompose.uiNumerator) /
                          static_cast<double>(ti.rateCompose.uiDenominator);
        if (std::isfinite(hz) && hz >= 24.0 && hz <= 360.0) {
            return std::clamp(hz, 30.0, 240.0);
        }
    }

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24) {
        return std::clamp(static_cast<double>(dm.dmDisplayFrequency), 30.0, 240.0);
    }

    return 60.0;
}

int64_t MouseFeedbackOverlayBase::computeAntiSpinRemainingUs(int64_t elapsedUs, int64_t targetPeriodUs) noexcept {
    if (targetPeriodUs <= 0) return 0;
    const int64_t safeElapsed = (std::max<int64_t>)(0, elapsedUs);
    // 仅当 DwmFlush / FrameLatencyWaitable 异常瞬时返回（实耗 < 15% 周期，例如 RDP / 锁屏 / Server 2016 假返回）时补足余量防 100% CPU 暴走；
    // 绝不可在真实 VSync 相位恢复（如等待 3~8ms 回到 VBlank 边沿）时额外睡眠，否则会破坏硬件 VSync 相位锁并诱发 30FPS 半帧振荡。
    if (safeElapsed < (targetPeriodUs * 15) / 100) {
        return targetPeriodUs - safeElapsed;
    }
    return 0;
}

bool MouseFeedbackOverlayBase::shouldInvokeDwmFlushAfterWaitable(
    int64_t waitedUs, int64_t sinceLastFrameUs, int64_t targetPeriodUs) noexcept {
    if (targetPeriodUs <= 0) return false;
    const int64_t safeWaited = (std::max<int64_t>)(0, waitedUs);
    const int64_t safeSince = (std::max<int64_t>)(0, sinceLastFrameUs);
    // 若 Waitable 本身已对齐了本次 VSync（等耗 >= 45% 周期），或连同本帧渲染自上帧已耗时 >= 55% 周期，
    // 则绝不能再叠加一次 DwmFlush()，否则跨过 VSync 边缘会强行空等整个下一周期导致帧率腰斩 (60->30 / 144->72)。
    if (safeWaited >= (targetPeriodUs * 45) / 100 || safeSince >= (targetPeriodUs * 55) / 100) {
        return false;
    }
    return true;
}

OverlayFrameMetrics MouseFeedbackOverlayBase::getFrameMetrics() const {
    OverlayFrameMetrics out{};
    std::vector<int64_t> samples;
    {
        std::lock_guard lock(m_metricsMutex);
        out.framesPresented = m_framesPresentedCount;
        out.surfaceRebuilds = m_surfaceRebuildCount;
        out.setWindowPosCalls = m_setWindowPosCount;
        out.deviceResets = m_deviceResetCount;
        out.presentTier = m_presentTier.load(std::memory_order_acquire);
        out.usingWarp = m_usingWarp;
        out.remoteSession = (GetSystemMetrics(SM_REMOTESESSION) != 0);
        out.targetHz = m_lastMeasuredTargetHz;
        samples.reserve(m_frameSampleCount);
        for (size_t i = 0; i < m_frameSampleCount; ++i) {
            samples.push_back(m_frameDeltasUs[i]);
        }
    }
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        auto pct = [&](double p) -> double {
            const size_t idx = std::min<size_t>(
                samples.size() - 1,
                static_cast<size_t>(std::floor(p * static_cast<double>(samples.size() - 1))));
            return static_cast<double>(samples[idx]) / 1000.0;
        };
        out.p50FrameMs = pct(0.50);
        out.p95FrameMs = pct(0.95);
        out.p99FrameMs = pct(0.99);

        const double expectedMs = (out.targetHz > 1.0) ? (1000.0 / out.targetHz) : 16.667;
        const double dropThresholdUs = expectedMs * 1.5 * 1000.0;
        size_t dropped = 0;
        for (int64_t d : samples) {
            if (static_cast<double>(d) > dropThresholdUs) {
                ++dropped;
            }
        }
        out.dropRatePercent = (static_cast<double>(dropped) * 100.0) / static_cast<double>(samples.size());
    }
    return out;
}

void MouseFeedbackOverlayBase::resetFrameMetrics() {
    std::lock_guard lock(m_metricsMutex);
    m_frameSampleCount = 0;
    m_frameSampleHead = 0;
    m_lastPresentedQpcUs = 0;
    m_framesPresentedCount = 0;
    m_surfaceRebuildCount = 0;
    m_setWindowPosCount = 0;
    m_deviceResetCount = 0;
}

void MouseFeedbackOverlayBase::recordFramePresentedLocked(double targetHz) noexcept {
    const int64_t nowUs = MouseStreamThrottle::monotonicMicros();
    std::lock_guard lock(m_metricsMutex);
    ++m_framesPresentedCount;
    m_lastMeasuredTargetHz = targetHz;
    if (m_lastPresentedQpcUs > 0) {
        const int64_t deltaUs = nowUs - m_lastPresentedQpcUs;
        // 忽略跨空闲休眠（> 250ms）的帧间隔，仅统计连续活跃动画帧
        if (deltaUs > 0 && deltaUs < 250000LL) {
            m_frameDeltasUs[m_frameSampleHead] = deltaUs;
            m_frameSampleHead = (m_frameSampleHead + 1) % kFrameSampleCapacity;
            if (m_frameSampleCount < kFrameSampleCapacity) {
                ++m_frameSampleCount;
            }
        }
    }
    m_lastPresentedQpcUs = nowUs;
}

void MouseFeedbackOverlayBase::wakeRender() noexcept {
    m_wakeRender.store(true, std::memory_order_release);
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
}

void MouseFeedbackOverlayBase::requestViewport(const MouseViewportRect& viewport) noexcept {
    {
        std::lock_guard lock(m_baseMutex);
        m_pendingViewport = viewport;
        m_viewportDirty.store(true, std::memory_order_release);
    }
    wakeRender();
}

void MouseFeedbackOverlayBase::requestHide() noexcept {
    m_hideRequested.store(true, std::memory_order_release);
    const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (hwnd && GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, nullptr)) {
        std::lock_guard lock(m_baseMutex);
        applyHideOverlayStateLocked();
        m_hideRequested.store(false, std::memory_order_release);
    } else {
        if (m_controlEvent) SetEvent(m_controlEvent);
        wakeRender();
    }
}

void MouseFeedbackOverlayBase::yieldZOrderForInput() noexcept {
    m_zOrderYieldRequested.store(true, std::memory_order_release);
    if (m_controlEvent) SetEvent(m_controlEvent);
    wakeRender();
}

void MouseFeedbackOverlayBase::raiseZOrderForDraw() noexcept {
    m_zOrderRaiseRequested.store(true, std::memory_order_release);
    if (m_controlEvent) SetEvent(m_controlEvent);
    wakeRender();
}

bool MouseFeedbackOverlayBase::initializeBase(HINSTANCE hInstance, const wchar_t* windowClassName,
                                              const wchar_t* helperOwnerName, bool lazyCompositor) {
    m_hInstance = hInstance ? hInstance : GetModuleHandleW(nullptr);
    m_windowClassName = windowClassName ? windowClassName : L"Tools3000_MouseFeedbackOverlay";
    m_helperOwnerName = helperOwnerName ? helperOwnerName : L"Tools3000_MouseHelperOwner";
    m_lazyCompositor = lazyCompositor;
    m_idleSince = {};
    m_hiddenSinceMs.store(0, std::memory_order_release);
    m_visualContentDetached = false;
    m_oversizedSince = {};

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wakeEvent) {
        LOG_ERROR("MouseFeedbackOverlayBase: Failed to create wake event, error={}", GetLastError());
        return false;
    }

    m_controlEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_controlEvent) {
        LOG_ERROR("MouseFeedbackOverlayBase: Failed to create control event, error={}", GetLastError());
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
        return false;
    }

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("MouseFeedbackOverlayBase: Failed to create ready event, error={}", GetLastError());
        CloseHandle(m_controlEvent);
        m_controlEvent = nullptr;
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
        return false;
    }

    m_renderThread = std::jthread([this, readyEvent](std::stop_token st) {
        baseRenderLoop(st, readyEvent);
    });

    if (m_renderThread.native_handle()) {
        SetThreadPriority(m_renderThread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }

    WaitForSingleObject(readyEvent, 5000);
    CloseHandle(readyEvent);

    if (!m_hwnd || (!m_lazyCompositor && !m_compositorReady)) {
        LOG_ERROR("MouseFeedbackOverlayBase: Render thread window or compositor initialization failed");
        shutdownBase();
        return false;
    }

    LOG_INFO("MouseFeedbackOverlayBase: Pipeline initialized successfully");
    return true;
}

void MouseFeedbackOverlayBase::shutdownBase() {
    if (m_renderThread.joinable()) {
        m_renderThread.request_stop();
        if (m_controlEvent) SetEvent(m_controlEvent);
        wakeRender();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_renderThread, 3000);
    }

    if (m_controlEvent) {
        CloseHandle(m_controlEvent);
        m_controlEvent = nullptr;
    }

    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }

    m_visible.store(false, std::memory_order_release);
    LOG_INFO("MouseFeedbackOverlayBase: Pipeline shutdown complete");
}

LRESULT CALLBACK MouseFeedbackOverlayBase::baseWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MouseFeedbackOverlayBase::baseRenderLoop(std::stop_token st, HANDLE readyEvent) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = baseWindowProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = m_windowClassName.c_str();
    RegisterClassExW(&wc);

    m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(wc.hInstance, m_helperOwnerName.c_str());

    // 首选 WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT：
    // - WS_EX_NOREDIRECTIONBITMAP 免去 4K 窗口约 33MB GDI 重定向显存，像素完全由 DirectComposition Flip SwapChain 硬件直出；
    // - WS_EX_LAYERED 必须与 WS_EX_TRANSPARENT 共存，否则 Windows 内核 (win32k) 不会跳过跨线程/跨进程鼠标与触控命中测试，导致拦截下方窗口点击！
    // 若老驱动/精简环境不支持该组合，自动回退传统 WS_EX_LAYERED | WS_EX_TRANSPARENT。
    m_usingNoRedirectionBitmap = true;
    HWND createdHwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        m_windowClassName.c_str(), L"",
        WS_POPUP,
        0, 0, 1, 1,
        m_helperOwnerHwnd, nullptr, wc.hInstance, nullptr
    );
    if (!createdHwnd) {
        m_usingNoRedirectionBitmap = false;
        createdHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            m_windowClassName.c_str(), L"",
            WS_POPUP,
            0, 0, 1, 1,
            m_helperOwnerHwnd, nullptr, wc.hInstance, nullptr
        );
    }
    m_hwnd.store(createdHwnd, std::memory_order_release);

    if (createdHwnd) {
        SetWindowLongPtrW(createdHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(createdHwnd, false);
        SetWindowDisplayAffinity(createdHwnd, WDA_NONE);
        if (!m_lazyCompositor) {
            std::lock_guard lock(m_baseMutex);
            initCompositorLocked();
        }
    }

    SetEvent(readyEvent);

    if (!createdHwnd || (!m_lazyCompositor && !m_compositorReady)) {
        LOG_ERROR("MouseFeedbackOverlayBase: Render thread failed to create window or initialize compositor");
        return;
    }

    static constexpr auto kLazyCompositorIdleRelease = std::chrono::milliseconds(kDefaultLazyCompositorIdleReleaseMs);
    ScopedMmcssBoost mmcss;
    AdaptiveFrameTimer frameTimer;
    int64_t lastAnimPacingEndUs = 0;
    double cachedRefreshHz = 60.0;
    int64_t lastRefreshProbeUs = 0;
    bool wasAnimating = false;

    MSG msg;
    while (!st.stop_requested()) {
        const bool idleNow = isOverlayIdle();
        const bool animatingNow = !idleNow && needsContinuousAnimation();

        if (animatingNow) {
            m_idleSince = {};
            m_hiddenSinceMs.store(0, std::memory_order_release);
            if (!wasAnimating) {
                mmcss.enter();
                frameTimer.setActivePeriodBoost(true);
                wasAnimating = true;
                lastAnimPacingEndUs = MouseStreamThrottle::monotonicMicros();
            }

            const int64_t nowUs = MouseStreamThrottle::monotonicMicros();
            const bool remoteSession = (GetSystemMetrics(SM_REMOTESESSION) != 0);
            if (lastRefreshProbeUs == 0 || (nowUs - lastRefreshProbeUs) >= 2000000LL) {
                cachedRefreshHz = queryEffectiveRefreshHz(remoteSession, m_usingWarp);
                lastRefreshProbeUs = nowUs;
                {
                    std::lock_guard mLock(m_metricsMutex);
                    m_lastMeasuredTargetHz = cachedRefreshHz;
                }
            }
            const int64_t targetPeriodUs = static_cast<int64_t>(std::llround(1000000.0 / cachedRefreshHz));

            // 1. 单源硬件 VSync 对齐（严禁 Waitable 与 DwmFlush 无条件串行叠加导致 60->30 / 144->72 帧率腰斩！）
            bool dwmFlushFailed = false;
            if (!remoteSession && !m_usingWarp) {
                bool needDwmFlush = true;
                if (m_frameLatencyWaitable) {
                    const int64_t waitStartUs = MouseStreamThrottle::monotonicMicros();
                    HANDLE waitHandles[2] = {m_frameLatencyWaitable, m_controlEvent};
                    const DWORD waitCount = m_controlEvent ? 2u : 1u;
                    const DWORD latTimeout = static_cast<DWORD>(std::clamp<int64_t>((targetPeriodUs / 1000LL) + 6LL, 8LL, 40LL));
                    WaitForMultipleObjects(waitCount, waitHandles, FALSE, latTimeout);
                    const int64_t waitEndUs = MouseStreamThrottle::monotonicMicros();
                    const int64_t waitedUs = waitEndUs - waitStartUs;
                    const int64_t sinceLastFrameUs = waitEndUs - lastAnimPacingEndUs;
                    needDwmFlush = shouldInvokeDwmFlushAfterWaitable(waitedUs, sinceLastFrameUs, targetPeriodUs);
                }
                if (needDwmFlush &&
                    !st.stop_requested() &&
                    !m_hideRequested.load(std::memory_order_acquire) &&
                    !m_zOrderYieldRequested.load(std::memory_order_acquire)) {
                    dwmFlushFailed = FAILED(DwmFlush());
                }
            }

            // 2. QPC 防空转兜底：RDP / WARP / 锁屏 / Server 2016 异常 DwmFlush 瞬时返回或失败时精准补足剩余周期，绝不 100% CPU 暴走
            if (!st.stop_requested() &&
                !m_hideRequested.load(std::memory_order_acquire) &&
                !m_zOrderYieldRequested.load(std::memory_order_acquire)) {
                const int64_t afterFlushUs = MouseStreamThrottle::monotonicMicros();
                const int64_t elapsedUs = afterFlushUs - lastAnimPacingEndUs;
                const int64_t remainUs = (remoteSession || m_usingWarp || dwmFlushFailed)
                    ? ((elapsedUs < targetPeriodUs) ? (targetPeriodUs - elapsedUs) : 0LL)
                    : computeAntiSpinRemainingUs(elapsedUs, targetPeriodUs);
                if (remainUs > 0) {
                    frameTimer.waitRemainingUs(remainUs, m_controlEvent);
                }
            }
            lastAnimPacingEndUs = MouseStreamThrottle::monotonicMicros();
        } else {
            if (wasAnimating) {
                mmcss.leave();
                frameTimer.setActivePeriodBoost(false);
                wasAnimating = false;
                lastAnimPacingEndUs = 0;
            }

            DWORD waitMs = INFINITE;
            if (idleNow) {
                const auto now = std::chrono::steady_clock::now();

                // 1. 隐藏后 3 秒滞后释放 SwapChain / DCompSurface + DXGI Trim：
                //    既免去高频连点水波纹/鼠标轨迹每秒反复销毁重建 SwapChain 引发的核显驱动提交碎片，
                //    又确保 3 秒无新特效后迅速归还显存表面。
                if (m_hasLiveSurface.load(std::memory_order_acquire)) {
                    const int64_t nowMs = static_cast<int64_t>(GetTickCount64());
                    int64_t hiddenSince = m_hiddenSinceMs.load(std::memory_order_acquire);
                    if (hiddenSince == 0) {
                        hiddenSince = nowMs;
                        m_hiddenSinceMs.store(hiddenSince, std::memory_order_release);
                    }
                    const int64_t surfaceLimitMs = static_cast<int64_t>(
                        m_lazySurfaceIdleReleaseMs.load(std::memory_order_acquire));
                    const int64_t elapsedSurfaceMs = (nowMs >= hiddenSince) ? (nowMs - hiddenSince) : surfaceLimitMs;
                    if (elapsedSurfaceMs >= surfaceLimitMs) {
                        std::lock_guard lock(m_baseMutex);
                        if (!m_visible.load(std::memory_order_acquire) &&
                            (m_swapChain || m_dcompSurface)) {
                            releaseDcompSurfaceLocked();
                            if (m_d3dDevice) {
                                Microsoft::WRL::ComPtr<IDXGIDevice3> dxgi3;
                                if (SUCCEEDED(m_d3dDevice.As(&dxgi3)) && dxgi3) {
                                    dxgi3->Trim();
                                }
                            }
                        }
                        m_hiddenSinceMs.store(0, std::memory_order_release);
                    } else {
                        const DWORD remainSurfaceMs = static_cast<DWORD>(
                            (std::max<int64_t>)(50, surfaceLimitMs - elapsedSurfaceMs));
                        waitMs = (std::min)(waitMs, remainSurfaceMs);
                    }
                } else {
                    m_hiddenSinceMs.store(0, std::memory_order_release);
                }

                // 2. 闲置 60 秒后彻底释放 D3D11/D2D/DComp 设备并收缩工作集（保持原有逻辑不变）
                if (m_lazyCompositor && m_compositorReady.load(std::memory_order_acquire)) {
                    if (m_idleSince == std::chrono::steady_clock::time_point{}) {
                        m_idleSince = now;
                    }
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_idleSince);
                    const auto limitMs = std::chrono::duration_cast<std::chrono::milliseconds>(kLazyCompositorIdleRelease);
                    if (elapsed >= limitMs) {
                        releaseIdleCompositor();
                        m_idleSince = {};
                        m_hiddenSinceMs.store(0, std::memory_order_release);
                        waitMs = INFINITE;
                    } else {
                        const DWORD remainCompMs = static_cast<DWORD>(
                            (std::max<int64_t>)(1000, (limitMs - elapsed).count()));
                        waitMs = (std::min)(waitMs, remainCompMs);
                    }
                } else {
                    m_idleSince = {};
                }
            } else {
                // StaticHolding 态：覆盖层常亮静止，INFINITE 深度挂起 0% CPU，待鼠标移动/按键事件即时唤醒
                m_idleSince = {};
                m_hiddenSinceMs.store(0, std::memory_order_release);
                waitMs = INFINITE;
            }

            HANDLE waitHandles[2] = {m_wakeEvent, m_controlEvent};
            DWORD handleCount = 0;
            if (m_wakeEvent) waitHandles[handleCount++] = m_wakeEvent;
            if (m_controlEvent) waitHandles[handleCount++] = m_controlEvent;
            (void)MsgWaitForMultipleObjectsEx(
                handleCount,
                handleCount > 0 ? waitHandles : nullptr,
                waitMs,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE
            );
            m_wakeRender.store(false, std::memory_order_release);
        }

        if (st.stop_requested()) break;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                mmcss.leave();
                frameTimer.setActivePeriodBoost(false);
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_zOrderYieldRequested.exchange(false, std::memory_order_acq_rel)) {
            const HWND curHwnd = m_hwnd.load(std::memory_order_acquire);
            if (curHwnd && IsWindow(curHwnd) && IsWindowVisible(curHwnd)) {
                SetWindowPos(curHwnd, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_zOrderRaiseRequested.exchange(false, std::memory_order_acq_rel)) {
            const HWND curHwnd = m_hwnd.load(std::memory_order_acquire);
            if (curHwnd && IsWindow(curHwnd)) {
                SetWindowPos(curHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_hideRequested.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard lock(m_baseMutex);
            applyHideOverlayStateLocked();
            continue;
        }

        if (m_viewportDirty.exchange(false, std::memory_order_acq_rel)) {
            MouseViewportRect vp;
            {
                std::lock_guard lock(m_baseMutex);
                vp = m_pendingViewport;
            }
            applyViewportLocked(vp);
        }

        onRenderTick();
    }

    mmcss.leave();
    frameTimer.setActivePeriodBoost(false);

    std::lock_guard lock(m_baseMutex);
    releaseCompositorLocked();
    if (HWND curHwnd = m_hwnd.exchange(nullptr, std::memory_order_acq_rel)) {
        DestroyWindow(curHwnd);
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
}

bool MouseFeedbackOverlayBase::initCompositorLocked() {
    if (m_compositorReady) return true;
    const HWND curHwnd = m_hwnd.load(std::memory_order_acquire);
    if (!curHwnd) return false;

    // 熔断护栏：若 5 秒内连续发生 >=3 次 GPU 驱动崩溃，短暂暂停重建避免死循环
    const auto now = std::chrono::steady_clock::now();
    if (m_circuitBreakerUntil != std::chrono::steady_clock::time_point{} && now < m_circuitBreakerUntil) {
        return false;
    }

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    m_usingWarp = false;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    if (FAILED(hr)) {
        m_usingWarp = true;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
        if (FAILED(hr)) {
            LOG_ERROR("MouseFeedbackOverlayBase: D3D11CreateDevice failed: hr=0x{:X}", hr);
            return false;
        }
    }
    m_d3dDevice = d3d;
    m_maxTextureDim = (featureLevel >= D3D_FEATURE_LEVEL_11_0)
        ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: As(IDXGIDevice) failed: hr=0x{:X}", hr);
        return false;
    }

    // 获取 IDXGIFactory2 用于 CreateSwapChainForComposition（获取失败时平滑降级 Tier 3 IDCompositionSurface）
    m_dxgiFactory2.Reset();
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) && adapter) {
            (void)adapter->GetParent(IID_PPV_ARGS(m_dxgiFactory2.GetAddressOf()));
        }
    }

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp;
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcomp.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: DCompositionCreateDevice failed: hr=0x{:X}", hr);
        return false;
    }
    m_dcompDevice = dcomp;

    hr = m_dcompDevice->CreateTargetForHwnd(curHwnd, TRUE, m_dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateTargetForHwnd failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_dcompDevice->CreateVisual(m_dcompVisual.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateVisual failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: SetRoot failed: hr=0x{:X}", hr);
        return false;
    }

    D2D1_FACTORY_OPTIONS opt{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, opt, m_d2dFactory1.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: D2D1CreateFactory failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_d2dFactory1->CreateDevice(dxgiDevice.Get(), m_d2dDevice.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateDevice failed: hr=0x{:X}", hr);
        return false;
    }

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateDeviceContext failed: hr=0x{:X}", hr);
        return false;
    }

    m_compositorReady.store(true, std::memory_order_release);
    onRecreateDeviceResources();
    return true;
}

void MouseFeedbackOverlayBase::releaseCompositorLocked() {
    onReleaseDeviceResources();
    releaseDcompSurfaceLocked();
    if (m_dcompDevice) {
        m_dcompDevice->Commit();
    }
    m_frameTargetBitmap.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory1.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_dxgiFactory2.Reset();
    if (m_d3dDevice) {
        Microsoft::WRL::ComPtr<IDXGIDevice3> dxgi3;
        if (SUCCEEDED(m_d3dDevice.As(&dxgi3)) && dxgi3) {
            dxgi3->Trim();
        }
    }
    m_d3dDevice.Reset();
    m_presentTier.store(OverlayPresentTier::None, std::memory_order_release);
    m_compositorReady.store(false, std::memory_order_release);
}

void MouseFeedbackOverlayBase::releaseIdleCompositor() {
    {
        std::lock_guard lock(m_baseMutex);
        releaseCompositorLocked();
    }
    tools3000::core::WinUtils::trimWorkingSet();
}

bool MouseFeedbackOverlayBase::createSwapChainForSizeLocked(int width, int height) {
    if (!m_dxgiFactory2 || !m_d3dDevice || !m_dcompVisual) return false;

    // 尝试 Tier 1 (FLIP_DISCARD + FRAME_LATENCY_WAITABLE_OBJECT)，不兼容的老 WDDM / 虚拟显卡自动回落 Tier 2 (FLIP_SEQUENTIAL)
    struct TierSpec {
        OverlayPresentTier tier;
        DXGI_SWAP_EFFECT swapEffect;
        UINT flags;
    };
    const TierSpec candidates[2] = {
        {OverlayPresentTier::SwapChainWaitable, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT},
        {OverlayPresentTier::SwapChainSequential, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 0u},
    };

    for (const auto& cand : candidates) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Stereo = FALSE;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = cand.swapEffect;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags = cand.flags;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> sc;
        HRESULT hr = m_dxgiFactory2->CreateSwapChainForComposition(
            m_d3dDevice.Get(), &desc, nullptr, sc.GetAddressOf());
        if (FAILED(hr) || !sc) {
            continue;
        }

        HANDLE waitable = nullptr;
        if (cand.tier == OverlayPresentTier::SwapChainWaitable) {
            Microsoft::WRL::ComPtr<IDXGISwapChain2> sc2;
            if (SUCCEEDED(sc.As(&sc2)) && sc2) {
                (void)sc2->SetMaximumFrameLatency(1);
                waitable = sc2->GetFrameLatencyWaitableObject();
            }
        }

        hr = m_dcompVisual->SetContent(sc.Get());
        if (FAILED(hr)) {
            if (waitable) CloseHandle(waitable);
            continue;
        }

        if (m_frameLatencyWaitable) {
            CloseHandle(m_frameLatencyWaitable);
            m_frameLatencyWaitable = nullptr;
        }
        m_dcompSurface.Reset();
        m_swapChain = std::move(sc);
        m_frameLatencyWaitable = waitable;
        m_swapChainFlags = cand.flags;
        m_surfaceWidth = width;
        m_surfaceHeight = height;
        m_requestedWidth = width;
        m_requestedHeight = height;
        m_dcompNeedsCommit = true;
        m_visualContentDetached = false;
        m_hiddenSinceMs.store(0, std::memory_order_release);
        m_presentTier.store(cand.tier, std::memory_order_release);
        m_hasDcompSurface.store(true, std::memory_order_release);
        m_hasLiveSurface.store(true, std::memory_order_release);
        {
            std::lock_guard mLock(m_metricsMutex);
            ++m_surfaceRebuildCount;
        }
        return true;
    }
    return false;
}

bool MouseFeedbackOverlayBase::createFallbackDcompSurfaceLocked(int width, int height) {
    if (!m_dcompDevice || !m_dcompVisual) return false;
    const int allocW = (std::min)(m_maxTextureDim, quantizeSurfaceDim(width, 64));
    const int allocH = (std::min)(m_maxTextureDim, quantizeSurfaceDim(height, 64));

    Microsoft::WRL::ComPtr<IDCompositionSurface> newSurface;
    HRESULT hr = m_dcompDevice->CreateSurface(
        static_cast<UINT>(allocW),
        static_cast<UINT>(allocH),
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        newSurface.GetAddressOf()
    );
    if (FAILED(hr) || !newSurface) {
        LOG_ERROR("MouseFeedbackOverlayBase: CreateSurface failed: w={}, h={}, hr=0x{:X}", allocW, allocH, hr);
        return false;
    }

    hr = m_dcompVisual->SetContent(newSurface.Get());
    if (FAILED(hr)) {
        LOG_ERROR("MouseFeedbackOverlayBase: SetContent(DCompSurface) failed: hr=0x{:X}", hr);
        return false;
    }

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    m_swapChain.Reset();
    m_swapChainFlags = 0;
    m_dcompSurface = std::move(newSurface);
    m_surfaceWidth = allocW;
    m_surfaceHeight = allocH;
    m_requestedWidth = width;
    m_requestedHeight = height;
    m_oversizedSince = {};
    m_dcompNeedsCommit = true;
    m_visualContentDetached = false;
    m_hiddenSinceMs.store(0, std::memory_order_release);
    m_presentTier.store(OverlayPresentTier::DcompSurfaceFallback, std::memory_order_release);
    m_hasDcompSurface.store(true, std::memory_order_release);
    m_hasLiveSurface.store(true, std::memory_order_release);
    {
        std::lock_guard mLock(m_metricsMutex);
        ++m_surfaceRebuildCount;
    }
    return true;
}

bool MouseFeedbackOverlayBase::ensurePresentSurfaceLocked(int width, int height) {
    if (!m_compositorReady && !initCompositorLocked()) return false;
    if (width <= 0 || height <= 0) return false;

    const int clampedW = (std::min)(width, m_maxTextureDim);
    const int clampedH = (std::min)(height, m_maxTextureDim);
    const auto tier = m_presentTier.load(std::memory_order_acquire);

    // Tier 1 / Tier 2 SwapChain 零销毁原位复用或平滑 ResizeBuffers
    if (m_swapChain && (tier == OverlayPresentTier::SwapChainWaitable || tier == OverlayPresentTier::SwapChainSequential)) {
        m_hiddenSinceMs.store(0, std::memory_order_release);
        if (m_visualContentDetached && m_dcompVisual) {
            (void)m_dcompVisual->SetContent(m_swapChain.Get());
            m_visualContentDetached = false;
            m_dcompNeedsCommit = true;
        }
        m_hasDcompSurface.store(true, std::memory_order_release);
        if (m_surfaceWidth == clampedW && m_surfaceHeight == clampedH) {
            m_requestedWidth = clampedW;
            m_requestedHeight = clampedH;
            return true;
        }
        if (m_d2dContext) {
            m_d2dContext->SetTarget(nullptr);
        }
        m_frameTargetBitmap.Reset();
        HRESULT hr = m_swapChain->ResizeBuffers(
            2,
            static_cast<UINT>(clampedW),
            static_cast<UINT>(clampedH),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            m_swapChainFlags);
        if (SUCCEEDED(hr)) {
            m_surfaceWidth = clampedW;
            m_surfaceHeight = clampedH;
            m_requestedWidth = clampedW;
            m_requestedHeight = clampedH;
            m_dcompNeedsCommit = true;
            {
                std::lock_guard mLock(m_metricsMutex);
                ++m_surfaceRebuildCount;
            }
            return true;
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            return false;
        }
        // ResizeBuffers 失败则释放并重新构建
        releaseDcompSurfaceLocked();
    }

    // Tier 3 高水位滞回复用（仅在不够大或持续 >4x 过大超过 2s 时重建，避免逐帧抖动）
    if (m_dcompSurface && tier == OverlayPresentTier::DcompSurfaceFallback) {
        m_hiddenSinceMs.store(0, std::memory_order_release);
        if (m_visualContentDetached && m_dcompVisual) {
            (void)m_dcompVisual->SetContent(m_dcompSurface.Get());
            m_visualContentDetached = false;
            m_dcompNeedsCommit = true;
        }
        m_hasDcompSurface.store(true, std::memory_order_release);
        const bool fits = (clampedW <= m_surfaceWidth && clampedH <= m_surfaceHeight);
        if (fits) {
            const int64_t surfaceArea = static_cast<int64_t>(m_surfaceWidth) * static_cast<int64_t>(m_surfaceHeight);
            const int64_t reqArea = static_cast<int64_t>(clampedW) * static_cast<int64_t>(clampedH);
            if (surfaceArea > reqArea * 4 && surfaceArea > (512LL * 512LL)) {
                const auto now = std::chrono::steady_clock::now();
                if (m_oversizedSince == std::chrono::steady_clock::time_point{}) {
                    m_oversizedSince = now;
                } else if (now - m_oversizedSince >= std::chrono::seconds(2)) {
                    return createFallbackDcompSurfaceLocked(clampedW, clampedH);
                }
            } else {
                m_oversizedSince = {};
            }
            m_requestedWidth = clampedW;
            m_requestedHeight = clampedH;
            return true;
        }
        return createFallbackDcompSurfaceLocked(clampedW, clampedH);
    }

    // 首次分配：优先 SwapChain Tier 1 -> Tier 2 -> Tier 3 DCompSurface
    if (createSwapChainForSizeLocked(clampedW, clampedH)) {
        return true;
    }
    return createFallbackDcompSurfaceLocked(clampedW, clampedH);
}

bool MouseFeedbackOverlayBase::ensureDcompSurfaceLocked(int width, int height) {
    return ensurePresentSurfaceLocked(width, height);
}

void MouseFeedbackOverlayBase::releaseDcompSurfaceLocked() {
    if (m_d2dContext) {
        m_d2dContext->SetTarget(nullptr);
    }
    m_frameTargetBitmap.Reset();
    if (m_dcompVisual) {
        m_dcompVisual->SetContent(nullptr);
    }
    if (m_dcompDevice) {
        m_dcompDevice->Commit();
    }
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    m_swapChain.Reset();
    m_swapChainFlags = 0;
    m_dcompSurface.Reset();
    m_presentTier.store(OverlayPresentTier::None, std::memory_order_release);
    m_hasDcompSurface.store(false, std::memory_order_release);
    m_hasLiveSurface.store(false, std::memory_order_release);
    m_visualContentDetached = false;
    m_hiddenSinceMs.store(0, std::memory_order_release);
    m_surfaceWidth = 0;
    m_surfaceHeight = 0;
    m_requestedWidth = 0;
    m_requestedHeight = 0;
    m_dcompNeedsCommit = false;
    m_frameInProgress = false;
}

void MouseFeedbackOverlayBase::applyViewportLocked(const MouseViewportRect& viewport) {
    const HWND curHwnd = m_hwnd.load(std::memory_order_acquire);
    if (!curHwnd) return;
    if (m_currentViewport == viewport && m_visible.load(std::memory_order_relaxed)) {
        return;
    }
    m_currentViewport = viewport;

    SetWindowPos(
        curHwnd,
        HWND_TOPMOST,
        viewport.x,
        viewport.y,
        viewport.width,
        viewport.height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
    );
    m_visible.store(true, std::memory_order_release);
    m_hiddenSinceMs.store(0, std::memory_order_release);
    {
        std::lock_guard mLock(m_metricsMutex);
        ++m_setWindowPosCount;
    }
}

void MouseFeedbackOverlayBase::ensureViewportBeforeDrawLocked(const MouseViewportRect& viewport) {
    applyViewportLocked(viewport);
}

bool MouseFeedbackOverlayBase::beginFrameLocked(int width, int height, ID2D1DeviceContext** outContext, POINT* surfaceOffset) {
    if (width <= 0 || height <= 0) return false;
    if (!m_compositorReady.load(std::memory_order_acquire) && !initCompositorLocked()) {
        return false;
    }

    auto handleDeviceLossAndRetry = [&](HRESULT errHr) -> bool {
        LOG_WARN("MouseFeedbackOverlayBase: GPU device lost/reset (hr=0x{:X}), rebuilding compositor",
                 static_cast<uint32_t>(errHr));
        const auto now = std::chrono::steady_clock::now();
        if (m_resetWindowStart == std::chrono::steady_clock::time_point{} ||
            (now - m_resetWindowStart) > std::chrono::seconds(5)) {
            m_resetWindowStart = now;
            m_recentDeviceResets = 1;
        } else {
            ++m_recentDeviceResets;
        }
        {
            std::lock_guard mLock(m_metricsMutex);
            ++m_deviceResetCount;
        }
        releaseCompositorLocked();
        if (m_recentDeviceResets >= 3) {
            LOG_ERROR("MouseFeedbackOverlayBase: device reset circuit breaker triggered (3 resets in 5s), pausing 3s");
            m_circuitBreakerUntil = now + std::chrono::seconds(3);
            return false;
        }
        return initCompositorLocked();
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensurePresentSurfaceLocked(width, height)) {
            if (attempt == 0 && handleDeviceLossAndRetry(DXGI_ERROR_DEVICE_RESET)) {
                continue;
            }
            return false;
        }
        if (!m_d2dContext) return false;

        const auto tier = m_presentTier.load(std::memory_order_acquire);
        POINT offset{0, 0};
        Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurface;
        HRESULT hr = E_FAIL;

        if (m_swapChain && (tier == OverlayPresentTier::SwapChainWaitable || tier == OverlayPresentTier::SwapChainSequential)) {
            hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(dxgiSurface.GetAddressOf()));
        } else if (m_dcompSurface) {
            hr = m_dcompSurface->BeginDraw(
                nullptr,
                __uuidof(IDXGISurface),
                reinterpret_cast<void**>(dxgiSurface.GetAddressOf()),
                &offset
            );
        }

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            if (attempt == 0 && handleDeviceLossAndRetry(hr)) {
                continue;
            }
            return false;
        }
        if (FAILED(hr) || !dxgiSurface) {
            LOG_WARN("MouseFeedbackOverlayBase: acquire frame surface failed: hr=0x{:X}", static_cast<uint32_t>(hr));
            return false;
        }

        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        m_frameTargetBitmap.Reset();
        hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bp, m_frameTargetBitmap.GetAddressOf());
        if (FAILED(hr) || !m_frameTargetBitmap) {
            if (tier == OverlayPresentTier::DcompSurfaceFallback && m_dcompSurface) {
                m_dcompSurface->EndDraw();
            }
            if (attempt == 0 && (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) &&
                handleDeviceLossAndRetry(hr)) {
                continue;
            }
            LOG_ERROR("MouseFeedbackOverlayBase: CreateBitmapFromDxgiSurface failed: hr=0x{:X}", static_cast<uint32_t>(hr));
            return false;
        }

        m_d2dContext->SetTarget(m_frameTargetBitmap.Get());
        m_d2dContext->BeginDraw();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0.0f));
        m_frameInProgress = true;

        if (surfaceOffset) *surfaceOffset = offset;
        if (outContext) *outContext = m_d2dContext.Get();
        return true;
    }

    return false;
}

bool MouseFeedbackOverlayBase::endFrameAndPresentLocked() {
    if (!m_compositorReady || !m_d2dContext || !m_frameInProgress) return false;
    m_frameInProgress = false;

    HRESULT hr = m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
    m_frameTargetBitmap.Reset();

    const auto tier = m_presentTier.load(std::memory_order_acquire);
    HRESULT presentHr = S_OK;
    if (m_swapChain && (tier == OverlayPresentTier::SwapChainWaitable || tier == OverlayPresentTier::SwapChainSequential)) {
        if (SUCCEEDED(hr)) {
            presentHr = m_swapChain->Present(0, 0);
            if (SUCCEEDED(presentHr) && m_dcompNeedsCommit && m_dcompDevice) {
                m_dcompDevice->Commit();
                m_dcompNeedsCommit = false;
            }
        }
    } else if (m_dcompSurface) {
        m_dcompSurface->EndDraw();
        if (SUCCEEDED(hr) && m_dcompDevice) {
            presentHr = m_dcompDevice->Commit();
            m_dcompNeedsCommit = false;
        }
    }

    if (SUCCEEDED(hr) && SUCCEEDED(presentHr)) {
        recordFramePresentedLocked(m_lastMeasuredTargetHz);
        return true;
    }

    const HRESULT failHr = FAILED(hr) ? hr : presentHr;
    if (failHr == D2DERR_RECREATE_TARGET || failHr == DXGI_ERROR_DEVICE_REMOVED || failHr == DXGI_ERROR_DEVICE_RESET) {
        LOG_WARN("MouseFeedbackOverlayBase: EndDraw/Present device lost (hr=0x{:X}), releasing compositor",
                 static_cast<uint32_t>(failHr));
        {
            std::lock_guard mLock(m_metricsMutex);
            ++m_deviceResetCount;
        }
        releaseCompositorLocked();
    }
    return false;
}

bool MouseFeedbackOverlayBase::beginDrawLocked(ID2D1DeviceContext** outContext, POINT* surfaceOffset) {
    return beginFrameLocked(m_surfaceWidth > 0 ? m_surfaceWidth : m_currentViewport.width,
                            m_surfaceHeight > 0 ? m_surfaceHeight : m_currentViewport.height,
                            outContext, surfaceOffset);
}

bool MouseFeedbackOverlayBase::endDrawAndCommitLocked() {
    return endFrameAndPresentLocked();
}

void MouseFeedbackOverlayBase::applyHideOverlayStateLocked() {
    const HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (hwnd && IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    }
    // 0ms 立即隐藏窗口并解绑 DComp Visual 防止再次显示瞬间残留旧帧，但保留底层 IDXGISwapChain1 / DCompSurface
    // 3 秒供高频连续点击水波纹与鼠标轨迹零重建复用，消除核显驱动提交内存碎片；连续隐藏满 3 秒后再释放并 DXGI Trim。
    if (m_dcompVisual && !m_visualContentDetached) {
        (void)m_dcompVisual->SetContent(nullptr);
        if (m_dcompDevice) {
            (void)m_dcompDevice->Commit();
        }
        m_visualContentDetached = true;
    }
    m_visible.store(false, std::memory_order_release);
    m_hasDcompSurface.store(false, std::memory_order_release);
    m_currentViewport = MouseViewportRect{};
    if (m_hasLiveSurface.load(std::memory_order_acquire)) {
        if (m_hiddenSinceMs.load(std::memory_order_acquire) == 0) {
            m_hiddenSinceMs.store(static_cast<int64_t>(GetTickCount64()), std::memory_order_release);
        }
    } else {
        m_hiddenSinceMs.store(0, std::memory_order_release);
    }
}

} // namespace tools3000::core
