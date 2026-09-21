#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// GestureInputPolicy — 手势触发键与取消条件的纯判定
//
// 这些规则必须能在没有钩子、没有窗口的情况下单独验证：一旦写进 MouseHook /
// GestureEngine 的回调里，再测就要装全局钩子。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_GESTURE_GESTUREINPUTPOLICY_H
#define TOOLS3000_GESTURE_GESTUREINPUTPOLICY_H

#include <windows.h>
#include "gesture/MouseHook.h"
#include "core/utils/ThemeUtils.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <string>

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace tools3000::gesture {

/// 左键手势触发合法性校验：仅当对应边缘确实开启了滑动/左键手势或全局显式开启左键时才允许拦截
inline bool isLeftButtonGestureAllowed(ScreenEdgeZone activeEdge, uint8_t modifiers, uint32_t triggerMask) noexcept {
    (void)modifiers;
    if (activeEdge == ScreenEdgeZone::Left) {
        return (triggerMask & (GestureTriggerMask::EdgeLeftSlide | GestureTriggerMask::Left)) != 0;
    }
    if (activeEdge == ScreenEdgeZone::Right) {
        return (triggerMask & (GestureTriggerMask::EdgeRightSlide | GestureTriggerMask::Left)) != 0;
    }
    if (activeEdge == ScreenEdgeZone::Bottom) {
        return (triggerMask & (GestureTriggerMask::EdgeBottomSlide | GestureTriggerMask::Left)) != 0;
    }
    if (activeEdge == ScreenEdgeZone::Top) {
        return (triggerMask & (GestureTriggerMask::EdgeTopSlide | GestureTriggerMask::EdgeTopLeft | GestureTriggerMask::Left)) != 0;
    }
    return (triggerMask & GestureTriggerMask::Left) != 0;
}

/// 生成符合规范的完整手势编码: [EdgePrefix+][ModPrefix+][TriggerPrefix+]bareCode
inline std::string formatFullGestureCode(ScreenEdgeZone edge, uint8_t modifiers,
                                         MouseEventType triggerDown,
                                         std::string_view bareCode) {
    std::string code;
    code.reserve(32);
    if (edge == ScreenEdgeZone::Top)         code += "TopEdge+";
    else if (edge == ScreenEdgeZone::Bottom) code += "BottomEdge+";
    else if (edge == ScreenEdgeZone::Left)   code += "LeftEdge+";
    else if (edge == ScreenEdgeZone::Right)  code += "RightEdge+";

    if (modifiers & MOUSE_MOD_CTRL)  code += "Ctrl+";
    if (modifiers & MOUSE_MOD_ALT)   code += "Alt+";
    if (modifiers & MOUSE_MOD_SHIFT) code += "Shift+";

    if (triggerDown == MouseEventType::MiddleDown)      code += "Middle+";
    else if (triggerDown == MouseEventType::X1Down)    code += "X1+";
    else if (triggerDown == MouseEventType::X2Down)    code += "X2+";
    else if (triggerDown == MouseEventType::LeftDown)  code += "Left+";

    code.append(bareCode.data(), bareCode.size());
    return code;
}

/// 判定是否为鼠标滚轮滚动事件
inline bool isWheelEvent(MouseEventType type) noexcept {
    return type == MouseEventType::WheelUp || type == MouseEventType::WheelDown;
}

/// 获取滚轮事件对应的裸手势编码 ("WheelUp" / "WheelDown")
inline const char* wheelBareCode(MouseEventType type) noexcept {
    if (type == MouseEventType::WheelUp) return "WheelUp";
    if (type == MouseEventType::WheelDown) return "WheelDown";
    return "";
}

/// 判定手势触发键抬起时是否应当抑制右键上下文菜单（滚轮手势已执行或识别到有效划动手势）
inline bool shouldSuppressContextMenuOnRelease(bool wheelExecuted, bool strokeExecuted) noexcept {
    return wheelExecuted || strokeExecuted;
}

/// 判定物理虚拟按键码是否对应当前的触发事件类型。
/// 注意：在低级鼠标钩子 WH_MOUSE_LL 拦截按下后，Windows 内核不会更新全局按键状态数组 gafAsyncKeyState，
/// 严禁在手势 Move / 看门狗热路径中使用 GetAsyncKeyState 检测触发键物理状态！
inline int physicalVkForTriggerDown(MouseEventType downEvent) noexcept {
    switch (downEvent) {
        case MouseEventType::RightDown:  return VK_RBUTTON;
        case MouseEventType::MiddleDown: return VK_MBUTTON;
        case MouseEventType::X1Down:     return VK_XBUTTON1;
        case MouseEventType::X2Down:     return VK_XBUTTON2;
        case MouseEventType::LeftDown:   return VK_LBUTTON;
        default:                         return 0;
    }
}

/// 判定虚拟桌面切换手势是否处于防抖/节流冷却期中 (默认最小间隔 120ms)
inline bool isDesktopSwitchThrottled(int64_t elapsedMs, int64_t minIntervalMs = 120) noexcept {
    return elapsedMs >= 0 && elapsedMs < minIntervalMs;
}

/// 当前触发模式与触发掩码下，这个按下事件是否应当开始一笔手势。
inline bool isGestureTriggerDown(MouseEventType type, TriggerMode mode,
                                 uint32_t triggerMask = GestureTriggerMask::AllTriggers,
                                 ScreenEdgeZone edgeZone = ScreenEdgeZone::None) noexcept {
    if (type == MouseEventType::RightDown) {
        if (triggerMask != GestureTriggerMask::AllTriggers && (triggerMask & GestureTriggerMask::Right) == 0) return false;
        return mode == TriggerMode::RightOnly || mode == TriggerMode::Both || mode == TriggerMode::All;
    }
    if (type == MouseEventType::MiddleDown) {
        if (triggerMask != GestureTriggerMask::AllTriggers && (triggerMask & GestureTriggerMask::Middle) == 0) return false;
        return mode == TriggerMode::MiddleOnly || mode == TriggerMode::Both || mode == TriggerMode::All;
    }
    if (type == MouseEventType::X1Down) {
        if (triggerMask != GestureTriggerMask::AllTriggers && (triggerMask & GestureTriggerMask::X1) == 0) return false;
        return mode == TriggerMode::Both || mode == TriggerMode::All || mode == TriggerMode::X1Only;
    }
    if (type == MouseEventType::X2Down) {
        if (triggerMask != GestureTriggerMask::AllTriggers && (triggerMask & GestureTriggerMask::X2) == 0) return false;
        return mode == TriggerMode::Both || mode == TriggerMode::All || mode == TriggerMode::X2Only;
    }
    if (type == MouseEventType::LeftDown) {
        if (triggerMask == GestureTriggerMask::AllTriggers && edgeZone == ScreenEdgeZone::None) {
            return true; // 兼容旧单测无掩码传参
        }
        return isLeftButtonGestureAllowed(edgeZone, 0, triggerMask);
    }
    return false;
}

/// 与按下配对的抬起事件。必须按实际按下的键配对，不能用配置项里的默认右键。
inline MouseEventType triggerUpFor(MouseEventType downEvent) noexcept {
    switch (downEvent) {
        case MouseEventType::MiddleDown: return MouseEventType::MiddleUp;
        case MouseEventType::X1Down:     return MouseEventType::X1Up;
        case MouseEventType::X2Down:     return MouseEventType::X2Up;
        case MouseEventType::LeftDown:   return MouseEventType::LeftUp;
        default:                         return MouseEventType::RightUp;
    }
}

/// 追踪过程中这些事件应立刻取消手势并放行（非当前触发键的其他鼠标键按下）。
/// 与当前触发键相同的再次按下不取消：那是状态机失步时的噪声，由钩子侧闩锁处理。
inline bool cancelsGestureTracking(MouseEventType type, MouseEventType activeTriggerDown) noexcept {
    if (type == MouseEventType::LeftDown || type == MouseEventType::LeftUp) {
        return activeTriggerDown != MouseEventType::LeftDown;
    }
    if (type == MouseEventType::RightDown && activeTriggerDown != MouseEventType::RightDown) {
        return true;
    }
    if (type == MouseEventType::MiddleDown && activeTriggerDown != MouseEventType::MiddleDown) {
        return true;
    }
    if (type == MouseEventType::X1Down && activeTriggerDown != MouseEventType::X1Down) {
        return true;
    }
    if (type == MouseEventType::X2Down && activeTriggerDown != MouseEventType::X2Down) {
        return true;
    }
    return false;
}

inline bool shouldShowGestureResultToast(bool recognized, bool hasResultText,
                                         bool excessive) noexcept {
    return excessive || (recognized && hasResultText);
}

/// 绘制过程中覆盖层只扩大、不收缩、不挪原点；否则卡片一闪窗口就搬一次，轨迹会抽搐。
inline void growOverlayRect(int& left, int& top, int& right, int& bottom,
                            int originX, int originY, int width, int height) noexcept {
    if (width <= 0 || height <= 0) return;
    left = (std::min)(left, originX);
    top = (std::min)(top, originY);
    right = (std::max)(right, originX + width);
    bottom = (std::max)(bottom, originY + height);
}

inline int snapDown(int value, int grid) noexcept {
    if (grid <= 0) return value;
    if (value >= 0) return (value / grid) * grid;
    return -(((-value + grid - 1) / grid) * grid);
}

inline int snapUp(int value, int grid) noexcept {
    if (grid <= 0) return value;
    if (value >= 0) return ((value + grid - 1) / grid) * grid;
    return -((-value / grid) * grid);
}

/// 阶梯扩容与包围盒步进计算 (1024px 充裕步进，128px 防抖回差，锁定空间原点防抽搐)
inline void computeOverlaySurfaceBounds(
    int left, int top, int right, int bottom,
    int originX, int originY, int currentW, int currentH,
    bool isLive,
    int virtualX, int virtualY, int virtualW, int virtualH,
    int& outLeft, int& outTop, int& outRight, int& outBottom) noexcept {
    constexpr int kPad = 128;
    constexpr int kMin = 1024;
    constexpr int kScreenMargin = 64;

    const int boundLeft = virtualX - kScreenMargin;
    const int boundTop = virtualY - kScreenMargin;
    const int boundRight = virtualX + virtualW + kScreenMargin;
    const int boundBottom = virtualY + virtualH + kScreenMargin;

    if (!isLive || currentW <= 0 || currentH <= 0) {
        // 初始手势帧：以起始点为中心预分配 1024x1024 充裕包围盒，向各方向提供 >= 500px 缓冲空间
        const int centerX = (left + right) / 2;
        const int centerY = (top + bottom) / 2;
        int expLeft = centerX - kMin / 2;
        int expTop = centerY - kMin / 2;
        int expRight = expLeft + kMin;
        int expBottom = expTop + kMin;

        // 若起点靠近虚拟屏边缘，平滑平移以完整容纳 1024x1024 视口
        if (expLeft < boundLeft) {
            expRight = (std::min)(boundRight, expRight + (boundLeft - expLeft));
            expLeft = boundLeft;
        }
        if (expRight > boundRight) {
            expLeft = (std::max)(boundLeft, expLeft - (expRight - boundRight));
            expRight = boundRight;
        }
        if (expTop < boundTop) {
            expBottom = (std::min)(boundBottom, expBottom + (boundTop - expTop));
            expTop = boundTop;
        }
        if (expBottom > boundBottom) {
            expTop = (std::max)(boundTop, expTop - (expBottom - boundBottom));
            expBottom = boundBottom;
        }

        // 与 snapDown 对齐保证边界整齐
        if (virtualX >= 0 && expLeft < virtualX) expLeft = virtualX;
        if (virtualY >= 0 && expTop < virtualY) expTop = virtualY;
        if (expRight - expLeft < kMin) expRight = (std::min)(boundRight, expLeft + kMin);
        if (expBottom - expTop < kMin) expBottom = (std::min)(boundBottom, expTop + kMin);

        outLeft = expLeft;
        outTop = expTop;
        outRight = expRight;
        outBottom = expBottom;
        return;
    }

    // 活态划动中 (isLive = true)：死锁已确立的原点 (originX, originY)，绝不轻易平移窗口原点引起抽搐
    int expLeft = originX;
    int expTop = originY;
    int expRight = originX + currentW;
    int expBottom = originY + currentH;

    if (left - kPad < expLeft) {
        expLeft = snapDown(left - kPad, 512);
        expLeft = (std::max)(expLeft, boundLeft);
    }
    if (top - kPad < expTop) {
        expTop = snapDown(top - kPad, 512);
        expTop = (std::max)(expTop, boundTop);
    }
    if (right + kPad > expRight) {
        expRight = snapUp(right + kPad, 512);
        expRight = (std::min)(expRight, boundRight);
    }
    if (bottom + kPad > expBottom) {
        expBottom = snapUp(bottom + kPad, 512);
        expBottom = (std::min)(expBottom, boundBottom);
    }

    outLeft = expLeft;
    outTop = expTop;
    outRight = expRight;
    outBottom = expBottom;
}

inline bool overlaySurfaceContains(int left, int top, int right, int bottom,
                                   int originX, int originY, int width, int height) noexcept {
    return width > 0 && height > 0 &&
           left >= originX && top >= originY &&
           right <= originX + width && bottom <= originY + height;
}

/// 上一笔留下的超大 DIB 即使几何上还能装下当前轨迹，也不能继续拿来提交：
/// UpdateLayeredWindow 一张近乎整屏的位图，短手势会一帧都画不出来。
inline bool overlayCanReuseSurface(int neededW, int neededH,
                                   int existingW, int existingH,
                                   int maxAreaFactor) noexcept {
    if (neededW <= 0 || neededH <= 0 || existingW <= 0 || existingH <= 0) return false;
    if (maxAreaFactor <= 0) return false;
    const long long neededArea = static_cast<long long>(neededW) * neededH;
    const long long existingArea = static_cast<long long>(existingW) * existingH;
    return existingArea <= neededArea * static_cast<long long>(maxAreaFactor);
}

/// 轨迹与结果卡片始终是无激活、鼠标穿透的工具窗口。无论调用方传入
/// 什么历史样式，都必须剥离 APPWINDOW，否则 Explorer 会为临时覆盖层
/// 创建任务栏按钮。
inline LONG_PTR normalizeGestureOverlayExStyle(LONG_PTR current) noexcept {
    constexpr LONG_PTR required = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                  WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                  WS_EX_TOOLWINDOW;
    return (current | required) & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
}

inline bool gestureOverlayIsTaskbarSafe(LONG_PTR exStyle) noexcept {
    return (exStyle & WS_EX_TOOLWINDOW) != 0 &&
           (exStyle & WS_EX_APPWINDOW) == 0 &&
           (exStyle & WS_EX_NOACTIVATE) != 0;
}

/// 松手结果卡片必须真正画出来，淡出时钟才能走。否则动作已经执行，用户只看到空白。
inline bool gestureFrameReadyToFade(bool trailPresented, bool toastRequired,
                                    bool toastPresented) noexcept {
    if (!trailPresented) return false;
    return !toastRequired || toastPresented;
}

/// 实时命中不要跟着方向编码每个拐点闪灰：刚命中过的动作保持一小段，直到稳定未匹配或换了新动作。
inline bool keepLiveGestureMatch(bool hasNewMatch, bool hadMatch,
                                 DWORD unmatchedElapsedMs, DWORD holdMs) noexcept {
    if (hasNewMatch) return true;
    if (!hadMatch) return false;
    return unmatchedElapsedMs < holdMs;
}

inline constexpr DWORD kLiveGestureMatchHoldMs = 120;

inline float clampTrailOutlineWidth(float width) noexcept {
    if (!std::isfinite(width) || width <= 0.0f) return 0.0f;
    return (std::min)(width, 8.0f);
}

/// 白色描边作为最外圈：核心宽度 + 两侧描边。
inline float trailOutlineWidenWidth(float coreWidth, float outlineWidth) noexcept {
    if (coreWidth <= 0.0f || outlineWidth <= 0.0f) return 0.0f;
    return coreWidth + outlineWidth * 2.0f;
}

/// 淡出时钟必须从第一帧真正画出来之后才走。若从松手瞬间起算，重建整屏
/// DIB 的耗时会被算进淡出窗口里，结果动作已经执行、轨迹和 Toast 一帧都没有。
inline bool gestureFadeShouldFinish(bool clockStarted, DWORD elapsedMs,
                                    DWORD fadeHoldMs, DWORD fadeOutMs) noexcept {
    if (!clockStarted) return false;
    return elapsedMs >= fadeHoldMs + fadeOutMs;
}

inline float gestureFadeAlpha(bool clockStarted, DWORD elapsedMs,
                              DWORD fadeHoldMs, DWORD fadeOutMs) noexcept {
    if (!clockStarted || elapsedMs <= fadeHoldMs) return 1.0f;
    if (fadeOutMs == 0) return 0.0f;
    const float progress = std::clamp(
        static_cast<float>(elapsedMs - fadeHoldMs) / static_cast<float>(fadeOutMs),
        0.0f, 1.0f);
    const float ease = 1.0f - progress;
    return ease * ease;
}

/// 手势松手命中成功时的【高光成功脉冲 (Success Flash Pulse)】强度计算 (0.0f ~ 1.0f)
/// 前 pulseHoldMs (默认 110ms，满足 100ms~150ms 饱满明亮高光规范) 维持 100% 满额高光与白光呼吸冲刷，
/// 随后 pulseFadeMs (默认 80ms) 经平滑二次衰减曲线自然过渡至 0.0，使卡片无缝回归稳态主题色展示
inline float gestureSuccessPulseIntensity(bool isFading, bool isSuccess, bool clockStarted,
                                          DWORD elapsedMs, DWORD pulseHoldMs = 110,
                                          DWORD pulseFadeMs = 80) noexcept {
    if (!isFading || !isSuccess) return 0.0f;
    if (!clockStarted || elapsedMs <= pulseHoldMs) return 1.0f;
    if (pulseFadeMs == 0 || elapsedMs >= pulseHoldMs + pulseFadeMs) return 0.0f;
    const float progress = std::clamp(
        static_cast<float>(elapsedMs - pulseHoldMs) / static_cast<float>(pulseFadeMs),
        0.0f, 1.0f);
    const float ease = 1.0f - progress;
    return ease * ease;
}

/// 计算成功高光脉冲下的背景 RGB (向纯白适度提亮 25% 以呈现饱满通透的高光主题色)
inline tools3000::core::AccentColorRGB computeSuccessPulseBgColor(
    const tools3000::core::AccentColorRGB& baseRgb, float pulseIntensity) noexcept {
    const float clampedIntensity = std::clamp(pulseIntensity, 0.0f, 1.0f);
    float r = (std::min)(1.0f, baseRgb.r + (1.0f - baseRgb.r) * 0.25f * clampedIntensity);
    float g = (std::min)(1.0f, baseRgb.g + (1.0f - baseRgb.g) * 0.25f * clampedIntensity);
    float b = (std::min)(1.0f, baseRgb.b + (1.0f - baseRgb.b) * 0.25f * clampedIntensity);
    return { r, g, b };
}

/// 计算成功高光脉冲下的白光高光冲刷透明度 (前 100ms~150ms 纯白微晶呼吸冲刷，峰值提升至 0.38f)
inline float computeSuccessPulseFlashAlpha(float pulseIntensity, float fadeAlpha) noexcept {
    const float clampedIntensity = std::clamp(pulseIntensity, 0.0f, 1.0f);
    const float clampedFade = std::clamp(fadeAlpha, 0.0f, 1.0f);
    return 0.38f * clampedIntensity * clampedFade;
}

/// 计算成功高光脉冲下的微晶边框厚度 (基线 2.6f 像素，峰值高光时动态微胀至 3.4f 像素，随 DPI 缩放)
inline float computeSuccessPulseBorderWidth(float baseWidth, float pulseIntensity, float scale) noexcept {
    const float clampedIntensity = std::clamp(pulseIntensity, 0.0f, 1.0f);
    const float expansion = 0.8f * clampedIntensity; // 峰值 +0.8f 物理微晶膨胀
    return (baseWidth + expansion) * scale;
}

/// auto：跟随全局强调色；custom：用手势页里的自定义 HEX。
inline tools3000::core::AccentColorRGB resolveGestureTrailRgb(
    std::string_view colorMode,
    std::string_view customHex,
    const tools3000::core::AccentColorRGB& accentRgb) noexcept {
    if (colorMode == "custom" && !customHex.empty()) {
        return tools3000::core::parseHexColor(std::string(customHex));
    }
    return accentRgb;
}

inline bool gestureTrailUsesLightPalette(std::string_view theme,
                                         bool systemAppsUseLight) noexcept {
    if (theme == "light") return true;
    if (theme == "system") return systemAppsUseLight;
    return false;
}

/// Tools3000 自己的顶层窗口类名统一用 Tools3000_ 前缀。
inline bool isTools3000UiClassName(std::wstring_view cls) noexcept {
    constexpr std::wstring_view kPrefix = L"Tools3000_";
    return cls.size() >= kPrefix.size() && cls.substr(0, kPrefix.size()) == kPrefix;
}

/// 轨迹 / toast 覆盖层：TOPMOST 分层窗，松手时光标一定压在笔迹上。
inline bool isGestureOverlayClassName(std::wstring_view cls) noexcept {
    return cls == L"Tools3000_GestureOverlay";
}

inline bool isGesturePassThroughClassName(std::wstring_view cls) noexcept {
    return isGestureOverlayClassName(cls) || cls == L"Tools3000_ToastOverlay";
}

/// 搜索结果中的 Shift+右键属于 Windows 原生菜单快捷操作，必须让物理按下/
/// 抬起原样进入 WebView。若先由手势引擎吞掉再补发，Shift 状态、坐标和菜单
/// 前台权限都会变得不可靠。
inline bool shouldBypassGestureForNativeSearchMenu(std::wstring_view cls,
                                                    bool shiftPressed) noexcept {
    return shiftPressed && cls == L"Tools3000_SearchWindow";
}

/// 识别 Windows 任务栏与系统托盘窗口（包括主任务栏、多显示器副任务栏、托盘通知区与 Win10/Win11 托盘溢出浮窗）
inline bool isSystemTrayOrTaskbar(std::wstring_view cls) noexcept {
    return cls == L"Shell_TrayWnd" ||
           cls == L"Shell_SecondaryTrayWnd" ||
           cls == L"TrayNotifyWnd" ||
           cls == L"NotifyIconOverflowWindow" ||
           cls == L"TopLevelWindowForOverflowXamlIsland";
}

/// 识别 Windows 系统级外壳、桌面、任务栏、开始菜单与弹出菜单窗口
/// 这些窗口绝不允许作为手势关闭窗口的目标（严禁发送 SC_CLOSE / Alt+F4）
inline bool isSystemDesktopOrShellWindow(std::wstring_view cls) noexcept {
    if (cls.empty()) return false;
    if (isSystemTrayOrTaskbar(cls)) return true;
    return cls == L"Progman" ||
           cls == L"WorkerW" ||
           cls == L"SHELLDLL_DefView" ||
           cls == L"SysListView32" ||
           cls == L"TrayClockWClass" ||
           cls == L"MSTaskListWClass" ||
           cls == L"ReBarWindow32" ||
           cls == L"#32768" ||
           cls == L"Windows.UI.Core.CoreWindow" ||
           cls == L"XamlExplorerHostIslandWindow" ||
           cls == L"DV2ControlHost";
}

/// 识别 Windows 桌面、资源管理器文件夹窗口、通用文件对话框及外壳视图控件。
/// 在这些窗口与控件中，左键点击与拖拽为 Windows 原生核心交互（框选、移动、复制、多选等），
/// 绝对禁止作为左键手势拦截，确保人类桌面文件拖拽与文件管理 100% 原生穿透。
inline bool isDesktopOrFileManagerWindow(std::wstring_view cls) noexcept {
    if (cls.empty()) return false;
    if (isSystemDesktopOrShellWindow(cls)) return true;
    return cls == L"CabinetWClass" ||
           cls == L"ExploreWClass" ||
           cls == L"DirectUIHWND" ||
           cls == L"#32770";
}

/// 从覆盖层往下找真实窗口时，不可见、覆盖层、几何上不含该点的候选都跳过。
inline bool gestureHitTestShouldSkipCandidate(bool visible, bool overlayClass,
                                              bool containsPoint) noexcept {
    return !visible || overlayClass || !containsPoint;
}

/// 手势命中窗口：跳过轨迹/Toast 这类穿透覆盖层，但设置窗、搜索窗仍是合法目标。
inline bool gestureHitTestAcceptsWindow(bool visible, bool passThrough,
                                        bool cloaked, bool containsPoint) noexcept {
    return visible && !passThrough && !cloaked && containsPoint;
}

enum class GestureTargetMode : unsigned char {
    UnderPointer = 0,
    Foreground = 1,
};

inline GestureTargetMode parseGestureTargetMode(std::string_view value) noexcept {
    return value == "foreground" ? GestureTargetMode::Foreground
                                 : GestureTargetMode::UnderPointer;
}

inline const char* gestureTargetModeKey(GestureTargetMode mode) noexcept {
    return mode == GestureTargetMode::Foreground ? "foreground" : "underPointer";
}

/// 0=起点下方, 1=终点下方, 2=前台窗口, -1=放弃。
/// underPointer 不用前台窗口兜底，否则画在设置窗上会打到背后的 Chrome。
inline int pickGestureTargetSlot(GestureTargetMode mode,
                                 bool startOk, bool endOk, bool foregroundOk) noexcept {
    if (mode == GestureTargetMode::Foreground) {
        return foregroundOk ? 2 : -1;
    }
    if (startOk) return 0;
    if (endOk) return 1;
    return -1;
}

/// Alt+F4 应直接向目标窗口投递关闭，而不是再合成按键（覆盖层抢前台时 SendInput 会打空）。
inline bool keyStrokeShouldPostClose(uint8_t modifiers, uint16_t virtualKey) noexcept {
    const uint8_t withoutAlt = static_cast<uint8_t>(modifiers & ~static_cast<uint8_t>(MOD_ALT));
    return withoutAlt == 0 && (modifiers & MOD_ALT) != 0 && virtualKey == VK_F4;
}

inline bool keyStrokeIsCtrlW(uint8_t modifiers, uint16_t virtualKey) noexcept {
    const uint8_t withoutCtrl = static_cast<uint8_t>(modifiers & ~static_cast<uint8_t>(MOD_CONTROL));
    return withoutCtrl == 0 && (modifiers & MOD_CONTROL) != 0 && virtualKey == 'W';
}

inline bool classNameStartsWith(std::wstring_view cls, std::wstring_view prefix) noexcept {
    return cls.size() >= prefix.size() && cls.substr(0, prefix.size()) == prefix;
}

/// Chrome / Edge / Firefox / 资源管理器 / Electron IDE 把 Ctrl+W 当成关标签。
inline bool isTabbedBrowserClassName(std::wstring_view cls) noexcept {
    return classNameStartsWith(cls, L"Chrome_WidgetWin") ||
           cls == L"MozillaWindowClass" ||
           cls == L"CabinetWClass";
}

/// CEF / Electron / Qt / UWP 等生产力宿主：即使无边框铺满屏幕，也不应按游戏全屏免打扰。
inline bool isProductivityToolkitClassName(std::wstring_view cls) noexcept {
    if (isTabbedBrowserClassName(cls) || isTools3000UiClassName(cls)) return true;
    if (cls == L"OrpheusBrowserHost" || cls == L"CefBrowserWindow" ||
        cls == L"ApplicationFrameWindow") {
        return true;
    }
    return classNameStartsWith(cls, L"Qt");
}

/// 仅对真正的全屏独占（游戏/播放器）免打扰；IDE / 浏览器 / CEF / Qt 全屏继续手势。
inline bool shouldAutoBypassFullscreenGestures(bool isFullscreen,
                                               bool isProductivityClass) noexcept {
    return isFullscreen && !isProductivityClass;
}

/// Chromium / Electron 用 WS_EX_NOREDIRECTIONBITMAP 走 DirectComposition。
/// 普通 UpdateLayeredWindow 分层窗即使 TOPMOST，也会被合成到这类窗口下面。
inline bool windowUsesCompositorSurface(LONG_PTR exStyle) noexcept {
    return (exStyle & WS_EX_NOREDIRECTIONBITMAP) != 0;
}

/// 沉底让路期间不要把覆盖层拉回来；丢失 TOPMOST 位且未沉底时才补插队。
/// 下一笔 beginTrail 会无条件 raise，不依赖这个 exstyle 位。
inline bool overlayPresentShouldForceTopmost(bool hasTopmostExStyle,
                                             bool yieldedBelow) noexcept {
    return !yieldedBelow && !hasTopmostExStyle;
}

/// 画在 Tools3000 自己的设置/搜索窗上时，关闭标签页应关掉该窗口，而不是把 Ctrl+W 打进 WebView。
inline bool keyStrokeShouldDismissTools3000Ui(uint8_t modifiers, uint16_t virtualKey) noexcept {
    if (keyStrokeShouldPostClose(modifiers, virtualKey)) return true;
    return keyStrokeIsCtrlW(modifiers, virtualKey);
}

/// Alt+F4 一律关窗。Ctrl+W 只在无标签页的宿主（网易云 Orpheus、微信 Qt 等）升格为关窗。
/// 系统桌面 (Progman/WorkerW)、任务栏 (Shell_TrayWnd) 与菜单等系统级窗口严禁作为关窗目标。
inline bool keyStrokeShouldCloseWindow(uint8_t modifiers, uint16_t virtualKey,
                                       std::wstring_view className) noexcept {
    if (isSystemDesktopOrShellWindow(className)) return false;
    if (keyStrokeShouldPostClose(modifiers, virtualKey)) return true;
    if (!keyStrokeIsCtrlW(modifiers, virtualKey)) return false;
    if (isTools3000UiClassName(className)) return true;
    return !isTabbedBrowserClassName(className);
}

/// 关窗目标走 owner 链：CEF 内嵌宿主往往是顶层窗，真正该关的是 GA_ROOTOWNER。
/// 严禁向桌面、任务栏等系统核心外壳窗口投递关闭请求。
inline HWND resolveCloseableWindow(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    auto isBlocked = [](HWND h) noexcept -> bool {
        if (!h || !IsWindow(h)) return false;
        wchar_t clsBuf[128]{};
        if (GetClassNameW(h, clsBuf, static_cast<int>(std::size(clsBuf)))) {
            if (isSystemDesktopOrShellWindow(clsBuf)) return true;
        }
        return false;
    };
    if (isBlocked(hwnd)) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && isBlocked(root)) return nullptr;
    HWND ownerRoot = GetAncestor(hwnd, GA_ROOTOWNER);
    if (ownerRoot && isBlocked(ownerRoot)) return nullptr;
    return ownerRoot ? ownerRoot : (root ? root : hwnd);
}

