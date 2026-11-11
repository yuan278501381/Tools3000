/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

namespace tools3000::dialog {

class ExplorerTracker {
public:
    static ExplorerTracker& instance();

    void start();
    void shutdown();
    void requestRefresh();
    void setPathChangedCallback(std::function<void()> callback);

    // 获取当前前台或最近活跃的资源管理器文件夹路径 (UTF-8, 主线程 O(1) 无阻塞读取快照)
    std::string getActiveExplorerPath();

    // 获取所有当前打开的资源管理器文件夹路径列表 (UTF-8, 主线程 O(1) 无阻塞读取快照)
    std::vector<std::string> getAllOpenExplorerPaths();

    // 从 URL 或 PIDL 提取本地规范路径（支持超 MAX_PATH 的长路径与 UTF-8 百分号解码，零磁盘 I/O）
    static std::string normalizeFolderPath(const std::wstring& rawPath);

private:
    ExplorerTracker() = default;
    ~ExplorerTracker();

    void ensureStarted();
    void workerLoop();
    void refreshSnapshotOnStaThread();

    mutable std::mutex m_stateMutex;
    std::condition_variable m_wakeCv;
    std::condition_variable m_doneCv;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    bool m_refreshRequested = false;
    uint64_t m_snapshotEpoch = 0;
    uint64_t m_lastRefreshTick = 0;

    // 按顶层 Explorer HWND 缓存其当前激活 Tab 的真实文件夹路径
    std::vector<std::pair<HWND, std::string>> m_perWindowActivePaths;
    std::vector<std::string> m_allOpenPaths;
    std::function<void()> m_onPathChanged;
};

} // namespace tools3000::dialog
