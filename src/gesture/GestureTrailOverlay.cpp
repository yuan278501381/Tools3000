// ─────────────────────────────────────────────────────────────────────────────
// GestureTrailOverlay.cpp — 手势轨迹可视化覆盖层实现
//
// 核心原理:
//   1. 创建 WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST 的全屏窗口
//   2. 使用 Direct2D 绘制连续贝塞尔平滑轨迹，带双层霓虹微光流光特效
//   3. 头部绘制发光能量微粒，提升绘制动感
//   4. 手势绘制过程中及手势完成后显示按键回显风格的实时动作名称
//   5. 窗口始终 click-through（WS_EX_TRANSPARENT），不影响用户操作
//   6. 颜色支持独立自定义配置或动态联动系统主题强调色
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureTrailOverlay.h"
#include "gesture/GestureInputPolicy.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/config/ConfigManager.h"
#include "core/accessibility/OverlayAnnouncement.h"
#include "core/accessibility/OverlayUiaProvider.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <climits>

#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <avrt.h>

#pragma comment(lib, "winmm.lib")

using namespace Microsoft::WRL;

namespace tools3000::gesture {

namespace {

void ensurePremultipliedAlpha(void* bits, int width, int height, int pitch) noexcept {
    if (!bits || width <= 0 || height <= 0 || pitch < width * 4) return;
    auto* row = reinterpret_cast<uint8_t*>(bits);
    for (int y = 0; y < height; ++y) {
        auto* px = reinterpret_cast<uint32_t*>(row);
        for (int x = 0; x < width; ++x) {
            const uint32_t val = px[x];
            if (val != 0) {
                const uint8_t a = static_cast<uint8_t>((val >> 24) & 0xFF);
                if (a == 0) {
                    const uint8_t r = static_cast<uint8_t>((val >> 16) & 0xFF);
                    const uint8_t g = static_cast<uint8_t>((val >> 8) & 0xFF);
                    const uint8_t b = static_cast<uint8_t>(val & 0xFF);
                    const uint8_t maxC = (std::max)({r, g, b});
                    px[x] = (static_cast<uint32_t>(maxC) << 24) | (val & 0x00FFFFFF);
                }
            }
        }
        row += pitch;
    }
}

}  // namespace

static constexpr const wchar_t* OVERLAY_CLASS = L"Tools3000_GestureOverlay";
static constexpr UINT WM_GESTURE_ACCESSIBILITY_RESULT = WM_APP + 73;

GestureTrailOverlay& GestureTrailOverlay::instance() {
    static GestureTrailOverlay inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// 初始化 / 关闭
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::initialize(HINSTANCE hInstance) {
    tools3000::core::TraceId::Scope scope;
    m_hInstance = hInstance;
    timeBeginPeriod(1);
    m_pacer.initialize();

    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_wakeEvent) {
        LOG_ERROR("创建手势渲染唤醒事件失败");
        return false;
    }

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent) {
        LOG_ERROR("创建手势渲染同步事件失败");
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
        return false;
    }

    // 渲染线程自建并独立持有 HWND (m_hwnd 与 m_toastHwnd)，彻底根除跨线程 HWND 亲和性互锁
    m_renderThread = std::jthread([this, readyEvent](std::stop_token st) {
        renderLoop(st, readyEvent);
    });

    if (m_renderThread.native_handle()) {
        SetThreadPriority(m_renderThread.native_handle(), gestureRenderThreadPriority());
    }

    // 等待渲染线程完成窗口与 DirectComposition 设备创建
    WaitForSingleObject(readyEvent, 5000);
    CloseHandle(readyEvent);

    LOG_INFO("手势轨迹覆盖层初始化成功 (专用异步渲染管线已启动)");
    return true;
}

void GestureTrailOverlay::setStyle(const TrailStyle& style) {
    m_style = style;
    m_textScale = 0.0f;
    if (m_dwriteFactory) updateTextFormat(m_dpiScale);
    reloadThemeColors();
}

void GestureTrailOverlay::reloadThemeColors() {
    m_themeDirty.store(true, std::memory_order_release);
    if (m_visible.load(std::memory_order_relaxed) ||
        m_wantVisible.load(std::memory_order_relaxed) ||
        m_fading.load(std::memory_order_relaxed)) {
        wakeRender();
    }
}

void GestureTrailOverlay::applyThemeColorsLocked() {
    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string colorMode = cfg.get<std::string>("/gesture/trailColorMode", "auto");
    const std::string customHex = cfg.get<std::string>("/gesture/trailColor", "#3B82F6");
    m_style.lineWidth = cfg.get<float>("/gesture/trailWidth", 2.5f);
    m_style.outlineWidth = clampTrailOutlineWidth(
        cfg.get<float>("/gesture/trailOutlineWidth", 1.5f));

    const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
    const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);
    m_cachedTrailRgb = resolveGestureTrailRgb(colorMode, customHex, themeRgb);

    // 检测是否为亮色主题 (仅在配置变更或主题刷新时执行一次低频注册表读取，杜绝热路径重复查询)
    const std::string theme = cfg.get<std::string>("/general/theme", "system");
    bool systemAppsUseLight = false;
    if (theme == "system") {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD value = 1;
            DWORD size = sizeof(value);
            if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size) == ERROR_SUCCESS) {
                systemAppsUseLight = (value != 0);
            }
            RegCloseKey(hKey);
        }
    }
    m_isLightTheme = gestureTrailUsesLightPalette(theme, systemAppsUseLight);
    m_isDarkTheme = !m_isLightTheme;

    const bool isLight = m_isLightTheme;
    const tools3000::core::AccentColorRGB& trailRgb = m_cachedTrailRgb;

    if (!m_renderTarget) return;

    // 主流光画笔（可自定义或跟随主题）
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 1.0f),
        m_lineBrush.ReleaseAndGetAddressOf()
    );

    // 外部柔光霓虹画笔
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.40f),
        m_glowBrush.ReleaseAndGetAddressOf()
    );

    // 灰色画笔 (未匹配动作时使用)。亮色主题必须足够深，浅银灰叠在白壁纸上等于没有轨迹。
    if (isLight) {
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.28f, 0.31f, 0.38f, 0.96f),
            m_greyLineBrush.ReleaseAndGetAddressOf()
        );
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.40f),
            m_greyGlowBrush.ReleaseAndGetAddressOf()
        );
    } else {
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.60f, 0.65f, 0.75f, 0.85f),
            m_greyLineBrush.ReleaseAndGetAddressOf()
        );
        m_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.40f, 0.45f, 0.55f, 0.30f),
            m_greyGlowBrush.ReleaseAndGetAddressOf()
        );
    }

    // 头部发光核心晶体画笔
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_headCoreBrush.ReleaseAndGetAddressOf()
    );
    m_renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
        m_outlineBrush.ReleaseAndGetAddressOf()
    );
}

void GestureTrailOverlay::shutdown() {
    if (m_renderThread.joinable()) {
        m_renderThread.request_stop();
        wakeRender();
        LOG_DEBUG("Gesture shutdown: waiting for render worker");
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_renderThread);
        LOG_DEBUG("Gesture shutdown: render worker stopped");
    }
    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    releaseD2DResources();
    m_visible.store(false);
    m_pacer.shutdown();
    timeEndPeriod(1);
    LOG_DEBUG("手势轨迹覆盖层已关闭");
}

void GestureTrailOverlay::wakeRender() noexcept {
    m_wakeRender.store(true, std::memory_order_release);
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
}

void GestureTrailOverlay::clearCanvas() {
    std::lock_guard lock(m_renderMutex);
    clearCanvasLocked();
}

