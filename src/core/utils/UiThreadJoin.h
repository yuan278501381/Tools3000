#pragma once

#include <windows.h>

#include <thread>

namespace tools3000::core {

// A worker that calls HWND APIs or invokes hooks can be waiting on a synchronous message
// owned by the UI thread. A plain join() from that UI thread then deadlocks both
// sides. Dispatch sent (not queued) messages until the worker exits, with a timeout
// fallback to prevent infinite process hangs.
template <typename ThreadType>
inline bool joinWorkerWhilePumpingSentMessages(ThreadType& worker, DWORD timeoutMs = 3000) {
    if (!worker.joinable()) return true;

    HANDLE threadHandle = static_cast<HANDLE>(worker.native_handle());
    if (!threadHandle) {
        if (worker.joinable()) worker.join();
        return true;
    }

    const ULONGLONG startTime = GetTickCount64();
    while (true) {
        const ULONGLONG elapsed = GetTickCount64() - startTime;
        if (elapsed >= timeoutMs) {
            // 超时保护：解耦分离线程，坚决杜绝主线程与 UI 永久卡死
            worker.detach();
            return false;
        }
        const DWORD remaining = static_cast<DWORD>(timeoutMs - elapsed);
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            1, &threadHandle, remaining, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_OBJECT_0) {
            break;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            MSG ignored{};
            // PeekMessage dispatches pending nonqueued sent messages before it
            // examines the queue. PM_NOREMOVE leaves queued user input alone.
            PeekMessageW(&ignored, nullptr, 0, 0, PM_NOREMOVE);
            continue;
        }
        if (waitResult == WAIT_TIMEOUT) {
            worker.detach();
            return false;
        }
        break;
    }
    if (worker.joinable()) {
        worker.join();
    }
    return true;
}

}  // namespace tools3000::core