inline constexpr DWORD kCloseObserveTimeoutMs = 40;

inline bool windowStillAcceptsClose(bool isWindow, bool isVisible) noexcept {
    return isWindow && isVisible;
}

/// 已投递关闭且窗口还在，才补发 Alt+F4，避免 Chrome 一类宿主被关两次。
inline bool closeShouldSendKeyFallback(bool posted, bool stillAcceptsClose) noexcept {
    return posted && stillAcceptsClose;
}

/// 目标进程相对本进程的完整性。Medium 钩子看不到 High/System 窗口上的鼠标。
enum class ProcessIntegrityRelation : unsigned char {
    SameOrLower = 0,
    Higher = 1,
    Unknown = 2,
};

/// QUERY_LIMITED 对同用户进程通常成功；QUERY_INFORMATION / TOKEN_QUERY
/// 在目标完整性更高时会被 UIPI 拒绝（ACCESS_DENIED）。令牌里的 elevated
/// 只有 token 查询成功时才可信。
inline ProcessIntegrityRelation classifyProcessIntegrityQuery(
    bool queryLimitedOk,
    bool queryInformationOk,
    bool tokenQueryOk,
    bool targetTokenElevated,
    bool selfElevated) noexcept {
    if (!queryLimitedOk) return ProcessIntegrityRelation::Unknown;
    if (!queryInformationOk || !tokenQueryOk) {
        return selfElevated ? ProcessIntegrityRelation::Unknown
                            : ProcessIntegrityRelation::Higher;
    }
    if (targetTokenElevated && !selfElevated) {
        return ProcessIntegrityRelation::Higher;
    }
    return ProcessIntegrityRelation::SameOrLower;
}