void GestureTrailOverlay::clearCanvasLocked() {
    m_accumulatedStrokeDirtyRect = { 0, 0, 0, 0 };
    m_frontCtx.renderTarget.Reset();
    m_frontCtx.dxgiSurface.Reset();
    m_frontCtx.lineBrush.Reset();
    m_frontCtx.glowBrush.Reset();
    m_frontCtx.outlineBrush.Reset();
    m_frontCtx.headCoreBrush.Reset();
    if (m_memoryBits && m_height > 0 && m_memoryPitch > 0) {
        std::memset(m_memoryBits, 0, static_cast<size_t>(m_memoryPitch * m_height));
    }
    if (m_compositorReady && m_dcompDevice) {
        if (m_trailDcompVisual) m_trailDcompVisual->SetContent(nullptr);
        if (m_toastDcompVisual) {
            m_toastDcompVisual->SetContent(nullptr);
        }
        if (m_toastEffectGroup) {
            m_toastEffectGroup->SetOpacity(0.0f);
        }
        m_dcompDevice->Commit();
    }
    if (m_hwnd && m_memoryDC && m_width > 0 && m_height > 0) {
        HDC hdcScreen = GetDC(nullptr);
        if (hdcScreen) {
            POINT ptSrc = { 0, 0 };
            SIZE sz = { m_width, m_height };
            POINT ptDst = { m_originX, m_originY };
            BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &sz, m_memoryDC, &ptSrc, 0, &blend, ULW_ALPHA);
            ReleaseDC(nullptr, hdcScreen);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 异步渲染核心循环 (在专用高优先级线程运行)
// ─────────────────────────────────────────────────────────────────────────────

void GestureTrailOverlay::renderLoop(std::stop_token stopToken, HANDLE readyEvent) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoGuard { ~CoGuard() { CoUninitialize(); } } coGuard;

    SetThreadPriority(GetCurrentThread(), gestureRenderThreadPriority());
    // 动态挂载 MMCSS 调度服务 (DisplayPostProcessing / Games)，彻底抹平系统高负荷抢占
    HMODULE hAvrt = LoadLibraryW(L"avrt.dll");
    HANDLE hMmcss = nullptr;
    typedef HANDLE (WINAPI *pfnAvSet)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *pfnAvRevert)(HANDLE);
    typedef BOOL (WINAPI *pfnAvSetPriority)(HANDLE, AVRT_PRIORITY);
    pfnAvRevert fnRevert = nullptr;
    pfnAvSetPriority fnSetPriority = nullptr;
    if (hAvrt) {
        auto fnSet = reinterpret_cast<pfnAvSet>(GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW"));
        fnRevert = reinterpret_cast<pfnAvRevert>(GetProcAddress(hAvrt, "AvRevertMmThreadCharacteristics"));
        fnSetPriority = reinterpret_cast<pfnAvSetPriority>(GetProcAddress(hAvrt, "AvSetMmThreadPriority"));
        if (fnSet) {
            DWORD taskIndex = 0;
            hMmcss = fnSet(L"DisplayPostProcessing", &taskIndex);
            if (!hMmcss) {
                taskIndex = 0;
                hMmcss = fnSet(L"Games", &taskIndex);
            }
            if (hMmcss && fnSetPriority) {
                fnSetPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
            }
        }
    }
    struct MmcssGuard {
        HANDLE handle;
        pfnAvRevert revert;
        HMODULE mod;
        ~MmcssGuard() {
            if (handle && revert) revert(handle);
            if (mod) FreeLibrary(mod);
        }
    } mmcssGuard{ hMmcss, fnRevert, hAvrt };

    D2D1_FACTORY_OPTIONS opt{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, opt, m_d2dFactory.GetAddressOf());
    if (SUCCEEDED(hr)) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
        );
        D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f
        );
        m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, m_strokeStyle.GetAddressOf());
    }

    if (!createOverlayWindow(m_hInstance)) {
        LOG_ERROR("渲染线程创建手势覆盖层窗口失败");
        if (readyEvent) SetEvent(readyEvent);
        return;
    }

    ensureCompositorLocked();

    if (readyEvent) {
        SetEvent(readyEvent);
    }

    while (!stopToken.stop_requested()) {
        const bool isFading = m_fading.load(std::memory_order_relaxed);
        const bool activeDraw = m_wantVisible.load(std::memory_order_relaxed) ||
                                (!m_pointQueue.empty()) ||
                                m_renderRequested.load(std::memory_order_relaxed);

        // 1. 同步节拍器状态
        if (isFading) {
            if (!m_pacer.isFading()) {
                m_pacer.beginFadeout(0);
            }
        } else if (activeDraw) {
            if (!m_pacer.isActive()) {
                m_pacer.beginStroke();
            }
        } else {
            if (!m_pacer.isIdle()) {
                m_pacer.setIdle();
            }
        }

        // F13: 活跃状态与休眠状态转换控制（双重校验防丢失唤醒）
        if (m_pacer.isIdle()) {
            m_renderLoopActive.store(false, std::memory_order_release);

            // 双重检验：如果在置 false 之前有新点推入或请求渲染，立即取消休眠
            if (!m_pointQueue.empty() ||
                m_wantVisible.load(std::memory_order_acquire) ||
                m_renderRequested.load(std::memory_order_acquire) ||
                m_fading.load(std::memory_order_acquire)) {
                m_renderLoopActive.store(true, std::memory_order_release);
                if (m_fading.load(std::memory_order_relaxed)) {
                    m_pacer.beginFadeout(0);
                } else {
                    m_pacer.beginStroke();
                }
            }
        } else {
            m_renderLoopActive.store(true, std::memory_order_release);
        }

        // 2. 节拍等待与硬件锁步 (Idle 0% CPU 挂起，Active/Fadeout 混合休眠微自旋)
        bool readyToPresent = false;
        if (m_pacer.isIdle()) {
            MsgWaitForMultipleObjectsEx(
                m_wakeEvent ? 1 : 0,
                m_wakeEvent ? &m_wakeEvent : nullptr,
                INFINITE,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE
            );
            m_wakeRender.store(false, std::memory_order_relaxed);
            if (!m_pointQueue.empty() || m_wantVisible.load(std::memory_order_relaxed) || m_fading.load(std::memory_order_relaxed)) {
                m_renderLoopActive.store(true, std::memory_order_release);
            }
        } else {
            readyToPresent = m_pacer.waitOrPace(m_wakeEvent);
            m_wakeRender.store(false, std::memory_order_relaxed);
        }

        if (stopToken.stop_requested()) break;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (m_zOrderYieldRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            // 严禁将 m_toastHwnd 置为 HWND_BOTTOM 或降级！
            // Toast 属于系统级 HUD 结果提示卡片，必须始终保持在最顶层 (HWND_TOPMOST)，确保 100% 不被前台窗口遮挡
        }

        if (m_zOrderRaiseRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hwnd && IsWindow(m_hwnd)) {
                SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        if (m_hideRequested.exchange(false, std::memory_order_acq_rel)) {
            if (m_hideEpoch.load(std::memory_order_acquire) == m_trailEpoch.load(std::memory_order_acquire)) {
                applyHideOverlayState();
                continue;
            }
        }

        if (m_dismissPrevious.exchange(false, std::memory_order_acq_rel)) {
            if (!m_wantVisible.load(std::memory_order_relaxed) &&
                !m_fading.load(std::memory_order_relaxed) &&
                m_hideEpoch.load(std::memory_order_acquire) == m_trailEpoch.load(std::memory_order_acquire)) {
                {
                    std::lock_guard renderLock(m_renderMutex);
                    clearCanvasLocked();
                }
                if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
                hideToastWindow();
                m_visible.store(false, std::memory_order_relaxed);
            }
        }

        m_renderRequested.store(false, std::memory_order_relaxed);

        bool fading = m_fading.load(std::memory_order_relaxed);
        if (fading) {
            const uint64_t currentEpoch = m_trailEpoch.load(std::memory_order_acquire);
            if (currentEpoch != m_fadeEpoch) {
                // 新手势已开始并打断了上一笔淡出，绝不隐藏窗口或释放资源
                m_fading.store(false, std::memory_order_relaxed);
                m_fadeClockStarted.store(false, std::memory_order_relaxed);
                fading = false;
            } else {
                const DWORD holdMs = static_cast<DWORD>(m_style.fadeHoldMs);
                const DWORD fadeMs = static_cast<DWORD>(m_style.fadeOutMs);
                const bool clockStarted = m_fadeClockStarted.load(std::memory_order_relaxed);
                const DWORD elapsed = clockStarted ? (GetTickCount() - m_fadeStartTick) : 0;
                if (gestureFadeShouldFinish(clockStarted, elapsed, holdMs, fadeMs)) {
                    if (m_trailEpoch.load(std::memory_order_acquire) != m_fadeEpoch) {
                        m_fading.store(false, std::memory_order_relaxed);
                        m_fadeClockStarted.store(false, std::memory_order_relaxed);
                        fading = false;
                    } else {
                        m_fading.store(false, std::memory_order_relaxed);
                        m_fadeClockStarted.store(false, std::memory_order_relaxed);
                        m_fadeAlpha.store(0.0f, std::memory_order_relaxed);
                        m_pulseIntensity.store(0.0f, std::memory_order_relaxed);
                        {
                            std::lock_guard trailLock(m_trailMutex);
                            m_points.clear();
                            m_resultText.clear();
                            m_smoothPathGeometry.Reset();
                        }
                        m_isRecognized.store(false, std::memory_order_relaxed);
                        m_wantVisible.store(false, std::memory_order_relaxed);
                        m_strokeSurfaceLive.store(false, std::memory_order_relaxed);
                        {
                            std::lock_guard renderLock(m_renderMutex);
                            clearCanvasLocked();
                            resetVisualOpacityLocked();
                        }
                        if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
                        hideToastWindow();
                        m_visible.store(false, std::memory_order_relaxed);
                        m_pacer.setIdle();
                        m_renderLoopActive.store(false, std::memory_order_release);
                        continue;
                    }
                } else {
                    const float fadeAlpha = gestureFadeAlpha(clockStarted, elapsed, holdMs, fadeMs);
                    m_fadeAlpha.store(fadeAlpha, std::memory_order_relaxed);

                    if (m_compositorReady && m_dcompDevice && m_trailDcompVisual) {
                        if (!clockStarted) {
                            // 最后一帧固化：以 1.0f 完成轨迹与 Toast 卡片的最后一次物理光栅化
                            m_fadeAlpha.store(1.0f, std::memory_order_relaxed);
                            render();
                            m_fadeStartTick = GetTickCount();
                            m_fadeClockStarted.store(true, std::memory_order_release);
                        } else {
                            // 纯硬件淡出：0 CPU 重绘，0 GPU 绘制指令
                            {
                                std::lock_guard renderLock(m_renderMutex);
                                applyVisualOpacityLocked(fadeAlpha);
                            }
                            m_pacer.advanceDeadline();
                        }
                    } else {
                        const bool isSuccess = m_isRecognized.load(std::memory_order_relaxed);
                        m_pulseIntensity.store(gestureSuccessPulseIntensity(true, isSuccess, clockStarted, elapsed), std::memory_order_relaxed);
                        const bool presented = render();
                        if (presented) {
                            if (!clockStarted) {
                                m_fadeStartTick = GetTickCount();
                                m_fadeClockStarted.store(true, std::memory_order_release);
                            }
                            m_pacer.advanceDeadline();
                        } else {
                            // 无法呈现帧（如无轨迹点或表面失效），立即结束淡出并隐藏覆盖层，杜绝死循环与窗口残留
                            m_fading.store(false, std::memory_order_relaxed);
                            m_fadeClockStarted.store(false, std::memory_order_relaxed);
                            m_pulseIntensity.store(0.0f, std::memory_order_relaxed);
                            applyHideOverlayState();
                            m_pacer.setIdle();
                            m_renderLoopActive.store(false, std::memory_order_release);
                            continue;
                        }
                    }
                    continue;
                }
            }
        }

        if (!fading && (m_visible.load(std::memory_order_relaxed) ||
                        m_wantVisible.load(std::memory_order_relaxed))) {
            if (readyToPresent) {
                m_pulseIntensity.store(0.0f, std::memory_order_relaxed);
                render();
                m_pacer.advanceDeadline();
            }
        }
    }

    // 渲染线程销毁自身持有的所有 HWND 与 DirectComposition 资源
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
    releaseCompositorLocked();
}

