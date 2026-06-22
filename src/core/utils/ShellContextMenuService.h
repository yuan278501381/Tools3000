#pragma once

#include "core/utils/Export.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <windows.h>

namespace tools3000::core {

/// 宿主拥有的常驻 Shell STA 菜单守护工作器。
/// 采用单例常驻 STA 消息循环架构，杜绝线程频繁创建、孤儿线程死锁与 COM 套间并发冲突。
class TOOLS3000CORE_API ShellContextMenuService final {
public:
    static ShellContextMenuService& instance();

    bool showAsync(std::wstring path, int screenX = -1, int screenY = -1);
    bool showAsync(std::wstring path, int screenX, int screenY, bool extendedVerbs);
    bool showDirect(std::wstring path, int screenX = -1, int screenY = -1, bool extendedVerbs = false);
    void shutdown();

private:
    ShellContextMenuService();
    ~ShellContextMenuService();
    ShellContextMenuService(const ShellContextMenuService&) = delete;
    ShellContextMenuService& operator=(const ShellContextMenuService&) = delete;

    struct MenuRequest {
        std::wstring path;
        int screenX{-1};
        int screenY{-1};
        bool extendedVerbs{false};
    };

    void ensureThreadStarted();
    void threadMain();
    void processRequest(const MenuRequest& req);

    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::queue<MenuRequest> m_queue;

    std::thread m_thread;
    std::atomic<HWND> m_helperWindow{nullptr};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_stopping{false};
};

}  // namespace tools3000::core
