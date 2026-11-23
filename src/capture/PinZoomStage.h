#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinZoomStage.h — Direct3D11 + Direct2D 1.1 + DirectComposition 零 Resize 硬件缩放舞台
//
// 核心原理 (对标 Rust Picasa Viewer 120Hz/144Hz/240Hz 电影级无级缩放):
//   1. 滚轮缩放期间创建/复用固定尺寸的 DirectComposition 透明 GPU 舞台窗口
//      (WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)，
//      全程 0 次 SetWindowPos、0 次 SwapChain Resize、0 次 SetWindowRgn；
//   2. 严格遵守 FocusAssistAvoidance 红线：任何全屏/跨屏视口物理高度至少比显示器/虚拟桌面小 1px，
//      且附加 NonRudeHWND 属性，绝不触发 Windows 专注助手免打扰；
//   3. 一次性将贴图当前显示画面上传至 GPU ID2D1Bitmap1（超长图自动二维瓦片切分），
//      按真实显示器 VSync (DwmFlush + 高精度 WaitableTimer 兜底) 驱动每帧亚像素浮点绘制；
//   4. 零新增链接库依赖：动态加载 dcomp.dll (DCompositionCreateDevice) 与 winmm.dll (timeBeginPeriod/End)，
//      兼容 x64 与 ARM64，系统不支持时自动返回 false 触发优化后的平滑降级路径。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINZOOMSTAGE_H
#define TOOLS3000_CAPTURE_PINZOOMSTAGE_H

#include "capture/FloatingGlassBar.h"
#include "capture/PinGroupManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tools3000::capture {

/// 贴图缩放舞台安全物理包围盒
struct PinZoomStageBounds {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;

    bool isValid() const noexcept { return width > 0 && height > 0; }
    bool equals(const PinZoomStageBounds& other) const noexcept {
        return left == other.left && top == other.top &&
               width == other.width && height == other.height;
    }
};

/// 单帧硬件缩放绘制参数（物理像素空间，1:1 对齐 PinWindow）
struct PinZoomStageFrameParams {
    double drawLeft = 0.0;
    double drawTop = 0.0;
    double drawWidth = 1.0;
    double drawHeight = 1.0;
    float scale = 1.0f;
    float opacity = 1.0f;
    float cardCornerRadius = 0.0f;
    float dpiScale = 1.0f;
    bool focused = true;
    bool clickThrough = false;
    int groupId = 0;
    std::wstring badgeText;
};

/// 舞台回调（全部在主 UI 线程同步触发）
struct PinZoomStageCallbacks {
    std::function<void()> onVSyncTick;
    std::function<void(WPARAM wParam, LPARAM lParam)> onMouseWheel;
    std::function<void(UINT msg, WPARAM wParam, POINT screenPt)> onInterruptMouse;
    std::function<void()> onDisplayChanged;
};

/// 动态加载 winmm.dll 的 1ms 高精度系统定时器 RAII 守卫（零新增 winmm.lib 链接依赖）
class ScopedHighResTimerPeriod {
public:
    ScopedHighResTimerPeriod() = default;
    ~ScopedHighResTimerPeriod() { unload(); }
    ScopedHighResTimerPeriod(const ScopedHighResTimerPeriod&) = delete;
    ScopedHighResTimerPeriod& operator=(const ScopedHighResTimerPeriod&) = delete;

    void acquire() noexcept {
        if (m_active) return;
        if (!m_winmm) {
            m_winmm = GetModuleHandleW(L"winmm.dll");
            if (!m_winmm) {
                m_winmm = LoadLibraryW(L"winmm.dll");
                m_ownsWinmm = (m_winmm != nullptr);
            }
            if (m_winmm) {
                m_pfnBegin = reinterpret_cast<PfnTimePeriod>(GetProcAddress(m_winmm, "timeBeginPeriod"));
                m_pfnEnd = reinterpret_cast<PfnTimePeriod>(GetProcAddress(m_winmm, "timeEndPeriod"));
            }
        }
        if (m_pfnBegin && m_pfnBegin(1u) == 0u /* TIMERR_NOERROR */) {
            m_active = true;
        }
    }

    void release() noexcept {
        if (m_active && m_pfnEnd) {
            m_pfnEnd(1u);
        }
        m_active = false;
    }

    void unload() noexcept {
        release();
        if (m_ownsWinmm && m_winmm) {
            FreeLibrary(m_winmm);
        }
        m_winmm = nullptr;
        m_ownsWinmm = false;
        m_pfnBegin = nullptr;
        m_pfnEnd = nullptr;
    }

    bool isActive() const noexcept { return m_active; }

private:
    using PfnTimePeriod = UINT(WINAPI*)(UINT);
    HMODULE m_winmm = nullptr;
    bool m_ownsWinmm = false;
    PfnTimePeriod m_pfnBegin = nullptr;
    PfnTimePeriod m_pfnEnd = nullptr;
    bool m_active = false;
};

/// 独立轻量化 VSync 节拍器线程：
/// - 活跃期按 DwmFlush() 精准锁相真实显示器 60/120/144/165/240Hz VSync，
///   仅通过异步 PostMessageW 投递单帧唤醒消息（内置防堆积原子闸门），0 阻塞主线程且数学上无死锁可能；
/// - 遇到 RDP / 无 DWM 导致 DwmFlush 瞬间返回时，自动切换至原生高精度 WaitableTimer 补足帧间隔；
/// - 静止期阻塞于内核事件，保持 0.00% CPU 占用。
class PinZoomVSyncPacer {
public:
    static constexpr UINT kMsgStageVSyncTick = WM_APP + 141;

    PinZoomVSyncPacer() = default;
    ~PinZoomVSyncPacer() { shutdown(); }
    PinZoomVSyncPacer(const PinZoomVSyncPacer&) = delete;
    PinZoomVSyncPacer& operator=(const PinZoomVSyncPacer&) = delete;