// ─────────────────────────────────────────────────────────────────────────────
// 轨迹操作：避免同步 I/O；实际耗时应通过 PerformanceMonitor 在目标设备上测量。
// ─────────────────────────────────────────────────────────────────────────────

void GestureTrailOverlay::beginTrail() {
    raiseZOrderForDraw();
    m_trailEpoch.fetch_add(1, std::memory_order_acq_rel);
    m_hideRequested.store(false, std::memory_order_release);
    m_wantVisible.store(false, std::memory_order_release);
    m_strokeSurfaceLive.store(false, std::memory_order_release);
    m_fading.store(false, std::memory_order_release);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_lastWakeTick.store(0, std::memory_order_release);
    m_fadeAlpha.store(1.0f, std::memory_order_release);
    m_pulseIntensity.store(0.0f, std::memory_order_release);
    // 节拍器状态由专用渲染循环根据活跃绘制状态机统一驱动，消除跨线程直接调用
    m_pointQueue.clear();
    {
        std::lock_guard lock(m_trailMutex);
        m_points.clear();
        m_resultText.clear();
        m_smoothPathGeometry.Reset();
    }

    m_isRecognized.store(false, std::memory_order_relaxed);
    m_themeDirty.store(true, std::memory_order_release);
    m_dismissPrevious.store(false, std::memory_order_release);
    m_accumulatedStrokeDirtyRect = { 0, 0, 0, 0 };

    // F9: 彻底重置 Visual Opacity 为 1.0f，防止被打断的前一笔淡出残留导致新笔划变暗，并清空新笔画表面
    {
        std::lock_guard renderLock(m_renderMutex);
        resetVisualOpacityLocked();
        if (m_compositorReady && m_frontCtx.surface && m_d2dFactory) {
            POINT offset{};
            Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurf;
            HRESULT hr = m_frontCtx.surface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset);
            if (SUCCEEDED(hr) && dxgiSurf) {
                Microsoft::WRL::ComPtr<ID2D1RenderTarget> clearRt;
                D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                hr = m_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurf.Get(), rtProps, clearRt.GetAddressOf());
                if (SUCCEEDED(hr) && clearRt) {
                    clearRt->BeginDraw();
                    clearRt->Clear(D2D1::ColorF(0, 0, 0, 0));
                    clearRt->EndDraw();
                }
                m_frontCtx.surface->EndDraw();
            }
            m_frontCtx.dxgiSurface.Reset();
            m_frontCtx.renderTarget.Reset();
            m_frontCtx.lineBrush.Reset();
            m_frontCtx.glowBrush.Reset();
            m_frontCtx.outlineBrush.Reset();
            m_frontCtx.headCoreBrush.Reset();
            if (m_trailDcompVisual) {
                m_trailDcompVisual->SetContent(m_frontCtx.surface.Get());
            }
            m_dcompDevice->Commit();
        }
    }

    wakeRender();
}

void GestureTrailOverlay::addPoint(float x, float y) {
    m_pointQueue.push(TrailPoint{x, y, GetTickCount()});

    const bool loopActive = m_renderLoopActive.load(std::memory_order_acquire);
    if (gestureShouldWakeRenderImmediately(m_hwnd != nullptr, loopActive)) {
        m_fading.store(false, std::memory_order_release);
        m_fadeAlpha = 1.0f;
        m_wantVisible.store(true, std::memory_order_release);
        m_renderRequested.store(true, std::memory_order_release);
        // 极致零延迟：点位推入后立即唤醒渲染线程，彻底根除 Windows GetTickCount 15.6ms 粗粒度时钟带来的轨迹跟手延迟
        wakeRender();
    } else {
        // F13 唤醒合并：渲染循环已处于活跃节流周期中，只需标记状态，免除每秒 1000 次内核事件唤醒风暴
        m_fading.store(false, std::memory_order_release);
        m_fadeAlpha = 1.0f;
        m_wantVisible.store(true, std::memory_order_release);
        m_renderRequested.store(true, std::memory_order_release);
    }
}

void GestureTrailOverlay::setLiveAction(const std::string& actionText) {
    bool changed = false;
    {
        std::lock_guard lock(m_trailMutex);
        if (m_resultText != actionText) {
            m_resultText = actionText;
            changed = true;
        }
    }
    if (changed && (m_visible.load(std::memory_order_relaxed) ||
                    m_wantVisible.load(std::memory_order_relaxed))) {
        m_renderRequested.store(true, std::memory_order_release);
        wakeRender();
    }
}

void GestureTrailOverlay::setRecognized(bool recognized) {
    // 若当前正处于手势松手命中淡出期，锁定最终呈现结果，严禁被并发调用篡改或清空
    if (m_fading.load(std::memory_order_acquire)) {
        return;
    }
    if (m_isRecognized.exchange(recognized) != recognized) {
        if (!recognized) {
            std::lock_guard lock(m_trailMutex);
            if (m_resultText != "•••") {
                m_resultText.clear();
            }
        }
        if (m_visible.load(std::memory_order_relaxed) ||
            m_wantVisible.load(std::memory_order_relaxed)) {
            m_renderRequested.store(true, std::memory_order_release);
            wakeRender();
        }
    }
}

void GestureTrailOverlay::endTrail(const std::string& resultText) {
    bool hasPoints = false;
    size_t overlayPoints = 0;
    {
        std::lock_guard lock(m_trailMutex);
        if (!resultText.empty()) {
            m_resultText = resultText;
        }
        TrailPoint drainedPt;
        while (m_pointQueue.pop(drainedPt)) {
            if (!m_points.empty()) {
                float dx = drainedPt.x - m_points.back().x;
                float dy = drainedPt.y - m_points.back().y;
                constexpr float minimumDelta = 1.0f;
                if (dx * dx + dy * dy < minimumDelta * minimumDelta) continue;
            }
            m_points.push_back(drainedPt);
        }
        hasPoints = !m_points.empty();
        overlayPoints = m_points.size();
    }
    LOG_DEBUG("手势轨迹结束: overlayPoints={}, label={}", overlayPoints, resultText);
    // 松手时才命中的短手势（如 U=关闭窗口）过程中方向可能没变过，
    // live setRecognized(true) 没走到，Toast 不能因此被关掉。
    if (!resultText.empty() && resultText != "•••") {
        m_isRecognized.store(true, std::memory_order_relaxed);
        // endTrail may run on the low-level mouse-hook path. Do not synchronously
        // call a window API here; hand the accessibility update to the HWND's
        // owning thread instead.
        if (m_hwnd) {
            PostMessageW(m_hwnd, WM_GESTURE_ACCESSIBILITY_RESULT, 0, 0);
        }
    }
    if (hasPoints) {
        m_wantVisible.store(true, std::memory_order_release);
    } else {
        // 无轨迹点（如纯滚轮手势），立即隐藏覆盖层表面，绝不进入僵死淡出循环
        hide();
        return;
    }
    m_fadeEpoch = m_trailEpoch.load(std::memory_order_acquire);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_fadeStartTick = 0;
    m_fadeAlpha.store(1.0f, std::memory_order_release);
    m_pulseIntensity.store((!resultText.empty() && resultText != "•••") ? 1.0f : 0.0f, std::memory_order_release);
    m_fading.store(true, std::memory_order_release);
    m_renderRequested.store(true, std::memory_order_release);
    // 节拍器状态由专用渲染循环根据 m_fading 状态机统一驱动进入 Fadeout 态
    wakeRender();
}

void GestureTrailOverlay::hide() {
    const uint64_t epoch = m_trailEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_hideEpoch.store(epoch, std::memory_order_release);
    m_fading.store(false, std::memory_order_release);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_pulseIntensity.store(0.0f, std::memory_order_release);
    m_wantVisible.store(false, std::memory_order_release);
    m_visible.store(false, std::memory_order_release);
    m_renderRequested.store(false, std::memory_order_release);
    m_hideRequested.store(true, std::memory_order_release);
    // 节拍器状态由专用渲染循环处理 hide 后安全归入 Idle 态并重置活跃标志
    wakeRender();
}

void GestureTrailOverlay::yieldZOrderForInput() {
    m_zOrderYieldRequested.store(true, std::memory_order_release);
    m_zOrderYielded.store(true, std::memory_order_release);
    wakeRender();
}

void GestureTrailOverlay::raiseZOrderForDraw() {
    m_zOrderRaiseRequested.store(true, std::memory_order_release);
    m_zOrderYielded.store(false, std::memory_order_release);
    wakeRender();
}

void GestureTrailOverlay::applyHideOverlayState() {
    if (m_hideEpoch.load(std::memory_order_acquire) != m_trailEpoch.load(std::memory_order_acquire)) {
        return;
    }
    m_visible.store(false, std::memory_order_release);
    m_wantVisible.store(false, std::memory_order_release);
    m_fading.store(false, std::memory_order_release);
    m_fadeClockStarted.store(false, std::memory_order_release);
    m_pulseIntensity.store(0.0f, std::memory_order_release);
    {
        std::lock_guard lock{m_renderMutex};
        clearCanvasLocked();
        // 保持全屏 DComp 表面常驻显存，杜绝下一笔手势重新申请表面
    }
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    hideToastWindow();
    m_pointQueue.clear();
    {
        std::lock_guard lock{m_trailMutex};
        m_points.clear();
        m_resultText.clear();
        m_smoothPathGeometry.Reset();
    }
    m_isRecognized.store(false);
    m_fadeAlpha.store(1.0f, std::memory_order_release);
    m_pulseIntensity.store(0.0f, std::memory_order_release);
    m_strokeSurfaceLive.store(false, std::memory_order_relaxed);
    if (!m_compositorReady) {
        releaseD2DResources();
        m_width = 0;
        m_height = 0;
    }
}

