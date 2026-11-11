#include "gesture/HotCornerEngine.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/WinUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/GestureAction.h"
#include "gesture/GestureInputPolicy.h"
#include "gesture/MouseHook.h"
#include "gesture/RadialMenuOverlay.h"
#include <algorithm>

namespace tools3000::gesture {

namespace {
constexpr size_t cornerToIndex(HotCorner corner) {
    switch (corner) {
        case HotCorner::TopLeft: return 0;
        case HotCorner::TopRight: return 1;
        case HotCorner::BottomLeft: return 2;
        case HotCorner::BottomRight: return 3;
        default: return 0;
    }
}

bool isVisibleWindowByClass(const wchar_t* className) noexcept {
    const HWND hwnd = FindWindowW(className, nullptr);
    return hwnd != nullptr && IsWindowVisible(hwnd) != FALSE;
}
} // namespace

HotCornerEngine& HotCornerEngine::instance() {
    static HotCornerEngine inst;
    return inst;
}

HotCornerEngine::~HotCornerEngine() {
    stop();
}

void HotCornerEngine::start() {
    if (m_running.exchange(true)) {
        return; // Already running
    }

    m_thread = std::jthread([this](std::stop_token st) { workerThread(st); });
    LOG_INFO("HotCornerEngine: 屏幕触发角引擎已启动");
}

void HotCornerEngine::stop() {
    if (m_running.exchange(false)) {
        m_thread.request_stop();
        m_cv.notify_all();
        if (m_thread.joinable()) {
            tools3000::core::joinWorkerWhilePumpingSentMessages(m_thread, 3000);
        }
        LOG_INFO("HotCornerEngine: 屏幕触发角引擎已停止");
    }
}

void HotCornerEngine::setCornerAction(HotCorner corner, const std::string& actionCmd) {
    if (corner == HotCorner::None) return;
    std::lock_guard lock(m_mutex);
    m_actions[cornerToIndex(corner)] = actionCmd;
}

std::string HotCornerEngine::getCornerAction(HotCorner corner) const {
    if (corner == HotCorner::None) return "";
    std::lock_guard lock(m_mutex);
    return m_actions[cornerToIndex(corner)];
}

void HotCornerEngine::setEnabled(bool enabled) {
    m_enabled.store(enabled);
}

bool HotCornerEngine::isInteractionBlockingHotCorner() noexcept {
    const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool gestureTracking =
        MouseHook::instance().isTracking() || MouseHook::instance().isTriggerButtonDown();
    const bool overlayActive =
        RadialMenuOverlay::instance().isVisible() ||
        isVisibleWindowByClass(L"Tools3000_CaptureOverlay") ||
        isVisibleWindowByClass(L"Tools3000_ScrollCaptureOverlay");
    return shouldSuppressTrigger(leftDown, rightDown, gestureTracking, overlayActive);
}

HotCorner HotCornerEngine::detectCorner(POINT pt) {
    // 获取当前所有显示器合并后的虚拟屏幕边界
    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int vRight = vLeft + vWidth - 1;
    int vBottom = vTop + vHeight - 1;

    // Windows 光标边缘可能在边界之内（多屏下也可能受物理排列影响）
    // 给一点容差（比如 1 像素），因为有些屏幕光标到不了真正的 max-1 像素
    const int tolerance = 1;

    if (pt.x <= vLeft + tolerance && pt.y <= vTop + tolerance) return HotCorner::TopLeft;
    if (pt.x >= vRight - tolerance && pt.y <= vTop + tolerance) return HotCorner::TopRight;
    if (pt.x <= vLeft + tolerance && pt.y >= vBottom - tolerance) return HotCorner::BottomLeft;
    if (pt.x >= vRight - tolerance && pt.y >= vBottom - tolerance) return HotCorner::BottomRight;

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (hMon) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMon, &mi)) {
            const int mLeft = mi.rcMonitor.left;
            const int mTop = mi.rcMonitor.top;
            const int mRight = mi.rcMonitor.right - 1;
            const int mBottom = mi.rcMonitor.bottom - 1;
            if (pt.x <= mLeft + tolerance && pt.y <= mTop + tolerance) return HotCorner::TopLeft;
            if (pt.x >= mRight - tolerance && pt.y <= mTop + tolerance) return HotCorner::TopRight;
            if (pt.x <= mLeft + tolerance && pt.y >= mBottom - tolerance) return HotCorner::BottomLeft;
            if (pt.x >= mRight - tolerance && pt.y >= mBottom - tolerance) return HotCorner::BottomRight;
        }
    }

    return HotCorner::None;
}