    bool ensureStarted() noexcept {
        if (m_running.load(std::memory_order_acquire) && m_thread.joinable()) {
            return true;
        }
        if (!m_wakeEvent) {
            m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!m_wakeEvent) return false;
        }
        if (!m_hiResTimer) {
            m_hiResTimer = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!m_hiResTimer) {
                m_hiResTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            }
        }
        m_running.store(true, std::memory_order_release);
        m_active.store(false, std::memory_order_release);
        m_tickPending.store(false, std::memory_order_release);
        try {
            m_thread = std::thread([this]() { loop(); });
            SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
        } catch (...) {
            m_running.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    uint64_t start(HWND targetHwnd, uint32_t refreshHz) noexcept {
        if (!targetHwnd || !ensureStarted()) return 0;
        const uint64_t nextEpoch = m_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        m_targetHwnd.store(targetHwnd, std::memory_order_release);
        m_refreshHz.store(std::clamp(refreshHz, 60u, 360u), std::memory_order_release);
        m_tickPending.store(false, std::memory_order_release);
        m_active.store(true, std::memory_order_release);
        if (m_wakeEvent) {
            SetEvent(m_wakeEvent);
        }
        return nextEpoch;
    }

    void pause() noexcept {
        m_active.store(false, std::memory_order_release);
        m_epoch.fetch_add(1, std::memory_order_acq_rel);
        m_tickPending.store(false, std::memory_order_release);
        if (m_wakeEvent) {
            SetEvent(m_wakeEvent);
        }
    }

    void acknowledgeTick() noexcept {
        m_tickPending.store(false, std::memory_order_release);
    }

    uint64_t currentEpoch() const noexcept {
        return m_epoch.load(std::memory_order_acquire);
    }

    void shutdown() noexcept {
        m_active.store(false, std::memory_order_release);
        m_running.store(false, std::memory_order_release);
        m_epoch.fetch_add(1, std::memory_order_acq_rel);
        m_targetHwnd.store(nullptr, std::memory_order_release);
        if (m_wakeEvent) {
            SetEvent(m_wakeEvent);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_hiResTimer) {
            CancelWaitableTimer(m_hiResTimer);
            CloseHandle(m_hiResTimer);
            m_hiResTimer = nullptr;
        }
        if (m_wakeEvent) {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }
    }

private:
    void waitPreciseMs(double ms) noexcept {
        const double clampedMs = std::clamp(ms, 0.8, 33.0);
        if (m_hiResTimer && m_wakeEvent) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(clampedMs * 10000.0);
            if (SetWaitableTimer(m_hiResTimer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE handles[2] = {m_wakeEvent, m_hiResTimer};
                WaitForMultipleObjects(2, handles, FALSE, 50);
                return;
            }
        }
        if (m_wakeEvent) {
            WaitForSingleObject(m_wakeEvent, static_cast<DWORD>((std::max)(1.0, std::floor(clampedMs))));
        }
    }

    void loop() noexcept {
        LARGE_INTEGER freqLi{};
        QueryPerformanceFrequency(&freqLi);
        const double qpcFreq = (freqLi.QuadPart > 0) ? static_cast<double>(freqLi.QuadPart) : 10000000.0;

        while (m_running.load(std::memory_order_acquire)) {
            if (!m_active.load(std::memory_order_acquire)) {
                if (m_wakeEvent) {
                    WaitForSingleObject(m_wakeEvent, INFINITE);
                } else {
                    Sleep(16);
                }
                continue;
            }

            const uint64_t epoch = m_epoch.load(std::memory_order_acquire);
            const HWND hwnd = m_targetHwnd.load(std::memory_order_acquire);
            const uint32_t hz = std::clamp(m_refreshHz.load(std::memory_order_acquire), 60u, 360u);
            if (!hwnd) {
                m_active.store(false, std::memory_order_release);
                continue;
            }

            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            const HRESULT hrFlush = DwmFlush();
            QueryPerformanceCounter(&t1);

            const double elapsedMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / qpcFreq;
            const double targetPeriodMs = 1000.0 / static_cast<double>(hz);

            // 若 DwmFlush 失败或未实际等待 VSync (如锁屏/RDP/无 DWM)，退回高精度内核定时器补足周期
            if (FAILED(hrFlush) || elapsedMs < 1.2) {
                const double remMs = (std::max)(1.0, targetPeriodMs - (std::max)(0.0, elapsedMs));
                waitPreciseMs(remMs);
            }

            if (!m_running.load(std::memory_order_acquire) ||
                !m_active.load(std::memory_order_acquire) ||
                m_epoch.load(std::memory_order_acquire) != epoch) {
                continue;
            }

            if (!m_tickPending.exchange(true, std::memory_order_acq_rel)) {
                if (!PostMessageW(hwnd, kMsgStageVSyncTick, static_cast<WPARAM>(epoch), 0)) {
                    m_tickPending.store(false, std::memory_order_release);
                }
            }
        }
    }

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_tickPending{false};
    std::atomic<uint64_t> m_epoch{0};
    std::atomic<HWND> m_targetHwnd{nullptr};
    std::atomic<uint32_t> m_refreshHz{120u};
    HANDLE m_wakeEvent = nullptr;
    HANDLE m_hiResTimer = nullptr;
    std::thread m_thread;
};

/// Direct3D11 + Direct2D 1.1 + DirectComposition 零 Resize 硬件缩放舞台
class PinZoomStage {
public:
    static constexpr const wchar_t* kStageWindowClass = L"Tools3000_PinZoomStage";
    static constexpr uint32_t kMaxStageDimCap = 7680u;
    static constexpr uint64_t kMaxStagePixelBudget = 16777216ULL; // 16M px (~64MB swapchain budget)

    PinZoomStage() = default;
    ~PinZoomStage() { destroy(); }
    PinZoomStage(const PinZoomStage&) = delete;
    PinZoomStage& operator=(const PinZoomStage&) = delete;