bool GestureTrailOverlay::recreateBitmapLocked(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || !m_hwnd) return false;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    if (!m_memoryDC) {
        m_memoryDC = CreateCompatibleDC(hdcScreen);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!m_memoryDC || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return false;
    }
    std::memset(bits, 0, static_cast<size_t>(width * 4 * height));

    HBITMAP selected = static_cast<HBITMAP>(SelectObject(m_memoryDC, bmp));
    if (m_memoryBitmap && selected == m_memoryBitmap) {
        DeleteObject(m_memoryBitmap);
    } else if (selected && selected != HGDI_ERROR && !m_oldBitmap) {
        m_oldBitmap = selected;
    }
    m_memoryBitmap = bmp;
    m_memoryBits = bits;
    m_memoryPitch = width * 4;
    m_originX = x;
    m_originY = y;
    m_width = width;
    m_height = height;

    if (m_renderTarget) {
        m_renderTarget.Reset();
        m_lineBrush.Reset();
        m_glowBrush.Reset();
        m_greyLineBrush.Reset();
        m_greyGlowBrush.Reset();
        m_headCoreBrush.Reset();
        m_outlineBrush.Reset();
    }
    LOG_DEBUG("手势轨迹表面重建: {}x{} at ({},{})", width, height, x, y);
    return true;
}

bool GestureTrailOverlay::presentLayeredLocked(HWND hwnd, HDC memDC, int x, int y,
                                               int width, int height) {
    if (!hwnd || !memDC || width <= 0 || height <= 0) return false;
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    POINT ptSrc = {0, 0};
    POINT ptWin = {x, y};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok = UpdateLayeredWindow(
        hwnd, hdcScreen, &ptWin, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    const DWORD err = ok ? 0 : GetLastError();
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
        LOG_WARN("手势覆盖层提交失败: {}x{} error={}", width, height, err);
        return false;
    }
    // HWND_BOTTOM 沉底后 WS_EX_TOPMOST 位经常还在，旧逻辑会跳过插队，轨迹画在
    // 最大化 Electron / CEF 窗下面。沉底过就必须无条件回到 TOPMOST 组。
    const bool yielded = m_zOrderYielded.load(std::memory_order_acquire);
    if (!yielded) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    return true;
}

void GestureTrailOverlay::hideToastWindow() {
    if (m_toastEffectGroup) {
        m_toastEffectGroup->SetOpacity(0.0f);
    }
    if (m_toastDcompVisual) {
        m_toastDcompVisual->SetContent(nullptr);
    }
    m_lastToastOriginX = -9999;
    m_lastToastOriginY = -9999;
    m_lastToastW = 0;
    m_lastToastH = 0;
}

bool GestureTrailOverlay::fitSurface(int left, int top, int right, int bottom,
                                     const std::vector<TrailPoint>& /*points*/,
                                     bool /*isRecognized*/,
                                     bool& alreadyRendered) {
    alreadyRendered = false;
    if (m_virtualW <= 0 || m_virtualH <= 0) {
        m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_virtualW = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        m_virtualH = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        m_originX = m_virtualX;
        m_originY = m_virtualY;
        m_width = m_virtualW;
        m_height = m_virtualH;
    }

    // 1. DirectComposition GPU 直通管线
    if (m_compositorReady && m_dcompDevice && m_trailDcompVisual) {
        if (!m_frontCtx.surface || m_trailDcompW != m_virtualW || m_trailDcompH != m_virtualH) {
            if (!preallocateTrailSurfacesLocked(m_virtualW, m_virtualH)) {
                return false;
            }
        }
        alreadyRendered = false;
        return true;
    }

    // 2. GDI 降级管线
    const int neededW = (std::max)(1, right - left);
    const int neededH = (std::max)(1, bottom - top);
    if (m_strokeSurfaceLive.load(std::memory_order_relaxed)) {
        if (overlaySurfaceContains(left, top, right, bottom,
                                   m_originX, m_originY, m_width, m_height) &&
            m_renderTarget && m_memoryBitmap) {
            return true;
        }

        int expLeft = 0, expTop = 0, expRight = 0, expBottom = 0;
        computeOverlaySurfaceBounds(
            left, top, right, bottom,
            m_originX, m_originY, m_width, m_height,
            true,
            m_virtualX, m_virtualY, m_virtualW, m_virtualH,
            expLeft, expTop, expRight, expBottom);

        const bool ok = recreateBitmapLocked(
            expLeft, expTop, (std::max)(1, expRight - expLeft), (std::max)(1, expBottom - expTop));
        if (ok) m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
        return ok;
    }

    if (overlaySurfaceContains(left, top, right, bottom,
                               m_originX, m_originY, m_width, m_height) &&
        overlayCanReuseSurface(neededW, neededH, m_width, m_height, 4) &&
        m_renderTarget && m_memoryBitmap) {
        m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
        return true;
    }

    int expLeft = 0, expTop = 0, expRight = 0, expBottom = 0;
    computeOverlaySurfaceBounds(
        left, top, right, bottom,
        m_originX, m_originY, m_width, m_height,
        false,
        m_virtualX, m_virtualY, m_virtualW, m_virtualH,
        expLeft, expTop, expRight, expBottom);

    const bool ok = recreateBitmapLocked(expLeft, expTop, (std::max)(1, expRight - expLeft), (std::max)(1, expBottom - expTop));
    if (ok) m_strokeSurfaceLive.store(true, std::memory_order_relaxed);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口创建
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::createOverlayWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    m_virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_virtualW = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    m_virtualH = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    m_originX = m_virtualX;
    m_originY = m_virtualY;
    m_width = m_virtualW;
    m_height = m_virtualH;

    m_helperOwnerHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        L"STATIC",
        L"Tools3000_GestureTrailHelperOwner",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInstance, nullptr
    );

    const DWORD layeredEx = static_cast<DWORD>(normalizeGestureOverlayExStyle(0));

    m_hwnd = CreateWindowExW(
        layeredEx,
        OVERLAY_CLASS,
        L"Tools3000 Gesture Trail",
        WS_POPUP,
        m_originX, m_originY, m_width, m_height,
        m_helperOwnerHwnd,
        nullptr,
        hInstance,
        this
    );

    if (!m_hwnd) {
        LOG_ERROR("创建手势轨迹窗口失败");
        return false;
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                      normalizeGestureOverlayExStyle(
                          GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)));
    const DWM_WINDOW_CORNER_PREFERENCE noCorners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &noCorners, sizeof(noCorners));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disableTransitions, sizeof(disableTransitions));
    SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
    // 预分配全虚拟屏架构：保持 m_width 与 m_height 锁定全屏尺寸，手势划动全程 0 次 SetWindowPos
    ShowWindow(m_hwnd, SW_HIDE);

    LOG_INFO("手势覆盖层初始化完成 (原生分层窗口硬件合成加速管线已就绪)");
    return true;
}

bool GestureTrailOverlay::ensureCompositorLocked() {
    if (m_compositorReady && m_dcompDevice && m_trailDcompTarget) return true;
    releaseCompositorLocked();
    if (!m_hwnd) return false;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
    };
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, d3d.GetAddressOf(), &featureLevel, nullptr);
    }
    if (FAILED(hr) || !d3d) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d.As(&dxgiDevice)) || !dxgiDevice) return false;

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp;
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcomp.GetAddressOf()));
    if (FAILED(hr) || !dcomp) return false;

    Microsoft::WRL::ComPtr<IDCompositionTarget> trailTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> rootVisual;
    Microsoft::WRL::ComPtr<IDCompositionVisual> trailVisual;
    Microsoft::WRL::ComPtr<IDCompositionVisual> toastVisual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> trailEffect;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> toastEffect;
    if (FAILED(dcomp->CreateTargetForHwnd(m_hwnd, TRUE, trailTarget.GetAddressOf())) ||
        FAILED(dcomp->CreateVisual(rootVisual.GetAddressOf())) ||
        FAILED(dcomp->CreateVisual(trailVisual.GetAddressOf())) ||
        FAILED(dcomp->CreateVisual(toastVisual.GetAddressOf())) ||
        FAILED(rootVisual->AddVisual(trailVisual.Get(), FALSE, nullptr)) ||
        FAILED(rootVisual->AddVisual(toastVisual.Get(), TRUE, trailVisual.Get())) ||
        FAILED(trailTarget->SetRoot(rootVisual.Get()))) {
        return false;
    }

    if (SUCCEEDED(dcomp->CreateEffectGroup(trailEffect.GetAddressOf())) && trailEffect) {
        trailEffect->SetOpacity(1.0f);
        trailVisual->SetEffect(trailEffect.Get());
    }
    if (SUCCEEDED(dcomp->CreateEffectGroup(toastEffect.GetAddressOf())) && toastEffect) {
        toastEffect->SetOpacity(0.0f);
        toastVisual->SetEffect(toastEffect.Get());
    }

    m_d3dDevice = std::move(d3d);
    m_dcompDevice = std::move(dcomp);
    m_trailDcompTarget = std::move(trailTarget);
    m_rootDcompVisual = std::move(rootVisual);
    m_trailDcompVisual = std::move(trailVisual);
    m_toastDcompVisual = std::move(toastVisual);
    m_trailEffectGroup = std::move(trailEffect);
    m_toastEffectGroup = std::move(toastEffect);
    m_compositorReady = true;

    // 预分配全虚拟屏 DirectComposition 双缓冲硬件表面
    preallocateTrailSurfacesLocked(m_virtualW, m_virtualH);
    return true;
}