inline bool lowLevelHookCanObserveTarget(ProcessIntegrityRelation rel) noexcept {
    return rel != ProcessIntegrityRelation::Higher;
}

inline bool shouldWarnGestureIntegrityBlocked(ProcessIntegrityRelation rel,
                                              bool tools3000Ui) noexcept {
    return rel == ProcessIntegrityRelation::Higher && !tools3000Ui;
}

/// 手势专用异步渲染工作线程优先级策略：采用最高优先级杜绝高负载丢帧与时钟抖动
constexpr int gestureRenderThreadPriority() noexcept {
    return THREAD_PRIORITY_HIGHEST;
}

/// 轨迹点推入即时唤醒与节流合并策略 (F13):
/// 1. 持有有效 HWND 且渲染循环处于休眠(renderLoopActive == false)时，立即唤醒渲染，杜绝粗粒度时钟截断延迟；
/// 2. 当渲染循环已处于活跃节拍(renderLoopActive == true)时，返回 false，抑制冗余的 SetEvent 系统调用风暴。
constexpr bool gestureShouldWakeRenderImmediately(bool hasHwnd, bool renderLoopActive = false) noexcept {
    return hasHwnd && !renderLoopActive;
}

/// 物理光标尖端原子同步插值策略：
/// 在非淡出态且已有轨迹点时，若物理光标位移平方达到阈值（默认 1.0px^2），原子补入尖端点抹平 DWM 合成相位差
inline bool gestureShouldInterpolateCursorTip(bool isFading, bool hasPoints,
                                              float cursorX, float cursorY,
                                              float lastX, float lastY,
                                              float minDistanceSq = 1.0f) noexcept {
    if (isFading || !hasPoints) return false;
    const float dx = cursorX - lastX;
    const float dy = cursorY - lastY;
    return (dx * dx + dy * dy) >= minDistanceSq;
}

