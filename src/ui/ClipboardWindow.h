#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClipboardWindow.h — 极速呼出剪贴板历史无边框悬浮面板 (WebView2 独立轻量表面)
//
// 职责:
//   1. 全局热键 (默认 Alt+V) 毫秒级呼出于鼠标所在显示器，自动记忆先前前台窗口；
//   2. 映射 %LOCALAPPDATA%\Tools3000\clipboard\blobs 到 clipboard-blobs.tools3000.local，
//      图片历史缩略图零 Base64 内存开销流式渲染；
//   3. 严格接入 applyUniversalRoundedCorners 双保险、KeyboardPipeline 与 60s 闲置自动销毁。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_CLIPBOARDWINDOW_H
#define TOOLS3000_UI_CLIPBOARDWINDOW_H

#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "core/utils/DpiUtils.h"
#include "ui/WebViewSuspend.h"

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace tools3000::ui {

class ClipboardWindow final {
public:
    static ClipboardWindow& instance();

    void toggle(HINSTANCE hInstance = nullptr);
    void show(HINSTANCE hInstance = nullptr);
    void hide();
    void destroy();

    bool isVisible() const noexcept;
    bool isForegroundActive() const noexcept;
    bool isPinned() const noexcept { return m_pinned; }
    bool isMaximized() const noexcept { return m_maximized; }
    void setPinned(bool pinned);
    void toggleMaximize();
    HWND getHwnd() const noexcept { return m_hwnd; }
    HWND previousForegroundHwnd() const noexcept { return m_previousForegroundHwnd; }

    /// 纯数学多屏定位描述与求解输入（支持 100% 无 HWND 确定性多屏混合 DPI 单元测试）
    struct MonitorLayoutTarget {
        std::string deviceName;
        RECT workArea{};
        unsigned dpi = 96;
        bool isPrimary = false;
    };

    struct PlacementInput {
        std::string mode = "follow_mouse";  // "follow_mouse" | "remember_last" | "fixed"
        int widthDip = 760;
        int heightDip = 560;
        bool hasAnchor = false;
        int anchorX = 0;
        int anchorY = 0;
        double anchorRelX = 0.5;
        double anchorRelY = 0.38;
        std::string anchorMonitorDevice;
    };

    struct ResolvedPlacement {
        RECT rect{};
        unsigned targetDpi = 96;
        std::string targetMonitorDevice;
        bool usedFallbackMonitor = false;
    };

    /// 世界级多屏零错乱定位求解器：
    /// - follow_mouse: 恒定居中于当前活动/鼠标显示器黄金视区，严格按目标屏 DPI 换算逻辑 DIP 宽高；
    /// - remember_last / fixed: 优先命中记录显示器与精确坐标；当外接副屏拔掉时按相对锚点安全回退主屏可见区，绝不掉出屏幕外。
    static inline ResolvedPlacement resolveTargetPlacement(
        const PlacementInput& input,
        const std::vector<MonitorLayoutTarget>& monitors,
        const std::string& activeMonitorDevice) noexcept {
        ResolvedPlacement out{};
        if (monitors.empty()) {
            out.rect = RECT{100, 100, 100 + std::clamp(input.widthDip, 560, 2400),
                            100 + std::clamp(input.heightDip, 420, 1600)};
            out.targetDpi = 96;
            return out;
        }

        auto findByDevice = [&](const std::string& dev) -> const MonitorLayoutTarget* {
            if (dev.empty()) return nullptr;
            for (const auto& m : monitors) {
                if (_stricmp(m.deviceName.c_str(), dev.c_str()) == 0) return &m;
            }
            return nullptr;
        };
        auto findPrimary = [&]() -> const MonitorLayoutTarget* {
            for (const auto& m : monitors) {
                if (m.isPrimary) return &m;
            }
            return &monitors.front();
        };

        const MonitorLayoutTarget* activeMon = findByDevice(activeMonitorDevice);
        if (!activeMon) activeMon = findPrimary();

        const bool useSavedAnchor =
            (input.mode == "fixed" || input.mode == "remember_last") && input.hasAnchor;

        const MonitorLayoutTarget* targetMon = activeMon;
        bool exactMonitorFound = false;
        if (useSavedAnchor) {
            if (const auto* byDev = findByDevice(input.anchorMonitorDevice)) {
                targetMon = byDev;
                exactMonitorFound = true;
            } else {
                // 兼容仅填写了 X/Y 坐标但未绑定设备名的场景：按坐标落点反查显示器
                for (const auto& m : monitors) {
                    if (input.anchorX >= m.workArea.left && input.anchorX < m.workArea.right &&
                        input.anchorY >= m.workArea.top && input.anchorY < m.workArea.bottom) {
                        targetMon = &m;
                        exactMonitorFound = true;
                        break;
                    }
                }
                if (!exactMonitorFound) {
                    targetMon = findPrimary();
                    out.usedFallbackMonitor = true;
                }
            }
        }

        const RECT work = targetMon->workArea;
        const unsigned dpi = (targetMon->dpi >= 72) ? targetMon->dpi : 96u;
        const float scale = tools3000::core::dpi::scaleForDpi(dpi);
        const int minW = tools3000::core::dpi::scaleMetric(560, scale);
        const int minH = tools3000::core::dpi::scaleMetric(420, scale);
        const int workW = (std::max)(minW, static_cast<int>(work.right - work.left));
        const int workH = (std::max)(minH, static_cast<int>(work.bottom - work.top));

        const int desiredDipW = std::clamp(input.widthDip, 560, 2400);
        const int desiredDipH = std::clamp(input.heightDip, 420, 1600);
        int targetW = std::clamp(tools3000::core::dpi::scaleMetric(desiredDipW, scale), minW, workW);
        int targetH = std::clamp(tools3000::core::dpi::scaleMetric(desiredDipH, scale), minH, workH);

        int x = 0;
        int y = 0;
        if (!useSavedAnchor) {
            // follow_mouse 或首次尚未记录锚点：在当前活动显示器黄金视区居中（左右预留边距防紧贴）
            const int maxCenterW = (std::max)(minW, workW - 32);
            const int maxCenterH = (std::max)(minH, workH - 32);
            targetW = (std::min)(targetW, maxCenterW);
            targetH = (std::min)(targetH, maxCenterH);
            x = work.left + (workW - targetW) / 2;
            y = work.top + static_cast<int>((workH - targetH) * 0.38);
        } else if (exactMonitorFound) {
            // 精准落位至已记录显示器坐标，并严密钳制不掉出工作区边界
            x = std::clamp(input.anchorX, static_cast<int>(work.left), static_cast<int>(work.right - targetW));
            y = std::clamp(input.anchorY, static_cast<int>(work.top), static_cast<int>(work.bottom - targetH));
        } else {
            // 目标外接副屏已断开：按归一化相对比例平滑回退至主屏对应可视位置，插回副屏后自动恢复原固定位
            const double relX = std::clamp(input.anchorRelX, 0.0, 1.0);
            const double relY = std::clamp(input.anchorRelY, 0.0, 1.0);
            x = work.left + static_cast<int>(std::lround((workW - targetW) * relX));
            y = work.top + static_cast<int>(std::lround((workH - targetH) * relY));
            x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right - targetW));
            y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom - targetH));
        }

        out.rect = RECT{x, y, x + targetW, y + targetH};
        out.targetDpi = dpi;
        out.targetMonitorDevice = targetMon->deviceName;
        return out;
    }

    /// 将当前剪贴板窗口的实际位置与逻辑 DIP 尺寸一键捕获并固定（供浮窗定位按钮与设置页共用）
    bool captureCurrentBoundsAsFixed();

    /// 在「固定当前位置与大小」与「跟随当前显示器」之间一键切换（供浮窗标题栏按钮调用）
    bool toggleFixedPlacement();

    /// 重置剪贴板窗口位置与尺寸为出厂默认（760x560 DIP + 跟随当前显示器）
    void resetPlacementToDefault();

    /// 当设置页修改了位置模式/固定坐标/尺寸后，若窗口正可见则立即实时生效
    void applyPlacementSettingsNow();

    /// 过滤空句柄、自身及其子窗口与不可见/任务栏窗口，归一化为顶层 GA_ROOT 句柄
    static HWND resolveValidTargetForegroundHwnd(HWND candidate, HWND selfHwnd) noexcept;

    /// 判定按下呼出热键时应隐藏还是强力前置显示（冷启等待中或掉到后台被遮挡时一律前置显示，绝不误关）
    static constexpr bool shouldHideOnToggle(
        bool visible,
        bool pendingReveal,
        bool foregroundActive) noexcept {
        return visible && !pendingReveal && foregroundActive;
    }

    /// 在任意线程原子预武装不关闭粘贴宽限期（必须在 activateItem 切走前台焦点之前调用，彻底消除跨线程竞态）
    void armPasteKeepOpenGrace() noexcept;

    /// 在主线程标记本次粘贴后保持面板打开并临时浮顶（用于 hideOnPaste=false && autoPaste=true 期间防压底与误杀）
    void markPasteKeepOpenSession();

    /// 判定失焦时是否允许自动隐藏剪贴板窗口（受 hideOnBlur 总控；置顶、缩放、原生拖拽、拖拽驻留期、拖拽及不关粘贴宽限期内严禁误隐藏）
    static bool shouldAutoHideOnDeactivate(
        bool visible,
        bool pendingReveal,
        bool pinned,
        bool inSizeMove,
        bool inNativeDrag,
        uint64_t nowTick,
        uint64_t showTick,
        uint64_t lastNativeDragEndTick,
        bool dragSessionHold = false,
        bool hideOnBlurEnabled = true,
        uint64_t lastPasteKeepOpenTick = 0) noexcept {
        if (!hideOnBlurEnabled || !visible || pendingReveal || pinned || inSizeMove || inNativeDrag || dragSessionHold) {
            return false;
        }
        if (nowTick >= showTick && (nowTick - showTick) <= 200) {
            return false;
        }
        if (lastNativeDragEndTick != 0 &&
            nowTick >= lastNativeDragEndTick &&
            (nowTick - lastNativeDragEndTick) < NATIVE_DRAG_HIDE_GRACE_MS) {
            return false;
        }
        if (lastPasteKeepOpenTick != 0 &&
            nowTick >= lastPasteKeepOpenTick &&
            (nowTick - lastPasteKeepOpenTick) < PASTE_KEEP_OPEN_GRACE_MS) {
            return false;
        }
        return true;
    }

    /// 向已创建的剪贴板 WebView 推送实时更新或主题事件
    void pushEventJson(const std::string& envelopeJson);
    void applyThemeChrome();