bool GestureTrailOverlay::preallocateTrailSurfacesLocked(int width, int height) {
    if (!m_dcompDevice || !m_trailDcompVisual || width <= 0 || height <= 0) return false;
    if (m_frontCtx.surface && m_trailDcompW == width && m_trailDcompH == height) {
        return true;
    }

    releaseCompositorSurfacesLocked();

    Microsoft::WRL::ComPtr<IDCompositionSurface> newFront;

    HRESULT hr = m_dcompDevice->CreateSurface(
        static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
        newFront.GetAddressOf());
    if (FAILED(hr) || !newFront) {
        LOG_WARN("创建 DirectComposition 前台轨迹表面失败: {}x{}, hr=0x{:X}", width, height, hr);
        return false;
    }

    m_frontCtx.reset();
    m_frontCtx.surface = newFront;
    m_trailDcompW = width;
    m_trailDcompH = height;
    m_trailDcompSurface = m_frontCtx.surface;
    m_accumulatedStrokeDirtyRect = { 0, 0, 0, 0 };
    m_trailDcompVisual->SetContent(m_frontCtx.surface.Get());
    m_dcompDevice->Commit();
    return true;
}

void GestureTrailOverlay::releaseCompositorSurfacesLocked() {
    if (m_trailDcompVisual) m_trailDcompVisual->SetContent(nullptr);
    if (m_toastDcompVisual) m_toastDcompVisual->SetContent(nullptr);
    if (m_toastEffectGroup) m_toastEffectGroup->SetOpacity(0.0f);
    m_frontCtx.reset();
    m_toastFrontCtx.reset();
    m_toastBackCtx.reset();
    m_trailDcompSurface.Reset();
    m_toastDcompSurface.Reset();
    m_trailDcompW = 0;
    m_trailDcompH = 0;
    m_toastDcompW = 0;
    m_toastDcompH = 0;
    m_lastToastOriginX = -9999;
    m_lastToastOriginY = -9999;
    m_lastToastW = 0;
    m_lastToastH = 0;
    m_accumulatedStrokeDirtyRect = { 0, 0, 0, 0 };
}

void GestureTrailOverlay::releaseCompositorLocked() {
    releaseCompositorSurfacesLocked();
    m_rootDcompVisual.Reset();
    m_trailDcompVisual.Reset();
    m_toastDcompVisual.Reset();
    m_trailEffectGroup.Reset();
    m_toastEffectGroup.Reset();
    m_trailDcompTarget.Reset();
    m_dcompDevice.Reset();
    m_d3dDevice.Reset();
    m_compositorReady = false;
}



// ─────────────────────────────────────────────────────────────────────────────
// Direct2D 资源管理
// ─────────────────────────────────────────────────────────────────────────────

bool GestureTrailOverlay::ensureStrokeStyleLocked() {
    if (m_strokeStyle) return true;
    if (!m_d2dFactory) {
        D2D1_FACTORY_OPTIONS options{};
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, options, m_d2dFactory.GetAddressOf());
        if (FAILED(hr) || !m_d2dFactory) return false;
    }
    D2D1_STROKE_STYLE_PROPERTIES strokeProps = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND,
        10.0f,
        D2D1_DASH_STYLE_SOLID,
        0.0f
    );
    HRESULT hr = m_d2dFactory->CreateStrokeStyle(strokeProps, nullptr, 0, m_strokeStyle.GetAddressOf());
    return SUCCEEDED(hr) && m_strokeStyle;
}

bool GestureTrailOverlay::createD2DResources() {
    if (m_renderTarget && m_memoryDC && m_memoryBitmap && m_lineBrush) return true;

    auto fail = [this]() {
        releaseD2DResourcesLocked();
        return false;
    };

    HRESULT hr = S_OK;

    // D2D 工厂
    if (!m_d2dFactory) {
        D2D1_FACTORY_OPTIONS options{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, options, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return fail();
    }

    // DirectWrite 工厂
    if (!m_dwriteFactory) {
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
        );
        if (FAILED(hr)) return fail();
    }

    if (!updateTextFormat(m_dpiScale)) return fail();

    // 渲染目标
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );

    hr = m_d2dFactory->CreateDCRenderTarget(&rtProps, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) return fail();

    if (!m_memoryDC) {
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen) return fail();
        m_memoryDC = CreateCompatibleDC(hdcScreen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_width;
        bmi.bmiHeader.biHeight = -m_height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        m_memoryBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdcScreen);
        if (!m_memoryDC || !m_memoryBitmap || !pBits) return fail();
        m_oldBitmap = (HBITMAP)SelectObject(m_memoryDC, m_memoryBitmap);
    }
    
    RECT memRect = { 0, 0, m_width, m_height };
    if (FAILED(m_renderTarget->BindDC(m_memoryDC, &memRect))) return fail();

    m_renderTarget->SetDpi(96.0f, 96.0f);

    // 笔触样式 (使线段更平滑，具有圆润笔头与圆角拐弯)
    if (!ensureStrokeStyleLocked()) return fail();

    applyThemeColorsLocked();
    m_themeDirty.store(false, std::memory_order_release);

    if (!m_lineBrush || !m_headCoreBrush) return fail();
    return true;
}

bool GestureTrailOverlay::updateTextFormat(float dpiScale) {
    dpiScale = std::clamp(dpiScale, 1.0f, 5.0f);
    if (m_textFormat && std::abs(m_textScale - dpiScale) < 0.01f) return true;
    if (!m_dwriteFactory) return false;
    ComPtr<IDWriteTextFormat> format;
    const HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        m_style.resultFontSize * dpiScale,
        L"zh-CN",
        format.GetAddressOf()
    );
    if (FAILED(hr) || !format) return false;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_textFormat = std::move(format);
    m_textScale = dpiScale;
    return true;
}

void GestureTrailOverlay::releaseD2DResources() {
    std::lock_guard lock(m_renderMutex);
    releaseD2DResourcesLocked();
}

void GestureTrailOverlay::releaseD2DResourcesLocked() {
    m_frontCtx.reset();
    m_toastFrontCtx.reset();
    m_toastBackCtx.reset();
    m_smoothPathGeometry.Reset();
    m_accumulatedStrokeDirtyRect = { 0, 0, 0, 0 };
    m_headCoreBrush.Reset();
    m_outlineBrush.Reset();
    m_glowBrush.Reset();
    m_greyGlowBrush.Reset();
    m_greyLineBrush.Reset();
    m_lineBrush.Reset();
    m_textFormat.Reset();
    m_textScale = 0.0f;
    // Note: m_strokeStyle is a factory-created device-independent resource and is preserved
    m_renderTarget.Reset();
    // Factories are preserved across memory trims for zero-recreation failure
    
    if (m_memoryDC && m_oldBitmap) {
        SelectObject(m_memoryDC, m_oldBitmap);
    }
    m_oldBitmap = nullptr;
    if (m_memoryBitmap) {
        DeleteObject(m_memoryBitmap);
        m_memoryBitmap = nullptr;
    }
    m_memoryBits = nullptr;
    m_memoryPitch = 0;
    if (m_memoryDC) {
        DeleteDC(m_memoryDC);
        m_memoryDC = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染核心与硬件合成管线
// ─────────────────────────────────────────────────────────────────────────────

RECT GestureTrailOverlay::computeTrailDirtyRect(
    const std::vector<TrailPoint>& points,
    int originX,
    int originY,
    int surfaceW,
    int surfaceH) const noexcept {
    return computeTrailPointsBoundingBox(
        points, originX, originY, surfaceW, surfaceH, m_style.lineWidth, m_dpiScale);
}

bool GestureTrailOverlay::renderTrailToSurfaceLocked(
    GpuSurfaceContext& ctx,
    const std::vector<TrailPoint>& points,
    bool isRecognized,
    float fadeAlpha,
    int originX,
    int originY,
    const RECT* pDirtyRect) {
    if (!ctx.surface || !m_dcompDevice || !m_d2dFactory) return false;

    POINT offset{};
    Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurf;
    HRESULT hr = ctx.surface->BeginDraw(pDirtyRect, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset);
    if (FAILED(hr) || !dxgiSurf) {
        LOG_WARN("DirectComposition 表面 BeginDraw 失败: hr=0x{:X}", hr);
        return false;
    }

    const tools3000::core::AccentColorRGB& trailRgb = m_cachedTrailRgb;
    const bool isLight = m_isLightTheme;

    D2D1_COLOR_F lineColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.96f * fadeAlpha);
    D2D1_COLOR_F glowColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.28f * fadeAlpha);
    if (!isRecognized && points.size() >= 4) {
        if (isLight) {
            lineColor = D2D1::ColorF(0.28f, 0.31f, 0.38f, 0.96f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.28f * fadeAlpha);
        } else {
            lineColor = D2D1::ColorF(0.60f, 0.65f, 0.75f, 0.85f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.45f, 0.55f, 0.28f * fadeAlpha);
        }
    }
    const D2D1_COLOR_F outlineColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f * fadeAlpha);
    const D2D1_COLOR_F headCoreColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha);

    // 显存 RenderTarget 与 SolidColorBrush 复用与缓存 (避免每帧 CreateDxgiSurfaceRenderTarget 与 4 次画刷分配)
    if (!ctx.renderTarget || !ctx.lineBrush || !ctx.glowBrush || !ctx.outlineBrush || !ctx.headCoreBrush || ctx.dxgiSurface.Get() != dxgiSurf.Get()) {
        ctx.dxgiSurface = dxgiSurf;
        ctx.renderTarget.Reset();
        ctx.lineBrush.Reset();
        ctx.glowBrush.Reset();
        ctx.outlineBrush.Reset();
        ctx.headCoreBrush.Reset();

        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = m_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurf.Get(), rtProps, ctx.renderTarget.GetAddressOf());
        if (FAILED(hr) || !ctx.renderTarget) {
            ctx.surface->EndDraw();
            return false;
        }

        ctx.renderTarget->CreateSolidColorBrush(lineColor, ctx.lineBrush.GetAddressOf());
        ctx.renderTarget->CreateSolidColorBrush(glowColor, ctx.glowBrush.GetAddressOf());
        ctx.renderTarget->CreateSolidColorBrush(outlineColor, ctx.outlineBrush.GetAddressOf());
        ctx.renderTarget->CreateSolidColorBrush(headCoreColor, ctx.headCoreBrush.GetAddressOf());
    } else {
        // 极速快路径：仅更新颜色值 (SetColor 仅更新 GPU 常量缓冲，0 次显存重分配与系统调用)
        if (ctx.lineBrush) ctx.lineBrush->SetColor(lineColor);
        if (ctx.glowBrush) ctx.glowBrush->SetColor(glowColor);
        if (ctx.outlineBrush) ctx.outlineBrush->SetColor(outlineColor);
        if (ctx.headCoreBrush) ctx.headCoreBrush->SetColor(headCoreColor);
    }

    ctx.renderTarget->BeginDraw();
    if (pDirtyRect) {
        const float tx = static_cast<float>(offset.x - pDirtyRect->left);
        const float ty = static_cast<float>(offset.y - pDirtyRect->top);
        ctx.renderTarget->SetTransform(D2D1::Matrix3x2F::Translation(tx, ty));
    } else {
        ctx.renderTarget->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    }
    ctx.renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    drawTrailGeometryDirect(
        ctx.renderTarget.Get(),
        ctx.lineBrush.Get(),
        ctx.glowBrush.Get(),
        ctx.outlineBrush.Get(),
        ctx.headCoreBrush.Get(),
        points,
        isRecognized,
        fadeAlpha,
        originX,
        originY);

    hr = ctx.renderTarget->EndDraw();
    ctx.surface->EndDraw();

    if (FAILED(hr)) {
        ctx.renderTarget.Reset();
        return false;
    }
    return true;
}

