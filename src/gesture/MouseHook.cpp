// ─────────────────────────────────────────────────────────────────────────────
// MouseHook.cpp — 低级鼠标钩子实现 (接入核心独立输入线程与无锁 SPSC 环形队列)
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/MouseHook.h"
#include "gesture/GestureAction.h"
#include "gesture/GestureInputPolicy.h"
#include "core/hotkey/MouseHook.h"
#include "core/logger/Logger.h"
#include "core/stats/StatsManager.h"
#include <cmath>

namespace tools3000::gesture {

MouseHook& MouseHook::instance() {
    static MouseHook inst;
    return inst;
}

bool MouseHook::install() {
    if (m_installed.load(std::memory_order_relaxed)) {
        return true;
    }

    // 接入全局单一输入源：确保核心库的独立高优先级输入线程已启动并挂载拦截器
    auto& coreHook = tools3000::core::MouseHook::instance();
    if (!coreHook.install()) {
        LOG_ERROR("安装核心鼠标输入线程失败");
        return false;
    }

    coreHook.setInterceptor([this](int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data) -> bool {
        return handleRawMouseEvent(nCode, wParam, data);
    });

    m_installed.store(true, std::memory_order_release);
    LOG_INFO("手势低级鼠标钩子已接入全局单一输入源 (线程隔离 + 无锁 SPSC 环形队列已就绪)");
    return true;
}

void MouseHook::uninstall() {
    if (m_installed.load(std::memory_order_relaxed)) {
        tools3000::core::MouseHook::instance().setInterceptor(nullptr);
        m_installed.store(false, std::memory_order_release);
        LOG_INFO("手势低级鼠标钩子已卸载拦截器");
    }
}

bool MouseHook::isInstalled() const {
    return m_installed.load(std::memory_order_relaxed) &&
           tools3000::core::MouseHook::instance().isInstalled();
}

void MouseHook::setPaused(bool paused) {
    m_paused.store(paused, std::memory_order_relaxed);
    LOG_INFO("手势鼠标钩子暂停状态: paused={}", paused);
}

void MouseHook::setEventCallback(MouseEventCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_callback = std::move(callback);
}

void MouseHook::setFaultCallback(MouseHookFaultCallback callback) {
    std::lock_guard lock(m_callbackMutex);
    m_faultCallback = std::move(callback);
}

void MouseHook::setTriggerMode(TriggerMode mode) {
    m_configuredTriggerMode.store(mode, std::memory_order_release);
    LOG_INFO("鼠标手势触发模式已更新: mode={}", static_cast<int>(mode));
}

void MouseHook::setTriggerButton(MouseEventType downEvent) {
    if (downEvent == MouseEventType::MiddleDown) {
        setTriggerMode(TriggerMode::MiddleOnly);
    } else {
        setTriggerMode(TriggerMode::RightOnly);
    }
    m_configuredTriggerDown.store(downEvent, std::memory_order_release);
}

void MouseHook::resetTriggerState() noexcept {
    m_triggerButtonDown.store(false, std::memory_order_release);
    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_release);
    m_cachedForegroundWindow = nullptr;
    m_cachedModifiers = 0;
    m_cachedEdgeZone = ScreenEdgeZone::None;
    m_cachedIsTopEdge = false;
}

std::vector<MouseEvent> MouseHook::drainEvents(size_t maxCount) {
    std::vector<MouseEvent> events;
    events.reserve(std::min(maxCount, m_ringBuffer.size()));
    MouseEvent ev;
    while (events.size() < maxCount && m_ringBuffer.pop(ev)) {
        events.push_back(std::move(ev));
    }
    return events;
}

static ScreenEdgeZone detectScreenEdgeZone(POINT pt, int tolerance = 4) {
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return ScreenEdgeZone::None;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi)) return ScreenEdgeZone::None;
    const RECT& rc = mi.rcMonitor;
    if (pt.y <= rc.top + tolerance) return ScreenEdgeZone::Top;
    if (pt.y >= rc.bottom - 1 - tolerance) return ScreenEdgeZone::Bottom;
    if (pt.x <= rc.left + tolerance) return ScreenEdgeZone::Left;
    if (pt.x >= rc.right - 1 - tolerance) return ScreenEdgeZone::Right;
    return ScreenEdgeZone::None;
}