bool HotCornerEngine::isPassThroughCorner(POINT pt, HotCorner corner) noexcept {
    if (corner == HotCorner::None) {
        return false;
    }
    const HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!hMon) {
        return false;
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return false;
    }

    POINT probes[3]{};
    switch (corner) {
        case HotCorner::TopLeft:
            probes[0] = {mi.rcMonitor.left - 4, mi.rcMonitor.top + 1};
            probes[1] = {mi.rcMonitor.left + 1, mi.rcMonitor.top - 4};
            probes[2] = {mi.rcMonitor.left - 4, mi.rcMonitor.top - 4};
            break;
        case HotCorner::TopRight:
            probes[0] = {mi.rcMonitor.right + 3, mi.rcMonitor.top + 1};
            probes[1] = {mi.rcMonitor.right - 2, mi.rcMonitor.top - 4};
            probes[2] = {mi.rcMonitor.right + 3, mi.rcMonitor.top - 4};
            break;
        case HotCorner::BottomLeft:
            probes[0] = {mi.rcMonitor.left - 4, mi.rcMonitor.bottom - 2};
            probes[1] = {mi.rcMonitor.left + 1, mi.rcMonitor.bottom + 3};
            probes[2] = {mi.rcMonitor.left - 4, mi.rcMonitor.bottom + 3};
            break;
        case HotCorner::BottomRight:
            probes[0] = {mi.rcMonitor.right + 3, mi.rcMonitor.bottom - 2};
            probes[1] = {mi.rcMonitor.right - 2, mi.rcMonitor.bottom + 3};
            probes[2] = {mi.rcMonitor.right + 3, mi.rcMonitor.bottom + 3};
            break;
        default:
            return false;
    }

    for (const POINT& probe : probes) {
        const HMONITOR neighbor = MonitorFromPoint(probe, MONITOR_DEFAULTTONULL);
        if (neighbor != nullptr && neighbor != hMon) {
            return true;
        }
    }
    return false;
}