// ─────────────────────────────────────────────────────────────────────────────
// Feature F10: 动态物理显示器刷新率与 QPC 节拍计算纯策略
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr uint32_t kMinDisplayFrequencyHz = 60;
inline constexpr uint32_t kMaxDisplayFrequencyHz = 360;
inline constexpr uint32_t kDefaultDisplayFrequencyHz = 60;

/// 物理显示器刷新率安全钳制规则：
/// 1. 原始频率 <= 1 时（驱动默认或虚拟机占位），安全回退至默认 60Hz
/// 2. 原始频率在 (1, 60) 之间时（如 24Hz/30Hz/59Hz），钳制向上提至 60Hz，保障桌面手势基础跟手度
/// 3. 原始频率在 [60, 360] 之间时，原样采用真实物理硬件刷新率（如 75, 120, 144, 165, 240, 360）
/// 4. 原始频率 > 360 时（如 540Hz 极端模式），钳制上限至 360Hz，杜绝 GPU 指令堆叠
inline constexpr uint32_t clampDisplayFrequency(uint32_t rawHz) noexcept {
    if (rawHz <= 1) {
        return kDefaultDisplayFrequencyHz;
    }
    if (rawHz < kMinDisplayFrequencyHz) {
        return kMinDisplayFrequencyHz;
    }
    if (rawHz > kMaxDisplayFrequencyHz) {
        return kMaxDisplayFrequencyHz;
    }
    return rawHz;
}

