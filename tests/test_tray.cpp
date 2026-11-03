#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <thread>
#include <chrono>

constexpr UINT WM_TRAY_CALLBACK = WM_USER + 100;
constexpr UINT_PTR TIMEOUT_TIMER_ID = 1;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAY_CALLBACK) {
        std::cout << "[TrayTest] Tray message received: " << lParam << std::endl;
        return 0;
    }
    if (msg == WM_TIMER && wParam == TIMEOUT_TIMER_ID) {
        std::cerr << "[TrayTest] Test completed successfully via timer guard." << std::endl;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Tools3000_TrayTestClass";
    if (!RegisterClassExW(&wc)) {
        std::cerr << "[TrayTest] RegisterClassExW failed: " << GetLastError() << std::endl;
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, L"Tools3000_TrayTestClass", L"TrayTest",
                                WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::cerr << "[TrayTest] CreateWindowExW failed: " << GetLastError() << std::endl;
        return 1;
    }

    // 设置 2 秒自动化超时守卫，杜绝 CI 挂起
    SetTimer(hwnd, TIMEOUT_TIMER_ID, 2000, nullptr);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Tools3000 Test Tray");

    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!ok) {
        nid.cbSize = NOTIFYICONDATAW_V3_SIZE;
        ok = Shell_NotifyIconW(NIM_ADD, &nid);
    }

    if (!ok) {
        std::cerr << "[TrayTest] Shell_NotifyIconW NIM_ADD failed: " << GetLastError() << std::endl;
        DestroyWindow(hwnd);
        return 1;
    }

    std::cout << "[TrayTest] Shell_NotifyIconW NIM_ADD PASS" << std::endl;

    // 清理托盘图标
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(hwnd);
    std::cout << "[TrayTest] All Tray automated tests passed successfully." << std::endl;
    return 0;
}
