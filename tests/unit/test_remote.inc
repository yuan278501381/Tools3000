// ─────────────────────────────────────────────────────────────────────────────
// test_remote.inc — 远程协助主控增强引擎单元测试套件 (Google Test)
//
// 覆盖三大核心引擎:
//   1. 沉浸式系统热键直通 (Immersive Remote Hotkey Tunnel)
//   2. 远程修饰键卡死一键急救 (Modifier Key Emergency Flush)
//   3. 远控输入法智能脱敏 (Smart Remote IME Sanitizer)
//   4. 进程与类名匹配器、扫描码消息构建、双击右 Ctrl 状态机与 JSON 序列化
// ─────────────────────────────────────────────────────────────────────────────

#include <gtest/gtest.h>
#include <windows.h>
#include <thread>
#include <chrono>

#include "core/remote/RemoteMasterEngine.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/events/EventBus.h"

using namespace tools3000::core;

TEST(RemoteMasterTest, ProcessAndClassMatching) {
    std::vector<std::string> targetProcesses = {
        "ToDesk.exe",
        "SunloginClient.exe",
        "AnyDesk.exe",
        "RustDesk.exe",
        "mstsc.exe",
        "TeamViewer.exe",
        "vncviewer.exe",
        "parsec.exe"
    };

    std::vector<std::string> targetClasses = {
        "TSSHELLWND",
        "IHWindowClass",
        "SunloginMain",
        "ToDesk_Main",
        "anydesk"
    };

    // 1. 精确与大小写匹配测试
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"ToDesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"todesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"TODESK.EXE", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"SunloginClient.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"mstsc.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"AnyDesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"RustDesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"TeamViewer.exe", L"", targetProcesses, targetClasses));

    // 2. 完整绝对路径测试
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"C:\\Program Files\\ToDesk\\ToDesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"D:\\Remote\\SunloginClient.exe", L"", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"C:\\Windows\\System32\\mstsc.exe", L"", targetProcesses, targetClasses));

    // 3. 类名匹配测试 (包含与精确匹配)
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"TSSHELLWND", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"IHWindowClass", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"SunloginMain", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"ToDesk_Main", targetProcesses, targetClasses));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"anydesk", targetProcesses, targetClasses));

    // 4. 阴性反例测试 (常规系统与办公软件绝对不可被误判为远控，且杜绝前后缀误匹配)
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"MyToDesk.exe", L"", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"FakeSunloginClient.exe", L"", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"explorer.exe", L"CabinetWClass", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"chrome.exe", L"Chrome_WidgetWin_1", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"notepad.exe", L"Notepad", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"cmd.exe", L"ConsoleWindowClass", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"powershell.exe", L"ConsoleWindowClass", targetProcesses, targetClasses));
    EXPECT_FALSE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"", targetProcesses, targetClasses));

    // 5. 无后缀配置兼容性与双向对称匹配测试
    std::vector<std::string> noExtProcesses = { "ToDesk", "SunloginClient" };
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"ToDesk.exe", L"", noExtProcesses, {}));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"ToDesk", L"", targetProcesses, {}));

    // 6. 空白字符自动剔除鲁棒性测试
    std::vector<std::string> paddedProcesses = { "  ToDesk.exe  " };
    std::vector<std::string> paddedClasses = { "  TSSHELLWND  " };
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"ToDesk.exe", L"", paddedProcesses, {}));
    EXPECT_TRUE(RemoteMasterEngine::isRemoteProcessOrClass(L"", L"TSSHELLWND", {}, paddedClasses));
}