/// 计算单帧物理周期的 QPC Ticks 计数
inline constexpr uint64_t computeFramePeriodTicks(uint64_t qpcFrequency, uint32_t refreshRateHz) noexcept {
    const uint32_t safeHz = clampDisplayFrequency(refreshRateHz);
    return qpcFrequency > 0 ? (qpcFrequency / safeHz) : 0;
}

/// 计算单帧物理周期的浮点毫秒数 (用于平滑动画与统计)
inline constexpr double computeFramePeriodMs(uint32_t refreshRateHz) noexcept {
    const uint32_t safeHz = clampDisplayFrequency(refreshRateHz);
    return 1000.0 / static_cast<double>(safeHz);
}

/// 基于 QPC 测量已流逝 Ticks 计算到下一帧截止点所需等待的毫秒数 (向下取整，0 表示应即刻渲染)
inline constexpr DWORD computePacingWaitMs(uint64_t elapsedTicks, uint64_t framePeriodTicks, uint64_t qpcFrequency) noexcept {
    if (framePeriodTicks == 0 || qpcFrequency == 0 || elapsedTicks >= framePeriodTicks) {
        return 0;
    }
    const uint64_t remainingTicks = framePeriodTicks - elapsedTicks;
    return static_cast<DWORD>((remainingTicks * 1000) / qpcFrequency);
}