ScreenEdgeZone MouseHook::getActiveScreenEdgeZone(POINT pt, MouseEventType type) const {
    const ScreenEdgeZone rawZone = detectScreenEdgeZone(pt);
    if (rawZone == ScreenEdgeZone::None) {
        return ScreenEdgeZone::None;
    }

    const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
    const bool isWheel = (type == MouseEventType::WheelUp || type == MouseEventType::WheelDown);

    switch (rawZone) {
        case ScreenEdgeZone::Top: {
            if (isWheel) {
                return (mask & GestureTriggerMask::EdgeTopWheel) ? ScreenEdgeZone::Top : ScreenEdgeZone::None;
            }
            if (type == MouseEventType::RightDown) {
                return ((mask & GestureTriggerMask::EdgeTopSlide) || (mask & GestureTriggerMask::EdgeTopRight))
                    ? ScreenEdgeZone::Top : ScreenEdgeZone::None;
            }
            if (type == MouseEventType::MiddleDown) {
                return ((mask & GestureTriggerMask::EdgeTopSlide) || (mask & GestureTriggerMask::EdgeTopMiddle))
                    ? ScreenEdgeZone::Top : ScreenEdgeZone::None;
            }
            if (type == MouseEventType::LeftDown) {
                return ((mask & GestureTriggerMask::EdgeTopSlide) || (mask & GestureTriggerMask::EdgeTopLeft) || (mask & GestureTriggerMask::Left))
                    ? ScreenEdgeZone::Top : ScreenEdgeZone::None;
            }
            const uint32_t topMask = GestureTriggerMask::EdgeTopSlide | GestureTriggerMask::EdgeTopWheel |
                                     GestureTriggerMask::EdgeTopRight | GestureTriggerMask::EdgeTopMiddle |
                                     GestureTriggerMask::EdgeTopLeft;
            return (mask & topMask) ? ScreenEdgeZone::Top : ScreenEdgeZone::None;
        }
        case ScreenEdgeZone::Bottom: {
            if (isWheel) {
                return (mask & GestureTriggerMask::EdgeBottomWheel) ? ScreenEdgeZone::Bottom : ScreenEdgeZone::None;
            }
            return (mask & (GestureTriggerMask::EdgeBottomSlide | GestureTriggerMask::Left)) ? ScreenEdgeZone::Bottom : ScreenEdgeZone::None;
        }
        case ScreenEdgeZone::Left: {
            if (isWheel) return ScreenEdgeZone::None;
            return (mask & (GestureTriggerMask::EdgeLeftSlide | GestureTriggerMask::Left)) ? ScreenEdgeZone::Left : ScreenEdgeZone::None;
        }
        case ScreenEdgeZone::Right: {
            if (isWheel) return ScreenEdgeZone::None;
            return (mask & (GestureTriggerMask::EdgeRightSlide | GestureTriggerMask::Left)) ? ScreenEdgeZone::Right : ScreenEdgeZone::None;
        }
        default:
            return ScreenEdgeZone::None;
    }
}