bool GestureTrailOverlay::renderToastToSurfaceLocked(
    GpuSurfaceContext& ctx,
    const std::string& resultText,
    bool isRecognized,
    bool excessive,
    int toastW,
    int toastH,
    float toastScale,
    float fadeAlpha,
    float pulseIntensity) {
    if (!ctx.surface || !m_dcompDevice || !m_d2dFactory) return false;

    POINT offset{};
    Microsoft::WRL::ComPtr<IDXGISurface> dxgiSurf;
    HRESULT hr = ctx.surface->BeginDraw(nullptr, IID_PPV_ARGS(dxgiSurf.GetAddressOf()), &offset);
    if (FAILED(hr) || !dxgiSurf) {
        LOG_WARN("DirectComposition 表面 BeginDraw 失败: hr=0x{:X}", hr);
        return false;
    }

    if (!ctx.renderTarget || ctx.dxgiSurface.Get() != dxgiSurf.Get()) {
        ctx.dxgiSurface = dxgiSurf;
        ctx.renderTarget.Reset();
        ctx.bgBrush.Reset();
        ctx.flashBrush.Reset();
        ctx.borderBrush.Reset();
        ctx.textBrush.Reset();
        ctx.excDotBrush.Reset();

        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = m_d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurf.Get(), rtProps, ctx.renderTarget.GetAddressOf());
        if (FAILED(hr) || !ctx.renderTarget) {
            ctx.surface->EndDraw();
            return false;
        }
    }

    ctx.renderTarget->BeginDraw();
    ctx.renderTarget->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    ctx.renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    drawToastContentDirect(
        ctx.renderTarget.Get(),
        ctx,
        resultText,
        isRecognized,
        excessive,
        toastW,
        toastH,
        toastScale,
        fadeAlpha,
        pulseIntensity);

    hr = ctx.renderTarget->EndDraw();
    ctx.surface->EndDraw();

    if (FAILED(hr)) {
        ctx.renderTarget.Reset();
        return false;
    }
    return true;
}

void GestureTrailOverlay::drawToastContentDirect(
    ID2D1RenderTarget* rt,
    GpuSurfaceContext& ctx,
    const std::string& resultText,
    bool isRecognized,
    bool excessive,
    int toastW,
    int toastH,
    float toastScale,
    float fadeAlpha,
    float pulseIntensity) {
    if (!rt) return;

    const tools3000::core::AccentColorRGB& trailRgb = m_cachedTrailRgb;
    const bool isLight = m_isLightTheme;

    const float resultScale = toastScale;
    const bool hasTextFormat = updateTextFormat(resultScale);
    const float centerX = static_cast<float>(toastW) * 0.5f;
    const float centerY = static_cast<float>(toastH) * 0.5f;

    if (excessive) {
        D2D1_COLOR_F bgColor = isLight
            ? D2D1::ColorF(0.92f, 0.18f, 0.24f, 0.94f * fadeAlpha)
            : D2D1::ColorF(0.52f, 0.08f, 0.12f, 0.92f * fadeAlpha);
        D2D1_COLOR_F borderColor = isLight
            ? D2D1::ColorF(0.78f, 0.10f, 0.16f, 0.85f * fadeAlpha)
            : D2D1::ColorF(0.96f, 0.28f, 0.36f, 0.85f * fadeAlpha);
        D2D1_COLOR_F dotColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha);

        if (!ctx.bgBrush) rt->CreateSolidColorBrush(bgColor, ctx.bgBrush.GetAddressOf());
        else ctx.bgBrush->SetColor(bgColor);

        if (!ctx.borderBrush) rt->CreateSolidColorBrush(borderColor, ctx.borderBrush.GetAddressOf());
        else ctx.borderBrush->SetColor(borderColor);

        if (!ctx.excDotBrush) rt->CreateSolidColorBrush(dotColor, ctx.excDotBrush.GetAddressOf());
        else ctx.excDotBrush->SetColor(dotColor);

        float boxW = 126.0f * resultScale;
        float boxH = 58.0f * resultScale;
        D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
            D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                        centerX + boxW / 2.0f, centerY + boxH / 2.0f),
            16.0f * resultScale, 16.0f * resultScale);
        if (ctx.bgBrush) rt->FillRoundedRectangle(&rrect, ctx.bgBrush.Get());
        if (ctx.borderBrush) rt->DrawRoundedRectangle(&rrect, ctx.borderBrush.Get(), 2.6f * resultScale);
        if (ctx.excDotBrush) {
            const float dotRadius = 6.0f * resultScale;
            const float dotSpacing = 22.0f * resultScale;
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX - dotSpacing, centerY), dotRadius, dotRadius), ctx.excDotBrush.Get());
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY), dotRadius, dotRadius), ctx.excDotBrush.Get());
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX + dotSpacing, centerY), dotRadius, dotRadius), ctx.excDotBrush.Get());
        }
    } else {
        const std::wstring wText = hasTextFormat
            ? tools3000::core::WinUtils::utf8ToWstring(resultText) : std::wstring{};
        if (hasTextFormat && !wText.empty() && m_dwriteFactory) {
            ComPtr<IDWriteTextLayout> layout;
            m_dwriteFactory->CreateTextLayout(
                wText.c_str(), static_cast<UINT32>(wText.length()),
                m_textFormat.Get(),
                10000.0f, 1000.0f,
                layout.GetAddressOf());

            float boxW = 140.0f * resultScale;
            float boxH = 58.0f * resultScale;
            if (layout) {
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) {
                    float paddingX = 38.0f * resultScale;
                    float paddingY = 16.0f * resultScale;
                    boxW = (std::max)(metrics.width + paddingX * 2.0f, 136.0f * resultScale);
                    boxH = (std::max)(metrics.height + paddingY * 2.0f, 58.0f * resultScale);
                }
            }

            D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                            centerX + boxW / 2.0f, centerY + boxH / 2.0f),
                16.0f * resultScale, 16.0f * resultScale);

            const bool isFading = m_fading.load(std::memory_order_relaxed);
            const bool isSuccess = (isRecognized || m_isRecognized.load(std::memory_order_relaxed));

            D2D1_COLOR_F bgColor;
            if (isFading && isSuccess) {
                const auto pulseBg = computeSuccessPulseBgColor(trailRgb, pulseIntensity);
                const float bgAlpha = (0.95f + 0.03f * pulseIntensity) * fadeAlpha;
                bgColor = D2D1::ColorF(pulseBg.r, pulseBg.g, pulseBg.b, bgAlpha);
            } else {
                bgColor = D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.82f * fadeAlpha);
            }
            D2D1_COLOR_F borderColor = (isFading && isSuccess)
                ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f * fadeAlpha)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f * fadeAlpha);
            D2D1_COLOR_F textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f * fadeAlpha);

            if (!ctx.bgBrush) rt->CreateSolidColorBrush(bgColor, ctx.bgBrush.GetAddressOf());
            else ctx.bgBrush->SetColor(bgColor);

            if (!ctx.borderBrush) rt->CreateSolidColorBrush(borderColor, ctx.borderBrush.GetAddressOf());
            else ctx.borderBrush->SetColor(borderColor);

            if (!ctx.textBrush) rt->CreateSolidColorBrush(textColor, ctx.textBrush.GetAddressOf());
            else ctx.textBrush->SetColor(textColor);

            const float flashAlpha = (isFading && isSuccess)
                ? computeSuccessPulseFlashAlpha(pulseIntensity, fadeAlpha) : 0.0f;
            if (flashAlpha > 0.005f) {
                D2D1_COLOR_F flashColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, flashAlpha);
                if (!ctx.flashBrush) rt->CreateSolidColorBrush(flashColor, ctx.flashBrush.GetAddressOf());
                else ctx.flashBrush->SetColor(flashColor);
            }

            if (ctx.bgBrush) rt->FillRoundedRectangle(&rrect, ctx.bgBrush.Get());
            if (flashAlpha > 0.005f && ctx.flashBrush) {
                rt->FillRoundedRectangle(&rrect, ctx.flashBrush.Get());
            }
            const float borderWidth = (isFading && isSuccess)
                ? computeSuccessPulseBorderWidth(2.6f, pulseIntensity, resultScale)
                : 2.6f * resultScale;
            if (ctx.borderBrush) rt->DrawRoundedRectangle(&rrect, ctx.borderBrush.Get(), borderWidth);
            if (ctx.textBrush && m_textFormat) {
                rt->DrawText(
                    wText.c_str(),
                    static_cast<UINT32>(wText.size()),
                    m_textFormat.Get(),
                    D2D1::RectF(centerX - boxW / 2.0f, centerY - boxH / 2.0f,
                                centerX + boxW / 2.0f, centerY + boxH / 2.0f),
                    ctx.textBrush.Get());
            }
        }
    }
}

