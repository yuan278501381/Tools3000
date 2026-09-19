// ─────────────────────────────────────────────────────────────────────────────
// test_main.cpp — Tools3000 单元测试套件 (基于 Google Test / GMock 工业级测试架构)
//
// 覆盖核心纯业务逻辑、状态机、多格式解析器与高分屏适配:
//   • GestureRecognizer: 方向编码、转弯圆角平滑消抖 (Fillet Folding) 与孤立防抖
//   • ScopeRule: 进程与窗口类名精确/通配符匹配、正则元字符转义与 JSON 往返
//   • GestureProfile: 三态触发模式 (Default/Enabled/Disabled)、手势调序与前缀冲突检测
//   • HotkeyManager / ConfigManager / MessageBridge / EventBus / PerfMonitor
//   • PinyinEngine / SearchExpression (Everything 语法) / ContentSearchEngine
//   • DpiUtils / Lua 沙箱安全与权限持久化 / 截图、录屏与长截图 Smoke 测试
//
// 由 deploy.ps1 在 CMake 构建后统一执行，并通过 OpenCppCoverage 生成防回退覆盖率报告。
// ─────────────────────────────────────────────────────────────────────────────

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <windows.h>
#include <sddl.h>
// WIN32_LEAN_AND_MEAN removes COM declarations from windows.h. UI Automation
// MIDL headers need them before their forward declarations.
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <UIAutomation.h>

#include "gesture/GestureRecognizer.h"
#include "gesture/GestureAction.h"
#include "gesture/GestureProfile.h"
#include "gesture/GestureInputPolicy.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/HotCornerEngine.h"
#include "gesture/ScopeRule.h"
#include "gesture/RadialMenuStyle.h"
#include "capture/ClipboardUtils.h"
#include "capture/CaptureBackend.h"
#include "capture/AudioCapture.h"
#include "capture/CursorOverlay.h"
#include "capture/ScreenRecorder.h"
#include "capture/RecordingIndicator.h"
#include "capture/FloatingGlassBar.h"
#include "capture/ScrollCapture.h"
#include "capture/ScrollCaptureStorage.h"
#include "capture/ShortcutHintOverlay.h"
#include "capture/ShortcutHintStyle.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/BeautyShell.h"
#include "capture/MarkupEngine.h"
#include "capture/MarkupBaseHelper.h"
#include "capture/CornerRadiusHelper.h"
#include "capture/HudAvoidanceEngine.h"
#include "capture/PinSpawnCalculator.h"
#include "capture/PinPasteCoordinator.h"
#include "capture/CaptureHistory.h"
#include "keycast/KeycastStyle.h"
#include "keycast/KeycastOverlay.h"
#include "ocr/OcrResultStyle.h"
#include "ui/ToastStyle.h"
#include "ui/SpotlightOverlay.h"
#include "ui/SearchWindow.h"
#include "ui/WebViewOriginPolicy.h"
#include "ui/WebViewSuspend.h"
#include "ui/KeyboardPipeline.h"
#include "service/PipeEndpoint.h"
#include "ui/WebViewWindowStyle.h"
#include "capture/CaptureToolbarLayout.h"
#include "capture/CaptureDropdownMenu.h"
#include "capture/CaptureToolbarAccessibility.h"
#include "core/accessibility/OverlayUiaProvider.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/HotkeyPolicy.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/MouseHook.h"
#include "core/mouse/MouseStreamCore.h"
#include "core/mouse/MouseViewportManager.h"
#include "core/mouse/MouseFeedbackOverlayBase.h"
#include "core/ipc/MessageBridge.h"
#include "core/ipc/FileInteractionHandlers.h"
#include "core/ipc/SystemInteractionHandlers.h"
#include "core/ipc/AutoStartPolicy.h"
#include "core/plugin/PluginManifest.h"
#include "core/plugin/PluginManager.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/stats/StatsManager.h"
#include "core/update/UpdateChecker.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "core/utils/ElevationPolicy.h"
#include "core/utils/PathOperations.h"
#include "core/utils/ShellContextMenuService.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/SpscRingBuffer.h"
#include "core/lua/LuaEngine.h"
#include "search/ServiceStartupPolicy.h"
#include "service/PinyinEngine.h"
#include "service/PipeProtocol.h"
#include "service/SearchCancellation.h"
#include "service/SearchExpression.h"
#include "service/SearchRequestLimits.h"
#include "service/content/ContentSearchEngine.h"
#include "service/content/PlainTextExtractor.h"
#include "service/content/ZipXmlExtractor.h"
#include "dialog/PathMemoryManager.h"
#include "dialog/ExplorerTracker.h"
#include "dialog/DialogNavigator.h" 
#include "service/content/PsdAiExtractor.h"
#include "service/content/DxfExtractor.h"
#include "service/MftParser.h"
#include "service/db/RunHistoryManager.h"
#include "service/db/SearchHistoryManager.h"
#include "service/db/DatabaseManager.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "gesture/MouseHook.h"
#include "tray/TrayIcon.h"
#include "ui/TrayWindow.h"
#include "ui/SearchWindow.h"

namespace tools3000::ui {
TrayWindow& TrayWindow::instance() {
    static TrayWindow inst;
    static bool initialized = false;
    if (!initialized) {
        inst.m_webViewReady.store(true);
        initialized = true;
    }
    return inst;
}
void TrayWindow::preload(HINSTANCE) { m_webViewReady.store(true); }
void TrayWindow::show(HINSTANCE, int, int) { m_visible.store(true); }
void TrayWindow::setContentSize(int, int) {}
void TrayWindow::hide() { m_visible.store(false); }
bool TrayWindow::isVisible() const { return m_visible.load(); }
void TrayWindow::destroy() { m_visible.store(false); }
}

using namespace tools3000::gesture;

// ─────────────────────────────────────────────────────────────────────────────
// 模块化测试子集包含 (Modular Test Suites Inclusion)
// ─────────────────────────────────────────────────────────────────────────────
#include "test_gesture.inc"
#include "test_core.inc"
// Force recompilation of test_capture.inc v3
#include "test_capture.inc"
#include "test_search.inc"
#include "test_ui_lifecycle.inc"
#include "test_dialog.inc"
#include "test_remote.inc"
#include "test_keycast_translator.inc"

TEST(UiThreadJoinTest, PumpsCrossThreadSentMessagesDuringShutdown) {
    HWND window = CreateWindowExW(
        0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);

    std::atomic<bool> workerEntered{false};
    std::atomic<bool> sendCompleted{false};
    std::jthread worker([&](std::stop_token) {
        workerEntered.store(true, std::memory_order_release);
        SendMessageW(window, WM_NULL, 0, 0);
        sendCompleted.store(true, std::memory_order_release);
    });
    while (!workerEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    tools3000::core::joinWorkerWhilePumpingSentMessages(worker);
    EXPECT_TRUE(sendCompleted.load(std::memory_order_acquire));
    DestroyWindow(window);
}

// ─────────────────────────────────────────────────────────────────────────────
// 单元测试主入口 (Google Test 初始化与执行，包含 BeautyShell 与录屏增强测试套件)
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // 注入测试模式环境变量，彻底熔断对宿主操作系统计划任务与自启动注册表的物理写入
    SetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", L"1");
    _wputenv(L"TOOLS3000_TEST_MODE=1");

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
