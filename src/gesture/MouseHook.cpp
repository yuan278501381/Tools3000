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
    m_callback = callback;
    if (callback) {
        m_callbackHolder = std::make_unique<MouseEventCallback>(std::move(callback));
        m_atomicCallback.store(m_callbackHolder.get(), std::memory_order_release);
        m_hasCallback.store(true, std::memory_order_release);
    } else {
        m_hasCallback.store(false, std::memory_order_release);
        m_atomicCallback.store(nullptr, std::memory_order_release);
        m_callbackHolder.reset();
    }
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
    m_isTracking.store(false, std::memory_order_release);
    m_cachedForegroundWindow = nullptr;
    m_cachedModifiers = 0;
    m_cachedEdgeZone = ScreenEdgeZone::None;
    m_cachedIsTopEdge = false;
}

std::vector<RawInputPacket> MouseHook::drainRawPackets(size_t maxCount) {
    std::vector<RawInputPacket> packets;
    packets.reserve(std::min(maxCount, m_rawRingBuffer.size()));
    RawInputPacket pkt;
    while (packets.size() < maxCount && m_rawRingBuffer.pop(pkt)) {
        packets.push_back(pkt);
    }
    return packets;
}

std::vector<MouseEvent> MouseHook::drainEvents(size_t maxCount) {
    std::vector<MouseEvent> events;
    events.reserve(std::min(maxCount, m_rawRingBuffer.size()));
    RawInputPacket pkt;
    while (events.size() < maxCount && m_rawRingBuffer.pop(pkt)) {
        MouseEvent ev{};
        ev.position = {pkt.x, pkt.y};
        ev.timestamp = std::chrono::steady_clock::now();
        switch (pkt.message) {
            case WM_MOUSEMOVE: ev.type = MouseEventType::Move; break;
            case WM_RBUTTONDOWN: ev.type = MouseEventType::RightDown; break;
            case WM_RBUTTONUP: ev.type = MouseEventType::RightUp; break;
            case WM_MBUTTONDOWN: ev.type = MouseEventType::MiddleDown; break;
            case WM_MBUTTONUP: ev.type = MouseEventType::MiddleUp; break;
            case WM_LBUTTONDOWN: ev.type = MouseEventType::LeftDown; break;
            case WM_LBUTTONUP: ev.type = MouseEventType::LeftUp; break;
            case WM_XBUTTONDOWN: ev.type = (HIWORD(pkt.mouseData) == XBUTTON2) ? MouseEventType::X2Down : MouseEventType::X1Down; break;
            case WM_XBUTTONUP: ev.type = (HIWORD(pkt.mouseData) == XBUTTON2) ? MouseEventType::X2Up : MouseEventType::X1Up; break;
            case WM_MOUSEWHEEL: ev.type = (static_cast<short>(HIWORD(pkt.mouseData)) > 0) ? MouseEventType::WheelUp : MouseEventType::WheelDown; break;
            default: continue;
        }
        events.push_back(ev);
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

    // 1. 组装 POD 结构体 RawInputPacket (24 字节, 0 堆分配)
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const RawInputPacket packet{
        static_cast<uint32_t>(wParam),
        static_cast<int32_t>(data.pt.x),
        static_cast<int32_t>(data.pt.y),
        static_cast<uint32_t>(data.mouseData),
        static_cast<uint64_t>(qpc.QuadPart)
    };

    // 2. 无锁推入 Input SpscRingBuffer (4096 槽位, 0 锁争用, 0 阻塞, <10ns)
    m_rawRingBuffer.push(packet);
    notifyWorker();

    // 3. 纯原子无锁状态机判定拦截策略 (完全消除所有互斥锁与 Win32 同步系统调用)
    const bool gestureEnabled = !m_paused.load(std::memory_order_relaxed);
    bool shouldCapture = false;
    bool wasTriggerDown = false;

    switch (wParam) {
        case WM_MOUSEMOVE: {
            shouldCapture = false; // 鼠标移动绝不拦截，保障原生指针 144Hz+ 极致跟手
            static POINT lastPt = { -1, -1 };
            if (lastPt.x != -1 && lastPt.y != -1) {
                const double dx = data.pt.x - lastPt.x;
                const double dy = data.pt.y - lastPt.y;
                const double dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 0) {
                    tools3000::core::StatsManager::instance().recordMouseDistance(dist);
                }
            }
            lastPt = data.pt;
            break;
        }
        case WM_RBUTTONDOWN: {
            const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
            const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
            const bool rightAllowed = ((mask & GestureTriggerMask::Right) != 0) || ((mask & GestureTriggerMask::AnyEdge) != 0);
            const bool modeAllowed = (mode == TriggerMode::RightOnly || mode == TriggerMode::Both || mode == TriggerMode::All);
            if (gestureEnabled && !m_triggerButtonDown.load(std::memory_order_relaxed) && rightAllowed && modeAllowed) {
                m_activeTriggerDown.store(MouseEventType::RightDown, std::memory_order_relaxed);
                m_triggerButtonDown.store(true, std::memory_order_relaxed);
                shouldCapture = true;
            }
            tools3000::core::StatsManager::instance().recordRightClick();
            break;
        }
        case WM_RBUTTONUP: {
            if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::RightDown) {
                shouldCapture = gestureEnabled;
                m_triggerButtonDown.store(false, std::memory_order_relaxed);
                m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
            } else {
                shouldCapture = false;
            }
            break;
        }
        case WM_MBUTTONDOWN: {
            const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
            const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
            const bool middleAllowed = ((mask & GestureTriggerMask::Middle) != 0) || ((mask & GestureTriggerMask::AnyEdge) != 0);
            const bool modeAllowed = (mode == TriggerMode::MiddleOnly || mode == TriggerMode::Both || mode == TriggerMode::All);
            if (gestureEnabled && !m_triggerButtonDown.load(std::memory_order_relaxed) && middleAllowed && modeAllowed) {
                m_activeTriggerDown.store(MouseEventType::MiddleDown, std::memory_order_relaxed);
                m_triggerButtonDown.store(true, std::memory_order_relaxed);
                shouldCapture = true;
            }
            break;
        }
        case WM_MBUTTONUP: {
            if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::MiddleDown) {
                shouldCapture = gestureEnabled;
                m_triggerButtonDown.store(false, std::memory_order_relaxed);
                m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
            } else {
                shouldCapture = false;
            }
            break;
        }
        case WM_XBUTTONDOWN: {
            const WORD xbtn = HIWORD(data.mouseData);
            const MouseEventType type = (xbtn == XBUTTON2) ? MouseEventType::X2Down : MouseEventType::X1Down;
            const auto mode = m_configuredTriggerMode.load(std::memory_order_relaxed);
            const uint32_t mask = m_activeTriggerMask.load(std::memory_order_relaxed);
            const bool allowX = (mode == TriggerMode::Both || mode == TriggerMode::All ||
                                 (mode == TriggerMode::X1Only && xbtn == XBUTTON1) ||
                                 (mode == TriggerMode::X2Only && xbtn == XBUTTON2));
            const bool xAllowed = ((xbtn == XBUTTON1 && (mask & GestureTriggerMask::X1)) ||
                                   (xbtn == XBUTTON2 && (mask & GestureTriggerMask::X2))) ||
                                  ((mask & GestureTriggerMask::AnyEdge) != 0);
            if (gestureEnabled && !m_triggerButtonDown.load(std::memory_order_relaxed) && allowX && xAllowed) {
                m_activeTriggerDown.store(type, std::memory_order_relaxed);
                m_triggerButtonDown.store(true, std::memory_order_relaxed);
                shouldCapture = true;
            }
            break;
        }
        case WM_XBUTTONUP: {
            const WORD xbtn = HIWORD(data.mouseData);
            const auto active = m_activeTriggerDown.load(std::memory_order_relaxed);
            if ((active == MouseEventType::X1Down && xbtn == XBUTTON1) ||
                (active == MouseEventType::X2Down && xbtn == XBUTTON2)) {
                shouldCapture = gestureEnabled;
                m_triggerButtonDown.store(false, std::memory_order_relaxed);
                m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
            } else {
                shouldCapture = false;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            wasTriggerDown = m_triggerButtonDown.load(std::memory_order_relaxed);
            m_triggerButtonDown.store(false, std::memory_order_release);
            m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_release);
            tools3000::core::StatsManager::instance().recordLeftClick();
            // 左键按下绝对 100% 穿透放行，保障人类日常点击与框选
            shouldCapture = false;
            break;
        }
        case WM_LBUTTONUP: {
            if (m_activeTriggerDown.load(std::memory_order_relaxed) == MouseEventType::LeftDown) {
                m_triggerButtonDown.store(false, std::memory_order_relaxed);
                m_activeTriggerDown.store(MouseEventType::Move, std::memory_order_relaxed);
            }
            shouldCapture = false;
            break;
        }
        case WM_MOUSEWHEEL: {
            tools3000::core::StatsManager::instance().recordScroll();
            if (gestureEnabled) {
                if (m_triggerButtonDown.load(std::memory_order_relaxed)) {
                    shouldCapture = true; // 按住手势触发键时的滚轮滚动进行拦截
                } else {
                    const short delta = static_cast<short>(HIWORD(data.mouseData));
                    const MouseEventType wheelType = (delta > 0) ? MouseEventType::WheelUp : MouseEventType::WheelDown;
                    if (getActiveScreenEdgeZone(data.pt, wheelType) != ScreenEdgeZone::None) {
                        shouldCapture = true; // 屏幕边缘滚轮手势进行拦截
                    }
                }
            }
            break;
        }
        default:
            break;
    }

    // 4. 同步测试回调支持：仅在异步工作线程未激活 (即独立单元测试环境) 且显式注册了测试 Mock 回调时，才执行同步测试派发
    if (!isAsyncWorkerActive() && m_hasCallback.load(std::memory_order_relaxed)) {
        auto* cb = m_atomicCallback.load(std::memory_order_acquire);
        if (cb && *cb) {
            MouseEvent ev{};
            ev.position = data.pt;
            ev.timestamp = std::chrono::steady_clock::now();
            bool shouldDispatch = true;
            switch (wParam) {
                case WM_MOUSEMOVE: ev.type = MouseEventType::Move; break;
                case WM_RBUTTONDOWN: ev.type = MouseEventType::RightDown; break;
                case WM_RBUTTONUP: ev.type = MouseEventType::RightUp; break;
                case WM_MBUTTONDOWN: ev.type = MouseEventType::MiddleDown; break;
                case WM_MBUTTONUP: ev.type = MouseEventType::MiddleUp; break;
                case WM_LBUTTONDOWN: {
                    ev.type = MouseEventType::LeftDown;
                    // 仅当此前确有触发键处于按下状态（异常中断自愈）时，才投递 LeftDown 给测试回调
                    if (!wasTriggerDown) {
                        shouldDispatch = false;
                    }
                    break;
                }
                case WM_LBUTTONUP: {
                    // 左键抬起不派发给手势事件回调
                    shouldDispatch = false;
                    break;
                }
                case WM_XBUTTONDOWN: ev.type = (HIWORD(data.mouseData) == XBUTTON2) ? MouseEventType::X2Down : MouseEventType::X1Down; break;
                case WM_XBUTTONUP: ev.type = (HIWORD(data.mouseData) == XBUTTON2) ? MouseEventType::X2Up : MouseEventType::X1Up; break;
                case WM_MOUSEWHEEL: ev.type = (static_cast<short>(HIWORD(data.mouseData)) > 0) ? MouseEventType::WheelUp : MouseEventType::WheelDown; break;
                default: shouldDispatch = false; break;
            }
            if (shouldDispatch) {
                bool cbIntercept = (*cb)(ev);
                if (wParam == WM_MOUSEWHEEL || wParam == WM_RBUTTONUP || wParam == WM_MBUTTONUP || wParam == WM_XBUTTONUP) {
                    shouldCapture = cbIntercept;
                }
            }
        }
    }

    return shouldCapture;
}