void GestureTrailOverlay::drawTrailGeometryDirect(
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* lineBrush,
    ID2D1SolidColorBrush* glowBrush,
    ID2D1SolidColorBrush* outlineBrush,
    ID2D1SolidColorBrush* headCoreBrush,
    const std::vector<TrailPoint>& points,
    bool isRecognized,
    float fadeAlpha,
    int originX,
    int originY) {
    if (!rt || points.empty()) return;

    auto getPt = [&](size_t idx) -> D2D1_POINT_2F {
        return D2D1::Point2F(points[idx].x - originX, points[idx].y - originY);
    };

    ensureStrokeStyleLocked();

    if (points.size() >= 2 && m_d2dFactory) {
        ComPtr<ID2D1PathGeometry> linePath;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(linePath.GetAddressOf())) && linePath) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(linePath->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(getPt(0), D2D1_FIGURE_BEGIN_HOLLOW);
                if (points.size() == 2) {
                    sink->AddLine(getPt(1));
                } else {
                    // C1 连续二次贝塞尔中点平滑算法 (Smooth Quadratic Bezier Spline)
                    const D2D1_POINT_2F p0 = getPt(0);
                    const D2D1_POINT_2F p1 = getPt(1);
                    sink->AddLine(D2D1::Point2F((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f));
                    for (size_t i = 1; i < points.size() - 1; ++i) {
                        const D2D1_POINT_2F pi = getPt(i);
                        const D2D1_POINT_2F pi1 = getPt(i + 1);
                        const D2D1_POINT_2F mid = D2D1::Point2F((pi.x + pi1.x) * 0.5f, (pi.y + pi1.y) * 0.5f);
                        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pi, mid));
                    }
                    sink->AddLine(getPt(points.size() - 1));
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) {
                    const float coreW = (std::max)(m_style.lineWidth * m_dpiScale, 4.0f);
                    const float outlineW = clampTrailOutlineWidth(m_style.outlineWidth) * m_dpiScale;
                    const float whiteW = trailOutlineWidenWidth(coreW, outlineW);
                    if (whiteW > 0.0f && outlineBrush) {
                        rt->DrawGeometry(linePath.Get(), outlineBrush, whiteW, m_strokeStyle.Get());
                        if (glowBrush) {
                            glowBrush->SetOpacity(0.22f * fadeAlpha);
                            rt->DrawGeometry(linePath.Get(), glowBrush, coreW * 1.35f, m_strokeStyle.Get());
                        }
                    } else if (glowBrush) {
                        glowBrush->SetOpacity(0.28f * fadeAlpha);
                        rt->DrawGeometry(linePath.Get(), glowBrush, coreW * 2.4f, m_strokeStyle.Get());
                    }
                    if (lineBrush) {
                        rt->DrawGeometry(linePath.Get(), lineBrush, coreW, m_strokeStyle.Get());
                    }
                }
            }
        }

        // 起点圆头笔刷与发光增强 (Start Cap & Glow)，彻底消除起点平整直角裁切
        const float startR = (std::max)(m_style.lineWidth * m_dpiScale * 0.55f, 3.0f);
        const float outlineW = clampTrailOutlineWidth(m_style.outlineWidth) * m_dpiScale;
        if (glowBrush) {
            glowBrush->SetOpacity(0.25f * fadeAlpha);
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR * 2.2f, startR * 2.2f),
                glowBrush);
        }
        if (outlineBrush && outlineW > 0.0f) {
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR + outlineW, startR + outlineW),
                outlineBrush);
        }
        if (lineBrush) {
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR, startR),
                lineBrush);
        }

        // 终点头部能量晶体 (End Head Crystal)
        ID2D1SolidColorBrush* activeHead = (isRecognized || points.size() < 4)
            ? headCoreBrush
            : lineBrush;
        if (activeHead) {
            const float headR = (std::max)(m_style.lineWidth * m_dpiScale * 0.55f, 3.0f);
            if (outlineBrush && outlineW > 0.0f) {
                rt->FillEllipse(
                    D2D1::Ellipse(getPt(points.size() - 1), headR + outlineW, headR + outlineW),
                    outlineBrush);
            }
            rt->FillEllipse(
                D2D1::Ellipse(getPt(points.size() - 1), headR, headR), activeHead);
        }
    } else if (points.size() == 1) {
        // 单点起始能量点渲染：按下瞬间即刻呈现圆润微光，杜绝白板延迟与直角切面
        const float startR = (std::max)(m_style.lineWidth * m_dpiScale * 0.55f, 3.0f);
        const float outlineW = clampTrailOutlineWidth(m_style.outlineWidth) * m_dpiScale;
        if (glowBrush) {
            glowBrush->SetOpacity(0.28f * fadeAlpha);
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR * 2.2f, startR * 2.2f),
                glowBrush);
        }
        if (outlineBrush && outlineW > 0.0f) {
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR + outlineW, startR + outlineW),
                outlineBrush);
        }
        if (lineBrush) {
            rt->FillEllipse(
                D2D1::Ellipse(getPt(0), startR, startR),
                lineBrush);
        }
    }
}

void GestureTrailOverlay::drawTrailGeometry(ID2D1RenderTarget* rt,
                                           const std::vector<TrailPoint>& points,
                                           bool isRecognized,
                                           float fadeAlpha) {
    if (!rt || points.empty()) return;

    const tools3000::core::AccentColorRGB& trailRgb = m_cachedTrailRgb;
    const bool isLight = m_isLightTheme;

    D2D1_COLOR_F lineColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.96f * fadeAlpha);
    D2D1_COLOR_F glowColor = D2D1::ColorF(trailRgb.r, trailRgb.g, trailRgb.b, 0.28f * fadeAlpha);
    if (!isRecognized && points.size() >= 4) {
        if (isLight) {
            lineColor = D2D1::ColorF(0.28f, 0.31f, 0.38f, 0.96f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.28f * fadeAlpha);
        } else {
            lineColor = D2D1::ColorF(0.60f, 0.65f, 0.75f, 0.85f * fadeAlpha);
            glowColor = D2D1::ColorF(0.40f, 0.45f, 0.55f, 0.28f * fadeAlpha);
        }
    }
    const D2D1_COLOR_F outlineColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f * fadeAlpha);
    const D2D1_COLOR_F headCoreColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, fadeAlpha);

    if (rt == m_renderTarget.Get() && m_lineBrush && m_glowBrush && m_outlineBrush && m_headCoreBrush) {
        m_lineBrush->SetColor(lineColor);
        m_glowBrush->SetColor(glowColor);
        m_outlineBrush->SetColor(outlineColor);
        m_headCoreBrush->SetColor(headCoreColor);
        drawTrailGeometryDirect(
            rt,
            m_lineBrush.Get(),
            m_glowBrush.Get(),
            m_outlineBrush.Get(),
            m_headCoreBrush.Get(),
            points,
            isRecognized,
            fadeAlpha,
            m_originX,
            m_originY);
        return;
    }

    ComPtr<ID2D1SolidColorBrush> lineBrush;
    ComPtr<ID2D1SolidColorBrush> glowBrush;
    ComPtr<ID2D1SolidColorBrush> outlineBrush;
    ComPtr<ID2D1SolidColorBrush> headCoreBrush;
    rt->CreateSolidColorBrush(lineColor, lineBrush.GetAddressOf());
    rt->CreateSolidColorBrush(glowColor, glowBrush.GetAddressOf());
    rt->CreateSolidColorBrush(outlineColor, outlineBrush.GetAddressOf());
    rt->CreateSolidColorBrush(headCoreColor, headCoreBrush.GetAddressOf());

    drawTrailGeometryDirect(
        rt,
        lineBrush.Get(),
        glowBrush.Get(),
        outlineBrush.Get(),
        headCoreBrush.Get(),
        points,
        isRecognized,
        fadeAlpha,
        m_originX,
        m_originY);
}

void GestureTrailOverlay::applyVisualOpacityLocked(float alpha) {
    if (m_compositorReady && m_dcompDevice) {
        if (m_trailEffectGroup) {
            m_trailEffectGroup->SetOpacity(alpha);
        }
        if (m_toastEffectGroup) {
            m_toastEffectGroup->SetOpacity(alpha);
        }
        m_dcompDevice->Commit();
    }
}

void GestureTrailOverlay::resetVisualOpacityLocked() {
    applyVisualOpacityLocked(1.0f);
}