TEST(RemoteMasterTest, EmergencyFlushInputsBatch) {
    auto inputs = RemoteMasterEngine::buildEmergencyFlushInputs();

    // 必须精准包含 8 组修饰键 + 3 组鼠标键 = 11 个原子操作
    ASSERT_EQ(inputs.size(), 11u);

    // 校验前 8 项均为键盘 KEYUP
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(inputs[i].type, INPUT_KEYBOARD);
        EXPECT_TRUE((inputs[i].ki.dwFlags & KEYEVENTF_KEYUP) != 0);
        EXPECT_TRUE((inputs[i].ki.dwFlags & KEYEVENTF_SCANCODE) != 0);
    }

    // 验证 8 组修饰键的扫描码与扩展键标记
    // 0: VK_LCONTROL (0x1D, non-ext)
    EXPECT_EQ(inputs[0].ki.wVk, VK_LCONTROL);
    EXPECT_EQ(inputs[0].ki.wScan, 0x1D);
    EXPECT_FALSE((inputs[0].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 1: VK_RCONTROL (0x1D, ext)
    EXPECT_EQ(inputs[1].ki.wVk, VK_RCONTROL);
    EXPECT_EQ(inputs[1].ki.wScan, 0x1D);
    EXPECT_TRUE((inputs[1].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 2: VK_LMENU (0x38, non-ext)
    EXPECT_EQ(inputs[2].ki.wVk, VK_LMENU);
    EXPECT_EQ(inputs[2].ki.wScan, 0x38);
    EXPECT_FALSE((inputs[2].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 3: VK_RMENU (0x38, ext)
    EXPECT_EQ(inputs[3].ki.wVk, VK_RMENU);
    EXPECT_EQ(inputs[3].ki.wScan, 0x38);
    EXPECT_TRUE((inputs[3].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 4: VK_LSHIFT (0x2A, non-ext)
    EXPECT_EQ(inputs[4].ki.wVk, VK_LSHIFT);
    EXPECT_EQ(inputs[4].ki.wScan, 0x2A);

    // 5: VK_RSHIFT (0x36, non-ext)
    EXPECT_EQ(inputs[5].ki.wVk, VK_RSHIFT);
    EXPECT_EQ(inputs[5].ki.wScan, 0x36);

    // 6: VK_LWIN (0x5B, ext)
    EXPECT_EQ(inputs[6].ki.wVk, VK_LWIN);
    EXPECT_EQ(inputs[6].ki.wScan, 0x5B);
    EXPECT_TRUE((inputs[6].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 7: VK_RWIN (0x5C, ext)
    EXPECT_EQ(inputs[7].ki.wVk, VK_RWIN);
    EXPECT_EQ(inputs[7].ki.wScan, 0x5C);
    EXPECT_TRUE((inputs[7].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0);

    // 校验后 3 项为鼠标按键释放 (Left, Right, Middle)
    EXPECT_EQ(inputs[8].type, INPUT_MOUSE);
    EXPECT_EQ(inputs[8].mi.dwFlags, MOUSEEVENTF_LEFTUP);

    EXPECT_EQ(inputs[9].type, INPUT_MOUSE);
    EXPECT_EQ(inputs[9].mi.dwFlags, MOUSEEVENTF_RIGHTUP);

    EXPECT_EQ(inputs[10].type, INPUT_MOUSE);
    EXPECT_EQ(inputs[10].mi.dwFlags, MOUSEEVENTF_MIDDLEUP);
}

TEST(RemoteMasterTest, KeyLParamConstruction) {
    constexpr LPARAM kExtendedBit = static_cast<LPARAM>(1ULL << 24);
    constexpr LPARAM kAltDownBit = static_cast<LPARAM>(1ULL << 29);
    constexpr LPARAM kPrevDownBit = static_cast<LPARAM>(1ULL << 30);
    constexpr LPARAM kTransitionUpBit = static_cast<LPARAM>(1ULL << 31);

    // 1. KeyDown (非扩展键，普通主键)
    LPARAM lp1 = RemoteMasterEngine::makeKeyLParam(0x1E, false, false, false);
    EXPECT_EQ(lp1 & 0xFFFF, 1); // repeat count = 1
    EXPECT_EQ((lp1 >> 16) & 0xFF, 0x1E); // scan code = 0x1E
    EXPECT_EQ(lp1 & kExtendedBit, 0); // non-extended
    EXPECT_EQ(lp1 & kAltDownBit, 0); // non-alt
    EXPECT_EQ(lp1 & kPrevDownBit, 0); // not previously down
    EXPECT_EQ(lp1 & kTransitionUpBit, 0); // transition down

    // 2. KeyUp (扩展键，如右 Ctrl)
    LPARAM lp2 = RemoteMasterEngine::makeKeyLParam(0x1D, true, true, false);
    EXPECT_EQ((lp2 >> 16) & 0xFF, 0x1D);
    EXPECT_NE(lp2 & kExtendedBit, 0); // extended
    EXPECT_NE(lp2 & kPrevDownBit, 0); // previous down
    EXPECT_NE(lp2 & kTransitionUpBit, 0); // transition up

    // 3. SysKeyDown (带有 Alt 键)
    LPARAM lp3 = RemoteMasterEngine::makeKeyLParam(0x0F, false, false, true);
    EXPECT_NE(lp3 & kAltDownBit, 0); // alt down bit set
}

TEST(RemoteMasterTest, SettingsJsonRoundTrip) {
    RemoteMasterSettings original;
    original.enabled = true;
    original.hotkeyTunnelEnabled = true;
    original.emergencyFlushEnabled = true;
    original.doubleRightCtrlTrigger = true;
    original.imeSanitizerEnabled = true;
    original.emergencyShortcut = "Ctrl+Alt+Backspace";
    original.targetProcesses = { "ToDesk.exe", "AnyDesk.exe", "CustomRemote.exe" };
    original.targetClasses = { "TSSHELLWND", "CustomCanvas" };

    nlohmann::json j = original.toJson();
    EXPECT_TRUE(j["enabled"].get<bool>());
    EXPECT_TRUE(j["hotkeyTunnelEnabled"].get<bool>());
    EXPECT_EQ(j["emergencyShortcut"].get<std::string>(), "Ctrl+Alt+Backspace");

    RemoteMasterSettings restored = RemoteMasterSettings::fromJson(j);
    EXPECT_EQ(restored.enabled, original.enabled);
    EXPECT_EQ(restored.hotkeyTunnelEnabled, original.hotkeyTunnelEnabled);
    EXPECT_EQ(restored.emergencyFlushEnabled, original.emergencyFlushEnabled);
    EXPECT_EQ(restored.doubleRightCtrlTrigger, original.doubleRightCtrlTrigger);
    EXPECT_EQ(restored.imeSanitizerEnabled, original.imeSanitizerEnabled);
    EXPECT_EQ(restored.emergencyShortcut, original.emergencyShortcut);
    EXPECT_EQ(restored.targetProcesses, original.targetProcesses);
    EXPECT_EQ(restored.targetClasses, original.targetClasses);

    // 容错性测试：传入非法字段或部分字段
    nlohmann::json corruptJson = {
        {"enabled", "invalid_type"},
        {"targetProcesses", 12345}
    };
    RemoteMasterSettings safeDefault = RemoteMasterSettings::fromJson(corruptJson);
    EXPECT_TRUE(safeDefault.enabled); // 维持默认 true
    EXPECT_FALSE(safeDefault.targetProcesses.empty()); // 维持默认列表
}

TEST(RemoteMasterTest, CacheInvalidationAndDefaults) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    // 默认列表验证 (doubleRightCtrlTrigger 默认关闭以避免与双击 Ctrl 聚光灯冲突)
    auto def = engine.getSettings();
    EXPECT_TRUE(def.enabled);
    EXPECT_TRUE(def.hotkeyTunnelEnabled);
    EXPECT_TRUE(def.emergencyFlushEnabled);
    EXPECT_FALSE(def.doubleRightCtrlTrigger);
    EXPECT_TRUE(def.imeSanitizerEnabled);
    EXPECT_EQ(def.emergencyShortcut, "Ctrl+Alt+Backspace");

    // 更新配置测试
    RemoteMasterSettings custom = def;
    custom.emergencyShortcut = "Ctrl+Shift+F10";
    custom.targetProcesses.push_back("CustomVnc.exe");
    engine.updateSettings(custom);

    EXPECT_EQ(engine.getSettings().emergencyShortcut, "Ctrl+Shift+F10");
    EXPECT_EQ(engine.getParsedEmergencyShortcut().vk, static_cast<UINT>(VK_F10));

    // 重置默认
    engine.resetDefaults();
    EXPECT_EQ(engine.getSettings().emergencyShortcut, "Ctrl+Alt+Backspace");
    EXPECT_EQ(engine.getParsedEmergencyShortcut().vk, static_cast<UINT>(VK_BACK));
}

TEST(RemoteMasterTest, RightCtrlDoubleTapStateMachine) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    // 模拟构造键盘钩子数据
    KBDLLHOOKSTRUCT rctrlData{};
    rctrlData.vkCode = VK_RCONTROL;
    rctrlData.scanCode = 0x1D;
    rctrlData.flags = LLKHF_EXTENDED;

    KBDLLHOOKSTRUCT otherKeyData{};
    otherKeyData.vkCode = 0x41; // 'A'
    otherKeyData.scanCode = 0x1E;

    // 0. 验证默认防御闭环：未开启或非远控前台时，双击右 Ctrl 绝不消费事件
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    bool consumedDefault = engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);
    EXPECT_FALSE(consumedDefault);

    // 1. 显式开启急救配置并模拟远控客户端处于前台
    RemoteMasterSettings s = engine.getSettings();
    s.doubleRightCtrlTrigger = true;
    engine.updateSettings(s);
    engine.setMockForeground(reinterpret_cast<HWND>(0x1234), true, "mstsc.exe");

    // 1.1 第一次单独按下并释放 Right Ctrl：不应触发急救
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    bool consumed1 = engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);
    EXPECT_FALSE(consumed1);

    // 1.2 在 400ms 内再次释放 Right Ctrl：触发急救冲刷并消费事件
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    bool consumed2 = engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);
    EXPECT_TRUE(consumed2);

    // 1.3 中间有其他键打断的情形：按下 Right Ctrl 后按了 'A'，再按 Right Ctrl 不应误触发急救
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);

    // 插入其他按键
    engine.onLowLevelKeyboardEvent(otherKeyData, WM_KEYDOWN);
    engine.onLowLevelKeyboardEvent(otherKeyData, WM_KEYUP);

    // 再次释放 Right Ctrl，已被其他按键重置，不触发急救
    engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYDOWN);
    bool consumedAfterIntervene = engine.onLowLevelKeyboardEvent(rctrlData, WM_KEYUP);
    EXPECT_FALSE(consumedAfterIntervene);

    // 1.4 键盘驱动兼容性：某些系统驱动以 VK_CONTROL + LLKHF_EXTENDED 上报 Right Ctrl
    engine.flushModifiers();
    KBDLLHOOKSTRUCT rctrlExtData{};
    rctrlExtData.vkCode = VK_CONTROL;
    rctrlExtData.scanCode = 0x1D;
    rctrlExtData.flags = LLKHF_EXTENDED;

    engine.onLowLevelKeyboardEvent(rctrlExtData, WM_KEYDOWN);
    engine.onLowLevelKeyboardEvent(rctrlExtData, WM_KEYUP);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.onLowLevelKeyboardEvent(rctrlExtData, WM_KEYDOWN);
    bool consumedExt = engine.onLowLevelKeyboardEvent(rctrlExtData, WM_KEYUP);
    EXPECT_TRUE(consumedExt);

    engine.clearMockForeground();
}

TEST(RemoteMasterTest, DedicatedEmergencyShortcut) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    // 1. 快捷键字符串静态解析测试
    auto sc1 = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+Alt+Backspace");
    EXPECT_TRUE(sc1.valid);
    EXPECT_TRUE(sc1.ctrl);
    EXPECT_TRUE(sc1.alt);
    EXPECT_FALSE(sc1.shift);
    EXPECT_FALSE(sc1.win);
    EXPECT_EQ(sc1.vk, static_cast<UINT>(VK_BACK));

    auto sc2 = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+Alt+End");
    EXPECT_TRUE(sc2.valid);
    EXPECT_TRUE(sc2.ctrl);
    EXPECT_TRUE(sc2.alt);
    EXPECT_EQ(sc2.vk, static_cast<UINT>(VK_END));

    auto sc3 = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+Shift+F12");
    EXPECT_TRUE(sc3.valid);
    EXPECT_TRUE(sc3.ctrl);
    EXPECT_TRUE(sc3.shift);
    EXPECT_EQ(sc3.vk, static_cast<UINT>(VK_F12));

    auto sc4 = RemoteMasterEngine::parseEmergencyShortcut("Alt+Win+Delete");
    EXPECT_TRUE(sc4.valid);
    EXPECT_TRUE(sc4.alt);
    EXPECT_TRUE(sc4.win);
    EXPECT_EQ(sc4.vk, static_cast<UINT>(VK_DELETE));

    auto sc5 = RemoteMasterEngine::parseEmergencyShortcut("");
    EXPECT_FALSE(sc5.valid);

    auto sc6 = RemoteMasterEngine::parseEmergencyShortcut("InvalidKeyName");
    EXPECT_FALSE(sc6.valid);

    // 2. 独立按键防误触测试
    KBDLLHOOKSTRUCT backData{};
    backData.vkCode = VK_BACK;
    backData.scanCode = 0x0E;
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(backData, WM_KEYDOWN));

    // 3. 动态配置快捷键响应测试 (配置为 Alt+F12 并在事件中携带 LLKHF_ALTDOWN 触发)
    auto s = engine.getSettings();
    s.emergencyShortcut = "Alt+F12";
    engine.updateSettings(s);
    EXPECT_EQ(engine.getParsedEmergencyShortcut().vk, static_cast<UINT>(VK_F12));

    KBDLLHOOKSTRUCT f12AltData{};
    f12AltData.vkCode = VK_F12;
    f12AltData.scanCode = 0x58;
    f12AltData.flags = LLKHF_ALTDOWN;
    HWND dummyLocal = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x30001));
    HWND dummyRemote = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x30002));
    // 本地普通窗口下绝不拦截急救快捷键（防止全局吞掉 VS Code / 终端 / 编辑器快捷键）
    engine.setMockForeground(dummyLocal, false, "code.exe");
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(f12AltData, WM_SYSKEYDOWN));

    // 远控窗口处于前台时，携带 Alt 标志的 F12 命中动态配置的 Alt+F12，精准触发并消费
    engine.setMockForeground(dummyRemote, true, "mstsc.exe");
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(f12AltData, WM_SYSKEYDOWN));
    engine.clearMockForeground();

    // 4. HotkeyManager Backspace 格式化与解析联动 (包含空白字符容错)
    auto hk = HotkeyDef::fromString("Ctrl+Alt+Backspace");
    ASSERT_TRUE(hk.has_value());
    EXPECT_EQ(hk->virtualKey, static_cast<UINT>(VK_BACK));
    EXPECT_TRUE((static_cast<UINT>(hk->modifiers) & MOD_CONTROL) != 0);
    EXPECT_TRUE((static_cast<UINT>(hk->modifiers) & MOD_ALT) != 0);
    EXPECT_EQ(hk->toString(), "Ctrl+Alt+Backspace");

    auto hkPadded = HotkeyDef::fromString("  Ctrl + Alt + Backspace  ");
    ASSERT_TRUE(hkPadded.has_value());
    EXPECT_EQ(hkPadded->virtualKey, static_cast<UINT>(VK_BACK));

    // 5. 方向键与小键盘数字键支持测试
    auto scArrow = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+Alt+Left");
    EXPECT_TRUE(scArrow.valid);
    EXPECT_EQ(scArrow.vk, static_cast<UINT>(VK_LEFT));

    auto scNum = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+Shift+Num1");
    EXPECT_TRUE(scNum.valid);
    EXPECT_EQ(scNum.vk, static_cast<UINT>(VK_NUMPAD1));

    auto scPadded = RemoteMasterEngine::parseEmergencyShortcut("  Ctrl + Alt + End  ");
    EXPECT_TRUE(scPadded.valid);
    EXPECT_EQ(scPadded.vk, static_cast<UINT>(VK_END));

    engine.resetDefaults();
}