void HotCornerEngine::workerThread(std::stop_token stop) {
    HotCorner currentCorner = HotCorner::None;
    HMONITOR currentMonitor = nullptr;
    POINT lastPt{0, 0};
    POINT cornerAnchorPt{0, 0};
    bool hasLastPt = false;
    auto cornerEnterTime = std::chrono::steady_clock::now();
    bool triggered = false; // 避免在同一个角落一直触发

    // 冷却机制
    auto lastTriggerTime = std::chrono::steady_clock::now() - std::chrono::hours(1);
    const auto cooldownDuration = std::chrono::milliseconds(1000); // 1秒冷却

    while (!stop.stop_requested() && m_running.load()) {
        {
            std::unique_lock<std::mutex> waitLock(m_waitMutex);
            m_cv.wait_for(waitLock, std::chrono::milliseconds(50), [&]() {
                return stop.stop_requested() || !m_running.load();
            });
        }
        if (stop.stop_requested() || !m_running.load()) {
            break;
        }

        if (!m_enabled.load()) {
            currentCorner = HotCorner::None;
            currentMonitor = nullptr;
            hasLastPt = false;
            triggered = false;
            continue;
        }

        // A6: 拖拽窗口/文件、右键画手势、或截图/轮盘浮层可见时，立即重置角落驻留计时并跳过触发
        if (isInteractionBlockingHotCorner()) {
            currentCorner = HotCorner::None;
            currentMonitor = nullptr;
            hasLastPt = false;
            cornerEnterTime = std::chrono::steady_clock::now();
            triggered = false;
            continue;
        }

        POINT pt{};
        if (!GetCursorPos(&pt)) {
            continue;
        }

        const HMONITOR curMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        const HotCorner detected = detectCorner(pt);

        if (detected != currentCorner || curMonitor != currentMonitor) {
            // 角落或所属物理显示器改变（如双屏横移从左屏 TopRight 跨入右屏 TopLeft）
            currentCorner = detected;
            currentMonitor = curMonitor;
            cornerEnterTime = std::chrono::steady_clock::now();
            cornerAnchorPt = pt;
            lastPt = pt;
            hasLastPt = true;
            triggered = false;
        } else if (currentCorner != HotCorner::None && !triggered) {
            const auto now = std::chrono::steady_clock::now();
            const bool passThrough = isPassThroughCorner(pt, currentCorner);

            // 多显示器跨屏通道角防误触：若角点紧邻相邻显示器有效边界，必须要求光标完全静止停驻且无跨屏穿透位移
            if (passThrough) {
                if ((hasLastPt && (pt.x != lastPt.x || pt.y != lastPt.y)) ||
                    std::abs(pt.x - cornerAnchorPt.x) > 1 ||
                    std::abs(pt.y - cornerAnchorPt.y) > 1) {
                    cornerEnterTime = now;
                    cornerAnchorPt = pt;
                    lastPt = pt;
                    hasLastPt = true;
                    continue;
                }
            }
            lastPt = pt;
            hasLastPt = true;

            const int baseDelayMs = std::clamp(m_triggerDelayMs.load(), 100, 5000);
            const int requiredDelayMs = passThrough ? (std::max)(baseDelayMs, 350) : baseDelayMs;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - cornerEnterTime).count();

            if (elapsed >= requiredDelayMs) {
                if (passThrough) {
                    POINT verifyPt{};
                    if (!GetCursorPos(&verifyPt) ||
                        verifyPt.x != pt.x || verifyPt.y != pt.y ||
                        MonitorFromPoint(verifyPt, MONITOR_DEFAULTTONULL) != curMonitor) {
                        cornerEnterTime = now;
                        cornerAnchorPt = verifyPt;
                        lastPt = verifyPt;
                        continue;
                    }
                }

                // 触发前二次确认未处于按键按住或浮层交互态
                if (isInteractionBlockingHotCorner()) {
                    currentCorner = HotCorner::None;
                    currentMonitor = nullptr;
                    hasLastPt = false;
                    cornerEnterTime = now;
                    triggered = false;
                    continue;
                }

                // 检查全屏免打扰
                if (m_autoBypassFullscreen.load()) {
                    HWND fg = GetForegroundWindow();
                    if (fg && tools3000::core::WinUtils::shouldBypassOverlaysForWindow(fg)) {
                        LOG_TRACE("前台处于全屏独占，触发角自动静默: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(fg));
                        triggered = true;
                        continue;
                    }
                }

                // 检查冷却
                if (now - lastTriggerTime >= cooldownDuration) {
                    std::string cmd = getCornerAction(currentCorner);
                    if (!cmd.empty()) {
                        LOG_INFO("HotCornerEngine: 触发角生效角={}, 执行命令='{}'", static_cast<int>(currentCorner), cmd);

                        // 新配置保存 BuiltinCommand 的数值索引；兼容早期 capture/search 字符串。
                        if (cmd == "capture") cmd = std::to_string(static_cast<int>(BuiltinCommand::TakeScreenshot));
                        if (cmd == "search") cmd = std::to_string(static_cast<int>(BuiltinCommand::ToggleSearch));
                        try {
                            const int index = std::stoi(cmd);
                            if (isValidBuiltinCommandIndex(index)) {
                                BuiltinCommandDispatcher::instance().execute(
                                    static_cast<BuiltinCommand>(index));
                            }
                        } catch (const std::exception& e) {
                            LOG_WARN("HotCornerEngine: 忽略无效动作 '{}': {}", cmd, e.what());
                        }

                        triggered = true;
                        lastTriggerTime = std::chrono::steady_clock::now();
                    }
                }
            }
        }
    }
}

} // namespace tools3000::gesture