bool GestureTrailOverlay::render() {
    std::lock_guard lock(m_renderMutex);
    if (!m_fading.load(std::memory_order_relaxed)) {
        m_fadeAlpha.store(1.0f, std::memory_order_relaxed);
        m_pulseIntensity.store(0.0f, std::memory_order_relaxed);
    }

    if (m_themeDirty.exchange(false, std::memory_order_acq_rel)) {
        applyThemeColorsLocked();
    }

    std::vector<TrailPoint> points;
    std::string resultText;
    const bool isRecognized = m_isRecognized.load(std::memory_order_relaxed);
    {
        std::lock_guard trailLock(m_trailMutex);
        // 消费无锁 SPSC 环形队列中由输入线程推入的新轨迹点 (必须在持有 m_trailMutex 保护下修改 m_points，彻底杜绝数据竞态)
        TrailPoint drainedPt;
        while (m_pointQueue.pop(drainedPt)) {
            if (!m_points.empty()) {
                float dx = drainedPt.x - m_points.back().x;
                float dy = drainedPt.y - m_points.back().y;
                constexpr float minimumDelta = 1.0f;
                if (dx * dx + dy * dy < minimumDelta * minimumDelta) continue;
            }
            m_points.push_back(drainedPt);
        }

        if (m_points.empty()) return false;
        points = m_points;
        resultText = m_resultText;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const DisplayPacingInfo pacing = m_pacer.getPacingForPoint(cursor);
    m_dpiScale = pacing.dpiScale;

    // 毫秒级跟手：在非淡出态下将实时采样的物理光标尖端原子补入轨迹尾部，
    // 彻底抹平输入队列投递与 DWM 表面合成间的采样相位差，实现极致贴合跟手感
    const float lastPtX = points.empty() ? 0.0f : points.back().x;
    const float lastPtY = points.empty() ? 0.0f : points.back().y;
    if (gestureShouldInterpolateCursorTip(m_fading.load(std::memory_order_relaxed), !points.empty(),
                                          static_cast<float>(cursor.x), static_cast<float>(cursor.y),
                                          lastPtX, lastPtY)) {
        points.push_back({static_cast<float>(cursor.x), static_cast<float>(cursor.y), GetTickCount()});
    }
    const RECT toastWork = pacing.rcWork;
    const float toastScale = pacing.dpiScale;
    const int toastCenterX = (toastWork.left + toastWork.right) / 2;
    const int toastCenterY = toastWork.top +
        static_cast<int>(static_cast<float>(toastWork.bottom - toastWork.top) * 0.82f);

    constexpr int kSafetyMargin = 32;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (const auto& p : points) {
        left = (std::min)(left, static_cast<int>(p.x) - kSafetyMargin);
        top = (std::min)(top, static_cast<int>(p.y) - kSafetyMargin);
        right = (std::max)(right, static_cast<int>(p.x) + 1 + kSafetyMargin);
        bottom = (std::max)(bottom, static_cast<int>(p.y) + 1 + kSafetyMargin);
    }
    bool alreadyRendered = false;
    if (!fitSurface(left, top, right, bottom, points, isRecognized, alreadyRendered)) return false;

    bool renderedOk = false;
    bool dcompUsed = false;

    if (alreadyRendered) {
        renderedOk = true;
        dcompUsed = true;
    } else if (m_compositorReady && m_frontCtx.surface && m_dcompDevice) {
        // 累积手势包围盒管线 (Cumulative Bounding Box Pipeline)
        const RECT currentBox = computeTrailDirtyRect(
            points, m_originX, m_originY, m_trailDcompW, m_trailDcompH);

        const RECT dirtyRect = unionAndClampStrokeDirtyRect(
            m_accumulatedStrokeDirtyRect, currentBox, m_trailDcompW, m_trailDcompH);

        if (renderTrailToSurfaceLocked(m_frontCtx, points, isRecognized, m_fadeAlpha, m_originX, m_originY, &dirtyRect)) {
            m_accumulatedStrokeDirtyRect = dirtyRect;
            m_trailDcompVisual->SetContent(m_frontCtx.surface.Get());

            if (!IsWindowVisible(m_hwnd)) {
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            }
            m_visible.store(true, std::memory_order_release);
            renderedOk = true;
            dcompUsed = true;
        } else {
            LOG_WARN("手势轨迹后台表面渲染失败");
        }
    }

    if (!renderedOk) {
        if (!m_renderTarget || !m_lineBrush) {
            if (!createD2DResources()) return false;
        }
        if (m_themeDirty.exchange(false, std::memory_order_acq_rel)) {
            applyThemeColorsLocked();
        }
        if (!m_renderTarget || !m_lineBrush || !m_memoryDC) return false;

        RECT memRect = {0, 0, m_width, m_height};
        if (FAILED(m_renderTarget->BindDC(m_memoryDC, &memRect))) return false;

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
        drawTrailGeometry(m_renderTarget.Get(), points, isRecognized, m_fadeAlpha);
        if (FAILED(m_renderTarget->EndDraw())) {
            LOG_WARN("手势轨迹 Direct2D 帧提交失败");
            return false;
        }

        ensurePremultipliedAlpha(m_memoryBits, m_width, m_height, m_memoryPitch);

        if (!presentLayeredLocked(m_hwnd, m_memoryDC, m_originX, m_originY, m_width, m_height)) {
            return false;
        }
        m_visible.store(true, std::memory_order_release);
        renderedOk = true;
    }

    const bool isExcessive = (resultText == "•••");
    const bool shouldShowToast = shouldShowGestureResultToast(
        isRecognized, !resultText.empty(), isExcessive);
    bool toastOk = true;
    if (shouldShowToast) {
        toastOk = presentToastLocked(resultText, isRecognized, isExcessive,
                                     toastCenterX, toastCenterY, toastScale, m_pulseIntensity.load(std::memory_order_relaxed));
    } else {
        hideToastWindow();
    }

    if (m_compositorReady && m_dcompDevice && (dcompUsed || (shouldShowToast && m_toastFrontCtx.surface))) {
        m_dcompDevice->Commit();
    }

    const uint64_t epoch = m_trailEpoch.load(std::memory_order_relaxed);
    if (m_loggedPresentEpoch != epoch) {
        m_loggedPresentEpoch = epoch;
        LOG_INFO("手势轨迹已提交: points={}, {}x{}, dcomp={}", points.size(), m_width, m_height, m_compositorReady);
    }
    return gestureFrameReadyToFade(true, shouldShowToast, toastOk);
}

bool GestureTrailOverlay::presentToastLocked(const std::string& resultText, bool recognized,
                                             bool excessive, int toastCenterX, int toastCenterY,
                                             float toastScale, float pulseIntensity) {
    const int toastW = static_cast<int>(400.0f * toastScale);
    const int toastH = static_cast<int>(120.0f * toastScale);
    m_toastOriginX = toastCenterX - toastW / 2;
    m_toastOriginY = toastCenterY - toastH / 2;

    if (m_compositorReady && m_toastDcompVisual && m_dcompDevice) {
        if (!m_toastFrontCtx.surface || !m_toastBackCtx.surface ||
            m_toastDcompW < toastW || m_toastDcompH < toastH) {
            m_toastFrontCtx.reset();
            m_toastBackCtx.reset();
            HRESULT hr1 = m_dcompDevice->CreateSurface(
                static_cast<UINT>(toastW), static_cast<UINT>(toastH),
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                m_toastFrontCtx.surface.GetAddressOf());
            HRESULT hr2 = m_dcompDevice->CreateSurface(
                static_cast<UINT>(toastW), static_cast<UINT>(toastH),
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
                m_toastBackCtx.surface.GetAddressOf());
            if (FAILED(hr1) || FAILED(hr2) || !m_toastFrontCtx.surface || !m_toastBackCtx.surface) {
                LOG_WARN("创建 DirectComposition Toast 双表面失败: hr1=0x{:X}, hr2=0x{:X}", hr1, hr2);
                return false;
            }
            m_toastDcompW = toastW;
            m_toastDcompH = toastH;
        }

        if (renderToastToSurfaceLocked(m_toastBackCtx, resultText, recognized, excessive, toastW, toastH, toastScale, m_fadeAlpha, pulseIntensity)) {
            m_toastDcompVisual->SetContent(m_toastBackCtx.surface.Get());
            std::swap(m_toastFrontCtx, m_toastBackCtx);
            m_toastDcompSurface = m_toastFrontCtx.surface;

            // 硬件级绝对零开销位移：直接通过 DComp Visual SetOffsetX/Y 实现，
            // 彻底消除 Win32 SetWindowPos、USER32 窗口树互斥锁与跨进程同步
            const float localX = static_cast<float>(m_toastOriginX - m_virtualX);
            const float localY = static_cast<float>(m_toastOriginY - m_virtualY);
            if (m_lastToastOriginX != m_toastOriginX || m_lastToastOriginY != m_toastOriginY) {
                m_lastToastOriginX = m_toastOriginX;
                m_lastToastOriginY = m_toastOriginY;
                m_toastDcompVisual->SetOffsetX(localX);
                m_toastDcompVisual->SetOffsetY(localY);
            }

            // 硬件级不透明度控制与淡出
            if (m_toastEffectGroup) {
                m_toastEffectGroup->SetOpacity(m_fadeAlpha.load(std::memory_order_relaxed));
            }
            return true;
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK GestureTrailOverlay::overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<GestureTrailOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {

        case WM_GETOBJECT:
            return tools3000::core::accessibility::respondToOverlayUiaGetObject(
                hwnd, wParam, lParam,
                {L"Tools3000.GestureTrail", L"Mouse gesture trail",
                 tools3000::core::accessibility::OverlayUiaRole::Pane, false});

        case WM_GESTURE_ACCESSIBILITY_RESULT:
            if (self) {
                std::string result;
                {
                    std::lock_guard lock(self->m_trailMutex);
                    result = self->m_resultText;
                }
                if (!result.empty()) {
                    tools3000::core::accessibility::announceOverlay(
                        hwnd, tools3000::core::WinUtils::utf8ToWstring(result));
                }
            }
            return 0;

        case WM_DISPLAYCHANGE: {
            if (self) {
                self->m_pacer.invalidateCache();
                self->handleDisplayChange();
            }
            return 0;
        }

        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_NCDESTROY:
            tools3000::core::accessibility::disconnectOverlayUiaProvider(hwnd);
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void GestureTrailOverlay::handleDisplayChange() {
    std::lock_guard lock(m_renderMutex);
    const int newVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int newVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int newVw = (std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int newVh = (std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    if (m_virtualX == newVx && m_virtualY == newVy &&
        m_virtualW == newVw && m_virtualH == newVh &&
        m_frontCtx.surface) {
        return;
    }

    m_virtualX = newVx;
    m_virtualY = newVy;
    m_virtualW = newVw;
    m_virtualH = newVh;
    m_originX = newVx;
    m_originY = newVy;
    m_width = newVw;
    m_height = newVh;

    if (m_hwnd && IsWindow(m_hwnd)) {
        SetWindowPos(m_hwnd, nullptr, m_originX, m_originY, m_width, m_height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (m_compositorReady && m_dcompDevice) {
        preallocateTrailSurfacesLocked(newVw, newVh);
    }
    LOG_INFO("Display topology change adapted: virtual screen ({},{}) {}x{}", newVx, newVy, newVw, newVh);
}

}  // namespace tools3000::gesture