TEST(RemoteMasterTest, HotkeyTunnelingInterception) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    HWND dummyHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x12345678));

    // 1. 当处于本地普通应用前台时 (isRemote == false)
    engine.setMockForeground(dummyHwnd, false, "explorer.exe");
    EXPECT_FALSE(engine.isRemoteForeground());

    KBDLLHOOKSTRUCT winData{};
    winData.vkCode = VK_LWIN;
    winData.scanCode = 0x5B;

    KBDLLHOOKSTRUCT rData{};
    rData.vkCode = 0x52; // 'R'
    rData.scanCode = 0x13;

    KBDLLHOOKSTRUCT normalKey{};
    normalKey.vkCode = 0x41; // 'A'
    normalKey.scanCode = 0x1E;

    // 本地窗口下，Win 键与主键均不应被直通引擎拦截
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(winData, WM_KEYDOWN));
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(rData, WM_KEYDOWN));
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(normalKey, WM_KEYDOWN));

    // 2. 当切换到远程客户端前台时 (isRemote == true)
    engine.setMockForeground(dummyHwnd, true, "ToDesk.exe");
    EXPECT_TRUE(engine.isRemoteForeground());
    EXPECT_EQ(engine.getActiveRemoteProcess(), "ToDesk.exe");

    // Win 键单独按下：必须被拦截并直通
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(winData, WM_KEYDOWN));

    // 在 Win 按下期间，按 'R' (Win + R)：必须被拦截并直通
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(rData, WM_KEYDOWN));
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(rData, WM_KEYUP));

    // Win 键释放：必须被拦截并直通
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(winData, WM_KEYUP));

    // 普通输入键 (如 'A') 在无 Win 键时：不应拦截，正常交由窗口接收
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(normalKey, WM_KEYDOWN));

    // 3. Alt + Tab 直通测试
    KBDLLHOOKSTRUCT tabData{};
    tabData.vkCode = VK_TAB;
    tabData.scanCode = 0x0F;
    // 带有 WM_SYSKEYDOWN (系统级 Alt 组合键)
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(tabData, WM_SYSKEYDOWN));
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(tabData, WM_SYSKEYUP));

    // 4. Ctrl + Shift + Esc 与 Ctrl + Esc 直通测试
    KBDLLHOOKSTRUCT escData{};
    escData.vkCode = VK_ESCAPE;
    escData.scanCode = 0x01;
    // 在远控窗口下触发 Ctrl+Esc 或 Ctrl+Shift+Esc
    // (注：由于处于 mock 远控状态，调用 GetAsyncKeyState 可能为 0，可通过携带系统钩子测试)
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(escData, WM_KEYDOWN)); // 无修饰键不拦截

    // 5. 失焦解挂测试：切回本地应用后立即停止拦截
    engine.setMockForeground(dummyHwnd, false, "chrome.exe");
    EXPECT_FALSE(engine.isRemoteForeground());
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(winData, WM_KEYDOWN));

    // 6. 焦点切换修饰键滞留防御测试 (防 Win 键粘滞导致后续输入失效)
    engine.setMockForeground(dummyHwnd, true, "ToDesk.exe");
    EXPECT_TRUE(engine.onLowLevelKeyboardEvent(winData, WM_KEYDOWN));
    // 远控中按下了 Win 键，但此时焦点丢失（用户切回本地桌面）
    engine.setMockForeground(dummyHwnd, false, "explorer.exe");
    EXPECT_FALSE(engine.isRemoteForeground());
    // 再次聚焦远控窗口
    engine.setMockForeground(dummyHwnd, true, "ToDesk.exe");
    // 此时按下普通字母键 'A'，状态机必须已被彻底复位，绝不能误判为 Win+A！
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(normalKey, WM_KEYDOWN));

    engine.clearMockForeground();
    engine.resetDefaults();
}