/// 空间包围盒高速命中判定：检查光标坐标是否落在指定显示器物理区域内 (含半开区间 [left, right), [top, bottom))
inline constexpr bool isPointInMonitorRect(const RECT& rc, POINT pt) noexcept {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

// ─────────────────────────────────────────────────────────────────────────────
// 累积手势包围盒管线 (Cumulative Bounding Box Pipeline) 纯计算策略
// ─────────────────────────────────────────────────────────────────────────────

/// 计算手势笔画外扩脏矩形安全裕量 (包含笔画线宽、柔光外发光、头部发光晶体与抗锯齿余量)
inline int computeTrailDirtyMargin(float lineWidth, float dpiScale) noexcept {
    const float coreW = (std::max)(lineWidth * dpiScale, 4.0f);
    return static_cast<int>(std::ceil(coreW * 2.5f + 16.0f * dpiScale)) + 8;
}

/// 合并并严格双向钳制累积手势脏矩形至虚拟表面边界
inline RECT unionAndClampStrokeDirtyRect(const RECT& prevAccumulated, const RECT& currentBox, int surfaceW, int surfaceH) noexcept {
    if (surfaceW <= 0 || surfaceH <= 0) {
        return RECT{0, 0, (std::max)(1, surfaceW), (std::max)(1, surfaceH)};
    }

    const bool prevEmpty = IsRectEmpty(&prevAccumulated);
    const bool currEmpty = IsRectEmpty(&currentBox);

    RECT dirtyRect{ 0, 0, 0, 0 };
    if (prevEmpty && currEmpty) {
        dirtyRect = { 0, 0, 1, 1 };
    } else if (prevEmpty) {
        dirtyRect = currentBox;
    } else if (currEmpty) {
        dirtyRect = prevAccumulated;
    } else {
        UnionRect(&dirtyRect, &prevAccumulated, &currentBox);
    }

    // 严格双向钳制，数学级保证 0 <= left < right <= surfaceW 以及 0 <= top < bottom <= surfaceH
    dirtyRect.left = (std::max)(0L, (std::min)(static_cast<LONG>(surfaceW - 1), dirtyRect.left));
    dirtyRect.top = (std::max)(0L, (std::min)(static_cast<LONG>(surfaceH - 1), dirtyRect.top));
    dirtyRect.right = (std::max)(static_cast<LONG>(dirtyRect.left + 1), (std::min)(static_cast<LONG>(surfaceW), dirtyRect.right));
    dirtyRect.bottom = (std::max)(static_cast<LONG>(dirtyRect.top + 1), (std::min)(static_cast<LONG>(surfaceH), dirtyRect.bottom));
    return dirtyRect;
}

/// 计算指定轨迹点集的几何外接包围盒 (含外扩安全裕量并严格双向钳制在虚拟表面边界内)
template <typename PointType>
inline RECT computeTrailPointsBoundingBox(
    const std::vector<PointType>& points,
    int originX,
    int originY,
    int surfaceW,
    int surfaceH,
    float lineWidth,
    float dpiScale) noexcept {
    if (points.empty() || surfaceW <= 0 || surfaceH <= 0) {
        return RECT{0, 0, (std::max)(1, surfaceW), (std::max)(1, surfaceH)};
    }

    const int margin = computeTrailDirtyMargin(lineWidth, dpiScale);

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (size_t i = 0; i < points.size(); ++i) {
        const int px = static_cast<int>(points[i].x) - originX;
        const int py = static_cast<int>(points[i].y) - originY;
        minX = (std::min)(minX, px);
        minY = (std::min)(minY, py);
        maxX = (std::max)(maxX, px);
        maxY = (std::max)(maxY, py);
    }

    RECT r;
    r.left = (std::max)(0L, (std::min)(static_cast<LONG>(surfaceW - 1), static_cast<LONG>(minX - margin)));
    r.top = (std::max)(0L, (std::min)(static_cast<LONG>(surfaceH - 1), static_cast<LONG>(minY - margin)));
    r.right = (std::max)(static_cast<LONG>(r.left + 1), (std::min)(static_cast<LONG>(surfaceW), static_cast<LONG>(maxX + margin + 1)));
    r.bottom = (std::max)(static_cast<LONG>(r.top + 1), (std::min)(static_cast<LONG>(surfaceH), static_cast<LONG>(maxY + margin + 1)));
    return r;
}

}  // namespace tools3000::gesture

#endif  // TOOLS3000_GESTURE_GESTUREINPUTPOLICY_H
