// ─────────────────────────────────────────────────────────────────────────────
// GestureDispatchWorker.cpp — 异步手势输入分发工作线程实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureDispatchWorker.h"
#include "gesture/GestureEngine.h"
#include "core/logger/Logger.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

namespace tools3000::gesture {

GestureDispatchWorker& GestureDispatchWorker::instance() {
    static GestureDispatchWorker s_instance;
    return s_instance;
}

GestureDispatchWorker::GestureDispatchWorker() = default;

GestureDispatchWorker::~GestureDispatchWorker() {
    stop();
}

void GestureDispatchWorker::start() {
    if (m_running.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    if (!m_wakeEvent) {
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    MouseHook::instance().setWorkerWakeEvent(m_wakeEvent);

    m_workerThread = std::jthread([this](std::stop_token st) {
        workerLoop(st);
    });

    LOG_INFO("GestureDispatchWorker: 异步手势输入分发工作线程已启动");
}

void GestureDispatchWorker::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    MouseHook::instance().setWorkerWakeEvent(nullptr);

    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }

    if (m_workerThread.joinable()) {
        m_workerThread.request_stop();
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_workerThread);
    }

    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }

    LOG_INFO("GestureDispatchWorker: 异步手势输入分发工作线程已安全停止, 累计处理包数={}",
             m_totalProcessedPackets.load());
}

void GestureDispatchWorker::workerLoop(std::stop_token stopToken) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    while (!stopToken.stop_requested()) {
        try {
            MouseHook::instance().setWorkerActive(true);
            const size_t processed = processBatch(128);
            MouseHook::instance().setWorkerActive(false);

            if (processed == 0) {
                // 队列已排空，休眠等待内核事件唤醒（节流降耗，避免 100% CPU 空转）
                WaitForSingleObject(m_wakeEvent, 2);
            }
        } catch (const std::exception& ex) {
            MouseHook::instance().setWorkerActive(false);
            LOG_ERROR("GestureDispatchWorker: 异步分发工作循环捕获异常: {}", ex.what());
        } catch (...) {
            MouseHook::instance().setWorkerActive(false);
            LOG_ERROR("GestureDispatchWorker: 异步分发工作循环捕获未知异常");
        }
    }

    // 退出前排空残留数据包
    try {
        processBatch(512);
    } catch (const std::exception& ex) {
        LOG_ERROR("GestureDispatchWorker: 退出排空捕获异常: {}", ex.what());
    } catch (...) {
        LOG_ERROR("GestureDispatchWorker: 退出排空捕获未知异常");
    }
}

size_t GestureDispatchWorker::processBatch(size_t maxBatch) {
    auto& ring = MouseHook::instance().rawRingBuffer();
    RawInputPacket pkt;
    size_t count = 0;

    while (count < maxBatch && ring.pop(pkt)) {
        ++count;
        m_totalProcessedPackets.fetch_add(1, std::memory_order_relaxed);

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

        // 异步查询前台窗口、屏幕边缘与修饰键（在后台工作线程中完成，完全不阻塞钩子线程）
        if (ev.type == MouseEventType::RightDown ||
            ev.type == MouseEventType::MiddleDown ||
            ev.type == MouseEventType::X1Down ||
            ev.type == MouseEventType::X2Down ||
            ev.type == MouseEventType::WheelUp ||
            ev.type == MouseEventType::WheelDown) {
            m_cachedForegroundWindow = GetForegroundWindow();
            uint8_t mods = 0;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOUSE_MOD_CTRL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOUSE_MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOUSE_MOD_SHIFT;
            m_cachedModifiers = mods;
            m_cachedEdgeZone = MouseHook::instance().getActiveScreenEdgeZone(ev.position, ev.type);
            m_cachedIsTopEdge = (m_cachedEdgeZone == ScreenEdgeZone::Top);
        }

        ev.foregroundWindow = m_cachedForegroundWindow;
        ev.modifiers = m_cachedModifiers;
        ev.edgeZone = m_cachedEdgeZone;
        ev.isTopEdge = m_cachedIsTopEdge;

        // 派发至 GestureEngine 状态机
        GestureEngine::instance().onMouseEvent(ev);

        // 抬起、取消或独立滚轮脉冲后清除缓存状态
        if (ev.type == MouseEventType::RightUp ||
            ev.type == MouseEventType::MiddleUp ||
            ev.type == MouseEventType::X1Up ||
            ev.type == MouseEventType::X2Up ||
            ev.type == MouseEventType::LeftDown ||
            ((ev.type == MouseEventType::WheelUp || ev.type == MouseEventType::WheelDown) &&
             !MouseHook::instance().isTriggerButtonDown())) {
            m_cachedForegroundWindow = nullptr;
            m_cachedModifiers = 0;
            m_cachedEdgeZone = ScreenEdgeZone::None;
            m_cachedIsTopEdge = false;
        }
    }

    return count;
}

}  // namespace tools3000::gesture