TEST(RemoteMasterTest, ImeSanitizerLifecycle) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    HWND dummyHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x87654321));

    // 初始状态未脱敏
    EXPECT_FALSE(engine.isImeSanitized());

    // 切换进入远控前台：自动切换为 ENG 0409 脱敏模式
    engine.setMockForeground(dummyHwnd, true, "RustDesk.exe");
    EXPECT_TRUE(engine.isImeSanitized());

    // 切换离开远控前台：自动还原脱敏状态
    engine.setMockForeground(dummyHwnd, false, "notepad.exe");
    EXPECT_FALSE(engine.isImeSanitized());

    // 处于脱敏态时动态关闭脱敏配置：应立即退出脱敏状态
    engine.resetDefaults();
    engine.setMockForeground(dummyHwnd, true, "RustDesk.exe");
    EXPECT_TRUE(engine.isImeSanitized());
    auto s2 = engine.getSettings();
    s2.imeSanitizerEnabled = false;
    engine.updateSettings(s2);
    EXPECT_FALSE(engine.isImeSanitized());

    // 当关闭脱敏配置时：再次进入远控不应激活脱敏
    engine.setMockForeground(dummyHwnd, false, "notepad.exe");
    engine.setMockForeground(dummyHwnd, true, "RustDesk.exe");
    EXPECT_FALSE(engine.isImeSanitized());

    engine.clearMockForeground();
    engine.resetDefaults();
}

