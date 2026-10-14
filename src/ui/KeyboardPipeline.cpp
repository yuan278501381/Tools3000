#include "ui/KeyboardPipeline.h"
#include "core/hotkey/HotkeyManager.h"
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <string>

namespace tools3000::ui {

void KeyboardPipeline::applyWebKeyboardPolicy(ICoreWebView2Controller* controller, bool devToolsAllowed) {
    if (!controller) return;

    EventRegistrationToken token{};
    using Microsoft::WRL::Callback;
    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [devToolsAllowed](ICoreWebView2Controller* ctrl, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                if (!args || !ctrl) return S_OK;

                COREWEBVIEW2_KEY_EVENT_KIND keyKind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                args->get_KeyEventKind(&keyKind);
                UINT vKey = 0;
                args->get_VirtualKey(&vKey);

                const bool isKeyDown = (keyKind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
                                        keyKind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN);
                const bool isKeyUp = (keyKind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP ||
                                      keyKind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_UP);
                const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool winDown = ((GetKeyState(VK_LWIN) & 0x8000) != 0) || ((GetKeyState(VK_RWIN) & 0x8000) != 0);

                const bool isRecording = tools3000::core::HotkeyManager::instance().isPaused();

                // 1. 录制状态下（用户正在 UI 录入器中捕获快捷键）：
                // 彻底放行所有按键；对 F1~F24 及 Alt+Space 主动向 DOM 派发合成 KeyboardEvent，杜绝 WebView2 静默吞没
                if (isRecording) {
                    if (vKey >= VK_F1 && vKey <= VK_F24) {
                        args->put_Handled(TRUE);
                        Microsoft::WRL::ComPtr<ICoreWebView2> webView;
                        if (SUCCEEDED(ctrl->get_CoreWebView2(&webView)) && webView) {
                            const wchar_t* eventType = isKeyDown ? L"keydown" : (isKeyUp ? L"keyup" : nullptr);
                            if (eventType) {
                                std::wstring fName = L"F" + std::to_wstring(vKey - VK_F1 + 1);
                                std::wstring script = L"window.dispatchEvent(new KeyboardEvent('" + std::wstring(eventType) +
                                    L"', { key: '" + fName + L"', code: '" + fName + L"', keyCode: " + std::to_wstring(vKey) +
                                    L", which: " + std::to_wstring(vKey) + L", altKey: " +
                                    (altDown ? L"true" : L"false") + L", ctrlKey: " +
                                    (ctrlDown ? L"true" : L"false") + L", shiftKey: " +
                                    (shiftDown ? L"true" : L"false") + L", metaKey: " +
                                    (winDown ? L"true" : L"false") + L", bubbles: true, cancelable: true }));";
                                webView->ExecuteScript(script.c_str(), nullptr);
                            }
                        }
                        return S_OK;
                    }

                    if (vKey == VK_SPACE && altDown) {
                        args->put_Handled(TRUE);
                        Microsoft::WRL::ComPtr<ICoreWebView2> webView;
                        if (SUCCEEDED(ctrl->get_CoreWebView2(&webView)) && webView) {
                            const wchar_t* eventType = isKeyDown ? L"keydown" : (isKeyUp ? L"keyup" : nullptr);
                            if (eventType) {
                                std::wstring script = L"window.dispatchEvent(new KeyboardEvent('" + std::wstring(eventType) +
                                    L"', { key: ' ', code: 'Space', keyCode: 32, which: 32, altKey: true, ctrlKey: " +
                                    (ctrlDown ? L"true" : L"false") + L", shiftKey: " +
                                    (shiftDown ? L"true" : L"false") + L", metaKey: " +
                                    (winDown ? L"true" : L"false") + L", bubbles: true, cancelable: true }));";
                                webView->ExecuteScript(script.c_str(), nullptr);
                            }
                        }
                        return S_OK;
                    }

                    // 其余按键（包含字母、数字、Ctrl+P、Ctrl+S 等快捷录入）全部无条件放行给 DOM
                    args->put_Handled(FALSE);
                    return S_OK;
                }

                // 2. 非录制状态：屏蔽不需要的 Chromium 浏览器默认行为（打印、查找、下载、历史记录等）
                if (isKeyDown) {
                    if (ctrlDown && !altDown && !shiftDown) {
                        switch (vKey) {
                            case 'P': case 'F': case 'U': case 'J':
                            case 'H': case 'O': case 'S':
                                args->put_Handled(TRUE);
                                return S_OK;
                            case 'R':
                                if (!devToolsAllowed) {
                                    args->put_Handled(TRUE);
                                    return S_OK;
                                }
                                break;
                            default:
                                break;
                        }
                    }

                    // F1~F24、F3、F7 等浏览器默认行为已由 ICoreWebView2Settings3::put_AreBrowserAcceleratorKeysEnabled(FALSE) 原生屏蔽
                    // 此处无需重复吞没，确保所有按键正常穿透至 Web DOM

                    // 屏蔽 F5 刷新（非调试模式）
                    if (vKey == VK_F5 && !devToolsAllowed) {
                        args->put_Handled(TRUE);
                        return S_OK;
                    }

                    // 屏蔽 Alt+Left / Alt+Right 触发的浏览器后退/前进
                    if (altDown && (vKey == VK_LEFT || vKey == VK_RIGHT)) {
                        args->put_Handled(TRUE);
                        return S_OK;
                    }
                }

                // 3. 特别处理 Alt+Space 系统菜单键：
                // Windows Chromium 内核会将 Alt+Space 判定为原生窗口菜单加速键直接吞没，不派发给 Web DOM；
                // 此处在捕获到 Alt+Space 时，阻止系统菜单并向 WebView2 DOM 主动注入合成 KeyboardEvent！
                if (vKey == VK_SPACE && altDown) {
                    args->put_Handled(TRUE);

                    Microsoft::WRL::ComPtr<ICoreWebView2> webView;
                    if (SUCCEEDED(ctrl->get_CoreWebView2(&webView)) && webView) {
                        const wchar_t* eventType = isKeyDown ? L"keydown" : (isKeyUp ? L"keyup" : nullptr);
                        if (eventType) {
                            std::wstring script = L"window.dispatchEvent(new KeyboardEvent('" + std::wstring(eventType) +
                                L"', { key: ' ', code: 'Space', keyCode: 32, which: 32, altKey: true, ctrlKey: " +
                                (ctrlDown ? L"true" : L"false") + L", shiftKey: " +
                                (shiftDown ? L"true" : L"false") + L", metaKey: " +
                                (winDown ? L"true" : L"false") + L", bubbles: true, cancelable: true }));";
                            webView->ExecuteScript(script.c_str(), nullptr);
                        }
                    }
                    return S_OK;
                }

                // 4. 常规按键（包含 Alt+V、Ctrl+Shift+... 等）全部放行给 DOM
                args->put_Handled(FALSE);
                return S_OK;
            }
        ).Get(),
        &token
    );
}

} // namespace tools3000::ui