bool MouseHook::processEvent(const MouseEvent& event) {
    if (m_hasCallback.load(std::memory_order_relaxed)) {
        auto* cb = m_atomicCallback.load(std::memory_order_acquire);
        if (cb && *cb) {
            return (*cb)(event);
        }
    }

    if (event.type == MouseEventType::RightDown ||
        event.type == MouseEventType::MiddleDown ||
        event.type == MouseEventType::X1Down ||
        event.type == MouseEventType::X2Down) {
        return false;
    }

    return false;
}

bool MouseHook::injectEventForTesting(const MouseEvent& event) {
    if (m_paused.load(std::memory_order_relaxed)) {
        return false;
    }
    RawInputPacket pkt{};
    pkt.x = event.position.x;
    pkt.y = event.position.y;
    switch (event.type) {
        case MouseEventType::Move: pkt.message = WM_MOUSEMOVE; break;
        case MouseEventType::RightDown: pkt.message = WM_RBUTTONDOWN; break;
        case MouseEventType::RightUp: pkt.message = WM_RBUTTONUP; break;
        case MouseEventType::MiddleDown: pkt.message = WM_MBUTTONDOWN; break;
        case MouseEventType::MiddleUp: pkt.message = WM_MBUTTONUP; break;
        case MouseEventType::LeftDown: pkt.message = WM_LBUTTONDOWN; break;
        case MouseEventType::LeftUp: pkt.message = WM_LBUTTONUP; break;
        case MouseEventType::X1Down: pkt.message = WM_XBUTTONDOWN; pkt.mouseData = MAKELONG(0, XBUTTON1); break;
        case MouseEventType::X1Up: pkt.message = WM_XBUTTONUP; pkt.mouseData = MAKELONG(0, XBUTTON1); break;
        case MouseEventType::X2Down: pkt.message = WM_XBUTTONDOWN; pkt.mouseData = MAKELONG(0, XBUTTON2); break;
        case MouseEventType::X2Up: pkt.message = WM_XBUTTONUP; pkt.mouseData = MAKELONG(0, XBUTTON2); break;
        case MouseEventType::WheelUp: pkt.message = WM_MOUSEWHEEL; pkt.mouseData = MAKELONG(0, 120); break;
        case MouseEventType::WheelDown: pkt.message = WM_MOUSEWHEEL; pkt.mouseData = MAKELONG(0, static_cast<WORD>(-120)); break;
    }
    m_rawRingBuffer.push(pkt);
    notifyWorker();
    return processEvent(event);
}

}  // namespace tools3000::gesture