TEST(RemoteMasterTest, InitializeReentrancyAndForegroundDetection) {
    auto& engine = RemoteMasterEngine::instance();
    engine.resetDefaults();

    // 1. 验证 initialize() 在真实系统前台窗口环境下执行绝不抛出 std::system_error (EDEADLK 自死锁)
    EXPECT_NO_THROW({
        bool ok = engine.initialize();
        EXPECT_TRUE(ok);
    });

    // 2. 验证多次连续调用 initialize() 具备完全的幂等性与重入安全性
    for (int i = 0; i < 5; ++i) {
        EXPECT_NO_THROW({
            bool ok = engine.initialize();
            EXPECT_TRUE(ok);
        });
    }

    // 3. 验证存在 Mock 远控前台窗口时的重入与状态保持
    HWND dummyRemote = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x12345678));
    engine.setMockForeground(dummyRemote, true, "ToDesk.exe");
    EXPECT_TRUE(engine.isRemoteForeground());
    EXPECT_EQ(engine.getActiveRemoteProcess(), "ToDesk.exe");

    EXPECT_NO_THROW({
        bool ok = engine.initialize();
        EXPECT_TRUE(ok);
    });
    EXPECT_TRUE(engine.isRemoteForeground());
    EXPECT_EQ(engine.getActiveRemoteProcess(), "ToDesk.exe");

    // 4. 验证 Mock 本地普通窗口时的重入
    HWND dummyLocal = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x87654321));
    engine.setMockForeground(dummyLocal, false, "notepad.exe");
    EXPECT_FALSE(engine.isRemoteForeground());

    EXPECT_NO_THROW({
        bool ok = engine.initialize();
        EXPECT_TRUE(ok);
    });
    EXPECT_FALSE(engine.isRemoteForeground());

    engine.clearMockForeground();

    // 5. 验证创建并聚焦实际本地原生窗口时的 initialize() 侦测路径与重入锁安全
    HWND testWnd = CreateWindowExW(
        0, L"STATIC", L"Tools3000TestWnd", WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
    );
    if (testWnd) {
        SetForegroundWindow(testWnd);
        Sleep(10);
        EXPECT_NO_THROW({
            bool ok = engine.initialize();
            EXPECT_TRUE(ok);
        });
        // 自身进程的窗口绝不应被判定为远控
        EXPECT_FALSE(engine.isRemoteForeground());
        DestroyWindow(testWnd);
    }

    // 6. 验证关闭与重新初始化的全生命周期
    engine.shutdown();
    EXPECT_NO_THROW({
        bool ok = engine.initialize();
        EXPECT_TRUE(ok);
    });

    // 7. 验证远控前台失焦转为非远控时 initialize() 能够正确触发清理与输入法还原
    engine.setMockForeground(dummyRemote, true, "ToDesk.exe");
    EXPECT_TRUE(engine.isRemoteForeground());
    EXPECT_TRUE(engine.isImeSanitized());

    // 切换至 Mock 本地窗口并再次 initialize()，确认状态正确转变为非远控并释放输入法脱敏
    engine.setMockForeground(dummyLocal, false, "notepad.exe");
    EXPECT_NO_THROW({
        bool ok = engine.initialize();
        EXPECT_TRUE(ok);
    });
    EXPECT_FALSE(engine.isRemoteForeground());
    EXPECT_FALSE(engine.isImeSanitized());

    engine.clearMockForeground();

    // 8. 验证多线程高并发重入调用 (含 initialize、shutdown、updateSettings 与状态查询) 绝对无死锁
    std::atomic<bool> stopWorkers{false};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&engine, &stopWorkers, i]() {
            while (!stopWorkers.load(std::memory_order_relaxed)) {
                if (i == 0) {
                    bool ok = engine.initialize();
                    EXPECT_TRUE(ok);
                } else if (i == 1) {
                    engine.shutdown();
                } else if (i == 2) {
                    auto s = engine.getSettings();
                    s.hotkeyTunnelEnabled = !s.hotkeyTunnelEnabled;
                    engine.updateSettings(s);
                } else {
                    auto s = engine.getSettings();
                    auto hkl = engine.getSavedHkl();
                    auto proc = engine.getActiveRemoteProcess();
                    (void)s;
                    (void)hkl;
                    (void)proc;
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopWorkers.store(true, std::memory_order_relaxed);
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    // 恢复干净初始化状态
    engine.initialize();
    engine.clearMockForeground();
    engine.resetDefaults();
}

TEST(RemoteMasterTest, FlushModifiersNeverStealsFocusOrFiresInLocalWindow) {
    auto& engine = tools3000::core::RemoteMasterEngine::instance();
    engine.resetDefaults();
    engine.clearMockForeground();

    std::atomic<int> toastCount{0};
    const auto subId = tools3000::core::EventBus::instance().subscribe<tools3000::core::ShowToastEvent>(
        [&toastCount](const tools3000::core::ShowToastEvent&) {
            toastCount.fetch_add(1, std::memory_order_relaxed);
        });

    HWND dummyRemote = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x40001));
    HWND dummyLocal = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x40002));

    // 先激活远控窗口记录历史上下文，再切回本地窗口
    engine.setMockForeground(dummyRemote, true, "mstsc.exe");
    EXPECT_TRUE(engine.isRemoteForeground());
    engine.setMockForeground(dummyLocal, false, "notepad.exe");
    EXPECT_FALSE(engine.isRemoteForeground());

    // 在本地窗口前台下调用 flushModifiers：不得抢夺后台远控窗口焦点，也不得误弹释放 Toast
    const HWND fgBefore = GetForegroundWindow();
    engine.flushModifiers();
    EXPECT_EQ(toastCount.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(GetForegroundWindow(), fgBefore);

    // 当活跃远控窗口真实位于前台时，flushModifiers 正常发送释放脉冲并提示用户
    engine.setMockForeground(dummyRemote, true, "mstsc.exe");
    engine.flushModifiers();
    EXPECT_EQ(toastCount.load(std::memory_order_relaxed), 1);

    tools3000::core::EventBus::instance().unsubscribeAndWait(subId);
    engine.clearMockForeground();
    engine.resetDefaults();
}