private:
    ClipboardWindow() = default;
    ~ClipboardWindow();
    ClipboardWindow(const ClipboardWindow&) = delete;
    ClipboardWindow& operator=(const ClipboardWindow&) = delete;

    bool createWindow(HINSTANCE hInstance);
    void initializeWebView2();
    void updatePlacement();
    void revealWindowNow();
    void enterDeferredSuspend();
    void notifyShownToWeb();
    void notifyWindowStateToWeb();
    void installForegroundTracker();
    void uninstallForegroundTracker();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundWinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    static constexpr UINT_PTR IDT_IDLE_DESTROY = 2102;
    static constexpr UINT_PTR IDT_DEFERRED_SUSPEND = 2103;
    static constexpr UINT_PTR IDT_REVEAL_FALLBACK = 2104;
    static constexpr UINT_PTR IDT_PASTE_TOPMOST_RELEASE = 2105;
    // 高频热保活宽限期 (25s)：隐藏后 25 秒内保留 GPU 合成帧不立刻 TrySuspend/trimWorkingSet，
    // 连续呼出 <16ms 零黑屏瞬开；超 25s 软挂起降内存，超 5 分钟彻底销毁归还全部物理内存。
    static constexpr UINT DEFERRED_SUSPEND_DELAY_MS = 25000;
    static constexpr UINT IDLE_DESTROY_TIMEOUT_MS = 90000;
    static constexpr UINT REVEAL_FALLBACK_WARM_MS = 450;
    static constexpr UINT REVEAL_FALLBACK_COLD_MS = 1200;
    static constexpr uint64_t NATIVE_DRAG_HIDE_GRACE_MS = 1200;
    static constexpr uint64_t PASTE_KEEP_OPEN_GRACE_MS = 1200;
    static constexpr UINT WM_CLIPBOARD_VERIFY_DEACTIVATED = WM_APP + 29;
    static constexpr UINT WM_CLIPBOARD_CAPTURE_FOREGROUND = WM_APP + 30;

    HWND m_hwnd = nullptr;
    HWND m_previousForegroundHwnd = nullptr;
    HWINEVENTHOOK m_fgEventHook = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<bool> m_inNativeDrag{false};
    bool m_webViewReady = false;
    bool m_suspendedOrCold = true;
    bool m_pendingReveal = false;
    bool m_pinned = false;
    bool m_dragSessionHold = false;
    bool m_maximized = false;
    bool m_inSizeMove = false;
    bool m_applyingProgrammaticPlacement = false;
    bool m_hasCustomBounds = false;
    RECT m_customBounds{};
    uint64_t m_showTimeTick = 0;
    uint64_t m_lastNativeDragEndTick = 0;
    std::atomic<uint64_t> m_lastPasteKeepOpenTick{0};
    bool m_pasteTopmostHold = false;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_CLIPBOARDWINDOW_H
