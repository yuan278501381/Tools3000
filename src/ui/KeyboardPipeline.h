#pragma once

#include <windows.h>

struct ICoreWebView2Controller;

namespace tools3000::ui {

class KeyboardPipeline {
public:
    /// 处理 Win32 宿主窗口消息中的系统快捷键与原生菜单干扰
    /// @return true 表示消息已被管线拦截消费，窗口过程应直接返回 0
    static inline bool filterWindowMessage(HWND /*hwnd*/, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
        switch (msg) {
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP: {
                // 拦截 Alt+Space (VK_SPACE) 以及单独的 F10、Alt 键，避免系统窗口菜单截获
                if (wParam == VK_SPACE || wParam == VK_F10 || wParam == VK_MENU) {
                    return true;
                }
                break;
            }
            case WM_SYSCOMMAND: {
                const WPARAM cmd = wParam & 0xFFF0;
                // 1. 拦截由 Alt / F10 / Alt+Space / Alt+字母触发的 Win32 原生窗口菜单与菜单栏激活
                if (cmd == SC_KEYMENU || cmd == SC_CONTEXTHELP) {
                    return true;
                }
                break;
            }
            case WM_SYSCHAR: {
                // 拦截 Alt+字母/数字 触发的系统字符蜂鸣器与原生助记键
                return true;
            }
            case WM_HELP: {
                // 拦截 F1 帮助广播
                return true;
            }
            default:
                break;
        }
        return false;
    }

    /// 为 WebView2 控制器配置全局键盘加速器管线
    /// 屏蔽浏览器默认功能键干扰（打印、查找、页面回退等），并将所有常规按键与修饰键组合 100% 透传给前端 DOM
    static void applyWebKeyboardPolicy(ICoreWebView2Controller* controller, bool devToolsAllowed = false);
};

} // namespace tools3000::ui