    /// 纯数学计算舞台安全物理包围盒（自动适配单屏与跨屏，100% 强制执行 FocusAssistAvoidance -1px 红线）
    static PinZoomStageBounds computeSafeStageBounds(
        const RECT& currentPinRect,
        const RECT& projectedTargetRect,
        const RECT& monitorRect,
        const RECT& virtualRect,
        uint32_t maxTextureDim = 16384u) noexcept {
        const uint32_t safeMaxDim = (std::min)((std::max)(256u, maxTextureDim), kMaxStageDimCap);
        const int monW = (std::max)(1, static_cast<int>(monitorRect.right - monitorRect.left));
        const int monH = (std::max)(1, static_cast<int>(monitorRect.bottom - monitorRect.top));
        const int virtW = (std::max)(1, static_cast<int>(virtualRect.right - virtualRect.left));
        const int virtH = (std::max)(1, static_cast<int>(virtualRect.bottom - virtualRect.top));

        RECT chosen = monitorRect;
        const bool multiMonitorDesktop = (virtW > monW + 8 || virtH > monH + 8);
        const bool pinCrossesMonitor =
            currentPinRect.left < monitorRect.left || currentPinRect.right > monitorRect.right ||
            currentPinRect.top < monitorRect.top || currentPinRect.bottom > monitorRect.bottom ||
            projectedTargetRect.left < monitorRect.left || projectedTargetRect.right > monitorRect.right ||
            projectedTargetRect.top < monitorRect.top || projectedTargetRect.bottom > monitorRect.bottom;

        if (multiMonitorDesktop && pinCrossesMonitor) {
            RECT united = monitorRect;
            united.left = (std::min)({united.left, currentPinRect.left, projectedTargetRect.left});
            united.top = (std::min)({united.top, currentPinRect.top, projectedTargetRect.top});
            united.right = (std::max)({united.right, currentPinRect.right, projectedTargetRect.right});
            united.bottom = (std::max)({united.bottom, currentPinRect.bottom, projectedTargetRect.bottom});

            united.left = (std::max)(united.left, virtualRect.left);
            united.top = (std::max)(united.top, virtualRect.top);
            united.right = (std::min)(united.right, virtualRect.right);
            united.bottom = (std::min)(united.bottom, virtualRect.bottom);

            const int uW = static_cast<int>(united.right - united.left);
            const int uH = static_cast<int>(united.bottom - united.top);
            if (uW > 0 && uH > 0 &&
                static_cast<uint32_t>(uW) <= safeMaxDim &&
                static_cast<uint32_t>(uH) <= safeMaxDim &&
                static_cast<uint64_t>(uW) * static_cast<uint64_t>(uH) <= kMaxStagePixelBudget) {
                chosen = united;
            }
        }

        int outLeft = static_cast<int>(chosen.left);
        int outTop = static_cast<int>(chosen.top);
        int outW = (std::min)(static_cast<int>(safeMaxDim), (std::max)(1, static_cast<int>(chosen.right - chosen.left)));
        int outH = (std::min)(static_cast<int>(safeMaxDim), (std::max)(1, static_cast<int>(chosen.bottom - chosen.top)));

        // FocusAssistAvoidance 红线：舞台物理矩形绝不能覆盖完整单显示器或完整虚拟桌面
        const bool coversFullMonitor =
            (outLeft <= monitorRect.left && outTop <= monitorRect.top &&
             outLeft + outW >= monitorRect.right && outTop + outH >= monitorRect.bottom);
        const bool coversFullVirtual =
            (outLeft <= virtualRect.left && outTop <= virtualRect.top &&
             outLeft + outW >= virtualRect.right && outTop + outH >= virtualRect.bottom);
        if ((coversFullMonitor || coversFullVirtual) && outH > 1) {
            --outH;
        }
        // 二次保险：若宽/高仍精确等于任意单屏全高或虚拟全高，高度再减 1px
        if ((outW == monW && outH == monH) || (outW == virtW && outH == virtH)) {
            if (outH > 1) --outH;
        }

        return PinZoomStageBounds{outLeft, outTop, (std::max)(1, outW), (std::max)(1, outH)};
    }