bool MouseHook::handleRawMouseEvent(int nCode, WPARAM wParam, const MSLLHOOKSTRUCT& data) {
    if (nCode < 0) return false;
    if (data.flags & LLMHF_INJECTED) {
        // 生产环境严格过滤软件模拟注入脉冲 (SendInput)，防止内部补发脉冲导致递归死锁；
        // 仅在显式设置测试环境变量或携带专用 E2E 测试签名时放行，以支持真实的操作系统级端到端测试。
        static const bool s_allowInjected = []() {
            return GetEnvironmentVariableW(L"TOOLS3000_ALLOW_INJECTED_MOUSE", nullptr, 0) > 0;
        }();
        constexpr ULONG_PTR TEST_EXTRA_INFO = 0x54455354; // "TEST"
        if (!s_allowInjected && data.dwExtraInfo != TEST_EXTRA_INFO) {
            return false;
        }
    }

    static thread_local bool s_reentry = false;
    if (s_reentry) return false;
    s_reentry = true;
    struct ReentryGuard { ~ReentryGuard() { s_reentry = false; } } reentryGuard;

    try {
        MouseEvent event{};
        event.position = data.pt;
        event.timestamp = std::chrono::steady_clock::now();

        bool shouldCapture = false;
        const bool gestureEnabled = !m_paused.load(std::memory_order_relaxed);

        switch (wParam) {
            case WM_MOUSEMOVE: {
                event.type = MouseEventType::Move;
                shouldCapture = gestureEnabled && m_triggerButtonDown.load(std::memory_order_relaxed);

                static POINT lastPt = { -1, -1 };
                if (lastPt.x != -1 && lastPt.y != -1) {
                    double dx = data.pt.x - lastPt.x;
                    double dy = data.pt.y - lastPt.y;
                    double dist = std::sqrt(dx*dx + dy*dy);
                    if (dist > 0) {
                        tools3000::core::StatsManager::instance().recordMouseDistance(dist);
                    }
                }
                lastPt = data.pt;
                break;
            }
            case WM_RBUTTONDOWN: {
                event.type = MouseEventType::RightDown;
                event.edgeZone = getActiveScreenEdgeZone(data.pt, event.type);
                event.isTopEdge = (event.edgeZone == ScreenEdgeZone::Top);
                const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
                uint8_t mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOUSE_MOD_CTRL;
                if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOUSE_MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOUSE_MOD_SHIFT;

                HWND rawHit = WindowFromPoint(data.pt);
                HWND hitWindow = rawHit;
                if (hitWindow) {
                    if (const HWND root = GetAncestor(hitWindow, GA_ROOT)) hitWindow = root;
                }
                wchar_t hitClass[256] = {};
                if (hitWindow) GetClassNameW(hitWindow, hitClass, 256);
                wchar_t rawClass[256] = {};
                if (rawHit && rawHit != hitWindow) GetClassNameW(rawHit, rawClass, 256);

                bool isTrayOrTaskbarTarget = isSystemTrayOrTaskbar(hitClass) || isSystemTrayOrTaskbar(rawClass);
                if (!isTrayOrTaskbarTarget && rawHit) {
                    HWND cur = rawHit;
                    while (cur && cur != hitWindow) {
                        wchar_t curCls[256] = {};
                        GetClassNameW(cur, curCls, 256);
                        if (isSystemTrayOrTaskbar(curCls)) {
                            isTrayOrTaskbarTarget = true;
                            break;
                        }
                        const HWND parent = GetAncestor(cur, GA_PARENT);
                        if (!parent || parent == cur) break;
                        cur = parent;
                    }
                }

                const HWND searchWindow = FindWindowW(L"Tools3000_SearchWindow", nullptr);
                bool inSearchBounds = false;
                if (searchWindow && IsWindow(searchWindow)) {
                    if (GetPropW(searchWindow, L"Tools3000_ShellMenuActive")) {
                        inSearchBounds = true;
                    } else {
                        RECT searchRc{};
                        GetWindowRect(searchWindow, &searchRc);
                        if (PtInRect(&searchRc, data.pt)) {
                            inSearchBounds = true;
                        }
                    }
                }

                const bool nativeSearchMenu = shouldBypassGestureForNativeSearchMenu(
                    hitClass, (mods & MOUSE_MOD_SHIFT) != 0) || ((mods & MOUSE_MOD_SHIFT) != 0 && inSearchBounds);
                const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
                const bool rightAllowed = ((mask & GestureTriggerMask::Right) != 0) || (event.edgeZone != ScreenEdgeZone::None);
                if (gestureEnabled &&
                    !nativeSearchMenu &&
                    !isTrayOrTaskbarTarget &&
                    !m_triggerButtonDown.load(std::memory_order_relaxed) &&
                    rightAllowed &&
                    (mode == TriggerMode::RightOnly || mode == TriggerMode::Both || mode == TriggerMode::All)) {
                    m_activeTriggerDown.store(event.type, std::memory_order_relaxed);
                    m_triggerButtonDown.store(true, std::memory_order_relaxed);
                    m_cachedForegroundWindow = GetForegroundWindow();
                    m_cachedModifiers = mods;
                    m_cachedEdgeZone = event.edgeZone;
                    m_cachedIsTopEdge = event.isTopEdge;
                    shouldCapture = true;
                }
                if (nativeSearchMenu || isTrayOrTaskbarTarget) {
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
                    m_triggerButtonDown.store(false, std::memory_order_release);
                    shouldCapture = false;
                }
                tools3000::core::StatsManager::instance().recordRightClick();
                break;
            }
            case WM_RBUTTONUP:
                event.type = MouseEventType::RightUp;
                if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::RightDown) {
                    shouldCapture = gestureEnabled;
                    m_triggerButtonDown.store(false, std::memory_order_relaxed);
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
                } else {
                    shouldCapture = false;
                }
                m_cachedForegroundWindow = nullptr;
                m_cachedModifiers = 0;
                m_cachedEdgeZone = ScreenEdgeZone::None;
                m_cachedIsTopEdge = false;
                break;
            case WM_MBUTTONDOWN: {
                event.type = MouseEventType::MiddleDown;
                event.edgeZone = getActiveScreenEdgeZone(data.pt, event.type);
                event.isTopEdge = (event.edgeZone == ScreenEdgeZone::Top);
                const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
                const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
                const bool middleAllowed = ((mask & GestureTriggerMask::Middle) != 0) || (event.edgeZone != ScreenEdgeZone::None);
                if (gestureEnabled &&
                    !m_triggerButtonDown.load(std::memory_order_relaxed) &&
                    middleAllowed &&
                    (mode == TriggerMode::MiddleOnly || mode == TriggerMode::Both || mode == TriggerMode::All)) {
                    m_activeTriggerDown.store(event.type, std::memory_order_relaxed);
                    m_triggerButtonDown.store(true, std::memory_order_relaxed);
                    m_cachedForegroundWindow = GetForegroundWindow();
                    uint8_t mods = 0;
                    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOUSE_MOD_CTRL;
                    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOUSE_MOD_ALT;
                    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOUSE_MOD_SHIFT;
                    m_cachedModifiers = mods;
                    m_cachedEdgeZone = event.edgeZone;
                    m_cachedIsTopEdge = event.isTopEdge;
                    shouldCapture = true;
                }
                break;
            }
            case WM_MBUTTONUP:
                event.type = MouseEventType::MiddleUp;
                if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::MiddleDown) {
                    shouldCapture = gestureEnabled;
                    m_triggerButtonDown.store(false, std::memory_order_relaxed);
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
                } else {
                    shouldCapture = false;
                }
                m_cachedForegroundWindow = nullptr;
                m_cachedModifiers = 0;
                m_cachedEdgeZone = ScreenEdgeZone::None;
                m_cachedIsTopEdge = false;
                break;
            case WM_XBUTTONDOWN: {
                const WORD xbtn = HIWORD(data.mouseData);
                event.type = (xbtn == XBUTTON2) ? MouseEventType::X2Down : MouseEventType::X1Down;
                event.edgeZone = getActiveScreenEdgeZone(data.pt, event.type);
                event.isTopEdge = (event.edgeZone == ScreenEdgeZone::Top);
                const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
                const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
                const bool allowX = (mode == TriggerMode::Both || mode == TriggerMode::All ||
                                     (mode == TriggerMode::X1Only && xbtn == XBUTTON1) ||
                                     (mode == TriggerMode::X2Only && xbtn == XBUTTON2));
                const bool xAllowed = ((xbtn == XBUTTON1 && (mask & GestureTriggerMask::X1)) ||
                                       (xbtn == XBUTTON2 && (mask & GestureTriggerMask::X2))) ||
                                      (event.edgeZone != ScreenEdgeZone::None);
                if (gestureEnabled &&
                    !m_triggerButtonDown.load(std::memory_order_relaxed) && allowX && xAllowed) {
                    m_activeTriggerDown.store(event.type, std::memory_order_relaxed);
                    m_triggerButtonDown.store(true, std::memory_order_relaxed);
                    m_cachedForegroundWindow = GetForegroundWindow();
                    uint8_t mods = 0;
                    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOUSE_MOD_CTRL;
                    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOUSE_MOD_ALT;
                    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOUSE_MOD_SHIFT;
                    m_cachedModifiers = mods;
                    m_cachedEdgeZone = event.edgeZone;
                    m_cachedIsTopEdge = event.isTopEdge;
                    shouldCapture = true;
                }
                break;
            }
            case WM_XBUTTONUP: {
                const WORD xbtn = HIWORD(data.mouseData);
                event.type = (xbtn == XBUTTON2) ? MouseEventType::X2Up : MouseEventType::X1Up;
                const auto active = m_activeTriggerDown.load(std::memory_order_relaxed);
                if ((active == MouseEventType::X1Down && xbtn == XBUTTON1) ||
                    (active == MouseEventType::X2Down && xbtn == XBUTTON2)) {
                    shouldCapture = gestureEnabled;
                    m_triggerButtonDown.store(false, std::memory_order_relaxed);
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
                } else {
                    shouldCapture = false;
                }
                m_cachedForegroundWindow = nullptr;
                m_cachedModifiers = 0;
                m_cachedEdgeZone = ScreenEdgeZone::None;
                m_cachedIsTopEdge = false;
                break;
            }
            case WM_LBUTTONDOWN: {
                event.type = MouseEventType::LeftDown;
                event.edgeZone = getActiveScreenEdgeZone(data.pt, event.type);
                event.isTopEdge = (event.edgeZone == ScreenEdgeZone::Top);
                uint8_t mods = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOUSE_MOD_CTRL;
                if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOUSE_MOD_ALT;
                if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOUSE_MOD_SHIFT;

                const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
                bool canStartGesture = isLeftButtonGestureAllowed(event.edgeZone, mods, mask);

                // 桌面与文件管理窗口强保护：
                // 当鼠标落在 Windows 桌面 (Progman/WorkerW/SHELLDLL_DefView/SysListView32)、
                // 资源管理器文件窗口 (CabinetWClass/ExploreWClass)、通用文件对话框 (#32770) 或系统外壳上时，
                // 绝对禁止左键被当作手势开始拦截，保障人类桌面图标框选、移动与文件拖拽 100% 原生穿透！
                if (canStartGesture) {
                    HWND hit = WindowFromPoint(data.pt);
                    if (hit) {
                        wchar_t hitCls[256] = {};
                        GetClassNameW(hit, hitCls, 256);
                        if (isDesktopOrFileManagerWindow(hitCls)) {
                            canStartGesture = false;
                        } else {
                            HWND root = GetAncestor(hit, GA_ROOT);
                            if (root && root != hit) {
                                wchar_t rootCls[256] = {};
                                GetClassNameW(root, rootCls, 256);
                                if (isDesktopOrFileManagerWindow(rootCls)) {
                                    canStartGesture = false;
                                }
                            }
                        }
                    }
                }

                if (gestureEnabled && !m_triggerButtonDown.load(std::memory_order_relaxed) && canStartGesture) {
                    m_activeTriggerDown.store(event.type, std::memory_order_relaxed);
                    m_triggerButtonDown.store(true, std::memory_order_relaxed);
                    m_cachedForegroundWindow = GetForegroundWindow();
                    m_cachedModifiers = mods;
                    m_cachedEdgeZone = event.edgeZone;
                    m_cachedIsTopEdge = event.isTopEdge;
                    shouldCapture = true;
                } else {
                    // 常规左键点击：无论此前 MouseHook 是否记录触发键按下，均防御性复位触发态，
                    // 清空缓存的旧边缘与修饰键，防止普通点击被污染为手势边缘事件；
                    const bool wasTriggerDown = m_triggerButtonDown.load(std::memory_order_relaxed);
                    m_triggerButtonDown.store(false, std::memory_order_release);
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_release);
                    m_cachedForegroundWindow = nullptr;
                    m_cachedModifiers = 0;
                    m_cachedEdgeZone = ScreenEdgeZone::None;
                    m_cachedIsTopEdge = false;

                    // 仅当此前确有触发键处于按下状态（如右键手势追踪过程中用户按下了左键），
                    // 才将 LeftDown 推入 processEvent 驱动手势引擎执行 cancelsGestureTracking 取消自愈；
                    // 当手势引擎原本处于 Idle 状态时，严禁向其投递非手势的常规 LeftDown，
                    // 彻底阻断 GestureEngine 误判启动手势与双脑失步！
                    if (wasTriggerDown) {
                        event.foregroundWindow = GetForegroundWindow();
                        event.modifiers = mods;
                        event.edgeZone = ScreenEdgeZone::None;
                        event.isTopEdge = false;
                        m_ringBuffer.push(event);
                        processEvent(event);
                    }
                    shouldCapture = false;
                }
                tools3000::core::StatsManager::instance().recordLeftClick();
                break;
            }
            case WM_LBUTTONUP:
                event.type = MouseEventType::LeftUp;
                if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::LeftDown) {
                    shouldCapture = gestureEnabled;
                    m_triggerButtonDown.store(false, std::memory_order_relaxed);
                    m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
                } else {
                    shouldCapture = false;
                }
                m_cachedForegroundWindow = nullptr;
                m_cachedModifiers = 0;
                m_cachedEdgeZone = ScreenEdgeZone::None;
                m_cachedIsTopEdge = false;
                break;
            case WM_MOUSEWHEEL: {
                short delta = HIWORD(data.mouseData);
                event.type = delta > 0 ? MouseEventType::WheelUp : MouseEventType::WheelDown;
                event.edgeZone = getActiveScreenEdgeZone(data.pt, event.type);
                event.isTopEdge = (event.edgeZone == ScreenEdgeZone::Top);
                shouldCapture = gestureEnabled &&
                    m_triggerButtonDown.load(std::memory_order_relaxed);
                tools3000::core::StatsManager::instance().recordScroll();
                break;
            }
            default:
                break;
        }

        // 彻底消除手势钩子内部每次移动都发布 EventBus 的 1000Hz 广播与锁争用！
        if (shouldCapture) {
            event.foregroundWindow = m_cachedForegroundWindow;
            event.modifiers = m_cachedModifiers;
            event.edgeZone = m_cachedEdgeZone;
            event.isTopEdge = m_cachedIsTopEdge;

            // 极速无锁单写单读环形队列推入 (<10ns)，永不阻塞
            m_ringBuffer.push(event);

            if (processEvent(event)) {
                return true; // 拦截事件
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("MouseHook 发生未捕获异常: {}", e.what());
    } catch (...) {
        LOG_ERROR("MouseHook 发生未知异常");
    }

    return false;
}

bool MouseHook::processEvent(const MouseEvent& event) {
    MouseEventCallback cb;
    {
        std::lock_guard lock(m_callbackMutex);
        cb = m_callback;
    }

    if (cb) {
        // 极速执行手势状态机判定，彻底铲除 100ms 超时熔断与 3000ms 假死机制
        return cb(event);
    }

    if (event.type == MouseEventType::RightDown ||
        event.type == MouseEventType::MiddleDown ||
        event.type == MouseEventType::X1Down ||
        event.type == MouseEventType::X2Down) {
        return false;
    }

    m_ringBuffer.push(event);
    return false;
}

bool MouseHook::injectEventForTesting(const MouseEvent& event) {
    if (m_paused.load(std::memory_order_relaxed)) {
        return false;
    }
    m_ringBuffer.push(event);
    return processEvent(event);
}

}  // namespace tools3000::gesture
