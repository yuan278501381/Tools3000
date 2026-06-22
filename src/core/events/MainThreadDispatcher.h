#pragma once

#include "core/utils/Export.h"

#include <windows.h>
#include <functional>

namespace tools3000::core {

// Central Win32 UI-thread queue. Native overlays, WebView2, and most window
// objects in Tools3000 are thread-affine; workers must marshal work through it.
class TOOLS3000CORE_API MainThreadDispatcher {
public:
    using Task = std::function<void()>;
    static constexpr UINT MessageId = WM_APP + 0x51;

    static MainThreadDispatcher& instance();
    void initialize(HWND messageWindow);
    void shutdown();
    bool post(Task task);
    // Always enqueue the task, even when called by the owner thread. This is
    // useful for escaping latency-sensitive callbacks such as WH_MOUSE_LL.
    bool postDeferred(Task task);
    void drain();
    bool isOwnerThread() const;
    bool isInitialized() const;

private:
    MainThreadDispatcher() = default;
    MainThreadDispatcher(const MainThreadDispatcher&) = delete;
    MainThreadDispatcher& operator=(const MainThreadDispatcher&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};

}  // namespace tools3000::core