TEST(RemoteMasterTest, RawInputClientBypassHotkeySuspendAndBareKeyRejection) {
    auto& engine = tools3000::core::RemoteMasterEngine::instance();
    auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
    engine.resetDefaults();
    engine.clearMockForeground();

    // 1. 验证 Raw Input / 自有低级键盘钩子远控客户端识别精准度
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("parsec.exe"));
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("C:\\Program Files\\Parsec\\parsecd.exe"));
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("Moonlight.exe"));
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("vmware-remotemks.exe"));
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("VirtualBoxVM.exe"));
    EXPECT_TRUE(RemoteMasterEngine::isRawInputRemoteClient("unknown.exe", L"SDL_app"));

    EXPECT_FALSE(RemoteMasterEngine::isRawInputRemoteClient("mstsc.exe"));
    EXPECT_FALSE(RemoteMasterEngine::isRawInputRemoteClient("ToDesk.exe"));
    EXPECT_FALSE(RemoteMasterEngine::isRawInputRemoteClient("explorer.exe", L"CabinetWClass"));

    // 2. 验证远控前台获焦/失焦联动 HotkeyManager 远控挂起标志位，且对 parsec.exe 不拦截伪造 Win 键消息
    HWND dummyParsec = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x51001));
    HWND dummyLocal = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x51002));
    engine.setMockForeground(dummyParsec, true, "parsec.exe");
    EXPECT_TRUE(engine.isRemoteForeground());
    EXPECT_TRUE(hotkeyMgr.isRemoteSuspended());

    KBDLLHOOKSTRUCT winKey{};
    winKey.vkCode = VK_LWIN;
    winKey.scanCode = 0x5B;
    EXPECT_FALSE(engine.onLowLevelKeyboardEvent(winKey, WM_KEYDOWN));

    engine.setMockForeground(dummyLocal, false, "explorer.exe");
    EXPECT_FALSE(engine.isRemoteForeground());
    EXPECT_FALSE(hotkeyMgr.isRemoteSuspended());

    // 3. 验证 U-12: parseEmergencyShortcut 严禁无修饰键的裸单键配置，必须至少含一个修饰键
    EXPECT_FALSE(RemoteMasterEngine::parseEmergencyShortcut("F12").valid);
    EXPECT_FALSE(RemoteMasterEngine::parseEmergencyShortcut("Backspace").valid);
    EXPECT_FALSE(RemoteMasterEngine::parseEmergencyShortcut("A").valid);
    EXPECT_FALSE(RemoteMasterEngine::parseEmergencyShortcut("0x70").valid);
    EXPECT_FALSE(RemoteMasterEngine::parseEmergencyShortcut("Escape").valid);

    const auto withMod = RemoteMasterEngine::parseEmergencyShortcut("Ctrl+F12");
    EXPECT_TRUE(withMod.valid);
    EXPECT_TRUE(withMod.ctrl);
    EXPECT_EQ(withMod.vk, static_cast<UINT>(VK_F12));

    engine.clearMockForeground();
    engine.resetDefaults();
}