    /// 探测所在监视器的真实物理刷新率 (Hz)，兼容 60/75/120/144/165/240/360Hz
    static uint32_t queryMonitorRefreshHz(HMONITOR hMon) noexcept {
        uint32_t rawHz = 0;
        if (hMon) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi) && mi.szDevice[0] != L'\0') {
                DEVMODEW dm{};
                dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                    dm.dmDisplayFrequency > 1) {
                    rawHz = static_cast<uint32_t>(dm.dmDisplayFrequency);
                }
            }
        }
        if (rawHz <= 1) {
            DWM_TIMING_INFO timing{};
            timing.cbSize = sizeof(timing);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) &&
                timing.rateRefresh.uiNumerator > 0 && timing.rateRefresh.uiDenominator > 0) {
                const double dwmHz = static_cast<double>(timing.rateRefresh.uiNumerator) /
                                     static_cast<double>(timing.rateRefresh.uiDenominator);
                const auto rounded = static_cast<uint32_t>(std::lround(dwmHz));
                if (rounded > 1) rawHz = rounded;
            }
        }
        return (rawHz <= 1) ? 120u : std::clamp(rawHz, 60u, 360u);
    }

    static void unregisterWindowClass() noexcept {
        if (s_classRegistered.exchange(false, std::memory_order_acq_rel)) {
            UnregisterClassW(kStageWindowClass, GetModuleHandleW(nullptr));
        }
    }

    void setCallbacks(PinZoomStageCallbacks callbacks) noexcept {
        m_callbacks = std::move(callbacks);
    }

    /// 准备舞台窗口、D3D11/D2D/DComp 设备、固定尺寸 SwapChain，并按需上传最新 GPU 位图（零重复上传）
    bool prepare(
        const cv::Mat& displayImage,
        uint64_t imageGeneration,
        const RECT& currentPinRect,
        const RECT& projectedTargetRect,
        HMONITOR monitor) {
        if (displayImage.empty() || displayImage.cols <= 0 || displayImage.rows <= 0) {
            return false;
        }
        if (!loadDCompSymbol()) {
            return false;
        }

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        RECT monitorRect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        if (monitor && GetMonitorInfoW(monitor, &mi)) {
            monitorRect = mi.rcMonitor;
        }
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        const int vh = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        const RECT virtualRect{vx, vy, vx + vw, vy + vh};

        // 先确保设备就绪以获取真实 GPU 最大纹理上限
        if (!ensureCoreDeviceResources()) {
            return false;
        }

        const uint32_t maxTexDim = m_d2dContext
            ? (std::max)(256u, m_d2dContext->GetMaximumBitmapSize())
            : 8192u;
        const PinZoomStageBounds desired = computeSafeStageBounds(
            currentPinRect, projectedTargetRect, monitorRect, virtualRect, maxTexDim);
        if (!desired.isValid()) {
            return false;
        }

        if (!ensureStageWindowAndSwapChain(desired)) {
            return false;
        }

        if (!ensureImageUploaded(displayImage, imageGeneration)) {
            return false;
        }

        m_refreshHz = queryMonitorRefreshHz(monitor);
        return true;
    }

    /// 显示舞台至最顶层（不抢焦点）并启动 VSync 节拍器
    bool beginSession(const PinZoomStageFrameParams& firstFrame) {
        if (!m_hwnd || !m_d2dContext || !m_swapChain || !m_dcompDevice || !m_dcompVisual) {
            return false;
        }

        // 确保视觉树已挂载当前 SwapChain
        if (!m_visualAttached) {
            if (FAILED(m_dcompVisual->SetContent(m_swapChain.Get()))) {
                return false;
            }
            m_visualAttached = true;
        }

        m_active = true;
        updateHitRect(firstFrame);

        // 先提交首帧像素到 SwapChain 再显现窗口，避免出现空白透明过渡帧
        if (!renderFrame(firstFrame)) {
            m_active = false;
            return false;
        }

        SetWindowPos(
            m_hwnd, HWND_TOPMOST,
            m_bounds.left, m_bounds.top, m_bounds.width, m_bounds.height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_visible = true;

        m_pacer.start(m_hwnd, m_refreshHz);
        return true;
    }

    /// 渲染单帧（全程 0 Resize / 0 SetWindowPos / 热路径 0 COM 分配）
    bool renderFrame(const PinZoomStageFrameParams& frame) {
        if (!m_d2dContext || !m_swapChain || !m_dcompDevice || !m_targetBitmap) {
            return false;
        }
        if (!m_bitmap && m_bitmapTiles.empty()) {
            return false;
        }

        updateHitRect(frame);

        const float localLeft = static_cast<float>(frame.drawLeft - static_cast<double>(m_bounds.left));
        const float localTop = static_cast<float>(frame.drawTop - static_cast<double>(m_bounds.top));
        const float drawW = (std::max)(1.0f, static_cast<float>(frame.drawWidth));
        const float drawH = (std::max)(1.0f, static_cast<float>(frame.drawHeight));
        const float localRight = localLeft + drawW;
        const float localBottom = localTop + drawH;
        const D2D1_RECT_F dstRect = D2D1::RectF(localLeft, localTop, localRight, localBottom);

        const float opacity = std::clamp(frame.opacity, 0.1f, 1.0f);
        const float dpiScale = (frame.dpiScale > 0.0f) ? frame.dpiScale : 1.0f;
        const float scaledCardR = (frame.cardCornerRadius > 0.5f)
            ? std::clamp(frame.cardCornerRadius * frame.scale,
                         3.0f, (std::max)(3.0f, (std::min)(drawW, drawH) * 0.5f))
            : 0.0f;

        m_d2dContext->BeginDraw();
        m_d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
        m_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        // 1. 轻柔外阴影（贴合系统 CS_DROPSHADOW 悬浮质感，仅 2 次快速填充）
        if (m_sharedBrush && opacity > 0.25f) {
            const float shScale = opacity;
            const D2D1_RECT_F outerSh = D2D1::RectF(
                localLeft - 3.0f, localTop - 1.0f, localRight + 3.0f, localBottom + 5.0f);
            m_sharedBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.085f * shScale));
            if (scaledCardR > 0.5f) {
                m_d2dContext->FillRoundedRectangle(
                    D2D1::RoundedRect(outerSh, scaledCardR + 3.0f, scaledCardR + 3.0f),
                    m_sharedBrush.Get());
            } else {
                m_d2dContext->FillRectangle(outerSh, m_sharedBrush.Get());
            }

            const D2D1_RECT_F nearSh = D2D1::RectF(
                localLeft - 1.0f, localTop, localRight + 1.0f, localBottom + 2.0f);
            m_sharedBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f * shScale));
            if (scaledCardR > 0.5f) {
                m_d2dContext->FillRoundedRectangle(
                    D2D1::RoundedRect(nearSh, scaledCardR + 1.0f, scaledCardR + 1.0f),
                    m_sharedBrush.Get());
            } else {
                m_d2dContext->FillRectangle(nearSh, m_sharedBrush.Get());
            }
        }

        // 2. 图像主体 + 边框（运动态常规缩放采用硬件单周期 LINEAR，>=3x 切至 NEAREST_NEIGHBOR 清晰呈现像素）
        const bool pixelGridZoom = (frame.scale >= 2.995f);
        const D2D1_INTERPOLATION_MODE interp = pixelGridZoom
            ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_INTERPOLATION_MODE_LINEAR;

        auto drawImageContent = [&](float contentOpacity) {
            if (m_bitmap) {
                m_d2dContext->DrawBitmap(m_bitmap.Get(), &dstRect, contentOpacity, interp, nullptr, nullptr);
            } else if (!m_bitmapTiles.empty() && m_imageWidth > 0 && m_imageHeight > 0) {
                const float sx = drawW / static_cast<float>(m_imageWidth);
                const float sy = drawH / static_cast<float>(m_imageHeight);
                const float stageW = static_cast<float>(m_bounds.width);
                const float stageH = static_cast<float>(m_bounds.height);
                for (const auto& tile : m_bitmapTiles) {
                    if (!tile.bitmap) continue;
                    const float tx0 = localLeft + static_cast<float>(tile.x) * sx;
                    const float ty0 = localTop + static_cast<float>(tile.y) * sy;
                    const float tx1 = localLeft + static_cast<float>(tile.x + tile.width) * sx;
                    const float ty1 = localTop + static_cast<float>(tile.y + tile.height) * sy;
                    if (tx1 <= -2.0f || ty1 <= -2.0f || tx0 >= stageW + 2.0f || ty0 >= stageH + 2.0f) {
                        continue;
                    }
                    const D2D1_RECT_F tileDst = D2D1::RectF(tx0, ty0, tx1, ty1);
                    m_d2dContext->DrawBitmap(tile.bitmap.Get(), &tileDst, contentOpacity, interp, nullptr, nullptr);
                }
            }
        };

        auto drawPinBorder = [&](float borderAlphaMul) {
            if (!m_sharedBrush) return;
            if (frame.focused) {
                const auto themeRgb = FloatingGlassBar::effectiveAccentRGB();
                D2D1_COLOR_F focusCol = (frame.groupId >= PinGroupConstants::kMinGroupId)
                    ? PinGroupPalette::accentColor(frame.groupId, 0.96f)
                    : D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f);
                focusCol.a *= borderAlphaMul;
                m_sharedBrush->SetColor(focusCol);
                const D2D1_RECT_F rc = D2D1::RectF(
                    localLeft + 1.0f, localTop + 1.0f,
                    (std::max)(localLeft + 1.0f, localRight - 1.0f),
                    (std::max)(localTop + 1.0f, localBottom - 1.0f));
                if (scaledCardR > 0.5f) {
                    const float r = (std::max)(2.0f, scaledCardR - 1.0f);
                    m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), m_sharedBrush.Get(), 2.0f);
                } else {
                    m_d2dContext->DrawRectangle(rc, m_sharedBrush.Get(), 2.0f);
                }
            } else if (frame.clickThrough) {
                m_sharedBrush->SetColor(D2D1::ColorF(0.0f, 0.85f, 1.0f, 0.65f * borderAlphaMul));
                const D2D1_RECT_F rc = D2D1::RectF(
                    localLeft + 0.75f, localTop + 0.75f,
                    (std::max)(localLeft + 0.75f, localRight - 0.75f),
                    (std::max)(localTop + 0.75f, localBottom - 0.75f));
                if (scaledCardR > 0.5f) {
                    const float r = (std::max)(2.0f, scaledCardR - 0.75f);
                    m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), m_sharedBrush.Get(), 1.5f);
                } else {
                    m_d2dContext->DrawRectangle(rc, m_sharedBrush.Get(), 1.5f);
                }
            } else if (scaledCardR > 0.5f) {
                m_sharedBrush->SetColor(D2D1::ColorF(0.75f, 0.80f, 0.86f, 0.78f * borderAlphaMul));
                const D2D1_RECT_F rc = D2D1::RectF(
                    localLeft + 0.5f, localTop + 0.5f,
                    (std::max)(localLeft + 0.5f, localRight - 0.5f),
                    (std::max)(localTop + 0.5f, localBottom - 0.5f));
                const float r = (std::max)(2.0f, scaledCardR - 0.5f);
                m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(rc, r, r), m_sharedBrush.Get(), 1.0f);
            } else {
                if (drawW > 3.0f && drawH > 3.0f) {
                    m_sharedBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f * borderAlphaMul));
                    m_d2dContext->DrawRectangle(
                        D2D1::RectF(localLeft + 1.5f, localTop + 1.5f, localRight - 1.5f, localBottom - 1.5f),
                        m_sharedBrush.Get(), 1.0f);
                }
                m_sharedBrush->SetColor(D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.45f * borderAlphaMul));
                m_d2dContext->DrawRectangle(
                    D2D1::RectF(localLeft + 0.5f, localTop + 0.5f,
                                (std::max)(localLeft + 0.5f, localRight - 0.5f),
                                (std::max)(localTop + 0.5f, localBottom - 0.5f)),
                    m_sharedBrush.Get(), 1.0f);
            }
        };

        if (scaledCardR > 0.5f && m_d2dFactory) {
            Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clipGeom;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(dstRect, scaledCardR, scaledCardR),
                    clipGeom.GetAddressOf())) && clipGeom) {
                D2D1_LAYER_PARAMETERS1 layerParams = D2D1::LayerParameters1(
                    dstRect,
                    clipGeom.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(),
                    opacity,
                    nullptr,
                    D2D1_LAYER_OPTIONS1_NONE);
                m_d2dContext->PushLayer(layerParams, nullptr);
                drawImageContent(1.0f);
                drawPinBorder(1.0f);
                m_d2dContext->PopLayer();
            } else {
                drawImageContent(opacity);
                drawPinBorder(opacity);
            }
        } else {
            drawImageContent(opacity);
            drawPinBorder(opacity);
        }

        // 3. 底部居中轻量化黑曜石百分比微晶胶囊（与原 PinWindowRender 风格与定位 1:1 衔接）
        if (!frame.badgeText.empty() && drawW >= 44.0f && drawH >= 24.0f &&
            ensureBadgeTextFormat(dpiScale) && m_badgeTextFormat && m_sharedBrush) {
            const float textW = estimateBadgeTextWidth(frame.badgeText, dpiScale);
            const float pillH = (std::min)(drawH - 4.0f, 24.0f * dpiScale);
            const float pillW = std::clamp(
                textW + 20.0f * dpiScale,
                42.0f * dpiScale,
                (std::max)(42.0f * dpiScale, drawW - 8.0f));
            const float pillLeft = localLeft + (drawW - pillW) * 0.5f;
            const float preferCenterY = localBottom - 20.0f * dpiScale;
            const float pillTop = std::clamp(
                preferCenterY - pillH * 0.5f,
                localTop + 4.0f,
                (std::max)(localTop + 4.0f, localBottom - pillH - 4.0f));
            const D2D1_RECT_F pillRect = D2D1::RectF(
                pillLeft, pillTop, pillLeft + pillW, pillTop + pillH);
            const float pillR = pillH * 0.5f;

            // 外轮廓暗环 + 黑曜石底色 + 微晶内高光环 + 清晰百分比文字（全部复用单支画笔，0 分配）
            m_sharedBrush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.52f * opacity));
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(pillRect.left - 0.6f, pillRect.top + 0.6f,
                                pillRect.right + 0.6f, pillRect.bottom + 1.6f),
                    pillR + 0.6f, pillR + 0.6f),
                m_sharedBrush.Get());

            m_sharedBrush->SetColor(D2D1::ColorF(18.0f / 255.0f, 20.0f / 255.0f, 26.0f / 255.0f, 0.94f * opacity));
            m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(pillRect, pillR, pillR), m_sharedBrush.Get());

            m_sharedBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * opacity));
            m_d2dContext->DrawRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(pillRect.left + 0.5f, pillRect.top + 0.5f,
                                pillRect.right - 0.5f, pillRect.bottom - 0.5f),
                    (std::max)(1.0f, pillR - 0.5f), (std::max)(1.0f, pillR - 0.5f)),
                m_sharedBrush.Get(), 1.0f);

            m_sharedBrush->SetColor(D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.98f * opacity));
            m_d2dContext->DrawTextW(
                frame.badgeText.c_str(),
                static_cast<UINT32>(frame.badgeText.size()),
                m_badgeTextFormat.Get(),
                pillRect,
                m_sharedBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const HRESULT hrEnd = m_d2dContext->EndDraw();
        if (isDeviceLostHr(hrEnd)) {
            LOG_WARN("PinZoomStage: D2D EndDraw reported device lost: hr=0x{:08X}", static_cast<unsigned>(hrEnd));
            return false;
        }
        if (FAILED(hrEnd)) {
            return false;
        }

        // 配合 VSync Pacer 线程，Present(0, 0) 零阻塞直交 DWM Composition，绝不卡死主线程消息泵
        const HRESULT hrPresent = m_swapChain->Present(0, 0);
        if (hrPresent == DXGI_ERROR_WAS_STILL_DRAWING) {
            return true;
        }
        if (isDeviceLostHr(hrPresent)) {
            LOG_WARN("PinZoomStage: DXGI Present reported device lost: hr=0x{:08X}", static_cast<unsigned>(hrPresent));
            return false;
        }
        if (FAILED(hrPresent)) {
            return false;
        }

        const HRESULT hrCommit = m_dcompDevice->Commit();
        return SUCCEEDED(hrCommit);
    }

    /// 暂停节拍并瞬时隐藏舞台，但保留热 GPU 资源供 2.5s 内连续滚轮 0ms 瞬启
    void hideKeepWarm() noexcept {
        m_active = false;
        m_pacer.pause();
        if (m_dcompVisual && m_dcompDevice && m_visualAttached) {
            m_dcompVisual->SetContent(nullptr);
            m_dcompDevice->Commit();
            m_visualAttached = false;
        }
        if (m_hwnd && IsWindow(m_hwnd) && m_visible) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_visible = false;
    }

    /// 仅失效已上传的图像纹理缓存（当原图旋转/反色/标注/译图切换时调用）
    void invalidateUploadedImage() noexcept {
        m_bitmap.Reset();
        m_bitmapTiles.clear();
        m_uploadedGeneration = 0;
        m_uploadedDataPtr = nullptr;
        m_imageWidth = 0;
        m_imageHeight = 0;
    }

    /// 彻底销毁全部 GPU/COM 资源、节拍器线程与舞台 HWND（零显存/句柄泄漏）
    void destroy() noexcept {
        m_active = false;
        m_pacer.shutdown();

        if (m_dcompVisual && m_dcompDevice && m_visualAttached) {
            m_dcompVisual->SetContent(nullptr);
            m_dcompDevice->Commit();
            m_visualAttached = false;
        }

        if (m_d2dContext) {
            m_d2dContext->SetTarget(nullptr);
        }
        invalidateUploadedImage();
        m_sharedBrush.Reset();
        m_badgeTextFormat.Reset();
        m_cachedBadgeDpi = 0.0f;
        m_dwriteFactory.Reset();
        m_targetBitmap.Reset();
        m_d2dContext.Reset();
        m_d2dDevice.Reset();
        m_d2dFactory.Reset();

        m_dcompVisual.Reset();
        m_dcompTarget.Reset();
        m_dcompDevice.Reset();
        m_swapChain.Reset();
        m_d3dContext.Reset();
        m_d3dDevice.Reset();

        if (m_hwnd) {
            HWND hwndToDestroy = std::exchange(m_hwnd, nullptr);
            if (IsWindow(hwndToDestroy)) {
                SetWindowLongPtrW(hwndToDestroy, GWLP_USERDATA, 0);
                DestroyWindow(hwndToDestroy);
            }
        }
        m_visible = false;
        m_bounds = {};

        if (m_ownsDcompDll && m_dcompDll) {
            FreeLibrary(m_dcompDll);
        }
        m_dcompDll = nullptr;
        m_ownsDcompDll = false;
        m_pfnDCompCreate = nullptr;
    }

    bool isActive() const noexcept { return m_active; }
    HWND hwnd() const noexcept { return m_hwnd; }
    uint32_t refreshHz() const noexcept { return m_refreshHz; }
    const PinZoomStageBounds& bounds() const noexcept { return m_bounds; }

