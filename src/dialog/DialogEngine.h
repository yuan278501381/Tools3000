/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace tools3000::dialog {

class DialogEngine {
public:
    static DialogEngine& instance();

    bool start();
    void stop();
    bool isRunning() const { return m_running.load(); }

    void onWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild);
    static bool shouldProcessWinEvent(DWORD event, HWND hwnd, LONG idObject, LONG idChild) noexcept;

private:
    DialogEngine() = default;
    ~DialogEngine() = default;

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime
    );

    void handleDialogShown(HWND hwnd);
    void handleDialogHiding(HWND hwnd);
    void handleDialogDestroyed(HWND hwnd);
    void handleForegroundChange(HWND hwnd);
    void handleDialogLocationChange(HWND hwnd);
    void handleDialogInvoked(HWND hwnd, LONG idObject, LONG idChild);
    void monitorThreadMain();
    void finalizeDialog(HWND hwnd, bool windowStillReadable = false);
    static HWND rootWindow(HWND hwnd);

    // 专用钩子线程：SetWinEventHook(WINEVENT_OUTOFCONTEXT) 必须在有消息循环的线程注册
    // 独立线程自己跑 GetMessage 循环，完全脱离主线程的限制
    void hookThreadMain();

    std::atomic<bool> m_running{false};
    std::atomic<HWINEVENTHOOK> m_hookShow{nullptr};
    std::atomic<HWINEVENTHOOK> m_hookForeground{nullptr};
    std::atomic<HWINEVENTHOOK> m_hookLocation{nullptr};
    std::atomic<HWINEVENTHOOK> m_hookInvoke{nullptr};

    std::thread m_hookThread;             // 专用钩子线程
    std::atomic<DWORD> m_hookThreadId{0}; // 钩子线程 tid，用于 PostThreadMessage
    std::atomic<bool> m_hookReady{false}; // 钩子注册完毕信号
    std::mutex m_hookStartupMutex;
    std::condition_variable m_hookStartupCv;

    struct DialogSession {
        HWND hwnd{nullptr};
        HWND okButtonHwnd{nullptr};
        HWND cancelButtonHwnd{nullptr};
        RECT okButtonRect{};
        RECT cancelButtonRect{};
        RECT dialogRect{};
        RECT shellViewRect{};
        float dpiScale{1.0f};
        DWORD processId{0};
        int dialogType{0};
        std::string processName;
        std::string restorePath;
        std::string initialFolder;
        std::string currentFolder;
        std::string initialSelection;
        std::string lastSelection;
        std::chrono::steady_clock::time_point discoveredAt{};
        std::chrono::steady_clock::time_point lastPoll{};
        unsigned detectionAttempts{0};
        bool initialized{false};
        bool isInstaller{false};
        bool restoreAttempted{false};
        bool selectionChanged{false};
        bool escapeObserved{false};
        bool enterObserved{false};
        bool confirmed{false};
        bool cancelled{false};
        bool closeRequested{false};
        bool readableAtClose{false};
    };

    std::thread m_monitorThread;
    std::condition_variable m_monitorCv;
    std::unordered_map<HWND, DialogSession> m_sessions;
    std::mutex m_mutex;
};

} // namespace tools3000::dialog
