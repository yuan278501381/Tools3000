/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#pragma once

#include "DialogEventAndLayoutPolicy.h"

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

    // 按需监控开关：无活动文件对话框时置 false 使后台线程无限期休眠（0% 空闲 CPU）
    void setActiveMonitoring(bool active);
    void requestRefresh();
    void setPathChangedCallback(std::function<void()> callback);

    // 由前台窗口钩子调用：记录任意支持的文件管理器窗口的最近激活顺序（LRU，即使最小化也保留优先级）
    void notifyFileManagerActivated(HWND hwnd);

    // 判定窗口是否为受支持的文件管理器（Windows 资源管理器 + 主流第三方管理器）
    static FileManagerKind detectFileManagerKind(HWND hwnd);

    // 获取当前前台或最近活跃的文件管理器文件夹路径 (UTF-8, 主线程 O(1) 无阻塞读取快照)
    std::string getActiveExplorerPath();

    // 专供快捷键一键跳转（后台工作线程调用）：强制执行一次实时快照并按 LRU + 多标签精确返回当前激活目录
    std::string getActiveExplorerPathFresh(uint32_t maxWaitMs = 260);

    // 获取所有当前打开的文件管理器文件夹路径列表 (UTF-8, 主线程 O(1) 无阻塞读取快照)
    std::vector<std::string> getAllOpenExplorerPaths();

    // 从 URL 或 PIDL 提取本地规范路径（支持超 MAX_PATH 的长路径与 UTF-8 百分号解码，零磁盘 I/O）
    static std::string normalizeFolderPath(const std::wstring& rawPath);

private:
    ExplorerTracker() = default;
    ~ExplorerTracker();

    void ensureStarted();
    void workerLoop();
    void refreshSnapshotOnStaThread(bool includeOnDemandUiaManagers);
    std::string selectBestActivePathLocked() const;

    mutable std::mutex m_stateMutex;
    std::condition_variable m_wakeCv;
    std::condition_variable m_doneCv;
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    bool m_activeMonitoring{false};
    bool m_refreshRequested{false};
    bool m_freshUiaRequested{false};
    uint64_t m_snapshotEpoch{0};
    uint64_t m_lastRefreshTick{0};

    // 最近激活的文件管理器窗口 LRU 列表（队首为最新激活，含已最小化窗口）
    std::vector<HWND> m_recentManagerHwnds;
    // 按顶层文件管理器 HWND 缓存其当前激活 Tab / 面板的真实文件夹路径
    std::vector<std::pair<HWND, std::string>> m_perWindowActivePaths;
    std::vector<std::string> m_allOpenPaths;
    std::function<void()> m_onPathChanged;
};

} // namespace tools3000::dialog