private:
    struct StageBitmapTile {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    };

    using PfnDCompositionCreateDevice = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

    static bool isDeviceLostHr(HRESULT hr) noexcept {
        return hr == D2DERR_RECREATE_TARGET ||
               hr == DXGI_ERROR_DEVICE_REMOVED ||
               hr == DXGI_ERROR_DEVICE_RESET;
    }

    bool loadDCompSymbol() noexcept {
        if (m_pfnDCompCreate) return true;
        if (!m_dcompDll) {
            m_dcompDll = GetModuleHandleW(L"dcomp.dll");
            if (!m_dcompDll) {
                m_dcompDll = LoadLibraryW(L"dcomp.dll");
                m_ownsDcompDll = (m_dcompDll != nullptr);
            }
        }
        if (!m_dcompDll) return false;
        m_pfnDCompCreate = reinterpret_cast<PfnDCompositionCreateDevice>(
            GetProcAddress(m_dcompDll, "DCompositionCreateDevice"));
        return m_pfnDCompCreate != nullptr;
    }

    bool ensureCoreDeviceResources() noexcept {
        if (m_d3dDevice && m_d2dFactory && m_d2dDevice && m_d2dContext && m_sharedBrush) {
            return true;
        }

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            m_d3dDevice.ReleaseAndGetAddressOf(),
            nullptr,
            m_d3dContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_d3dDevice) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels, static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                m_d3dDevice.ReleaseAndGetAddressOf(),
                nullptr,
                m_d3dContext.ReleaseAndGetAddressOf());
            if (FAILED(hr) || !m_d3dDevice) {
                LOG_WARN("PinZoomStage: D3D11CreateDevice failed: hr=0x{:08X}", static_cast<unsigned>(hr));
                return false;
            }
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = m_d3dDevice.As(&dxgiDevice);
        if (FAILED(hr) || !dxgiDevice) return false;

        D2D1_FACTORY_OPTIONS opts{};
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &opts,
            reinterpret_cast<void**>(m_d2dFactory.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !m_d2dFactory) return false;

        hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), m_d2dDevice.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_d2dDevice) return false;

        hr = m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_d2dContext) return false;

        m_d2dContext->SetDpi(96.0f, 96.0f);
        m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        hr = m_d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), m_sharedBrush.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_sharedBrush) return false;

        return true;
    }

    bool ensureStageWindowAndSwapChain(const PinZoomStageBounds& desired) noexcept {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        if (!s_classRegistered.load(std::memory_order_acquire)) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = stageWndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
            wc.lpszClassName = kStageWindowClass;
            if (!RegisterClassExW(&wc) && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
                UnregisterClassW(kStageWindowClass, hInst);
                RegisterClassExW(&wc);
            }
            s_classRegistered.store(true, std::memory_order_release);
        }

        const bool hadWindow = (m_hwnd && IsWindow(m_hwnd));
        if (!hadWindow) {
            m_hwnd = CreateWindowExW(
                WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kStageWindowClass, L"",
                WS_POPUP,
                desired.left, desired.top, desired.width, desired.height,
                nullptr, nullptr, hInst, this);
            if (!m_hwnd) {
                LOG_WARN("PinZoomStage: CreateWindowExW failed: err={}", GetLastError());
                return false;
            }
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
            m_bounds = desired;
            m_visible = false;
        }

        // 若已存在 SwapChain 且尺寸完全一致，仅更新窗口原点（例如贴图移到另一块同分辨率副屏），零重建开销
        if (m_swapChain && m_targetBitmap && m_dcompDevice && m_dcompTarget && m_dcompVisual) {
            if (m_bounds.width == desired.width && m_bounds.height == desired.height) {
                if (m_bounds.left != desired.left || m_bounds.top != desired.top) {
                    m_bounds = desired;
                    SetWindowPos(
                        m_hwnd, nullptr, desired.left, desired.top, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return true;
            }
            // 仅在闲置复热且跨到不同分辨率监视器时调整一次 SwapChain（动画进行中绝不调用）
            m_bounds = desired;
            SetWindowPos(
                m_hwnd, nullptr, desired.left, desired.top, desired.width, desired.height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            m_d2dContext->SetTarget(nullptr);
            m_targetBitmap.Reset();
            const HRESULT hrResize = m_swapChain->ResizeBuffers(
                0,
                static_cast<UINT>(desired.width),
                static_cast<UINT>(desired.height),
                DXGI_FORMAT_UNKNOWN,
                0);
            if (SUCCEEDED(hrResize) && bindSwapChainTargetBitmap()) {
                return true;
            }
            // Resize 失败则干净重建 SwapChain 与 DComp 绑定
            m_dcompVisual.Reset();
            m_dcompTarget.Reset();
            m_dcompDevice.Reset();
            m_swapChain.Reset();
            m_visualAttached = false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_d3dDevice.As(&dxgiDevice)) || !dxgiDevice) return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) || !adapter) return false;

        Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()))) || !dxgiFactory) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width = static_cast<UINT>(desired.width);
        scDesc.Height = static_cast<UINT>(desired.height);
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.Stereo = FALSE;
        scDesc.SampleDesc.Count = 1;
        scDesc.SampleDesc.Quality = 0;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.Scaling = DXGI_SCALING_STRETCH;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scDesc.Flags = 0;

        HRESULT hr = dxgiFactory->CreateSwapChainForComposition(
            dxgiDevice.Get(), &scDesc, nullptr, m_swapChain.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_swapChain) {
            LOG_WARN("PinZoomStage: CreateSwapChainForComposition failed: hr=0x{:08X}", static_cast<unsigned>(hr));
            return false;
        }

        hr = m_pfnDCompCreate(
            dxgiDevice.Get(),
            __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(m_dcompDevice.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !m_dcompDevice) {
            LOG_WARN("PinZoomStage: DCompositionCreateDevice failed: hr=0x{:08X}", static_cast<unsigned>(hr));
            return false;
        }

        hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, m_dcompTarget.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_dcompTarget) return false;

        hr = m_dcompDevice->CreateVisual(m_dcompVisual.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_dcompVisual) return false;

        hr = m_dcompVisual->SetContent(m_swapChain.Get());
        if (FAILED(hr)) return false;

        hr = m_dcompTarget->SetRoot(m_dcompVisual.Get());
        if (FAILED(hr)) return false;

        m_visualAttached = true;
        if (!bindSwapChainTargetBitmap()) {
            return false;
        }

        hr = m_dcompDevice->Commit();
        if (FAILED(hr)) return false;

        m_bounds = desired;
        return true;
    }

    bool bindSwapChainTargetBitmap() noexcept {
        if (!m_swapChain || !m_d2dContext) return false;
        Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr) || !backBuffer) return false;

        const D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        hr = m_d2dContext->CreateBitmapFromDxgiSurface(
            backBuffer.Get(), &targetProps, m_targetBitmap.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !m_targetBitmap) return false;

        m_d2dContext->SetTarget(m_targetBitmap.Get());
        return true;
    }

    bool ensureImageUploaded(const cv::Mat& image, uint64_t imageGeneration) {
        if (!m_d2dContext || image.empty()) return false;
        if (imageGeneration != 0 &&
            m_uploadedGeneration == imageGeneration &&
            m_uploadedDataPtr == image.data &&
            m_imageWidth == image.cols &&
            m_imageHeight == image.rows &&
            (m_bitmap || !m_bitmapTiles.empty())) {
            return true;
        }

        cv::Mat bgra;
        if (image.channels() == 3) {
            cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
        } else if (image.channels() == 1) {
            cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
        } else if (image.channels() == 4) {
            bgra = image.clone();
            for (int r = 0; r < bgra.rows; ++r) {
                auto* p = reinterpret_cast<cv::Vec4b*>(bgra.data + static_cast<size_t>(r) * bgra.step[0]);
                for (int c = 0; c < bgra.cols; ++c) {
                    const uchar a = p[c][3];
                    if (a == 0) {
                        p[c][0] = 0;
                        p[c][1] = 0;
                        p[c][2] = 0;
                    } else if (a < 255) {
                        p[c][0] = static_cast<uchar>((p[c][0] * a) / 255);
                        p[c][1] = static_cast<uchar>((p[c][1] * a) / 255);
                        p[c][2] = static_cast<uchar>((p[c][2] * a) / 255);
                    }
                }
            }
        } else {
            return false;
        }

        if (!bgra.isContinuous()) {
            bgra = bgra.clone();
        }

        invalidateUploadedImage();

        const D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        const UINT32 maxBmp = (std::max)(256u, m_d2dContext->GetMaximumBitmapSize());

        if (static_cast<UINT32>(bgra.cols) <= maxBmp && static_cast<UINT32>(bgra.rows) <= maxBmp) {
            const HRESULT hr = m_d2dContext->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                bgra.data,
                static_cast<UINT32>(bgra.step[0]),
                &bmpProps,
                m_bitmap.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr) && m_bitmap) {
                m_uploadedGeneration = imageGeneration;
                m_uploadedDataPtr = image.data;
                m_imageWidth = bgra.cols;
                m_imageHeight = bgra.rows;
                return true;
            }
            m_bitmap.Reset();
        }

        // 超长截图自动二维瓦片切分，完整支持 >8192/16384px 贴图 120Hz 硬件缩放
        const int tileStep = static_cast<int>((std::min)(maxBmp, 4096u));
        for (int ty = 0; ty < bgra.rows; ty += tileStep) {
            const int th = (std::min)(tileStep, bgra.rows - ty);
            for (int tx = 0; tx < bgra.cols; tx += tileStep) {
                const int tw = (std::min)(tileStep, bgra.cols - tx);
                cv::Mat sub = bgra(cv::Rect(tx, ty, tw, th));
                if (!sub.isContinuous()) {
                    sub = sub.clone();
                }
                StageBitmapTile tile;
                tile.x = tx;
                tile.y = ty;
                tile.width = tw;
                tile.height = th;
                const HRESULT hr = m_d2dContext->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(tw), static_cast<UINT32>(th)),
                    sub.data,
                    static_cast<UINT32>(sub.step[0]),
                    &bmpProps,
                    tile.bitmap.GetAddressOf());
                if (FAILED(hr) || !tile.bitmap) {
                    m_bitmapTiles.clear();
                    return false;
                }
                m_bitmapTiles.push_back(std::move(tile));
            }
        }

        if (m_bitmapTiles.empty()) return false;
        m_uploadedGeneration = imageGeneration;
        m_uploadedDataPtr = image.data;
        m_imageWidth = bgra.cols;
        m_imageHeight = bgra.rows;
        return true;
    }

    bool ensureBadgeTextFormat(float dpiScale) noexcept {
        const float safeScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;
        if (!m_dwriteFactory) {
            if (FAILED(DWriteCreateFactory(
                    DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
                return false;
            }
        }
        if (m_badgeTextFormat && std::abs(m_cachedBadgeDpi - safeScale) <= 0.005f) {
            return true;
        }
        const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
        const float fontPx = (std::max)(12.0f, 12.0f * safeScale);
        Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(m_dwriteFactory->CreateTextFormat(
                uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontPx, L"en-US", fmt.GetAddressOf())) || !fmt) {
            return false;
        }
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        m_badgeTextFormat = std::move(fmt);
        m_cachedBadgeDpi = safeScale;
        return true;
    }

    static float estimateBadgeTextWidth(const std::wstring& text, float dpiScale) noexcept {
        // 百分比文字仅含数字与 '%' (如 "125%"), 零热路径 IDWriteTextLayout 分配
        return std::ceil(static_cast<float>(text.size()) * 7.8f * dpiScale + 6.0f * dpiScale);
    }

    void updateHitRect(const PinZoomStageFrameParams& frame) noexcept {
        constexpr double kHitPadPx = 12.0;
        m_hitLeft = frame.drawLeft - kHitPadPx;
        m_hitTop = frame.drawTop - kHitPadPx;
        m_hitRight = frame.drawLeft + (std::max)(1.0, frame.drawWidth) + kHitPadPx;
        m_hitBottom = frame.drawTop + (std::max)(1.0, frame.drawHeight) + kHitPadPx;
    }

    static LRESULT CALLBACK stageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<PinZoomStage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;

            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_NCHITTEST: {
                if (!self || !self->m_active) {
                    return HTTRANSPARENT;
                }
                const double sx = static_cast<double>(GET_X_LPARAM(lParam));
                const double sy = static_cast<double>(GET_Y_LPARAM(lParam));
                if (sx >= self->m_hitLeft && sx <= self->m_hitRight &&
                    sy >= self->m_hitTop && sy <= self->m_hitBottom) {
                    return HTCLIENT;
                }
                return HTTRANSPARENT;
            }

            case WM_SETCURSOR:
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;

            case WM_MOUSEWHEEL: {
                if (self && self->m_callbacks.onMouseWheel) {
                    auto cb = self->m_callbacks.onMouseWheel;
                    cb(wParam, lParam);
                }
                return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP: {
                if (self && self->m_callbacks.onInterruptMouse) {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ClientToScreen(hwnd, &pt);
                    auto cb = self->m_callbacks.onInterruptMouse;
                    cb(msg, wParam, pt);
                }
                return 0;
            }

            case PinZoomVSyncPacer::kMsgStageVSyncTick: {
                if (self) {
                    self->m_pacer.acknowledgeTick();
                    if (self->m_active &&
                        static_cast<uint64_t>(wParam) == self->m_pacer.currentEpoch() &&
                        self->m_callbacks.onVSyncTick) {
                        auto cb = self->m_callbacks.onVSyncTick;
                        cb();
                    }
                }
                return 0;
            }

            case WM_DISPLAYCHANGE: {
                if (self && self->m_callbacks.onDisplayChanged) {
                    auto cb = self->m_callbacks.onDisplayChanged;
                    cb();
                }
                return 0;
            }

            default:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    inline static std::atomic<bool> s_classRegistered{false};

    HWND m_hwnd = nullptr;
    PinZoomStageBounds m_bounds{};
    bool m_active = false;
    bool m_visible = false;
    bool m_visualAttached = false;
    uint32_t m_refreshHz = 120u;

    double m_hitLeft = 0.0;
    double m_hitTop = 0.0;
    double m_hitRight = 0.0;
    double m_hitBottom = 0.0;

    PinZoomStageCallbacks m_callbacks{};
    PinZoomVSyncPacer m_pacer{};

    HMODULE m_dcompDll = nullptr;
    bool m_ownsDcompDll = false;
    PfnDCompositionCreateDevice m_pfnDCompCreate = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_dcompVisual;

    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_targetBitmap;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_sharedBrush;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeTextFormat;
    float m_cachedBadgeDpi = 0.0f;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_bitmap;
    std::vector<StageBitmapTile> m_bitmapTiles;
    uint64_t m_uploadedGeneration = 0;
    const uchar* m_uploadedDataPtr = nullptr;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
};

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_PINZOOMSTAGE_H
